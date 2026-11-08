/* Bounded donor BMS client launched by native init with an explicit token.
 * No shell, credential changes, permission-check patches, or vendor writes. */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *const apps[] = {
    "ABCreadingHm", "CapCutHm", "DouyinHm", "IqiyiHm", "KuaiShouHm",
    "MeituxiuxiuHm", "MindmasterHm", "WPSHm", "XiaohongshuHm",
    "XiaoyuanAIHm", "XunleiHm", "ZhimuyunHm", "ZuoyebangHm", "ZwcadHm"
};

int main(void)
{
    alarm(240);
    FILE *f = fopen("/data/local/tmp/dipper-bundle-request.txt", "r");
    if (!f) return 2;
    char mode[24], name[160] = {0}, extra[2];
    int count = fscanf(f, "%23s %159s %1s", mode, name, extra);
    fclose(f);
    if (count < 1 || count > 2) return 3;
    char path[256];
    int system_core = !strcmp(mode, "install-system");
    if (!strcmp(mode, "install") && count == 2) {
        unsigned i;
        for (i = 0; i < sizeof(apps) / sizeof(apps[0]); ++i)
            if (!strcmp(name, apps[i])) break;
        if (i == sizeof(apps) / sizeof(apps[0])) return 4;
        snprintf(path, sizeof(path), "/preload/app/%s", name);
    } else if (system_core && count == 2) {
        if (strcmp(name, "ArkWebCore") && strcmp(name, "ArkWebCoreLegacy")) return 4;
        snprintf(path, sizeof(path), "/system/app/%s", name);
    } else if (!strcmp(mode, "dump") && count == 2) {
        for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
            if (!isalnum(*p) && *p != '.' && *p != '_') return 4;
    } else if (strcmp(mode, "list") || count != 1) return 4;
    if (!freopen("/data/local/tmp/dipper-bundle-last.log", "w", stdout)) return 5;
    dup2(fileno(stdout), STDERR_FILENO);
    setbuf(stdout, NULL);
    printf("NATIVE_BUNDLE_CLI uid=%d gid=%d mode=%s name=%s\n", getuid(), getgid(), mode, name);
    if (!strcmp(mode, "install") || system_core) {
        /* Donor preload directories also contain ag_config.json. Pass only
         * package files; install all modules of a bundle in one transaction. */
        DIR *dir = opendir(path);
        if (!dir) { perror("preload directory"); return 7; }
        char files[32][512];
        char *args[39] = {"bm", "install", "-p"};
        unsigned count = 0;
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            const char *suffix = strrchr(entry->d_name, '.');
            if (!suffix || (strcmp(suffix, ".hap") && strcmp(suffix, ".hsp"))) continue;
            if (count == 32 || snprintf(files[count], sizeof(files[count]), "%s/%s", path,
                entry->d_name) >= (int)sizeof(files[count])) { closedir(dir); return 8; }
            printf("BUNDLE_MODULE=%s\n", files[count]);
            args[3 + count] = files[count];
            ++count;
        }
        closedir(dir);
        if (!count) return 9;
        args[3 + count] = "-u";
        args[4 + count] = system_core ? "0" : "100";
        args[5 + count] = NULL;
        execv("/system/bin/bm", args);
    }
    else if (!strcmp(mode, "dump"))
        execl("/system/bin/bm", "bm", "dump", "-n", name, "-u", "100", (char *)0);
    else
        execl("/system/bin/bm", "bm", "dump", "-a", "-u", "100", (char *)0);
    perror("exec bm");
    return 6;
}
