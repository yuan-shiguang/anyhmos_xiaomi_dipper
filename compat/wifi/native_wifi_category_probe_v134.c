/* Bounded read-only validation: original HAL getter, own-wiphy query, ioctl pass-through. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include "wifi_caps_v134.h"

int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IOLBF,0); alarm(7);
    if (argc!=3) return 2;
    if (!strcmp(argv[1],"caps")) {
        struct DipperWifiCaps caps;
        int rc=DipperReadWifiCaps(argv[2],&caps);
        printf("WIFI_OWN_WIPHY rc=%d ifindex=%u wiphy=%u he=%u eht=%u records=%u mask=%u\n",
            rc,caps.ifindex,caps.wiphy,caps.he,caps.eht,caps.records,caps.mask);
        return rc?3:0;
    }
    if (!strcmp(argv[1],"hal")) {
        void *lib=dlopen("/vendor/lib64/libwifi_hal_hw.z.so",RTLD_NOW|RTLD_LOCAL);
        if (!lib) { printf("WIFI_ORIGINAL_HAL_LOAD_ERROR=%s\n",dlerror()); return 4; }
        int (*init)(void *)=dlsym(lib,"InitWifiVendorHalFuncTable");
        if (!init) return 5;
        union { void *slots[64]; unsigned char bytes[512]; } table;
        memset(&table,0,sizeof(table));
        int rc=init(&table); if (rc) { printf("WIFI_ORIGINAL_TABLE_RC=%d\n",rc); return 6; }
        void *getter=table.slots[12]; Dl_info loc;
        if (!getter || !dladdr(getter,&loc) || (uintptr_t)getter-(uintptr_t)loc.dli_fbase!=0x10e88) {
            puts("WIFI_ORIGINAL_TABLE_LAYOUT_GUARD_FAILED=1"); return 7;
        }
        const unsigned char guard[]={0x4f,0,0,0xb0,0xef,0x59,0x47,0xf9,0xef,0x01,0x1d,0xca,0xef,0x01,0x1e,0xca};
        if (memcmp(getter,guard,sizeof(guard))) { puts("WIFI_ORIGINAL_GETTER_GUARD_FAILED=1"); return 8; }
        uint32_t (*get_caps)(const char *)=(uint32_t (*)(const char *))getter;
        uint32_t caps=get_caps(argv[2]);
        printf("WIFI_ORIGINAL_HAL_GET_CHIP_CAPS iface=%s caps=%u\n",argv[2],caps);
        return 0;
    }
    if (!strcmp(argv[1],"passthrough")) {
        int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0); if (fd<0) return 9;
        struct ifreq a={0},b={0};
        if (strlen(argv[2])>=IFNAMSIZ) return 10;
        strcpy(a.ifr_name,argv[2]); strcpy(b.ifr_name,argv[2]);
        errno=0; int ra=ioctl(fd,SIOCGIFFLAGS,&a),ea=errno;
        errno=0; int rb=(int)syscall(SYS_ioctl,fd,SIOCGIFFLAGS,&b),eb=errno;
        printf("WIFI_PASSTHROUGH_FLAGS same=%d rc=%d/%d errno=%d/%d\n",
            ra==rb && ea==eb && !memcmp(&a,&b,sizeof(a)),ra,rb,ea,eb);
        if (ra!=rb || ea!=eb || memcmp(&a,&b,sizeof(a))) return 11;
        errno=0;ra=ioctl(fd,0x89f1,(void *)1);ea=errno;
        errno=0;rb=(int)syscall(SYS_ioctl,fd,0x89f1,(void *)1);eb=errno;
        printf("WIFI_PASSTHROUGH_INVALID same=%d rc=%d/%d errno=%d/%d\n",ra==rb && ea==eb,ra,rb,ea,eb);
        close(fd);return ra==rb && ea==eb?0:12;
    }
    return 13;
}
