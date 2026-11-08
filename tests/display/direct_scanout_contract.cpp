#include "composer_exports.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include <xf86drm.h>
#include <xf86drmMode.h>

using namespace OHOS::HDI::Display::Composer;
extern "C" void Ace5InitializeSubmitForTest();
extern "C" int32_t Ace5SubmitInternalForTest();
extern "C" int32_t Ace5TestClientBuffer(uint32_t);
extern "C" void Ace5ResetDirectForTest(unsigned);
extern "C" void Ace5SetDirectPolicyForTest(unsigned);
extern "C" int Ace5BorrowDirectReleaseForTest(uint32_t &);
extern "C" void Ace5DirectStateForTest(uint32_t &,uint32_t &,bool &,bool &,int &,bool &);
namespace {
uint64_t *fenceSlot = nullptr;
uint32_t nextFb = 100, removed = 0;
int failure = 0, createdFd = -1;
bool omitFence = false, useZero = false;
uint64_t *releaseSlot = nullptr;
int releaseFd = -1;
bool omitRelease = false, releaseZero = false, failTest = false, mockActive = true;
bool directTest = false;
uint32_t requestFb = 0, appliedFb = 0;
unsigned realCalls = 0, testCalls = 0, removedApplied = 0;
uint64_t blendState[2]{0,0}, alphaState[2]{255,255};
uint64_t blendRequest[2]{}, alphaRequest[2]{};
bool blendAdded[2]{}, alphaAdded[2]{};
void Check(bool passed, const char *message)
{
    if (!passed) { fprintf(stderr, "PRESENT_CONTRACT_FAIL %s\n", message); exit(1); }
}
bool Closed(int fd) { return fcntl(fd, F_GETFD) == -1 && errno == EBADF; }
}
extern "C" drmModeAtomicReq *__real_drmModeAtomicAlloc();
extern "C" int __real_drmModeAtomicAddProperty(drmModeAtomicReq *, uint32_t, uint32_t, uint64_t);
extern "C" drmModeAtomicReq *__wrap_drmModeAtomicAlloc()
{
    fenceSlot = releaseSlot = nullptr; requestFb = 0;
    for (unsigned i=0;i<2;++i) blendAdded[i]=alphaAdded[i]=false;
    return __real_drmModeAtomicAlloc();
}
extern "C" int __wrap_drmModeAtomicAddProperty(drmModeAtomicReq *req, uint32_t object,
    uint32_t property, uint64_t value)
{
    if (property == 999) fenceSlot = reinterpret_cast<uint64_t *>(value);
    if (property == 998) releaseSlot = reinterpret_cast<uint64_t *>(value);
    if (property == 200 || property == 211) requestFb = uint32_t(value);
    for (unsigned i=0;i<2;++i) {
        if (property == 300+i*2) { blendAdded[i]=true; blendRequest[i]=value; }
        if (property == 301+i*2) { alphaAdded[i]=true; alphaRequest[i]=value; }
    }
    return __real_drmModeAtomicAddProperty(req, object, property, value);
}
extern "C" int __wrap_drmPrimeFDToHandle(int, int, uint32_t *handle)
{ *handle = 1; return 0; }
extern "C" int __wrap_drmModeAddFB2(int, uint32_t, uint32_t, uint32_t,
    const uint32_t[4], const uint32_t[4], const uint32_t[4], uint32_t *fb, uint32_t)
{ *fb = ++nextFb; return 0; }
extern "C" int __wrap_drmModeRmFB(int, uint32_t fb)
{ removed = fb; if (directTest && appliedFb && fb==appliedFb) ++removedApplied; return 0; }
// 模拟驱动在提交失败前已创建 fence 的情况
extern "C" int __wrap_drmModeAtomicCommit(int, drmModeAtomicReq *, uint32_t flags, void *)
{
    if (flags & DRM_MODE_ATOMIC_TEST_ONLY) {
        ++testCalls;
        Check(!fenceSlot && !releaseSlot, "test-only has no fence properties");
        errno = EINVAL; return failTest ? -1 : 0;
    }
    ++realCalls;
    Check(!(flags & DRM_MODE_ATOMIC_NONBLOCK), "blocking commit retained");
    if (!directTest) Check(fenceSlot && *fenceSlot == UINT64_MAX, "64-bit fence slot initialized");
    if (fenceSlot && !omitFence) {
        Check(*fenceSlot == UINT64_MAX, "present slot initialized");
        if (useZero) close(0);
        createdFd = open("/dev/null", O_RDONLY);
        Check(createdFd >= 0, "mock present opened"); *fenceSlot = createdFd;
    }
    if (releaseSlot && !omitRelease) {
        Check(*releaseSlot == UINT64_MAX, "release slot initialized");
        if (releaseZero) { close(0); releaseZero = false; }
        releaseFd = open("/dev/null", O_RDONLY);
        Check(releaseFd >= 0, "mock release opened"); *releaseSlot = releaseFd;
    }
    if (!failure && directTest) {
        appliedFb = requestFb; mockActive = appliedFb != 0;
        for (unsigned i=0;i<2;++i) {
            if (blendAdded[i]) blendState[i]=blendRequest[i];
            if (alphaAdded[i]) alphaState[i]=alphaRequest[i];
        }
    }
    errno = EINVAL; return failure;
}
extern "C" FILE *__wrap_fopen(const char *, const char *)
{ errno=ENOENT; return nullptr; }
extern "C" int __wrap_ioctl(int, unsigned long, ...)
{ return 0; }
extern "C" int __wrap_drmModeDestroyPropertyBlob(int, uint32_t) { return 0; }
extern "C" drmModeObjectProperties *__wrap_drmModeObjectGetProperties(int, uint32_t, uint32_t)
{
    auto *p = static_cast<drmModeObjectProperties *>(calloc(1,sizeof(drmModeObjectProperties)));
    p->count_props=1; p->props=static_cast<uint32_t *>(calloc(1,sizeof(uint32_t)));
    p->prop_values=static_cast<uint64_t *>(calloc(1,sizeof(uint64_t)));
    p->props[0]=103; p->prop_values[0]=mockActive; return p;
}
extern "C" drmModePropertyRes *__wrap_drmModeGetProperty(int, uint32_t id)
{
    auto *p = static_cast<drmModePropertyRes *>(calloc(1,sizeof(drmModePropertyRes)));
    p->prop_id=id; strcpy(p->name,"ACTIVE"); return p;
}

int LegacyPresentContract()
{
    Ace5InitializeSubmitForTest();
    Ace5ResetDirectForTest(0);
    int32_t fence = -1;
    Check(Commit(0, fence) == 0 && fence == createdFd && !Closed(fence), "ownership transferred");
    Check(fcntl(fence, F_GETFD) & FD_CLOEXEC, "fence not inherited across exec");
    close(fence);
    failure = -1;
    Check(Commit(0, fence) != 0 && fence == -1 && Closed(createdFd) && removed == 102,
        "failure closes fence and removes only candidate framebuffer");
    failure = 0;
    Check(Ace5TestClientBuffer(0) == 0 && removed == 103, "test-only releases candidate");
    Check(Ace5SubmitInternalForTest() == 0 && Closed(createdFd) && removed == 101,
        "internal submission closes unused fence");
    omitFence = true;
    Check(Commit(0, fence) == 0 && fence == -1 && removed == 104,
        "missing fence does not undo successful submission");
    omitFence = false; useZero = true;
    Check(Commit(0, fence) == 0 && fence == 0 && !Closed(0), "descriptor zero transferred");
    close(fence); failure = -1;
    Check(Commit(0, fence) != 0 && fence == -1 && Closed(0) && removed == 107,
        "descriptor zero closed on failure");
    puts("PRESENT_CONTRACT_OK ownership failure test_only internal missing_fd descriptor_zero");
    return 0;
}

namespace {
struct DirectState {
    uint32_t prepared = 0, framebuffer = 0;
    bool appliedRules = false, clientRequired = false;
    int cleanup = 0;
    bool appliedError = false;
};
DirectState State()
{
    DirectState s;
    Ace5DirectStateForTest(s.prepared,s.framebuffer,s.appliedRules,s.clientRequired,s.cleanup,s.appliedError);
    return s;
}
void Prepare(bool expected)
{
    bool flush=!expected;
    Check(PrepareDisplayLayers(0,flush)==0 && flush==expected,"prepare decision");
}
void ClientBuffer()
{
    BufferHandle b{}; b.fd=open("/dev/null",O_RDONLY);
    b.width=1080; b.height=2248; b.stride=4352;
    b.size=4352*2248+4096; b.format=V1_0::PIXEL_FMT_RGBA_8888;
    Check(SetDisplayClientBuffer(0,b,-1)==0,"fresh client buffer"); close(b.fd);
}
void LayerBuffer(uint32_t layer)
{
    BufferHandle b{}; b.fd=open("/dev/null",O_RDONLY);
    b.width=1080; b.height=2248; b.stride=4352;
    b.size=4352*2248+4096; b.format=V1_0::PIXEL_FMT_RGBA_8888;
    Check(SetLayerBuffer(0,layer,b,-1)==0,"layer buffer"); close(b.fd);
}
uint32_t Layer()
{
    LayerInfo info{}; info.width=1080; info.height=2248; info.bpp=32;
    info.pixFormat=V1_0::PIXEL_FMT_RGBA_8888;
    uint32_t layer=0; Check(CreateLayer(0,info,layer)==0,"create layer");
    IRect r{0,0,1080,2248}; LayerAlpha a{true,false,0,0,255};
    std::vector<IRect> visible{r};
    Check(SetLayerRegion(0,layer,r)==0 && SetLayerCrop(0,layer,r)==0,"layer geometry");
    Check(SetLayerVisibleRegion(0,layer,visible)==0 && SetLayerAlpha(0,layer,a)==0,"opaque visible");
    Check(SetLayerCompositionType(0,layer,V1_0::COMPOSITION_DEVICE)==0 &&
        SetLayerBlendType(0,layer,V1_0::BLEND_SRCOVER)==0 &&
        SetLayerTransformMode(0,layer,V1_0::ROTATE_NONE)==0,"native device metadata");
    LayerBuffer(layer); return layer;
}
int Release(uint32_t layer, bool invalid=false, size_t count=1)
{
    std::vector<uint32_t> ids; std::vector<int32_t> fences;
    Check(GetDisplayReleaseFence(0,ids,fences)==0 && ids.size()==fences.size(),"release query");
    Check(ids.size()==count,"complete release list");
    if (!count) return -1;
    for (size_t i=0;i<ids.size();++i) {
        if (ids[i]!=layer) continue;
        if (invalid) { Check(fences[i]==-1,"client release without hardware fence"); return -1; }
        Check(fences[i]>=0 && fcntl(fences[i],F_GETFD)>=0,"borrowed release remains open");
        return fences[i];
    }
    Check(false,"missing layer release item"); return -1;
}
void GoodCommit(bool direct)
{
    unsigned before=realCalls; int32_t present=-1;
    Check(Commit(0,present)==0 && realCalls==before+1 && present>=0,"one successful real commit");
    Check(bool(releaseSlot)==direct,"release requested only by direct");
    Check((fcntl(present,F_GETFD)&FD_CLOEXEC)!=0,"present close-on-exec"); close(present);
    Check(State().framebuffer==appliedFb && !removedApplied,"applied framebuffer retained");
}
// 使用真实 VDI 接口验证释放围栏、回退与已应用状态
void DirectCase(unsigned number)
{
    directTest=false; DestroyVdiImpl();
    if (fcntl(0,F_GETFD)<0) Check(open("/dev/null",O_RDONLY)==0,"reserve stdin before DRM mock");
    failure=0; omitFence=useZero=omitRelease=releaseZero=failTest=false;
    mockActive=true; appliedFb=0; realCalls=testCalls=removedApplied=0;
    blendState[0]=blendState[1]=0; alphaState[0]=alphaState[1]=255;
    Ace5InitializeSubmitForTest(); Ace5ResetDirectForTest(number<2?number:2);
    Check(SetDisplayPowerStatus(0,V1_0::POWER_STATUS_ON)==0,"restore on request");
    directTest=true; uint32_t layer=Layer();
    if (number<2) {
        Prepare(true); GoodCommit(false); Release(layer,true);
        Check(blendState[0]==0 && blendState[1]==0,"off/shadow keep client plane state");
    } else if (number==2 || number==3) {
        Prepare(false); GoodCommit(true); int fd=Release(layer);
        for (unsigned i=0;i<100;++i) Check(Release(layer)==fd,"stable borrowed release queries");
        Check((fcntl(fd,F_GETFD)&FD_CLOEXEC)!=0,"release close-on-exec");
        Check(blendState[0]==1 && blendState[1]==1 && alphaState[0]==255 && alphaState[1]==255,
            "opaque plane state");
        if (number==3) {
            Prepare(false); Check(Release(layer)==fd,"lookahead prepare preserves release");
            // 保留直扫层并新增客户端层，验证回退后的释放列表完整性
            uint32_t other=Layer();
            Prepare(true);
            Check(SetLayerCompositionType(0,layer,V1_0::COMPOSITION_CLIENT)==0 &&
                SetLayerCompositionType(0,other,V1_0::COMPOSITION_CLIENT)==0,"client layer metadata");
            ClientBuffer(); GoodCommit(false);
            Check(Release(layer,false,2)==fd,"client fallback keeps matching direct fence");
            Release(other,true,2);
            Check(DestroyLayer(0,other)==0,"remove extra client layer");
        }
    } else if (number==4) {
        Prepare(false); GoodCommit(true); int old=Release(layer);
        LayerBuffer(layer); Prepare(false); GoodCommit(true);
        Check(Closed(old) && Release(layer)!=old,"next successful direct replaces original release");
    } else if (number==5 || number==6) {
        Prepare(false); GoodCommit(true); int old=Release(layer); uint32_t oldFb=appliedFb;
        LayerBuffer(layer); Prepare(false); failure=number==5?-1:0; failTest=number==6;
        int32_t present=8; Check(Commit(0,present)!=0 && present==-1,"candidate failure");
        Check(State().framebuffer==oldFb && appliedFb==oldFb && Release(layer)==old,
            "failed candidate preserves old scanout/release");
        if (number==5) Check(Closed(createdFd) && Closed(releaseFd),"failed candidate closes both new fences");
        failure=0; failTest=false; Prepare(true); ClientBuffer(); GoodCommit(false);
        Check(Release(layer)==old && !State().appliedRules && blendState[0]==0 && blendState[1]==0,
            "client fallback retains last release and restores plane rules");
    } else if (number==7 || number==8) {
        if (number==7) Check(SetLayerCompositionType(0,layer,V1_0::COMPOSITION_CLIENT)==0,"upper client");
        else Check(SetLayerTransformMode(0,layer,V1_0::ROTATE_90)==0,"rotation guard");
        Prepare(true); GoodCommit(false); Release(layer,true);
    } else if (number==9) {
        Prepare(false); GoodCommit(true); int old=Release(layer); uint32_t oldFb=appliedFb;
        LayerBuffer(layer); Prepare(false); Ace5SetDirectPolicyForTest(0);
        int32_t present=-1; Check(Commit(0,present)!=0 && appliedFb==oldFb && Release(layer)==old,
            "mode change does not use stale client frame");
        Prepare(true); ClientBuffer(); GoodCommit(false); Check(Release(layer)==old,"mode fallback preserves release");
    } else if (number==10) {
        Prepare(false); GoodCommit(true); uint32_t oldFb=appliedFb;
        LayerBuffer(layer); Prepare(false); omitRelease=true; int32_t present=-1;
        Check(Commit(0,present)!=0 && State().appliedError && appliedFb!=oldFb &&
            State().framebuffer==appliedFb && !removedApplied,"bad release after success keeps NEW scanout");
        std::vector<uint32_t> ids; std::vector<int32_t> fences;
        Check(GetDisplayReleaseFence(0,ids,fences)!=0,"bad release cannot be reported ready");
        omitRelease=false; Prepare(true); ClientBuffer(); GoodCommit(false);
    } else if (number==11) {
        Prepare(false); releaseZero=true; GoodCommit(true); Check(Release(layer)==0,"release descriptor zero");
    } else if (number==12 || number==13) {
        Prepare(false); GoodCommit(true); int old=Release(layer);
        Check((number==12?ClearLayerBuffer(0,layer):DestroyLayer(0,layer))==0,"layer lifecycle operation");
        Check(Closed(old),"layer lifecycle closes held release");
        Release(layer,true,number==13?0:1);
    } else if (number==14) {
        Prepare(false); GoodCommit(true); int old=Release(layer); failure=-1;
        Check(SetDisplayPowerStatus(0,V1_0::POWER_STATUS_OFF)!=0 && Release(layer)==old && !Closed(old),
            "failed power off preserves release");
        failure=0; Check(SetDisplayPowerStatus(0,V1_0::POWER_STATUS_OFF)==0 && Closed(old),
            "successful power off closes held release");
    } else if (number==15) {
        Prepare(false); GoodCommit(true); int old=Release(layer);
        Check(DestroyVdiImpl()==0 && Closed(old),"VDI destruction closes held release");
        directTest=false; printf("DIRECT_CASE_OK id=%u\n",number); return;
    }
    directTest=false; Check(DestroyVdiImpl()==0,"VDI shutdown");
    printf("DIRECT_CASE_OK id=%u\n",number);
}
}
int main()
{
    Check(LegacyPresentContract()==0,"original present regression");
    for (unsigned number=0;number<16;++number) DirectCase(number);
    puts("DIRECT_CONTRACT_OK cases=16 legacy_present=PASS real_vdi_wrapped_drm=1 physical_commits=0");
    return 0;
}
