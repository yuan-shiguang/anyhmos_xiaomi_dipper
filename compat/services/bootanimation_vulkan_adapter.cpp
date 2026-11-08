// Exact-donor adapter for the public OHOS-6 RSSurfaceOhosVulkan::SetSkContext.
// Layout is pinned by full DSO SHA at deployment, constructor and RequestFrame.
#include <memory>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <dlfcn.h>

using Ptr=std::shared_ptr<void>;
using Extract=Ptr(*)(Ptr);
extern "C" Ptr dipper_extract(Ptr node)
 asm("_ZN4OHOS5Rosen18RSSurfaceExtractor16ExtractRSSurfaceENSt3__h10shared_ptrINS0_13RSSurfaceNodeEEE");
extern "C" Ptr dipper_extract(Ptr node) {
 auto original=(Extract)dlsym(RTLD_NEXT,"_ZN4OHOS5Rosen18RSSurfaceExtractor16ExtractRSSurfaceENSt3__h10shared_ptrINS0_13RSSurfaceNodeEEE");
 if(!original){fprintf(stderr,"DIPPER_ADAPTER no original extractor\n");return {};}
 auto surface=original(std::move(node));
 auto lib=dlopen("librender_service_base.z.so",RTLD_NOW|RTLD_NOLOAD);
 auto vt=(unsigned char*)dlsym(lib,"_ZTVN4OHOS5Rosen19RSSurfaceOhosVulkanE");
 auto mode=(int*)dlsym(lib,"_ZN4OHOS5Rosen18RSSystemProperties17systemGpuApiType_E");
 if(!surface||!vt||!mode||*mode!=1||*(void**)surface.get()!=vt+16){
  fprintf(stderr,"DIPPER_ADAPTER guard rejected mode=%d surface=%p\n",mode?*mode:-1,surface.get());return surface;
 }
 auto singleton=(void*(*)(const std::string&))dlsym(lib,"_ZN4OHOS5Rosen15RsVulkanContext12GetSingletonERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE");
 auto drawing=(Ptr(*)(void*,const std::string&))dlsym(lib,"_ZN4OHOS5Rosen15RsVulkanContext17GetDrawingContextERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE");
 if(!singleton||!drawing){fprintf(stderr,"DIPPER_ADAPTER missing original context API\n");return surface;}
 std::string cache="";auto rs=singleton(cache);auto ctx=rs?drawing(rs,cache):Ptr{};
 if(!ctx){fprintf(stderr,"DIPPER_ADAPTER original GPU context null\n");return surface;}
 // Equivalent to the original inline SetSkContext(shared_ptr<Drawing::GPUContext>).
 // Exact donor: ctor zeros q0 at this offset; RequestFrame loads +312 and
 // rejects null. No object fabrication, mode override or caller/token changes.
 auto member=reinterpret_cast<Ptr*>((unsigned char*)surface.get()+312);
 if(*member){fprintf(stderr,"DIPPER_ADAPTER existing GPU context retained\n");return surface;}
 *member=ctx;
 fprintf(stderr,"DIPPER_ADAPTER SetSkContext surface=%p gpu=%p uid-preserved=1\n",surface.get(),ctx.get());
 return surface;
}
