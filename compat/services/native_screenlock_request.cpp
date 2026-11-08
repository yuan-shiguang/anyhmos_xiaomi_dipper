// Original no-password screenlock API request with bounded state polling.
#include <atomic>
#include <cstdio>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <cstring>
#include "ipc_skeleton.h"
#include "screenlock_manager.h"
#include "screenlock_callback_interface.h"
#include "iremote_object.h"
#include <dlfcn.h>
#include <new>
#include <cstdint>
using namespace OHOS;
using namespace OHOS::ScreenLock;
static std::atomic<int>* callbackResult;
static int receive(void*,uint32_t code,MessageParcel& data,MessageParcel&,MessageOption&)
{
    if(data.ReadInterfaceToken()!=ScreenLockCallbackInterface::GetDescriptor())return E_SCREENLOCK_TRANSACT_ERROR;
    if(code!=ScreenLockCallbackInterface::ON_CALLBACK)return -1;
    int value=data.ReadInt32();
    if(callbackResult)callbackResult->store(value);
    std::printf("SCREENLOCK_CALLBACK_RECEIVED result=%d\n",value);
    return 0;
}
static sptr<IRemoteObject> makeOriginalRemote()
{
    void* lib=dlopen("libipc_single.z.so",RTLD_NOW|RTLD_LOCAL);
    if(!lib)return nullptr;
    auto ctor=reinterpret_cast<void(*)(void*,std::u16string,bool)>(dlsym(lib,
        "_ZN4OHOS13IPCObjectStubC1ENSt3__h12basic_stringIDsNS1_11char_traitsIDsEENS1_9allocatorIDsEEEEb"));
    void* originalRequest=dlsym(lib,"_ZN4OHOS13IPCObjectStub15OnRemoteRequestEjRNS_13MessageParcelES2_RNS_13MessageOptionE");
    auto* send=reinterpret_cast<const uint32_t*>(dlsym(lib,
        "_ZN4OHOS13IPCObjectStub16SendRequestInnerEjRNS_13MessageParcelES2_RNS_13MessageOptionE"));
    // Pinned donor dispatch: ldr x8,[x8,#200], immediately before blr x8.
    if(!ctor||!originalRequest||!send||send[0x98/4]!=0xf9406508||send[0x9c/4]!=0xd63f0100){
        std::puts("SCREENLOCK_DONOR_ABI_GUARD_FAILED");return nullptr;
    }
    // Opaque storage deliberately exceeds the inspected complete constructor's
    // object (Parcelable at120, RefBase at136). No header-defined fields are read.
    void* object=::operator new(4096);
    ctor(object,ScreenLockCallbackInterface::GetDescriptor(),false);
    auto** table=*reinterpret_cast<void***>(object);
    if(table[200/8]!=originalRequest){std::puts("SCREENLOCK_DONOR_VTABLE_GUARD_FAILED");return nullptr;}
    // Original complete ctor uses table+32 for the primary vptr and+416 for
    // its next secondary vptr. Preserve that entire primary block/prefix.
    alignas(16) static uintptr_t privateTable[52];
    std::memcpy(privateTable,reinterpret_cast<const char*>(table)-32,sizeof(privateTable));
    privateTable[4+200/8]=reinterpret_cast<uintptr_t>(&receive);
    *reinterpret_cast<void***>(object)=reinterpret_cast<void**>(privateTable+4);
    std::puts("SCREENLOCK_ORIGINAL_STUB_READY private_dispatch_slot=200 original_library_untouched=1");
    return sptr<IRemoteObject>(reinterpret_cast<IRemoteObject*>(object));
}
class Callback final : public ScreenLockCallbackInterface {
    sptr<IRemoteObject> remote_;
public:
    std::atomic<int> result{-999};
    Callback():remote_(makeOriginalRemote()){callbackResult=&result;}
    sptr<IRemoteObject> AsObject() override {return remote_;}
    void OnCallBack(int32_t value) override {result.store(value);}
    bool Ready() const {return remote_!=nullptr;}
};
static void crash(int sig, siginfo_t* info, void* raw)
{
    auto* u = static_cast<ucontext_t*>(raw);
    dprintf(1,"SCREENLOCK_CRASH signal=%d address=%p pc=%llx lr=%llx sp=%llx\n",sig,info->si_addr,
        (unsigned long long)u->uc_mcontext.pc,(unsigned long long)u->uc_mcontext.regs[30],
        (unsigned long long)u->uc_mcontext.sp);
    int fd=open("/proc/self/maps",O_RDONLY);char data[4096];ssize_t n;
    while(fd>=0&&(n=read(fd,data,sizeof(data)))>0)write(1,data,n);
    _exit(128+sig);
}
int main()
{
    alarm(35);
    if (!freopen("/data/local/tmp/dipper-capture/unlock-request.txt", "w", stdout)) return 2;
    setbuf(stdout, nullptr);
    struct sigaction action{};
    action.sa_sigaction=crash;action.sa_flags=SA_SIGINFO;
    sigaction(SIGSEGV,&action,nullptr);sigaction(SIGILL,&action,nullptr);sigaction(SIGABRT,&action,nullptr);
    auto manager = ScreenLockManager::GetInstance();
    if (!manager) return 3;
    bool deviceLocked = true;
    int check = manager->IsDeviceLocked(100, deviceLocked);
    bool secure = manager->GetSecure();
    std::printf("SCREENLOCK_CHECK result=%d deviceLocked=%d secure=%d\n", check, deviceLocked, secure);
    if (check != E_SCREENLOCK_OK || deviceLocked || secure) return 5;
    bool forceCallback=access("/data/local/tmp/dipper-capture/screenlock-callback-test",F_OK)==0;
    bool locked = manager->IsScreenLocked();
    std::printf("SCREENLOCK_REQUEST uid=%d before=%d\n", getuid(), locked);
    if (!locked && !forceCallback) {
        std::puts("SCREENLOCK_VERIFIED after=0 already_unlocked=1");
        return 0;
    }
    if (!IPCSkeleton::SetMaxWorkThreadNum(2)) return 7;
    pthread_t ipcThread;
    if (pthread_create(&ipcThread, nullptr, [](void*) -> void* {
        IPCSkeleton::JoinWorkThread(); return nullptr;
    }, nullptr) != 0) return 8;
    pthread_detach(ipcThread);
    sptr<Callback> callback = new Callback;
    if(!callback->Ready())return 11;
    int ret = manager->Unlock(Action::UNLOCKSCREEN, callback);
    std::printf("SCREENLOCK_REQUEST ret=%d\n", ret);
    if (ret != E_SCREENLOCK_OK) return 4;
    if (forceCallback) {
        for(unsigned i=0;i<25&&callback->result.load()==-999;++i)sleep(1);
        std::printf("SCREENLOCK_CALLBACK_TEST result=%d\n",callback->result.load());
        if(callback->result.load()==-999)return 10;
    }
    // Callback delivery is optional. Query the original authoritative state.
    for (unsigned i = 0; i < 25; ++i) {
        sleep(1);
        locked = manager->IsScreenLocked();
        if (!locked) {
            std::printf("SCREENLOCK_VERIFIED after=0 callback=%d elapsed=%u\n", callback->result.load(), i + 1);
            return 0;
        }
    }
    std::puts("SCREENLOCK_TIMEOUT after=1");
    return 6;
}
