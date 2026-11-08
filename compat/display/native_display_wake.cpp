// 通过原电源工具唤醒，并读取电源服务的亮屏状态
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static bool ReadBootId(char (&boot_id)[64])
{
    FILE *file = std::fopen("/proc/sys/kernel/random/boot_id", "re");
    if (!file) {
        return false;
    }
    bool valid = std::fgets(boot_id, sizeof(boot_id), file) &&
        std::strlen(boot_id) == 37 && boot_id[36] == '\n' &&
        std::fgetc(file) == EOF && !std::ferror(file);
    if (std::fclose(file) != 0 || !valid) {
        return false;
    }
    boot_id[36] = '\0';
    for (unsigned i = 0; i < 36; ++i) {
        const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
        if (separator ? boot_id[i] != '-' :
            !((boot_id[i] >= '0' && boot_id[i] <= '9') ||
              (boot_id[i] >= 'a' && boot_id[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

// 每次调用创建独立日志，避免复用旧唤醒结果
static bool OpenLog(const char *boot_id)
{
    char path[160];
    const int length = std::snprintf(path, sizeof(path),
        "/data/local/tmp/dipper-capture/powermgr-wake-%s-%d.txt", boot_id, getpid());
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(path)) {
        return false;
    }
    const int fd = open(path,
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    struct stat info {};
    const bool valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
        info.st_uid == 2000 && info.st_gid == 2000 &&
        (info.st_mode & 07777) == 0600 && info.st_nlink == 1;
    if (!valid || dup2(fd, STDOUT_FILENO) != STDOUT_FILENO) {
        close(fd);
        return false;
    }
    if (fd != STDOUT_FILENO && close(fd) != 0) {
        return false;
    }
    const int flags = fcntl(STDOUT_FILENO, F_GETFD);
    if (flags < 0 || fcntl(STDOUT_FILENO, F_SETFD, flags & ~FD_CLOEXEC) != 0) {
        return false;
    }
    std::setbuf(stdout, nullptr);
    return true;
}

// 子进程沿用原服务身份，父进程结束时一并终止
static bool RunOriginalWakeup()
{
    const pid_t parent = getpid();
    const pid_t child = fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent) {
            _exit(90);
        }
        alarm(6);
        char executable[] = "/system/bin/power-shell";
        char command[] = "wakeup";
        char *arguments[] = {executable, command, nullptr};
        execve(executable, arguments, environ);
        _exit(91);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    std::printf("POWERMGR_WAKE_TOOL pid=%d waited=%d status=%d\n",
        child, waited, status);
    return waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int Run()
{
    if (getuid() != 2000 || geteuid() != 2000 || getgid() != 2000) {
        return 2;
    }
    char boot_id[64] {};
    if (!ReadBootId(boot_id) || !OpenLog(boot_id)) {
        return 3;
    }
    alarm(15);
    std::printf("POWERMGR_WAKE_BOOT_ID=%s\n", boot_id);
    std::printf("POWERMGR_WAKE_ID uid=%d euid=%d gid=%d\n",
        getuid(), geteuid(), getgid());

    void *library = dlopen("/system/lib64/platformsdk/libpowermgr_client.z.so",
        RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        return 4;
    }
    auto get_instance = reinterpret_cast<void *(*)()>(dlsym(library,
        "_ZN4OHOS8PowerMgr14PowerMgrClient11GetInstanceEv"));
    auto get_state = reinterpret_cast<unsigned (*)(void *)>(dlsym(library,
        "_ZN4OHOS8PowerMgr14PowerMgrClient8GetStateEv"));
    auto is_screen_on = reinterpret_cast<bool (*)(void *, bool)>(dlsym(library,
        "_ZN4OHOS8PowerMgr14PowerMgrClient10IsScreenOnEb"));
    if (!get_instance || !get_state || !is_screen_on) {
        return 5;
    }
    void *manager = get_instance();
    if (!manager) {
        return 6;
    }
    const unsigned before_state = get_state(manager);
    const bool before_on = is_screen_on(manager, true);
    std::printf("POWERMGR_WAKE_BEFORE state_raw=%u screen_on=%d\n",
        before_state, before_on);
    if (!RunOriginalWakeup()) {
        return 7;
    }

    // 工具返回零不代表亮屏，成功标记取自原状态接口
    for (unsigned i = 0; i < 40; ++i) {
        const unsigned state = get_state(manager);
        const bool on = is_screen_on(manager, true);
        std::printf("POWERMGR_WAKE_READBACK index=%u state_raw=%u screen_on=%d\n",
            i, state, on);
        if (on) {
            std::puts("POWERMGR_LOGICAL_SCREEN_ON_VERIFIED=1");
            return 0;
        }
        usleep(50000);
    }
    return 8;
}

int main()
{
    const int result = Run();
    const bool flushed = std::fflush(stdout) == 0;
    std::_Exit(flushed ? result : 9);
}
