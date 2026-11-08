/* Bounded original303 Qualcomm backend experiment. No framework substitution. */
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#include <pthread.h>
#include <functional>
#include <atomic>
#include <time.h>
#include <cstdint>
#include <stdlib.h>

enum ProtocolType { TYPE_BT=0 };
enum HciPacketType { HCI_COMMAND=1,HCI_ACL=2,HCI_SCO=3,HCI_EVENT=4 };
namespace android { namespace hardware { template<class T> class hidl_vec; } }
using Packet=android::hardware::hidl_vec<unsigned char>;
static std::atomic<int> initialized{-1};
static std::atomic<unsigned> replies{0};
#ifdef DIPPER_BT_BRIDGE
static void bridge_deliver(HciPacketType,const unsigned char *,size_t);
#endif
static void packet_received(HciPacketType type,const Packet *packet)
{
    // Layout read from upstream Android13 HidlSupport.h: pointer,uint32,bool.
    // This view never constructs, owns, or mutates an Android hidl_vec.
    struct View { const unsigned char *data; uint32_t size; bool owns; } view{};
    static_assert(sizeof(View)==16,"HIDL vector read view");
    if(!packet)return;
    memcpy(&view,packet,sizeof(view));
    if(!view.data || view.size>65540){puts("BT_RX_INVALID_VECTOR");return;}
#ifdef DIPPER_BT_BRIDGE
    bridge_deliver(type,view.data,view.size);
#else
    flockfile(stdout);printf("BT_RX_PACKET type=%d size=%u bytes=",type,view.size);
    for(unsigned i=0;i<view.size && i<64;i++)printf("%02x",view.data[i]);
    putchar('\n');funlockfile(stdout);
#endif
    if(type==HCI_EVENT && view.size>=6 && view.data[0]==0x0e && view.data[5]==0){
        unsigned opcode=view.data[3]|(view.data[4]<<8);
        if(opcode==0x1001)replies.fetch_or(1);
        if(opcode==0x1009)replies.fetch_or(2);
    }
}
static pthread_mutex_t props_lock=PTHREAD_MUTEX_INITIALIZER;
static struct {char key[128],value[92];} props[64];
static unsigned prop_count;
extern "C" int property_set(const char *key,const char *value)
{
    if(strlen(key)>=128 || strlen(value)>=92)return -1;
    pthread_mutex_lock(&props_lock);
    unsigned i=0;for(;i<prop_count;i++)if(!strcmp(key,props[i].key))break;
    if(i==64){pthread_mutex_unlock(&props_lock);return -1;}
    if(i==prop_count)prop_count++;
    strcpy(props[i].key,key);strcpy(props[i].value,value);
    pthread_mutex_unlock(&props_lock);
    printf("BT_PROCESS_LOCAL_PROP %s=%s\n",key,value);return 0;
}
extern "C" int property_get(const char *key,char *value,const char *fallback)
{
    pthread_mutex_lock(&props_lock);
    for(unsigned i=0;i<prop_count;i++)if(!strcmp(key,props[i].key)){
        strcpy(value,props[i].value);pthread_mutex_unlock(&props_lock);return strlen(value);
    }
    pthread_mutex_unlock(&props_lock);
    using Real=int (*)(const char *,char *,const char *);
    auto real=reinterpret_cast<Real>(dlsym(RTLD_NEXT,"property_get"));
    if(real)return real(key,value,fallback);
    size_t n=strnlen(fallback?fallback:"",91);memcpy(value,fallback?fallback:"",n);value[n]=0;return n;
}
extern "C" int __android_log_print(int priority,const char *tag,const char *fmt,...)
{
    flockfile(stdout);printf("BT_LOG[%d][%s] ",priority,tag);
    va_list args;va_start(args,fmt);int n=vprintf(fmt,args);va_end(args);
    putchar('\n');funlockfile(stdout);return n;
}
#ifndef DIPPER_BT_BRIDGE
static int run(int,char **,char **)
{
    setvbuf(stdout,nullptr,_IOLBF,0);
    static_assert(sizeof(std::function<void(bool)>)==48,"Android function ABI");
    void *lib=dlopen("/hmos-root/data/local/tmp/bt-backend-v136/lib64/android.hardware.bluetooth@1.0-impl-qti.so",RTLD_NOW|RTLD_LOCAL);
    if(!lib){printf("BT_DLOPEN_ERROR=%s\n",dlerror());return 11;}
    using Init=bool (*)(ProtocolType,std::function<void(bool)>,std::function<void(HciPacketType,const Packet *)>);
    using Clean=bool (*)(ProtocolType);
    auto init=reinterpret_cast<Init>(dlsym(lib,"_ZN7android8hardware9bluetooth4V1_014implementation11DataHandler4InitE12ProtocolTypeNSt3__18functionIFvbEEENS7_IFv13HciPacketTypePKNS0_8hidl_vecIhEEEEE"));
    auto clean=reinterpret_cast<Clean>(dlsym(lib,"_ZN7android8hardware9bluetooth4V1_014implementation11DataHandler7CleanUpE12ProtocolType"));
    if(!init || !clean)return 12;
    // Original303 HAL has an explicit development option for missing PCService.
    // This stays inside this disposable process; no secure service is emulated.
    property_set("persist.vendor.service.bdroid.disable_perisec","1");
    puts("BT_ORIGINAL_INIT_CALL protocol=0");
    bool accepted=init(TYPE_BT,[](bool ok){initialized.store(ok?1:0);printf("BT_INIT_CALLBACK_SUCCESS=%d\n",ok);},
        packet_received);
    printf("BT_ORIGINAL_INIT_ACCEPTED=%d\n",accepted);
    if(!accepted)return 14;
    for(unsigned i=0;i<200 && initialized.load()<0;i++)usleep(100000);
    printf("BT_INIT_RESULT=%d\n",initialized.load());
    if(initialized.load()==1){
        using Get=void *(*)();
        using Send=size_t (*)(void *,ProtocolType,HciPacketType,const unsigned char *,size_t);
        auto get=reinterpret_cast<Get>(dlsym(lib,"_ZN7android8hardware9bluetooth4V1_014implementation11DataHandler3GetEv"));
        auto send=reinterpret_cast<Send>(dlsym(lib,"_ZN7android8hardware9bluetooth4V1_014implementation11DataHandler8SendDataE12ProtocolType13HciPacketTypePKhm"));
        if(get && send){
            void *handler=get();unsigned char version[]={0x01,0x10,0x00},address[]={0x09,0x10,0x00};
            printf("BT_SEND_READ_VERSION bytes=%zu\n",send(handler,TYPE_BT,HCI_COMMAND,version,sizeof(version)));
            for(unsigned i=0;i<20 && !(replies.load()&1);i++)usleep(100000);
            printf("BT_SEND_READ_ADDRESS bytes=%zu\n",send(handler,TYPE_BT,HCI_COMMAND,address,sizeof(address)));
            for(unsigned i=0;i<20 && !(replies.load()&2);i++)usleep(100000);
            printf("BT_REAL_COMMAND_REPLY_MASK=%u\n",replies.load());
        }
    }
    puts("BT_ORIGINAL_CLEANUP_CALL=1");bool stopped=clean(TYPE_BT);
    printf("BT_ORIGINAL_CLEANUP_RETURN=%d\n",stopped);
    return initialized.load()==1 && replies.load()==3?0:13;
}
#else
static int run(int,char **,char **);
#endif
extern "C" __attribute__((noreturn)) void __libc_init(void *raw,void (*onexit)(void),
    int (*)(int,char **,char **),const void *structors)
{
    using Entry=void (*)(void *,void (*)(void),int (*)(int,char **,char **),const void *);
    auto real=reinterpret_cast<Entry>(dlsym(RTLD_NEXT,"__libc_init"));
    if(real)real(raw,onexit,run,structors);
    _exit(90);
}
