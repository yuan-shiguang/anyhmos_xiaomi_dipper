/* 限时执行原生输入命令，--ability 选择原版 aa
 * 两种模式均直接执行目标程序，保留原有凭据及访问令牌 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    alarm(10);
    FILE *f = fopen("/data/local/tmp/dipper-input.args", "r");
    if (!f) {
        return 2;
    }
    char values[20][96];
    char *args[22] = {"/compat/bin/uinput-original"};
    unsigned count = 0;
    while (count < 20 && fscanf(f, "%95s", values[count]) == 1) {
        for (const unsigned char *p = (unsigned char *)values[count]; *p; ++p) {
            if (!isalnum(*p) && *p != '-' && *p != '.') {
                fclose(f);
                return 3;
            }
        }
        args[count + 1] = values[count];
        ++count;
    }
    int extra = fgetc(f);
    fclose(f);
    if (count < 2 || (extra != EOF && !isspace((unsigned char)extra))) {
        return 4;
    }
    args[count + 1] = NULL;
    const int ability = strcmp(values[0], "--ability") == 0;
    if (ability) {
        if (strcmp(values[1], "start") && strcmp(values[1], "force-stop") &&
            strcmp(values[1], "help")) {
            return 7;
        }
        args[1] = "/system/bin/aa";
    }
    if (!freopen("/data/local/tmp/dipper-input-last.log", "w", stdout)) {
        return 5;
    }
    dup2(fileno(stdout), STDERR_FILENO);
    setbuf(stdout, NULL);
    printf("NATIVE_DEBUG uid=%d mode=%s argc=%u\n",
        getuid(), ability ? "ability" : "input", count);
    if (ability) {
        execv(args[1], args + 1);
    } else {
        execv(args[0], args);
    }
    perror("execv");
    return 6;
}
