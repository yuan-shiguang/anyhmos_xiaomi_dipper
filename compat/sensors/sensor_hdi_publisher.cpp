#include "sensor/v3_1/isensor_interface.h"
#include "object_collector.h"
#include "iservmgr_hdi.h"
#include "hdf_remote_service.h"
#include "hdf_device_class.h"
#include "message_parcel.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

extern "C" int SetParameter(const char *key, const char *value);

using namespace OHOS;
namespace Sensor = OHOS::HDI::Sensor::V3_1;

namespace {
constexpr char SERVICE_NAME[] = "sensor_interface_service";
constexpr char SERVICE_DESCRIPTOR[] = "ohos.hdi.sensor.v3_1.ISensorInterface";
constexpr char READY_PARAMETER[] = "bootevent.dipper_sensor_hdi.ready";
volatile sig_atomic_t stopping = 0;
sem_t stopWake;

void StopSignal(int)
{
    int saved = errno;
    stopping = 1;
    sem_post(&stopWake);
    errno = saved;
}

int32_t SetReady(bool ready)
{
    int32_t result = SetParameter(READY_PARAMETER, ready ? "true" : "false");
    std::printf("SENSOR_PUBLISH_READY value=%d rc=%d\n", ready, result);
    return result;
}

bool WaitMilliseconds(unsigned duration)
{
    while (duration != 0 && !stopping) {
        unsigned tick = duration > 100 ? 100 : duration;
        timespec requested{0, static_cast<long>(tick) * 1000000};
        while (nanosleep(&requested, &requested) != 0 && errno == EINTR && !stopping) {}
        duration -= tick;
    }
    return !stopping;
}

struct Enumeration {
    Sensor::ISensorInterface *implementation;
    std::atomic<bool> complete{false};
    int32_t result = HDF_FAILURE;
    bool accel = false;
    bool gyro = false;
};

void *Enumerate(void *context)
{
    auto &enumeration = *static_cast<Enumeration *>(context);
    std::vector<Sensor::HdfSensorInformation> info;
    enumeration.result = enumeration.implementation->GetAllSensorInfo(info);
    for (const auto &sensor : info) {
        enumeration.accel |= sensor.deviceSensorInfo.sensorType == 1;
        enumeration.gyro |= sensor.deviceSensorInfo.sensorType == 2;
    }
    enumeration.complete.store(true, std::memory_order_release);
    return nullptr;
}

bool WaitForSensors(Sensor::ISensorInterface *implementation)
{
    unsigned backoff = 1000;
    while (!stopping) {
        Enumeration enumeration{implementation};
        pthread_t worker;
        int created = pthread_create(&worker, nullptr, Enumerate, &enumeration);
        if (created != 0) {
            std::printf("SENSOR_PUBLISH_ENUM_THREAD rc=%d\n", created);
            return false;
        }
        // 枚举不启动采样，停止时直接退出并关闭本进程的查询连接
        while (!enumeration.complete.load(std::memory_order_acquire)) {
            if (!WaitMilliseconds(100)) {
                std::puts("SENSOR_PUBLISH_STOP phase=discover");
                _exit(0);
            }
        }
        int joined = pthread_join(worker, nullptr);
        std::printf("SENSOR_PUBLISH_ENUM rc=%d accel=%d gyro=%d join=%d\n",
            enumeration.result, enumeration.accel, enumeration.gyro, joined);
        if (joined != 0) return false;
        if (stopping) break;
        if (enumeration.result == HDF_SUCCESS && enumeration.accel && enumeration.gyro) return true;
        // 失败发现不缓存，持续等待原SSC实际发布两类传感器
        if (!WaitMilliseconds(backoff)) break;
        backoff = backoff < 4000 ? backoff * 2 : 5000;
    }
    return false;
}

struct RegistryApi {
    HdfRemoteService *(*getRemote)(int32_t) = nullptr;
    bool (*setDesc)(HdfRemoteService *, const char *) = nullptr;
    bool (*writeToken)(HdfRemoteService *, HdfSBuf *) = nullptr;
    HdfSBuf *(*obtain)(HdfSbufType) = nullptr;
    void (*recycle)(HdfSBuf *) = nullptr;
    bool (*writeString)(HdfSBuf *, const char *) = nullptr;
    bool (*writeU16)(HdfSBuf *, uint16_t) = nullptr;
    bool (*writeU32)(HdfSBuf *, uint32_t) = nullptr;
    int32_t (*toParcel)(HdfSBuf *, MessageParcel **) = nullptr;

    bool Load(void *driver)
    {
#define LOAD_MEMBER(member, symbol) \
        *reinterpret_cast<void **>(&(member)) = dlsym(driver, symbol); \
        if ((member) == nullptr) return false
        LOAD_MEMBER(getRemote, "HdfRemoteServiceGet");
        LOAD_MEMBER(setDesc, "HdfRemoteServiceSetInterfaceDesc");
        LOAD_MEMBER(writeToken, "HdfRemoteServiceWriteInterfaceToken");
        LOAD_MEMBER(obtain, "HdfSbufTypedObtain");
        LOAD_MEMBER(recycle, "HdfSbufRecycle");
        LOAD_MEMBER(writeString, "HdfSbufWriteString");
        LOAD_MEMBER(writeU16, "HdfSbufWriteUint16");
        LOAD_MEMBER(writeU32, "HdfSbufWriteUint32");
        LOAD_MEMBER(toParcel, "SbufToParcel");
#undef LOAD_MEMBER
        return true;
    }

    int32_t Publish(HdfRemoteService *registry, const sptr<IRemoteObject> &stub)
    {
        HdfSBuf *data = obtain(SBUF_IPC);
        HdfSBuf *reply = obtain(SBUF_IPC);
        MessageParcel *parcel = nullptr;
        // 沿用原UHDF注册字段及原生Binder桩
        bool valid = data != nullptr && reply != nullptr && writeToken(registry, data) &&
            writeString(data, SERVICE_NAME) && writeU16(data, DEVICE_CLASS_SENSOR) &&
            writeU32(data, 0) && toParcel(data, &parcel) == 0 && parcel != nullptr &&
            parcel->WriteRemoteObject(stub) && writeString(data, "") &&
            writeString(data, SERVICE_DESCRIPTOR);
        int32_t result = valid ? registry->dispatcher->Dispatch(registry, 1, data, reply) : HDF_FAILURE;
        if (data != nullptr) recycle(data);
        if (reply != nullptr) recycle(reply);
        return result;
    }

    int32_t Unpublish(HdfRemoteService *registry)
    {
        HdfSBuf *data = obtain(SBUF_IPC);
        HdfSBuf *reply = obtain(SBUF_IPC);
        bool valid = data != nullptr && reply != nullptr && writeToken(registry, data) &&
            writeString(data, SERVICE_NAME);
        int32_t result = valid ? registry->dispatcher->Dispatch(registry, 8, data, reply) : HDF_FAILURE;
        if (data != nullptr) recycle(data);
        if (reply != nullptr) recycle(reply);
        return result;
    }
};
}

int main(int argc, char **argv)
{
    // 兼容现有启动参数，服务始终常驻
    if (argc != 2 && !(argc == 3 && std::strcmp(argv[2], "--serve") == 0)) return 64;
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (sem_init(&stopWake, 0, 0) != 0) return 12;
    struct sigaction action{};
    action.sa_handler = StopSignal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);

    auto serviceManager = HDI::ServiceManager::V1_0::IServiceManager::Get();
    if (serviceManager == nullptr) {
        puts("SENSOR_PUBLISH_REGISTRY_AVAILABLE=0");
        return 1;
    }
    if (serviceManager->GetService(SERVICE_NAME) != nullptr) {
        puts("SENSOR_PUBLISH_ALREADY_REGISTERED=1");
        return 2;
    }
    if (SetReady(false) != 0) return 10;

    // 原驱动注册3.0和3.1桩，实际实现从独立路径创建
    void *driver = dlopen("/vendor/lib64/libsensor_driver.z.so", RTLD_NOW | RTLD_GLOBAL);
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    std::printf("SENSOR_PUBLISH_LOAD driver=%d factory=%d\n", driver != nullptr, library != nullptr);
    if (driver == nullptr || library == nullptr) return 3;
    using Factory = Sensor::ISensorInterface *(*)();
    using Shutdown = int32_t (*)(Sensor::ISensorInterface *);
    auto create = reinterpret_cast<Factory>(dlsym(library, "SensorInterfaceImplGetInstance"));
    auto shutdown = reinterpret_cast<Shutdown>(dlsym(library, "SensorInterfaceImplStopAll"));
    RegistryApi api;
    if (create == nullptr || shutdown == nullptr || !api.Load(driver)) {
        puts("SENSOR_PUBLISH_SYMBOLS_READY=0");
        return 4;
    }
    sptr<Sensor::ISensorInterface> implementation = create();
    if (implementation == nullptr) return 5;
    if (!WaitForSensors(implementation.GetRefPtr())) {
        int32_t stopped = shutdown(implementation.GetRefPtr());
        std::printf("SENSOR_PUBLISH_STOP phase=prepublish stop=%d\n", stopped);
        _exit(stopping && stopped == HDF_SUCCESS ? 0 : 11);
    }
    auto stub = HDI::ObjectCollector::GetInstance().GetOrNewObject(
        implementation, Sensor::ISensorInterface::GetDescriptor());
    if (stub == nullptr) return 6;
    auto registry = api.getRemote(5100);
    if (registry == nullptr || !api.setDesc(registry, "HDI.IServiceManager.V1_0")) return 7;
    int32_t published = api.Publish(registry, stub);
    std::printf("SENSOR_PUBLISH_RESULT rc=%d uid=%u gid=%u\n", published, getuid(), getgid());
    if (published != HDF_SUCCESS) return 8;
    int32_t ready = stopping ? 0 : SetReady(true);
    int waitError = 0;
    // 发布后仅由退出信号唤醒，不产生周期性轮询
    while (ready == 0 && !stopping) {
        if (sem_wait(&stopWake) != 0 && errno != EINTR) {
            waitError = errno;
            std::printf("SENSOR_PUBLISH_WAIT errno=%d\n", waitError);
            break;
        }
    }

    // 先撤销发现入口，再停止全部真实采样线程
    int32_t cleared = SetReady(false);
    int32_t removed = api.Unpublish(registry);
    int32_t stopped = shutdown(implementation.GetRefPtr());
    std::printf("SENSOR_PUBLISH_CLEANUP unpublish=%d stop=%d\n", removed, stopped);
    // IPC线程可能仍持有对象，保持代码映射直到进程退出
    _exit(ready == 0 && waitError == 0 && cleared == 0 && removed == HDF_SUCCESS && stopped == HDF_SUCCESS ? 0 : 9);
}
