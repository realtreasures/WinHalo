#include "../Core/VisibilityPolicy.h"
#include "../Services/BatteryService.h"
#include "../Services/FullscreenMonitor.h"
#include "../Services/StartupRegistration.h"
#include "../UI/IslandView.h"
#include <iostream>
#include <stdexcept>

using namespace winisland;
namespace
{
void Expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
ViewAction Hit(IslandView& view, ViewRect rect)
{
    return view.ActionAt((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
}
}

void TestSystemBehavior()
{
    SYSTEM_POWER_STATUS power{};
    power.BatteryFlag = 128;
    Expect(!BatteryService::Decode(power).available, "Desktop without a battery exposes battery UI");
    power.BatteryFlag = 255;
    Expect(!BatteryService::Decode(power).available, "Unknown battery flags were treated as a battery");
    power.BatteryFlag = 8; power.ACLineStatus = 1; power.BatteryLifePercent = 73;
    auto battery = BatteryService::Decode(power);
    Expect(battery.available && battery.charging && !battery.onBattery && !battery.low && battery.percent == 73,
        "Charging battery was decoded incorrectly");
    power.BatteryFlag = 2; power.ACLineStatus = 0; power.BatteryLifePercent = 20;
    battery = BatteryService::Decode(power);
    BatteryService batteryService;
    Expect(battery.low && batteryService.ShouldNotify(battery) && !batteryService.ShouldNotify(battery),
        "Low battery notification was missing or repeated");
    power.BatteryLifePercent = 22;
    Expect(!batteryService.ShouldNotify(BatteryService::Decode(power)), "Battery hysteresis repeated an alert");
    power.BatteryLifePercent = 25;
    batteryService.ShouldNotify(BatteryService::Decode(power));
    power.BatteryLifePercent = 20;
    Expect(batteryService.ShouldNotify(BatteryService::Decode(power)), "Recovered battery did not rearm its alert");
    power.ACLineStatus = 1;
    Expect(!batteryService.ShouldNotify(BatteryService::Decode(power)), "AC power requested a low battery alert");
    power.ACLineStatus = 0;
    Expect(batteryService.ShouldNotify(BatteryService::Decode(power)), "AC disconnect did not rearm low battery alert");
    power.ACLineStatus = 1;
    Expect(!batteryService.ShouldNotify(BatteryService::Decode(power), false), "Hidden AC state opened an alert");
    power.ACLineStatus = 0;
    Expect(!batteryService.ShouldNotify(BatteryService::Decode(power), false), "Hidden low battery opened an alert");
    Expect(batteryService.ShouldNotify(BatteryService::Decode(power)), "Hidden power cycle consumed or failed to rearm the deferred alert");
    power.BatteryLifePercent = 255;
    battery = BatteryService::Decode(power);
    Expect(battery.available && battery.percent == -1 && !battery.low, "Unknown percentage fabricated a low battery");
    power.ACLineStatus = 255; power.BatteryLifePercent = 10;
    Expect(!BatteryService::Decode(power).low, "Unknown power source fabricated an unplugged battery");

    IslandView view;
    view.Configure({}, true);
    view.RevealCapsule();
    battery = {true, true, false, 15, true};
    view.SetBattery(battery, false);
    Expect(!view.TransientToast() && !view.Animating(), "Battery status silently opened a notification");
    view.SetBattery(battery, true);
    Expect(view.BatteryToast() && view.TransientToast() && !view.Expanded() && view.Frame().height == 88,
        "Low battery did not use the compact transient panel");
    VolumeSnapshot volume;
    volume.available = true; volume.level = 0.5f;
    view.SetVolume(volume, true);
    Expect(view.BatteryToast() && !view.VolumeToast(), "Volume notification replaced the low battery alert");
    view.DismissTransientToast();
    Expect(!view.TransientToast() && view.Frame().height == view.Layout().height, "Battery dismissal did not restore the capsule");
    view.SetVolume(volume, true);
    view.SetBattery(battery, true);
    Expect(view.BatteryToast() && !view.VolumeToast(), "Low battery did not supersede a volume toast");
    battery.charging = true; battery.onBattery = false;
    view.SetBattery(battery, false);
    Expect(!view.BatteryToast(), "Connecting power left a low battery toast visible");
    view.ShowSettings();
    battery = {true, true, false, 10, true};
    view.SetBattery(battery, true);
    Expect(view.Settings() && !view.BatteryToast(), "Low battery interrupted settings");
    Expect(Hit(view, view.SettingsPageButton()) == ViewAction::SystemSettings, "System settings page cannot be entered");
    view.ShowSystemSettings();
    Expect(view.Settings() && view.SystemSettings() && Hit(view, view.SpeedButton()) == ViewAction::Startup &&
        Hit(view, view.MotionButton()) == ViewAction::AutoHideFullscreen,
        "System settings buttons resolve to animation actions");
    view.SetSystemOptions(true, false, false, L"fixture error");
    Expect(view.StartupEnabled() && !view.AutoHideFullscreen() && !view.StartupAvailable() &&
        Hit(view, view.SpeedButton()) == ViewAction::None && view.SystemError() == L"fixture error",
        "System settings do not represent unavailable startup registration");
    view.ShowMain();
    Expect(!view.SystemSettings() && !view.Settings(), "Main panel retained the system page");
    battery.available = false;
    view.SetBattery(battery, true);
    Expect(!view.Battery().available && !view.BatteryToast(), "No-battery status retained battery UI");

    VisibilityPolicy visibility;
    Expect(visibility.ShouldShow(), "New island starts manually hidden");
    visibility.SetFullscreen(true);
    Expect(!visibility.ShouldShow() && !visibility.ManuallyHidden(), "Auto hide changed manual preference");
    visibility.SetFullscreen(false);
    Expect(visibility.ShouldShow(), "Leaving fullscreen did not restore the island");
    visibility.UserToggle();
    visibility.SetFullscreen(true); visibility.SetFullscreen(false);
    Expect(visibility.ManuallyHidden() && !visibility.ShouldShow(), "Fullscreen exit overrode a manual hide");
    visibility.UserShow(); visibility.SetFullscreen(true); visibility.UserToggle();
    Expect(visibility.ShouldShow() && !visibility.ManuallyHidden(), "Explicit show could not override fullscreen hide");
    visibility.SetFullscreen(true);
    Expect(visibility.ShouldShow(), "Repeated fullscreen event revoked explicit show");
    visibility.SetFullscreen(false); visibility.SetFullscreen(true);
    Expect(!visibility.ShouldShow(), "A new fullscreen period retained an old show override");
    Expect(FullscreenMonitor::CoversMonitor({-1920, 0, 0, 1080}, {-1920, 0, 0, 1080}), "Negative monitor coordinates rejected");
    Expect(!FullscreenMonitor::CoversMonitor({0, 0, 1920, 1040}, {0, 0, 1920, 1080}), "Work-area maximization is fullscreen");
    Expect(!FullscreenMonitor::CoversMonitor({0, 0, 3840, 1080}, {0, 0, 1920, 1080}), "Different monitor rectangle accepted");

    struct HiddenWindow
    {
        HWND value = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"WinHalo system lifecycle test", WS_POPUP,
            0, 0, 420, 280, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~HiddenWindow() { if (value) DestroyWindow(value); }
    } notificationWindow;
    Expect(notificationWindow.value != nullptr, "Notification test window failed");
    FullscreenMonitor monitor;
    Expect(monitor.Start(notificationWindow.value, WM_APP + 22), "Fullscreen event hook registration failed");
    FullscreenMonitor duplicate;
    Expect(!duplicate.Start(notificationWindow.value, WM_APP + 22) && monitor.Available(),
        "Duplicate fullscreen monitor broke the active hook owner");
    monitor.Stop();
    batteryService.Start(notificationWindow.value); batteryService.Stop();
    DWORD handlesBefore = 0, handlesAfter = 0;
    Expect(GetProcessHandleCount(GetCurrentProcess(), &handlesBefore) != FALSE, "Handle count query failed");
    for (int i = 0; i < 20; ++i)
    {
        batteryService.Start(notificationWindow.value); batteryService.Stop(); batteryService.Stop();
        Expect(monitor.Start(notificationWindow.value, WM_APP + 22), "Fullscreen hook restart failed");
        monitor.Stop(); monitor.Stop();
    }
    Expect(GetProcessHandleCount(GetCurrentProcess(), &handlesAfter) != FALSE && handlesAfter <= handlesBefore + 4,
        "Repeated power/hook start-stop leaked handles");

    Expect(StartupRegistration::CommandForPath(L"D:\\With Spaces\\WinHalo.exe") == L"\"D:\\With Spaces\\WinHalo.exe\"",
        "Startup executable path was not quoted");
    for (const auto& invalid : {std::wstring{}, std::wstring(260, L'x'), std::wstring(L"path\"injection"), std::wstring(L"path\nline")})
    {
        bool rejected = false;
        try { StartupRegistration::CommandForPath(invalid); } catch (const std::invalid_argument&) { rejected = true; }
        Expect(rejected, "Invalid startup command was accepted");
    }
    bool rejected = false;
    try { StartupRegistration unsafe(L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"); }
    catch (const std::invalid_argument&) { rejected = true; }
    Expect(rejected, "Startup test escaped the isolated registry subtree");
    const auto key = L"Software\\WinHalo\\Tests\\Unit-" + std::to_wstring(GetCurrentProcessId());
    struct Cleanup
    {
        std::wstring key;
        ~Cleanup() { RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str()); }
    } cleanup{key};
    StartupRegistration registration(key);
    Expect(!registration.Read().enabled, "Isolated startup registration started enabled");
    const auto enabled = registration.SetEnabled(true);
    Expect(enabled.available && enabled.enabled && registration.Read().enabled, "Isolated startup enable/read failed");
    const auto disabled = registration.SetEnabled(false);
    Expect(disabled.available && !disabled.enabled && !registration.SetEnabled(false).enabled,
        "Startup disable was not idempotent");
    const auto actual = batteryService.Read();
    std::cout << "System battery read: available=" << actual.available << ", percent=" << actual.percent << '\n';
}
