#pragma once
#include "NotificationTypes.h"
#include <functional>
#include <memory>
#include <thread>

namespace winisland
{
class Logger;
struct NotificationServiceState;

class NotificationService
{
public:
    explicit NotificationService(Logger& logger);
    ~NotificationService();
    NotificationService(const NotificationService&) = delete;
    NotificationService& operator=(const NotificationService&) = delete;

    // Lifecycle and configuration are controlled by the UI; callbacks run on the worker.
    void Start(std::function<void(ChatNotification)> sink,
        std::function<void(NotificationStatus)> statusSink);
    void Configure(bool enabled, bool qq, bool wechat);
    void Stop() noexcept;
    // Does not request access or read notifications. Caller must initialize WinRT.
    static NotificationStatus ReadAccess() noexcept;

private:
    Logger& logger_;
    std::shared_ptr<NotificationServiceState> state_;
    std::jthread worker_;
};
}
