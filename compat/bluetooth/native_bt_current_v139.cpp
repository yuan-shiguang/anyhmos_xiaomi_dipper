// The donor Fusion service imports this current public BluetoothHost API.
#include <string>
#include <dlfcn.h>
#include <cstdio>
extern "C" int DipperBtEnableCurrent(void *lib,void *host)
{
    auto enable=reinterpret_cast<int (*)(void *,std::string,bool)>(dlsym(lib,
        "_ZN4OHOS9Bluetooth13BluetoothHost9EnableBleENSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEb"));
    if(!enable) return -100;
    puts("BT_HOST_CURRENT_API=original_EnableBle");
    return enable(host,std::string(),false);
}
