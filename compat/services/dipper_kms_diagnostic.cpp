/* SPDX-License-Identifier: MIT
 * Explicit, bounded dipper-only scanout diagnostic. No image/partition writes.
 * A hardware color-fill trial isolates SSPP fetch from DSC/DSI scanout.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <initializer_list>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
static volatile sig_atomic_t stopped;
static void Stop(int){stopped=1;}
struct Session {
    int fd=-1,dma=-1;void *map=MAP_FAILED;size_t size=0;
    uint32_t fb=0,handle=0,blob=0,connector=0,crtc=0,plane=0,secondPlane=0;bool active=false;
    int Prop(drmModeAtomicReq *req,uint32_t obj,uint32_t type,const char *name,uint64_t value){
        auto *ps=drmModeObjectGetProperties(fd,obj,type);if(!ps)return -1;int rc=-1;
        for(uint32_t i=0;i<ps->count_props;i++){auto *p=drmModeGetProperty(fd,ps->props[i]);
            if(p){if(!strcmp(p->name,name))rc=drmModeAtomicAddProperty(req,obj,p->prop_id,value);drmModeFreeProperty(p);}if(rc>=0)break;}
        drmModeFreeObjectProperties(ps);if(rc<0)printf("KMS_PROPERTY_FAIL %s errno=%d\n",name,errno);return rc;
    }
    ~Session(){
        if(active){auto *req=drmModeAtomicAlloc();bool ok=req;
            if(req){for(uint32_t id:{plane,secondPlane}){if(!id)continue;
                ok&=Prop(req,id,DRM_MODE_OBJECT_PLANE,"FB_ID",0)>=0;ok&=Prop(req,id,DRM_MODE_OBJECT_PLANE,"CRTC_ID",0)>=0;
                ok&=Prop(req,id,DRM_MODE_OBJECT_PLANE,"color_fill",0)>=0;}
                ok&=Prop(req,connector,DRM_MODE_OBJECT_CONNECTOR,"CRTC_ID",0)>=0;
                ok&=Prop(req,crtc,DRM_MODE_OBJECT_CRTC,"ACTIVE",0)>=0;ok&=Prop(req,crtc,DRM_MODE_OBJECT_CRTC,"MODE_ID",0)>=0;}
            int rc=ok?drmModeAtomicCommit(fd,req,DRM_MODE_ATOMIC_ALLOW_MODESET,nullptr):-1;
            printf("KMS_DIAGNOSTIC_DISABLE rc=%d errno=%d\n",rc,errno);if(req)drmModeAtomicFree(req);
        }
        if(fb)drmModeRmFB(fd,fb);if(blob)drmModeDestroyPropertyBlob(fd,blob);
        if(map!=MAP_FAILED)munmap(map,size);if(handle){drm_gem_close c{handle,0};ioctl(fd,DRM_IOCTL_GEM_CLOSE,&c);}
        if(dma>=0)close(dma);if(fd>=0){drmDropMaster(fd);close(fd);}
    }
};
#define CHECK(x) do{if(!(x)){printf("KMS_DIAGNOSTIC_FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);return 1;}}while(0)
int main(int argc,char **argv){
    setbuf(stdout,nullptr);CHECK(argc==4||argc==5);bool split=argc==5;CHECK(!split||!strcmp(argv[4],"split"));
    unsigned hz=strtoul(argv[1],nullptr,10),hold=strtoul(argv[3],nullptr,10);
    bool solid=!strcmp(argv[2],"solid");CHECK(solid||!strcmp(argv[2],"bars"));CHECK(hold>=1&&hold<=40);
    signal(SIGINT,Stop);signal(SIGTERM,Stop);signal(SIGALRM,Stop);alarm(hold+12);
    Session s;s.fd=open("/dev/dri/card0",O_RDWR|O_CLOEXEC);CHECK(s.fd>=0);
    CHECK(!drmSetClientCap(s.fd,DRM_CLIENT_CAP_UNIVERSAL_PLANES,1));CHECK(!drmSetClientCap(s.fd,DRM_CLIENT_CAP_ATOMIC,1));CHECK(!drmSetMaster(s.fd));
    auto *rs=drmModeGetResources(s.fd);CHECK(rs);drmModeModeInfo mode{};unsigned index=0;
    for(int i=0;i<rs->count_connectors&&!s.connector;i++){auto *c=drmModeGetConnector(s.fd,rs->connectors[i]);if(!c)continue;
        if(c->connection==DRM_MODE_CONNECTED&&c->connector_type==DRM_MODE_CONNECTOR_DSI){
            for(int m=0;m<c->count_modes;m++)if(((c->modes[m].hdisplay==1080&&c->modes[m].vdisplay==2248)||(c->modes[m].hdisplay==2248&&c->modes[m].vdisplay==1080))&&c->modes[m].vrefresh==hz){mode=c->modes[m];s.connector=c->connector_id;break;}
            for(int j=0;j<c->count_encoders&&s.connector&&!s.crtc;j++){auto *e=drmModeGetEncoder(s.fd,c->encoders[j]);if(e){
                for(int k=0;k<rs->count_crtcs;k++)if(e->possible_crtcs&(1u<<k)){s.crtc=rs->crtcs[k];index=k;break;}drmModeFreeEncoder(e);}}}
        drmModeFreeConnector(c);}
    drmModeFreeResources(rs);CHECK(s.connector&&s.crtc);
    auto *old=drmModeGetCrtc(s.fd,s.crtc);CHECK(old);bool occupied=old->buffer_id!=0;drmModeFreeCrtc(old);CHECK(!occupied);
    auto *planes=drmModeGetPlaneResources(s.fd);CHECK(planes);
    for(uint32_t i=0;i<planes->count_planes;i++){auto *p=drmModeGetPlane(s.fd,planes->planes[i]);if(!p)continue;
        bool primary=false,overlay=false,rgb=false;for(uint32_t f=0;f<p->count_formats;f++)rgb|=p->formats[f]==DRM_FORMAT_XRGB8888;
        auto *ps=drmModeObjectGetProperties(s.fd,p->plane_id,DRM_MODE_OBJECT_PLANE);if(ps){for(uint32_t j=0;j<ps->count_props;j++){
            auto *prop=drmModeGetProperty(s.fd,ps->props[j]);if(prop){if(!strcmp(prop->name,"type")){primary=ps->prop_values[j]==DRM_PLANE_TYPE_PRIMARY;overlay=ps->prop_values[j]==DRM_PLANE_TYPE_OVERLAY;}drmModeFreeProperty(prop);}}drmModeFreeObjectProperties(ps);}
        if(rgb&&(p->possible_crtcs&(1u<<index))){if(primary&&!s.plane)s.plane=p->plane_id;if(split&&overlay&&!s.secondPlane)s.secondPlane=p->plane_id;}drmModeFreePlane(p);}
    drmModeFreePlaneResources(planes);CHECK(s.plane&&(!split||s.secondPlane));
    unsigned pitch=(mode.hdisplay*4+127)&~127u;s.size=(size_t(pitch)*mode.vdisplay+4095)&~4095ULL;
    int heap=open("/dev/dma_heap/system",O_RDONLY|O_CLOEXEC);CHECK(heap>=0);dma_heap_allocation_data allocation{};allocation.len=s.size;allocation.fd_flags=O_RDWR|O_CLOEXEC;
    int rc=ioctl(heap,DMA_HEAP_IOCTL_ALLOC,&allocation);close(heap);CHECK(!rc);s.dma=allocation.fd;
    s.map=mmap(nullptr,s.size,PROT_READ|PROT_WRITE,MAP_SHARED,s.dma,0);CHECK(s.map!=MAP_FAILED);
    dma_buf_sync sync{DMA_BUF_SYNC_START|DMA_BUF_SYNC_WRITE};CHECK(!ioctl(s.dma,DMA_BUF_IOCTL_SYNC,&sync));
    const uint32_t colors[]={0xffffffff,0xffffff00,0xff00ffff,0xff00ff00,0xffff00ff,0xffff0000,0xff0000ff,0xff000000};
    for(unsigned y=0;y<mode.vdisplay;y++){auto *row=reinterpret_cast<uint32_t*>(static_cast<char*>(s.map)+size_t(y)*pitch);for(unsigned x=0;x<mode.hdisplay;x++)row[x]=colors[x*8/mode.hdisplay];}
    sync.flags=DMA_BUF_SYNC_END|DMA_BUF_SYNC_WRITE;CHECK(!ioctl(s.dma,DMA_BUF_IOCTL_SYNC,&sync));CHECK(!drmPrimeFDToHandle(s.fd,s.dma,&s.handle));
    uint32_t handles[4]{s.handle},pitches[4]{pitch},offsets[4]{};CHECK(!drmModeAddFB2(s.fd,mode.hdisplay,mode.vdisplay,DRM_FORMAT_XRGB8888,handles,pitches,offsets,&s.fb,0));
    CHECK(!drmModeCreatePropertyBlob(s.fd,&mode,sizeof(mode),&s.blob));auto *req=drmModeAtomicAlloc();CHECK(req);bool valid=true;
#define PROP(obj,type,name,val) valid&=s.Prop(req,obj,type,name,val)>=0
    PROP(s.connector,DRM_MODE_OBJECT_CONNECTOR,"CRTC_ID",s.crtc);PROP(s.crtc,DRM_MODE_OBJECT_CRTC,"MODE_ID",s.blob);PROP(s.crtc,DRM_MODE_OBJECT_CRTC,"ACTIVE",1);
    for(unsigned i=0;i<(split?2u:1u);i++){uint32_t id=i?s.secondPlane:s.plane;unsigned width=mode.hdisplay/(split?2:1),x=i*width;
        PROP(id,DRM_MODE_OBJECT_PLANE,"FB_ID",s.fb);PROP(id,DRM_MODE_OBJECT_PLANE,"CRTC_ID",s.crtc);
        PROP(id,DRM_MODE_OBJECT_PLANE,"SRC_X",uint64_t(x)<<16);PROP(id,DRM_MODE_OBJECT_PLANE,"SRC_Y",0);
        PROP(id,DRM_MODE_OBJECT_PLANE,"SRC_W",uint64_t(width)<<16);PROP(id,DRM_MODE_OBJECT_PLANE,"SRC_H",uint64_t(mode.vdisplay)<<16);
        PROP(id,DRM_MODE_OBJECT_PLANE,"CRTC_X",x);PROP(id,DRM_MODE_OBJECT_PLANE,"CRTC_Y",0);
        PROP(id,DRM_MODE_OBJECT_PLANE,"CRTC_W",width);PROP(id,DRM_MODE_OBJECT_PLANE,"CRTC_H",mode.vdisplay);
        PROP(id,DRM_MODE_OBJECT_PLANE,"color_fill",solid?0x8000ff00:0);}
    rc=valid?drmModeAtomicCommit(s.fd,req,DRM_MODE_ATOMIC_ALLOW_MODESET|DRM_MODE_ATOMIC_TEST_ONLY,nullptr):-1;
    if(!rc)rc=drmModeAtomicCommit(s.fd,req,DRM_MODE_ATOMIC_ALLOW_MODESET,nullptr);drmModeAtomicFree(req);CHECK(!rc);s.active=true;
    printf("KMS_DIAGNOSTIC_ACTIVE hz=%u pattern=%s color_fill=%08x plane=%u second_plane=%u crtc=%u pitch=%u hold=%u\n",hz,argv[2],solid?0x8000ff00:0,s.plane,s.secondPlane,s.crtc,pitch,hold);
    timespec start,now;clock_gettime(CLOCK_MONOTONIC,&start);unsigned frames=0;
    do{drmVBlank vb{};vb.request.type=static_cast<drmVBlankSeqType>(DRM_VBLANK_RELATIVE|(index<<DRM_VBLANK_HIGH_CRTC_SHIFT));vb.request.sequence=1;
        CHECK(!drmWaitVBlank(s.fd,&vb));frames++;clock_gettime(CLOCK_MONOTONIC,&now);
    }while(!stopped&&now.tv_sec-start.tv_sec+(now.tv_nsec-start.tv_nsec)/1e9<hold);
    printf("KMS_DIAGNOSTIC_RESULT=0 frames=%u physical_visibility_unconfirmed=1\n",frames);return 0;
}
