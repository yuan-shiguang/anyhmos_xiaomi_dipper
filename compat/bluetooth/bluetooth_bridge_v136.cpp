/* Original303 backend -> bounded H4 transport -> original OHOS HCI service. */
#define DIPPER_BT_BRIDGE 1
#include "bluetooth_initialize_v136.cpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <vector>
// The compile sysroot is OHOS; resolve errno through the actual Bionic ABI.
#undef errno
extern "C" int *__errno(void);
#define errno (*__errno())
static std::atomic<int> client_fd{-1};
static pthread_mutex_t transmit_lock=PTHREAD_MUTEX_INITIALIZER;
static bool write_all(int fd,const void *data,size_t n)
{
    auto p=static_cast<const unsigned char *>(data);
    while(n){
        ssize_t k=send(fd,p,n,MSG_NOSIGNAL|MSG_DONTWAIT);
        if(k>0){p+=k;n-=k;continue;}
        if(k<0 && errno==EINTR)continue;
        if(k<0 && (errno==EAGAIN || errno==EWOULDBLOCK)){
            pollfd wait={fd,POLLOUT,0};
            if(poll(&wait,1,1000)>0 && (wait.revents&POLLOUT))continue;
        }
        return false;
    }
    return true;
}
static void bridge_deliver(HciPacketType type,const unsigned char *data,size_t n)
{
    pthread_mutex_lock(&transmit_lock);
    int fd=client_fd.load();
    if(fd>=0){unsigned char h4=type;
        if(!write_all(fd,&h4,1) || !write_all(fd,data,n))shutdown(fd,SHUT_RDWR);
    }
    pthread_mutex_unlock(&transmit_lock);
}
static bool read_all(int fd,void *data,size_t n)
{
    auto p=static_cast<unsigned char *>(data);
    while(n){ssize_t k=recv(fd,p,n,0);
        if(k>0){p+=k;n-=k;continue;}
        if(k<0 && errno==EINTR)continue;
        return false;
    }
    return true;
}
static int run(int,char **,char **)
{
    setvbuf(stdout,nullptr,_IOLBF,0);
    const char *library=getenv("DIPPER_BT_ORIGINAL_LIBRARY");
    if(!library)return 10;
    void *lib=dlopen(library,RTLD_NOW|RTLD_LOCAL);
    if(!lib){printf("BT_BRIDGE_DLOPEN_ERROR=%s\n",dlerror());return 11;}
    using Init=bool (*)(ProtocolType,std::function<void(bool)>,std::function<void(HciPacketType,const Packet *)>);
    using Get=void *(*)();using Clean=void (*)(ProtocolType);
    using Send=size_t (*)(void *,ProtocolType,HciPacketType,const unsigned char *,size_t);
    auto init=reinterpret_cast<Init>(dlsym(lib,"_ZN7android8hardware9bluetooth4V1_014implementation11DataHandler4InitE12ProtocolTypeNSt3__18functionIFvbEEENS7_IFv13HciPacketTypePKNS0_8hidl_vecIhEEEEE"));
    auto get=reinterpret_cast<Get>(dlsym(lib,"_ZN7android8hardware9bluetooth4V1_014implementation11DataHandler3GetEv"));
    auto clean=reinterpret_cast<Clean>(dlsym(lib,"_ZN7android8hardware9bluetooth4V1_014implementation11DataHandler7CleanUpE12ProtocolType"));
    auto send_data=reinterpret_cast<Send>(dlsym(lib,"_ZN7android8hardware9bluetooth4V1_014implementation11DataHandler8SendDataE12ProtocolType13HciPacketTypePKhm"));
    if(!init || !get || !clean || !send_data)return 12;
    property_set("persist.vendor.service.bdroid.disable_perisec","1");
    int lock=open("/hmos-root/data/local/tmp/bt-backend-v136.lock",O_CREAT|O_RDWR|O_CLOEXEC,0600);
    if(lock<0 || flock(lock,LOCK_EX|LOCK_NB))return 13;
    int listener=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(listener<0)return 14;
    sockaddr_un address{};address.sun_family=AF_UNIX;
    strcpy(address.sun_path,"/hmos-root/dev/unix/socket/dipper.bt.h4");
    unlink(address.sun_path);
    if(bind(listener,reinterpret_cast<sockaddr *>(&address),sizeof(address)))return 15;
    if(chown(address.sun_path,0,3021) || chmod(address.sun_path,0660) || listen(listener,1))return 16;
    puts("BT_BRIDGE_LISTENING=1 hardware_off_until_client=1");
    unsigned sessions=0,max_sessions=0;const char *max=getenv("DIPPER_BT_MAX_SESSIONS");
    if(max)max_sessions=strtoul(max,nullptr,10);
    for(;;){
        int fd=accept4(listener,nullptr,nullptr,SOCK_CLOEXEC);
        if(fd<0){if(errno==EINTR)continue;break;}
        ucred peer{};socklen_t length=sizeof(peer);
        if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&length) || (peer.uid!=0 && peer.uid!=3021)){close(fd);continue;}
        printf("BT_BRIDGE_CLIENT pid=%d uid=%d\n",peer.pid,peer.uid);
        initialized.store(-1);
        bool accepted=init(TYPE_BT,[](bool ok){initialized.store(ok?1:0);printf("BT_BRIDGE_INITIALIZED=%d\n",ok);},packet_received);
        if(accepted)for(unsigned i=0;i<200 && initialized.load()<0;i++)usleep(100000);
        if(!accepted || initialized.load()!=1){
            write_all(fd,"SBE1",4);close(fd);
            // Failed native initialization can have live timers/threads. Exit;
            // the owning service can restart after recording this failure.
            puts("BT_BRIDGE_INIT_FAILED=1");_exit(17);
        }
        if(write_all(fd,"SBT1",4))client_fd.store(fd);
        while(client_fd.load()>=0){
            unsigned char type=0,header[4]{};
            if(!read_all(fd,&type,1))break;
            size_t h=type==1?3:type==2?4:type==3?3:type==5?4:0;
            if(!h || !read_all(fd,header,h))break;
            size_t body=type==1?header[2]:type==3?header[2]:header[2]|(header[3]<<8);
            if(type==5)body&=0x3fff;
            if(body>65535)break;
            std::vector<unsigned char> packet(header,header+h);packet.resize(h+body);
            if(body && !read_all(fd,packet.data()+h,body))break;
            size_t sent=send_data(get(),TYPE_BT,static_cast<HciPacketType>(type),packet.data(),packet.size());
            if(sent!=packet.size()){printf("BT_BRIDGE_SEND_ERROR=%zu/%zu\n",sent,packet.size());break;}
        }
        pthread_mutex_lock(&transmit_lock);client_fd.store(-1);shutdown(fd,SHUT_RDWR);close(fd);pthread_mutex_unlock(&transmit_lock);
        clean(TYPE_BT);puts("BT_BRIDGE_SESSION_CLOSED=1");
        sessions++;if(max_sessions && sessions>=max_sessions)break;
    }
    close(listener);unlink(address.sun_path);close(lock);return 0;
}
