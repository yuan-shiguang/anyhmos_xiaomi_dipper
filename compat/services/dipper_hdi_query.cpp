/* SPDX-License-Identifier: MIT - read-only native HDI/Binder display contract. */
#include "display/composer/v1_3/idisplay_composer.h"
#include <atomic>
#include <cstdio>
#include <unistd.h>
namespace C=OHOS::HDI::Display::Composer;
static std::atomic<unsigned> callbacks{0};
static std::atomic<uint32_t> screen{0};
class Plug final:public C::V1_0::IHotPlugCallback {
public:
    int32_t OnHotPlug(uint32_t id,bool connected)override {
        printf("HDI_HOTPLUG id=%u connected=%d\n",id,int(connected));
        if(connected){screen=id;callbacks++;}return 0;
    }
};
#define CHECK(x) do{if(!(x)){printf("HDI_QUERY_FAIL line=%d %s\n",__LINE__,#x);return 1;}}while(0)
int main(){
    setbuf(stdout,nullptr);alarm(15);
    auto service=C::V1_3::IDisplayComposer::Get("display_composer_service",false);CHECK(service!=nullptr);
    OHOS::sptr<Plug> callback=new Plug;
    int result=service->RegHotPlugCallback(callback);printf("HDI_REG_CALLBACK result=%d\n",result);CHECK(result==0);
    for(unsigned i=0;i<100&&!callbacks;i++)usleep(10000);CHECK(callbacks.load()>0);
    C::V1_0::DisplayCapability capability{};result=service->GetDisplayCapability(screen,capability);
    printf("HDI_CAPABILITY result=%d name=%s layers=%u mm=%ux%u\n",result,capability.name.c_str(),capability.supportLayers,capability.phyWidth,capability.phyHeight);CHECK(result==0&&capability.name=="Dipper DSI");
    std::vector<C::V1_0::DisplayModeInfo> modes;CHECK(service->GetDisplaySupportedModes(screen,modes)==0&&!modes.empty());
    for(const auto &m:modes)printf("HDI_MODE %dx%d refresh=%u id=%u\n",m.width,m.height,m.freshRate,m.id);
    CHECK((modes[0].width==2248&&modes[0].height==1080)||(modes[0].width==1080&&modes[0].height==2248));
    puts("HDI_QUERY_RESULT=0 original_proxy=1 binder_callback=1 original_service_wrapper=1");return 0;
}
