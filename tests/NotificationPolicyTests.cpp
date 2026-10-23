#include "../Services/NotificationPolicy.h"
#include "../Services/NotificationService.h"
#include "../Utils/Logger.h"
#include "../Core/Platform.h"
#include <appmodel.h>
#include <winrt/base.h>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>

namespace
{
using namespace winisland;
using namespace std::chrono_literals;
void Check(bool value, const char* message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
ChatNotification Item(std::uint32_t id, NotificationQueue::Clock::time_point now,
    ChatApp app = ChatApp::QQ)
{
    ChatNotification value;
    value.id = id; value.app = app;
    value.source = app == ChatApp::QQ ? L"Tencent.QQ" : L"Tencent.WeChat";
    value.title = L"Synthetic sender"; value.body = L"Synthetic content";
    value.createdAt = std::chrono::system_clock::time_point{std::chrono::seconds(id)};
    value.receivedAt = now;
    return value;
}

void DisabledServiceLifecycle()
{
    // Isolate logs from the user's configuration. The worker remains disabled
    // throughout every lifecycle; it must never enumerate system notifications.
    const auto directory = std::filesystem::absolute(std::filesystem::path(L"artifacts") /
        (L"notification-disabled-lifecycle-" + std::to_wstring(GetCurrentProcessId())));
    Check(SetEnvironmentVariableW(L"WINHALO_DATA_DIR", directory.c_str()) != FALSE,
        "set isolated lifecycle log folder");
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    UINT32 packageLength = 0;
    const LONG identity = GetCurrentPackageFullName(&packageLength, nullptr);
    const auto access = NotificationService::ReadAccess();
    Check(!access.detail.empty(), "read-only access check supplies status");
    if (identity == APPMODEL_ERROR_NO_PACKAGE)
        Check(access.access == NotificationAccess::IdentityRequired, "unpackaged identity check");
    winrt::uninit_apartment();

    Logger logger;
    std::atomic<unsigned int> messages = 0;
    for (unsigned int iteration = 0; iteration < 8; ++iteration)
    {
        NotificationService service(logger);
        std::mutex mutex;
        std::condition_variable ready;
        unsigned int statuses = 0;
        bool validStatuses = true;
        service.Configure(false, true, true);
        service.Start([&](ChatNotification) { ++messages; }, [&](NotificationStatus status)
        {
            bool configureAgain;
            {
                const std::lock_guard lock(mutex);
                validStatuses = validStatuses && status.access == NotificationAccess::Disabled;
                configureAgain = ++statuses == 1;
            }
            // Exercise the revision changing inside a successful publication.
            // The following iteration must republish the same access state for
            // the new revision rather than suppressing it as an unchanged value.
            if (configureAgain) service.Configure(false, true, true);
            ready.notify_all();
        });
        {
            std::unique_lock lock(mutex);
            Check(ready.wait_for(lock, 2s, [&] { return statuses >= 2; }),
                "disabled callback and reentrant configuration republish");
            Check(validStatuses, "disabled worker publishes Disabled only");
        }
        service.Configure(false, false, false);
        {
            std::unique_lock lock(mutex);
            Check(ready.wait_for(lock, 2s, [&] { return statuses >= 3; }),
                "configuration wakes indefinitely waiting disabled worker");
        }
        for (unsigned int change = 0; change < 100; ++change)
            service.Configure(false, (change % 2) == 0, (change % 3) == 0);
        service.Configure(false, false, false);
        const auto stopping = NotificationQueue::Clock::now();
        service.Stop();
        service.Stop();
        Check(NotificationQueue::Clock::now() - stopping < 1500ms,
            "disabled and repeated Stop remain bounded");
        unsigned int stoppedCount;
        { const std::lock_guard lock(mutex); stoppedCount = statuses; }
        // Configure after Stop must not resurrect a worker or callbacks.
        service.Configure(false, true, true);
        {
            std::unique_lock lock(mutex);
            Check(!ready.wait_for(lock, 30ms, [&] { return statuses != stoppedCount; }),
                "no callbacks after Stop returns");
            Check(validStatuses, "disabled statuses preserved across rapid reconfiguration");
        }
    }
    Check(messages.load() == 0, "disabled lifecycles never deliver messages");
}
}

int main()
{
    using namespace winisland;
    using namespace std::chrono_literals;
    // Policy cases use only synthetic memory data. The final lifecycle cases
    // activate a disabled worker and a read-only access check; no permissions,
    // notification enumeration, system settings, or clipboard are accessed.
    Check(IdentifyChatApp(L"Tencent.QQ") == ChatApp::QQ, "QQ AUMID");
    Check(IdentifyChatApp(L"tencent.wechat") == ChatApp::WeChat, "case-insensitive WeChat AUMID");
    Check(IdentifyChatApp(L"Tencent.Weixin") == ChatApp::WeChat, "Weixin AUMID");
    Check(IdentifyChatApp(L"Tencent.QQMusic") == ChatApp::Unknown, "exclude QQ Music");
    Check(IdentifyChatApp(L"random.qq.helper") == ChatApp::Unknown, "exclude arbitrary QQ substring");
    Check(IdentifyChatApp(L"WeChat Support") == ChatApp::Unknown, "exclude display-name match");
    Check(IdentifyChatApp(L"random", L"QQ.exe") == ChatApp::Unknown, "reject unresolved exe name");
    Check(IdentifyChatApp(L"random", L"C:\\Program Files\\Tencent\\QQ.exe") == ChatApp::QQ,
        "OS-resolved exact QQ executable");
    Check(IdentifyChatApp(L"random", L"C:\\Apps\\Weixin.exe") == ChatApp::WeChat, "Weixin executable");
    Check(IdentifyChatApp(L"random", L"C:\\Apps\\fakeQQ.exe") == ChatApp::Unknown, "exact executable only");
    Check(IdentifyChatApp(L"random", L"C:\\Apps\\QQMusic.exe") == ChatApp::Unknown, "exclude music executable");

    const auto now = NotificationQueue::Clock::time_point{100s};
    auto first = Item(1, now), second = Item(2, now), wechat = Item(3, now, ChatApp::WeChat);
    NotificationTracker tracker;
    tracker.Configure(true, true, true, NotificationAccess::Allowed);
    Check(tracker.Observe({first}).empty(), "initial scan is baseline");
    Check(tracker.Observe({first}).empty(), "unchanged notifications do not replay");
    auto updates = tracker.Observe({first, second});
    Check(updates.size() == 1 && updates.front().id == 2, "new notification emits once");
    second.body = L"Updated synthetic content";
    updates = tracker.Observe({first, second});
    Check(updates.size() == 1 && updates.front().body == second.body, "same notification update emits");
    Check(tracker.Observe({first, second}).empty(), "updated notification deduplicates");
    Check(tracker.Observe({first}, false).empty(), "one unreadable object does not replay others");
    Check(tracker.Observe({first, second}).empty(), "recovered object retains dedup fingerprint");
    Check(tracker.Observe({}).empty(), "removal does not emit");
    second.createdAt += 1s;
    updates = tracker.Observe({second});
    Check(updates.size() == 1, "reused id with new creation time emits");
    tracker.Configure(true, true, true, NotificationAccess::Denied);
    Check(tracker.Observe({first, second}).empty(), "denied access emits nothing");
    tracker.Configure(true, true, true, NotificationAccess::Allowed);
    Check(tracker.Observe({first, second}).empty(), "permission restoration resets baseline");
    tracker.Configure(true, false, true, NotificationAccess::Allowed);
    Check(tracker.Observe({first, second}).empty(), "source switch resets baseline");
    updates = tracker.Observe({first, second, wechat});
    Check(updates.size() == 1 && updates.front().app == ChatApp::WeChat, "independent source switches");
    auto unknown = Item(5, now, ChatApp::Unknown);
    Check(tracker.Observe({first, second, wechat, unknown}).empty(), "unknown sources never emit");
    tracker.Configure(false, true, true, NotificationAccess::Allowed);
    Check(tracker.Observe({first}).empty(), "disabled emits nothing");
    tracker.Configure(true, true, true, NotificationAccess::Allowed);
    Check(tracker.Observe({first}).empty(), "reenabling does not replay");
    tracker.Stop();
    Check(tracker.Observe({second}).empty(), "stopped tracker emits nothing");
    tracker.Configure(true, true, true, NotificationAccess::Allowed);
    Check(tracker.Observe({first}, false).empty(), "partial first scan is silent");
    Check(tracker.Observe({first, second}).empty(), "complete baseline after partial scan is silent");

    NotificationQueue queue;
    Check(queue.Push(first, now), "queue accepts fresh item");
    auto replacement = first; replacement.body = L"replacement";
    Check(queue.Push(replacement, now) && queue.Size() == 1, "queue updates same identity in place");
    const auto popped = queue.Pop(now);
    Check(popped && popped->body == replacement.body, "pop returns updated content");
    Check(!queue.Pop(now), "empty pop");
    auto reused = first; reused.createdAt += 1s;
    queue.Push(first, now); queue.Push(reused, now);
    Check(queue.Size() == 2, "reused notification ID with a new creation time is a separate queued message");
    queue.Clear();
    for (std::uint32_t index = 0; index < 25; ++index) queue.Push(Item(index, now), now);
    Check(queue.Size() == 20 && queue.Pop(now)->id == 5, "queue bound drops oldest");
    queue.DiscardExpired(now + 30s);
    Check(queue.Size() == 0, "thirty-second expiry removes backlog");
    auto stale = Item(30, now - 31s);
    Check(!queue.Push(stale, now), "expired arrival rejected");
    auto future = Item(31, now + 10s);
    Check(!queue.Push(future, now), "invalid future clock rejected");
    queue.Push(first, now); queue.Clear();
    Check(queue.Size() == 0, "disable/lock can clear queued messages");
    DisabledServiceLifecycle();
    std::cout << "PASS notification allowlist, baseline, dedup, updates, removal, denial, switches, bounded queue, expiry, read-only access and disabled-service lifecycles\n";
    return 0;
}
