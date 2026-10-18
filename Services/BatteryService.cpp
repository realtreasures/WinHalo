#include "BatteryService.h"

namespace winisland
{
void BatteryService::Start(HWND window) noexcept
{
    Stop();
    capacity_ = RegisterPowerSettingNotification(window, &GUID_BATTERY_PERCENTAGE_REMAINING, DEVICE_NOTIFY_WINDOW_HANDLE);
    source_ = RegisterPowerSettingNotification(window, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);
}

void BatteryService::Stop() noexcept
{
    if (capacity_) UnregisterPowerSettingNotification(capacity_);
    if (source_) UnregisterPowerSettingNotification(source_);
    capacity_ = source_ = nullptr;
    lowLatched_ = false;
}

BatterySnapshot BatteryService::Decode(const SYSTEM_POWER_STATUS& status) noexcept
{
    BatterySnapshot result;
    result.available = status.BatteryFlag != 255 && (status.BatteryFlag & 128) == 0;
    if (!result.available) return result;
    result.onBattery = status.ACLineStatus == 0;
    result.charging = (status.BatteryFlag & 8) != 0;
    result.percent = status.BatteryLifePercent <= 100 ? status.BatteryLifePercent : -1;
    result.low = result.onBattery && !result.charging && result.percent >= 0 && result.percent <= 20;
    return result;
}

BatterySnapshot BatteryService::Read() const noexcept
{
    SYSTEM_POWER_STATUS status{};
    return GetSystemPowerStatus(&status) ? Decode(status) : BatterySnapshot{};
}

bool BatteryService::ShouldNotify(const BatterySnapshot& snapshot, bool allowNotification) noexcept
{
    if (!snapshot.available || !snapshot.onBattery || snapshot.charging || snapshot.percent >= 25) lowLatched_ = false;
    if (!allowNotification || !snapshot.low || lowLatched_) return false;
    lowLatched_ = true;
    return true;
}
}
