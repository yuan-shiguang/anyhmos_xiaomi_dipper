#include "composer_exports.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <xf86drmMode.h>

using namespace OHOS::HDI::Display::Composer;
extern "C" void Ace5InitializeModesForTest();
namespace {
int testFailure = 0, commitFailure = 0, tests = 0, commits = 0, destroyed = 0, callbacks = 0;
uint32_t nextBlob = 2;
void Check(bool passed, const char *message)
{
    if (!passed) { fprintf(stderr, "MODE_CONTRACT_FAIL %s\n", message); exit(1); }
}
void State(uint32_t expected, uint64_t period)
{
    uint32_t id = UINT32_MAX; uint64_t actual = 0;
    Check(GetDisplayMode(0, id) == 0 && id == expected, "mode retained");
    Check(GetDisplayVBlankPeriod(0, actual) == 0 && actual == period, "period retained");
}
void Callback(uint32_t id, uint64_t period, void *)
{
    // 回调内再次查询状态，检查调用方已经释放显示锁
    State(id, period); ++callbacks;
}
}
// 模拟内核提交结果，验证真实状态转换及失败回退
extern "C" int __wrap_drmModeCreatePropertyBlob(int, const void *, size_t, uint32_t *id)
{ *id = nextBlob++; return 0; }
extern "C" int __wrap_drmModeDestroyPropertyBlob(int, uint32_t)
{ ++destroyed; return 0; }
extern "C" int __wrap_drmModeAtomicCommit(int, drmModeAtomicReq *request, uint32_t flags, void *)
{
    Check(drmModeAtomicGetCursor(request) == 1, "only mode property changed");
    Check(flags & DRM_MODE_ATOMIC_ALLOW_MODESET, "modeset permission");
    if (flags & DRM_MODE_ATOMIC_TEST_ONLY) {
        ++tests; errno = EINVAL; return testFailure;
    }
    ++commits; errno = EIO; return commitFailure;
}
int main()
{
    Ace5InitializeModesForTest();
    std::vector<DisplayModeInfo> modes;
    std::vector<DisplayModeInfoExt> extended;
    Check(GetDisplaySupportedModes(0, modes) == 0 && modes.size() == 2, "mode enumeration");
    Check(modes[0].id == 0 && modes[0].freshRate == 60 && modes[1].id == 1 &&
        modes[1].freshRate == 30, "mode identity");
    Check(GetDisplaySupportedModesExt(0, extended) == 0 && extended.size() == 2 &&
        extended[1].v1_0.id == 1 && extended[0].groupId != extended[1].groupId, "non-seamless groups");
    Check(SetDisplayMode(0, 99) == -3 && tests == 0, "invalid mode rejected");
    State(0, 16666666);
    testFailure = -1;
    Check(SetDisplayMode(0, 1) != 0 && tests == 1 && commits == 0 && destroyed == 1,
        "failed test does not commit");
    State(0, 16666666);
    testFailure = 0; commitFailure = -1;
    Check(SetDisplayMode(0, 1) != 0 && tests == 2 && commits == 1 && destroyed == 2,
        "failed commit releases candidate");
    State(0, 16666666);
    commitFailure = 0;
    Check(SetDisplayMode(0, 1) == 0 && tests == 3 && commits == 2 && destroyed == 3,
        "successful commit releases previous blob");
    State(1, 33333333);
    Check(SetDisplayMode(0, 1) == 0 && tests == 3 && commits == 2, "same mode is unchanged");
    Check(SetDisplayModeAsync(0, 0, Callback, nullptr) == 0 && callbacks == 1, "callback completed");
    State(0, 16666666);
    testFailure = -1;
    Check(SetDisplayModeAsync(0, 1, Callback, nullptr) != 0 && callbacks == 1,
        "failed switch does not report completion");
    Check(SetDisplayModeAsync(0, 0, nullptr, nullptr) == -3, "missing callback rejected");
    puts("MODE_CONTRACT_OK enumeration invalid test_failure commit_failure 60Hz callback rollback");
    return 0;
}
