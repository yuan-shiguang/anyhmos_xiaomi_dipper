/* 单个输入工作线程独占输入连接，主循环不等待 PAL 采集读 */
struct capture_worker {
    struct client client;
    struct dipper_pal_request first;
    uint64_t operation_started;
    int input_slot;
    struct capture_worker *next;
};
static struct capture_worker *capture_workers;
static int capture_workers_lock;
static unsigned capture_workers_count;
static unsigned capture_input_count;
_Static_assert(sizeof(pthread_t)==8,"Bionic thread handle ABI");

static uint64_t capture_now_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC,&now)) return 0;
    return (uint64_t)now.tv_sec*1000000000u+(uint64_t)now.tv_nsec;
}
static void capture_list_lock(void)
{
    while (__atomic_exchange_n(&capture_workers_lock,1,__ATOMIC_ACQUIRE)) sched_yield();
}
static void capture_list_unlock(void)
{
    __atomic_store_n(&capture_workers_lock,0,__ATOMIC_RELEASE);
}
static int capture_send_reply(struct client *c, const struct dipper_pal_request *h,
    unsigned char *pcm, struct dipper_pal_reply *reply)
{
    int payload=h->op==DIPPER_PAL_CAPTURE_READ && !reply->rc && reply->bytes;
    struct iovec iov[2]={{reply,sizeof(*reply)},{pcm,reply->bytes}};
    struct msghdr msg={.msg_iov=iov,.msg_iovlen=payload?2:1};
    size_t size=sizeof(*reply)+(payload?reply->bytes:0);
    return sendmsg(c->fd,&msg,MSG_NOSIGNAL)==(ssize_t)size?0:-5;
}
static void *capture_worker_main(void *opaque)
{
    struct capture_worker *w=opaque;
    struct client *c=&w->client;
    struct { struct dipper_pal_request h; unsigned char pcm[DIPPER_PAL_MAX_DATA]; } packet={.h=w->first};
    int first=1;
    for (;;) {
        struct dipper_pal_reply reply={.magic=DIPPER_PAL_MAGIC,.sequence=packet.h.sequence};
        uint64_t began=capture_now_ns();
        __atomic_store_n(&w->operation_started,began,__ATOMIC_RELEASE);
        if (!began) reply.rc=-5;
        else if (!first && packet.h.op==DIPPER_PAL_CAPTURE_OPEN) reply.rc=-16;
        else reply.rc=dispatch_capture(c,&packet.h,packet.pcm,&reply.bytes,&reply.written_frames);
        __atomic_store_n(&w->operation_started,0,__ATOMIC_RELEASE);
        if (capture_send_reply(c,&packet.h,packet.pcm,&reply) ||
            !w->input_slot || packet.h.op==DIPPER_PAL_CAPTURE_CLOSE ||
            (first && packet.h.op==DIPPER_PAL_CAPTURE_OPEN && reply.rc)) break;
        first=0;
        int ready=0;
        while (!__atomic_load_n(&stopping,__ATOMIC_ACQUIRE)) {
            struct pollfd fd={c->fd,POLLIN,0};
            int rc=poll(&fd,1,100);
            if (rc>0) { ready=1; break; }
            if (rc<0 && *__errno()!=4) break;
        }
        if (!ready) break;
        ssize_t received=recv(c->fd,&packet,sizeof(packet),MSG_DONTWAIT|MSG_TRUNC);
        if ((size_t)received<sizeof(packet.h) || received<0 ||
            packet.h.magic!=DIPPER_PAL_MAGIC || packet.h.op<DIPPER_PAL_CAPTURE_OPEN ||
            packet.h.bytes || (size_t)received!=sizeof(packet.h)) break;
    }
    __atomic_store_n(&w->operation_started,capture_now_ns(),__ATOMIC_RELEASE);
    (void)close_capture(c);
    capture_list_lock();
    struct capture_worker **slot=&capture_workers;
    while (*slot && *slot!=w) slot=&(*slot)->next;
    if (*slot) *slot=w->next;
    if (w->input_slot) __atomic_sub_fetch(&capture_input_count,1,__ATOMIC_RELEASE);
    __atomic_sub_fetch(&capture_workers_count,1,__ATOMIC_RELEASE);
    capture_list_unlock();
    close(c->fd); free(w);
    return 0;
}
static int route_capture_request(struct client *c, const struct dipper_pal_request *h)
{
    if (!DIPPER_CAPTURE_EXPERIMENTAL) return -95;
    if (h->op!=DIPPER_PAL_CAPTURE_OPEN && h->op!=DIPPER_PAL_CAPTURE_STATUS &&
        h->op!=DIPPER_PAL_MIC_MUTE && h->op!=DIPPER_PAL_MIC_GET_MUTE) return -19;
    if (c->stream) return -16;
    struct capture_worker *w=calloc(1,sizeof(*w));
    if (!w) return -12;
    w->client.fd=c->fd; w->first=*h; w->input_slot=h->op==DIPPER_PAL_CAPTURE_OPEN;
    capture_list_lock();
    if (capture_workers_count>=MAX_CLIENTS || (w->input_slot && capture_input_count)) {
        capture_list_unlock(); free(w); return -16;
    }
    w->next=capture_workers; capture_workers=w;
    __atomic_add_fetch(&capture_workers_count,1,__ATOMIC_RELEASE);
    if (w->input_slot) __atomic_add_fetch(&capture_input_count,1,__ATOMIC_RELEASE);
    pthread_t thread;
    int rc=pthread_create(&thread,0,capture_worker_main,w);
    if (rc) {
        capture_workers=w->next;
        if (w->input_slot) __atomic_sub_fetch(&capture_input_count,1,__ATOMIC_RELEASE);
        __atomic_sub_fetch(&capture_workers_count,1,__ATOMIC_RELEASE);
        capture_list_unlock(); free(w); return -rc;
    }
    (void)pthread_detach(thread);
    capture_list_unlock();
    return 0;
}
static int capture_watchdog_expired(void)
{
    uint64_t now=capture_now_ns();
    int expired=!now && __atomic_load_n(&capture_workers_count,__ATOMIC_ACQUIRE);
    capture_list_lock();
    for (struct capture_worker *w=capture_workers;w;w=w->next) {
        uint64_t began=__atomic_load_n(&w->operation_started,__ATOMIC_ACQUIRE);
        if (now && began && now>began && now-began>2000000000u) expired=1;
    }
    capture_list_unlock();
    return expired;
}
static int finish_capture_workers(void)
{
    capture_list_lock();
    for (struct capture_worker *w=capture_workers;w;w=w->next) shutdown(w->client.fd,SHUT_RDWR);
    capture_list_unlock();
    for (unsigned attempt=0;attempt<100;attempt++) {
        if (!__atomic_load_n(&capture_workers_count,__ATOMIC_ACQUIRE)) return 0;
        usleep(10000);
    }
    return -110;
}
