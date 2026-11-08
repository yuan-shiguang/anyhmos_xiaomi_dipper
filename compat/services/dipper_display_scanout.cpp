/* SPDX-License-Identifier: MIT
 * Native OHOS VDI -> DMA heap -> PRIME -> atomic KMS, explicit diagnostic only.
 */
#include "composer_exports.h"
#include "idisplay_buffer_vdi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <drm.h>
#include <drm_mode.h>
namespace C=OHOS::HDI::Display::Composer;
namespace B=OHOS::HDI::Display::Buffer::V1_0;
#define CHECK(x) do { if(!(x)){printf("NATIVE_SCANOUT_FAIL line=%d %s\n",__LINE__,#x);return 1;} }while(0)
#define LOAD(name) auto name=reinterpret_cast<decltype(&C::name)>(dlsym(composer,#name));CHECK(name)
static bool connected;
static void plug(uint32_t id,bool online,void*){connected=id==0&&online;}
int main(int argc,char **argv) {
    setbuf(stdout,nullptr);CHECK(argc>=3&&argc<=5);
    bool combined=argc==5&&!strcmp(argv[4],"cycle-repeat");
    bool cycle=combined||(argc==5&&!strcmp(argv[4],"cycle")),repeat=combined||(argc==5&&!strcmp(argv[4],"repeat"));
    CHECK(argc<5||cycle||repeat);
    unsigned hold=8;
    if(argc>=4){char *end=nullptr;unsigned long seconds=strtoul(argv[3],&end,10);CHECK(end&&!*end&&seconds>=1&&seconds<=40);hold=seconds;}
    alarm(hold+15);
    void *composer=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!composer){puts(dlerror());return 1;}
    void *allocator=dlopen(argv[2],RTLD_NOW|RTLD_LOCAL);if(!allocator){puts(dlerror());return 1;}
    LOAD(RegHotPlugCallback);LOAD(GetDisplaySupportedModes);LOAD(SetDisplayClientCrop);
    LOAD(SetDisplayClientBuffer);LOAD(Commit);LOAD(SetDisplayPowerStatus);LOAD(DestroyVdiImpl);
    CHECK(RegHotPlugCallback(plug,nullptr)==0&&connected);
    std::vector<C::V1_0::DisplayModeInfo> modes;CHECK(GetDisplaySupportedModes(0,modes)==0&&!modes.empty());
    auto m=modes[0];CHECK((m.width==2248&&m.height==1080)||(m.width==1080&&m.height==2248));
    auto create=reinterpret_cast<B::CreateDisplayBufferVdiFunc>(dlsym(allocator,"CreateDisplayBufferVdi"));
    auto destroy=reinterpret_cast<B::DestroyDisplayBufferVdiFunc>(dlsym(allocator,"DestroyDisplayBufferVdi"));CHECK(create&&destroy);
    auto *vdi=create();CHECK(vdi);BufferHandle *buffer=nullptr;
    CHECK(vdi->AllocMem({uint32_t(m.width),uint32_t(m.height),3|(1ULL<<10),12,0},buffer)==0&&buffer);
    auto *map=static_cast<unsigned char*>(vdi->Mmap(*buffer));CHECK(map);
    CHECK(vdi->InvalidateCache(*buffer)==0);
    const uint32_t colors[]={0xffffffff,0xff00ffff,0xffffff00,0xff00ff00,0xffff00ff,0xff0000ff,0xffff0000,0xff000000};
    CHECK(buffer->stride>=m.width*4&&int64_t(buffer->stride)*m.height<=buffer->size);
    for(int y=0;y<m.height;y++){auto *row=reinterpret_cast<uint32_t*>(map+size_t(y)*buffer->stride);for(int x=0;x<m.width;x++)row[x]=colors[x*8/m.width];}
    for(int y:{0,m.height/2,m.height-1}){
        auto *row=reinterpret_cast<const uint32_t*>(map+size_t(y)*buffer->stride);
        CHECK(row[0]==colors[0]&&row[m.width/2]==colors[4]&&row[m.width-1]==colors[7]);
        printf("NATIVE_BUFFER_ROW y=%d first=%08x middle=%08x last=%08x\n",y,row[0],row[m.width/2],row[m.width-1]);
    }
    CHECK(vdi->FlushCache(*buffer)==0);
    BufferHandle *alternate=nullptr;
    if(repeat){
        CHECK(vdi->AllocMem({uint32_t(m.width),uint32_t(m.height),3|(1ULL<<10),12,0},alternate)==0&&alternate);
        auto *other=static_cast<unsigned char*>(vdi->Mmap(*alternate));CHECK(other);CHECK(vdi->InvalidateCache(*alternate)==0);
        CHECK(alternate->stride>=m.width*4&&int64_t(alternate->stride)*m.height<=alternate->size);
        for(int y=0;y<m.height;y++){auto *row=reinterpret_cast<uint32_t*>(other+size_t(y)*alternate->stride);for(int x=0;x<m.width;x++)row[x]=colors[(x*8/m.width+1)%8];}
        CHECK(vdi->FlushCache(*alternate)==0);
    }
    CHECK(SetDisplayClientCrop(0,{0,0,m.width,m.height})==0);
    CHECK(SetDisplayClientBuffer(0,*buffer,-1)==0);int fence=-1;CHECK(Commit(0,fence)==0);if(fence>=0)close(fence);
    int fd=open("/dev/dri/card0",O_RDWR|O_CLOEXEC);CHECK(fd>=0);
    uint32_t crtcs[32];drm_mode_card_res resources{};resources.crtc_id_ptr=reinterpret_cast<uintptr_t>(crtcs);resources.count_crtcs=32;
    CHECK(ioctl(fd,DRM_IOCTL_MODE_GETRESOURCES,&resources)==0&&resources.count_crtcs<=32);
    unsigned index=0;for(;index<resources.count_crtcs;index++){drm_mode_crtc crtc{};crtc.crtc_id=crtcs[index];if(!ioctl(fd,DRM_IOCTL_MODE_GETCRTC,&crtc)&&crtc.mode_valid&&crtc.mode.hdisplay==m.width&&crtc.mode.vdisplay==m.height)break;}
    CHECK(index<resources.count_crtcs);drm_crtc_get_sequence before{},after{};before.crtc_id=after.crtc_id=crtcs[index];CHECK(ioctl(fd,DRM_IOCTL_CRTC_GET_SEQUENCE,&before)==0);
    timespec start,now;clock_gettime(CLOCK_MONOTONIC,&start);unsigned frames=0,powerCycles=0,recommits=0;
    do{drm_wait_vblank wait{};wait.request.type=static_cast<drm_vblank_seq_type>(_DRM_VBLANK_RELATIVE|(index<<_DRM_VBLANK_HIGH_CRTC_SHIFT));wait.request.sequence=1;
        CHECK(ioctl(fd,DRM_IOCTL_WAIT_VBLANK,&wait)==0);frames++;clock_gettime(CLOCK_MONOTONIC,&now);
        if(repeat&&frames%120==0){
            CHECK(SetDisplayClientBuffer(0,recommits%2?*buffer:*alternate,-1)==0);
            int nextFence=-1;CHECK(Commit(0,nextFence)==0);if(nextFence>=0)close(nextFence);recommits++;
            printf("NATIVE_BUFFER_FLIP completed=%u buffer=%u\n",recommits,recommits%2);
        }
        double elapsed=now.tv_sec-start.tv_sec+(now.tv_nsec-start.tv_nsec)/1e9;
        if(cycle&&powerCycles<2&&elapsed>=double(hold)*(powerCycles+1)/3){
            CHECK(SetDisplayPowerStatus(0,C::V1_0::POWER_STATUS_OFF)==0);usleep(200000);
            CHECK(SetDisplayPowerStatus(0,C::V1_0::POWER_STATUS_ON)==0);powerCycles++;
            printf("NATIVE_POWER_CYCLE completed=%u\n",powerCycles);
        }
    }while(now.tv_sec-start.tv_sec+(now.tv_nsec-start.tv_nsec)/1e9<hold);
    CHECK(ioctl(fd,DRM_IOCTL_CRTC_GET_SEQUENCE,&after)==0&&after.active&&after.sequence>before.sequence);close(fd);
    // Power cycling can reset the driver's sequence timestamp epoch.
    // Never interpret a counter difference across modesets as a refresh rate.
    double hz=cycle?0.0:double(after.sequence-before.sequence)*1e9/double(after.sequence_ns-before.sequence_ns);
    printf("NATIVE_SCANOUT active=1 %dx%d pitch=%d bytes=%d frames=%u hz=%.3f hz_valid=%u recommits=%u power_cycles=%u\n",m.width,m.height,buffer->stride,buffer->size,frames,hz,cycle?0:1,recommits,powerCycles);
    CHECK(SetDisplayPowerStatus(0,C::V1_0::POWER_STATUS_OFF)==0);CHECK(DestroyVdiImpl()==0);
    if(alternate){CHECK(vdi->Unmap(*alternate)==0);vdi->FreeMem(*alternate);}
    CHECK(vdi->Unmap(*buffer)==0);vdi->FreeMem(*buffer);destroy(vdi);dlclose(composer);dlclose(allocator);
    puts("NATIVE_SCANOUT_RESULT=0 actual_atomic_commit=1 physical_visibility_unconfirmed=1");return 0;
}
