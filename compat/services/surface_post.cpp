// Keep cleanup on its owning GPU thread. The gate permits same-thread release
// inline, while all cross-thread tasks retain the donor's original scheduler.
// Begin required component: native_surface_lifetime_v1.cpp
// Observe wrapper destruction without changing the donor's destructors.
// Begin required component: native_surface_diagnostic_v6.cpp
// Read-only observation of image creation and cache format comparisons.
// Begin required component: native_surface_diagnostic_v5.cpp
// Observe the original native-image release boundary; keep ownership unchanged.
// Begin required component: native_surface_diagnostic_v4.cpp
// Additional read-only handle validation at the original donor import boundary.
// Begin required component: native_surface_diagnostic_v2.cpp
// Observe the original Drawing factory without changing its return value or GPU mode.
#include <cstdio>
#include <cstdint>
#include <dlfcn.h>
#include <memory>
#include <cstring>
#include <sys/syscall.h>
#include <unistd.h>

using Surface = std::shared_ptr<void>;
using Factory = Surface (*)(void*, const void*, uint32_t, int, uint32_t,
                           std::shared_ptr<void>, void (*)(void*), void*);
struct ImageInfo { int width, height, color, alpha; std::shared_ptr<void> colorSpace; };
template <class T> static T Field(const void* address, size_t offset)
{
    T value{};
    std::memcpy(&value, static_cast<const char*>(address) + offset, sizeof(value));
    return value;
}
Surface ObserveFactory(void*, const void*, uint32_t, int, uint32_t,
                       std::shared_ptr<void>, void (*)(void*), void*)
    asm("_ZN4OHOS5Rosen7Drawing7Surface22MakeFromBackendTextureEPNS1_10GPUContextERKNS1_11TextureInfoENS1_13TextureOriginEiNS1_9ColorTypeENSt3__h10shared_ptrINS1_10ColorSpaceEEEPFvPvESE_");

Surface ObserveFactory(void* gpu, const void* texture, uint32_t origin, int samples,
                       uint32_t color, std::shared_ptr<void> space,
                       void (*cleanup)(void*), void* helper)
{
    static auto original = reinterpret_cast<Factory>(dlsym(RTLD_NEXT,
        "_ZN4OHOS5Rosen7Drawing7Surface22MakeFromBackendTextureEPNS1_10GPUContextERKNS1_11TextureInfoENS1_13TextureOriginEiNS1_9ColorTypeENSt3__h10shared_ptrINS1_10ColorSpaceEEEPFvPvESE_"));
    if (!original) { std::fprintf(stderr, "DIPPER_SURFACE_FACTORY_MISSING %s\n", dlerror()); return {}; }
    auto result = original(gpu, texture, origin, samples, color, space, cleanup, helper);
    static unsigned calls = 0, failures = 0;
    unsigned call = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    if (call <= 12 || (!result && __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED) <= 24)) {
        auto drawing = static_cast<int*>(dlsym(RTLD_DEFAULT,
            "_ZN4OHOS5Rosen7Drawing16SystemProperties17systemGpuApiType_E"));
        auto rosen = static_cast<int*>(dlsym(RTLD_DEFAULT,
            "_ZN4OHOS5Rosen18RSSystemProperties17systemGpuApiType_E"));
        Dl_info location{}; dladdr(reinterpret_cast<void*>(original), &location);
        std::fprintf(stderr,
            "DIPPER_SURFACE_FACTORY tid=%ld gpu=%p texture=%p origin=%u samples=%d color=%u result=%p drawing=%d rosen=%d original=%s\n",
            syscall(SYS_gettid), gpu, texture, origin, samples, color, result.get(),
            drawing ? *drawing : -1, rosen ? *rosen : -1,
            location.dli_fname ? location.dli_fname : "?");
        // Offsets verified against this donor's ConvertToGrBackendVKTexture
        // implementation (0x195d0c), rather than inferred from another ABI.
        if (texture) {
            auto vk = Field<const void*>(texture, 24);
            std::fprintf(stderr, "DIPPER_TEXTURE_V2 size=%dx%d vk=%p\n",
                Field<int>(texture, 0), Field<int>(texture, 4), vk);
            if (vk) {
                std::fprintf(stderr,
                    "DIPPER_TEXTURE_V2 image=%llx memory=%llx offset=%llu bytes=%llu allocflags=%x tiling=%u layout=%u format=%u usage=%x samples=%u levels=%u queue=%x protected=%u sharing=%u\n",
                    (unsigned long long)Field<uint64_t>(vk, 0),
                    (unsigned long long)Field<uint64_t>(vk, 8),
                    (unsigned long long)Field<uint64_t>(vk, 16),
                    (unsigned long long)Field<uint64_t>(vk, 24),
                    Field<uint32_t>(vk, 32), Field<uint32_t>(vk, 64),
                    Field<uint32_t>(vk, 68), Field<uint32_t>(vk, 72),
                    Field<uint32_t>(vk, 76), Field<uint32_t>(vk, 80),
                    Field<uint32_t>(vk, 84), Field<uint32_t>(vk, 88),
                    Field<uint8_t>(vk, 92), Field<uint32_t>(vk, 144));
            }
        }
        // A small real render target distinguishes an invalid context from a
        // rejected external texture. Bounded diagnostics; never a substitute image.
        if (!result && gpu) {
            auto target = reinterpret_cast<Surface (*)(void*, bool, const ImageInfo&)>(dlsym(RTLD_NEXT,
                "_ZN4OHOS5Rosen7Drawing7Surface16MakeRenderTargetEPNS1_10GPUContextEbRKNS1_9ImageInfoE"));
            if (target) {
                ImageInfo info{8, 8, 4, 2, {}};
                auto test = target(gpu, false, info);
                std::fprintf(stderr, "DIPPER_SURFACE_CONTEXT_TEST tid=%ld target=%p\n", syscall(SYS_gettid), test.get());
            }
        }
    }
    return result;
}

// End required component: native_surface_diagnostic_v2.cpp
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <cerrno>

extern "C" void* DipperResolveBufferImport()
{
    static void* original = dlsym(RTLD_NEXT,
        "_ZN4OHOS5Rosen17NativeBufferUtils34MakeBackendTextureFromNativeBufferEP18NativeWindowBufferiib");
    if (!original) std::abort();
    return original;
}

extern "C" void DipperObserveBufferImport(const void* backend, void* window,
                                         int width, int height, bool protectedBuffer)
{
    static auto valid = reinterpret_cast<bool (*)(const void*)>(dlsym(RTLD_NEXT,
        "_ZNK4OHOS5Rosen7Drawing14BackendTexture7IsValidEv"));
    static auto texture = reinterpret_cast<const void* (*)(const void*)>(dlsym(RTLD_NEXT,
        "_ZNK4OHOS5Rosen7Drawing14BackendTexture14GetTextureInfoEv"));
    static auto handle = reinterpret_cast<const void* (*)(void*)>(dlsym(RTLD_NEXT,
        "OH_NativeWindow_GetBufferHandleFromNative"));
    static auto native = reinterpret_cast<void* (*)(void*)>(dlsym(RTLD_NEXT,
        "OH_NativeBufferFromNativeWindowBuffer"));
    const bool good = valid && valid(backend);
    static unsigned calls = 0, failures = 0;
    const auto call = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    if (call > 24 && (good || __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED) > 32)) return;
    const void* info = texture ? texture(backend) : nullptr;
    const void* vk = info ? Field<const void*>(info, 24) : nullptr;
    const void* h = handle ? handle(window) : nullptr;
    void* n = native ? native(window) : nullptr;
    std::fprintf(stderr,
        "DIPPER_IMPORT_V4 tid=%ld call=%u window=%p native=%p size=%dx%d protected=%d valid=%d vk=%p handle=%p\n",
        syscall(SYS_gettid), call, window, n, width, height, protectedBuffer, good, vk, h);
    if (h) {
        // Donor's 56-byte HDF BufferHandle, not the SDK's 64-byte variant
        // containing key. Read no bytes beyond the fixed header.
        const int fd = Field<int>(h, 0);
        struct stat st{};
        const int statResult = fstat(fd, &st);
        const int statError = errno;
        std::fprintf(stderr,
            "DIPPER_HANDLE_V4 fd=%d flags=%d fstat=%d errno=%d fdsize=%llu width=%d height=%d stride=%d size=%d format=%d usage=%llx reserveFds=%u reserveInts=%u\n",
            fd, fcntl(fd, F_GETFD), statResult, statError,
            static_cast<unsigned long long>(st.st_size), Field<int>(h, 4),
            Field<int>(h, 12), Field<int>(h, 8), Field<int>(h, 16), Field<int>(h, 20),
            (unsigned long long)Field<uint64_t>(h, 24), Field<uint32_t>(h, 48), Field<uint32_t>(h, 52));
    }
}

// End required component: native_surface_diagnostic_v4.cpp
extern "C" void DipperObserveDelete(void*)
    asm("_ZN4OHOS5Rosen17NativeBufferUtils13DeleteVkImageEPv");
extern "C" void DipperObserveDelete(void* helper)
{
    static auto original = reinterpret_cast<void (*)(void*)>(dlsym(RTLD_NEXT,
        "_ZN4OHOS5Rosen17NativeBufferUtils13DeleteVkImageEPv"));
    if (!original) std::abort();
    static unsigned calls = 0;
    const auto n = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    if (n <= 16 || n % 64 == 0) {
        std::fprintf(stderr,
            "DIPPER_DELETE_V5 n=%u helper=%p refs=%u device=%p image=%p memory=%p\n",
            n, helper, helper ? Field<uint32_t>(helper, 56) : 0,
            helper ? Field<void*>(helper, 0) : nullptr,
            helper ? Field<void*>(helper, 8) : nullptr,
            helper ? Field<void*>(helper, 16) : nullptr);
    }
    original(helper);
}

// End required component: native_surface_diagnostic_v5.cpp
struct BitmapFormatV6 { uint32_t color, alpha; };

extern "C" bool DipperObserveImage(void*, void*, const void*, uint32_t,
    BitmapFormatV6, const Surface&, void (*)(void*), void*, bool)
    asm("_ZN4OHOS5Rosen7Drawing5Image16BuildFromTextureERNS1_10GPUContextERKNS1_11TextureInfoENS1_13TextureOriginENS1_12BitmapFormatERKNSt3__h10shared_ptrINS1_10ColorSpaceEEEPFvPvESG_b");
extern "C" bool DipperObserveImage(void* self, void* gpu, const void* texture,
    uint32_t origin, BitmapFormatV6 format, const Surface& color,
    void (*cleanup)(void*), void* helper, bool ignoreAlpha)
{
    using Fn = bool (*)(void*, void*, const void*, uint32_t, BitmapFormatV6,
        const Surface&, void (*)(void*), void*, bool);
    static auto original = reinterpret_cast<Fn>(dlsym(RTLD_NEXT,
        "_ZN4OHOS5Rosen7Drawing5Image16BuildFromTextureERNS1_10GPUContextERKNS1_11TextureInfoENS1_13TextureOriginENS1_12BitmapFormatERKNSt3__h10shared_ptrINS1_10ColorSpaceEEEPFvPvESG_b"));
    if (!original) std::abort();
#ifdef DIPPER_BEFORE_IMAGE
    DIPPER_BEFORE_IMAGE(gpu);
#endif
    const bool result = original(self, gpu, texture, origin, format, color,
        cleanup, helper, ignoreAlpha);
    static unsigned calls = 0, failed = 0;
    const auto n = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    if (n <= 12 || n % 64 == 0 || (!result && __atomic_add_fetch(&failed, 1, __ATOMIC_RELAXED) <= 16))
        std::fprintf(stderr, "DIPPER_IMAGE_V6 n=%u result=%d self=%p gpu=%p texture=%p color=%u alpha=%u cleanup=%p helper=%p\n",
            n, result, self, gpu, texture, format.color, format.alpha,
            reinterpret_cast<void*>(cleanup), helper);
    return result;
}

extern "C" bool DipperObserveCompare(void*, uint32_t, uint32_t)
    asm("_ZN4OHOS5Rosen17NativeBufferUtils27IsYcbcrModelOrRangeNotEqualEP15OH_NativeBuffer29VkSamplerYcbcrModelConversion19VkSamplerYcbcrRange");
extern "C" bool DipperObserveCompare(void* native, uint32_t model, uint32_t range)
{
    using Fn = bool (*)(void*, uint32_t, uint32_t);
    static auto original = reinterpret_cast<Fn>(dlsym(RTLD_NEXT,
        "_ZN4OHOS5Rosen17NativeBufferUtils27IsYcbcrModelOrRangeNotEqualEP15OH_NativeBuffer29VkSamplerYcbcrModelConversion19VkSamplerYcbcrRange"));
    if (!original) std::abort();
    const bool result = original(native, model, range);
    static unsigned calls = 0, different = 0;
    const auto n = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    if (n <= 12 || (result && __atomic_add_fetch(&different, 1, __ATOMIC_RELAXED) <= 16))
        std::fprintf(stderr, "DIPPER_COMPARE_V6 n=%u different=%d native=%p cached_model=%u cached_range=%u\n",
            n, result, native, model, range);
    return result;
}

// End required component: native_surface_diagnostic_v6.cpp
extern "C" void DipperImageD0(void*)
    asm("_ZN4OHOS5Rosen7Drawing5ImageD0Ev");
extern "C" void DipperImageD0(void* self)
{
    static auto fn = reinterpret_cast<void (*)(void*)>(dlsym(RTLD_NEXT,
        "_ZN4OHOS5Rosen7Drawing5ImageD0Ev"));
    if (!fn) std::abort();
    static unsigned calls = 0;
    const auto n = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    if (n <= 12 || n % 64 == 0)
        std::fprintf(stderr, "DIPPER_IMAGE_D0 n=%u self=%p tid=%ld\n", n, self, syscall(SYS_gettid));
    fn(self);
}

extern "C" void DipperImageD1(void*)
    asm("_ZN4OHOS5Rosen7Drawing5ImageD1Ev");
extern "C" void DipperImageD1(void* self)
{
    static auto fn = reinterpret_cast<void (*)(void*)>(dlsym(RTLD_NEXT,
        "_ZN4OHOS5Rosen7Drawing5ImageD1Ev"));
    if (!fn) std::abort();
    static unsigned calls = 0;
    const auto n = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    if (n <= 12 || n % 64 == 0)
        std::fprintf(stderr, "DIPPER_IMAGE_D1 n=%u self=%p tid=%ld\n", n, self, syscall(SYS_gettid));
    fn(self);
}

// End required component: native_surface_lifetime_v1.cpp
#include <functional>
using TaskV2 = std::function<void()>;
using PostV2 = std::function<void(const TaskV2&)>;

extern "C" void DipperRegisterPostV2(void*, const PostV2&)
    asm("_ZN4OHOS5Rosen7Drawing10GPUContext16RegisterPostFuncERKNSt3__h8functionIFvRKNS4_IFvvEEEEEE");
extern "C" void DipperRegisterPostV2(void* gpu, const PostV2& post)
{
    static auto original = reinterpret_cast<void (*)(void*, const PostV2&)>(dlsym(RTLD_NEXT,
        "_ZN4OHOS5Rosen7Drawing10GPUContext16RegisterPostFuncERKNSt3__h8functionIFvRKNS4_IFvvEEEEEE"));
    if (!original) std::abort();
    const long owner = syscall(SYS_gettid);
    std::fprintf(stderr, "DIPPER_POST_V2_REGISTER gpu=%p owner=%ld enabled=%d\n", gpu, owner, bool(post));
    if (!post) { original(gpu, post); return; }
    original(gpu, [post, owner](const TaskV2& task) {
        static unsigned queued = 0, inlineCalls = 0;
        const auto n = __atomic_add_fetch(&queued, 1, __ATOMIC_RELAXED);
        const long caller = syscall(SYS_gettid);
        if (caller == owner && access("/data/local/tmp/dipper-gpu-owner-cleanup", F_OK) == 0) {
            const auto done = __atomic_add_fetch(&inlineCalls, 1, __ATOMIC_RELAXED);
            if (done <= 12 || done % 64 == 0)
                std::fprintf(stderr, "DIPPER_POST_V2_INLINE done=%u owner=%ld caller=%ld\n", done, owner, caller);
            task();
            return;
        }
        if (n <= 12 || n % 64 == 0)
            std::fprintf(stderr, "DIPPER_POST_V2_QUEUE n=%u owner=%ld caller=%ld\n", n, owner, caller);
        post([task, owner, n]() {
            static unsigned executed = 0;
            const auto done = __atomic_add_fetch(&executed, 1, __ATOMIC_RELAXED);
            if (done <= 12 || done % 64 == 0)
                std::fprintf(stderr, "DIPPER_POST_V2_RUN done=%u queued_id=%u owner=%ld actual=%ld\n", done, n, owner, syscall(SYS_gettid));
            task();
        });
    });
}
