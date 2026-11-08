/* Development adapter for explicitly listed, absent SceneBoard dependencies.
 * The donor performs the real CheckSystemAbility IPC. Only a null result for
 * an allowlisted optional SA terminates its enclosing synchronous retry loop.
 * Existing objects and every other process/SA retain their original behavior.
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ABI carrier, not an owning replacement for OHOS::sptr. Both have one pointer
// and a nontrivial destructor, hence the AArch64 indirect-result x8 convention.
// The donor constructs/acquires the result; the original caller destroys it.
// Local carriers never acquire/release another reference, even without NRVO.
struct RemoteRefCarrier {
    void *pointer = nullptr;
    ~RemoteRefCarrier() {}
};
static bool listed_scene_board(int id)
{
    char cmd[80] = {};
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = read(fd, cmd, sizeof(cmd) - 1);
    close(fd);
    if (n <= 0 || strcmp(cmd, "com.ohos.sceneboard")) return false;
    char list[1024] = {};
    fd = open("/compat/etc/sceneboard-optional-sa.list", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    n = read(fd, list, sizeof(list) - 1);
    close(fd);
    if (n <= 0 || n == ssize_t(sizeof(list) - 1)) return false;
    char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
        if (!*p) break;
        char *end = nullptr;
        long candidate = strtol(p, &end, 10);
        if (end == p || (*end && *end != ' ' && *end != '\n' && *end != '\t' && *end != '\r')) return false;
        if (candidate == id) return true;
        p = end;
    }
    return false;
}
using NextCheck = RemoteRefCarrier (*)(void *, int, bool *, int *);
static void *next_check;
extern "C" RemoteRefCarrier dipper_optional_check(void *, int, bool *, int *)
    __asm__("_ZN4OHOS25SystemAbilityManagerProxy18CheckSystemAbilityEiRbRi");
extern "C" RemoteRefCarrier dipper_optional_check(void *self, int id, bool *exists, int *error)
{
    void *symbol = __atomic_load_n(&next_check, __ATOMIC_ACQUIRE);
    if (!symbol) {
        symbol = dlsym(RTLD_NEXT, "_ZN4OHOS25SystemAbilityManagerProxy18CheckSystemAbilityEiRbRi");
        if (symbol) __atomic_store_n(&next_check, symbol, __ATOMIC_RELEASE);
    }
    if (!symbol) {
        if (exists) *exists = false;
        if (error) *error = 1;
        return {};
    }
    RemoteRefCarrier result = reinterpret_cast<NextCheck>(symbol)(self, id, exists, error);
    if (!result.pointer && error && listed_scene_board(id)) {
        // The donor wrapper's error==1 branch exits with a null object; no
        // connected/enabled state or successful operation is fabricated.
        *error = 1;
        auto print = reinterpret_cast<int (*)(int,int,unsigned,const char *,const char *,...)>(dlsym(RTLD_DEFAULT, "HiLogPrint"));
        if (print) print(3, 5, 0xD001810, "DIPPER_OPTIONAL_SA", "absent optional SA %{public}d: stop SceneBoard retry", id);
    }
    return result;
}
