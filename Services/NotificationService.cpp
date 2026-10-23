#include "NotificationService.h"
#include "NotificationPolicy.h"
#include "MediaAppResolver.h"
#include "MediaInterop.h"
#include "../Core/Platform.h"
#include "../Utils/Logger.h"
#include <appmodel.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

namespace winisland
{
namespace
{
using namespace std::chrono_literals;
using winrt::Windows::UI::Notifications::UserNotification;
using winrt::Windows::UI::Notifications::NotificationKinds;
using winrt::Windows::UI::Notifications::KnownNotificationBindings;
using winrt::Windows::UI::Notifications::Management::UserNotificationListener;
using winrt::Windows::UI::Notifications::Management::UserNotificationListenerAccessStatus;
using Clock = std::chrono::steady_clock;
struct Apartment
{
    Apartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    ~Apartment() { winrt::uninit_apartment(); }
};

std::wstring Limited(const winrt::hstring& text, std::size_t maximum)
{
    std::wstring result;
    result.reserve(std::min<std::size_t>(text.size(), maximum));
    for (const wchar_t value : text)
    {
        if (result.size() >= maximum) break;
        if (value >= L' ' || value == L'\n' || value == L'\t') result += value;
    }
    // Never split a UTF-16 surrogate pair at the size limit.
    if (!result.empty() && result.back() >= 0xD800 && result.back() <= 0xDBFF) result.pop_back();
    return result;
}
}

struct NotificationServiceState
{
    std::mutex mutex;
    std::condition_variable_any changed;
    bool enabled = false, qq = true, wechat = true, stopping = false, dirty = true;
    std::uint64_t revision = 0;
    std::function<void(ChatNotification)> sink;
    std::function<void(NotificationStatus)> statusSink;
};

namespace
{
void Signal(const std::weak_ptr<NotificationServiceState>& weak) noexcept
{
    if (const auto state = weak.lock())
    {
        { const std::lock_guard lock(state->mutex); if (state->stopping) return; state->dirty = true; }
        state->changed.notify_all();
    }
}

bool PublishStatus(const std::shared_ptr<NotificationServiceState>& state,
    const NotificationStatus& value, std::uint64_t revision) noexcept
{
    try
    {
        std::function<void(NotificationStatus)> sink;
        { const std::lock_guard lock(state->mutex);
            if (state->stopping || state->revision != revision) return false;
            sink = state->statusSink; }
        if (sink) sink(value);
        return true;
    }
    catch (...) { return false; }
}
void Publish(const std::shared_ptr<NotificationServiceState>& state,
    ChatNotification value, std::uint64_t revision) noexcept
{
    try
    {
        std::function<void(ChatNotification)> sink;
        { const std::lock_guard lock(state->mutex);
            if (state->stopping || state->revision != revision || !state->enabled) return;
            sink = state->sink; }
        if (sink) sink(std::move(value));
    }
    catch (...) {}
}

struct ResolvedApp
{
    std::shared_ptr<const MediaAppInfo> application;
    ChatApp app = ChatApp::Unknown;
};

MediaAppInfo ResolveChatApplication(const std::wstring& source)
{
    auto application = ResolveMediaApp(source);
    if (application.executable.empty())
    {
        // A closed/background-only desktop app may have no discoverable window.
        // Resolve the shell's registered target path rather than guessing by name.
        winrt::com_ptr<IShellItem2> item;
        if (SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, 0, source.c_str(),
            IID_PPV_ARGS(item.put()))))
        {
            PWSTR target = nullptr;
            if (SUCCEEDED(item->GetString(PKEY_Link_TargetParsingPath, &target)))
            {
                if (target && IdentifyChatApp({}, target) != ChatApp::Unknown &&
                    GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES)
                    application.executable = target;
                CoTaskMemFree(target);
            }
        }
    }
    return application;
}

std::optional<ChatNotification> ReadNotification(const UserNotification& notification,
    std::unordered_map<std::wstring, ResolvedApp>& apps, bool qq, bool wechat, bool& complete)
{
    using media_interop::Read;
    using namespace winrt::Windows::UI::Notifications;
    if (!notification) return std::nullopt;
    winrt::Windows::ApplicationModel::AppInfo appInfo{nullptr};
    if (FAILED(Read(notification, &media_interop::Abi<UserNotification>::get_AppInfo, appInfo)))
    { complete = false; return std::nullopt; }
    if (!appInfo) return std::nullopt;
    winrt::hstring identity;
    if (FAILED(Read(appInfo, &media_interop::Abi<decltype(appInfo)>::get_AppUserModelId, identity)))
    { complete = false; return std::nullopt; }
    const auto source = Limited(identity, 512);
    if (source.empty()) return std::nullopt;
    auto found = apps.find(source);
    if (found == apps.end())
    {
        // Resolve only OS-provided AUMIDs, never notification display names or text.
        // Bound this cache even if other applications continually post notifications.
        if (apps.size() >= 128) apps.clear();
        auto resolved = std::make_shared<const MediaAppInfo>(ResolveChatApplication(source));
        const auto app = IdentifyChatApp(source, resolved->executable);
        found = apps.emplace(source, ResolvedApp{std::move(resolved), app}).first;
    }
    if (found->second.app == ChatApp::Unknown) return std::nullopt;
    if ((found->second.app == ChatApp::QQ && !qq) ||
        (found->second.app == ChatApp::WeChat && !wechat)) return std::nullopt;
    ChatNotification result;
    if (FAILED(Read(notification, &media_interop::Abi<UserNotification>::get_Id, result.id)))
    { complete = false; return std::nullopt; }
    result.app = found->second.app; result.source = source;
    result.application = found->second.application;
    winrt::Windows::ApplicationModel::AppDisplayInfo display{nullptr};
    winrt::hstring displayName;
    if (SUCCEEDED(Read(appInfo, &media_interop::Abi<decltype(appInfo)>::get_DisplayInfo, display)) && display)
        Read(display, &media_interop::Abi<decltype(display)>::get_DisplayName, displayName);
    result.appName = Limited(displayName, 128);
    if (result.appName.empty()) result.appName = result.app == ChatApp::QQ ? L"QQ" : L"微信";
    winrt::Windows::Foundation::DateTime created{};
    if (FAILED(Read(notification, &media_interop::Abi<UserNotification>::get_CreationTime, created)))
    { complete = false; return std::nullopt; }
    result.createdAt = winrt::clock::to_sys(created);
    result.receivedAt = Clock::now();
    Notification content{nullptr};
    if (FAILED(Read(notification, &media_interop::Abi<UserNotification>::get_Notification, content)))
    { complete = false; return std::nullopt; }
    if (content)
    {
        NotificationVisual visual{nullptr};
        if (FAILED(Read(content, &media_interop::Abi<Notification>::get_Visual, visual)))
        { complete = false; return std::nullopt; }
        if (visual)
        {
            NotificationBinding binding{nullptr};
            const winrt::hstring generic{L"ToastGeneric"};
            if (FAILED(Read(visual, &media_interop::Abi<NotificationVisual>::GetBinding,
                binding, winrt::get_abi(generic))))
            { complete = false; return std::nullopt; }
            if (!binding)
            {
                // Legacy text templates are still produced by some desktop clients.
                decltype(visual.Bindings()) bindings{nullptr}; std::uint32_t count = 0;
                if (SUCCEEDED(Read(visual, &media_interop::Abi<NotificationVisual>::get_Bindings, bindings)) &&
                    bindings && SUCCEEDED(Read(bindings, &media_interop::Abi<decltype(bindings)>::get_Size, count)) && count)
                    if (FAILED(Read(bindings, &media_interop::Abi<decltype(bindings)>::GetAt, binding, std::uint32_t{0})))
                    { complete = false; return std::nullopt; }
            }
            if (binding)
            {
                decltype(binding.GetTextElements()) texts{nullptr}; std::uint32_t count = 0;
                if (FAILED(Read(binding, &media_interop::Abi<NotificationBinding>::GetTextElements, texts)))
                { complete = false; return std::nullopt; }
                if (texts && FAILED(Read(texts, &media_interop::Abi<decltype(texts)>::get_Size, count)))
                { complete = false; return std::nullopt; }
                count = std::min<std::uint32_t>(count, 8);
                for (std::uint32_t index = 0; index < count; ++index)
                {
                    AdaptiveNotificationText text{nullptr}; winrt::hstring value;
                    if (FAILED(Read(texts, &media_interop::Abi<decltype(texts)>::GetAt, text, index)))
                    { complete = false; return std::nullopt; }
                    if (!text) continue;
                    if (FAILED(Read(text, &media_interop::Abi<AdaptiveNotificationText>::get_Text, value)))
                    { complete = false; return std::nullopt; }
                    if (index == 0) result.title = Limited(value, 256);
                    else if (result.body.size() < 1024)
                    {
                        if (!result.body.empty()) result.body += L'\n';
                        result.body += Limited(value, 1024 - result.body.size());
                    }
                }
            }
        }
    }
    if (result.title.empty()) result.title = result.appName;
    if (result.body.empty()) result.body = L"收到一条新消息";
    return result;
}
}

NotificationStatus NotificationService::ReadAccess() noexcept
{
    UINT32 length = 0;
    const LONG identity = GetCurrentPackageFullName(&length, nullptr);
    if (identity == APPMODEL_ERROR_NO_PACKAGE)
        return { NotificationAccess::IdentityRequired, L"需要安装带应用身份的 WinHalo 版本" };
    if (identity != ERROR_INSUFFICIENT_BUFFER && identity != ERROR_SUCCESS)
        return { NotificationAccess::Error, L"无法检查应用身份" };
    try
    {
        const auto listener = UserNotificationListener::Current();
        std::int32_t raw = 0;
        const HRESULT code = static_cast<media_interop::Abi<UserNotificationListener>*>(
            winrt::get_abi(listener))->GetAccessStatus(&raw);
        if (FAILED(code))
            return { NotificationAccess::Unavailable, L"Windows 通知访问暂时不可用" };
        switch (static_cast<UserNotificationListenerAccessStatus>(raw))
        {
        case UserNotificationListenerAccessStatus::Allowed:
            return { NotificationAccess::Allowed, L"已允许读取 QQ 和微信的系统通知" };
        case UserNotificationListenerAccessStatus::Denied:
            return { NotificationAccess::Denied, L"通知访问已拒绝，请在 Windows 设置中允许" };
        default:
            return { NotificationAccess::NotRequested, L"尚未申请 Windows 通知访问权限" };
        }
    }
    catch (...) { return { NotificationAccess::Unavailable, L"Windows 通知接口暂时不可用" }; }
}

NotificationService::NotificationService(Logger& logger)
    : logger_(logger), state_(std::make_shared<NotificationServiceState>()) {}
NotificationService::~NotificationService() { Stop(); }

void NotificationService::Configure(bool enabled, bool qq, bool wechat)
{
    { const std::lock_guard lock(state_->mutex);
        state_->enabled = enabled; state_->qq = qq; state_->wechat = wechat;
        ++state_->revision; state_->dirty = true; }
    state_->changed.notify_all();
}

void NotificationService::Start(std::function<void(ChatNotification)> sink,
    std::function<void(NotificationStatus)> statusSink)
{
    if (worker_.joinable()) return;
    { const std::lock_guard lock(state_->mutex);
        state_->sink = std::move(sink); state_->statusSink = std::move(statusSink);
        state_->stopping = false; state_->dirty = true; }
    const auto state = state_;
    worker_ = std::jthread([state, this](std::stop_token stop)
    {
        try
        {
            Apartment apartment;
            NotificationTracker tracker;
            UserNotificationListener listener{nullptr};
            UserNotificationListener::NotificationChanged_revoker changed;
            std::unordered_map<std::wstring, ResolvedApp> apps;
            NotificationStatus last{NotificationAccess::Error, {}};
            bool hasLast = false;
            std::uint64_t lastPublishedRevision = static_cast<std::uint64_t>(-1);
            std::uint64_t previousRevision = static_cast<std::uint64_t>(-1);
            while (!stop.stop_requested())
            {
                bool enabled, qq, wechat; std::uint64_t revision;
                { const std::lock_guard lock(state->mutex);
                    if (state->stopping) break;
                    enabled = state->enabled; qq = state->qq; wechat = state->wechat;
                    revision = state->revision; state->dirty = false; }
                auto status = enabled ? ReadAccess() :
                    NotificationStatus{NotificationAccess::Disabled, L"聊天消息提醒已关闭"};
                if (revision != previousRevision)
                {
                    tracker.Stop(); // Even an identical Configure requests a fresh access/baseline check.
                    previousRevision = revision;
                }
                tracker.Configure(enabled, qq, wechat, status.access);
                if (status.access == NotificationAccess::Allowed && (qq || wechat))
                {
                    try
                    {
                        if (!listener)
                        {
                            listener = UserNotificationListener::Current();
                            using Callback = winrt::Windows::Foundation::TypedEventHandler<
                                UserNotificationListener, winrt::Windows::UI::Notifications::UserNotificationChangedEventArgs>;
                            const Callback callback{[weak = std::weak_ptr(state)](const auto&, const auto&) noexcept
                            { Signal(weak); }};
                            // Event registration can be unsupported on some desktop systems;
                            // the two-second bounded poll remains operational in that case.
                            media_interop::Subscribe(listener,
                                &media_interop::Abi<UserNotificationListener>::add_NotificationChanged,
                                callback, changed);
                        }
                        using Notifications = winrt::Windows::Foundation::Collections::IVectorView<UserNotification>;
                        winrt::Windows::Foundation::IAsyncOperation<Notifications> operation{nullptr};
                        HRESULT code = media_interop::Read(listener,
                            &media_interop::Abi<UserNotificationListener>::GetNotificationsAsync,
                            operation, static_cast<std::uint32_t>(NotificationKinds::Toast));
                        if (SUCCEEDED(code))
                        {
                            const auto result = media_interop::Await(operation, stop, Clock::now() + 2500ms);
                            code = result.code;
                            if (SUCCEEDED(code))
                            {
                                std::vector<ChatNotification> snapshot;
                                bool complete = true;
                                std::uint32_t count = 0;
                                if (result.value)
                                    code = media_interop::Read(result.value,
                                        &media_interop::Abi<Notifications>::get_Size, count);
                                count = std::min<std::uint32_t>(count, 1024);
                                for (std::uint32_t i = 0; i < count && !stop.stop_requested(); ++i)
                                {
                                    try
                                    {
                                        UserNotification notification{nullptr};
                                        if (FAILED(media_interop::Read(result.value,
                                            &media_interop::Abi<Notifications>::GetAt, notification, i)))
                                        { complete = false; continue; }
                                        if (auto item = ReadNotification(notification, apps, qq, wechat, complete))
                                            snapshot.push_back(std::move(*item));
                                    }
                                    catch (...) { complete = false; } // Isolate an unreadable notification.
                                }
                                bool current;
                                { const std::lock_guard lock(state->mutex);
                                    current = !state->stopping && state->revision == revision; }
                                if (current)
                                    for (auto& item : tracker.Observe(snapshot, complete)) Publish(state, std::move(item), revision);
                            }
                        }
                        if (FAILED(code) && !stop.stop_requested())
                            status = {NotificationAccess::Error, L"通知读取暂时失败，正在重试"};
                    }
                    catch (...) { status = {NotificationAccess::Error, L"通知读取暂时失败，正在重试"}; }
                }
                else
                {
                    changed.revoke(); listener = nullptr; apps.clear();
                }
                if (status.access == NotificationAccess::Error)
                {
                    tracker.Configure(enabled, qq, wechat, status.access);
                    changed.revoke(); listener = nullptr; apps.clear();
                }
                if (!hasLast || lastPublishedRevision != revision ||
                    status.access != last.access || status.detail != last.detail)
                {
                    if (PublishStatus(state, status, revision))
                    {
                        last = status; hasLast = true; lastPublishedRevision = revision;
                    }
                }
                std::unique_lock lock(state->mutex);
                const auto pending = [&] { return state->stopping || state->dirty; };
                if (enabled && (qq || wechat)) state->changed.wait_for(lock, stop, 2s, pending);
                else state->changed.wait(lock, stop, pending);
            }
            tracker.Stop(); changed.revoke();
        }
        catch (...)
        {
            logger_.Write("Chat notification worker unavailable; no notification content logged.");
            std::uint64_t revision;
            { const std::lock_guard lock(state->mutex); revision = state->revision; }
            PublishStatus(state, {NotificationAccess::Unavailable, L"无法启动 Windows 通知监听"}, revision);
        }
    });
}

void NotificationService::Stop() noexcept
{
    { const std::lock_guard lock(state_->mutex); state_->stopping = true; state_->dirty = true; }
    if (worker_.joinable())
    {
        worker_.request_stop(); state_->changed.notify_all(); worker_.join();
    }
    { const std::lock_guard lock(state_->mutex); state_->sink = {}; state_->statusSink = {}; }
}
}
