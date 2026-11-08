/* SPDX-License-Identifier: MIT
 * Explicit bounded diagnostic through original HDI wrappers and real SMQ/FDs.
 * CPU test pattern only; this is not a desktop or a system screenshot.
 */
#include "v1_3/include/idisplay_composer_interface.h"
#include "v1_0/include/idisplay_buffer.h"
#include <atomic>
#include <cstdio>
#include <unistd.h>
namespace C=OHOS::HDI::Display::Composer;
namespace B=OHOS::HDI::Display::Buffer::V1_0;
static std::atomic<bool> online{false};
static std::atomic<uint32_t> device{0};
static void plug(uint32_t id,bool connected,void*){printf("HDI_FRAME_HOTPLUG id=%u connected=%d\n",id,connected);if(connected){device=id;online=true;}}
#define CHECK(x) do{if(!(x)){printf("HDI_FRAME_FAIL line=%d %s\n",__LINE__,#x);return 1;}}while(0)
int main(){
    setbuf(stdout,nullptr);alarm(45);
    auto *c=C::V1_3::IDisplayComposerInterface::Get(true);CHECK(c);
    CHECK(c->RegHotPlugCallback(plug,nullptr)==0);
    for(unsigned i=0;i<100&&!online;i++)usleep(10000);CHECK(online);
    auto id=device.load();std::vector<C::V1_0::DisplayModeInfo> modes;
    CHECK(c->GetDisplaySupportedModes(id,modes)==0&&!modes.empty());auto m=modes[0];
    CHECK((m.width==1080&&m.height==2248)||(m.width==2248&&m.height==1080));
    auto *a=B::IDisplayBuffer::Get();CHECK(a);BufferHandle* buffers[2]{};
    const uint32_t colors[]={0xffffffff,0xff00ffff,0xffffff00,0xff00ff00,0xffff00ff,0xff0000ff,0xffff0000,0xff000000};
    for(unsigned n=0;n<2;n++){
        int result=a->AllocMem({uint32_t(m.width),uint32_t(m.height),3|(1ULL<<10),12,0},buffers[n]);
        printf("HDI_FRAME_ALLOC n=%u result=%d handle=%p\n",n,result,buffers[n]);CHECK(result==0&&buffers[n]);
        auto *b=buffers[n];CHECK(b->stride>=m.width*4&&int64_t(b->stride)*m.height<=b->size);
        auto *map=static_cast<unsigned char*>(a->Mmap(*b));CHECK(map);CHECK(a->InvalidateCache(*b)==0);
        for(int y=0;y<m.height;y++){auto *row=reinterpret_cast<uint32_t*>(map+size_t(y)*b->stride);for(int x=0;x<m.width;x++)row[x]=colors[(x*8/m.width+n)%8];}
        CHECK(a->FlushCache(*b)==0);
    }
    CHECK(c->SetClientBufferCacheCount(id,2)==0);
    CHECK(c->SetDisplayClientCrop(id,{0,0,m.width,m.height})==0);
    unsigned frames=0;
    for(unsigned n=0;n<28;n++){
        int result=c->SetDisplayClientBuffer(id,buffers[n%2],n%2,-1);
        printf("HDI_FRAME_SET frame=%u result=%d\n",n,result);CHECK(result==0);
        int fence=-1;result=c->Commit(id,fence);
        printf("HDI_FRAME_COMMIT frame=%u result=%d fence=%d\n",n,result,fence);CHECK(result==0);if(fence>=0)close(fence);
        frames++;usleep(1000000);
        if(n==7||n==15){CHECK(c->SetDisplayPowerStatus(id,C::V1_0::POWER_STATUS_OFF)==0);usleep(200000);CHECK(c->SetDisplayPowerStatus(id,C::V1_0::POWER_STATUS_ON)==0);printf("HDI_FRAME_POWER_CYCLE=%u\n",n==7?1:2);}
    }
    CHECK(c->SetDisplayPowerStatus(id,C::V1_0::POWER_STATUS_OFF)==0);
    for(auto *b:buffers){CHECK(a->Unmap(*b)==0);a->FreeMem(*b);}
    printf("HDI_FRAME_RESULT=0 original_wrappers=1 smq=1 remote_allocator=1 commits=%u physical_visibility_unconfirmed=1\n",frames);
    return 0;
}
