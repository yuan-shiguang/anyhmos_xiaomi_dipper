/* SPDX-License-Identifier: MIT */
#include "pal_client.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>

int dipper_pal_connect(void)
{
    int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);
    if (fd<0) return -errno;
    struct sockaddr_un a={.sun_family=AF_UNIX};
    memcpy(a.sun_path+1,DIPPER_PAL_SOCKET,sizeof(DIPPER_PAL_SOCKET)-1);
    socklen_t n=offsetof(struct sockaddr_un,sun_path)+sizeof(DIPPER_PAL_SOCKET);
    struct timeval timeout={2,0};
    if (setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout)) ||
        setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)) ||
        connect(fd,(void *)&a,n)) {
        int rc=-errno; close(fd); return rc;
    }
    return fd;
}

int dipper_pal_request(int fd, uint32_t op, uint32_t arg0, uint32_t arg1,
    const void *pcm, uint32_t bytes, struct dipper_pal_reply *reply)
{
    static uint32_t sequence;
    if (!reply || bytes>DIPPER_PAL_MAX_DATA || (bytes && !pcm)) return -EINVAL;
    uint32_t seq=__atomic_add_fetch(&sequence,1,__ATOMIC_RELAXED);
    struct dipper_pal_request h={DIPPER_PAL_MAGIC,op,bytes,seq,arg0,arg1};
    struct iovec iov[2]={{&h,sizeof(h)},{(void *)pcm,bytes}};
    struct msghdr msg={.msg_iov=iov,.msg_iovlen=bytes ? 2 : 1};
    ssize_t n=sendmsg(fd,&msg,MSG_NOSIGNAL);
    if (n<0) return -errno;
    if ((size_t)n!=sizeof(h)+bytes) return -EIO;
    n=recv(fd,reply,sizeof(*reply),MSG_TRUNC);
    if (n<0) return -errno;
    if (n!=sizeof(*reply) || reply->magic!=DIPPER_PAL_MAGIC || reply->sequence!=seq ||
        reply->bytes>bytes) return -EPROTO;
    return reply->rc;
}

/* 采集只接收后端返回的真实 PCM，不合成静音或补齐短读 */
int dipper_pal_capture_read(int fd, void *pcm, uint32_t capacity,
    struct dipper_pal_reply *reply)
{
    static uint32_t sequence;
    if (!pcm || !reply || !capacity || capacity>DIPPER_PAL_CAPTURE_CHUNK ||
        (capacity&3)) return -EINVAL;
    memset(reply,0,sizeof(*reply));
    uint32_t seq=__atomic_add_fetch(&sequence,1,__ATOMIC_RELAXED);
    struct dipper_pal_request request={DIPPER_PAL_MAGIC,DIPPER_PAL_CAPTURE_READ,0,seq,capacity,0};
    ssize_t n=send(fd,&request,sizeof(request),MSG_NOSIGNAL);
    if (n<0) return -errno;
    if ((size_t)n!=sizeof(request)) return -EIO;
    struct { struct dipper_pal_reply reply; unsigned char pcm[DIPPER_PAL_CAPTURE_CHUNK]; } packet;
    n=recv(fd,&packet,sizeof(packet),MSG_TRUNC);
    if (n<0) return -errno;
    if ((size_t)n<sizeof(packet.reply) || packet.reply.magic!=DIPPER_PAL_MAGIC ||
        packet.reply.sequence!=seq || packet.reply.bytes>capacity || (packet.reply.bytes&3) ||
        (size_t)n!=sizeof(packet.reply)+packet.reply.bytes ||
        (packet.reply.rc && packet.reply.bytes) ||
        (!packet.reply.rc && !packet.reply.bytes)) return -EPROTO;
    *reply=packet.reply;
    if (reply->bytes) memcpy(pcm,packet.pcm,reply->bytes);
    return reply->rc;
}
