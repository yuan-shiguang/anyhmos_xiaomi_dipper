/* SPDX-License-Identifier: MIT
 * Isolated original303 Bionic/PAL PCM service. No vendor files are changed.
 * The diagnostic entry runs after Bionic constructors, outside loader locks.
 */
#include <sys/types.h>
#include <PalApi.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/time.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <sched.h>
#include "protocol.h"

#define MAX_CLIENTS 8
extern int *__errno(void);
static int (*stream_open)(struct pal_stream_attributes *, unsigned, struct pal_device *,
    unsigned, struct modifier_kv *, pal_stream_callback, unsigned long, pal_stream_handle_t **);
static int (*stream_close)(pal_stream_handle_t *);
static int (*stream_start)(pal_stream_handle_t *);
static int (*stream_stop)(pal_stream_handle_t *);
static int (*stream_pause)(pal_stream_handle_t *);
static int (*stream_resume)(pal_stream_handle_t *);
static int (*stream_write)(pal_stream_handle_t *, struct pal_buffer *);
static int (*stream_volume)(pal_stream_handle_t *, struct pal_volume_data *);
static int (*stream_buffer)(pal_stream_handle_t *, pal_buffer_config_t *, pal_buffer_config_t *);
static ssize_t (*stream_read)(pal_stream_handle_t *, struct pal_buffer *);
static int (*stream_set_mute)(pal_stream_handle_t *, bool);
static int (*stream_get_mute)(pal_stream_handle_t *, bool *);
static int (*mic_set_mute)(bool);
static int (*mic_get_mute)(bool *);
static int (*get_param)(uint32_t,void **,size_t *,void *);
static int (*register_global)(pal_global_callback,uint64_t);
static volatile sig_atomic_t stopping;

int __android_log_print(int priority, const char *tag, const char *fmt, ...)
{
    static unsigned count;
    if (__atomic_fetch_add(&count,1,__ATOMIC_RELAXED)>=1500) return 0;
    char b[768];
    int n=snprintf(b,sizeof(b),"PAL_LOG_%d_%s: ",priority,tag?tag:"?");
    if (n<0 || n>=(int)sizeof(b)) return 0;
    va_list ap; va_start(ap,fmt);
    int m=vsnprintf(b+n,sizeof(b)-(size_t)n,fmt,ap);
    va_end(ap);
    if (m<0) return 0;
    size_t used=(size_t)n+(size_t)m;
    if (used>sizeof(b)-2) used=sizeof(b)-2;
    b[used++]='\n';
    (void)write(1,b,used);
    return m;
}

struct client {
    int fd;
    pal_stream_handle_t *stream;
    int state; /* 0 closed, 1 open, 2 running, 3 paused */
    uint64_t frames;
    uint64_t nonzero_samples;
    unsigned peak;
    int capture;
    uint64_t capture_epoch;
};
static void end_signal(int sig) { (void)sig; __atomic_store_n(&stopping,1,__ATOMIC_RELEASE); }

static int close_stream(struct client *c)
{
    if (!c->stream) return 0;
    int rc=0;
    if (c->state>=2) rc=stream_stop(c->stream);
    int crc=stream_close(c->stream);
    printf("PAL_SERVER_PCM_SUMMARY frames=%llu nonzero_samples=%llu peak_s16=%u stop_rc=%d close_rc=%d\n",
        (unsigned long long)c->frames,(unsigned long long)c->nonzero_samples,c->peak,rc,crc);
    c->stream=0; c->state=0;
    return rc ? rc : crc;
}

static int open_stream(struct client *c, unsigned rate, unsigned channels)
{
    if (c->stream) return -16;
    if (rate!=48000 || channels!=2) return -95;
    struct { struct pal_stream_attributes value; unsigned char tail[128]; } a={0};
    struct { struct pal_device value; unsigned char tail[128]; } d={0};
    a.value.type=PAL_STREAM_LOW_LATENCY;
    a.value.direction=PAL_AUDIO_OUTPUT;
    a.value.out_media_config.sample_rate=rate;
    a.value.out_media_config.bit_width=16;
    a.value.out_media_config.aud_fmt_id=PAL_AUDIO_FMT_PCM_S16_LE;
    a.value.out_media_config.ch_info.channels=2;
    a.value.out_media_config.ch_info.ch_map[0]=PAL_CHMAP_CHANNEL_FL;
    a.value.out_media_config.ch_info.ch_map[1]=PAL_CHMAP_CHANNEL_FR;
    d.value.id=PAL_DEVICE_OUT_SPEAKER;
    d.value.config=a.value.out_media_config;
    int rc=stream_open(&a.value,1,&d.value,0,0,0,0,&c->stream);
    if (rc) return rc;
    if (!c->stream) return -5;
    c->state=1; c->frames=0; c->nonzero_samples=0; c->peak=0;
    pal_buffer_config_t out={.buf_count=2,.buf_size=960};
    rc=stream_buffer(c->stream,0,&out);
    if (!rc) {
        struct { unsigned count; struct pal_channel_vol_kv pair; } v={1,{3,0.03f}};
        rc=stream_volume(c->stream,(struct pal_volume_data *)&v);
    }
    if (rc) (void)close_stream(c);
    return rc;
}

#include "pal_capture_backend.inc"
#include "pal_capture_worker.inc"

static int dispatch(struct client *c, const struct dipper_pal_request *h,
                    unsigned char *data, uint32_t *written)
{
    if (h->op==DIPPER_PAL_OPEN) return open_stream(c,h->arg0,h->arg1);
    if (h->op==DIPPER_PAL_CLOSE) return close_stream(c);
    if (h->op==DIPPER_PAL_STATUS) return 0;
    if (!c->stream) return -19;
    int rc;
    switch (h->op) {
    case DIPPER_PAL_START:
        if (c->state==2) return 0;
        if (c->state!=1) return -22;
        rc=stream_start(c->stream);
        if (!rc) c->state=2;
        return rc;
    case DIPPER_PAL_STOP:
        if (c->state==1) return 0;
        rc=stream_stop(c->stream);
        if (!rc) c->state=1;
        return rc;
    case DIPPER_PAL_PAUSE:
        if (c->state!=2) return -22;
        rc=stream_pause(c->stream);
        if (!rc) c->state=3;
        return rc;
    case DIPPER_PAL_RESUME:
        if (c->state!=3) return -22;
        rc=stream_resume(c->stream);
        if (!rc) c->state=2;
        return rc;
    case DIPPER_PAL_VOLUME: {
        if (h->arg0>1000000) return -22;
        struct { unsigned count; struct pal_channel_vol_kv pair; } v={1,{3,h->arg0/1000000.0f}};
        return stream_volume(c->stream,(struct pal_volume_data *)&v);
    }
    case DIPPER_PAL_WRITE:
        if (c->state!=2 || !h->bytes || (h->bytes&3)) return -22;
        while (*written<h->bytes) {
            unsigned n=h->bytes-*written;
            if (n>960) n=960;
            struct { struct pal_buffer value; unsigned char tail[64]; } b={0};
            b.value.buffer=data+*written;
            b.value.size=n;
            b.value.alloc_info.alloc_handle=-1;
            rc=stream_write(c->stream,&b.value);
            if (rc<0) return rc;
            if (!rc || (unsigned)rc>n || (rc&3)) return -5;
            for (int i=0;i<rc;i+=2) {
                int value=(int16_t)((unsigned)data[*written+(unsigned)i] |
                    ((unsigned)data[*written+(unsigned)i+1]<<8));
                unsigned magnitude=(unsigned)(value<0?-value:value);
                if (magnitude) c->nonzero_samples++;
                if (magnitude>c->peak) c->peak=magnitude;
            }
            if (!c->frames) printf("PAL_SERVER_FIRST_PCM_WRITE bytes=%d\n",rc);
            *written+=(unsigned)rc;
            c->frames+=(unsigned)rc/4;
        }
        return 0;
    default: return -95;
    }
}

static int serve(void)
{
    signal(SIGTERM,end_signal); signal(SIGINT,end_signal); signal(SIGPIPE,SIG_IGN);
    int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);
    if (fd<0) return -(*__errno());
    struct sockaddr_un a={.sun_family=AF_UNIX};
    memcpy(a.sun_path+1,DIPPER_PAL_SOCKET,sizeof(DIPPER_PAL_SOCKET)-1);
    socklen_t len=offsetof(struct sockaddr_un,sun_path)+sizeof(DIPPER_PAL_SOCKET);
    if (bind(fd,(void *)&a,len) || listen(fd,MAX_CLIENTS)) {
        int rc=-(*__errno()); close(fd); return rc;
    }
    struct client cs[MAX_CLIENTS];
    for (int i=0;i<MAX_CLIENTS;i++) cs[i]=(struct client){.fd=-1};
    struct { struct dipper_pal_request h; unsigned char data[DIPPER_PAL_MAX_DATA]; } packet;
    printf("PAL_SERVER_READY=1 pid=%d socket=@%s\n",getpid(),DIPPER_PAL_SOCKET);
    fflush(stdout);
    while (!__atomic_load_n(&stopping,__ATOMIC_ACQUIRE)) {
        if (capture_watchdog_expired()) {
            _exit(88);
        }
        struct pollfd fds[MAX_CLIENTS+1]={{fd,POLLIN,0}};
        for (int i=0;i<MAX_CLIENTS;i++) fds[i+1]=(struct pollfd){cs[i].fd,POLLIN,0};
        if (poll(fds,MAX_CLIENTS+1,200)<0) continue;
        if (fds[0].revents&POLLIN) {
            int nfd=accept4(fd,0,0,SOCK_CLOEXEC);
            if (nfd>=0) {
                struct ucred cred={0}; socklen_t n=sizeof(cred);
                /* Verified donor /system/etc/passwd: audio1041/audio_host3127. */
                if (getsockopt(nfd,SOL_SOCKET,SO_PEERCRED,&cred,&n) ||
                    (cred.uid!=0 && cred.uid!=1041 && cred.uid!=3127)) close(nfd);
                else {
                    struct timeval send_timeout={1,0};
                    if (setsockopt(nfd,SOL_SOCKET,SO_SNDTIMEO,&send_timeout,sizeof(send_timeout))) {
                        close(nfd); continue;
                    }
                    int slot;
                    for (slot=0;slot<MAX_CLIENTS && cs[slot].fd>=0;slot++) {}
                    if (slot==MAX_CLIENTS) close(nfd);
                    else cs[slot]=(struct client){.fd=nfd};
                }
            }
        }
        for (int i=0;i<MAX_CLIENTS;i++) {
            struct client *c=&cs[i];
            if (c->fd<0 || !fds[i+1].revents) continue;
            ssize_t n=recv(c->fd,&packet,sizeof(packet),MSG_DONTWAIT|MSG_TRUNC);
            if (n<=0) { (void)close_client_stream(c); close(c->fd); c->fd=-1; continue; }
            if ((size_t)n<sizeof(packet.h) || packet.h.magic!=DIPPER_PAL_MAGIC ||
                packet.h.bytes>DIPPER_PAL_MAX_DATA || (size_t)n!=sizeof(packet.h)+packet.h.bytes ||
                (packet.h.op!=DIPPER_PAL_WRITE && packet.h.bytes)) {
                (void)close_client_stream(c); close(c->fd); c->fd=-1; continue;
            }
            struct dipper_pal_reply reply={.magic=DIPPER_PAL_MAGIC,.sequence=packet.h.sequence};
            if (packet.h.op>=DIPPER_PAL_CAPTURE_OPEN) {
                reply.rc=route_capture_request(c,&packet.h);
                if (!reply.rc) { c->fd=-1; continue; }
            } else {
                reply.rc=c->capture?-95:dispatch(c,&packet.h,packet.data,&reply.bytes);
                reply.written_frames=c->frames;
            }
            struct iovec response[2]={{&reply,sizeof(reply)},{packet.data,reply.bytes}};
            int capture_data=packet.h.op==DIPPER_PAL_CAPTURE_READ && !reply.rc && reply.bytes;
            struct msghdr message={.msg_iov=response,.msg_iovlen=capture_data?2:1};
            size_t response_size=sizeof(reply)+(capture_data?reply.bytes:0);
            if (sendmsg(c->fd,&message,MSG_NOSIGNAL)!=(ssize_t)response_size) {
                (void)close_client_stream(c); close(c->fd); c->fd=-1;
            }
            if (packet.h.op!=DIPPER_PAL_WRITE || reply.rc)
                printf("PAL_SERVER_REQUEST op=%u rc=%d bytes=%u frames=%llu\n",packet.h.op,
                    reply.rc,reply.bytes,(unsigned long long)reply.written_frames);
        }
    }
    for (int i=0;i<MAX_CLIENTS;i++) if (cs[i].fd>=0) { (void)close_client_stream(&cs[i]); close(cs[i].fd); }
    if (finish_capture_workers()) {
        _exit(88);
    }
    close(fd);
    return 0;
}

static int server_main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    setbuf(stdout,0);
    void *pal=dlopen("libar-pal.so",RTLD_NOW), *agm=dlopen("libagm.so",RTLD_NOW);
    if (!pal || !agm) { printf("PAL_SERVER_DLOPEN_ERROR=%s\n",dlerror()); return 80; }
    int (*agm_init_fn)(void)=dlsym(agm,"agm_init");
    int (*agm_deinit_fn)(void)=dlsym(agm,"agm_deinit");
    int (*pal_init_fn)(void)=dlsym(pal,"pal_init");
    void (*pal_deinit_fn)(void)=dlsym(RTLD_DEFAULT,"pal_deinit");
#define LOAD(field, name) field=dlsym(pal,name); if (!field) return 81
    LOAD(stream_open,"pal_stream_open"); LOAD(stream_close,"pal_stream_close");
    LOAD(stream_start,"pal_stream_start"); LOAD(stream_stop,"pal_stream_stop");
    LOAD(stream_pause,"pal_stream_pause"); LOAD(stream_resume,"pal_stream_resume");
    LOAD(stream_write,"pal_stream_write"); LOAD(stream_volume,"pal_stream_set_volume");
    LOAD(stream_buffer,"pal_stream_set_buffer_size");
#undef LOAD
    /* 输入扩展缺少时保留原播放服务，查询明确返回不支持 */
    stream_read=dlsym(pal,"pal_stream_read");
    stream_set_mute=dlsym(pal,"pal_stream_set_mute");
    stream_get_mute=dlsym(pal,"pal_stream_get_mute");
    mic_set_mute=dlsym(pal,"pal_set_mic_mute");
    mic_get_mute=dlsym(pal,"pal_get_mic_mute");
    get_param=dlsym(pal,"pal_get_param");
    register_global=dlsym(pal,"pal_register_global_callback");
    if (!agm_init_fn || !agm_deinit_fn || !pal_init_fn || !pal_deinit_fn) return 82;
    int rc=agm_init_fn(); printf("PAL_SERVER_AGM_INIT_RC=%d\n",rc);
    if (rc) return 83;
    rc=pal_init_fn(); printf("PAL_SERVER_PAL_INIT_RC=%d\n",rc);
    if (!rc) { capture_monitor_init(); rc=serve(); pal_deinit_fn(); }
    printf("PAL_SERVER_AGM_DEINIT_RC=%d\n",agm_deinit_fn());
    printf("PAL_SERVER_EXIT=%d\n",rc);
    return rc ? 84 : 0;
}

__attribute__((noreturn)) void __libc_init(void *args, void (*onexit)(void),
    int (*original_main)(int,char **,char **), const void *structors)
{
    (void)original_main;
    void (*real)(void *,void (*)(void),int (*)(int,char **,char **),const void *)=dlsym(RTLD_NEXT,"__libc_init");
    if (real) real(args,onexit,server_main,structors);
    _exit(90);
    __builtin_unreachable();
}
