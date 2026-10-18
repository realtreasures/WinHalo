#pragma once
#include "../Graphics/CompositionRenderer.h"
#include "../Window/IslandWindow.h"
#include "Configuration.h"
#include "../Services/MediaService.h"
#include "../Services/VolumeService.h"
#include "../Services/BatteryService.h"
#include "../Services/FullscreenMonitor.h"
#include "../Services/StartupRegistration.h"
#include <chrono>
#include <memory>

namespace winisland
{
struct ServiceMailbox;
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
    DWORD ContentTimeout() const noexcept;
    std::wstring Diagnostics() const;
    // 服务先停止，再销毁窗口；日志最后释放。
    Logger logger_;
    ConfigurationStore store_;
    Configuration configuration_;
    IslandView view_;
    CompositionRenderer renderer_;
    IslandWindow window_;
    std::shared_ptr<ServiceMailbox> mailbox_;
    MediaService mediaService_;
    VolumeService volumeService_;
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
