/* SPDX-License-Identifier: MIT
 * Bounded real donor HDI stub publication, without restarting existing HDF hosts.
 * Wire fields follow the checked-in official UHDF2 DevSvcManagerProxy source.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include "hdf_remote_service.h"
#include "hdf_device_class.h"

static volatile sig_atomic_t stop;
static void end(int sig){ (void)sig; stop=1; }
#define LOAD(var,lib,name) do { *(void **)(&(var))=dlsym(lib,name); if (!(var)) { printf("AUDIO_HDI_MISSING=%s:%s\n",name,dlerror()); return 2; } } while(0)
int main(int argc,char **argv)
{
    setbuf(stdout,0); setbuf(stderr,0);
    bool persistent=argc==2 && !strcmp(argv[1],"--serve");
    if (argc>1 && !persistent) return 1;
    signal(SIGTERM,end); signal(SIGINT,end); signal(SIGALRM,end);
    if (!persistent) alarm(24);
    void *driver=dlopen("/vendor/lib64/libaudio_primary_driver.z.so",RTLD_NOW|RTLD_GLOBAL);
    void *factory=dlopen("/compat/lib64/libdipper_audio_manager_service.so",RTLD_NOW|RTLD_LOCAL);
    if (!driver || !factory) { printf("AUDIO_HDI_LOAD=%s\n",dlerror()); return 2; }
    void *(*create)(void); void (*release)(void *);
    struct HdfRemoteService **(*stub)(const char *,void *);
    void (*remove_stub)(const char *,void *);
    struct HdfRemoteService *(*get_remote)(int32_t);
    bool (*set_desc)(struct HdfRemoteService *,const char *);
    bool (*write_token)(struct HdfRemoteService *,struct HdfSBuf *);
    struct HdfSBuf *(*obtain)(enum HdfSbufType);
    void (*recycle)(struct HdfSBuf *);
    bool (*str)(struct HdfSBuf *,const char *);
    bool (*u16)(struct HdfSBuf *,uint16_t);
    bool (*u32)(struct HdfSBuf *,uint32_t);
    int (*remote_field)(struct HdfSBuf *,struct HdfRemoteService *);
    LOAD(create,factory,"AudioManagerImplGetInstance"); LOAD(release,factory,"AudioManagerImplRelease");
    LOAD(stub,driver,"StubCollectorGetOrNewObject"); LOAD(remove_stub,driver,"StubCollectorRemoveObject");
    LOAD(get_remote,driver,"HdfRemoteServiceGet"); LOAD(set_desc,driver,"HdfRemoteServiceSetInterfaceDesc");
    LOAD(write_token,driver,"HdfRemoteServiceWriteInterfaceToken"); LOAD(obtain,driver,"HdfSbufTypedObtain");
    LOAD(recycle,driver,"HdfSbufRecycle"); LOAD(str,driver,"HdfSbufWriteString");
    LOAD(u16,driver,"HdfSbufWriteUint16"); LOAD(u32,driver,"HdfSbufWriteUint32");
    LOAD(remote_field,driver,"HdfSbufWriteRemoteService");
    void *manager=create(); if (!manager) return 3;
    const char *desc="ohos.hdi.audio.v6_1.IAudioManager";
    struct HdfRemoteService **service=stub(desc,manager);
    if (!service || !*service) { printf("AUDIO_HDI_STUB_NULL\n"); release(manager); return 4; }
    struct HdfRemoteService *registry=get_remote(5100);
    if (!registry || !set_desc(registry,"HDI.IServiceManager.V1_0")) return 5;
    struct HdfSBuf *data=obtain(SBUF_IPC),*reply=obtain(SBUF_IPC);
    if (!data || !reply) return 6;
    bool ok=write_token(registry,data) && str(data,"audio_manager_service") &&
        u16(data,DEVICE_CLASS_AUDIO) && u32(data,0) && remote_field(data,*service)==0 &&
        str(data,"") && str(data,desc);
    int rc=ok?registry->dispatcher->Dispatch(registry,1,data,reply):-1;
    recycle(data); recycle(reply);
    printf("AUDIO_HDI_REAL_STUB_PUBLISH_RC=%d uid=%d pid=%d\n",rc,getuid(),getpid());
    if (!rc) {
        while (!stop) sleep(1);
        data=obtain(SBUF_IPC); reply=obtain(SBUF_IPC);
        if (data && reply && write_token(registry,data) && str(data,"audio_manager_service"))
            printf("AUDIO_HDI_UNPUBLISH_RC=%d\n",registry->dispatcher->Dispatch(registry,8,data,reply));
        if (data) recycle(data); if (reply) recycle(reply);
    }
    remove_stub(desc,manager); release(manager);
    printf("AUDIO_HDI_PUBLISHER_EXIT=%d\n",rc); return rc?7:0;
}
