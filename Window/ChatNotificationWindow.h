#pragma once
#include "../Core/Platform.h"
#include "../Services/NotificationTypes.h"
#include <functional>
#include <memory>

namespace winisland
{
// All calls and callbacks belong to the application's UI thread.
class ChatNotificationWindow
{
public:
    using OpenHandler = std::function<void(const ChatNotification&)>;
    explicit ChatNotificationWindow(HINSTANCE instance);
    ~ChatNotificationWindow();
    ChatNotificationWindow(const ChatNotificationWindow&) = delete;
    ChatNotificationWindow& operator=(const ChatNotificationWindow&) = delete;
    void Show(const ChatNotification& notification, RECT anchorScreen, UINT dpi,
        int durationSeconds, bool showBody, bool reducedMotion,
        size_t pendingCount, OpenHandler open);
    void Dismiss(bool immediate = false);
    void SetAnchor(RECT anchorScreen, UINT dpi);
    void SetPendingCount(size_t count);
    // Suppression discards the visible notification; unsuppressing never replays it.
    void SetSuppressed(bool suppressed);
    void SetDismissHandler(std::function<void()> dismissed);
    bool Visible() const noexcept;
    HWND Handle() const noexcept;
    void Close();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
