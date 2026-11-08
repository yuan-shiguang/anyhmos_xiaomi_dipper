/* Minimal AArch64 PID1 entry: all substantive code/assets live in vendor_boot.
 * No libc, mounts, services or userdata changes in this bootstrap. */
#define P "/dipper-compat-v118/"
static long call3(long number, long a, long b, long c)
{
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}
static long link_file(const char *target, const char *name)
{
    register long x8 __asm__("x8") = 37; /* linkat */
    register long x0 __asm__("x0") = -100;
    register long x1 __asm__("x1") = (long)target;
    register long x2 __asm__("x2") = -100;
    register long x3 __asm__("x3") = (long)name;
    register long x4 __asm__("x4") = 0;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory");
    return x0;
}
static const char *const aliases[][2] = {
    {P "busybox", "/busybox"},
    {P "drm-probe", "/drm-probe"},
    {P "graphics", "/graphics"},
    {P "stock-gpu-firmware", "/stock-gpu-firmware"},
    {P "hmos-ipc.ko", "/hmos-ipc.ko"},
    {P "hmos-init-debug.cfg", "/hmos-init-debug.cfg"},
    {P "hmos-hdf-graphics.hcb", "/hmos-hdf-graphics.hcb"},
    {P "hmos-native-hilog", "/hmos-native-hilog"},
    {P "init.android.original", "/init.android.original"}
};
__attribute__((noreturn)) void entry(void)
{
    int failed = call3(172, 0, 0, 0) != 1; /* getpid */
    for (unsigned i = 0; !failed && i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
        long result = (i == 2 || i == 3)
            ? call3(36, (long)aliases[i][0], -100, (long)aliases[i][1])
            : link_file(aliases[i][0], aliases[i][1]);
        if (result < 0) failed = 1; /* symlinkat: collisions fail closed */
    }
    char *const args[] = {P "hmos-probe", 0};
    char *const env[] = {0};
    if (!failed) call3(221, (long)args[0], (long)args, (long)env);
    const char error[] = "DIPPER_VENDOR_ENTRY_V118_FAILED: retain PID1 for recovery\n";
    call3(64, 2, (long)error, sizeof(error)-1);
    const long delay[] = {1, 0};
    for (;;) call3(101, (long)delay, 0, 0);
}
__asm__(".global _start\n_start:\nmov x29, xzr\nmov x30, xzr\nb entry\n");
