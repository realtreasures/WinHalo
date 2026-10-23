#pragma once
#include "../Graphics/CompositionRenderer.h"
#include "../Window/IslandWindow.h"
#include "Configuration.h"
#include "../Services/MediaService.h"
#include "../Services/VolumeService.h"
#include "../Services/BatteryService.h"
#include "../Services/FullscreenMonitor.h"
#include "../Services/StartupRegistration.h"
#include "../Services/BehaviorService.h"
#include "../Window/SettingsWindow.h"
#include "../Services/NotificationService.h"
#include "../Services/NotificationPolicy.h"
#include "../Window/ChatNotificationWindow.h"
#include <chrono>
#include <memory>
#include <optional>

namespace winisland
{
struct ServiceMailbox;
struct ChatAuthorization;
class Application
{
public:
    explicit Application(HINSTANCE instance);
    ~Application();
    int Run();
private:
    void OnWindowEvent(WindowEvent event);
    void AnimateFrame();
    void ApplyAction(ViewAction action);
    void DrainServices();
    void RefreshTimedContent();
    void RefreshBattery();
    void RefreshDesktop();
    void ReadSystemOptions(std::wstring error = {});
    void ShowSettings();
    void ApplyConfiguration(Configuration configuration);
    void RefreshSettings();
    void OnWheel(int steps, bool brightness);
    void OnClipboard();
    void RequestChatAccess();
    void TestChatNotification();
    void RefreshChatStatus();
    void RefreshChatWindow();
    void PositionChatWindow();
    RECT ChatAnchor() const;
    void DisplayChat(ChatNotification notification);
    void CancelChatAuthorization() noexcept;
    DWORD ContentTimeout() const noexcept;
    std::wstring Diagnostics() const;
    // 服务先停止，再销毁窗口；日志最后释放。
    Logger logger_;
    ConfigurationStore store_;
    Configuration configuration_;
    IslandView view_;
    CompositionRenderer renderer_;
    IslandWindow window_;
    SettingsWindow settings_;
    ChatNotificationWindow chatWindow_;
    SettingsStatus settingsStatus_;
    std::shared_ptr<ServiceMailbox> mailbox_;
    MediaService mediaService_;
    VolumeService volumeService_;
    BehaviorService behaviorService_;
    NotificationService notificationService_;
    NotificationQueue notificationQueue_;
    NotificationStatus notificationStatus_;
    std::optional<ChatNotification> activeChat_;
    std::shared_ptr<ChatAuthorization> chatAuthorization_;
    bool sessionLocked_ = false;
    bool chatFullscreen_ = false;
    bool sessionNotifications_ = false;
    BehaviorSnapshot behavior_;
    std::wstring pendingLink_, lastLink_;
    bool brightnessRequested_ = false;
    std::optional<float> pendingVolume_;
    std::chrono::steady_clock::time_point volumeInputAt_{};
    BatteryService batteryService_;
    FullscreenMonitor fullscreenMonitor_;
    StartupRegistration startup_;
    bool integrationEnabled_ = true;
    using Clock = std::chrono::steady_clock;
    Clock::time_point previousFrame_ = Clock::now();
    Clock::time_point previousMetric_ = Clock::now();
    Clock::time_point toastUntil_{};
    Clock::time_point nextProgress_{};
    Clock::time_point nextClock_{};
    double animationMilliseconds_ = 0;
    double cpuMilliseconds_ = 0;
    double measuredFps_ = 0;
    unsigned long long metricFrames_ = 0;
    unsigned long long metricBaseline_ = 0;
    unsigned int stressRemaining_ = 0;
    double stressElapsed_ = 0;
};
}
