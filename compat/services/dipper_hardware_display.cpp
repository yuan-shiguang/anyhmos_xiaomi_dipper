/* Explicit control through the running original HDI service. No callback stealing. */
#include "display/composer/v1_3/idisplay_composer.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
namespace C=OHOS::HDI::Display::Composer;
int main(int argc,char **argv){
 setbuf(stdout,nullptr);alarm(12);
 auto service=C::V1_3::IDisplayComposer::Get("display_composer_service",false);if(!service)return 2;
 std::vector<C::V1_0::DisplayModeInfo> modes;
 int ret=service->GetDisplaySupportedModes(0,modes);printf("HDI_MODES result=%d count=%zu\n",ret,modes.size());if(ret)return 3;
 for(const auto &m:modes)printf("MODE id=%u %dx%d hz=%u\n",m.id,m.width,m.height,m.freshRate);
 uint32_t current=0,brightness=0;
 ret=service->GetDisplayMode(0,current);printf("CURRENT_MODE result=%d id=%u\n",ret,current);
 ret=service->GetDisplayBacklight(0,brightness);printf("BACKLIGHT result=%d level=%u\n",ret,brightness);
 if(argc==1)return 0;
 if(argc!=3)return 4;
 char *end=nullptr;unsigned long v=strtoul(argv[2],&end,10);if(!end||*end||v>1000)return 4;
 if(!strcmp(argv[1],"hz")){
  for(const auto &m:modes)if(m.freshRate==v){ret=service->SetDisplayMode(0,m.id);printf("SET_MODE hz=%lu id=%u result=%d\n",v,m.id,ret);return ret?5:0;}
  return 6;
 }
 if(!strcmp(argv[1],"brightness")){
  if(v>255)return 4;
  ret=service->SetDisplayBacklight(0,v);printf("SET_BACKLIGHT requested=%lu result=%d\n",v,ret);if(ret)return 7;
  ret=service->GetDisplayBacklight(0,brightness);printf("READBACK result=%d level=%u\n",ret,brightness);return ret||brightness!=v?8:0;
 }
 return 4;
}
