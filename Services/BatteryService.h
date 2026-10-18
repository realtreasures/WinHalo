#pragma once
#include "../Core/Platform.h"
#include "BatteryTypes.h"

namespace winisland
{
class BatteryService
{
public:
    BatteryService() = default;
    BatteryService(const BatteryService&) = delete;
    BatteryService& operator=(const BatteryService&) = delete;
    ~BatteryService() { Stop(); }
    void Start(HWND window) noexcept;
    void Stop() noexcept;
    BatterySnapshot Read() const noexcept;
    static BatterySnapshot Decode(const SYSTEM_POWER_STATUS& status) noexcept;
    // 一次低电量区间只提示一次；接通电源或回升至 25% 后重新允许提示。
    bool ShouldNotify(const BatterySnapshot& snapshot, bool allowNotification = true) noexcept;
private:
    HPOWERNOTIFY capacity_ = nullptr;
    HPOWERNOTIFY source_ = nullptr;
    bool lowLatched_ = false;
};
}
