#pragma once
#include "../Core/Platform.h"
#include "../Core/Configuration.h"
#include <functional>
#include <memory>

namespace winisland
{
struct SettingsStatus
{
    bool startupEnabled = false;
    bool startupAvailable = true;
    bool brightnessAvailable = false;
    bool privacyAvailable = false;
    std::wstring startupError;
    std::wstring brightnessStatus;
    std::wstring privacyStatus;
    std::wstring saveError;
};

class SettingsWindow
{
public:
    using ChangeHandler = std::function<void(const Configuration&)>;
    using StartupHandler = std::function<void(bool)>;
    explicit SettingsWindow(HINSTANCE instance);
    ~SettingsWindow();
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;
    void Show(const Configuration& configuration, const SettingsStatus& status,
        ChangeHandler change, StartupHandler startup);
    void Refresh(const Configuration& configuration, const SettingsStatus& status);
    void Close();
    HWND Handle() const noexcept;
    bool PreTranslate(MSG& message);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
