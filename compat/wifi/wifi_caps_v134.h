#ifndef DIPPER_WIFI_CAPS_V134_H
#define DIPPER_WIFI_CAPS_V134_H
#include <stdint.h>
struct DipperWifiCaps {
    uint32_t ifindex, wiphy;
    unsigned he, eht, records, mask;
};
int DipperReadWifiCaps(const char *ifname, struct DipperWifiCaps *caps);
#endif
