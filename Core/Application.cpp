#include "Application.h"
#include "../Utils/Win32Error.h"
#include <exception>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <optional>
#include <algorithm>
#include <type_traits>

namespace winisland
{
// 每种服务只保留最新状态，事件洪水不会向 UI 堆积大批封面或消息。
struct ServiceMailbox
{
    std::mutex mutex;
    HWND window = nullptr;
    bool posted = false;
    std::optional<MediaSnapshot> media;
    std::optional<VolumeSnapshot> volume;

    template<class Snapshot> void Publish(Snapshot snapshot)
    {
        const std::lock_guard lock(mutex);
        if (!window) return;
        if constexpr (std::is_same_v<Snapshot, MediaSnapshot>) media = std::move(snapshot);
        else
        {
            // 合并时保留真实变化标志，避免紧随其后的刷新吞掉通知。
            if (volume) snapshot.notify = snapshot.notify || volume->notify;
            volume = std::move(snapshot);
        }
        if (!posted) posted = PostMessageW(window, IslandWindow::ServiceMessage, 0, 0) != FALSE;
    }
};

Application::Application(HINSTANCE instance)
    : store_(logger_), configuration_(store_.Load()), view_(configuration_.layout),
      renderer_(logger_), window_(instance, renderer_, view_, logger_),
      mailbox_(std::make_shared<ServiceMailbox>()), mediaService_(logger_), volumeService_(logger_)
{
    view_.Configure(configuration_.spring, configuration_.reducedMotion);
    view_.SetSystemOptions(false, configuration_.autoHideFullscreen);
    window_.SetDiagnostics([this] { return Diagnostics(); });
    window_.SetServiceEventHandler([this] { DrainServices(); });
}

Application::~Application()
{
    {
        const std::lock_guard lock(mailbox_->mutex);
        mailbox_->window = nullptr;
    }
    mediaService_.Stop();
    volumeService_.Stop();
    fullscreenMonitor_.Stop();
    batteryService_.Stop();
}

int Application::Run()
{
    logger_.Write("Starting WinHalo (GPU rendering and spring animation)");
    try
    {
        integrationEnabled_ = GetEnvironmentVariableW(L"WINHALO_DISABLE_SERVICES", nullptr, 0) == 0;
        window_.Create([this](WindowEvent event) { OnWindowEvent(event); }, false);
        window_.SetNativeGlass(configuration_.nativeGlass);
        {
            const std::lock_guard lock(mailbox_->mutex);
            mailbox_->window = window_.Handle();
        }
        // 自动交互测试可隔离正在播放的用户会话；正常启动始终使用真实服务。
        ReadSystemOptions();
        if (integrationEnabled_)
        {
            batteryService_.Start(window_.Handle());
            if (!fullscreenMonitor_.Start(window_.Handle(), IslandWindow::DesktopMessage))
            {
                logger_.Write("Fullscreen event hooks unavailable");
                ReadSystemOptions(L"无法监听全屏变化，请重新启动后重试");
            }
            const std::weak_ptr<ServiceMailbox> weak = mailbox_;
            mediaService_.Start([weak](MediaSnapshot snapshot) {
                if (const auto mailbox = weak.lock()) mailbox->Publish(std::move(snapshot));
            });
            volumeService_.Start([weak](VolumeSnapshot snapshot) {
                if (const auto mailbox = weak.lock()) mailbox->Publish(std::move(snapshot));
            });
        }
        // 创建时保持隐藏，先判断前台全屏，避免启动时在游戏上方闪现。
        RefreshDesktop();
        if (integrationEnabled_) RefreshBattery();
        MSG message{};
        for (;;)
        {
            // 每轮最多处理 64 条消息，避免输入洪水无限延迟绘制。
            for (unsigned int i = 0; i < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i)
            {
                if (message.message == WM_QUIT)
                {
                    window_.RethrowWindowError();
                    logger_.Write("WinHalo stopped normally");
                    return static_cast<int>(message.wParam);
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
                window_.RethrowWindowError();
            }
            RefreshTimedContent();
            const bool active = view_.Animating() || view_.ContinuousFrames() || stressRemaining_ != 0;
            if (IsWindowVisible(window_.Handle()) && (active || renderer_.NeedsFrame()))
            {
                HANDLE ready = renderer_.FrameReadyHandle();
                if (!ready)
                {
                    // 设备恢复时尚无交换链，先重新建立设备和帧同步句柄。
                    window_.Render();
                    continue;
                }
                const DWORD wait = MsgWaitForMultipleObjectsEx(1, &ready, ContentTimeout(), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                if (wait == WAIT_FAILED) ThrowLastError("Wait for frame or input");
                if (wait == WAIT_OBJECT_0)
                {
                    renderer_.NotifyFrameReady();
                    if (active) AnimateFrame();
                    else window_.Render();
                }
            }
            else
            {
                // 静止或隐藏时完全等待消息，不消费 DXGI 信号、不持续提交帧。
                previousFrame_ = Clock::now();
                if (MsgWaitForMultipleObjectsEx(0, nullptr, ContentTimeout(), QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_FAILED)
                    ThrowLastError("Wait for window input");
            }
        }
    }
    catch (const std::exception& error)
    {
        logger_.Write(error.what());
        throw;
    }
}

void Application::AnimateFrame()
{
    const auto begin = Clock::now();
    const double elapsed = std::chrono::duration<double>(begin - previousFrame_).count();
    previousFrame_ = begin;
    if (stressRemaining_)
    {
        stressElapsed_ += std::min(elapsed, 0.05);
        if (stressElapsed_ >= 0.09)
        {
            stressElapsed_ = 0;
            view_.Toggle();
            if (--stressRemaining_ == 0) view_.Collapse();
        }
    }
    view_.Step(elapsed);
    const auto animated = Clock::now();
    animationMilliseconds_ = std::chrono::duration<double, std::milli>(animated - begin).count();
    window_.Render();
    cpuMilliseconds_ = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    if (std::chrono::duration<double>(begin - previousMetric_).count() > 2)
    {
        previousMetric_ = begin;
        metricBaseline_ = renderer_.Frames();
    }
    const double duration = std::chrono::duration<double>(begin - previousMetric_).count();
    if (duration >= 0.25)
    {
        metricFrames_ = renderer_.Frames() - metricBaseline_;
        measuredFps_ = static_cast<double>(metricFrames_) / duration;
        previousMetric_ = begin;
        metricBaseline_ = renderer_.Frames();
    }
}

void Application::ApplyAction(ViewAction action)
{
    switch (action)
    {
    case ViewAction::RevealCapsule: view_.RevealCapsule(); break;
    case ViewAction::Toggle: view_.Toggle(); break;
    case ViewAction::Collapse: view_.Collapse(); break;
    case ViewAction::Settings: ReadSystemOptions(); view_.ShowSettings(); break;
    case ViewAction::Back: view_.ShowMain(); break;
    case ViewAction::Speed:
        configuration_.spring.speed = configuration_.spring.speed < 0.9 ? 1.0 :
            (configuration_.spring.speed < 1.4 ? 1.5 : 0.7);
        view_.Configure(configuration_.spring, configuration_.reducedMotion);
        view_.SetSaveFailed(!store_.Save(configuration_));
        break;
    case ViewAction::ReducedMotion:
        configuration_.reducedMotion = !configuration_.reducedMotion;
        view_.Configure(configuration_.spring, configuration_.reducedMotion);
        view_.SetSaveFailed(!store_.Save(configuration_));
        break;
    case ViewAction::MediaToggle: mediaService_.Command(MediaCommand::TogglePlayPause, view_.Media().source, view_.Media().sessionId); break;
    case ViewAction::MediaPrevious: mediaService_.Command(MediaCommand::Previous, view_.Media().source, view_.Media().sessionId); break;
    case ViewAction::MediaNext: mediaService_.Command(MediaCommand::Next, view_.Media().source, view_.Media().sessionId); break;
    case ViewAction::NextMediaSession: mediaService_.NextSession(view_.Media().sessionId); break;
    case ViewAction::OpenMediaApp:
        mediaService_.OpenApp(view_.Media().sessionId);
        view_.Collapse();
        break;
    case ViewAction::VolumeDown: volumeService_.SetLevel(std::max(0.0f, view_.Volume().level - 0.05f)); break;
    case ViewAction::VolumeUp: volumeService_.SetLevel(std::min(1.0f, view_.Volume().level + 0.05f)); break;
    case ViewAction::VolumeMute: volumeService_.ToggleMute(); break;
    case ViewAction::None: break;
    case ViewAction::OpenMedia: view_.ShowPage(IslandPage::Media); break;
    case ViewAction::OpenVolume: view_.ShowPage(IslandPage::Volume); break;
    case ViewAction::OpenBattery: view_.ShowPage(IslandPage::Battery); break;
    case ViewAction::Home: view_.ShowPage(IslandPage::Home); break;
    case ViewAction::NativeGlass:
        configuration_.nativeGlass = !configuration_.nativeGlass;
        window_.SetNativeGlass(configuration_.nativeGlass);
        view_.SetSaveFailed(!store_.Save(configuration_));
        break;
    case ViewAction::SystemSettings: ReadSystemOptions(); view_.ShowSystemSettings(); break;
    case ViewAction::AnimationSettings: view_.ShowSettings(); break;
    case ViewAction::Startup:
    {
        const auto result = startup_.SetEnabled(!view_.StartupEnabled());
        ReadSystemOptions(result.error);
        break;
    }
    case ViewAction::AutoHideFullscreen:
        configuration_.autoHideFullscreen = !configuration_.autoHideFullscreen;
        view_.SetSaveFailed(!store_.Save(configuration_));
        ReadSystemOptions();
        RefreshDesktop();
        break;
    }
}

void Application::ReadSystemOptions(std::wstring error)
{
    const auto status = startup_.Read();
    if (error.empty()) error = status.error;
    view_.SetSystemOptions(status.enabled, configuration_.autoHideFullscreen, status.available, std::move(error));
}

void Application::RefreshDesktop()
{
    const bool fullscreen = integrationEnabled_ && configuration_.autoHideFullscreen && fullscreenMonitor_.Check();
    window_.SetFullscreenHidden(fullscreen);
}

void Application::RefreshBattery()
{
    if (!integrationEnabled_) return;
    if (!view_.Animating()) previousFrame_ = Clock::now();
    const auto snapshot = batteryService_.Read();
    // 隐藏期间也要重置供电变化后的去重状态，但不消耗下一次可见时的提示。
    const bool notify = batteryService_.ShouldNotify(snapshot, window_.Visible());
    view_.SetBattery(snapshot, notify);
    if (notify && view_.BatteryToast()) toastUntil_ = Clock::now() + std::chrono::seconds(5);
    if (!view_.Line() || view_.Animating() || renderer_.NeedsFrame()) window_.Render();
}

void Application::DrainServices()
{
    std::optional<MediaSnapshot> media;
    std::optional<VolumeSnapshot> volume;
    {
        const std::lock_guard lock(mailbox_->mutex);
        media.swap(mailbox_->media);
        volume.swap(mailbox_->volume);
        mailbox_->posted = false;
    }
    if (!view_.Animating()) previousFrame_ = Clock::now();
    if (media)
    {
        window_.MediaChanged(*media);
        // The borderless window still exposes its current player to native accessibility and diagnostics.
        if (media->source != view_.Media().source || media->title != view_.Media().title)
        {
            const auto title = media->available ? L"WinHalo · " + media->title + L" [" + media->source + L"]" : L"WinHalo";
            SetWindowTextW(window_.Handle(), title.c_str());
        }
        view_.SetMedia(std::move(*media));
        if (!view_.Media().available) stressRemaining_ = 0;
    }
    if (volume)
    {
        const bool notify = volume->notify && volume->available && window_.Visible();
        view_.SetVolume(std::move(*volume), notify);
        if (notify && view_.VolumeToast()) toastUntil_ = Clock::now() + std::chrono::seconds(2);
    }
    nextProgress_ = Clock::now() + std::chrono::seconds(1);
    // Keep hidden media data current without uploading covers or redrawing a static line.
    if (!view_.Line() || view_.Animating() || renderer_.NeedsFrame()) window_.Render();
}

DWORD Application::ContentTimeout() const noexcept
{
    const auto now = Clock::now();
    auto deadline = Clock::time_point::max();
    if (view_.TransientToast()) deadline = toastUntil_;
    if (window_.Visible()) deadline = std::min(deadline, view_.IdleDeadline());
    if (window_.Visible() && view_.Expanded() && view_.Page() == IslandPage::Media && view_.Media().playing && !view_.ContinuousFrames())
        deadline = std::min(deadline, nextProgress_);
    if (window_.Visible() && view_.ShowsClock()) deadline = std::min(deadline, nextClock_);
    if (deadline == Clock::time_point::max()) return INFINITE;
    const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<DWORD>(std::clamp(milliseconds, 0LL, static_cast<long long>(INFINITE) - 1));
}

void Application::RefreshTimedContent()
{
    const auto now = Clock::now();
    bool dirty = false;
    if (window_.Visible() && now >= view_.IdleDeadline())
    {
        if (!view_.Animating()) previousFrame_ = now;
        dirty = view_.RefreshIdle(now);
    }
    if (integrationEnabled_ && window_.Visible() && batteryService_.ShouldNotify(view_.Battery()))
    {
        if (!view_.Animating()) previousFrame_ = now;
        view_.SetBattery(view_.Battery(), true);
        if (view_.BatteryToast()) toastUntil_ = now + std::chrono::seconds(5);
        dirty = true;
    }
    if (view_.TransientToast() && now >= toastUntil_)
    {
        if (!view_.Animating()) previousFrame_ = now;
        view_.DismissTransientToast();
        dirty = true;
    }
    if (window_.Visible() && view_.Expanded() && view_.Page() == IslandPage::Media && view_.Media().playing && !view_.ContinuousFrames() && now >= nextProgress_)
    {
        nextProgress_ = now + std::chrono::seconds(1);
        dirty = true;
    }
    if (window_.Visible() && view_.ShowsClock() && now >= nextClock_)
    {
        SYSTEMTIME local{}; GetLocalTime(&local);
        nextClock_ = now + std::chrono::milliseconds(60000 - local.wSecond * 1000 - local.wMilliseconds);
        dirty = true;
    }
    if (dirty) window_.Render();
}

void Application::OnWindowEvent(WindowEvent event)
{
    if (!view_.Animating()) previousFrame_ = Clock::now();
    switch (event)
    {
    case WindowEvent::PointerEnter: view_.SetPointerInside(true); break;
    case WindowEvent::PointerLeave: view_.SetPointerInside(false); break;
    case WindowEvent::MenuOpened: view_.SetIdlePaused(true); return;
    case WindowEvent::MenuClosed: view_.SetIdlePaused(false); break;
    case WindowEvent::WindowShown:
        previousFrame_ = Clock::now();
        view_.SetHostVisible(true);
        break;
    case WindowEvent::WindowHidden:
        view_.SetHostVisible(false);
        return;
    case WindowEvent::OutsideClick:
        if (view_.Expanded() && view_.Page() == IslandPage::Media) view_.Collapse();
        break;
    case WindowEvent::Click:
        stressRemaining_ = 0;
        ApplyAction(window_.ClickAction());
#ifdef _DEBUG
        logger_.Write(view_.Expanded() ? "Click target: expanded" : "Click target: collapsed");
#endif
        break;
    case WindowEvent::TogglePanel: stressRemaining_ = 0; view_.Toggle(); break;
    case WindowEvent::Settings: stressRemaining_ = 0; ReadSystemOptions(); view_.ShowSettings(); break;
    case WindowEvent::PowerChanged: RefreshBattery(); return;
    case WindowEvent::DesktopChanged: RefreshDesktop(); return;
    case WindowEvent::StressTest:
        if (!view_.Media().available) break;
        stressRemaining_ = 80;
        stressElapsed_ = 0;
        break;
    case WindowEvent::DebugSize: view_.CycleDebugSize(); break;
    case WindowEvent::RecreateDevice: renderer_.ResetDevice(); break;
    }
    // 设置（如减少动态效果）可直接改变当前布局，必须提交最终帧。
    window_.Render();
    // Sent cursor/menu messages can change the view while the outer loop is
    // waiting without a DXGI handle. Queue input so it recalculates that wait.
    if (view_.Animating() || renderer_.NeedsFrame())
        CheckWin32(PostMessageW(window_.Handle(), WM_NULL, 0, 0), "Wake animation loop");
}

std::wstring Application::Diagnostics() const
{
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    DWORD hz = 0;
    if (GetMonitorInfoW(MonitorFromWindow(window_.Handle(), MONITOR_DEFAULTTONEAREST), &monitor) &&
        EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode)) hz = mode.dmDisplayFrequency;
    std::wostringstream text;
    text << renderer_.Diagnostics() << L"\n显示器模式刷新率：" << hz << L" Hz\n"
        << L"动画期间采样提交帧率：" << std::fixed << std::setprecision(1) << measuredFps_ << L" FPS\n"
        << L"UI 帧处理耗时：" << std::setprecision(3) << cpuMilliseconds_ << L" ms\n"
        << L"弹簧更新耗时：" << animationMilliseconds_ << L" ms\n"
        << L"当前：" << (view_.Animating() ? L"动画进行中" :
            view_.ContinuousFrames() ? L"媒体动画进行中" : L"静止（不持续提交帧）")
        << L"\n\n提交帧率与显示器最终呈现帧率不同；数值来自最近一次动画采样。";
    text << L"\n\n媒体：" << (view_.Media().available ? view_.Media().source : L"没有媒体会话")
        << L"\n音频设备：" << (view_.Volume().available ? view_.Volume().deviceName : L"没有输出设备");
    text << L"\n电池：" << (view_.Battery().available ? std::to_wstring(view_.Battery().percent) + L"%" : L"无电池或状态未知")
        << L"\n全屏自动隐藏：" << (configuration_.autoHideFullscreen ? L"开启" : L"关闭")
        << L"\n开机启动注册：" << (view_.StartupEnabled() ? L"开启" : L"关闭");
    return text.str();
}
}
