// Dipper 显示后端：保留客户端合成，按原生同步协议直扫不透明单层
#include "composer_exports.h"
#include "log.h"
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <string>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#ifndef DIPPER_DEFAULT_DISPLAY_ROUTE
#define DIPPER_DEFAULT_DISPLAY_ROUTE 0
#endif
static_assert(DIPPER_DEFAULT_DISPLAY_ROUTE == 0 || DIPPER_DEFAULT_DISPLAY_ROUTE == 2,
    "Default display route must be 0 or 2");

namespace OHOS::HDI::Display::Composer {
namespace {
constexpr int OK = 0, FAIL = -1, INVALID = -3, UNSUPPORTED = -5;
namespace V = V1_0;
// The donor's non-community command responser calls fdParcel.Move() before
// returning from both buffer setters. The VDI owns this incoming fence.
struct OwnedFence {
    int fd;
    explicit OwnedFence(int value) : fd(value) {}
    ~OwnedFence() { if (fd >= 0) close(fd); }
    int Take() { int value = fd; fd = -1; return value; }
};
struct Buffer {
    BufferHandle value{};
    int fence = -1;
    Buffer() { value.fd = -1; }
    void Clear() {
        if (value.fd >= 0) close(value.fd);
        if (fence >= 0) close(fence);
        value.fd = -1; fence = -1;
    }
    ~Buffer() { Clear(); }
    int Set(const BufferHandle &b, OwnedFence &acquire) {
        int fd = fcntl(b.fd, F_DUPFD_CLOEXEC, 0);
        if (fd < 0) return FAIL;
        Clear(); value = b; value.fd = fd; value.virAddr = nullptr; fence = acquire.Take();
        return OK;
    }
};
struct Layer {
    LayerInfo info{};
    LayerAlpha alpha{};
    IRect region{}, crop{};
    uint32_t zorder = 0;
    bool premulti = false;
    TransformType transform = V::ROTATE_NONE;
    V::CompositionType composition = V::COMPOSITION_CLIENT;
    BlendType blend = V::BLEND_NONE;
    MaskInfo mask{};
    LayerColor color{};
    std::vector<IRect> dirty, visible;
    Buffer buffer;
};
#include "direct_scanout_state.h"
DirectReleaseCache directRelease;
uint32_t preparedDirectLayer = 0;
int scanoutPolicy = 0;
uint64_t policyReadTime = 0, directFrames = 0, directFailures = 0, frameAtomicTotal = 0, frameAtomicEnd = 0;
bool requireClientFrame = false, directAppliedError = false, planeRulesApplied = false;
bool directReleaseUnavailable = false;
struct PlaneBlendRules {
    uint32_t id = 0;
    bool queried = false, valid = false;
    uint64_t oldBlend = 0, oldAlpha = 0, opaqueAlpha = 0;
};
PlaneBlendRules planeBlend[2];
#ifdef ACE5_DIRECT_TEST
int scanoutPolicyOverride = -1;
#endif
std::mutex mutex;
int drm = -1;
// A single-DSI panel drives the whole width through one primary plane; a
// dual-DSI panel needs one SSPP plane per half. Follow the planes the
// connector actually exposes instead of assuming either layout.
uint32_t connector = 0, crtc = 0, plane = 0, secondPlane = 0, modeBlob = 0, currentFb = 0;
uint32_t mmWidth = 0, mmHeight = 0, nextLayer = 1;
drmModeModeInfo mode{};
std::vector<drmModeModeInfo> displayModes;
uint32_t selectedMode = 0, hardwareCrtcIndex = 0;
// 1 for a single-DSI panel, 2 when a second usable plane exists.
unsigned ScanoutPlaneCount() { return secondPlane && secondPlane != plane ? 2 : 1; }
uint32_t ScanoutPlane(unsigned half) { return half ? secondPlane : plane; }
std::map<std::pair<uint32_t, std::string>, uint32_t> propertyIds;
std::map<uint32_t, Layer> layers;
Buffer client;
// Protected by mutex, together with the native power request and every commit.
// Late RS frames must never turn a successfully powered-off panel back on.
bool requestedPowerOn = true;
bool loggedOffSubmission = false;
IRect clientCrop{};
std::vector<IRect> clientDamage;
std::mutex vsyncMutex;
bool vsyncEnabled = false, quit = false, threadStarted = false;
uint64_t vsyncPeriod = 16666667ULL;
pthread_t vsyncThread;
VBlankCallback vsyncCallback = nullptr;
void *vsyncData = nullptr;
uint64_t modeGeneration = 0;
void Metric(const char *format, ...);
uint64_t Period() { return mode.vrefresh ? 1000000000ULL / mode.vrefresh : 16666667ULL; }
bool Device(uint32_t id) { return id == 0 && drm >= 0 && connector; }
void Log(const char *message) { Ace5Log("%s errno=%d", message, errno); }
int Active(bool &active)
{
    auto *props = drmModeObjectGetProperties(drm, crtc, DRM_MODE_OBJECT_CRTC);
    if (!props) return FAIL;
    bool found = false;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        auto *p = drmModeGetProperty(drm, props->props[i]);
        if (p) {
            if (!strcmp(p->name, "ACTIVE")) { active = props->prop_values[i] != 0; found = true; }
            drmModeFreeProperty(p);
        }
    }
    drmModeFreeObjectProperties(props); return found ? OK : FAIL;
}
int Property(drmModeAtomicReqPtr request, uint32_t id, uint32_t type,
    const char *name, uint64_t value)
{
    auto key = std::make_pair(id, std::string(name));
    auto known = propertyIds.find(key);
    if (known != propertyIds.end()) return drmModeAtomicAddProperty(request, id, known->second, value);
    auto *props = drmModeObjectGetProperties(drm, id, type);
    if (!props) return FAIL;
    int result = FAIL;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        auto *p = drmModeGetProperty(drm, props->props[i]);
        if (!p) continue;
        if (!strcmp(name, p->name)) {
            propertyIds[key] = p->prop_id;
            result = drmModeAtomicAddProperty(request, id, p->prop_id, value);
        }
        drmModeFreeProperty(p);
        if (result >= 0) break;
    }
    drmModeFreeObjectProperties(props);
    return result;
}
void CleanupDrm()
{
    directRelease.Reset(); preparedDirectLayer = 0; requireClientFrame = false; directReleaseUnavailable = false;
    planeRulesApplied = false; planeBlend[0] = {}; planeBlend[1] = {};
    if (drm >= 0) {
        if (currentFb) drmModeRmFB(drm, currentFb);
        if (modeBlob) drmModeDestroyPropertyBlob(drm, modeBlob);
        close(drm);
    }
    drm = -1; connector = crtc = plane = secondPlane = modeBlob = currentFb = 0;
    propertyIds.clear(); displayModes.clear(); selectedMode = 0;
}
int Initialize()
{
    if (Device(0)) return OK;
    drm = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm < 0) { Log("open card0 failed"); return FAIL; }
    if (drmSetClientCap(drm, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
        drmSetClientCap(drm, DRM_CLIENT_CAP_ATOMIC, 1) || drmSetMaster(drm)) {
        Log("atomic/master failed"); CleanupDrm(); return FAIL;
    }
    auto *resources = drmModeGetResources(drm);
    if (!resources) { Log("resources failed"); CleanupDrm(); return FAIL; }
    uint32_t crtcIndex = 0;
    for (int i = 0; i < resources->count_connectors && !connector; ++i) {
        auto *c = drmModeGetConnector(drm, resources->connectors[i]);
        if (!c) continue;
        if (c->connection == DRM_MODE_CONNECTED && c->connector_type == DRM_MODE_CONNECTOR_DSI && c->count_modes) {
            mode = c->modes[0];
            for (int m = 0; m < c->count_modes; ++m)
                if (c->modes[m].type & DRM_MODE_TYPE_PREFERRED) { mode = c->modes[m]; break; }
            for (int m = 0; m < c->count_modes; ++m) {
                const auto &candidate = c->modes[m];
                if (candidate.hdisplay != mode.hdisplay || candidate.vdisplay != mode.vdisplay || !candidate.vrefresh) continue;
                if (!memcmp(&candidate, &mode, sizeof(mode))) selectedMode = displayModes.size();
                displayModes.push_back(candidate);
            }
            connector = c->connector_id; mmWidth = c->mmWidth; mmHeight = c->mmHeight;
            for (int e = 0; e < c->count_encoders && !crtc; ++e) {
                auto *encoder = drmModeGetEncoder(drm, c->encoders[e]);
                if (!encoder) continue;
                for (int k = 0; k < resources->count_crtcs; ++k)
                    if (encoder->possible_crtcs & (1U << k)) { crtc = resources->crtcs[k]; crtcIndex = k; break; }
                drmModeFreeEncoder(encoder);
            }
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(resources);
    auto *planes = drmModeGetPlaneResources(drm);
    if (planes) {
        for (uint32_t i = 0; i < planes->count_planes; ++i) {
            auto *p = drmModeGetPlane(drm, planes->planes[i]);
            if (!p) continue;
            bool primary = false, overlay = false, rgba = false;
            for (uint32_t j = 0; j < p->count_formats; ++j)
                if (p->formats[j] == DRM_FORMAT_ABGR8888) rgba = true;
            auto *props = drmModeObjectGetProperties(drm, p->plane_id, DRM_MODE_OBJECT_PLANE);
            if (props) {
                for (uint32_t j = 0; j < props->count_props; ++j) {
                    auto *prop = drmModeGetProperty(drm, props->props[j]);
                    if (prop) {
                        if (!strcmp(prop->name, "type")) {
                            primary = props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY;
                            overlay = props->prop_values[j] == DRM_PLANE_TYPE_OVERLAY;
                        }
                        drmModeFreeProperty(prop);
                    }
                }
                drmModeFreeObjectProperties(props);
            }
            if (rgba && (p->possible_crtcs & (1U << crtcIndex))) {
                if (primary && !plane) plane = p->plane_id;
                if (overlay && !secondPlane) secondPlane = p->plane_id;
            }
            drmModeFreePlane(p);
        }
        drmModeFreePlaneResources(planes);
    }
    if (!connector || !crtc || !plane || mode.hdisplay != 1080 || mode.vdisplay != 2248 || drmModeCreatePropertyBlob(drm, &mode, sizeof(mode), &modeBlob)) {
        Log("no usable display"); CleanupDrm(); return FAIL;
    }
    for (auto item : {std::make_pair(crtc,uint32_t(DRM_MODE_OBJECT_CRTC)), std::make_pair(connector,uint32_t(DRM_MODE_OBJECT_CONNECTOR))}) {
        auto *props=drmModeObjectGetProperties(drm,item.first,item.second);
        if (!props) continue;
        for (uint32_t i=0;i<props->count_props;i++) {
            auto*p=drmModeGetProperty(drm,props->props[i]);
            if (!p) continue;
            if (strstr(p->name,"core_") || strstr(p->name,"fence") || strstr(p->name,"sync") || strstr(p->name,"trigger") || strstr(p->name,"idle") || !strcmp(p->name,"LP"))
                Ace5Log("initial_property object=%u name=%s value=%llu", item.first,p->name,(unsigned long long)props->prop_values[i]);
            drmModeFreeProperty(p);
        }
        drmModeFreeObjectProperties(props);
    }
    hardwareCrtcIndex = crtcIndex;
    uint64_t monotonic = 0;
    if (drmGetCap(drm, DRM_CAP_TIMESTAMP_MONOTONIC, &monotonic) || !monotonic) {
        Log("hardware vsync requires monotonic kernel timestamps"); CleanupDrm(); return FAIL;
    }
    clientCrop = {0, 0, mode.hdisplay, mode.vdisplay};
    Ace5Log("discovered Dipper DSI=%u CRTC=%u planes=%u,%u %ux%u@%u mm=%ux%u",
        connector, crtc, plane, secondPlane, mode.hdisplay, mode.vdisplay, mode.vrefresh, mmWidth, mmHeight);
    return OK;
}
uint64_t MonotonicNs()
{
    timespec now{}; clock_gettime(CLOCK_MONOTONIC, &now);
    return uint64_t(now.tv_sec) * 1000000000ULL + now.tv_nsec;
}
void *VsyncLoop(void *)
{
    unsigned sequence = 0, errors = 0;
    uint64_t generation = 0, previous = 0, intervals = 0, sum = 0, minimum = 0, maximum = 0;
    uint64_t dispatchCount = 0, callbackSum = 0, callbackMax = 0, callbackSlow = 0;
    uint64_t wakeSum = 0, wakeMax = 0, skipped = 0;
    unsigned previousHardwareSequence = 0;
    bool hardwareSequenceKnown = false;
    for (;;) {
        bool enabled;
        {
            std::lock_guard<std::mutex> state(vsyncMutex);
            if (quit) break;
            enabled = vsyncEnabled;
        }
        if (!enabled) { previous = 0; hardwareSequenceKnown = false; usleep(5000); continue; }
        // Only the vblank worker waits here: never hold the commit mutex.
        // Timestamp is the kernel's actual scanout timestamp, not wake time.
        drmVBlank wait{};
        wait.request.type = static_cast<drmVBlankSeqType>(DRM_VBLANK_RELATIVE |
            (hardwareCrtcIndex << DRM_VBLANK_HIGH_CRTC_SHIFT));
        wait.request.sequence = 1;
        int rc;
        do { rc = drmWaitVBlank(drm, &wait); } while (rc < 0 && errno == EINTR);
        if (rc) {
            if (++errors <= 4) Ace5Log("hardware vsync wait failed errno=%d", errno);
            usleep(20000); continue;
        }
        errors = 0;
        uint64_t timestamp = uint64_t(wait.reply.tval_sec) * 1000000000ULL +
            uint64_t(wait.reply.tval_usec) * 1000ULL;
        VBlankCallback callback; void *data; uint64_t period, currentGeneration;
        {
            std::lock_guard<std::mutex> state(vsyncMutex);
            if (quit) break;
            callback = vsyncEnabled ? vsyncCallback : nullptr; data = vsyncData;
            period = vsyncPeriod; currentGeneration = modeGeneration;
        }
        // 按内核时间戳统计周期，切换后的首个间隔不参与统计
        if (generation != currentGeneration) {
            previous = intervals = sum = minimum = maximum = 0;
            generation = currentGeneration;
            dispatchCount = callbackSum = callbackMax = callbackSlow = 0;
            wakeSum = wakeMax = skipped = 0; hardwareSequenceKnown = false;
        }
        if (previous && timestamp > previous && timestamp - previous < 100000000ULL) {
            uint64_t delta = timestamp - previous;
            sum += delta; ++intervals;
            if (!minimum || delta < minimum) minimum = delta;
            if (delta > maximum) maximum = delta;
            if (intervals >= 240) {
                Metric("HARDWARE_VSYNC expected_ns=%llu samples=%llu avg_ns=%llu min_ns=%llu max_ns=%llu",
                    (unsigned long long)period, (unsigned long long)intervals,
                    (unsigned long long)(sum / intervals), (unsigned long long)minimum,
                    (unsigned long long)maximum);
                intervals = sum = minimum = maximum = 0;
            }
        }
        previous = callback ? timestamp : 0;
        if (callback) {
            // 分别统计内核唤醒延迟与回调耗时，不改变提交时序
            uint64_t start = MonotonicNs();
            uint64_t wake = start > timestamp ? start - timestamp : 0;
            callback(++sequence, timestamp, data);
            uint64_t elapsed = MonotonicNs() - start;
            if (hardwareSequenceKnown) {
                unsigned delta = wait.reply.sequence - previousHardwareSequence;
                if (delta > 1 && delta < 1000) skipped += delta - 1;
            }
            previousHardwareSequence = wait.reply.sequence; hardwareSequenceKnown = true;
            ++dispatchCount; callbackSum += elapsed; wakeSum += wake;
            if (elapsed > callbackMax) callbackMax = elapsed;
            if (wake > wakeMax) wakeMax = wake;
            if (elapsed >= period) ++callbackSlow;
            if (dispatchCount >= 240) {
                Metric("VSYNC_DISPATCH period_ns=%llu callbacks=%llu callback_avg_us=%llu callback_max_us=%llu wake_avg_us=%llu wake_max_us=%llu skipped=%llu callback_over_period=%llu",
                    (unsigned long long)period, (unsigned long long)dispatchCount,
                    (unsigned long long)(callbackSum / dispatchCount / 1000),
                    (unsigned long long)(callbackMax / 1000),
                    (unsigned long long)(wakeSum / dispatchCount / 1000),
                    (unsigned long long)(wakeMax / 1000), (unsigned long long)skipped,
                    (unsigned long long)callbackSlow);
                dispatchCount = callbackSum = callbackMax = callbackSlow = 0;
                wakeSum = wakeMax = skipped = 0;
            }
        } else hardwareSequenceKnown = false;
    }
    return nullptr;
}
uint32_t Format(int format)
{
    switch (format) {
        case V::PIXEL_FMT_RGBA_8888: return DRM_FORMAT_ABGR8888;
        case V::PIXEL_FMT_RGBX_8888: return DRM_FORMAT_XBGR8888;
        case V::PIXEL_FMT_BGRA_8888: return DRM_FORMAT_ARGB8888;
        case V::PIXEL_FMT_BGRX_8888: return DRM_FORMAT_XRGB8888;
        case V::PIXEL_FMT_RGB_565: return DRM_FORMAT_RGB565;
        default: return 0;
    }
}
bool Linear(const BufferHandle &b)
{
    int bpp = b.format == V::PIXEL_FMT_RGB_565 ? 2 : 4;
    return Format(b.format) && b.fd >= 0 && !b.reserveFds && !b.reserveInts &&
        !(b.usage & (1ULL << 11)) && b.width > 0 && b.height > 0 &&
        b.width <= 16384 && b.height <= 16384 && int64_t(b.stride) >= int64_t(b.width) * bpp &&
        b.size > 0 && int64_t(b.stride) * b.height <= b.size;
}
void Metric(const char *format, ...)
{
    int saved = errno;
    FILE *file = fopen("/data/local/tmp/dipper-composer-metrics.txt", "ae");
    if (file) {
        fprintf(file, "t_ns=%llu ", (unsigned long long)MonotonicNs());
        va_list args; va_start(args, format); vfprintf(file, format, args); va_end(args);
        fputc('\n', file); fclose(file);
    }
    errno = saved;
}
// 保存原有混合属性，直扫与客户端回退分别使用不透明和原值
bool PlaneBlendProperties(drmModeAtomicReqPtr request, unsigned half, bool opaque)
{
    uint32_t target = ScanoutPlane(half);
    auto &rules = planeBlend[half];
    if (rules.id != target) { rules = {}; rules.id = target; }
    if (!rules.queried) {
        auto *props = drmModeObjectGetProperties(drm, target, DRM_MODE_OBJECT_PLANE);
        if (!props) return false;
        bool blend = false, alpha = false;
        for (uint32_t i = 0; i < props->count_props; ++i) {
            auto *p = drmModeGetProperty(drm, props->props[i]);
            if (!p) continue;
            if (!strcmp(p->name, "blend_op")) {
                for (int e = 0; e < p->count_enums; ++e)
                    if (!strcmp(p->enums[e].name, "opaque") && p->enums[e].value == 1) blend = true;
                rules.oldBlend = props->prop_values[i];
            } else if (!strcmp(p->name, "alpha") && (p->flags & DRM_MODE_PROP_RANGE) &&
                p->count_values >= 2 && !p->values[0] && p->values[1] > 0 && p->values[1] <= 65535) {
                rules.oldAlpha = props->prop_values[i]; rules.opaqueAlpha = p->values[1]; alpha = true;
            }
            drmModeFreeProperty(p);
        }
        drmModeFreeObjectProperties(props);
        rules.queried = true; rules.valid = blend && alpha;
    }
    if (!rules.valid) return false;
    bool blend = Property(request, target, DRM_MODE_OBJECT_PLANE, "blend_op",
        opaque ? 1 : rules.oldBlend) >= 0;
    bool alpha = Property(request, target, DRM_MODE_OBJECT_PLANE, "alpha",
        opaque ? rules.opaqueAlpha : rules.oldAlpha) >= 0;
    return blend && alpha;
}
// 为当前模式构造扫描属性，单 DSI 用一个平面，双 DSI 用两个半屏平面
bool ScanoutProperties(drmModeAtomicReqPtr request, uint32_t fb,
    const IRect &crop, uint64_t *retireFence, uint64_t *releaseFence = nullptr, bool opaque = false)
{
    bool valid = true;
#define PROP(object, type, name, value) valid &= Property(request, object, type, name, value) >= 0
    PROP(connector, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", crtc);
    PROP(crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", modeBlob);
    PROP(crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);
    if (retireFence) PROP(connector, DRM_MODE_OBJECT_CONNECTOR, "RETIRE_FENCE",
        reinterpret_cast<uintptr_t>(retireFence));
    // 层释放使用 CRTC 原生 fence，不复用连接器呈现 fence
    if (releaseFence) PROP(crtc, DRM_MODE_OBJECT_CRTC, "output_fence",
        reinterpret_cast<uintptr_t>(releaseFence));
    // Partition the source exactly; no gap or duplicated column at the seam.
    const unsigned planeCount = ScanoutPlaneCount();
    const int leftSourceWidth = crop.w / int(planeCount);
    for (unsigned half = 0; half < planeCount; ++half) {
        const uint32_t target = ScanoutPlane(half);
        const int sourceX = crop.x + (half ? leftSourceWidth : 0);
        const int sourceWidth = half ? crop.w - leftSourceWidth : leftSourceWidth;
        const uint32_t outputWidth = mode.hdisplay / planeCount;
        PROP(target, DRM_MODE_OBJECT_PLANE, "FB_ID", fb);
        PROP(target, DRM_MODE_OBJECT_PLANE, "CRTC_ID", crtc);
        PROP(target, DRM_MODE_OBJECT_PLANE, "SRC_X", uint64_t(sourceX) << 16);
        PROP(target, DRM_MODE_OBJECT_PLANE, "SRC_Y", uint64_t(crop.y) << 16);
        PROP(target, DRM_MODE_OBJECT_PLANE, "SRC_W", uint64_t(sourceWidth) << 16);
        PROP(target, DRM_MODE_OBJECT_PLANE, "SRC_H", uint64_t(crop.h) << 16);
        PROP(target, DRM_MODE_OBJECT_PLANE, "CRTC_X", half * outputWidth);
        PROP(target, DRM_MODE_OBJECT_PLANE, "CRTC_Y", 0);
        PROP(target, DRM_MODE_OBJECT_PLANE, "CRTC_W", outputWidth);
        PROP(target, DRM_MODE_OBJECT_PLANE, "CRTC_H", mode.vdisplay);
        PROP(target, DRM_MODE_OBJECT_PLANE, "color_fill", 0);
        if (opaque || planeRulesApplied) valid &= PlaneBlendProperties(request, half, opaque);
    }
#undef PROP
    return valid;
}
struct LayerTestResult {
    int result = UNSUPPORTED, error = 0, cleanup = 0;
    const char *stage = "candidate";
};
int singleLayerCleanupError = 0;
bool FullScreen(const IRect &rect)
{
    return !rect.x && !rect.y && rect.w == mode.hdisplay && rect.h == mode.vdisplay;
}
// 仅检查不透明单层及当前保存的旋转状态
const char *SingleLayerReason(const Layer &layer)
{
    const auto &b = layer.buffer.value;
    if (!requestedPowerOn) return "power_off";
    if (layer.composition != V::COMPOSITION_DEVICE) return "composition";
    if (!plane || (ScanoutPlaneCount() > 1 && (mode.hdisplay & 1))) return "planes";
    if (!Linear(b)) return "layout";
    if (b.width != mode.hdisplay || b.height != mode.vdisplay) return "buffer_size";
    if (!FullScreen(layer.region) || !FullScreen(layer.crop)) return "rect";
    if (layer.visible.size() != 1 || !FullScreen(layer.visible[0])) return "visible";
    if (layer.transform != V::ROTATE_NONE) return "transform";
    if (layer.mask != V::LAYER_NORAML) return "mask";
    if (layer.alpha.enPixelAlpha || (layer.alpha.enGlobalAlpha && layer.alpha.gAlpha != 255))
        return "alpha";
    if (layer.blend != V::BLEND_NONE && layer.blend != V::BLEND_SRC &&
        layer.blend != V::BLEND_SRCOVER) return "blend";
    return nullptr;
}
// 临时导入候选缓冲区，仅验证双半屏原子请求
LayerTestResult TestSingleLayer(const Layer &layer)
{
    const auto &b = layer.buffer.value;
    uint32_t handle = 0, fb = 0;
    if (drmPrimeFDToHandle(drm, b.fd, &handle)) return {FAIL, errno, 0, "import"};
    uint32_t handles[4]{handle}, pitches[4]{uint32_t(b.stride)}, offsets[4]{};
    int rc = drmModeAddFB2(drm, b.width, b.height, Format(b.format), handles, pitches,
        offsets, &fb, 0);
    int error = errno;
    drm_gem_close closeHandle{handle, 0};
    int cleanup = ioctl(drm, DRM_IOCTL_GEM_CLOSE, &closeHandle) ? errno : 0;
    if (rc) return {FAIL, error, cleanup, "framebuffer"};
    if (cleanup) {
        error = cleanup;
        drmModeRmFB(drm, fb);
        return {FAIL, error, cleanup, "gem_close"};
    }
    auto *request = drmModeAtomicAlloc();
    if (!request) {
        error = errno;
        if (drmModeRmFB(drm, fb) && !cleanup) cleanup = errno;
        return {FAIL, error, cleanup, "request"};
    }
    bool valid = ScanoutProperties(request, fb, layer.crop, nullptr, nullptr, true);
    rc = valid ? drmModeAtomicCommit(drm, request,
        DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_ATOMIC_TEST_ONLY, nullptr) : FAIL;
    error = errno;
    drmModeAtomicFree(request);
    if (drmModeRmFB(drm, fb) && !cleanup) cleanup = errno;
    return {rc || cleanup ? FAIL : OK, rc ? error : 0, cleanup, valid ? "test_only" : "properties"};
}
// 零为原路径，一为影子测试，二为受限单层直扫
int ReadScanoutPolicy()
{
#ifdef ACE5_DIRECT_TEST
    if (scanoutPolicyOverride >= 0) return scanoutPolicyOverride;
#endif
    int fd = open("/dev/dipper-display-route-mode",
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    // 控制文件缺失时使用构建默认，其他打开失败保持原路径
    if (fd < 0) {
#if DIPPER_DEFAULT_DISPLAY_ROUTE == 2
        return errno == ENOENT ? 2 : 0;
#else
        return 0;
#endif
    }
    char enabled = 0;
    // 固定控制文件仅取首字符，允许常规换行及原有文件权限
    int result = read(fd, &enabled, 1) == 1 && enabled >= '0' && enabled <= '2' ? enabled - '0' : 0;
    close(fd); return result;
}
// 在持锁的准备与提交入口刷新策略，不丢弃已提交的释放 fence
void RefreshScanoutPolicy(uint64_t now)
{
    if (policyReadTime && (now < policyReadTime || now - policyReadTime < 2000000000ULL)) return;
    policyReadTime = now;
    int saved = errno;
    int policy = ReadScanoutPolicy();
    if (policy != scanoutPolicy) {
        Metric("SCANOUT_POLICY previous=%d requested=%d", scanoutPolicy, policy);
        scanoutPolicy = policy;
    }
    errno = saved;
}
// 仅显式影子模式执行周期性诊断，不干扰正式直扫
void RecordSingleLayerDiagnostic(uint64_t now)
{
    static uint64_t last = 0;
    if (last && (now < last || now - last < 2000000000ULL)) return;
    last = now;
    int saved = errno;
    RefreshScanoutPolicy(now);
    if (scanoutPolicy != 1) { errno = saved; return; }
    if (layers.size() != 1) {
        Metric("LAYER_TEST count=%zu stage=layer_count result=%d", layers.size(), UNSUPPORTED);
        errno = saved; return;
    }
    const auto &entry = *layers.begin();
    const auto &layer = entry.second;
    const auto &b = layer.buffer.value;
    const char *reason = SingleLayerReason(layer);
    LayerTestResult result;
    if (reason) result.stage = reason;
    else if (singleLayerCleanupError) result = {FAIL, singleLayerCleanupError, 0, "cleanup_latched"};
    else {
        result = TestSingleLayer(layer);
        // 清理失败后停止分配测试资源，保留元数据诊断
        if (result.cleanup) singleLayerCleanupError = result.cleanup;
    }
    Metric("LAYER_TEST count=1 id=%u stage=%s result=%d error=%d cleanup_error=%d latched_error=%d "
        "transform=%d format=%d width=%d height=%d stride=%d size=%lld usage=%llu "
        "reserve_fds=%u reserve_ints=%u region=%d,%d,%d,%d crop=%d,%d,%d,%d "
        "visible=%zu global_alpha=%d pixel_alpha=%d alpha=%u premulti=%d blend=%d mask=%d "
        "composition=%d acquire_fence=%d client_fb=%u mode_id=%u policy=%d prepared_layer=%u "
        "direct_frames=%llu direct_failures=%llu applied_error=%d",
        entry.first, result.stage, result.result, result.error, result.cleanup, singleLayerCleanupError,
        int(layer.transform), b.format, b.width, b.height, b.stride, (long long)b.size,
        (unsigned long long)b.usage, b.reserveFds, b.reserveInts,
        layer.region.x, layer.region.y, layer.region.w, layer.region.h,
        layer.crop.x, layer.crop.y, layer.crop.w, layer.crop.h, layer.visible.size(),
        int(layer.alpha.enGlobalAlpha), int(layer.alpha.enPixelAlpha), unsigned(layer.alpha.gAlpha),
        int(layer.premulti), int(layer.blend), int(layer.mask), int(layer.composition),
        layer.buffer.fence >= 0, currentFb, selectedMode, scanoutPolicy, preparedDirectLayer,
        (unsigned long long)directFrames, (unsigned long long)directFailures, int(directAppliedError));
    errno = saved;
}
struct FenceObservation {
    enum State { MISSING, READY, PENDING, ERROR, COUNT } state = MISSING;
    uint64_t duration = 0;
};
// 零超时查询只观察 fence，不等待或转移所有权
FenceObservation ObserveFence(int fd)
{
    if (fd < 0) return {};
    int saved = errno;
    uint64_t begin = MonotonicNs();
    pollfd descriptor{fd, POLLIN, 0};
    int result = poll(&descriptor, 1, 0);
    uint64_t end = MonotonicNs();
    errno = saved;
    FenceObservation observation;
    if (end >= begin) observation.duration = end - begin;
    if (result < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)))
        observation.state = FenceObservation::ERROR;
    else if (result > 0 && (descriptor.revents & POLLIN))
        observation.state = FenceObservation::READY;
    else if (!result)
        observation.state = FenceObservation::PENDING;
    else observation.state = FenceObservation::ERROR;
    return observation;
}
// Aggregate only: avoid per-frame logging changing the pacing being measured.
struct SubmitMetrics {
    uint64_t begin = 0, frames = 0, sum[5]{}, maximum = 0, slow = 0;
    uint64_t last = 0, gaps[8]{};
    uint64_t lastEntry = 0, lastReturn = 0;
    bool continuous = false, routeKnown = false, directRoute = false;
    // 路径切换先记录未满统计桶，再断开跨路径间隔
    void StartRoute(bool direct)
    {
        if (routeKnown && directRoute != direct) {
            Metric("SUBMIT_ROUTE previous=%s next=%s partial_frames=%llu begin_ns=%llu last_ns=%llu atomic_total=%llu atomic_end_ns=%llu",
                directRoute ? "DIRECT" : "CLIENT", direct ? "DIRECT" : "CLIENT",
                (unsigned long long)frames, (unsigned long long)begin, (unsigned long long)last,
                (unsigned long long)frameAtomicTotal, (unsigned long long)frameAtomicEnd);
            *this = {};
        }
        routeKnown = true; directRoute = direct;
    }
    uint64_t pairs = 0, entrySum = 0, entryMin = 0, entryMax = 0;
    uint64_t outsideSum = 0, outsideMax = 0, breaks = 0;
    uint64_t serviceSum = 0, serviceMax = 0, clockErrors = 0;
    uint64_t fenceStates[FenceObservation::COUNT]{}, probeSum = 0, probeMax = 0;
    // 分别记录入口节奏、提交耗时与两次调用之间的空隙
    void Record(const uint64_t (&t)[6], bool previous,
        const FenceObservation &fence) {
        if (previous && lastEntry && lastReturn >= lastEntry && t[0] >= lastReturn) {
            uint64_t interval = t[0] - lastEntry, outside = t[0] - lastReturn;
            uint64_t service = lastReturn - lastEntry;
            ++pairs; entrySum += interval; outsideSum += outside;
            serviceSum += service;
            if (!entryMin || interval < entryMin) entryMin = interval;
            if (interval > entryMax) entryMax = interval;
            if (outside > outsideMax) outsideMax = outside;
            if (service > serviceMax) serviceMax = service;
        } else {
            ++breaks;
            if (previous) ++clockErrors;
        }
        lastEntry = t[0];
        ++fenceStates[fence.state]; probeSum += fence.duration;
        if (fence.duration > probeMax) probeMax = fence.duration;
        if (last) {
            uint64_t gap = t[5] - last;
            const uint64_t limits[] = {8000000,12000000,17000000,25000000,34000000,50000000,100000000};
            unsigned bin = 0; while (bin < 7 && gap > limits[bin]) ++bin;
            ++gaps[bin];
        }
        last = t[5];
        if (!begin) begin = t[0];
        ++frames;
        for (int i=0;i<5;++i) sum[i] += t[i+1]-t[i];
        uint64_t total=t[5]-t[0];
        if (total>maximum) maximum=total;
        if (total>25000000ULL) ++slow;
        if (t[5]-begin < 2000000000ULL) return;
        Ace5Log("submit frames=%llu span_ms=%llu avg_us fence=%llu import=%llu props=%llu commit=%llu cleanup=%llu max_us=%llu over25ms=%llu",
            (unsigned long long)frames, (unsigned long long)((t[5]-begin)/1000000),
            (unsigned long long)(sum[0]/frames/1000), (unsigned long long)(sum[1]/frames/1000),
            (unsigned long long)(sum[2]/frames/1000), (unsigned long long)(sum[3]/frames/1000),
            (unsigned long long)(sum[4]/frames/1000), (unsigned long long)(maximum/1000),
            (unsigned long long)slow);
        Metric("submit frames=%llu span_ms=%llu avg_us fence=%llu import=%llu props=%llu commit=%llu cleanup=%llu max_us=%llu over25ms=%llu",
            (unsigned long long)frames, (unsigned long long)((t[5]-begin)/1000000),
            (unsigned long long)(sum[0]/frames/1000), (unsigned long long)(sum[1]/frames/1000),
            (unsigned long long)(sum[2]/frames/1000), (unsigned long long)(sum[3]/frames/1000),
            (unsigned long long)(sum[4]/frames/1000), (unsigned long long)(maximum/1000),
            (unsigned long long)slow);
        Metric("FRAME_GAPS bins_ms=le8,le12,le17,le25,le34,le50,le100,gt100 counts=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu",
            (unsigned long long)gaps[0],(unsigned long long)gaps[1],(unsigned long long)gaps[2],(unsigned long long)gaps[3],
            (unsigned long long)gaps[4],(unsigned long long)gaps[5],(unsigned long long)gaps[6],(unsigned long long)gaps[7]);
        uint64_t probes = frames - fenceStates[FenceObservation::MISSING];
        Metric("SUBMIT_PACING frames=%llu pairs=%llu entry_avg_us=%llu entry_min_us=%llu entry_max_us=%llu "
            "previous_service_avg_us=%llu previous_service_max_us=%llu outside_avg_us=%llu outside_max_us=%llu "
            "chain_breaks=%llu clock_errors=%llu fence_missing=%llu fence_ready=%llu fence_pending=%llu fence_error=%llu "
            "probe_samples=%llu probe_avg_ns=%llu probe_max_ns=%llu route=%s atomic_total=%llu atomic_end_ns=%llu",
            (unsigned long long)frames, (unsigned long long)pairs,
            (unsigned long long)(pairs ? entrySum / pairs / 1000 : 0),
            (unsigned long long)(entryMin / 1000), (unsigned long long)(entryMax / 1000),
            (unsigned long long)(pairs ? serviceSum / pairs / 1000 : 0),
            (unsigned long long)(serviceMax / 1000),
            (unsigned long long)(pairs ? outsideSum / pairs / 1000 : 0),
            (unsigned long long)(outsideMax / 1000),
            (unsigned long long)breaks, (unsigned long long)clockErrors,
            (unsigned long long)fenceStates[FenceObservation::MISSING],
            (unsigned long long)fenceStates[FenceObservation::READY],
            (unsigned long long)fenceStates[FenceObservation::PENDING],
            (unsigned long long)fenceStates[FenceObservation::ERROR],
            (unsigned long long)probes,
            (unsigned long long)(probes ? probeSum / probes : 0), (unsigned long long)probeMax,
            directRoute ? "DIRECT" : "CLIENT", (unsigned long long)frameAtomicTotal,
            (unsigned long long)frameAtomicEnd);
        RecordSingleLayerDiagnostic(t[5]);
        uint64_t previousLast = last, previousEntry = lastEntry, previousReturn = lastReturn;
        bool previousRoute = directRoute;
        *this = {};
        last = previousLast; lastEntry = previousEntry; lastReturn = previousReturn;
        routeKnown = true; directRoute = previousRoute;
    }
} submitMetrics;
int Submit(bool testOnly, int32_t *presentFence = nullptr)
{
    if (presentFence) *presentFence = -1;
    if (!testOnly && !requestedPowerOn) {
        if (!loggedOffSubmission) {
            Ace5Log("POWER_OFF_LATCH suppressed late frame without DRM commit");
            loggedOffSubmission = true;
        }
        submitMetrics.continuous = false;
        return OK;
    }
    // 只统计实际提交路径，关屏空返回不建立新统计桶
    if (!testOnly) submitMetrics.StartRoute(false);
    bool previous = submitMetrics.continuous;
    submitMetrics.continuous = false;
    uint64_t timing[6]{MonotonicNs()};
    const auto &b = client.value;
    if (!Linear(b)) return INVALID;
    if (clientCrop.w < 2 || int64_t(clientCrop.x) + clientCrop.w > b.width ||
        int64_t(clientCrop.y) + clientCrop.h > b.height) return INVALID;
    if (client.fence >= 0) {
        pollfd p{client.fence, POLLIN, 0}; int rc;
        do { rc = poll(&p, 1, 1000); } while (rc < 0 && errno == EINTR);
        if (rc <= 0 || !(p.revents & POLLIN)) return FAIL;
    }
    timing[1] = MonotonicNs();
    uint32_t handle = 0, fb = 0;
    if (drmPrimeFDToHandle(drm, b.fd, &handle)) { Log("prime import failed"); return FAIL; }
    uint32_t handles[4]{handle}, pitches[4]{uint32_t(b.stride)}, offsets[4]{};
    int rc = drmModeAddFB2(drm, b.width, b.height, Format(b.format), handles, pitches, offsets, &fb, 0);
    drm_gem_close closeHandle{handle, 0}; ioctl(drm, DRM_IOCTL_GEM_CLOSE, &closeHandle);
    if (rc) { Log("AddFB2 failed"); return FAIL; }
    auto *request = drmModeAtomicAlloc();
    if (!request) { drmModeRmFB(drm, fb); return FAIL; }
    timing[2] = MonotonicNs();
    uint64_t retireFence = UINT64_MAX;
    bool valid = ScanoutProperties(request, fb, clientCrop, testOnly ? nullptr : &retireFence);
    timing[3] = MonotonicNs();
    rc = valid ? drmModeAtomicCommit(drm, request,
        DRM_MODE_ATOMIC_ALLOW_MODESET | (testOnly ? DRM_MODE_ATOMIC_TEST_ONLY : 0), nullptr) : -1;
    timing[4] = MonotonicNs();
    if (!rc && !testOnly) { ++frameAtomicTotal; frameAtomicEnd = timing[4]; }
    drmModeAtomicFree(request);
    // 失败提交也可能已创建 fence，未移交的描述符由作用域释放
    OwnedFence present(retireFence <= INT_MAX ? int(retireFence) : -1);
    if (present.fd >= 0) fcntl(present.fd, F_SETFD, FD_CLOEXEC);
    if (rc || testOnly) drmModeRmFB(drm, fb);
    else { if (currentFb) drmModeRmFB(drm, currentFb); currentFb = fb; }
    timing[5] = MonotonicNs();
    FenceObservation observation;
    if (!rc && !testOnly) observation = ObserveFence(present.fd);
    if (!rc && !testOnly && presentFence) {
        *presentFence = present.Take();
        static bool reported = false;
        if (!reported) {
            Metric("PRESENT_FENCE first_fd=%d blocking_commit=1", *presentFence);
            reported = true;
        }
    }
    if (!rc && !testOnly) {
        planeRulesApplied = false; directReleaseUnavailable = false;
        submitMetrics.Record(timing, previous, observation);
        // 返回时间在聚合日志之后采样，避免将日志开销计入外部空隙
        int savedErrno = errno;
        submitMetrics.lastReturn = MonotonicNs();
        errno = savedErrno;
        submitMetrics.continuous = true;
    }
    if (rc) Log("atomic commit failed");
    return rc == 0 ? OK : FAIL;
}
// 直扫复用原层缓冲区，分别保留原生呈现与层释放 fence
int SubmitDirect(uint32_t layerId, const Layer &layer, int32_t &presentFence)
{
    submitMetrics.StartRoute(true);
    bool previous = submitMetrics.continuous;
    submitMetrics.continuous = false;
    presentFence = -1;
    uint64_t timing[6]{MonotonicNs()};
    const auto &b = layer.buffer.value;
    if (layer.buffer.fence >= 0) {
        pollfd p{layer.buffer.fence, POLLIN, 0}; int rc;
        do { rc = poll(&p, 1, 1000); } while (rc < 0 && errno == EINTR);
        if (rc <= 0 || !(p.revents & POLLIN) || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) return FAIL;
    }
    timing[1] = MonotonicNs();
    uint32_t handle = 0, fb = 0;
    if (drmPrimeFDToHandle(drm, b.fd, &handle)) return FAIL;
    uint32_t handles[4]{handle}, pitches[4]{uint32_t(b.stride)}, offsets[4]{};
    int rc = drmModeAddFB2(drm, b.width, b.height, Format(b.format), handles, pitches, offsets, &fb, 0);
    int error = errno;
    drm_gem_close closeHandle{handle, 0};
    int cleanup = ioctl(drm, DRM_IOCTL_GEM_CLOSE, &closeHandle) ? errno : 0;
    if (rc || cleanup) {
        if (!rc && drmModeRmFB(drm, fb) && !cleanup) cleanup = errno;
        if (cleanup) singleLayerCleanupError = cleanup;
        errno = rc ? error : cleanup; return FAIL;
    }
    auto *request = drmModeAtomicAlloc();
    if (!request) {
        error = errno;
        if (drmModeRmFB(drm, fb)) singleLayerCleanupError = errno;
        errno = error; return FAIL;
    }
    timing[2] = MonotonicNs();
    uint64_t presentSlot = UINT64_MAX, releaseSlot = UINT64_MAX;
    bool valid = ScanoutProperties(request, fb, layer.crop, &presentSlot, &releaseSlot, true);
    timing[3] = MonotonicNs();
    rc = valid ? drmModeAtomicCommit(drm, request, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr) : FAIL;
    error = errno;
    timing[4] = MonotonicNs();
    if (!rc) { ++frameAtomicTotal; frameAtomicEnd = timing[4]; }
    drmModeAtomicFree(request);
    OwnedFence present(presentSlot <= INT_MAX ? int(presentSlot) : -1);
    OwnedFence release(releaseSlot <= INT_MAX ? int(releaseSlot) : -1);
    if (present.fd >= 0) fcntl(present.fd, F_SETFD, FD_CLOEXEC);
    if (release.fd >= 0) fcntl(release.fd, F_SETFD, FD_CLOEXEC);
    if (rc) {
        if (drmModeRmFB(drm, fb)) singleLayerCleanupError = errno;
        errno = error; return FAIL;
    }
    // 已应用的提交必须保留新 framebuffer，不能按提交失败回收
    if (currentFb && drmModeRmFB(drm, currentFb)) singleLayerCleanupError = errno;
    currentFb = fb; planeRulesApplied = true;
    timing[5] = MonotonicNs();
    if (release.fd < 0) {
        directAppliedError = true; directReleaseUnavailable = true; directRelease.Reset();
        Metric("DIRECT_APPLIED_ERROR layer=%u framebuffer=%u release_slot=%llu present_slot=%llu atomic_total=%llu atomic_end_ns=%llu",
            layerId, fb, (unsigned long long)releaseSlot, (unsigned long long)presentSlot,
            (unsigned long long)frameAtomicTotal, (unsigned long long)frameAtomicEnd);
        return FAIL;
    }
    directRelease.Adopt(layerId, release.Take());
    presentFence = present.Take();
    ++directFrames;
    FenceObservation observation = ObserveFence(presentFence);
    submitMetrics.Record(timing, previous, observation);
    int saved = errno;
    submitMetrics.lastReturn = MonotonicNs();
    errno = saved;
    submitMetrics.continuous = true;
    return OK;
}
// 在声明设备合成之前验证当前单层，不修改已提交结果
void PrepareSingleLayer(bool &needFlushFb)
{
    RefreshScanoutPolicy(MonotonicNs());
    preparedDirectLayer = 0; needFlushFb = true;
    if (scanoutPolicy != 2 || requireClientFrame || directAppliedError ||
        singleLayerCleanupError || layers.size() != 1) return;
    const auto &entry = *layers.begin();
    if (SingleLayerReason(entry.second)) return;
    LayerTestResult test = TestSingleLayer(entry.second);
    if (test.cleanup) singleLayerCleanupError = test.cleanup;
    if (test.result) return;
    preparedDirectLayer = entry.first; needFlushFb = false;
}
// 跳过重绘后的失败不使用旧客户端帧，保留旧扫描并重新协商
int CommitFrame(int32_t &fence)
{
    RefreshScanoutPolicy(MonotonicNs());
    if (!preparedDirectLayer) {
        int result = Submit(false, &fence);
        if (!result && requestedPowerOn) requireClientFrame = false;
        return result;
    }
    uint32_t selected = preparedDirectLayer;
    auto it = layers.find(selected);
    bool eligible = scanoutPolicy == 2 && !singleLayerCleanupError && !directAppliedError &&
        it != layers.end() && layers.size() == 1 && !SingleLayerReason(it->second);
    int result = FAIL;
    fence = -1;
    if (!requestedPowerOn) {
        preparedDirectLayer = 0; return OK;
    }
    if (eligible) {
        LayerTestResult test = TestSingleLayer(it->second);
        if (test.cleanup) singleLayerCleanupError = test.cleanup;
        if (!test.result) result = SubmitDirect(selected, it->second, fence);
    }
    if (result) {
        ++directFailures; preparedDirectLayer = 0; requireClientFrame = true;
        submitMetrics.continuous = false;
    }
    return result;
}
int BlackPowerOn()
{
    int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (heap < 0) return FAIL;
    uint32_t pitch = (uint32_t(mode.hdisplay) * 4 + 255) & ~255U;
    dma_heap_allocation_data alloc{};
    alloc.len = (uint64_t(pitch) * mode.vdisplay + 4095) & ~4095ULL;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;
    int result = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &alloc); close(heap);
    if (result) return FAIL;
    void *map = mmap(nullptr, alloc.len, PROT_WRITE, MAP_SHARED, alloc.fd, 0);
    if (map == MAP_FAILED) { close(alloc.fd); return FAIL; }
    dma_buf_sync sync{DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE};
    result = ioctl(alloc.fd, DMA_BUF_IOCTL_SYNC, &sync);
    if (!result) {
        memset(map, 0, alloc.len);
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
        result = ioctl(alloc.fd, DMA_BUF_IOCTL_SYNC, &sync);
    }
    munmap(map, alloc.len);
    BufferHandle b{};
    b.fd = alloc.fd; b.width = mode.hdisplay; b.height = mode.vdisplay;
    b.stride = pitch; b.size = alloc.len; b.format = V::PIXEL_FMT_RGBX_8888;
    OwnedFence noFence(-1);
    if (!result) result = client.Set(b, noFence);
    close(alloc.fd);
    if (result) return FAIL;
    clientCrop = {0, 0, mode.hdisplay, mode.vdisplay};
    return Submit(false);
}
// 采用先检验模式再提交的方法，失败时保留目前状态防止黑砖救不了
int ChangeMode(uint32_t modeId)
{
    if (modeId >= displayModes.size()) return INVALID;
    if (modeId == selectedMode) return OK;
    const auto &candidate = displayModes[modeId];
    uint32_t candidateBlob = 0;
    if (drmModeCreatePropertyBlob(drm, &candidate, sizeof(candidate), &candidateBlob)) return FAIL;
    auto *request = drmModeAtomicAlloc();
    if (!request) { drmModeDestroyPropertyBlob(drm, candidateBlob); return FAIL; }
    // 仅改变模式属性，保留双屏平面、帧缓冲及亮灭屏状态
    int result = Property(request, crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", candidateBlob) < 0 ? FAIL : OK;
    if (!result) result = drmModeAtomicCommit(drm, request,
        DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_ATOMIC_TEST_ONLY, nullptr);
    if (!result) result = drmModeAtomicCommit(drm, request, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
    int saved = errno;
    drmModeAtomicFree(request);
    if (result) {
        drmModeDestroyPropertyBlob(drm, candidateBlob);
        Metric("MODE_REJECT id=%u hz=%u errno=%d", modeId, candidate.vrefresh, saved);
        return FAIL;
    }
    uint32_t previousBlob = modeBlob;
    mode = candidate; modeBlob = candidateBlob; selectedMode = modeId;
    {
        std::lock_guard<std::mutex> state(vsyncMutex);
        vsyncPeriod = Period(); ++modeGeneration;
    }
    if (previousBlob) drmModeDestroyPropertyBlob(drm, previousBlob);
    Metric("MODE_APPLIED id=%u hz=%u period_ns=%llu powered_on=%d",
        selectedMode, mode.vrefresh, (unsigned long long)Period(), requestedPowerOn);
    Ace5Log("mode applied id=%u %ux%u@%u", selectedMode, mode.hdisplay, mode.vdisplay, mode.vrefresh);
    return OK;
}
}

#define DEVICE_LOCK(id) std::lock_guard<std::mutex> lock(mutex); if (!Device(id)) return INVALID
#define LAYER_LOCK(id, lid) DEVICE_LOCK(id); auto it = layers.find(lid); if (it == layers.end()) return INVALID; auto &layer = it->second
extern "C" int32_t LoadVdiImpl() { std::lock_guard<std::mutex> lock(mutex); return Initialize(); }
extern "C" int32_t DestroyVdiImpl()
{
    { std::lock_guard<std::mutex> lock(vsyncMutex); quit = true; }
    if (threadStarted) pthread_join(vsyncThread, nullptr);
    std::lock_guard<std::mutex> lock(mutex);
    threadStarted = false;
    { std::lock_guard<std::mutex> state(vsyncMutex); vsyncEnabled = false; vsyncCallback = nullptr; }
    layers.clear(); client.Clear(); CleanupDrm(); return OK;
}
extern "C" int32_t RegHotPlugCallback(HotPlugCallback cb, void *data)
{
    if (!cb) return INVALID;
    { std::lock_guard<std::mutex> lock(mutex); if (Initialize()) return FAIL; }
    cb(0, true, data); return OK;
}
extern "C" int32_t GetDisplayCapability(uint32_t id, DisplayCapability &info)
{
    DEVICE_LOCK(id); info = {}; info.name = "Dipper DSI"; // The donor directly casts the HDI value to GraphicInterfaceType, which
    // includes UNKNOWN=0. Its live dump identified 9 as VGA; MIPI is 10.
    info.type = static_cast<decltype(info.type)>(10);
    info.phyWidth = mmWidth; info.phyHeight = mmHeight; info.supportLayers = 64;
    return OK;
}
extern "C" int32_t GetDisplaySupportedModes(uint32_t id, std::vector<DisplayModeInfo> &modes)
{
    DEVICE_LOCK(id); modes.clear();
    // 使用连接器枚举顺序作为稳定编号，保留内核提供的真实时序
    for (uint32_t i = 0; i < displayModes.size(); ++i) {
        const auto &item = displayModes[i];
        modes.push_back({item.hdisplay, item.vdisplay, item.vrefresh, int32_t(i)});
    }
    return OK;
}
extern "C" int32_t GetDisplaySupportedModesExt(uint32_t id, std::vector<DisplayModeInfoExt> &modes)
{
    DEVICE_LOCK(id); modes.clear();
    for (uint32_t i = 0; i < displayModes.size(); ++i) {
        const auto &item = displayModes[i];
        modes.push_back({{item.hdisplay, item.vdisplay, item.vrefresh, int32_t(i)}, i});
    }
    return OK;
}
extern "C" int32_t GetDisplayMode(uint32_t id, uint32_t &modeId)
{ DEVICE_LOCK(id); modeId = selectedMode; return OK; }
extern "C" int32_t SetDisplayMode(uint32_t id, uint32_t modeId)
{ DEVICE_LOCK(id); return ChangeMode(modeId); }
extern "C" int32_t SetDisplayModeAsync(uint32_t id, uint32_t modeId, ModeCallback cb, void *data)
{
    if (!cb) return INVALID;
    uint64_t period;
    {
        DEVICE_LOCK(id);
        int result = ChangeMode(modeId);
        if (result) return result;
        period = Period();
    }
    // 解锁后回报已生效的周期，允许回调再次查询显示状态
    cb(modeId, period, data);
    return OK;
}
extern "C" int32_t GetDisplayPowerStatus(uint32_t id, V::DispPowerStatus &status)
{
    DEVICE_LOCK(id); bool active;
    if (Active(active)) return FAIL;
    status = active ? V::POWER_STATUS_ON : V::POWER_STATUS_OFF;
    return OK;
}
extern "C" int32_t SetDisplayPowerStatus(uint32_t id, V::DispPowerStatus status)
{
    DEVICE_LOCK(id);
    if (status != V::POWER_STATUS_ON && status != V::POWER_STATUS_OFF) return UNSUPPORTED;
    bool active;
    if (Active(active)) return FAIL;
    bool wanted = status == V::POWER_STATUS_ON;
    if (active == wanted) {
        requestedPowerOn = wanted;
        loggedOffSubmission = false;
        if (!wanted) { directRelease.Reset(); preparedDirectLayer = 0; directReleaseUnavailable = false; }
        return OK;
    }
    const bool previousRequest = requestedPowerOn;
    // ON must be able to submit the retained client buffer. Restore the prior
    // request if the atomic power transition fails, instead of claiming success.
    if (wanted) requestedPowerOn = true;
    int result;
    if (wanted) result = client.value.fd >= 0 ? Submit(false) : BlackPowerOn();
    else {
        auto *request = drmModeAtomicAlloc();
        if (!request) return FAIL;
        // DIPPER_FULL_POWER_OFF_V1: detach all objects, so reactivation
        // rebuilds the complete routing state instead of retaining a mode.
        bool valid = true;
        for (unsigned half = 0; half < ScanoutPlaneCount(); ++half) {
            const uint32_t target = ScanoutPlane(half);
            valid &= Property(request, target, DRM_MODE_OBJECT_PLANE, "FB_ID", 0) >= 0;
            valid &= Property(request, target, DRM_MODE_OBJECT_PLANE, "CRTC_ID", 0) >= 0;
        }
        valid &= Property(request, connector, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", 0) >= 0;
        valid &= Property(request, crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", 0) >= 0;
        valid &= Property(request, crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", 0) >= 0;
        result = valid ? drmModeAtomicCommit(drm, request, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr) : FAIL;
        drmModeAtomicFree(request);
        if (!result && currentFb) { drmModeRmFB(drm, currentFb); currentFb = 0; }
    }
    if (!result) {
        requestedPowerOn = wanted;
        loggedOffSubmission = false;
        if (!wanted) { directRelease.Reset(); preparedDirectLayer = 0; directReleaseUnavailable = false; }
    } else {
        requestedPowerOn = previousRequest;
    }
    Ace5Log("power requested=%d result=%d latched_on=%d", int(status), result, requestedPowerOn);
    return result == 0 ? OK : FAIL;
}
// Donor UI is 0..255; its display pipeline sends a distinct 0..65535 HDI unit.
// Verified original LUT has 65535 entries and real UI requests exceed 255.
int ReadPanelValue(const char *name, uint32_t &value)
{
    char path[160]; snprintf(path,sizeof(path),"/sys/class/backlight/panel0-backlight/%s",name);
    FILE *file=fopen(path,"r"); if(!file)return FAIL;
    unsigned n=0; int result=fscanf(file,"%u",&n); fclose(file);
    if(result!=1)return FAIL; value=n; return OK;
}
extern "C" int32_t GetDisplayBacklight(uint32_t id, uint32_t &level)
{
    DEVICE_LOCK(id); uint32_t maximum=0,native=0;
    if(ReadPanelValue("max_brightness",maximum)||!maximum||ReadPanelValue("brightness",native)||native>maximum)return FAIL;
    level=(uint64_t(native)*65535+maximum/2)/maximum; return OK;
}
extern "C" int32_t SetDisplayBacklight(uint32_t id, uint32_t level)
{
    DEVICE_LOCK(id);
    Metric("BACKLIGHT_REQUEST raw=%u supported_max=65535", level);
    if(level>65535){Metric("BACKLIGHT_REJECT raw=%u",level);return INVALID;}
    uint32_t maximum=0;
    if(ReadPanelValue("max_brightness",maximum)||!maximum)return FAIL;
    uint32_t native=(uint64_t(level)*maximum+32767)/65535;
    int fd=open("/sys/class/backlight/panel0-backlight/brightness",O_WRONLY|O_CLOEXEC);
    if(fd<0)return FAIL; char text[32]; int length=snprintf(text,sizeof(text),"%u\n",native);
    ssize_t written; do{written=write(fd,text,length);}while(written<0&&errno==EINTR); int saved=errno;close(fd);errno=saved;
    if(written!=length)return FAIL; uint32_t observed=0;
    if(ReadPanelValue("brightness",observed)||observed!=native)return FAIL;
    Metric("BACKLIGHT_APPLIED raw=%u native=%u max=%u verified=%u",level,native,maximum,observed);Ace5Log("backlight level=%u native=%u max=%u verified=%u",level,native,maximum,observed);return OK;
}
extern "C" int32_t GetDisplayVBlankPeriod(uint32_t id, uint64_t &period)
{ DEVICE_LOCK(id); period = Period(); return OK; }
extern "C" int32_t RegDisplayVBlankCallback(uint32_t id, VBlankCallback cb, void *data)
{
    DEVICE_LOCK(id); std::lock_guard<std::mutex> state(vsyncMutex);
    vsyncCallback = cb; vsyncData = data; vsyncPeriod = Period();
    if (!threadStarted) { quit = false; if (pthread_create(&vsyncThread, nullptr, VsyncLoop, nullptr)) return FAIL; threadStarted = true; }
    return OK;
}
extern "C" int32_t SetDisplayVsyncEnabled(uint32_t id, bool enabled)
{ DEVICE_LOCK(id); std::lock_guard<std::mutex> state(vsyncMutex); vsyncEnabled = enabled; return OK; }
extern "C" int32_t SetDisplayClientBuffer(uint32_t id, const BufferHandle &buffer, int32_t fence)
{ OwnedFence acquire(fence); DEVICE_LOCK(id); return Linear(buffer) ? client.Set(buffer, acquire) : UNSUPPORTED; }
extern "C" int32_t SetDisplayClientCrop(uint32_t id, const IRect &rect)
{
    DEVICE_LOCK(id);
    if (rect.x < 0 || rect.y < 0 || rect.w <= 0 || rect.h <= 0 ||
        int64_t(rect.x) + rect.w > 16384 || int64_t(rect.y) + rect.h > 16384) return INVALID;
    clientCrop = rect; return OK;
}
extern "C" int32_t SetDisplayClientDamage(uint32_t id, std::vector<IRect> &rects)
{ DEVICE_LOCK(id); clientDamage = rects; return OK; }
extern "C" int32_t Commit(uint32_t id, int32_t &fence)
{ DEVICE_LOCK(id); return CommitFrame(fence); }
// Explicit test-only entry point for the independent recovery contract test.
extern "C" int32_t Ace5TestClientBuffer(uint32_t id)
{ DEVICE_LOCK(id); return Submit(true); }
extern "C" int32_t CreateLayer(uint32_t id, const LayerInfo &info, uint32_t &layerId)
{
    DEVICE_LOCK(id); if (layers.size() >= 64 || !nextLayer || info.width <= 0 || info.height <= 0) return INVALID;
    layerId = nextLayer++; layers[layerId].info = info; return OK;
}
extern "C" int32_t DestroyLayer(uint32_t id, uint32_t layerId)
{
    DEVICE_LOCK(id);
    if (!layers.erase(layerId)) return INVALID;
    directRelease.ResetLayer(layerId);
    return OK;
}
extern "C" int32_t PrepareDisplayLayers(uint32_t id, bool &needFlushFb)
{ DEVICE_LOCK(id); PrepareSingleLayer(needFlushFb); return OK; }
extern "C" int32_t GetDisplayCompChange(uint32_t id, std::vector<uint32_t> &ids, std::vector<int32_t> &types)
{
    DEVICE_LOCK(id); ids.clear(); types.clear();
    for (const auto &entry : layers) {
        auto wanted = entry.first == preparedDirectLayer ? V::COMPOSITION_DEVICE : V::COMPOSITION_CLIENT;
        if (entry.second.composition != wanted) {
            ids.push_back(entry.first); types.push_back(wanted);
        }
    }
    return OK;
}
extern "C" int32_t GetDisplayReleaseFence(uint32_t id, std::vector<uint32_t> &ids, std::vector<int32_t> &fences)
{
    DEVICE_LOCK(id); ids.clear(); fences.clear();
    if (directReleaseUnavailable) return FAIL;
    uint32_t layerId = 0;
    int fd = directRelease.Borrow(layerId);
    // 逐层返回释放项，避免直扫缓存使客户端合成层漏归还
    for (const auto &entry : layers) {
        ids.push_back(entry.first);
        fences.push_back(fd >= 0 && entry.first == layerId ? fd : -1);
    }
    return OK;
}
extern "C" int32_t SetLayerBuffer(uint32_t id, uint32_t layerId, const BufferHandle &buffer, int32_t fence)
{ OwnedFence acquire(fence); LAYER_LOCK(id, layerId); return buffer.fd >= 0 ? layer.buffer.Set(buffer, acquire) : INVALID; }
extern "C" int32_t ClearLayerBuffer(uint32_t id, uint32_t layerId)
{ LAYER_LOCK(id, layerId); layer.buffer.Clear(); directRelease.ResetLayer(layerId); return OK; }
extern "C" int32_t ClearDisplayClientBuffer(uint32_t id)
{ DEVICE_LOCK(id); client.Clear(); return OK; }
#define SETTER(name, type, member) extern "C" int32_t name(uint32_t id, uint32_t layerId, type value) { LAYER_LOCK(id, layerId); layer.member = value; return OK; }
SETTER(SetLayerAlpha, const LayerAlpha &, alpha)
SETTER(SetLayerRegion, const IRect &, region)
SETTER(SetLayerCrop, const IRect &, crop)
SETTER(SetLayerZorder, uint32_t, zorder)
SETTER(SetLayerPreMulti, bool, premulti)
SETTER(SetLayerTransformMode, TransformType, transform)
SETTER(SetLayerDirtyRegion, const std::vector<IRect> &, dirty)
SETTER(SetLayerVisibleRegion, std::vector<IRect> &, visible)
SETTER(SetLayerCompositionType, V::CompositionType, composition)
SETTER(SetLayerBlendType, BlendType, blend)
SETTER(SetLayerMaskInfo, const MaskInfo, mask)
SETTER(SetLayerColor, const LayerColor &, color)
#undef SETTER
}

#ifdef ACE5_VSYNC_TEST
namespace OHOS::HDI::Display::Composer {
extern "C" void Ace5InitializeVsyncForTest() { std::lock_guard<std::mutex> lock(mutex); drm=open("/dev/null",O_RDWR); connector=1; mode.vrefresh=60; }

extern "C" void Ace5HoldComposerLockForTest(unsigned microseconds)
{ std::lock_guard<std::mutex> lock(mutex); usleep(microseconds); }
}
#endif

#ifdef ACE5_SUBMIT_TEST
namespace OHOS::HDI::Display::Composer {
extern "C" void Ace5InitializeModesForTest();
extern "C" void Ace5InitializeSubmitForTest()
{
    Ace5InitializeModesForTest();
    std::lock_guard<std::mutex> lock(mutex);
    plane = 90; secondPlane = 133;
    propertyIds[{connector, "CRTC_ID"}] = 102;
    propertyIds[{connector, "RETIRE_FENCE"}] = 999;
    propertyIds[{crtc, "ACTIVE"}] = 103;
    uint32_t property = 200;
    for (uint32_t target : {plane, secondPlane})
        for (const char *name : {"FB_ID", "CRTC_ID", "SRC_X", "SRC_Y", "SRC_W", "SRC_H",
            "CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H", "color_fill"})
            propertyIds[{target, name}] = property++;
    client.value.fd = open("/dev/null", O_RDWR);
    client.value.width = 1080; client.value.height = 2248;
    client.value.stride = 4352; client.value.size = 4352 * 2248;
    client.value.format = V::PIXEL_FMT_RGBA_8888;
    clientCrop = {0, 0, 1080, 2248};
}
extern "C" int32_t Ace5SubmitInternalForTest() { return Submit(false); }
}
#endif

#ifdef ACE5_MODE_TEST
namespace OHOS::HDI::Display::Composer {
extern "C" void Ace5InitializeModesForTest()
{
    std::lock_guard<std::mutex> lock(mutex);
    drm = open("/dev/null", O_RDWR); connector = 88; crtc = 176; modeBlob = 1;
    mode = {}; mode.hdisplay = 1080; mode.vdisplay = 2248; mode.vrefresh = 60;
    // 面板只上报 60Hz；第二个合成模式仅用于覆盖切换与回退路径
    displayModes = {mode}; mode.vrefresh = 30; displayModes.push_back(mode);
    mode = displayModes[0]; selectedMode = 0;
    propertyIds[{crtc, "MODE_ID"}] = 101;
}
}
#endif

#ifdef ACE5_DIRECT_TEST
namespace OHOS::HDI::Display::Composer {
// 测试入口只重置模拟状态，不读取运行策略文件
extern "C" void Ace5ResetDirectForTest(unsigned policy)
{
    std::lock_guard<std::mutex> lock(mutex);
    directRelease.Reset(); layers.clear();
    preparedDirectLayer = 0; requireClientFrame = false;
    directAppliedError = directReleaseUnavailable = planeRulesApplied = false;
    singleLayerCleanupError = 0;
    directFrames = directFailures = frameAtomicTotal = frameAtomicEnd = 0; submitMetrics = {};
    scanoutPolicy = policy <= 2 ? int(policy) : 0;
    scanoutPolicyOverride = scanoutPolicy; policyReadTime = 0;
    propertyIds[{crtc, "output_fence"}] = 998;
    for (unsigned half = 0; half < 2; ++half) {
        uint32_t target = half ? secondPlane : plane;
        planeBlend[half] = {target, true, true, 0, 255, 255};
        propertyIds[{target, "blend_op"}] = 300 + half * 2;
        propertyIds[{target, "alpha"}] = 301 + half * 2;
    }
}
// 测试实际文件控制逻辑，不更改当前覆盖值
extern "C" int Ace5ReadDisplayPolicyForTest()
{
    std::lock_guard<std::mutex> lock(mutex);
    int previous = scanoutPolicyOverride;
    scanoutPolicyOverride = -1;
    int result = ReadScanoutPolicy();
    scanoutPolicyOverride = previous;
    return result;
}
extern "C" void Ace5SetDirectPolicyForTest(unsigned policy)
{
    std::lock_guard<std::mutex> lock(mutex);
    scanoutPolicyOverride = policy <= 2 ? int(policy) : 0;
    policyReadTime = 0;
}
extern "C" int Ace5BorrowDirectReleaseForTest(uint32_t &layerId)
{
    std::lock_guard<std::mutex> lock(mutex);
    return directRelease.Borrow(layerId);
}
extern "C" void Ace5DirectStateForTest(uint32_t &prepared, uint32_t &fb,
    bool &rulesApplied, bool &clientRequired, int &cleanupError, bool &appliedError)
{
    std::lock_guard<std::mutex> lock(mutex);
    prepared = preparedDirectLayer; fb = currentFb;
    rulesApplied = planeRulesApplied; clientRequired = requireClientFrame;
    cleanupError = singleLayerCleanupError; appliedError = directAppliedError;
}
}
#endif
