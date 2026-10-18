#include "OutsideClickMonitor.h"
#include <algorithm>
#include <cmath>

namespace winisland
{
bool OutsideClickMonitor::Watch(HWND window, UINT message, Area area)
{
    std::unique_lock lock(mutex_);
    window_ = window; message_ = message; area_ = area;
    if (thread_.joinable()) return installed_;
    ready_ = installed_ = posted_ = false;
    thread_ = std::jthread([this]
    {
        current_ = this;
        MSG message{}; PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        const HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, Hook, GetModuleHandleW(nullptr), 0);
        {
            std::lock_guard guard(mutex_);
            threadId_ = GetCurrentThreadId(); installed_ = hook != nullptr; ready_ = true;
        }
        started_.notify_one();
        if (hook)
        {
            while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
            UnhookWindowsHookEx(hook);
        }
        current_ = nullptr;
    });
    started_.wait(lock, [this] { return ready_; });
    return installed_;
}

void OutsideClickMonitor::Stop() noexcept
{
    DWORD id = 0;
    {
        std::lock_guard lock(mutex_);
        window_ = nullptr; id = threadId_;
    }
    if (thread_.joinable())
    {
        if (id) PostThreadMessageW(id, WM_QUIT, 0, 0);
        thread_.join();
    }
    std::lock_guard lock(mutex_);
    ready_ = installed_ = posted_ = false; threadId_ = 0;
}

LRESULT CALLBACK OutsideClickMonitor::Hook(int code, WPARAM button, LPARAM data) noexcept
{
    if (code == HC_ACTION && current_ && (button == WM_LBUTTONDOWN || button == WM_RBUTTONDOWN ||
        button == WM_MBUTTONDOWN || button == WM_XBUTTONDOWN))
    {
        const auto point = reinterpret_cast<const MSLLHOOKSTRUCT*>(data)->pt;
        std::lock_guard lock(current_->mutex_);
        const auto& area = current_->area_;
        const float x = std::abs(static_cast<float>(point.x) - area.left - area.width / 2) - (area.width / 2 - area.radius);
        const float y = std::abs(static_cast<float>(point.y) - area.top - area.height / 2) - (area.height / 2 - area.radius);
        const float outsideX = std::max(x, 0.0f), outsideY = std::max(y, 0.0f);
        const float distance = std::sqrt(outsideX * outsideX + outsideY * outsideY) + std::min(std::max(x, y), 0.0f) - area.radius;
        if (distance > 1 && current_->window_ && !current_->posted_)
            current_->posted_ = PostMessageW(current_->window_, current_->message_, 0, 0) != FALSE;
    }
    // Observe only. The original input continues through the hook chain and reaches its target.
    return CallNextHookEx(nullptr, code, button, data);
}
}
