/* Read-only original TWRP Qualcomm Boot Control HAL probe.
 * Public signatures: AOSP hardware/libhardware android13-release boot_control.h.
 * Reuses the already staged original303 Bionic launcher, before its audio main.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <sys/stat.h>

struct boot_control_module;
static char slot[8], bootdevice[92];

static int read_boot_properties(void)
{
    FILE *f=fopen("/proc/bootconfig","re");
    if (!f) return -1;
    char line[512], value[92];
    while (fgets(line,sizeof(line),f)) {
        if (sscanf(line,"androidboot.slot_suffix = \"%91[^\"]\"",value)==1 &&
            (!strcmp(value,"_a") || !strcmp(value,"_b"))) strcpy(slot,value);
        if (sscanf(line,"androidboot.bootdevice = \"%91[^\"]\"",value)==1 &&
            strspn(value,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")==strlen(value))
            strcpy(bootdevice,value);
    }
    fclose(f);
    return *slot && *bootdevice ? 0 : -1;
}

int property_get(const char *key,char *value,const char *fallback)
{
    const char *source=NULL;
    if (key && !strcmp(key,"ro.boot.slot_suffix")) source=slot;
    if (key && !strcmp(key,"ro.boot.bootdevice")) source=bootdevice;
    if (source) {
        if (!*source) return 0;
        strcpy(value,source); return (int)strlen(source);
    }
    int (*real)(const char *,char *,const char *)=dlsym(RTLD_NEXT,"property_get");
    if (real) return real(key,value,fallback);
    const char *s=fallback?fallback:"";
    size_t n=strnlen(s,91);memcpy(value,s,n);value[n]=0;return (int)n;
}

static int run(int argc,char **argv,char **envp)
{
    (void)argc;(void)argv;(void)envp;
    setvbuf(stdout,NULL,_IOLBF,0);alarm(6);
    if (read_boot_properties()) { puts("BOOTCONTROL_REAL_BOOTCONFIG_MISSING"); return 10; }
    printf("BOOTCONTROL_KERNEL_PROPS slot=%s bootdevice=%s\n",slot,bootdevice);
    void *lib=dlopen("/compat/bootcontrol-v135/bootctrl.xiaomi_sm8550.so",RTLD_NOW|RTLD_LOCAL);
    if (!lib) { printf("BOOTCONTROL_DLOPEN_ERROR=%s\n",dlerror());return 11; }
    struct boot_control_module *module=dlsym(lib,"HMI");
    void (*init)(struct boot_control_module *)=dlsym(lib,"boot_control_init");
    unsigned (*number)(struct boot_control_module *)=dlsym(lib,"get_number_slots");
    unsigned (*current)(struct boot_control_module *)=dlsym(lib,"get_current_slot");
    int (*bootable)(struct boot_control_module *,unsigned)=dlsym(lib,"is_slot_bootable");
    int (*success)(struct boot_control_module *,unsigned)=dlsym(lib,"is_slot_marked_successful");
    uint32_t tag=0;if (module) memcpy(&tag,module,4);
    if (!module || tag!=0x48574d54 || !init || !number || !current || !bootable || !success) {
        puts("BOOTCONTROL_ORIGINAL_ABI_GUARD_FAILED");return 12;
    }
    init(module);
    unsigned count=number(module),active=current(module);
    printf("BOOTCONTROL_ORIGINAL_SLOTS count=%u current=%u\n",count,active);
    if (count!=2 || active>1 || active!=(unsigned)(slot[1]-'a')) return 13;
    for (unsigned i=0;i<count;i++)
        printf("BOOTCONTROL_ORIGINAL_STATUS slot=%u bootable=%d successful=%d\n",i,bootable(module,i),success(module,i));
#ifdef DIPPER_ALLOW_MARK_BOOT_SUCCESS
    const char *requested=getenv("DIPPER_MARK_SUCCESS_BOOT_ID");
    char boot_id[64]={0};
    FILE *f=fopen("/proc/sys/kernel/random/boot_id","re");
    if (!f || !fgets(boot_id,sizeof(boot_id),f)) { if (f) fclose(f); return 14; }
    fclose(f);boot_id[strcspn(boot_id,"\r\n")]=0;
    if (!requested || strlen(requested)!=36 || strcmp(requested,boot_id)) {
        puts("BOOTCONTROL_MARK_DENIED_BOOT_ID");return 15;
    }
    f=fopen("/hmos-root/data/local/tmp/v116-autostart.txt","re");
    if (!f) return 16;
    int boot_match=0,desktop=0,unlock=0,complete=0;
    char line[512];
    while (fgets(line,sizeof(line),f)) {
        if (!strncmp(line,boot_id,36) && line[36]=='\n') boot_match=1;
        if (!strcmp(line,"AUTOSTART_SERVICES_READY=1\n")) desktop=1;
        /* 解锁状态由辅助程序核实，兼容回调及已解锁路径 */
        if (strstr(line,"SCREENLOCK_VERIFIED after=0 ")) unlock=1;
        if (!strcmp(line,"AUTOSTART_EXIT code=0 stage=complete\n")) complete=1;
    }
    fclose(f);
    struct stat capture;
    if (!boot_match || !desktop || !unlock || !complete ||
        stat("/hmos-root/data/local/tmp/dipper-capture/display0.jpeg",&capture) ||
        !S_ISREG(capture.st_mode) || capture.st_size<65536) {
        puts("BOOTCONTROL_MARK_DENIED_DESKTOP_GATES");return 17;
    }
    int (*mark)(struct boot_control_module *)=dlsym(lib,"mark_boot_successful");
    if (!mark || bootable(module,active)!=1) return 18;
    int before=success(module,active);
    if (before<0 || before>1) return 19;
    if (before==1) { puts("BOOTCONTROL_ALREADY_SUCCESSFUL_NO_WRITE=1");return 0; }
    sync();
    int rc=mark(module),after=success(module,active);
    printf("BOOTCONTROL_ORIGINAL_MARK rc=%d slot=%u before=%d after=%d boot_id=%s\n",rc,active,before,after,boot_id);
    return rc || after!=1 ? 20 : 0;
#else
    puts("BOOTCONTROL_READ_ONLY_NO_MARK=1");
    return 0;
#endif
}

__attribute__((noreturn)) void __libc_init(void *raw_args,void (*onexit)(void),
    int (*original_main)(int,char **,char **),const void *structors)
{
    (void)original_main;
    void (*real)(void *,void (*)(void),int (*)(int,char **,char **),const void *)=dlsym(RTLD_NEXT,"__libc_init");
    if (real) real(raw_args,onexit,run,structors);
    _exit(90);
}
