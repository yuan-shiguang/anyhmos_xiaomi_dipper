/* SPDX-License-Identifier: MIT
 * Native passthrough factory around the original donor6.1 implementation.
 * The donor implementation omits GetVersion because its IPC stub supplies it.
 * Supply the verified table version when using the standard direct HDI mode.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static void *library,*manager,*remote;
static unsigned references;
static int version(void *self,uint32_t *major,uint32_t *minor)
{
    if (!self || !major || !minor) return -EINVAL;
    *major=6; *minor=1; return 0;
}
/* Donor LocalDeviceManager::InitAudioManager calls the verified AsObject slot
 * at +40 and registers a death recipient. Return the real published HDI stub's
 * Binder object, retaining its original death/lifetime behavior. */
static void *as_object(void *self)
{
    pthread_mutex_lock(&lock);
    if (self==manager && !remote) {
        void *hdi=dlopen("libhdi.z.so",RTLD_NOW|RTLD_LOCAL);
        void *(*get)(void)=hdi?dlsym(hdi,"HDIServiceManagerGet"):0;
        void (*release)(void *)=hdi?dlsym(hdi,"HDIServiceManagerRelease"):0;
        struct registry { void *(*GetService)(void *,const char *); };
        struct registry *registry=get?get():0;
        if (registry) {
            remote=registry->GetService(registry,"audio_manager_service");
            if (release) release(registry);
        }
        if (hdi) dlclose(hdi);
        fprintf(stderr,"DIPPER_AUDIO_AS_OBJECT=%p\n",remote);
    }
    void *result=self==manager?remote:0;
    pthread_mutex_unlock(&lock); return result;
}
void *AudioManagerImplGetInstance(void)
{
    pthread_mutex_lock(&lock);
    if (!manager) {
        if (!library) library=dlopen("/vendor/lib64/libaudio_primary_impl_vendor.z.so",RTLD_NOW|RTLD_LOCAL);
        void *(*create)(void)=library?dlsym(library,"AudioManagerCreateIfInstance"):0;
        if (create) manager=create();
        if (manager) {
            ((void **)manager)[4]=(void *)version;
            ((void **)manager)[5]=(void *)as_object;
        }
        else fprintf(stderr,"DIPPER_AUDIO_FACTORY_ERROR=%s\n",dlerror());
    }
    if (manager) references++;
    void *result=manager;
    pthread_mutex_unlock(&lock); return result;
}
void AudioManagerImplRelease(void *self)
{
    pthread_mutex_lock(&lock);
    if (self && self==manager && references && --references==0) {
        int (*destroy)(void *)=dlsym(library,"AudioManagerDestroyIfInstance");
        if (destroy) {
            int rc=destroy(manager);
            fprintf(stderr,"DIPPER_AUDIO_FACTORY_RELEASE_RC=%d\n",rc);
        }
        if (remote) {
            void *ipc=dlopen("libhdf_ipc_adapter.z.so",RTLD_NOW|RTLD_LOCAL);
            void (*recycle)(void *)=ipc?dlsym(ipc,"HdfRemoteServiceRecycle"):0;
            if (recycle) recycle(remote);
            if (ipc) dlclose(ipc);
            remote=0;
        }
        manager=0;
    }
    pthread_mutex_unlock(&lock);
}
