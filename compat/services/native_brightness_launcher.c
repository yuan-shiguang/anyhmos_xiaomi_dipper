/* Keep the donor init identity and make early startup failures observable. */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

/* 诊断日志不可用时沿用原标准输出 */
static void redirect_log(void)
{
    int fd = open("/data/local/tmp/dipper-brightness-stderr.txt",
        O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    /* 避免日志描述符占用标准流编号 */
    if (fd <= STDERR_FILENO) {
        int moved = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        close(fd);
        if (moved < 0) return;
        fd = moved;
    }
    int flags = fcntl(STDOUT_FILENO, F_GETFD);
    if (flags < 0) {
        close(fd);
        return;
    }
    int saved = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (saved < 0) {
        close(fd);
        return;
    }
    if (dup2(fd, STDOUT_FILENO) >= 0) {
        /* 第二次重定向失败时恢复原标准输出及描述符标志 */
        if (dup2(fd, STDERR_FILENO) < 0 && dup2(saved, STDOUT_FILENO) >= 0)
            fcntl(STDOUT_FILENO, F_SETFD, flags);
    }
    close(saved);
    close(fd);
}

int main(void)
{
    redirect_log();
    dprintf(2, "BRIGHTNESS_NATIVE_LAUNCH uid=%d gid=%d\n", getuid(), getgid());
    /* sa_main derives the registered process name from this basename. */
    execl("/system/bin/sa_main", "sa_main", "/compat/etc/powermgr.json", (char *)0);
    perror("brightness sa_main exec");
    return 4;
}
