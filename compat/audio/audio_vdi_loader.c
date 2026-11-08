/* SPDX-License-Identifier: MIT - exact path redirect in the audio process only. */
#include <dlfcn.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>

void *dlopen(const char *path,int flags)
{
    void *(*real)(const char *,int)=dlsym(RTLD_NEXT,"dlopen");
    if (!real) return 0;
    if (path && !strcmp(path,"/vendor/lib64/libaudio_primary_impl.z.so"))
        path="/compat/lib64/libdipper_audio_vdi.so";
    if (path && (!strcmp(path,"/vendor/lib64/libaudio_manager_service_6.1.z.so") ||
                 !strcmp(path,"/vendor/lib64/libaudio_manager_service_6.0.z.so") ||
                 !strcmp(path,"libaudio_manager_service_6.1.z.so") ||
                 !strcmp(path,"libaudio_manager_service_6.0.z.so")))
        path="/compat/lib64/libdipper_audio_manager_service.so";
    return real(path,flags);
}

/* The generated HDI supports direct mode, already checked with the original
 * 6.1 proxy. Select it only for the primary manager in this audio process.
 * Preserve original getter/release behavior for any other named endpoint. */
static bool primary(const char *name)
{ return name && (!strcmp(name,"default") || !strcmp(name,"audio_manager_service")); }
static void *proxy_symbol(const char *symbol)
{
    void *fn=dlsym(RTLD_NEXT,symbol);
    if (fn) return fn;
    /* sa_main loads SA libraries locally. Resolve the original proxy in its
     * own scope when it is absent from RTLD_NEXT's global search list. */
    static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    static void *proxy60,*proxy61;
    pthread_mutex_lock(&lock);
    void **slot=strstr(symbol,"V6_1")?&proxy61:&proxy60;
    if (!*slot) {
        void *(*real_open)(const char *,int)=dlsym(RTLD_NEXT,"dlopen");
        if (real_open) *slot=real_open(strstr(symbol,"V6_1")?"libaudio_proxy_6.1.z.so":"libaudio_proxy_6.0.z.so",RTLD_NOW|RTLD_LOCAL);
    }
    if (*slot) fn=dlsym(*slot,symbol);
    pthread_mutex_unlock(&lock);
    return fn;
}
static void *get(const char *symbol,const char *name,bool stub)
{
    void *(*real)(const char *,bool)=proxy_symbol(symbol);
    const char *target=primary(name)?"audio_manager_service":name;
    void *result=real ? real(target,primary(name)?true:stub) : 0;
    fprintf(stderr,"DIPPER_AUDIO_PROXY_GET symbol=%s name=%s original=%p result=%p\n",symbol,name?name:"(null)",(void *)real,result);
    return result;
}
static void release(const char *symbol,const char *name,void *manager,bool stub)
{
    void (*real)(const char *,void *,bool)=proxy_symbol(symbol);
    if (real) real(primary(name)?"audio_manager_service":name,manager,primary(name)?true:stub);
}
void *IAudioManagerGetInstance(const char *name,bool stub)
{ return get("IAudioManagerGetInstance",name,stub); }
void *IAudioManagerGet(bool stub)
{ return IAudioManagerGetInstance("default",stub); }
void *IAudioManagerGetInstanceV6_1(const char *name,bool stub)
{ return get("IAudioManagerGetInstanceV6_1",name,stub); }
void *IAudioManagerGetV6_1(bool stub)
{ return IAudioManagerGetInstanceV6_1("default",stub); }
void IAudioManagerReleaseInstance(const char *name,void *manager,bool stub)
{ release("IAudioManagerReleaseInstance",name,manager,stub); }
void IAudioManagerRelease(void *manager,bool stub)
{ IAudioManagerReleaseInstance("default",manager,stub); }
void IAudioManagerReleaseInstanceV6_1(const char *name,void *manager,bool stub)
{ release("IAudioManagerReleaseInstanceV6_1",name,manager,stub); }
void IAudioManagerReleaseV6_1(void *manager,bool stub)
{ IAudioManagerReleaseInstanceV6_1("default",manager,stub); }
