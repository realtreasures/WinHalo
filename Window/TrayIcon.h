#pragma once
#include "../Core/Platform.h"
#include <shellapi.h>

namespace winisland
{
// Shell owns the displayed image; the resource icon itself remains shared.
class TrayIcon
{
public:
    static constexpr UINT CallbackMessage = WM_APP + 21;
    static constexpr UINT Identifier = 1;
    TrayIcon() = default;
    ~TrayIcon() { Remove(); }
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;
    bool Create(HWND owner, HICON icon) noexcept;
    bool Restore() noexcept;
    void Remove() noexcept;
    bool Registered() const noexcept { return registered_; }

private:
    NOTIFYICONDATAW data_{};
    bool registered_ = false;
};
}
