#pragma once
#include "../Core/Platform.h"

namespace winisland
{
class FullscreenMonitor
{
public:
    FullscreenMonitor() = default;
    FullscreenMonitor(const FullscreenMonitor&) = delete;
    FullscreenMonitor& operator=(const FullscreenMonitor&) = delete;
    ~FullscreenMonitor() { Stop(); }
    bool Start(HWND window, UINT message) noexcept;
    void Stop() noexcept;
    bool Check() noexcept;
    bool Available() const noexcept { return foreground_ && location_; }
    static bool CoversMonitor(const RECT& frame, const RECT& monitor) noexcept;
private:
    static void CALLBACK Event(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) noexcept;
    static thread_local FullscreenMonitor* active_;
    HWND window_ = nullptr;
    UINT message_ = 0;
    HWINEVENTHOOK foreground_ = nullptr;
    HWINEVENTHOOK location_ = nullptr;
    bool posted_ = false;
    bool previous_ = false;
};
}
