/* C ABI adapter for the original OHOS HCI vendor interface (Apache-2.0 header).
 * Qualcomm initialization and SIBS/wakelocks run in the separate native backend.
 */
#include <stddef.h>
#include "ohos_bt_vendor_lib.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
static BtVendorCallbacksT callbacks{};
static int channel=-1;
static bool ready=false;
static void release_channel()
{
    if(channel>=0){shutdown(channel,SHUT_RDWR);close(channel);channel=-1;}
    ready=false;
}
static int initialize(const BtVendorCallbacksT *cb,unsigned char *)
{
    if(!cb || cb->size<sizeof(BtVendorCallbacksT) || !cb->initCb)return -EINVAL;
    release_channel();callbacks=*cb;return 0;
}
static int connect_backend()
{
    if(ready)return 0;
    release_channel();
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return -errno;
    sockaddr_un a{};a.sun_family=AF_UNIX;strcpy(a.sun_path,"/dev/unix/socket/dipper.bt.h4");
    if(connect(fd,reinterpret_cast<sockaddr *>(&a),sizeof(a))){int e=errno;close(fd);return -e;}
    char reply[4]{};size_t n=0;
    while(n<sizeof(reply)){
        pollfd p={fd,POLLIN,0};int rc=poll(&p,1,25000);
        if(rc<0 && errno==EINTR)continue;
        if(rc<=0 || !(p.revents&POLLIN)){close(fd);return -ETIMEDOUT;}
        ssize_t got=recv(fd,reply+n,sizeof(reply)-n,0);
        if(got<=0){close(fd);return -EIO;}n+=got;
    }
    if(memcmp(reply,"SBT1",4)){close(fd);return -EIO;}
    channel=fd;ready=true;return 0;
}
static int operation(BtOpcodeT op,void *arg)
{
    switch(op){
    case BT_OP_POWER_ON:return connect_backend();
    case BT_OP_POWER_OFF:release_channel();return 0;
    case BT_OP_HCI_CHANNEL_OPEN:
        if(!ready || !arg)return -ENOTCONN;
        static_cast<int *>(arg)[0]=channel;return 1;
    case BT_OP_HCI_CHANNEL_CLOSE:release_channel();return 0;
    case BT_OP_INIT:
        if(!callbacks.initCb)return -EINVAL;
        callbacks.initCb(ready?BTC_OP_RESULT_SUCCESS:BTC_OP_RESULT_FAIL);return ready?0:-ENOTCONN;
    case BT_OP_GET_LPM_TIMER:
        if(!arg)return -EINVAL;*static_cast<uint32_t *>(arg)=1000;return 0;
    case BT_OP_LPM_ENABLE:case BT_OP_LPM_DISABLE:
    case BT_OP_WAKEUP_LOCK:case BT_OP_WAKEUP_UNLOCK:
        // QTI DataHandler performs its own native SIBS/wakelock handling.
        return ready?0:-ENOTCONN;
    default:return -ENOTSUP;
    }
}
static void shutdown_interface(){release_channel();memset(&callbacks,0,sizeof(callbacks));}
extern "C" __attribute__((visibility("default"))) const BtVendorInterfaceT BLUETOOTH_VENDOR_LIB_INTERFACE={
    sizeof(BtVendorInterfaceT),initialize,operation,shutdown_interface
};
