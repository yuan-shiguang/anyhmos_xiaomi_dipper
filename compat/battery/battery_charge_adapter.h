#ifndef DIPPER_BATTERY_CHARGE_ADAPTER_H
#define DIPPER_BATTERY_CHARGE_ADAPTER_H

#include "battery/v2_0/ibattery_interface.h"

namespace Dipper::Charging {
namespace Battery = OHOS::HDI::Battery::V2_0;

struct ChargeSnapshot {
    bool valid = false;
    bool online = false;
    Battery::ChargeType type = Battery::CHARGE_TYPE_NONE;
    int32_t maxPowerMw = 0;
};

using SnapshotReader = ChargeSnapshot (*)();

// 只读真实连接、认证和协议状态，不修改任何电源节点
ChargeSnapshot ReadRealChargeSnapshot();

// reader 仅供独立测试注入，实际发布使用默认真实读取器
OHOS::sptr<Battery::IBatteryInterface> MakeBatteryChargeAdapter(
    const OHOS::sptr<Battery::IBatteryInterface>& original,
    SnapshotReader reader = ReadRealChargeSnapshot);
} // namespace Dipper::Charging

#endif
