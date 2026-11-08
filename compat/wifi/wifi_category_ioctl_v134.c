/* Translate only the donor's CMD_WIFI_CATEGORY ioctl into real nl80211 data.
 * The vendor files and original HDI handlers remain untouched.
 */
#define _GNU_SOURCE
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <net/if.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include "wifi_caps_v134.h"

struct DonorPrivCmd { unsigned char *buf; uint32_t size, len; };
_Static_assert(sizeof(struct DonorPrivCmd)==16,"donor private command ABI");
_Static_assert(sizeof(struct ifreq)==40,"donor ifreq ABI");
static ssize_t safe_copy(void *local, const void *remote, size_t size, int write_remote)
{
    struct iovec here={local,size},there={(void *)remote,size};
    return syscall(write_remote?SYS_process_vm_writev:SYS_process_vm_readv,
        getpid(),&here,1,&there,1,0);
}
static void log_caps(const char *ifname, const struct DipperWifiCaps *caps)
{
    static atomic_uint count=0;
    if (atomic_fetch_add(&count,1)>=24) return;
    int fd=open("/data/local/tmp/dipper-wifi-category-v134.txt",O_WRONLY|O_APPEND|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return;
    char line[192]; int n=snprintf(line,sizeof(line),
        "WIFI_CATEGORY_NL if=%s ifindex=%u wiphy=%u he=%u eht=%u records=%u mask=%u uid=%u\n",
        ifname,caps->ifindex,caps->wiphy,caps->he,caps->eht,caps->records,caps->mask,(unsigned)getuid());
    if (n>0 && (unsigned)n<sizeof(line)) (void)write(fd,line,(size_t)n);
    close(fd);
}
__attribute__((visibility("default"))) int ioctl(int fd, int request, ...)
{
    va_list ap; va_start(ap,request); void *arg=va_arg(ap,void *); va_end(ap);
    int saved_errno=errno;
    if (request==0x89f1 && arg) {
        struct ifreq ifr; struct DonorPrivCmd priv;
        const char command[]="CMD_WIFI_CATEGORY"; char actual[sizeof(command)];
        if (safe_copy(&ifr,arg,sizeof(ifr),0)==sizeof(ifr) &&
            memchr(ifr.ifr_name,0,IFNAMSIZ) && ifr.ifr_data &&
            safe_copy(&priv,ifr.ifr_data,sizeof(priv),0)==sizeof(priv) &&
            priv.buf && priv.size>=64 && priv.size<=4096 && priv.len>=sizeof(command) && priv.len<=priv.size &&
            safe_copy(actual,priv.buf,sizeof(actual),0)==sizeof(actual) && !memcmp(actual,command,sizeof(command))) {
            struct DipperWifiCaps caps;
            if (!DipperReadWifiCaps(ifr.ifr_name,&caps)) {
                unsigned char value=(unsigned char)caps.mask;
                if (safe_copy(&value,priv.buf,1,1)==1) {
                    log_caps(ifr.ifr_name,&caps); errno=saved_errno; return 0;
                }
            }
        }
    }
    return (int)syscall(SYS_ioctl,fd,request,arg);
}
