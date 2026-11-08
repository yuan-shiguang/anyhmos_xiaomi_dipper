/* Read-only nl80211 capabilities for one interface's own wiphy. */
#define _GNU_SOURCE
#define sockaddr_storage kernel_sockaddr_storage
#include <linux/netlink.h>
#include <linux/genetlink.h>
#include <linux/nl80211.h>
#undef sockaddr_storage
#include <sys/socket.h>
#include <sys/time.h>
#include <net/if.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "wifi_caps_v134.h"

struct Query { int fd, family; uint32_t seq; int64_t deadline; };
static int64_t millis(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static int alen(const struct nlattr *a) { return a->nla_len - NLA_HDRLEN; }
static const void *adata(const struct nlattr *a) { return (const char *)a + NLA_HDRLEN; }
#define ATTRS(a, ptr, left) for (const struct nlattr *a=(ptr); (left)>=NLA_HDRLEN && a->nla_len>=NLA_HDRLEN && a->nla_len<=(left); (left)-=NLA_ALIGN(a->nla_len), a=(const void *)((const char *)a+NLA_ALIGN(a->nla_len)))
static uint32_t u32(const struct nlattr *a) { uint32_t n; memcpy(&n, adata(a), sizeof(n)); return n; }
static void add_attr(struct nlmsghdr *h, int type, const void *value, unsigned size)
{
    struct nlattr *a=(void *)((char *)h+h->nlmsg_len);
    a->nla_type=(uint16_t)type; a->nla_len=(uint16_t)(NLA_HDRLEN+size);
    if (size) memcpy((char *)a+NLA_HDRLEN,value,size);
    h->nlmsg_len+=NLA_ALIGN(a->nla_len);
}
static int request(struct Query *q, int type, int cmd, int flags,
    int attr, const void *value, unsigned size, int split)
{
    unsigned char buf[256]={0}; struct nlmsghdr *h=(void *)buf;
    struct genlmsghdr *g=(void *)(buf+NLMSG_HDRLEN);
    h->nlmsg_len=NLMSG_HDRLEN+GENL_HDRLEN; h->nlmsg_type=(uint16_t)type;
    h->nlmsg_flags=(uint16_t)(NLM_F_REQUEST|flags); h->nlmsg_seq=++q->seq;
    g->cmd=(unsigned char)cmd; g->version=1;
    add_attr(h,attr,value,size);
    if (split) add_attr(h,NL80211_ATTR_SPLIT_WIPHY_DUMP,NULL,0);
    struct sockaddr_nl peer={.nl_family=AF_NETLINK};
    return sendto(q->fd,buf,h->nlmsg_len,0,(void *)&peer,sizeof(peer))<0?-errno:0;
}
static int receive(struct Query *q, unsigned char *buf, size_t cap)
{
    if (millis()>=q->deadline) return -ETIMEDOUT;
    struct sockaddr_nl peer={0}; struct iovec iov={buf,cap};
    struct msghdr msg={.msg_name=&peer,.msg_namelen=sizeof(peer),.msg_iov=&iov,.msg_iovlen=1};
    ssize_t n=recvmsg(q->fd,&msg,0);
    if (n<0) return -errno;
    if (!n || (msg.msg_flags&MSG_TRUNC) || peer.nl_pid!=0) return -EPROTO;
    return (int)n;
}
static void iftype(const struct nlattr *entry, struct DipperWifiCaps *caps)
{
    int left=alen(entry),station=0,he=0,eht=0;
    ATTRS(a,adata(entry),left) {
        int type=a->nla_type&NLA_TYPE_MASK;
        if (type==NL80211_BAND_IFTYPE_ATTR_IFTYPES) {
            int n=alen(a);
            ATTRS(t,adata(a),n) if ((t->nla_type&NLA_TYPE_MASK)==NL80211_IFTYPE_STATION) station=1;
        }
        if (type==NL80211_BAND_IFTYPE_ATTR_HE_CAP_PHY && alen(a)==11) he=1;
        if (type==NL80211_BAND_IFTYPE_ATTR_EHT_CAP_PHY && alen(a)==9) eht=1;
    }
    if (station) { caps->records++; caps->he|=(unsigned)he; caps->eht|=(unsigned)eht; }
}
static void bands(const struct nlattr *bands_attr, struct DipperWifiCaps *caps)
{
    int left=alen(bands_attr);
    ATTRS(band,adata(bands_attr),left) {
        int n=alen(band);
        ATTRS(field,adata(band),n) if ((field->nla_type&NLA_TYPE_MASK)==NL80211_BAND_ATTR_IFTYPE_DATA) {
            int m=alen(field); ATTRS(entry,adata(field),m) iftype(entry,caps);
        }
    }
}
static int error_message(const struct nlmsghdr *h)
{
    if (h->nlmsg_type!=NLMSG_ERROR) return 0;
    if (h->nlmsg_len<NLMSG_HDRLEN+sizeof(struct nlmsgerr)) return -EPROTO;
    const struct nlmsgerr *e=NLMSG_DATA(h);
    return e->error?e->error:-EPROTO;
}
int DipperReadWifiCaps(const char *ifname, struct DipperWifiCaps *caps)
{
    if (!ifname || !caps || !*ifname || strnlen(ifname,IFNAMSIZ)>=IFNAMSIZ) return -EINVAL;
    memset(caps,0,sizeof(*caps)); caps->ifindex=if_nametoindex(ifname);
    if (!caps->ifindex) return -ENODEV;
    struct Query q={.fd=-1,.family=0,.seq=0,.deadline=millis()+1000};
    q.fd=socket(AF_NETLINK,SOCK_RAW|SOCK_CLOEXEC,NETLINK_GENERIC);
    if (q.fd<0) return -errno;
    int rc=-ENOMEM; unsigned char *buf=malloc(65536); if (!buf) goto out;
    struct sockaddr_nl local={.nl_family=AF_NETLINK}; struct timeval tv={.tv_usec=250000};
    if (bind(q.fd,(void *)&local,sizeof(local)) ||
        setsockopt(q.fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv))) { rc=-errno; goto out; }
    rc=request(&q,GENL_ID_CTRL,CTRL_CMD_GETFAMILY,0,CTRL_ATTR_FAMILY_NAME,"nl80211",8,0);
    if (rc) goto out;
    ssize_t n=receive(&q,buf,65536); if (n<0) { rc=(int)n; goto out; }
    for (struct nlmsghdr *h=(void *)buf;NLMSG_OK(h,n);h=NLMSG_NEXT(h,n)) {
        if (h->nlmsg_seq!=q.seq) continue;
        if ((rc=error_message(h))) goto out;
        if (h->nlmsg_type!=GENL_ID_CTRL || h->nlmsg_len<NLMSG_HDRLEN+GENL_HDRLEN) continue;
        int left=(int)h->nlmsg_len-NLMSG_HDRLEN-GENL_HDRLEN;
        ATTRS(a,(const void *)((const char *)NLMSG_DATA(h)+GENL_HDRLEN),left)
            if ((a->nla_type&NLA_TYPE_MASK)==CTRL_ATTR_FAMILY_ID && alen(a)==2) {
                uint16_t family; memcpy(&family,adata(a),2); q.family=family;
            }
    }
    if (!q.family) { rc=-ENOENT; goto out; }
    rc=request(&q,q.family,NL80211_CMD_GET_INTERFACE,0,NL80211_ATTR_IFINDEX,&caps->ifindex,4,0);
    if (rc) goto out;
    n=receive(&q,buf,65536); if (n<0) { rc=n; goto out; }
    int got_phy=0,got_interface=0;
    for (struct nlmsghdr *h=(void *)buf;NLMSG_OK(h,n);h=NLMSG_NEXT(h,n)) {
        if (h->nlmsg_seq!=q.seq) continue;
        if ((rc=error_message(h))) goto out;
        if (h->nlmsg_type!=q.family || h->nlmsg_len<NLMSG_HDRLEN+GENL_HDRLEN) continue;
        int left=(int)h->nlmsg_len-NLMSG_HDRLEN-GENL_HDRLEN;
        ATTRS(a,(const void *)((const char *)NLMSG_DATA(h)+GENL_HDRLEN),left) if (alen(a)==4) {
            if ((a->nla_type&NLA_TYPE_MASK)==NL80211_ATTR_WIPHY) { caps->wiphy=u32(a); got_phy=1; }
            if ((a->nla_type&NLA_TYPE_MASK)==NL80211_ATTR_IFINDEX && u32(a)==caps->ifindex) got_interface=1;
        }
    }
    if (!got_phy || !got_interface) { rc=-EPROTO; goto out; }
    rc=request(&q,q.family,NL80211_CMD_GET_WIPHY,NLM_F_DUMP,NL80211_ATTR_WIPHY,&caps->wiphy,4,1);
    if (rc) goto out;
    int done=0;
    while (!done) {
        n=receive(&q,buf,65536); if (n<0) { rc=n; goto out; }
        for (struct nlmsghdr *h=(void *)buf;NLMSG_OK(h,n);h=NLMSG_NEXT(h,n)) {
            if (h->nlmsg_seq!=q.seq) continue;
            if ((rc=error_message(h))) goto out;
            if (h->nlmsg_flags&NLM_F_DUMP_INTR) { rc=-EINTR; goto out; }
            if (h->nlmsg_type==NLMSG_DONE) {
                if (h->nlmsg_len>=NLMSG_HDRLEN+4) { int status; memcpy(&status,NLMSG_DATA(h),4); if (status) { rc=status; goto out; } }
                done=1; break;
            }
            if (h->nlmsg_type!=q.family || h->nlmsg_len<NLMSG_HDRLEN+GENL_HDRLEN) continue;
            int left=(int)h->nlmsg_len-NLMSG_HDRLEN-GENL_HDRLEN,match=0;
            const struct nlattr *band_attr=NULL;
            ATTRS(a,(const void *)((const char *)NLMSG_DATA(h)+GENL_HDRLEN),left) {
                int type=a->nla_type&NLA_TYPE_MASK;
                if (type==NL80211_ATTR_WIPHY && alen(a)==4 && u32(a)==caps->wiphy) match=1;
                if (type==NL80211_ATTR_WIPHY_BANDS) band_attr=a;
            }
            if (match && band_attr) bands(band_attr,caps);
        }
    }
    if (!caps->records || (caps->eht && !caps->he)) { rc=-EOPNOTSUPP; goto out; }
    /* Original framework's category capability bit mask: DEFAULT=1, HE=2, EHT=4. */
    caps->mask=1u|(caps->he?2u:0u)|(caps->eht?4u:0u); rc=0;
out:
    free(buf); close(q.fd); return rc;
}
