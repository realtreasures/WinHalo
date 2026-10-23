#pragma once
#include "NotificationTypes.h"
#include <array>
#include <deque>
#include <optional>
#include <string_view>
#include <vector>

namespace winisland
{
// Exact application identities or an executable path resolved from the OS are
// accepted. Notification display names and notification text are never identities.
ChatApp IdentifyChatApp(std::wstring_view appUserModelId,
    std::wstring_view resolvedExecutable = {}) noexcept;

class NotificationTracker
{
public:
    void Configure(bool enabled, bool qq, bool wechat, NotificationAccess access);
    // First successful scan after configuration/access changes is a silent baseline.
    std::vector<ChatNotification> Observe(const std::vector<ChatNotification>& snapshot, bool complete = true);
    void Stop() noexcept;
private:
    bool enabled_ = false, qq_ = true, wechat_ = true, stopped_ = false, baseline_ = true;
    NotificationAccess access_ = NotificationAccess::Disabled;
    struct Stamp
    {
        std::uint32_t id;
        std::wstring source;
        std::chrono::system_clock::time_point createdAt;
        std::array<std::uint64_t, 2> fingerprint;
    };
    // Baselines retain no titles, message bodies, sender names, or artwork.
    std::vector<Stamp> previous_;
};

class NotificationQueue
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t Capacity = 20;
    static constexpr auto MaximumAge = std::chrono::seconds(30);
    bool Push(ChatNotification notification, Clock::time_point now = Clock::now());
    std::optional<ChatNotification> Pop(Clock::time_point now = Clock::now());
    void DiscardExpired(Clock::time_point now = Clock::now());
    void Clear() noexcept;
    std::size_t Size() const noexcept;
private:
    std::deque<ChatNotification> queue_;
};
}
