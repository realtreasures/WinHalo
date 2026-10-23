#pragma once
#include "MediaTypes.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace winisland
{
enum class ChatApp { Unknown, QQ, WeChat };
enum class NotificationAccess { Disabled, IdentityRequired, NotRequested, Allowed, Denied, Unavailable, Error };

struct ChatNotification
{
    std::uint32_t id = 0;
    ChatApp app = ChatApp::Unknown;
    std::wstring source, appName, title, body;
    std::shared_ptr<const MediaAppInfo> application;
    std::chrono::system_clock::time_point createdAt{};
    std::chrono::steady_clock::time_point receivedAt = std::chrono::steady_clock::now();
    bool test = false;
};

struct NotificationStatus
{
    NotificationAccess access = NotificationAccess::Disabled;
    // Diagnostics contain fixed status text only, never notification contents.
    std::wstring detail;
};
}
