#ifdef DIPPER_POLICY_HOST
using size_t = __SIZE_TYPE__;
using ssize_t = __PTRDIFF_TYPE__;
constexpr int O_RDONLY = 0, O_CLOEXEC = 1, O_NOFOLLOW = 2;
constexpr int ENOENT = 2, EACCES = 13, EIO = 5, EINTR = 4;
int errno;
extern "C" void *memset(void *target, int value, size_t size)
{ auto *p = static_cast<unsigned char *>(target); while (size--) *p++ = value; return target; }
extern "C" void *memcpy(void *target, const void *source, size_t size)
{ auto *p = static_cast<unsigned char *>(target); auto *q = static_cast<const unsigned char *>(source); while (size--) *p++ = *q++; return target; }
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#endif

#ifndef DIPPER_DEFAULT_DISPLAY_ROUTE
#define DIPPER_DEFAULT_DISPLAY_ROUTE 0
#endif

namespace {
int openError = 0, readError = 0;
int opens = 0, reads = 0, closes = 0, unexpected = 0;
const char *contents = "2";

bool Equal(const char *a, const char *b)
{ while (*a && *a == *b) { ++a; ++b; } return *a == *b; }

void Reset()
{
    opens = reads = closes = unexpected = 0;
    openError = readError = 0;
    contents = "2";
}
}

// 仅模拟固定控制文件，拒绝其他路径的打开请求
extern "C" int __wrap_open(const char *path, int flags, ...)
{
    ++opens;
    if (!Equal(path, "/dev/dipper-display-route-mode") ||
        flags != (O_RDONLY | O_CLOEXEC | O_NOFOLLOW)) {
        ++unexpected; errno = EACCES; return -1;
    }
    if (openError) { errno = openError; return -1; }
    return 0;
}
extern "C" ssize_t __wrap_read(int fd, void *out, size_t length)
{
    ++reads;
    if (fd || length != 1) { ++unexpected; errno = EIO; return -1; }
    if (readError) { errno = readError; return -1; }
    if (!contents[0]) return 0;
    *static_cast<char *>(out) = contents[0]; return 1;
}
extern "C" int __wrap_close(int fd)
{
    ++closes;
    if (fd) { ++unexpected; errno = EIO; return -1; }
    return 0;
}

#ifdef DIPPER_POLICY_HOST
#define open __wrap_open
#define read __wrap_read
#define close __wrap_close
// 提取实际读取函数，宿主与原生契约使用相同用例
#include "policy_under_test.inc"
#undef open
#undef read
#undef close
extern "C" int Ace5ReadDisplayPolicyForTest() { return ReadScanoutPolicy(); }
#else
extern "C" int Ace5ReadDisplayPolicyForTest();
#endif

#define CHECK(test) do { if (!(test)) return __LINE__; } while (0)
int RunCase(unsigned number)
{
    Reset();
    int expected = 0;
    constexpr const char *routes[] = {"0", "1", "2"};
    if (number <= 2) { contents = routes[number]; expected = number; }
    else if (number == 3) { openError = ENOENT; expected = DIPPER_DEFAULT_DISPLAY_ROUTE; }
    else if (number == 4) openError = EACCES;
    else if (number == 5) contents = "3";
    else if (number == 6) contents = "\n";
    else if (number == 7) contents = "";
    else if (number == 8) readError = EIO;
    else if (number == 9) readError = EINTR;
    else if (number == 10) { contents = "2\n"; expected = 2; }
    else if (number == 11) { contents = "1 ignored"; expected = 1; }
    else return -1;
    CHECK(Ace5ReadDisplayPolicyForTest() == expected);
    CHECK(!unexpected && opens == 1);
    if (openError) CHECK(!reads && !closes);
    else CHECK(reads == 1 && closes == 1);
    return 0;
}

#ifdef DIPPER_POLICY_HOST
extern "C" __declspec(dllimport) __declspec(noreturn) void ExitProcess(unsigned);
extern "C" __declspec(dllimport) char *GetCommandLineA();
extern "C" void HostMain()
{
    const char *argument = GetCommandLineA();
    for (const char *p = argument; *p; ++p) if (*p == ' ') argument = p + 1;
    unsigned number = 0;
    for (; *argument; ++argument) {
        if (*argument < '0' || *argument > '9') ExitProcess(254);
        number = number * 10 + *argument - '0';
    }
    ExitProcess(RunCase(number));
}
#else
int main()
{
    for (unsigned number = 0; number < 12; ++number) {
        int result = RunCase(number);
        if (result) {
            printf("ROUTE_POLICY_FAIL default=%d case=%u line=%d\n", DIPPER_DEFAULT_DISPLAY_ROUTE, number, result);
            return 1;
        }
    }
    printf("ROUTE_POLICY_OK default=%d cases=12 real_reader_mocked_files=1\n", DIPPER_DEFAULT_DISPLAY_ROUTE);
    return 0;
}
#endif
