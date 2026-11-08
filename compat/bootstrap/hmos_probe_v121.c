/* SPDX-License-Identifier: MIT
 * dipper HMOS autonomous compatibility bootstrap with diagnostic collector.
 * System changes are RAM overlays; framework data uses the ext4 userdata partition.
 * dipper has no super partition: system/vendor are static read-only partitions, and
 * the compatibility payload plus sys_prod/preload live on the cust partition.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <termios.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <elf.h>
#ifdef __aarch64__
#include <asm/ptrace.h>
#endif
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/dm-ioctl.h>
#include <linux/watchdog.h>
#include <linux/reboot.h>

#ifndef DIPPER_IPC_ONEWAY_PID
#define DIPPER_IPC_ONEWAY_PID 0
#endif
_Static_assert(DIPPER_IPC_ONEWAY_PID==0||DIPPER_IPC_ONEWAY_PID==1,
               "DIPPER_IPC_ONEWAY_PID must be 0 or 1");
#if DIPPER_IPC_ONEWAY_PID
static int ipc_pair_loaded;
#endif

#define CAP (2*1024*1024)
#define DISK_CAP (16*1024*1024)
static char history[CAP];
static size_t used, serial_pos, disk_bytes, history_head;
static int logfd=-1,kmsg=-1,serialfd=-1,watchfd=-1,consolefd=-1;
static char logpath[256], suffix[3]="_a";
static int slot_known,trace_requested,ipc_requested,ipc_stage=-1,ipc_resultfd=-1;
static pid_t ipc_pid;
static pid_t ipc_test_pid;
static int ipc_test_requested;
static time_t ipc_test_deadline;
static pid_t traced_pid;
static int trace_held,trace_enter=1,trace_allow_exit,trace_result;
static long trace_last_nr;
static int trace_pending_kmsg,trace_kmsg_fd=-1;
static int trace_hmos,trace_pending_enforce,trace_enforce_fd=-1;
static int hilogfd=-1;
static int trace_syscalls=1;
#ifdef __aarch64__
static int trace_msync_count;
#endif
static void allwrite(int fd,const void *buf,size_t n) {
    const char *p=buf;
    while(n) { ssize_t z=write(fd,p,n); if(z<0&&errno==EINTR)continue; if(z<=0)break; p+=z;n-=z; }
}
// 按逻辑偏移读取环形历史，保留回放与部分写入顺序
static size_t history_span(size_t offset,const char **data) {
    if(offset>=used){*data=NULL;return 0;}
    size_t start=(history_head+offset)%CAP,n=used-offset;
    if(n>CAP-start)n=CAP-start;
    *data=history+start;return n;
}
static void history_write(int fd) {
    size_t offset=0;
    while(offset<used){
        const char *data;size_t n=history_span(offset,&data);
        ssize_t z=write(fd,data,n);
        if(z<0&&errno==EINTR)continue;
        if(z<=0)return;
        offset+=(size_t)z;
    }
}
static void emit(const char *b,size_t n) {
    if(n>CAP)n=CAP;
    // 满缓冲只推进起点，避免每条日志搬移整段历史
    if(used+n>CAP) {
        size_t drop=used+n-CAP;history_head=(history_head+drop)%CAP;
        used-=drop;serial_pos=serial_pos>drop?serial_pos-drop:0;
    }
    size_t tail=(history_head+used)%CAP,first=n;
    if(first>CAP-tail)first=CAP-tail;
    memcpy(history+tail,b,first);memcpy(history,b+first,n-first);used+=n;
    if(consolefd>=0)allwrite(consolefd,b,n);
    if(logfd>=0&&disk_bytes<DISK_CAP) {size_t z=n;if(z>DISK_CAP-disk_bytes)z=DISK_CAP-disk_bytes;allwrite(logfd,b,z);disk_bytes+=z;}
}
static void note(const char *fmt,...) {
    char b[2048];struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
    int z=snprintf(b,sizeof(b),"[HMOS-PROBE %lld.%03ld] ",(long long)ts.tv_sec,ts.tv_nsec/1000000);
    va_list ap;va_start(ap,fmt);vsnprintf(b+z,sizeof(b)-(size_t)z,fmt,ap);va_end(ap);
    size_t n=strlen(b);if(n<sizeof(b)-1)b[n++]='\n';emit(b,n);
}
static ssize_t readfile(const char *p,char *b,size_t n) {
    int fd=open(p,O_RDONLY|O_CLOEXEC);if(fd<0)return -1;
    ssize_t z=read(fd,b,n-1);close(fd);if(z>=0)b[z]=0;return z;
}
static int put(const char *p,const char *v) {
    int fd=open(p,O_WRONLY|O_CLOEXEC);if(fd<0)return -1;
    ssize_t n=write(fd,v,strlen(v));int e=errno;close(fd);errno=e;return n==(ssize_t)strlen(v)?0:-1;
}
static void dump(const char *p) {char b[16384];ssize_t n=readfile(p,b,sizeof(b));note("FILE %s %s",p,n<0?strerror(errno):"");if(n>0)emit(b,(size_t)n);emit("\n",1);}
static void makedir(const char *p) {if(mkdir(p,0755)<0&&errno!=EEXIST)note("mkdir %s: %s",p,strerror(errno));}
static void drain(void) {
    char b[8192];if(kmsg<0)return;
    for(int i=0;i<32;i++) {ssize_t n=read(kmsg,b,sizeof(b));if(n>0)emit(b,(size_t)n);else if(n<0&&errno==EPIPE)continue;else break;}
}
static void hilog_tick(void) {
    if(!trace_hmos)return;
    /* v22 delegates these sockets to the original hilogd. */
    if(access("/hmos-native-hilog",F_OK)==0)return;
    if(hilogfd<0){
        struct sockaddr_un addr={.sun_family=AF_UNIX};
        snprintf(addr.sun_path,sizeof(addr.sun_path),"/hmos-root/dev/unix/socket/hilogInput");
        if(access("/hmos-root/dev/unix/socket",F_OK)<0)return;
        int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(fd<0)return;
        int size=1024*1024;setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&size,sizeof(size));
        if(bind(fd,(struct sockaddr*)&addr,sizeof(addr))<0){close(fd);return;}
        chmod(addr.sun_path,0666);hilogfd=fd;note("HILOG datagram capture ready (debug collector, not hilogd)");
    }
    for(int i=0;i<64;i++){
        unsigned char raw[8192];ssize_t n=recv(hilogfd,raw,sizeof(raw),MSG_DONTWAIT);if(n<=0)break;
        char header[145],body[8193];size_t h=0;
        for(ssize_t j=0;j<n&&j<48;j++)h+=(size_t)snprintf(header+h,sizeof(header)-h,"%02x",raw[j]);
        /* Header layout is still proprietary; retain its hex separately.
         * Find the printable tag+message after its fixed metadata. */
        ssize_t start=0;for(ssize_t j=24;j+5<n&&j<48;j++)if(raw[j]=='I'&&!memcmp(raw+j,"Init\0",5)){start=j;break;}else if(raw[j]=='P'&&!memcmp(raw+j,"PARAM\0",6)){start=j;break;}
        for(ssize_t j=start;j<n;j++)body[j-start]=(raw[j]>=32||raw[j]=='\n'||raw[j]=='\t')?(char)raw[j]:'|';
        body[n-start]=0;note("HILOG packet bytes=%ld header=%s",(long)n,header);emit(body,(size_t)(n-start));emit("\n",1);
    }
}
static int node_from_sys(const char *sys,const char *dst,mode_t type) {
    char p[512],b[128];unsigned a,c;snprintf(p,sizeof(p),"%s/dev",sys);
    if(readfile(p,b,sizeof(b))<0||sscanf(b,"%u:%u",&a,&c)!=2)return -1;
    if(mknod(dst,type|0600,makedev(a,c))<0&&errno!=EEXIST)return -1;
    return 0;
}
static void discover(void) {
    DIR *d=opendir("/sys/class/block");if(!d)return;struct dirent *de;
    while((de=readdir(d))) {if(de->d_name[0]=='.')continue;
        char sys[512],dst[512],path[1024],buf[8192];snprintf(sys,sizeof(sys),"/sys/class/block/%s",de->d_name);
        snprintf(dst,sizeof(dst),"/dev/block/%s",de->d_name);node_from_sys(sys,dst,S_IFBLK);
        snprintf(path,sizeof(path),"%s/uevent",sys);if(readfile(path,buf,sizeof(buf))<0)continue;
        char *p=strstr(buf,"PARTNAME=");if(!p)continue;p+=9;char *end=strchr(p,'\n');if(end)*end=0;
        if(!*p||strchr(p,'/')||strlen(p)>255)continue;
        snprintf(path,sizeof(path),"/dev/block/by-name/%s",p);
        if(access(path,F_OK)&&symlink(dst,path)<0&&errno!=EEXIST)note("by-name %s: %s",p,strerror(errno));
    }closedir(d);
}
static void persistent(void) {
    if(logfd>=0)return;
    const char *dir=NULL;
    if(access("/dev/block/by-name/metadata",F_OK)==0){
        const char *fs[]={"f2fs","mifs","ext4"};
        for(unsigned i=0;i<sizeof(fs)/sizeof(fs[0]);i++) {
            if(mount("/dev/block/by-name/metadata","/metadata",fs[i],MS_NOSUID|MS_NODEV|MS_NOATIME,"")==0||errno==EBUSY) {note("metadata mounted as %s (no format/fsck)",fs[i]);dir="/metadata/hmos-debug";break;}
        }
    }
    /* 米 8 没有 metadata 分区：cust 是唯一常驻可写的静态分区，日志放这里 */
    if(!dir&&access("/cust",F_OK)==0){dir="/cust/hmos-debug";note("no metadata partition; persistent log on cust");}
    if(!dir)return;
    if(mkdir(dir,0700)<0&&errno!=EEXIST)return;
    char id[128]="unknown";readfile("/proc/sys/kernel/random/boot_id",id,sizeof(id));id[strcspn(id,"\r\n")]=0;
    snprintf(logpath,sizeof(logpath),"%s/probe-%s.log",dir,id);
    logfd=open(logpath,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_DSYNC|O_NOFOLLOW,0600);
    if(logfd<0)return;
    history_write(logfd);disk_bytes=used;note("PERSISTENT_LOG=%s",logpath);fsync(logfd);
}
static int userdata_is_mounted(const char *device,const char *target) {
    /* Accept any normally formatted ext4 userdata, including a fresh UUID.
     * Never mistake the early RAM placeholder for persistent device storage. */
    struct stat partition,data;
    struct statfs fs;
    return stat(device,&partition)==0&&S_ISBLK(partition.st_mode)&&
       stat(target,&data)==0&&data.st_dev==partition.st_rdev&&
       statfs(target,&fs)==0&&fs.f_type==0xef53;
}
static void runtime_persistent(void) {
    if(logfd>=0||traced_pid<=0||
       !userdata_is_mounted("/dev/block/by-name/userdata","/hmos-root/data"))return;
    char id[64];if(readfile("/proc/sys/kernel/random/boot_id",id,sizeof(id))<=0)return;
    id[strcspn(id,"\r\n")]=0;
    snprintf(logpath,sizeof(logpath),"/hmos-root/data/local/tmp/probe-%s.log",id);
    logfd=open(logpath,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);
    if(logfd<0)return;
    history_write(logfd);disk_bytes=used;
    note("PERSISTENT_RUNTIME_LOG=%s",logpath);fsync(logfd);
}
static void watchdog(void) {
    if(watchfd<0) {
        node_from_sys("/sys/class/watchdog/watchdog0","/dev/watchdog0",S_IFCHR);
        watchfd=open("/dev/watchdog0",O_WRONLY|O_CLOEXEC);
        if(watchfd>=0) {int t=120;ioctl(watchfd,WDIOC_SETTIMEOUT,&t);note("watchdog opened timeout=%d",t);}
    }
    if(watchfd>=0)ioctl(watchfd,WDIOC_KEEPALIVE,0);
}

struct mod {char name[128],deps[4096],pre[1024],post[1024];int state;};
static struct mod mods[1024];static int nmods;
static const char *basename_c(const char *p){const char *q=strrchr(p,'/');return q?q+1:p;}
static int same_module(const char *a,const char *b) {
    size_t an=strlen(a),bn=strlen(b);
    if(an>3&&!strcmp(a+an-3,".ko"))an-=3;
    if(bn>3&&!strcmp(b+bn-3,".ko"))bn-=3;
    if(an!=bn)return 0;
    for(size_t i=0;i<an;i++)if((a[i]=='_'?'-':a[i])!=(b[i]=='_'?'-':b[i]))return 0;
    return 1;
}
static void loadmod(const char *name,int depth);
static void module_parameters(const char *module,char *output,size_t capacity) {
    output[0]=0;char cmdline[32768];if(readfile("/proc/cmdline",cmdline,sizeof(cmdline))<0)return;
    char *save,*word=strtok_r(cmdline," \t\r\n",&save);
    while(word){char *dot=strchr(word,'.');if(dot&&strchr(dot,'=')){*dot=0;if(same_module(module,word)){const char *parameter=dot+1;size_t used=strlen(output),length=strlen(parameter);if(used+length+2<capacity){if(used)strcat(output," ");strcat(output,parameter);}}}word=strtok_r(NULL," \t\r\n",&save);}
}
static void load_words(const char *words,int depth) {
    char dep[4096];snprintf(dep,sizeof(dep),"%s",words);char *save,*p=strtok_r(dep," \r\n\t",&save);
    while(p){loadmod(p,depth+1);p=strtok_r(NULL," \r\n\t",&save);}
}
static void loadmod(const char *name,int depth) {
    if(depth>64)return;
    const char *base=basename_c(name);int idx=-1;
    for(int i=0;i<nmods;i++)if(same_module(mods[i].name,base)){idx=i;break;}
    if(idx>=0) {
        if(mods[idx].state)return;
        mods[idx].state=1;
        load_words(mods[idx].pre,depth);
        load_words(mods[idx].deps,depth);
        base=mods[idx].name;
    }
    char path[256];snprintf(path,sizeof(path),"/lib/modules/%.127s",base);int fd=open(path,O_RDONLY|O_CLOEXEC);
    if(fd<0){note("module missing %s",path);return;}
    char parameters[4096];module_parameters(base,parameters,sizeof(parameters));if(*parameters)note("module %s kernel arguments: %s",base,parameters);
    int r=(int)syscall(SYS_finit_module,fd,parameters,0),e=errno;close(fd);
    if(r<0&&e!=EEXIST)note("module %s FAIL errno=%d %s",base,e,strerror(e));
    else note("module %s OK",base);
    if(idx>=0)mods[idx].state=r==0||e==EEXIST?2:0;
    drain();watchdog();
    if(idx>=0&&mods[idx].state==2)load_words(mods[idx].post,depth);
}
static void module_index(void) {
    if(nmods)return;
    FILE *f=fopen("/lib/modules/modules.dep","r");char line[8192];
    if(f) {while(fgets(line,sizeof(line),f)&&nmods<1024){char *p=strchr(line,':');if(!p)continue;*p++=0;snprintf(mods[nmods].name,sizeof(mods[nmods].name),"%.127s",basename_c(line));snprintf(mods[nmods].deps,sizeof(mods[nmods].deps),"%.4095s",p);nmods++;}fclose(f);}
    f=fopen("/lib/modules/modules.softdep","r");
    if(f){while(fgets(line,sizeof(line),f)){char module[128];if(sscanf(line,"softdep %127s",module)!=1)continue;int idx=-1;for(int j=0;j<nmods;j++)if(same_module(module,mods[j].name)){idx=j;break;}if(idx<0)continue;
        char *pre=strstr(line,"pre:"),*post=strstr(line,"post:");if(pre){pre+=4;if(post&&post>pre){size_t n=(size_t)(post-pre);if(n>=sizeof(mods[idx].pre))n=sizeof(mods[idx].pre)-1;memcpy(mods[idx].pre,pre,n);mods[idx].pre[n]=0;}else snprintf(mods[idx].pre,sizeof(mods[idx].pre),"%.1023s",pre);}if(post)snprintf(mods[idx].post,sizeof(mods[idx].post),"%.1023s",post+5);
    }fclose(f);}
}
static void loadmodules(void) {
    module_index();FILE *f;char line[8192];
    note("module hard/soft dependencies: %d",nmods);f=fopen("/lib/modules/modules.load","r");
    if(!f){note("FAIL no stock vendor ramdisk modules.load; use matching stock vendor_boot");return;}
    int i=0;while(fgets(line,sizeof(line),f)){line[strcspn(line,"# \t\r\n")]=0;if(*line)loadmod(line,0);if((++i%8)==0){discover();persistent();}}fclose(f);
    /* Normal first-stage modules.load omits the USB drivers that Android loads
     * later. These are present in this device's vendor recovery ramdisk. */
    const char *usbmods[]={"pinctrl-spmi-gpio.ko","pinctrl-spmi-mpp.ko","bam_dma.ko","msm_gpi.ko","i2c-msm-geni.ko","pmic_glink.ko","ucsi_glink.ko","usb_bam.ko","phy-generic.ko","phy-msm-ssusb-qmp.ko","phy-msm-snps-eusb2.ko","phy-msm-m31-eusb2.ko","repeater.ko","repeater-qti-pmic-eusb2.ko","repeater-i2c-eusb2.ko","redriver.ko","eud.ko","ps5169.ko","dwc3-msm.ko"};
    for(unsigned j=0;j<sizeof(usbmods)/sizeof(usbmods[0]);j++){loadmod(usbmods[j],0);discover();persistent();}
    if(access("/hmos-hdf-graphics.hcb",F_OK)==0)loadmod("msm_drm.ko",0);
}

/* SHA-256 for validating liblp geometry/header/table checksums before mapping. */
static uint32_t rr(uint32_t x,int n){return(x>>n)|(x<<(32-n));}
static void sha256(const void *data,size_t len,uint8_t out[32]) {
    static const uint32_t k[64]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const uint8_t *src=data;size_t total=((len+9+63)/64)*64;
    for(size_t off=0;off<total;off+=64){uint8_t b[64]={0};for(size_t j=0;j<64;j++){size_t x=off+j;if(x<len)b[j]=src[x];else if(x==len)b[j]=0x80;else if(x>=total-8)b[j]=(uint8_t)(((uint64_t)len*8)>>(8*(total-1-x)));}
        uint32_t w[64];for(int j=0;j<16;j++)w[j]=((uint32_t)b[j*4]<<24)|((uint32_t)b[j*4+1]<<16)|((uint32_t)b[j*4+2]<<8)|b[j*4+3];
        for(int j=16;j<64;j++)w[j]=w[j-16]+(rr(w[j-15],7)^rr(w[j-15],18)^(w[j-15]>>3))+w[j-7]+(rr(w[j-2],17)^rr(w[j-2],19)^(w[j-2]>>10));
        uint32_t a=h[0],bb=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for(int j=0;j<64;j++){uint32_t t1=hh+(rr(e,6)^rr(e,11)^rr(e,25))+((e&f)^((~e)&g))+k[j]+w[j];uint32_t t2=(rr(a,2)^rr(a,13)^rr(a,22))+((a&bb)^(a&c)^(bb&c));hh=g;g=f;f=e;e=d+t1;d=c;c=bb;bb=a;a=t1+t2;}
        h[0]+=a;h[1]+=bb;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }for(int i=0;i<32;i++)out[i]=(uint8_t)(h[i/4]>>(24-8*(i%4)));
}
static uint32_t u32(const void *p){uint32_t n;memcpy(&n,p,4);return n;}
static uint64_t u64(const void *p){uint64_t n;memcpy(&n,p,8);return n;}
static int digest_ok(uint8_t *b,size_t n,size_t at) {uint8_t expected[32],actual[32];memcpy(expected,b+at,32);memset(b+at,0,32);sha256(b,n,actual);memcpy(b+at,expected,32);return !memcmp(expected,actual,32);}
static void dm_init(void *buf,size_t sz,const char *name) {
    memset(buf,0,sz);struct dm_ioctl *d=buf;d->version[0]=4;d->version[1]=0;d->version[2]=0;d->data_size=(uint32_t)sz;d->data_start=sizeof(*d);snprintf(d->name,sizeof(d->name),"%s",name);
}
static int dm_map(const char *name,const uint8_t *ext,uint32_t n,uint32_t es,dev_t superdev) {
    int fd=open("/dev/mapper/control",O_RDWR|O_CLOEXEC);if(fd<0){note("DM control: %s",strerror(errno));return -1;}
    char buf[65536];struct dm_ioctl *d=(void*)buf;dm_init(buf,sizeof(buf),name);
    if(ioctl(fd,DM_DEV_CREATE,d)<0){note("DM create %s: %s",name,strerror(errno));close(fd);return -1;}
    dev_t dev=(dev_t)d->dev;dm_init(buf,sizeof(buf),name);d->flags=DM_READONLY_FLAG;d->target_count=n;
    size_t off=sizeof(*d);uint64_t logical=0;
    for(uint32_t i=0;i<n;i++){const uint8_t *e=ext+i*es;size_t start=off;struct dm_target_spec *t=(void*)(buf+off);off+=sizeof(*t);t->sector_start=logical;t->length=u64(e);logical+=t->length;strcpy(t->target_type,"linear");
        int z=snprintf(buf+off,sizeof(buf)-off,"%u:%u %llu",major(superdev),minor(superdev),(unsigned long long)u64(e+12));if(z<0||(size_t)z>=sizeof(buf)-off){close(fd);return -1;}off=(off+(size_t)z+1+7)&~7UL;t->next=(uint32_t)(off-start);}
    d->data_size=(uint32_t)off;
    if(ioctl(fd,DM_TABLE_LOAD,d)<0){note("DM load %s: %s",name,strerror(errno));close(fd);return -1;}
    dm_init(buf,sizeof(buf),name);if(ioctl(fd,DM_DEV_SUSPEND,d)<0){note("DM resume %s: %s",name,strerror(errno));close(fd);return -1;}
    char p[256];snprintf(p,sizeof(p),"/dev/mapper/%s",name);int r=mknod(p,S_IFBLK|0600,dev);close(fd);return r==0||errno==EEXIST?0:-1;
}
struct table {uint32_t off,num,es;};
static int table_get(const uint8_t *b,size_t ts,struct table *t,size_t at,size_t min) {
    t->off=u32(b+at);t->num=u32(b+at+4);t->es=u32(b+at+8);
    return t->es>=min&&t->num<=1024&&(uint64_t)t->off+(uint64_t)t->num*t->es<=ts;
}
static const char *const logical_parts[]={"system","vendor","sys_prod","vendor_dlkm","preload"};
static int logical_part_index(const char *name) {
    for(unsigned j=0;j<sizeof(logical_parts)/sizeof(logical_parts[0]);j++){
        char active[64];snprintf(active,sizeof(active),"%s%s",logical_parts[j],suffix);
        if(!strcmp(name,logical_parts[j])||!strcmp(name,active))return (int)j;
    }
    return -1;
}
static int logical_source(const char *part,char *path,size_t capacity) {
    struct stat st;
    snprintf(path,capacity,"/dev/mapper/hmos_%s%s",part,suffix);
    if(stat(path,&st)==0&&S_ISBLK(st.st_mode))return 0;
    snprintf(path,capacity,"/dev/mapper/hmos_%s",part);
    if(stat(path,&st)==0&&S_ISBLK(st.st_mode))return 0;
    errno=ENOENT;note("Missing required logical partition %s (active or shared)",part);return -1;
}
/* dipper 的 Android 侧是单槽静态分区：system 和 vendor 是真实分区，没有 super，
 * 也没有 sys_prod/preload/vendor_dlkm 分区。后两者以目录形式放在 cust 上，与兼容层
 * 载荷同处一个分区。只有引导链（abl/xbl/tz 等）是 A/B，与这里的挂载无关。 */
static int mkdirs(const char *path);
static const char *const static_parts[]={"system","vendor"};
static const char *const staged_parts[]={"sys_prod","preload"};
#define PAYLOAD_DEV "/dev/block/by-name/cust"
#define PAYLOAD_MNT "/cust"
static int static_layout;
static int mount_readonly(const char *dev,const char *mp) {
    /* 镜像可能是 ext4、erofs 或 f2fs：按顺序试挂，不靠分区名猜格式 */
    const char *fs[]={"ext4","erofs","f2fs"};
    for(unsigned i=0;i<sizeof(fs)/sizeof(fs[0]);i++){
        if(mount(dev,mp,fs[i],MS_RDONLY|MS_NOSUID|MS_NODEV|MS_NOATIME,"")==0)return (int)i+1;
        if(errno==EBUSY)return 0;
    }
    return -1;
}
static int static_partitions_at(const char *devroot,const char *mntroot) {
    unsigned total=(unsigned)(sizeof(static_parts)/sizeof(static_parts[0])),mounted=0;
    for(unsigned i=0;i<total;i++){
        char dev[256],mp[256];
        snprintf(dev,sizeof(dev),"%s/%s",devroot,static_parts[i]);
        snprintf(mp,sizeof(mp),"%s/%s",mntroot,static_parts[i]);
        if(access(dev,F_OK)<0){note("FAIL static partition %s missing (%s)",static_parts[i],dev);continue;}
        if(mkdirs(mp)<0)return -1;
        int fsid=mount_readonly(dev,mp);
        if(fsid<0){note("FAIL mount %s: %s",dev,strerror(errno));continue;}
        mounted++;note("MOUNT %s -> %s read-only fs=%d",dev,mp,fsid);
    }
    if(mounted!=total){note("FAIL static layout needs both system and vendor");errno=ENODEV;return -1;}
    return 0;
}
/* 载荷根整体绑到 /compat：第二阶段用 /compat/bin/init 启动。启动脚本另外按绝对
 * 路径引用 /graphics、/busybox 和 /hmos-init-debug.cfg，这几项单独再绑一次。
 * 刻意不整目录展开绑定：/bin、/lib64、/etc 属于 initramfs 自己，不能被载荷盖住。 */
static const char *const payload_entries[]={"graphics","busybox","hmos-init-debug.cfg"};
static int payload_bind(const char *root,const char *target) {
    if(access(root,F_OK)<0){note("FAIL payload root %s missing",root);errno=ENOENT;return -1;}
    char compat[512],init[1024];
    snprintf(compat,sizeof(compat),"%s%scompat",target,target[1]?"/":"");
    if(mkdirs(compat)<0)return -1;
    if(mount(root,compat,0,MS_BIND,0)<0){note("FAIL bind %s -> %s: %s",root,compat,strerror(errno));return -1;}
    snprintf(init,sizeof(init),"%s/bin/init",root);
    if(access(init,F_OK)<0){note("FAIL payload %s has no bin/init; second stage would not start",root);errno=ENOENT;return -1;}
    note("PAYLOAD bind %s -> %s",root,compat);
    unsigned bound=1;
    for(unsigned i=0;i<sizeof(payload_entries)/sizeof(payload_entries[0]);i++){
        char src[1024],dst[512];struct stat st;
        snprintf(src,sizeof(src),"%s/%s",root,payload_entries[i]);
        if(lstat(src,&st)<0)continue;
        /* target 为 "/" 时拼成 /name，否则拼成 target/name */
        snprintf(dst,sizeof(dst),"%s%s%s",target,target[1]?"/":"",payload_entries[i]);
        if(S_ISDIR(st.st_mode)){if(mkdirs(dst)<0)return -1;}
        else {int fd=open(dst,O_CREAT|O_CLOEXEC|O_WRONLY,0600);if(fd<0)return -1;close(fd);}
        if(mount(src,dst,0,MS_BIND,0)<0){note("FAIL bind %s -> %s: %s",src,dst,strerror(errno));return -1;}
        note("PAYLOAD bind %s -> %s",src,dst);
        bound++;
    }
    note("PAYLOAD bound entries=%u from %s",bound,root);
    return 0;
}
static int staged_parts_at(const char *staged,const char *mntroot) {
    unsigned present=0;
    for(unsigned i=0;i<sizeof(staged_parts)/sizeof(staged_parts[0]);i++){
        char src[1024],dst[256];
        snprintf(src,sizeof(src),"%s/%s",staged,staged_parts[i]);
        snprintf(dst,sizeof(dst),"%s/%s",mntroot,staged_parts[i]);
        if(access(src,F_OK)<0){note("staged %s absent; donor partition not carried over",staged_parts[i]);continue;}
        if(mkdirs(dst)<0)return -1;
        if(mount(src,dst,0,MS_BIND,0)<0){note("FAIL bind %s: %s",src,strerror(errno));return -1;}
        if(mount(0,dst,0,MS_BIND|MS_REMOUNT|MS_RDONLY|MS_NOSUID|MS_NODEV,0)<0)note("bind %s stays writable: %s",dst,strerror(errno));
        present++;
    }
    note("STAGED parts mounted=%u",present);
    return 0;
}
static int cust_payload_at(const char *dev,const char *mnt,const char *bindroot,const char *stagedroot) {
    if(access(dev,F_OK)<0){note("FAIL cust partition missing (%s); no compatibility payload",dev);errno=ENODEV;return -1;}
    if(mkdirs(mnt)<0)return -1;
    /* cust 必须可写：载荷脚本会 chmod 自己的文件，只读挂载会让 set -eu 直接退出 */
    if(mount(dev,mnt,"ext4",MS_NOSUID|MS_NODEV|MS_NOATIME,"")<0&&errno!=EBUSY){note("FAIL cust mount: %s",strerror(errno));return -1;}
    char root[256],staged[512];snprintf(root,sizeof(root),"%s/compat",mnt);snprintf(staged,sizeof(staged),"%s/hmos",mnt);
    if(access(root,F_OK)<0){note("FAIL compatibility payload missing at %s",root);errno=ENOENT;return -1;}
    if(payload_bind(root,bindroot)<0)return -1;
    return staged_parts_at(staged,stagedroot);
}
static int static_layout_mount(void) {
    if(static_partitions_at("/dev/block/by-name","/hmos")<0)return -1;
    if(cust_payload_at(PAYLOAD_DEV,PAYLOAD_MNT,"/","/hmos")<0)return -1;
    static_layout=1;
    note("STATIC LAYOUT ready: system+vendor read-only, payload from %s/compat",PAYLOAD_MNT);
    return 0;
}
static void partitions(const char *superpath,int inspect) {
    int fd=open(superpath,O_RDONLY|O_CLOEXEC);if(fd<0){note("super not found: %s",strerror(errno));return;}
    uint8_t geom[4096];int valid=0;
    for(int copy=0;copy<2;copy++){if(pread(fd,geom,sizeof(geom),4096+copy*4096)!=(ssize_t)sizeof(geom))continue;uint32_t size=u32(geom+4);if(u32(geom)==0x616c4467&&size>=52&&size<=4096&&digest_ok(geom,size,8)){valid=1;break;}}
    if(!valid){note("FAIL LP geometry/checksum");close(fd);return;}
    uint32_t max=u32(geom+40),slots=u32(geom+44);if(max<128||max>1048576||slots<1||slots>8){note("FAIL LP geometry bounds");close(fd);return;}
    if(!slot_known){note("FAIL active slot unknown; refusing to assume A or map partitions");close(fd);return;}
    unsigned slot=suffix[1]=='b'?1:0;if(slot>=slots){note("FAIL metadata slot out of range");close(fd);return;}
    uint8_t *b=malloc(max);if(!b){close(fd);return;}valid=0;uint32_t hs=0,ts=0;
    for(int backup=0;backup<2;backup++){off_t off=12288+(off_t)max*(slot+(backup?slots:0));if(pread(fd,b,max,off)!=(ssize_t)max)continue;hs=u32(b+8);ts=u32(b+44);
        if(u32(b)!=0x414c5030||b[4]!=10||hs<128||hs>max||ts>max-hs)continue;
        uint8_t sum[32];sha256(b+hs,ts,sum);
        if(digest_ok(b,hs,12)&&!memcmp(sum,b+48,32)){valid=1;note("LP slot=%u %s checksums OK",slot,backup?"backup":"primary");break;}}
    if(!valid){note("FAIL LP metadata/checksum");free(b);close(fd);return;}
    struct table pt,et,bt;if(!table_get(b,ts,&pt,80,52)||!table_get(b,ts,&et,92,24)||!table_get(b,ts,&bt,116,64)||bt.num!=1){note("FAIL LP tables/multiple block devices unsupported");free(b);close(fd);return;}
    uint8_t *tbl=b+hs;uint64_t super_size=u64(tbl+bt.off+16);struct stat st;fstat(fd,&st);
    /* Prefer an active-slot entry when present, otherwise accept a shared
     * unsuffixed entry. Never map the inactive slot or an unknown partition. */
    unsigned selected[sizeof(logical_parts)/sizeof(logical_parts[0])]={0};
    unsigned priority[sizeof(logical_parts)/sizeof(logical_parts[0])]={0};
    for(uint32_t i=0;i<pt.num;i++){
        uint8_t *p=tbl+pt.off+i*pt.es;char name[37];memcpy(name,p,36);name[36]=0;
        if(!u32(p+44))continue;
        int j=logical_part_index(name);if(j<0)continue;
        unsigned rank=strcmp(name,logical_parts[j])?2:1;
        if(rank==priority[j]){note("FAIL ambiguous logical partition %s",name);free(b);close(fd);return;}
        if(rank>priority[j]){priority[j]=rank;selected[j]=i+1;}
    }
    unsigned found=0;
    for(uint32_t i=0;i<pt.num;i++){
        uint8_t *p=tbl+pt.off+i*pt.es;char name[37];memcpy(name,p,36);name[36]=0;uint32_t first=u32(p+40),n=u32(p+44);uint64_t size=0;
        int part=logical_part_index(name);
        if(!n||part<0||selected[part]!=i+1)continue;
        found++;
        if(n>64||first>et.num||n>et.num-first){note("FAIL extent bounds %s",name);continue;}int good=1;
        for(uint32_t j=0;j<n;j++){uint8_t *e=tbl+et.off+(first+j)*et.es;uint64_t sectors=u64(e),start=u64(e+12);if(u32(e+8)!=0||u32(e+20)!=0||start>super_size/512||sectors>super_size/512-start)good=0;size+=sectors*512;}
        note("PARTITION %s bytes=%llu extents=%u valid=%d",name,(unsigned long long)size,n,good);
        if(!good||inspect)continue;
        char dmname[128];snprintf(dmname,sizeof(dmname),"hmos_%s",name);if(dm_map(dmname,tbl+et.off+first*et.es,n,et.es,st.st_rdev)<0)continue;
        char mp[256],dev[256];snprintf(mp,sizeof(mp),"/hmos/%s",logical_parts[part]);makedir(mp);snprintf(dev,sizeof(dev),"/dev/mapper/%s",dmname);
        int r=mount(dev,mp,"erofs",MS_RDONLY|MS_NOSUID|MS_NODEV,"");note("MOUNT %s -> %s ro result=%d errno=%d %s",dev,mp,r,r<0?errno:0,r<0?strerror(errno):"OK");
    }if(!found)note("FAIL no populated logical partitions for active slot %s",suffix);
    free(b);close(fd);
}
static int usb_configured,usb_mode_set;
static void usb_setup(void) {
    if(usb_configured)return;
    /* The first PS5169 probe enables its GPIO, but its initial ID read can
     * still be zero and return EPROBE_DEFER. Recovery loads more drivers,
     * causing another probe; this small init must request that retry itself. */
    if(access("/sys/bus/i2c/devices/1-0028",F_OK)==0&&access("/sys/bus/i2c/devices/1-0028/driver",F_OK)!=0){int r=put("/sys/bus/i2c/drivers_probe","1-0028");note("USB PS5169 reprobe result=%d errno=%d",r,r<0?errno:0);}
    if(access("/sys/bus/platform/devices/a600000.ssusb",F_OK)==0&&access("/sys/bus/platform/devices/a600000.ssusb/driver",F_OK)!=0){int r=put("/sys/bus/platform/drivers_probe","a600000.ssusb");note("USB controller reprobe result=%d errno=%d",r,r<0?errno:0);}
    /* dipper recovery explicitly switches the Qualcomm wrapper to peripheral
     * before waiting for its child UDC. Waiting for UDC first deadlocks setup. */
    const char *mode="/sys/bus/platform/devices/a600000.ssusb/mode";
    if(!usb_mode_set&&access(mode,F_OK)==0){if(put(mode,"peripheral")==0){usb_mode_set=1;note("USB Qualcomm mode set to peripheral");}else note("USB peripheral mode: %s",strerror(errno));}
    DIR *d=opendir("/sys/class/udc");if(!d)d=opendir("/sys/class/UDC");if(!d)return;struct dirent *de;char controller[256]="";
    while((de=readdir(d)))if(de->d_name[0]!='.'){snprintf(controller,sizeof(controller),"%s",de->d_name);break;}
    closedir(d);if(!*controller)return;
    makedir("/config");if(mount("configfs","/config","configfs",MS_NOSUID|MS_NODEV|MS_NOEXEC,"")<0&&errno!=EBUSY){note("configfs: %s",strerror(errno));return;}
    const char *g="/config/usb_gadget/hmos_probe";makedir(g);char p[512];
#define ATTR(k,v) do{snprintf(p,sizeof(p),"%s/%s",g,k);if(put(p,v)<0)note("USB attr %s: %s",k,strerror(errno));}while(0)
    ATTR("idVendor","0x1d6b");ATTR("idProduct","0x0104");ATTR("bcdUSB","0x0200");ATTR("bcdDevice","0x0100");ATTR("bDeviceClass","0x02");
    makedir("/config/usb_gadget/hmos_probe/strings/0x409");ATTR("strings/0x409/serialnumber","dipper-hmos-probe");ATTR("strings/0x409/manufacturer","Local bring-up");ATTR("strings/0x409/product","dipper HMOS early log");
    makedir("/config/usb_gadget/hmos_probe/configs/c.1");ATTR("configs/c.1/MaxPower","250");
    makedir("/config/usb_gadget/hmos_probe/functions/acm.usb0");
    if(access("/config/usb_gadget/hmos_probe/functions/acm.usb0/port_num",F_OK)){note("FAIL USB ACM function unavailable");return;}
    if(symlink("/config/usb_gadget/hmos_probe/functions/acm.usb0","/config/usb_gadget/hmos_probe/configs/c.1/acm.usb0")<0&&errno!=EEXIST)note("USB function link: %s",strerror(errno));
    snprintf(p,sizeof(p),"%s/UDC",g);if(put(p,controller)<0){note("USB bind %s: %s",controller,strerror(errno));return;}usb_configured=1;note("USB ACM bound to %s",controller);
#undef ATTR
}
#include "probe_command_v116.inc"
static void process_diag(void) {
    note("DIAG begin: process state and native-root device nodes");
    dump("/proc/1/status");dump("/proc/1/task/1/children");dump("/proc/misc");
    dump("/sys/fs/selinux/enforce");
    DIR *dir=opendir("/proc");struct dirent *entry;
    if(dir){while((entry=readdir(dir))){if(entry->d_name[0]<'0'||entry->d_name[0]>'9')continue;
        char path[512],cmd[1024];snprintf(path,sizeof(path),"/proc/%s/cmdline",entry->d_name);
        ssize_t n=readfile(path,cmd,sizeof(cmd));if(n<=0)continue;
        for(ssize_t j=0;j<n;j++)if(!cmd[j])cmd[j]=' ';
        cmd[n]=0;note("PROCESS pid=%s cmd=%s",entry->d_name,cmd);
    }closedir(dir);}
    const char *names[]={"binder","hwbinder","vndbinder","access_token_id","hmos_binder"};
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++){char path[256];struct stat st;snprintf(path,sizeof(path),"/hmos-root/dev/%s",names[i]);if(stat(path,&st)<0)note("DIAG %s: %s",path,strerror(errno));else note("DIAG %s mode=%o uid=%u gid=%u device=%u:%u",path,(unsigned)st.st_mode,(unsigned)st.st_uid,(unsigned)st.st_gid,major(st.st_rdev),minor(st.st_rdev));}
    note("DIAG end");
}
static int ipc_nodes(const char *directory) {
    char misc[8192];int count=0;if(readfile("/proc/misc",misc,sizeof(misc))<0)return -1;
    char *save,*line=strtok_r(misc,"\n",&save);
    while(line){unsigned minor;char name[128];
        if(sscanf(line,"%u %127s",&minor,name)==2&&(!strcmp(name,"hmos_binder")||!strcmp(name,"hmos_hwbinder")||!strcmp(name,"hmos_vndbinder")||!strcmp(name,"access_token_id"))){
            char path[512];snprintf(path,sizeof(path),"%s/%s",directory,name);
            if(mknod(path,S_IFCHR|0666,makedev(10,minor))<0&&errno!=EEXIST)return -1;
            struct stat st;if(stat(path,&st)<0||!S_ISCHR(st.st_mode)||st.st_rdev!=makedev(10,minor)){errno=EEXIST;return -1;}
            if(chmod(path,0666)<0)return -1;
            if(!strcmp(name,"access_token_id")){if(chown(path,3020,3020)<0)return -1;}
            else {char alias[512];snprintf(alias,sizeof(alias),"%s/%s",directory,name+5);if(symlink(name,alias)<0&&errno!=EEXIST)return -1;if(stat(alias,&st)<0||st.st_rdev!=makedev(10,minor)){errno=EEXIST;return -1;}}
            note("IPC node %s device=10:%u",path,minor);count++;
        }line=strtok_r(NULL,"\n",&save);
    }return count;
}
#if DIPPER_IPC_ONEWAY_PID
/* 封印候选副本并核对完整内容 */
static int ipc_verified_fd(int source) {
    static const uint8_t expected[32]={
        0x34,0x72,0x6f,0x52,0x44,0x52,0xad,0x12,
        0x51,0xe9,0x93,0x86,0xd1,0x93,0x15,0xb8,
        0x47,0x59,0x4b,0xcd,0x54,0x7f,0x2d,0x43,
        0xc5,0x23,0x29,0xde,0x97,0xbd,0xdf,0x67
    };
    const size_t size=554256;
    struct stat st;
    if(fstat(source,&st)<0)return -1;
    if(!S_ISREG(st.st_mode)||st.st_size!=(off_t)size){errno=EINVAL;return -1;}
    uint8_t *data=malloc(size),actual[32];
    if(!data)return -1;
    int sealed=-1;
    for(size_t used=0;used<size;){
        ssize_t n=pread(source,data+used,size-used,(off_t)used);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){if(!n)errno=EIO;goto failed;}
        used+=(size_t)n;
    }
    sealed=memfd_create("dipper-ipc-verified",MFD_CLOEXEC|MFD_ALLOW_SEALING);
    if(sealed<0)goto failed;
    for(size_t used=0;used<size;){
        ssize_t n=write(sealed,data+used,size-used);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){if(!n)errno=EIO;goto failed;}
        used+=(size_t)n;
    }
    if(fcntl(sealed,F_ADD_SEALS,F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL)<0||
       lseek(sealed,0,SEEK_SET)!=0)goto failed;
    if(fstat(sealed,&st)<0)goto failed;
    if(st.st_size!=(off_t)size){errno=EINVAL;goto failed;}
    /* 在最终封印的同一描述符上核对完整内容 */
    for(size_t used=0;used<size;){
        ssize_t n=pread(sealed,data+used,size-used,(off_t)used);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){if(!n)errno=EIO;goto failed;}
        used+=(size_t)n;
    }
    sha256(data,size,actual);
    if(memcmp(actual,expected,sizeof(expected))){errno=EBADMSG;goto failed;}
    free(data);return sealed;
failed:
    {int saved=errno;if(sealed>=0)close(sealed);free(data);errno=saved;return -1;}
}
static int ipc_pair_params(char *buffer,size_t size,int stage) {
    int n=snprintf(buffer,size,"diagnostic_stage=%d oneway_sender_pid=1 "
                   "devices=hmos_binder,hmos_hwbinder,hmos_vndbinder,hmos_pidtest,hmos_pidtest_ns",stage);
    if(n<0||(size_t)n>=size){errno=EOVERFLOW;return -1;}
    return 0;
}
/* 仅验证内存文件与参数，不调用模块加载 */
static int ipc_pair_selftest(void) {
    alarm(10);
    int fd=memfd_create("dipper-ipc-selftest",MFD_CLOEXEC|MFD_ALLOW_SEALING);
    if(fd<0){perror("selftest memfd");return 1;}
    int sealed=-1,failed=1;
    sealed=ipc_verified_fd(fd);
    if(sealed>=0||errno!=EINVAL)goto done;
    if(ftruncate(fd,554256)<0)goto done;
    sealed=ipc_verified_fd(fd);
    if(sealed>=0||errno!=EBADMSG)goto done;
    if(ftruncate(fd,3)<0||pwrite(fd,"abc",3,0)!=3)goto done;
    int seals=F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL;
    if(fcntl(fd,F_ADD_SEALS,seals)<0||fcntl(fd,F_GET_SEALS)!=seals)goto done;
    if(pwrite(fd,"x",1,0)!=-1||errno!=EPERM)goto done;
    if(ftruncate(fd,4)!=-1||errno!=EPERM)goto done;
    char data[3];
    if(pread(fd,data,sizeof(data),0)!=3||memcmp(data,"abc",3))goto done;
    for(int stage=-1;stage<=3;stage++){
        char params[256],expected[256];
        snprintf(expected,sizeof(expected),"diagnostic_stage=%d oneway_sender_pid=1 "
                 "devices=hmos_binder,hmos_hwbinder,hmos_vndbinder,hmos_pidtest,hmos_pidtest_ns",stage);
        if(ipc_pair_params(params,sizeof(params),stage)<0||strcmp(params,expected))goto done;
        if(ipc_pair_params(params,8,stage)!=-1||errno!=EOVERFLOW)goto done;
    }
    failed=0;
done:
    if(sealed>=0)close(sealed);
    close(fd);alarm(0);
    puts(failed?"FAIL IPC pair memfd/hash/options selftest":"PASS IPC pair memfd/hash/options; module loading was not called");
    return failed;
}
/* 仅创建合约专用节点，不占用系统上下文 */
static int ipc_pid_nodes(void) {
    char misc[8192];int count=0;
    if(readfile("/proc/misc",misc,sizeof(misc))<0)return -1;
    char *save,*line=strtok_r(misc,"\n",&save);
    while(line){
        unsigned minor;char name[128];
        if(sscanf(line,"%u %127s",&minor,name)==2&&
           (!strcmp(name,"hmos_pidtest")||!strcmp(name,"hmos_pidtest_ns"))){
            char path[256];struct stat st;
            snprintf(path,sizeof(path),"/dev/%s",name);
            if(mknod(path,S_IFCHR|0600,makedev(10,minor))<0&&errno!=EEXIST)return -1;
            if(lstat(path,&st)<0)return -1;
            if(!S_ISCHR(st.st_mode)||st.st_rdev!=makedev(10,minor)){errno=EEXIST;return -1;}
            if(chmod(path,0600)<0)return -1;
            count++;
        }
        line=strtok_r(NULL,"\n",&save);
    }
    return count;
}
#endif
static void ipc_test(int which) {
    if(ipc_test_pid>0||ipc_pid>0){note("IPC test rejected: another IPC operation is running");return;}
    if(access("/sys/module/dipper_hmos_ipc",F_OK)<0||ipc_nodes("/dev")!=4){note("IPC test requires a loaded driver and all four verified misc nodes");return;}
    const char *program=which==1?"/token_contract":"/binder_nested";
    int timeout=35;
#if DIPPER_IPC_ONEWAY_PID
    /* 仅在独立节点验证保留发送方标识 */
    if(which==3){
        char mode[16];
        if(!ipc_pair_loaded||
           readfile("/sys/module/dipper_hmos_ipc/parameters/oneway_sender_pid",mode,sizeof(mode))<0||
           (strcmp(mode,"Y\n")&&strcmp(mode,"1\n"))||ipc_pid_nodes()!=2){
            note("IPC PID contract requires the verified pair, retained mode and both private nodes");return;
        }
        program="/binder_oneway_pid";timeout=60;
    }
#endif
    note("IPC CONTRACT begin %s; %d-second process-group timeout",program,timeout);
    pid_t child=fork();
    if(child==0){if(setsid()<0)_exit(126);int out=open("/dev/kmsg",O_WRONLY|O_CLOEXEC);if(out<0)_exit(126);dup2(out,1);dup2(out,2);close(out);
#if DIPPER_IPC_ONEWAY_PID
        if(which==3){
            char *args[]={(char*)program,"/dev/hmos_pidtest","/dev/hmos_pidtest_ns","1",NULL};
            execv(program,args);dprintf(2,"IPC PID contract exec failed: %s\n",strerror(errno));_exit(126);
        }
#endif
        char *args[]={(char*)program,"/dev/access_token_id","/dev/binder",NULL};execv(program,args);dprintf(2,"IPC contract exec failed: %s\n",strerror(errno));_exit(126);}
    if(child<0){note("IPC test fork failed: %s",strerror(errno));return;}
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);ipc_test_pid=child;ipc_test_deadline=ts.tv_sec+timeout;
}
static void ipc_test_tick(void) {
    if(ipc_test_pid<=0)return;
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
    if(ts.tv_sec>=ipc_test_deadline){note("IPC CONTRACT timeout; terminating only test process group %ld",(long)ipc_test_pid);kill(-ipc_test_pid,SIGKILL);ipc_test_deadline=ts.tv_sec+3600;}
}
static void ipc_load(void) {
    if(ipc_pid>0){note("IPC LOAD already running pid=%ld",(long)ipc_pid);return;}
    if(access("/sys/module/dipper_hmos_ipc",F_OK)==0){note("IPC module already loaded; reboot before another load");return;}
    int fd=open("/hmos-ipc.ko",O_RDONLY|O_CLOEXEC
#if DIPPER_IPC_ONEWAY_PID
                |O_NOFOLLOW
#endif
    );if(fd<0){note("IPC candidate missing: %s",strerror(errno));return;}
#if DIPPER_IPC_ONEWAY_PID
    /* 校验失败时保留串口与恢复入口 */
    int verified=ipc_verified_fd(fd),saved=errno;close(fd);fd=verified;
    if(fd<0){note("IPC pair verification failed: %s; HOLD",strerror(saved));return;}
    note("IPC verified pair sha256=34726f524452ad1251e99386d19315b847594bcd547f2d43c52329de97bddf67 retained=1");
#endif
    int result[2];if(pipe2(result,O_CLOEXEC|O_NONBLOCK)<0){close(fd);note("IPC pipe failed: %s",strerror(errno));return;}
    note("IPC LOAD begin: stage=%d, separate child, PID1 continues logging",ipc_stage);
    if(logfd>=0)fsync(logfd);
    pid_t child=fork();
    if(child==0){
        close(result[0]);
#if DIPPER_IPC_ONEWAY_PID
        char params[256];
        if(ipc_pair_params(params,sizeof(params),ipc_stage)<0){
            int values[2]={-1,errno};allwrite(result[1],values,sizeof(values));_exit(125);
        }
#else
        char params[64];snprintf(params,sizeof(params),"diagnostic_stage=%d",ipc_stage);
#endif
        int out=open("/dev/kmsg",O_WRONLY|O_CLOEXEC);
        if(out>=0)dprintf(out,"<6>DIPPER_IPC user-loader pid=%ld params=%s\n",(long)getpid(),params);
        int values[2];values[0]=(int)syscall(SYS_finit_module,fd,params,0);values[1]=values[0]<0?errno:0;
        if(out>=0)dprintf(out,"<6>DIPPER_IPC user-loader result=%d errno=%d\n",values[0],values[1]);
        allwrite(result[1],values,sizeof(values));_exit(values[0]<0?125:0);
    }
    close(fd);close(result[1]);
    if(child<0){close(result[0]);note("IPC fork failed: %s",strerror(errno));return;}
    ipc_pid=child;ipc_resultfd=result[0];note("IPC child=%ld; USB/recovery collector stays in PID1",(long)child);
}
static void reap_children(void) {
    pid_t done;int status;
    while((done=waitpid(-1,&status,WNOHANG))>0){
        if(command_finish(done,status))continue;
        if(done==ipc_pid){
            int values[2];ssize_t n=read(ipc_resultfd,values,sizeof(values));
            if(n==(ssize_t)sizeof(values))note("IPC LOAD result=%d errno=%d %s",values[0],values[1],values[1]?strerror(values[1]):"success");
            else note("IPC child ended without syscall result; status=%d signal=%d",status,WIFSIGNALED(status)?WTERMSIG(status):0);
#if DIPPER_IPC_ONEWAY_PID
            ipc_pair_loaded=n==(ssize_t)sizeof(values)&&values[0]==0&&values[1]==0&&
                            WIFEXITED(status)&&WEXITSTATUS(status)==0;
#endif
            close(ipc_resultfd);ipc_resultfd=-1;ipc_pid=0;dump("/proc/misc");dump("/proc/modules");
            if(logfd>=0)fsync(logfd);
        }else if(done==ipc_test_pid){note("IPC CONTRACT ended pid=%ld status=%d exit=%d signal=%d",(long)done,status,WIFEXITED(status)?WEXITSTATUS(status):-1,WIFSIGNALED(status)?WTERMSIG(status):0);kill(-ipc_test_pid,SIGKILL);ipc_test_pid=0;}
        else note("Child reaped pid=%ld status=%d",(long)done,status);
    }
}
static void serial_command(char *cmd) {
    if(!strncmp(cmd,"cmd ",4)){
        // An explicit command takes priority over old unsolicited USB replay.
        // Persistent history remains available through the replay command.
        serial_pos=used;command_start(cmd+4);return;
    }
    char *ipc=strstr(cmd,"ipc");if(ipc){ipc_stage=ipc[3]>='0'&&ipc[3]<='3'?ipc[3]-'0':-1;ipc_requested=1;}if(strstr(cmd,"test-token"))ipc_test_requested=1;if(strstr(cmd,"test-binder"))ipc_test_requested=2;if(strstr(cmd,"replay"))serial_pos=0;if(strstr(cmd,"status"))note("status: pid=%ld traced=%ld held=%d syscalls=%d ipc_child=%ld test_child=%ld; %zu bytes history; persistent=%s",(long)getpid(),(long)traced_pid,trace_held,trace_syscalls,(long)ipc_pid,(long)ipc_test_pid,used,logfd>=0?logpath:"unavailable");if(strstr(cmd,"diag"))process_diag();if(strstr(cmd,"sync")&&logfd>=0)fsync(logfd);if(strstr(cmd,"trace"))trace_requested=1;if(strstr(cmd,"hmos-list"))trace_requested=2;else if(strstr(cmd,"hmos"))trace_requested=3;
#if DIPPER_IPC_ONEWAY_PID
    if(strstr(cmd,"test-oneway"))ipc_test_requested=3;
#endif
    if(strstr(cmd,"reboot-recovery")){note("USB requested reboot to recovery");if(logfd>=0)fsync(logfd);sync();syscall(SYS_reboot,LINUX_REBOOT_MAGIC1,LINUX_REBOOT_MAGIC2,LINUX_REBOOT_CMD_RESTART2,"recovery");note("reboot syscall returned: %s",strerror(errno));}
    /* 同步日志后返回引导加载器 */
    if(strstr(cmd,"reboot-bootloader")){
        note("USB requested reboot to bootloader");
        if(logfd>=0)fsync(logfd);
        sync();
        syscall(SYS_reboot,LINUX_REBOOT_MAGIC1,LINUX_REBOOT_MAGIC2,
                LINUX_REBOOT_CMD_RESTART2,"bootloader");
        note("reboot syscall returned: %s",strerror(errno));
    }
}
#include "probe_serial_v121.inc"
static void slot_detect(void) {
    char b[32768];const char *files[]={"/proc/bootconfig","/proc/cmdline"};
    for(unsigned i=0;i<2;i++)if(readfile(files[i],b,sizeof(b))>0){char *p=strstr(b,"androidboot.slot_suffix");if(p){p=strchr(p,'=');if(p){p++;while(*p==' '||*p=='\t'||*p=='\"')p++;if(p[0]=='_'&&(p[1]=='a'||p[1]=='b')){suffix[1]=p[1];slot_known=1;}}}}
    note("active slot=%s known=%d",suffix,slot_known);
}
static void snapshots(void) {
    dump("/proc/cmdline");dump("/proc/bootconfig");dump("/proc/version");dump("/proc/partitions");dump("/proc/filesystems");dump("/proc/modules");dump("/proc/mounts");
    dump("/hmos/vendor/etc/fstab.KirinT93B");dump("/hmos/system/system/etc/ld-musl-aarch64.path");
    makedir("/sys/kernel/debug");mount("debugfs","/sys/kernel/debug","debugfs",MS_NOSUID|MS_NODEV|MS_NOEXEC,"");dump("/sys/kernel/debug/devices_deferred");
    dump("/sys/class/usb_role/a600000.ssusb-role-switch/role");dump("/sys/class/usb_role/a600000.dwc3-role-switch/role");
    dump("/sys/bus/platform/devices/a600000.ssusb/mode");
    const char *paths[]={"/hmos/system/system/bin/init","/hmos/system/system/lib/ld-musl-aarch64.so.1","/hmos/system/init"};
    for(unsigned i=0;i<3;i++){struct stat st;int r=lstat(paths[i],&st);note("ENTRY %s present=%d",paths[i],r==0);if(r==0&&S_ISLNK(st.st_mode)){char b[256];ssize_t n=readlink(paths[i],b,sizeof(b)-1);if(n>=0){b[n]=0;note("symlink -> %s",b);}}}
}
static int mkdirs(const char *path) {
    char b[512];if(strlen(path)>=sizeof(b))return -1;strcpy(b,path);
    for(char *p=b+1;;p++){if(*p!='/'&&*p)continue;char c=*p;*p=0;int created=mkdir(b,0755);if(created<0&&errno!=EEXIST)return -1;if(created==0&&chmod(b,0755)<0)return -1;*p=c;if(!c)break;}return 0;
}
static int textfile(const char *path,const char *text) {
    char parent[512];if(strlen(path)>=sizeof(parent))return -1;strcpy(parent,path);char *slash=strrchr(parent,'/');if(slash){*slash=0;if(mkdirs(parent)<0)return -1;}
    int fd=open(path,O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC,0644);if(fd<0)return -1;if(fchmod(fd,0644)<0){close(fd);return -1;}allwrite(fd,text,strlen(text));close(fd);return 0;
}
static int ram_overlay(const char *lower,const char *target,const char *name) {
    char upper[256],work[256],opts[1024];snprintf(upper,sizeof(upper),"/hmos-ram/%s",name);snprintf(work,sizeof(work),"/hmos-ram/%s.work",name);
    if(mkdirs(upper)<0||mkdirs(work)<0||mkdirs(target)<0)return -1;
    snprintf(opts,sizeof(opts),"lowerdir=%s,upperdir=%s,workdir=%s",lower,upper,work);
    if(mount("overlay",target,"overlay",MS_NOSUID,opts)<0){note("HMOS overlay %s failed: %s",name,strerror(errno));return -1;}return 0;
}
static int ram_mount(const char *target,const char *options) {
    if(mkdirs(target)<0)return -1;
    return mount("tmpfs",target,"tmpfs",MS_NOSUID|MS_NODEV,options);
}
#include "probe_graphics_v118.inc"
static int hmos_root(void) {
    /* The collector was forked before this point and keeps umask 0077.
     * Native init and its RAM filesystem use the normal boot umask. */
    umask(0022);
    /* System upper layers are RAM-backed; physical userdata supplies /data.
     * Logical devices remain read-only; userdata is formatted as ext4 in Recovery. */
    if(ram_mount("/hmos-ram","mode=0755,size=2048m")<0)return -1;
    if(ram_overlay("/hmos/system","/hmos-root","root")<0)return -1;
    struct stat root_mode;if(stat("/hmos-root",&root_mode)<0)return -1;note("HMOS RAM root mode=%o uid=%u gid=%u",(unsigned)(root_mode.st_mode&07777),(unsigned)root_mode.st_uid,(unsigned)root_mode.st_gid);if((root_mode.st_mode&0777)!=0755){errno=EACCES;return -1;}
    /* compat 也在 cust 上，用 RAM overlay 挂进新根：载荷脚本改写的是内存副本 */
    const char *parts[]={"vendor","sys_prod","vendor_dlkm","preload","compat"};
    for(unsigned i=0;i<sizeof(parts)/sizeof(parts[0]);i++){char lower[256],target[256];snprintf(lower,sizeof(lower),"/hmos/%s",parts[i]);snprintf(target,sizeof(target),"/hmos-root/%s",parts[i]);/* 本机没有独立 vendor_dlkm 分区：内核模块在 vendor/lib/modules */if(!strcmp(parts[i],"vendor_dlkm")&&access(lower,F_OK)<0){note("HMOS vendor_dlkm absent; modules stay in vendor/lib/modules");continue;}if(!strcmp(parts[i],"vendor")||!strcmp(parts[i],"vendor_dlkm")){
            if(mkdirs(target)<0||mount(lower,target,0,MS_BIND,0)<0||mount(0,target,0,MS_BIND|MS_REMOUNT|MS_RDONLY|MS_NOSUID,0)<0)return -1;
        }else if(ram_overlay(lower,target,parts[i])<0)return -1;}
    if(ram_mount("/hmos-root/data","mode=0771,size=256m")<0||ram_mount("/hmos-root/metadata","mode=0755,size=16m")<0||ram_mount("/hmos-root/log","mode=0755,size=64m")<0||ram_mount("/hmos-root/mnt","mode=0755,size=64m")<0||ram_mount("/hmos-root/storage","mode=0755,size=64m")<0)return -1;
    if(mount(0,"/hmos-root/mnt",0,MS_SLAVE,0)<0||ram_mount("/hmos-root/mnt/data","mode=0755,size=32m")<0||mount(0,"/hmos-root/mnt/data",0,MS_SHARED,0)<0||mkdirs("/hmos-root/mnt/sandbox")<0)return -1;
    if(mkdirs("/hmos-root/dev")<0||mount("tmpfs","/hmos-root/dev","tmpfs",MS_NOSUID,"mode=0755,size=32m")<0)return -1;
    struct {const char *name;unsigned maj,min;} chars[]={{"null",1,3},{"zero",1,5},{"random",1,8},{"urandom",1,9},{"kmsg",1,11},{"tty",5,0},{"console",5,1}};
    for(unsigned i=0;i<sizeof(chars)/sizeof(chars[0]);i++){char p[256];snprintf(p,sizeof(p),"/hmos-root/dev/%s",chars[i].name);mode_t mode=(!strcmp(chars[i].name,"kmsg")||!strcmp(chars[i].name,"console"))?0600:0666;if(mknod(p,S_IFCHR|mode,makedev(chars[i].maj,chars[i].min))<0&&errno!=EEXIST)return -1;if(chmod(p,mode)<0)return -1;}
    if(mkdirs("/hmos-root/dev/block/by-name")<0||mkdirs("/hmos-root/dev/block/mapper")<0||mkdirs("/hmos-root/dev/socket")<0||mkdirs("/hmos-root/dev/unix/socket")<0||mkdirs("/hmos-root/dev/pts")<0)return -1;
    if(mount("devpts","/hmos-root/dev/pts","devpts",0,"mode=0620,ptmxmode=0666")<0)return -1;
    if(symlink("pts/ptmx","/hmos-root/dev/ptmx")<0&&errno!=EEXIST)return -1;
    /* Mount the whole ext4 userdata partition; no special UUID is required. */
    if(graphics_run_script("/graphics/init-userdata-ext4.sh")<0)return -1;
    const char *state_dirs[]={"/hmos-root/dev/__parameters__","/hmos-root/data/service/el0/startup/init","/hmos-root/data/service/el1/startup/parameters","/hmos-root/data/local/tmp","/hmos-root/data/service/el1/public","/hmos-root/data/app/el1","/hmos-root/data/storage/el1","/hmos-root/data/storage/el2","/hmos-root/data/service/el1/public/for-all-app","/hmos-root/data/service/el1/startup","/hmos-root/data/data"};
    for(unsigned i=0;i<sizeof(state_dirs)/sizeof(state_dirs[0]);i++)if(mkdirs(state_dirs[i])<0)return -1;
    if(graphics_ram_root()<0)return -1;
    if(static_layout){
        /* 单槽静态分区：system/vendor 的 by-name 节点由 discover() 直接从内核
         * 分区名建立，sys_prod/preload 是 cust 上的目录，没有块设备可给。 */
        note("HMOS static layout: by-name nodes come from the kernel partition table");
    }else{
    for(unsigned i=0;i<sizeof(logical_parts)/sizeof(logical_parts[0]);i++){
        char source[256],dest[256],alias[256],link[256];struct stat st;
        if(logical_source(logical_parts[i],source,sizeof(source))<0||stat(source,&st)<0){if(!strcmp(logical_parts[i],"vendor_dlkm")){note("HMOS vendor_dlkm not in super; alias skipped");continue;}return -1;}
        const char *actual=strrchr(source,'/')+1+5; /* skip hmos_, preserve real LP name */
        snprintf(dest,sizeof(dest),"/hmos-root/dev/block/mapper/%s",actual);
        if(mknod(dest,S_IFBLK|0600,st.st_rdev)<0&&errno!=EEXIST)return -1;
        snprintf(alias,sizeof(alias),"/hmos-root/dev/block/by-name/%s",logical_parts[i]);
        snprintf(link,sizeof(link),"/dev/block/mapper/%s",actual);
        if(symlink(link,alias)<0&&errno!=EEXIST)return -1;
        note("HMOS logical alias %s -> %s (read-only DM)",logical_parts[i],actual);
    }}
    char misc[4096];if(readfile("/proc/misc",misc,sizeof(misc))>0){char *save,*line=strtok_r(misc,"\n",&save);while(line){unsigned minor;char name[128];if(sscanf(line,"%u %127s",&minor,name)==2&&(!strcmp(name,"binder")||!strcmp(name,"hwbinder")||!strcmp(name,"vndbinder"))){char target[256];snprintf(target,sizeof(target),"/hmos-root/dev/%s",name);if(mknod(target,S_IFCHR|0666,makedev(10,minor))<0&&errno!=EEXIST)return -1;note("HMOS stock Android IPC node %s minor=%u; OHOS extensions not assumed",name,minor);}line=strtok_r(NULL,"\n",&save);}}
    if(access("/sys/module/dipper_hmos_ipc",F_OK)==0&&ipc_nodes("/hmos-root/dev")!=4)return -1;
    /* Replace the diagnostic sysfs mount with the flags expected by init. */
    umount2("/sys/kernel/debug",MNT_DETACH);umount2("/sys",MNT_DETACH);
    if(mount("sysfs","/sys","sysfs",0,"")<0)return -1;
    if(mkdirs("/sys/fs/selinux")<0||mount("selinuxfs","/sys/fs/selinux","selinuxfs",0,"")<0)return -1;
    const char *binds[]={"proc","sys","config"};
    for(unsigned i=0;i<sizeof(binds)/sizeof(binds[0]);i++){char source[128],dest[256];snprintf(source,sizeof(source),"/%s",binds[i]);snprintf(dest,sizeof(dest),"/hmos-root/%s",binds[i]);if(mkdirs(dest)<0||mount(source,dest,0,MS_BIND|MS_REC,0)<0)return -1;}
    /* Only explicitly selected core services; no donor hardware autostart. */
    const char *hide[]={"/hmos-root/system/etc/init","/hmos-root/sys_prod/etc/init","/hmos-root/preload/etc/init"};
    for(unsigned i=0;i<sizeof(hide)/sizeof(hide[0]);i++)if(ram_mount(hide[i],"mode=0755,size=1m")<0)return -1;
    char cfg[32768];if(readfile("/hmos-init-debug.cfg",cfg,sizeof(cfg))<0)return -1;
    if(textfile("/hmos-root/system/etc/init.cfg",cfg)<0||textfile("/hmos-root/system/etc/fstab.required","# Physical userdata already mounted by dipper bootstrap; never format here.\n")<0)return -1;
    if(textfile("/hmos-root/system/etc/param/dipper_debug.para","persist.init.debug.loglevel=0\n")<0)return -1;
    char cmdline[32768];if(readfile("/proc/cmdline",cmdline,sizeof(cmdline))<0)return -1;
    size_t n=strlen(cmdline);if(n+150>=sizeof(cmdline))return -1;strcat(cmdline," ohos.boot.hardware=dipper ohos.boot.selinux=permissive androidboot.selinux=permissive\n");
    if(textfile("/hmos-ram/cmdline",cmdline)<0||mount("/hmos-ram/cmdline","/hmos-root/proc/cmdline",0,MS_BIND,0)<0)return -1;
    char bootconfig[32768];ssize_t z=readfile("/proc/bootconfig",bootconfig,sizeof(bootconfig));if(z<0)bootconfig[0]=0;
    if(strlen(bootconfig)+150<sizeof(bootconfig)){strcat(bootconfig,"\nohos.boot.hardware = \"dipper\"\nohos.boot.selinux = \"permissive\"\n");if(textfile("/hmos-ram/bootconfig",bootconfig)<0||mount("/hmos-ram/bootconfig","/hmos-root/proc/bootconfig",0,MS_BIND,0)<0)return -1;}
    put("/sys/fs/selinux/enforce","0");
    note("HMOS debug root prepared: RAM overlays, direct whole-partition ext4 userdata, RAM metadata");return 0;
}
static void trace_string(uint64_t address,char *out,size_t cap) {
    size_t n=0;
    while(n+1<cap){errno=0;long word=ptrace(PTRACE_PEEKDATA,traced_pid,(void*)(uintptr_t)(address+n),0);if(word==-1&&errno)break;
        for(size_t j=0;j<sizeof(word)&&n+1<cap;j++){char c=((char*)&word)[j];out[n++]=c;if(!c)return;}
    }out[n]=0;
}
struct trace_context {
    pid_t pid;int held,enter,allow_exit,syscalls,pending_kmsg,kmsg_fd,pending_enforce,enforce_fd;long last_nr;
};
/* A stopped child may be reported before its parent's fork event. Keep its
 * own state immediately; never park it waiting for another waitpid result.
 * The former 32-slot table also lost later child stops when all slots filled. */
struct trace_follower {struct trace_context state;pid_t key;int announced;struct trace_follower *next;};
static struct trace_follower *trace_followers;
static struct trace_follower *trace_follower_get(pid_t pid) {
    for(struct trace_follower *f=trace_followers;f;f=f->next)if(f->key==pid)return f;
    struct trace_follower *f=calloc(1,sizeof(*f));if(!f)return NULL;
    f->key=pid;f->state=(struct trace_context){.pid=pid,.enter=1,.allow_exit=1,.syscalls=!(trace_hmos&&access("/hmos-native-hilog",F_OK)==0),.kmsg_fd=-1,.enforce_fd=-1,.last_nr=SYS_clone};
    f->next=trace_followers;trace_followers=f;return f;
}
static void trace_followers_collect(void) {
    struct trace_follower **link=&trace_followers;
    while(*link){struct trace_follower *f=*link;if(!f->state.pid&&f->announced){*link=f->next;free(f);}else link=&f->next;}
}
static void trace_save(struct trace_context *c) {
    *c=(struct trace_context){.pid=traced_pid,.held=trace_held,.enter=trace_enter,.allow_exit=trace_allow_exit,.syscalls=trace_syscalls,.pending_kmsg=trace_pending_kmsg,.kmsg_fd=trace_kmsg_fd,.pending_enforce=trace_pending_enforce,.enforce_fd=trace_enforce_fd,.last_nr=trace_last_nr};
}
static void trace_restore(const struct trace_context *c) {
    traced_pid=c->pid;trace_held=c->held;trace_enter=c->enter;trace_allow_exit=c->allow_exit;trace_syscalls=c->syscalls;trace_pending_kmsg=c->pending_kmsg;trace_kmsg_fd=c->kmsg_fd;trace_pending_enforce=c->pending_enforce;trace_enforce_fd=c->enforce_fd;trace_last_nr=c->last_nr;
}
static int trace_followers_active(void) {for(struct trace_follower *f=trace_followers;f;f=f->next)if(f->state.pid>0&&!f->state.held&&f->state.syscalls)return 1;return 0;}
static void trace_process(int status) {
    if(WIFEXITED(status)||WIFSIGNALED(status)){note("PTRACED process ended pid=%ld status=%d",(long)traced_pid,status);trace_result=status;traced_pid=0;return;}
    if(!WIFSTOPPED(status))return;
    int sig=WSTOPSIG(status);unsigned event=(unsigned)status>>16;
    if(event==PTRACE_EVENT_FORK){unsigned long child=0;ptrace(PTRACE_GETEVENTMSG,traced_pid,0,&child);
        struct trace_follower *f=child?trace_follower_get((pid_t)child):NULL;if(f)f->announced=1;
        note("PTRACED FORK parent=%ld child=%lu followed=%d",(long)traced_pid,child,f!=NULL);
    }
    else if(event==PTRACE_EVENT_EXEC){note("PTRACED EXEC completed pid=%ld",(long)traced_pid);trace_enter=0;trace_kmsg_fd=-1;trace_enforce_fd=-1;if(trace_allow_exit&&trace_hmos&&access("/hmos-native-hilog",F_OK)==0){trace_syscalls=0;note("Native service pid=%ld now uses event/signal tracing plus original hilogd",(long)traced_pid);}}
    else if(event==PTRACE_EVENT_EXIT){unsigned long message=0;ptrace(PTRACE_GETEVENTMSG,traced_pid,0,&message);note("PTRACED EXIT event pid=%ld status=%lu",(long)traced_pid,message);if(!trace_allow_exit){trace_held=1;note("PTRACED HOLD before PID 1 exit; USB recovery command remains available");return;}}
    else if(sig==(SIGTRAP|0x80)) {
#ifdef __aarch64__
        struct user_pt_regs regs;struct iovec io={.iov_base=&regs,.iov_len=sizeof(regs)};
        struct __ptrace_syscall_info callinfo;
        if(ptrace(PTRACE_GET_SYSCALL_INFO,traced_pid,(void*)sizeof(callinfo),&callinfo)>0){if(callinfo.op==PTRACE_SYSCALL_INFO_ENTRY)trace_enter=1;else if(callinfo.op==PTRACE_SYSCALL_INFO_EXIT)trace_enter=0;}
        if(ptrace(PTRACE_GETREGSET,traced_pid,(void*)(uintptr_t)NT_PRSTATUS,&io)==0){long nr=(long)regs.regs[8];char a[512]="",b[256]="";
            if(trace_enter){trace_last_nr=nr;trace_pending_kmsg=0;trace_pending_enforce=0;
                if(nr==SYS_openat){trace_string(regs.regs[1],a,sizeof(a));trace_pending_kmsg=!strcmp(a,"/dev/kmsg");trace_pending_enforce=!strcmp(a,"/sys/fs/selinux/enforce");note("SYSCALL pid=%ld enter openat %s",(long)traced_pid,a);}
                else if(nr==SYS_execve){trace_string(regs.regs[0],a,sizeof(a));note("SYSCALL pid=%ld enter execve %s",(long)traced_pid,a);}
                else if(nr==SYS_prctl||nr==SYS_ioctl)note("SYSCALL pid=%ld enter nr=%ld args=%llu,%llu,%llu",(long)traced_pid,nr,(unsigned long long)regs.regs[0],(unsigned long long)regs.regs[1],(unsigned long long)regs.regs[2]);
                else if(nr==SYS_mount){trace_string(regs.regs[0],a,sizeof(a));trace_string(regs.regs[1],b,sizeof(b));note("SYSCALL enter mount %s -> %s flags=%llu",a,b,(unsigned long long)regs.regs[3]);}
                else if(nr==SYS_msync&&trace_msync_count++<4)note("SYSCALL msync sample addr=%llu length=%llu flags=%llu",(unsigned long long)regs.regs[0],(unsigned long long)regs.regs[1],(unsigned long long)regs.regs[2]);
                else if(nr==SYS_clone)note("SYSCALL clone flags=%llu",(unsigned long long)regs.regs[0]);
                else if(nr==SYS_close){if((int)regs.regs[0]==trace_enforce_fd)trace_enforce_fd=-1;if((int)regs.regs[0]==trace_kmsg_fd)trace_kmsg_fd=-1;}
                else if(nr==SYS_write&&trace_hmos&&(int)regs.regs[0]==trace_enforce_fd){errno=0;long w=ptrace(PTRACE_PEEKDATA,traced_pid,(void*)(uintptr_t)regs.regs[1],0);if(!errno&&((char*)&w)[0]=='1'){((char*)&w)[0]='0';if(ptrace(PTRACE_POKEDATA,traced_pid,(void*)(uintptr_t)regs.regs[1],(void*)w)==0)note("HMOS debug: retaining SELinux permissive during native-init observation");}}
                else if(nr==SYS_write&&((int)regs.regs[0]==trace_kmsg_fd||regs.regs[0]==1||regs.regs[0]==2)){trace_string(regs.regs[1],a,sizeof(a));size_t len=(size_t)regs.regs[2];if(len<sizeof(a))a[len]=0;note("NATIVE pid=%ld write fd=%llu %s",(long)traced_pid,(unsigned long long)regs.regs[0],a);if(trace_hmos&&strstr(a,"HMOS_CORE_START_DONE")){trace_syscalls=0;note("Native core start command completed; switching PID1 to event/signal tracing; service forks remain traced");}}
                else if(nr==SYS_reboot&&(trace_hmos||!trace_allow_exit)){trace_string(regs.regs[3],a,sizeof(a));trace_held=1;note("PTRACED HOLD pid=%ld before reboot cmd=%llu reason=%s",(long)traced_pid,(unsigned long long)regs.regs[2],a);return;}
                else if(nr==SYS_exit_group||nr==SYS_exit){note("SYSCALL pid=%ld enter exit nr=%ld code=%llu",(long)traced_pid,nr,(unsigned long long)regs.regs[0]);}
            }else{
                long result=(long)regs.regs[0];if(trace_last_nr==SYS_openat&&trace_pending_kmsg&&result>=0)trace_kmsg_fd=(int)result;if(trace_last_nr==SYS_openat&&trace_pending_enforce&&result>=0)trace_enforce_fd=(int)result;
                if(trace_last_nr==SYS_openat||trace_last_nr==SYS_mount||trace_last_nr==SYS_execve||trace_last_nr==SYS_clone||trace_last_nr==SYS_prctl||trace_last_nr==SYS_ioctl||(result<0&&trace_last_nr!=SYS_msync&&trace_last_nr!=SYS_madvise))note("SYSCALL pid=%ld exit nr=%ld result=%ld",(long)traced_pid,trace_last_nr,result);
            }trace_enter=!trace_enter;
        }else note("PTRACE_GETREGSET failed: %s",strerror(errno));
#endif
    }else if(sig==SIGABRT||sig==SIGSEGV||sig==SIGBUS||sig==SIGILL||sig==SIGSYS||sig==SIGFPE){note("PTRACED fatal pid=%ld signal=%d",(long)traced_pid,sig);
#ifdef __aarch64__
        struct user_pt_regs fatal_regs;struct iovec fatal_io={.iov_base=&fatal_regs,.iov_len=sizeof(fatal_regs)};
        if(ptrace(PTRACE_GETREGSET,traced_pid,(void*)(uintptr_t)NT_PRSTATUS,&fatal_io)==0)
            note("FATAL_REGS pid=%ld pc=%llx sp=%llx lr=%llx fp=%llx",(long)traced_pid,(unsigned long long)fatal_regs.pc,(unsigned long long)fatal_regs.sp,(unsigned long long)fatal_regs.regs[30],(unsigned long long)fatal_regs.regs[29]);
        siginfo_t fatal_info;if(ptrace(PTRACE_GETSIGINFO,traced_pid,0,&fatal_info)==0)
            note("FATAL_SIGINFO code=%d address=%p",fatal_info.si_code,fatal_info.si_addr);
        char fatal_maps[64];snprintf(fatal_maps,sizeof(fatal_maps),"/proc/%ld/maps",(long)traced_pid);dump(fatal_maps);
#endif
        if(!trace_allow_exit){trace_held=1;return;}if(ptrace(PTRACE_SYSCALL,traced_pid,0,(void*)(uintptr_t)sig)<0)note("ptrace deliver fatal: %s",strerror(errno));return;}
    else if(sig!=SIGTRAP&&sig!=SIGSTOP){note("PTRACED signal pid=%ld signal=%d",(long)traced_pid,sig);if(ptrace(trace_syscalls?PTRACE_SYSCALL:PTRACE_CONT,traced_pid,0,(void*)(uintptr_t)sig)<0)note("ptrace resume signal: %s",strerror(errno));return;}
    if(ptrace(trace_syscalls?PTRACE_SYSCALL:PTRACE_CONT,traced_pid,0,0)<0)note("ptrace resume failed: %s",strerror(errno));
}
static void trace_poll(void) {
    // 有界处理已就绪状态，保留主进程 HOLD 与逐子进程上下文
    for(unsigned count=0;count<32&&traced_pid>0&&!trace_held;count++) {
        int status;pid_t r=waitpid(-1,&status,__WALL|WNOHANG);if(r<=0)return;
        if(command_finish(r,status))continue;
        if(r==traced_pid){trace_process(status);trace_followers_collect();continue;}
        struct trace_follower *f=NULL;
        for(struct trace_follower *it=trace_followers;it;it=it->next)if(it->key==r){f=it;break;}
        if(!f&&WIFSTOPPED(status)){f=trace_follower_get(r);note("PTRACED early child stop pid=%ld tracked=%d; dispatching immediately",(long)r,f!=NULL);}
        if(f){struct trace_context root;trace_save(&root);trace_restore(&f->state);trace_process(status);trace_save(&f->state);trace_restore(&root);trace_followers_collect();continue;}
        if(WIFSTOPPED(status)){if(ptrace(PTRACE_DETACH,r,0,0)<0)note("ptrace detach untracked child: %s",strerror(errno));}
        else note("Non-traced child reaped pid=%ld status=%d",(long)r,status);
    }
}
static int framebuffer_probe(int draw) {
    if(geteuid()!=0)return 2;
    if(node_from_sys("/sys/class/graphics/fb0","/dev/hmos-fb0",S_IFCHR)<0){perror("fb0 node");return 1;}
    int fd=open("/dev/hmos-fb0",O_RDWR|O_CLOEXEC);if(fd<0){perror("open fb0");return 1;}
    struct fb_fix_screeninfo fixed;struct fb_var_screeninfo var;
    if(ioctl(fd,FBIOGET_FSCREENINFO,&fixed)<0||ioctl(fd,FBIOGET_VSCREENINFO,&var)<0){perror("framebuffer info");close(fd);return 1;}
    printf("FB id=%.16s visible=%ux%u virtual=%ux%u offset=%u,%u bpp=%u pitch=%u bytes=%u\n",fixed.id,var.xres,var.yres,var.xres_virtual,var.yres_virtual,var.xoffset,var.yoffset,var.bits_per_pixel,fixed.line_length,fixed.smem_len);
    if(!draw){close(fd);return 0;}
    if(!var.xres||!var.yres||var.xres>8192||var.yres>8192||(var.bits_per_pixel!=16&&var.bits_per_pixel!=24&&var.bits_per_pixel!=32)||fixed.smem_len>128*1024*1024U){close(fd);return 2;}
    unsigned bytes=var.bits_per_pixel/8;uint64_t end=(uint64_t)(var.yoffset+var.yres-1)*fixed.line_length+(uint64_t)(var.xoffset+var.xres)*bytes;
    if(end>fixed.smem_len){close(fd);return 2;}
    unsigned char *map=mmap(NULL,fixed.smem_len,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);if(map==MAP_FAILED){perror("mmap fb");close(fd);return 1;}
    static const unsigned colors[]={0xffffff,0xffff00,0x00ffff,0x00ff00,0xff00ff,0xff0000,0x0000ff,0x000000};
    struct fb_bitfield fields[]={var.red,var.green,var.blue,var.transp};
    for(unsigned i=0;i<4;i++)if(fields[i].length>16||fields[i].offset+fields[i].length>var.bits_per_pixel){munmap(map,fixed.smem_len);close(fd);return 2;}
    for(unsigned y=0;y<var.yres;y++)for(unsigned x=0;x<var.xres;x++){
        unsigned rgb=colors[(uint64_t)x*8/var.xres],values[]={rgb>>16,(rgb>>8)&255,rgb&255,255};uint32_t pixel=0;
        for(unsigned i=0;i<4;i++)if(fields[i].length)pixel|=((values[i]*((1U<<fields[i].length)-1)/255)<<fields[i].offset);
        memcpy(map+(uint64_t)(var.yoffset+y)*fixed.line_length+(uint64_t)(var.xoffset+x)*bytes,&pixel,bytes);
    }
    msync(map,fixed.smem_len,MS_SYNC);int blank=ioctl(fd,FBIOBLANK,FB_BLANK_UNBLANK);var.activate=FB_ACTIVATE_NOW;int pan=ioctl(fd,FBIOPAN_DISPLAY,&var);
    printf("FB color bars written; unblank=%d pan=%d (physical visibility still requires observation)\n",blank,pan);
    munmap(map,fixed.smem_len);close(fd);return pan<0?1:0;
}
static int trace_selftest(int seize,int stress) {
    int ready[2];if(pipe2(ready,O_CLOEXEC)<0)return 1;
    pid_t child=fork();if(child<0)return 1;
    if(child==0){close(ready[1]);if(seize){char ack;ssize_t n;do{n=read(ready[0],&ack,1);}while(n<0&&errno==EINTR);if(n!=1)_exit(8);}else{if(ptrace(PTRACE_TRACEME,0,0,0)<0)_exit(2);raise(SIGSTOP);}close(ready[0]);allwrite(1,"ptrace child test\n",18);int count=stress?96:1;for(int i=0;i<count;i++){pid_t nested=fork();if(nested<0)_exit(3);if(nested==0){if(stress)usleep(200000);_exit(7);}}for(int i=0;i<count;i++){int result;if(waitpid(-1,&result,0)<=0||!WIFEXITED(result)||WEXITSTATUS(result)!=7)_exit(4);}_exit(0);}
    unsigned long options=PTRACE_O_TRACESYSGOOD|PTRACE_O_TRACEEXEC|PTRACE_O_TRACEEXIT|PTRACE_O_TRACEFORK;
    close(ready[0]);if(seize&&(ptrace(PTRACE_SEIZE,child,0,(void*)options)<0||ptrace(PTRACE_INTERRUPT,child,0,0)<0))return 3;
    int status;if(waitpid(child,&status,__WALL)!=child||!WIFSTOPPED(status))return 2;
    if(!seize&&ptrace(PTRACE_SETOPTIONS,child,0,(void*)options)<0)return 3;
    if(seize){allwrite(ready[1],"R",1);}close(ready[1]);
    traced_pid=child;trace_allow_exit=1;trace_enter=1;ptrace(PTRACE_SYSCALL,child,0,0);
    while(traced_pid>0){trace_poll();usleep(1000);}history_write(1);return trace_result!=0;
}
static void trace_handoff(int kind) {
    if(getpid()!=1)return;
    if(kind!=0&&access("/sys/module/dipper_hmos_ipc",F_OK)<0){note("HMOS handoff rejected: load the verified IPC module first with ipc");return;}
#if DIPPER_IPC_ONEWAY_PID
    if(kind!=0&&!ipc_pair_loaded){note("HMOS handoff rejected: verified retained-mode IPC pair is required; HOLD");return;}
#endif
    if(logfd<0&&serialfd<0){note("TRACE ABORT: neither metadata nor USB log available; HOLD");return;}
    note("TRACE preparing collector handshake before original Android init");
    if(logfd>=0)fsync(logfd);
    int ready[2];
    if(pipe2(ready,O_CLOEXEC)<0){note("TRACE pipe failed: %s; HOLD",strerror(errno));return;}
    trace_hmos=kind!=0;pid_t child=fork();
    if(child>0){close(ready[1]);struct pollfd p={.fd=ready[0],.events=POLLIN};char ack=0;int ok=poll(&p,1,3000)>0&&read(ready[0],&ack,1)==1&&ack=='R';close(ready[0]);
        if(ok){
            if(kind!=0){
                if(hmos_root()<0){note("HMOS root preparation failed: %s; HOLD",strerror(errno));return;}
                if(kind==2){
                    pid_t boot=fork();
                    if(boot<0){note("AUTOSTART fork failed: %s; HOLD",strerror(errno));return;}
                    if(boot==0){
                        if(setsid()<0)_exit(126);
                        /* PID1 may have opened its logger/watchdog on fd 0..2.
                         * Close inherited copies BEFORE opening new stdio.
                         * Do not close logger descriptor numbers after dup2. */
                        for(int fd=0;fd<1024;fd++)close(fd);
                        int input=open("/dev/null",O_RDONLY);
                        int output=open("/hmos-root/data/local/tmp/v116-autostart.txt",O_CREAT|O_TRUNC|O_WRONLY,0600);
                        if(input!=0||output!=1||dup2(output,2)!=2)_exit(126);
                        dprintf(1,"AUTOSTART_STDIO_READY input=0 output=1 error=2\n");
                        execl("/busybox","busybox","timeout","900","/busybox","sh","/graphics/start-native-desktop-v116.sh",(char*)0);
                        _exit(127);
                    }
                    note("AUTOSTART bootstrap child=%ld; bounded=900s; original init follows",(long)boot);
                }
                int out=open("/hmos-root/dev/kmsg",O_WRONLY|O_CLOEXEC);if(out>=0){dup2(out,1);dup2(out,2);close(out);}
                if(chroot("/hmos-root")<0||chdir("/")<0){note("HMOS chroot failed: %s; HOLD",strerror(errno));return;}
                clearenv();setenv("PATH","/system/bin:/vendor/bin",1);
                if(kind==1){char *args[]={"/system/lib/ld-musl-aarch64.so.1","--list","/system/bin/init",NULL};execv(args[0],args);}
                else {char *args[]={"/compat/bin/init","--second-stage",NULL};execv(args[0],args);}
                note("HMOS exec failed: %s; HOLD",strerror(errno));return;
            }
            /* SwitchRoot moves every surviving mount into the new system
             * root. /hmos is diagnostic-only and does not exist in that RO
             * root, so none of these inspection mounts may survive handoff. */
            const char *inspected[]={"preload","vendor_dlkm","sys_prod","vendor","system"};
            for(unsigned j=0;j<sizeof(inspected)/sizeof(inspected[0]);j++){char mp[128];snprintf(mp,sizeof(mp),"/hmos/%s",inspected[j]);if(umount2(mp,MNT_DETACH)<0&&errno!=EINVAL&&errno!=ENOENT)note("TRACE detach %s: %s",mp,strerror(errno));}
            /* Android first-stage mounts sysfs itself with flags 0. Leaving
             * our diagnostic sysfs mount in place makes that call fail EBUSY.
             * The collector keeps open kmsg/serial/log descriptors instead. */
            if(umount2("/sys/kernel/debug",MNT_DETACH)<0&&errno!=EINVAL&&errno!=ENOENT)note("TRACE debugfs detach: %s",strerror(errno));
            if(umount2("/sys",MNT_DETACH)<0)note("TRACE sysfs detach: %s",strerror(errno));
            note("TRACE HANDOFF: collector ready pid=%ld; exec original Android init",(long)child);if(logfd>=0)fsync(logfd);char *args[]={"/init",NULL};execv("/init.android.original",args);note("TRACE exec failed errno=%d %s; HOLD",errno,strerror(errno));}
        else {note("TRACE ABORT: collector did not acknowledge readiness; HOLD");kill(child,SIGTERM);}
    } else if(child<0){close(ready[0]);close(ready[1]);note("TRACE fork failed: %s; HOLD",strerror(errno));}
    else {close(ready[0]);if(setsid()<0)note("collector setsid: %s",strerror(errno));if(setpriority(PRIO_PROCESS,0,-20)<0)note("collector priority: %s",strerror(errno));note("TRACE collector running pid=%ld",(long)getpid());
        pid_t target=getppid();unsigned long options=PTRACE_O_TRACESYSGOOD|PTRACE_O_TRACEEXEC|PTRACE_O_TRACEEXIT|PTRACE_O_TRACEFORK;int status;
        if(ptrace(PTRACE_SEIZE,target,0,(void*)options)<0){note("PTRACE_SEIZE PID 1 failed: %s",strerror(errno));allwrite(ready[1],"E",1);close(ready[1]);return;}
        if(ptrace(PTRACE_INTERRUPT,target,0,0)<0||waitpid(target,&status,__WALL)!=target){note("PTRACE_INTERRUPT failed: %s",strerror(errno));allwrite(ready[1],"E",1);close(ready[1]);return;}
        traced_pid=target;trace_enter=0;note("PTRACE attached PID 1 before handoff");if(logfd>=0)fsync(logfd);allwrite(ready[1],"R",1);close(ready[1]);ptrace(PTRACE_SYSCALL,target,0,0);
    }
}
int main(int argc,char **argv) {
#if DIPPER_IPC_ONEWAY_PID
    if(argc==2&&!strcmp(argv[1],"--ipc-pair-selftest"))return ipc_pair_selftest();
#endif
    if(argc==2&&!strcmp(argv[1],"--fb-info"))return framebuffer_probe(0);
    if(argc==2&&!strcmp(argv[1],"--fb-pattern"))return framebuffer_probe(1);
    if(argc==3&&!strcmp(argv[1],"--load-module")){
        const char *name=argv[2];if(geteuid()!=0||strlen(name)>127||strspn(name,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")!=strlen(name))return 2;
        module_index();loadmod(name,0);history_write(1);for(int i=0;i<nmods;i++)if(same_module(mods[i].name,name))return mods[i].state==2?0:1;return 1;
    }
    if(argc==3&&!strcmp(argv[1],"--inspect-super")) {slot_known=1;if(getenv("HMOS_SLOT")&&!strcmp(getenv("HMOS_SLOT"),"b"))suffix[1]='b';partitions(argv[2],1);history_write(1);return 0;}
    if(argc==2&&!strcmp(argv[1],"--sha-selftest")){uint8_t sum[32];sha256("abc",3,sum);for(int i=0;i<32;i++)printf("%02x",sum[i]);puts("");return 0;}
    if(argc==2&&!strcmp(argv[1],"--fork-selftest")){pid_t p=fork();if(p<0){perror("fork");return 1;}if(p==0){allwrite(1,"child ready\n",12);_exit(0);}int status;if(waitpid(p,&status,0)<0){perror("waitpid");return 1;}printf("parent received child status=%d\n",status);return status!=0;}
    if(argc==2&&!strcmp(argv[1],"--ptrace-selftest"))return trace_selftest(0,0);
    if(argc==2&&!strcmp(argv[1],"--ptrace-seize-selftest"))return trace_selftest(1,0);
    if(argc==2&&!strcmp(argv[1],"--ptrace-stress-selftest"))return trace_selftest(1,1);
    if(getpid()!=1){fprintf(stderr,"This program must be init/PID 1. Use --inspect-super only for offline checks.\n");return 2;}
    umask(0077);signal(SIGPIPE,SIG_IGN);makedir("/dev");makedir("/proc");makedir("/sys");
    mount("tmpfs","/dev","tmpfs",MS_NOSUID,"mode=0755");mount("proc","/proc","proc",MS_NOSUID|MS_NODEV|MS_NOEXEC,"");mount("sysfs","/sys","sysfs",MS_NOSUID|MS_NODEV|MS_NOEXEC,"");
    mknod("/dev/kmsg",S_IFCHR|0600,makedev(1,11));mknod("/dev/null",S_IFCHR|0666,makedev(1,3));
    mknod("/dev/console",S_IFCHR|0600,makedev(5,1));consolefd=open("/dev/console",O_WRONLY|O_NONBLOCK|O_CLOEXEC);
    kmsg=open("/dev/kmsg",O_RDONLY|O_NONBLOCK|O_CLOEXEC);makedir("/dev/block");makedir("/dev/block/by-name");makedir("/dev/mapper");makedir("/metadata");makedir("/hmos");
#ifdef TRACE_ANDROID
    note("dipper HMOS trace v116 -- collect early logs, then execute original Android init");
#else
    note("dipper HMOS probe v121 USB-session recovery payload -- autonomous native startup; vendor read-only; existing isolated data only");
#endif
    slot_detect();drain();discover();
    /* 米 8 没有 super：system/vendor 是静态分区，载荷和 sys_prod/preload 在 cust 上 */
    if(static_layout_mount()<0)note("STATIC LAYOUT INCOMPLETE: boot continues only for diagnostics");
    persistent();loadmodules();
    for(int i=0;i<50;i++){discover();persistent();drain();watchdog();usb_setup();serial_tick();usleep(100000);}
    char misc[2048];if(readfile("/proc/misc",misc,sizeof(misc))>0){char *save,*p=strtok_r(misc,"\n",&save);while(p){unsigned n;char name[128];if(sscanf(p,"%u %127s",&n,name)==2&&!strcmp(name,"device-mapper"))mknod("/dev/mapper/control",S_IFCHR|0600,makedev(10,n));p=strtok_r(NULL,"\n",&save);}}
    snapshots();drain();
#ifdef TRACE_ANDROID
    trace_handoff(0);
#else
    note("AUTOSTART loading verified IPC module before native init");
    ipc_load();trace_requested=3;
#endif
    time_t flush_second=-1;for(;;){drain();watchdog();hilog_tick();serial_tick();command_tick();ipc_test_tick();if(ipc_requested){ipc_requested=0;if(getpid()==1&&!trace_hmos)ipc_load();else note("IPC LOAD rejected after OS handoff");}if(ipc_test_requested){int which=ipc_test_requested;ipc_test_requested=0;if(getpid()==1&&!trace_hmos)ipc_test(which);else note("IPC contracts only run before native OS handoff");}if(trace_requested&&getpid()==1&&ipc_pid==0&&ipc_test_pid==0&&command_pid==0){int kind=trace_requested-1;trace_requested=0;trace_handoff(kind);}if(traced_pid>0)trace_poll();else reap_children();struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);if(ts.tv_sec!=flush_second){flush_second=ts.tv_sec;runtime_persistent();if(traced_pid<=0){discover();persistent();usb_setup();}if(logfd>=0)fsync(logfd);}struct pollfd p={.fd=kmsg,.events=POLLIN};struct timespec delay={.tv_nsec=traced_pid>0&&!trace_held&&(trace_syscalls||trace_followers_active())?100000:5000000};ppoll(&p,1,&delay,NULL);}
}
