// Retain the init-assigned native identity while preserving Vulkan stderr.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(void)
{
    const char *path = "/data/local/tmp/dipper-render-stderr.txt";
    int fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 2;
    if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) return 3;
    close(fd);
    setenv("MESA_LOG_FILE", "/data/local/tmp/dipper-mesa.txt", 1);
    setenv("MESA_LOG_LEVEL", "debug", 1);
    dprintf(STDERR_FILENO, "RENDER_NATIVE_LAUNCH uid=%d gid=%d\n", getuid(), getgid());
    execl("/system/bin/render_service", "render_service", (char *)0);
    perror("render exec");
    return 4;
}
