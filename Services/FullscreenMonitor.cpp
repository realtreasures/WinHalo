#include "FullscreenMonitor.h"
#include <dwmapi.h>
#include <cmath>
#include <cwchar>

namespace winisland
{
thread_local FullscreenMonitor* FullscreenMonitor::active_ = nullptr;

bool FullscreenMonitor::Start(HWND window, UINT message) noexcept
{
    Stop();
    if (active_ || !window || !message) return false;
    window_ = window;
    message_ = message;
    active_ = this;
    foreground_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, Event,
        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    location_ = SetWinEventHook(EVENT_OBJECT_STATECHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, Event,
        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (Available()) return true;
    Stop();
    return false;
}

void FullscreenMonitor::Stop() noexcept
{
    if (active_ == this) active_ = nullptr;
    if (foreground_) UnhookWinEvent(foreground_);
    if (location_) UnhookWinEvent(location_);
    foreground_ = location_ = nullptr;
    window_ = nullptr;
    posted_ = previous_ = false;
}

void CALLBACK FullscreenMonitor::Event(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child,
    DWORD, DWORD) noexcept
{
    const auto self = active_;
    if (!self || !self->window_ || !window) return;
    if (event != EVENT_SYSTEM_FOREGROUND && (object != OBJID_WINDOW || child != CHILDID_SELF || window != GetForegroundWindow())) return;
    if (!self->posted_) self->posted_ = PostMessageW(self->window_, self->message_, 0, 0) != FALSE;
}

bool FullscreenMonitor::CoversMonitor(const RECT& frame, const RECT& monitor) noexcept
{
    // 容忍 DWM 的两像素边界差，不把普通窗口或仅覆盖工作区的最大化窗口视为全屏。
    return std::abs(static_cast<long long>(frame.left) - monitor.left) <= 2 &&
        std::abs(static_cast<long long>(frame.top) - monitor.top) <= 2 &&
        std::abs(static_cast<long long>(frame.right) - monitor.right) <= 2 &&
        std::abs(static_cast<long long>(frame.bottom) - monitor.bottom) <= 2 &&
        monitor.right > monitor.left && monitor.bottom > monitor.top;
}

bool FullscreenMonitor::Check() noexcept
{
    posted_ = false;
    if (!window_) return false;
    const HWND foreground = GetForegroundWindow();
    if (!foreground || !IsWindowVisible(foreground) || IsIconic(foreground)) return previous_ = false;
    DWORD process = 0;
    GetWindowThreadProcessId(foreground, &process);
    // 托盘菜单是同进程临时窗口，不能因此错误恢复游戏中的胶囊。
    if (process == GetCurrentProcessId()) return previous_;
    wchar_t name[64]{};
    GetClassNameW(foreground, name, 64);
    if (wcscmp(name, L"Progman") == 0 || wcscmp(name, L"WorkerW") == 0 ||
        wcscmp(name, L"Shell_TrayWnd") == 0 || wcscmp(name, L"Shell_SecondaryTrayWnd") == 0)
        return previous_ = false;
    const LONG_PTR style = GetWindowLongPtrW(foreground, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION || (style & WS_CHILD)) return previous_ = false;
    const auto monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
    if (MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST) != monitor) return previous_ = false;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(foreground, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return previous_ = false;
    RECT frame{};
    if (FAILED(DwmGetWindowAttribute(foreground, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) &&
        !GetWindowRect(foreground, &frame)) return previous_ = false;
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return previous_ = false;
    return previous_ = CoversMonitor(frame, info.rcMonitor);
}
}
