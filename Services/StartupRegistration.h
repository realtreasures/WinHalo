#pragma once
#include "../Core/Platform.h"
#include <string>

namespace winisland
{
struct StartupStatus
{
    bool available = true;
    bool enabled = false;
    std::wstring error;
};

class StartupRegistration
{
public:
    // 测试键仅允许 Software\WinHalo\Tests\ 下的隔离位置。
    explicit StartupRegistration(std::wstring testKey = {});
    StartupStatus Read() const noexcept;
    StartupStatus SetEnabled(bool enabled) const noexcept;
    static std::wstring CommandForPath(const std::wstring& path);
private:
    std::wstring key_;
    static std::wstring ExecutableCommand();
};
}
