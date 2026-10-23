#include "NotificationPolicy.h"
#include <algorithm>
#include <cwctype>

namespace winisland
{
namespace
{
bool Equal(std::wstring_view a, std::wstring_view b) noexcept
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    return true;
}
std::array<std::uint64_t, 2> ContentFingerprint(const ChatNotification& item) noexcept
{
    std::array<std::uint64_t, 2> value{14695981039346656037ULL, 5381ULL};
    const auto add = [&](std::uint64_t code)
    {
        value[0] = (value[0] ^ code) * 1099511628211ULL;
        value[1] = ((value[1] << 5) + value[1]) ^ code;
    };
    add(static_cast<std::uint64_t>(item.app));
    for (const auto* text : {&item.appName, &item.title, &item.body})
    {
        add(text->size());
        for (const wchar_t code : *text) add(static_cast<std::uint16_t>(code));
    }
    return value;
}
bool Expired(const ChatNotification& value, NotificationQueue::Clock::time_point now) noexcept
{
    return value.receivedAt > now + std::chrono::seconds(2) ||
        now - value.receivedAt >= NotificationQueue::MaximumAge;
}
}

ChatApp IdentifyChatApp(std::wstring_view source, std::wstring_view executable) noexcept
{
    // Common desktop AUMIDs. A dotted prefix match would accidentally admit
    // unrelated Tencent products (QQ Music, browsers, game launchers, and so on).
    if (Equal(source, L"Tencent.QQ") || Equal(source, L"QQ") || Equal(source, L"com.tencent.qq"))
        return ChatApp::QQ;
    if (Equal(source, L"Tencent.WeChat") || Equal(source, L"WeChat") ||
        Equal(source, L"com.tencent.wechat") || Equal(source, L"Tencent.Weixin") || Equal(source, L"Weixin"))
        return ChatApp::WeChat;

    // executable is populated only by the shell/process resolver, never by toast text.
    // Require an absolute Windows path; a metadata string such as "qq.exe" is insufficient.
    const bool absolute = executable.size() > 3 &&
        ((executable[1] == L':' && (executable[2] == L'\\' || executable[2] == L'/')) ||
        (executable[0] == L'\\' && executable[1] == L'\\'));
    if (!absolute) return ChatApp::Unknown;
    const auto separator = executable.find_last_of(L"\\/");
    const auto filename = executable.substr(separator == std::wstring_view::npos ? 0 : separator + 1);
    if (Equal(filename, L"QQ.exe")) return ChatApp::QQ;
    if (Equal(filename, L"WeChat.exe") || Equal(filename, L"Weixin.exe")) return ChatApp::WeChat;
    return ChatApp::Unknown;
}

void NotificationTracker::Configure(bool enabled, bool qq, bool wechat, NotificationAccess access)
{
    if (enabled_ == enabled && qq_ == qq && wechat_ == wechat && access_ == access && !stopped_) return;
    enabled_ = enabled; qq_ = qq; wechat_ = wechat; access_ = access;
    stopped_ = false; baseline_ = true; previous_.clear();
}

std::vector<ChatNotification> NotificationTracker::Observe(const std::vector<ChatNotification>& snapshot, bool complete)
{
    std::vector<ChatNotification> changed;
    std::vector<Stamp> accepted;
    if (stopped_ || !enabled_ || access_ != NotificationAccess::Allowed) return changed;
    accepted.reserve(std::min<std::size_t>(snapshot.size(), 512));
    for (const auto& item : snapshot)
    {
        if ((item.app != ChatApp::QQ || !qq_) && (item.app != ChatApp::WeChat || !wechat_)) continue;
        if (accepted.size() >= 512) break;
        const auto found = std::find_if(previous_.begin(), previous_.end(),
            [&](const Stamp& old) { return old.id == item.id && old.source == item.source && old.createdAt == item.createdAt; });
        const auto fingerprint = ContentFingerprint(item);
        if (!baseline_ && (found == previous_.end() || found->fingerprint != fingerprint)) changed.push_back(item);
        accepted.push_back({item.id, item.source, item.createdAt, fingerprint});
    }
    if (!complete)
    {
        // A single malformed/vanished COM object must not discard other known
        // identities and replay them when they become readable on the next poll.
        for (const auto& old : previous_)
        {
            if (accepted.size() >= 512) break;
            const auto found = std::find_if(accepted.begin(), accepted.end(), [&](const Stamp& item)
            { return old.id == item.id && old.source == item.source && old.createdAt == item.createdAt; });
            if (found == accepted.end()) accepted.push_back(old);
        }
    }
    previous_ = std::move(accepted);
    if (complete) baseline_ = false;
    return changed;
}

void NotificationTracker::Stop() noexcept
{
    stopped_ = true; baseline_ = true; previous_.clear();
}

bool NotificationQueue::Push(ChatNotification value, Clock::time_point now)
{
    DiscardExpired(now);
    if (Expired(value, now)) return false;
    const auto found = std::find_if(queue_.begin(), queue_.end(), [&](const ChatNotification& item)
    { return item.id == value.id && item.source == value.source && item.createdAt == value.createdAt && item.test == value.test; });
    if (found != queue_.end()) { *found = std::move(value); return true; }
    if (queue_.size() >= Capacity) queue_.pop_front();
    queue_.push_back(std::move(value));
    return true;
}

std::optional<ChatNotification> NotificationQueue::Pop(Clock::time_point now)
{
    DiscardExpired(now);
    if (queue_.empty()) return std::nullopt;
    auto value = std::move(queue_.front()); queue_.pop_front();
    return value;
}

void NotificationQueue::DiscardExpired(Clock::time_point now)
{
    std::erase_if(queue_, [&](const ChatNotification& value) { return Expired(value, now); });
}
void NotificationQueue::Clear() noexcept { queue_.clear(); }
std::size_t NotificationQueue::Size() const noexcept { return queue_.size(); }
}
