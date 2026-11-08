#include "battery_charge_adapter.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace Dipper::Charging {
namespace {
constexpr const char* ONLINE = "/sys/class/power_supply/usb/online";
constexpr const char* ROOT = "/sys/class/qcom-battery/";

bool ReadText(const std::string& path, std::string& value)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    char buffer[129];
    size_t used = 0;
    bool ok = true;
    while (used < sizeof(buffer)) {
        const ssize_t count = read(fd, buffer + used, sizeof(buffer) - used);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            ok = false;
            break;
        }
        if (count == 0) {
            break;
        }
        used += static_cast<size_t>(count);
    }
    close(fd);
    if (!ok || used == 0 || used == sizeof(buffer)) {
        return false;
    }
    value.assign(buffer, used);
    const size_t last = value.find_last_not_of(" \t\r\n");
    if (last == std::string::npos) {
        return false;
    }
    value.erase(last + 1);
    const size_t first = value.find_first_not_of(" \t\r\n");
    value.erase(0, first);
    return value.find('\0') == std::string::npos;
}

bool ReadNumber(const std::string& path, int32_t& value)
{
    std::string text;
    if (!ReadText(path, text) || text.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long result = strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' || result < 0 || result > INT32_MAX) {
        return false;
    }
    value = static_cast<int32_t>(result);
    return true;
}

struct Facts {
    int32_t online = 0;
    int32_t authentic = 0;
    int32_t verified = 0;
    int32_t quickType = 0;
    int32_t powerW = 0;
    std::string realType;

};

bool ReadFacts(Facts& facts)
{
    if (!ReadNumber(ONLINE, facts.online) || facts.online > 1) {
        return false;
    }
    if (facts.online == 0) {
        return true;
    }
    return ReadNumber(std::string(ROOT) + "authentic", facts.authentic) &&
        facts.authentic <= 1 &&
        ReadNumber(std::string(ROOT) + "pd_verifed", facts.verified) &&
        facts.verified <= 1 &&
        ReadNumber(std::string(ROOT) + "quick_charge_type", facts.quickType) &&
        ReadNumber(std::string(ROOT) + "power_max", facts.powerW) &&
        ReadText(std::string(ROOT) + "real_type", facts.realType);
}

class ChargeAdapter final : public Battery::IBatteryInterface {
public:
    ChargeAdapter(const OHOS::sptr<Battery::IBatteryInterface>& original, SnapshotReader reader)
        : original_(original), reader_(reader)
    {
    }

    int32_t Register(const OHOS::sptr<Battery::IBatteryCallback>& event) override
    {
        // 保留原远端回调对象，不改变原厂Register所需的代理布局
        return original_->Register(event);
    }

    int32_t UnRegister() override { return original_->UnRegister(); }
    int32_t ChangePath(const std::string& path) override { return original_->ChangePath(path); }
    int32_t GetCapacity(int32_t& value) override { return original_->GetCapacity(value); }
    int32_t GetVoltage(int32_t& value) override { return original_->GetVoltage(value); }
    int32_t GetTemperature(int32_t& value) override { return original_->GetTemperature(value); }
    int32_t GetHealthState(Battery::BatteryHealthState& value) override { return original_->GetHealthState(value); }
    int32_t GetPluggedType(Battery::BatteryPluggedType& value) override { return original_->GetPluggedType(value); }
    int32_t GetChargeState(Battery::BatteryChargeState& value) override { return original_->GetChargeState(value); }
    int32_t GetPresent(bool& value) override { return original_->GetPresent(value); }
    int32_t GetTechnology(std::string& value) override { return original_->GetTechnology(value); }
    int32_t GetTotalEnergy(int32_t& value) override { return original_->GetTotalEnergy(value); }
    int32_t GetCurrentAverage(int32_t& value) override { return original_->GetCurrentAverage(value); }
    int32_t GetCurrentNow(int32_t& value) override { return original_->GetCurrentNow(value); }
    int32_t GetRemainEnergy(int32_t& value) override { return original_->GetRemainEnergy(value); }
    int32_t GetBatteryInfo(Battery::BatteryInfo& value) override { return original_->GetBatteryInfo(value); }
    int32_t SetChargingLimit(const std::vector<Battery::ChargingLimit>& value) override
    {
        return original_->SetChargingLimit(value);
    }

    int32_t GetChargeType(Battery::ChargeType& type) override
    {
        const ChargeSnapshot snapshot = reader_();
        type = snapshot.valid ? snapshot.type : Battery::CHARGE_TYPE_NONE;
        return HDF_SUCCESS;
    }

    int32_t SetBatteryConfig(const std::string& scene, const std::string& value) override
    {
        return original_->SetBatteryConfig(scene, value);
    }

    int32_t GetBatteryConfig(const std::string& scene, std::string& value) override
    {
        if (scene != "charger_type" && scene != "max_power" && scene != "display_max_power") {
            return original_->GetBatteryConfig(scene, value);
        }
        const ChargeSnapshot snapshot = reader_();
        // 临时读失败只撤销快充显示，原电池服务继续提供真实基础状态
        value = std::to_string(!snapshot.valid ? 0 : scene == "charger_type" ?
            static_cast<int32_t>(snapshot.type) : snapshot.maxPowerMw);
        return HDF_SUCCESS;
    }

    int32_t IsBatteryConfigSupported(const std::string& scene, bool& supported) override
    {
        if (scene == "charger_type" || scene == "max_power" || scene == "display_max_power") {
            supported = true;
            return HDF_SUCCESS;
        }
        return original_->IsBatteryConfigSupported(scene, supported);
    }

    int32_t GetVersion(uint32_t& major, uint32_t& minor) override
    {
        return original_->GetVersion(major, minor);
    }

private:
    OHOS::sptr<Battery::IBatteryInterface> original_;
    SnapshotReader reader_;
};
} // namespace

ChargeSnapshot ReadRealChargeSnapshot()
{
    Facts facts;
    int32_t stillOnline = 0;
    if (!ReadFacts(facts) || !ReadNumber(ONLINE, stillOnline) || stillOnline > 1) {
        return {};
    }
    ChargeSnapshot snapshot;
    snapshot.valid = true;
    // 认证字段只读一遍，末尾确认没有拔线
    snapshot.online = facts.online != 0 && stillOnline != 0;
    if (!snapshot.online) {
        return snapshot;
    }
    snapshot.type = Battery::CHARGE_TYPE_WIRED_NORMAL;
    // 只映射本机已验证的认证快充，不照搬两套枚举的相同数字
    if (facts.authentic == 1 && facts.verified == 1 && facts.quickType == 4 &&
        facts.realType == "PD_PPS" && facts.powerW > 0 && facts.powerW <= INT32_MAX / 1000) {
        snapshot.type = Battery::CHARGE_TYPE_WIRED_SUPER_QUICK;
        snapshot.maxPowerMw = facts.powerW * 1000;
    }
    return snapshot;
}

OHOS::sptr<Battery::IBatteryInterface> MakeBatteryChargeAdapter(
    const OHOS::sptr<Battery::IBatteryInterface>& original, SnapshotReader reader)
{
    if (original == nullptr || reader == nullptr) {
        return nullptr;
    }
    return new ChargeAdapter(original, reader);
}
} // namespace Dipper::Charging
