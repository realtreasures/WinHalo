#include "Application.h"
#include "../Utils/Win32Error.h"
#include "../Utils/Localization.h"
#include <shellapi.h>
#include <cwctype>
#include <cmath>
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
    std::optional<BehaviorSnapshot> behavior;

    template<class Snapshot> void Publish(Snapshot snapshot)
    {
        const std::lock_guard lock(mutex);
        if (!window) return;
        if constexpr (std::is_same_v<Snapshot, MediaSnapshot>) media = std::move(snapshot);
        else if constexpr (std::is_same_v<Snapshot, BehaviorSnapshot>) behavior = std::move(snapshot);
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
      renderer_(logger_), window_(instance, renderer_, view_, logger_), settings_(instance),
      mailbox_(std::make_shared<ServiceMailbox>()), mediaService_(logger_), volumeService_(logger_), behaviorService_(logger_)
{
    SetUiLanguage(configuration_.language);
    view_.ApplyLayout(configuration_.layout, configuration_.autoHideIdle);
    view_.SetControlsEnabled(configuration_.systemControls);
    view_.Configure(configuration_.spring, configuration_.reducedMotion);
    view_.SetSystemOptions(false, configuration_.autoHideFullscreen);
    window_.SetDiagnostics([this] { return Diagnostics(); });
    window_.SetServiceEventHandler([this] { DrainServices(); });
    window_.SetPositionChangedHandler([this](float x, float y, const std::wstring& monitor) {
        auto next = configuration_; next.layout.offsetX = x; next.layout.offsetY = y;
        next.monitorDevice = monitor; ApplyConfiguration(std::move(next));
    });
    window_.SetWheelHandler([this](int steps, bool brightness) { OnWheel(steps, brightness); });
    window_.SetClipboardHandler([this] { OnClipboard(); });
}

Application::~Application()
{
    {
        const std::lock_guard lock(mailbox_->mutex);
        mailbox_->window = nullptr;
    }
    mediaService_.Stop();
    volumeService_.Stop();
    behaviorService_.Stop();
    settings_.Close();
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
        window_.ApplyPreferences(configuration_);
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
            behaviorService_.Start([weak](BehaviorSnapshot snapshot) {
                if (const auto mailbox = weak.lock()) mailbox->Publish(std::move(snapshot));
            });
            behaviorService_.Configure(configuration_.monitorDevice, configuration_.systemControls, configuration_.privacyIndicators);
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
                if (settings_.PreTranslate(message)) continue;
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
    case ViewAction::Settings: ShowSettings(); break;
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
    case ViewAction::VolumeDown: OnWheel(-1, false); break;
    case ViewAction::VolumeUp: OnWheel(1, false); break;
    case ViewAction::VolumeMute:
        if (configuration_.systemControls) { pendingVolume_.reset(); volumeService_.ToggleMute(); }
        break;
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
    case ViewAction::SystemSettings:
    case ViewAction::AnimationSettings: ShowSettings(); break;
    case ViewAction::OpenLink:
        if (view_.LinkNotice() && !pendingLink_.empty())
        {
            const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", pendingLink_.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            view_.DismissTransientToast();
            if (result <= 32) { view_.ShowNotice(UiText(L"无法打开链接", L"Unable to open link"), UiText(L"请检查默认浏览器设置", L"Check your default browser")); toastUntil_ = Clock::now() + std::chrono::seconds(4); }
        }
        pendingLink_.clear();
        break;
    case ViewAction::DismissNotice: view_.DismissTransientToast(); pendingLink_.clear(); break;
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
    settingsStatus_.startupEnabled = status.enabled;
    settingsStatus_.startupAvailable = status.available;
    settingsStatus_.startupError = error;
    view_.SetSystemOptions(status.enabled, configuration_.autoHideFullscreen, status.available, std::move(error));
    RefreshSettings();
}

void Application::ShowSettings()
{
    ReadSystemOptions();
    settings_.Show(configuration_, settingsStatus_, [this](const Configuration& value) { ApplyConfiguration(value); },
        [this](bool enabled) { const auto result = startup_.SetEnabled(enabled); ReadSystemOptions(result.error); });
}

void Application::RefreshSettings()
{
    settingsStatus_.brightnessAvailable = behavior_.brightnessAvailable;
    settingsStatus_.privacyAvailable = behavior_.cameraAvailable || behavior_.microphoneAvailable;
    settingsStatus_.brightnessStatus = englishLanguage ? (behavior_.brightnessAvailable ? L"Selected display supports brightness adjustment" :
        (configuration_.systemControls ? L"Selected display brightness is unavailable or still being detected" : L"Brightness shortcuts are disabled")) : behavior_.brightnessStatus;
    settingsStatus_.privacyStatus = englishLanguage ? (std::wstring(behavior_.cameraAvailable ? L"Camera activity available" : L"Camera activity unavailable or awaiting system report") +
        (behavior_.microphoneAvailable ? L"; microphone activity available" : L"; microphone activity unavailable")) : behavior_.privacyStatus;
    if (settings_.Handle()) settings_.Refresh(configuration_, settingsStatus_);
}

void Application::ApplyConfiguration(Configuration configuration)
{
    NormalizeConfiguration(configuration);
    const auto& a = configuration_.layout;
    const auto& b = configuration.layout;
    const bool previewCompact = a.width != b.width || a.height != b.height || a.compactScale != b.compactScale || a.fontSize != b.fontSize;
    configuration_ = std::move(configuration);
    SetUiLanguage(configuration_.language);
    view_.Configure(configuration_.spring, configuration_.reducedMotion);
    view_.ApplyLayout(configuration_.layout, configuration_.autoHideIdle);
    view_.SetControlsEnabled(configuration_.systemControls);
    if (previewCompact) view_.RevealCapsule();
    window_.ApplyPreferences(configuration_);
    window_.SetNativeGlass(configuration_.nativeGlass);
    if (integrationEnabled_)
        behaviorService_.Configure(configuration_.monitorDevice, configuration_.systemControls, configuration_.privacyIndicators);
    if (!configuration_.showNotifications || !configuration_.clipboardLinks)
    {
        if (!configuration_.showNotifications || view_.LinkNotice()) view_.DismissTransientToast();
        pendingLink_.clear();
    }
    const bool saved = store_.Save(configuration_);
    view_.SetSaveFailed(!saved);
    settingsStatus_.saveError = saved ? L"" : UiText(L"设置未能保存，请检查数据目录权限", L"Settings could not be saved; check data folder permissions");
    RefreshSettings();
    RefreshDesktop();
    window_.Render();
    PostMessageW(window_.Handle(), WM_NULL, 0, 0);
}

void Application::OnWheel(int steps, bool brightness)
{
    if (!configuration_.systemControls || !integrationEnabled_ || !steps) return;
    if (brightness)
    {
        brightnessRequested_ = true;
        behaviorService_.AdjustBrightness(steps * 5);
        if (!behavior_.brightnessAvailable && configuration_.showNotifications && !view_.Expanded())
        {
            view_.ShowNotice(UiText(L"亮度调节不可用", L"Brightness unavailable"), behavior_.brightnessStatus);
            toastUntil_ = Clock::now() + std::chrono::seconds(4);
        }
    }
    else if (view_.Volume().available)
    {
        const auto now = Clock::now();
        const float baseline = pendingVolume_ && now - volumeInputAt_ < std::chrono::milliseconds(750) ? *pendingVolume_ : view_.Volume().level;
        pendingVolume_ = std::clamp(baseline + static_cast<float>(steps) * .05f, 0.0f, 1.0f);
        volumeInputAt_ = now;
        volumeService_.SetLevel(*pendingVolume_);
    }
    window_.Render();
    PostMessageW(window_.Handle(), WM_NULL, 0, 0);
}

void Application::OnClipboard()
{
    if (!configuration_.clipboardLinks || !configuration_.showNotifications || !window_.Visible() || view_.Expanded()) return;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(window_.Handle())) return;
    std::wstring link;
    if (const HANDLE data = GetClipboardData(CF_UNICODETEXT))
    {
        const SIZE_T count = GlobalSize(data) / sizeof(wchar_t);
        if (count && count <= 8193)
        {
            if (const auto value = static_cast<const wchar_t*>(GlobalLock(data)))
            {
                size_t n = 0; while (n < count && value[n]) ++n;
                if (n < count) link.assign(value, n);
                GlobalUnlock(data);
            }
        }
    }
    CloseClipboard();
    const auto first = link.find_first_not_of(L" \t\r\n"), last = link.find_last_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return;
    link = link.substr(first, last - first + 1);
    std::wstring lower = link;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    const size_t prefix = lower.starts_with(L"https://") ? 8 : (lower.starts_with(L"http://") ? 7 : 0);
    if (!prefix || link.size() <= prefix || link == lastLink_ || std::any_of(link.begin(), link.end(), [](wchar_t c) { return std::iswspace(c) || c < 32; })) return;
    const auto hostEnd = link.find_first_of(L"/?#", prefix);
    if (hostEnd == prefix || link.find(L'@', prefix) < hostEnd || link.find(L'\\') != std::wstring::npos) return;
    pendingLink_ = lastLink_ = link;
    const std::wstring host = link.substr(prefix, hostEnd == std::wstring::npos ? hostEnd : hostEnd - prefix);
    view_.ShowNotice(UiText(L"打开复制的链接？", L"Open copied link?"), host, true);
    toastUntil_ = Clock::now() + std::chrono::seconds(10);
    if (!view_.Animating()) previousFrame_ = Clock::now();
    window_.Render(); PostMessageW(window_.Handle(), WM_NULL, 0, 0);
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
    const bool notify = batteryService_.ShouldNotify(snapshot, window_.Visible() && configuration_.showNotifications);
    view_.SetBattery(snapshot, notify);
    if (notify && view_.BatteryToast()) toastUntil_ = Clock::now() + std::chrono::seconds(5);
    if (!view_.Line() || view_.Animating() || renderer_.NeedsFrame()) window_.Render();
}

void Application::DrainServices()
{
    std::optional<MediaSnapshot> media;
    std::optional<VolumeSnapshot> volume;
    std::optional<BehaviorSnapshot> behavior;
    {
        const std::lock_guard lock(mailbox_->mutex);
        media.swap(mailbox_->media);
        volume.swap(mailbox_->volume);
        behavior.swap(mailbox_->behavior);
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
        if (!volume->available || (pendingVolume_ && (std::abs(volume->level - *pendingVolume_) < .001f ||
            Clock::now() - volumeInputAt_ > std::chrono::seconds(1)))) pendingVolume_.reset();
        const bool notify = volume->notify && volume->available && window_.Visible() && configuration_.showNotifications;
        view_.SetVolume(std::move(*volume), notify);
        if (notify && view_.VolumeToast()) toastUntil_ = Clock::now() + std::chrono::seconds(2);
    }
    if (behavior)
    {
        const bool privacyChanged = behavior_.cameraActive != behavior->cameraActive || behavior_.microphoneActive != behavior->microphoneActive;
        const bool brightnessChanged = behavior_.brightness != behavior->brightness;
        behavior_ = std::move(*behavior);
        RefreshSettings();
        if (window_.Visible() && configuration_.showNotifications && !view_.Expanded())
        {
            if (privacyChanged && configuration_.privacyIndicators)
            {
                const auto detail = behavior_.cameraActive && behavior_.microphoneActive ? UiText(L"摄像头和麦克风正在使用", L"Camera and microphone in use") : behavior_.cameraActive ? UiText(L"摄像头正在使用", L"Camera in use") : behavior_.microphoneActive ? UiText(L"麦克风正在使用", L"Microphone in use") : UiText(L"摄像头和麦克风已停止使用", L"Camera and microphone are idle");
                view_.ShowNotice(UiText(L"设备使用提示", L"Device activity"), detail);
                toastUntil_ = Clock::now() + std::chrono::seconds(5);
            }
            else if (brightnessRequested_ && brightnessChanged && behavior_.brightnessAvailable)
            {
                view_.ShowNotice(UiText(L"屏幕亮度", L"Screen brightness"), std::to_wstring(behavior_.brightness) + L"%");
                toastUntil_ = Clock::now() + std::chrono::seconds(2);
            }
        }
        if (brightnessChanged) brightnessRequested_ = false;
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
    if (integrationEnabled_ && configuration_.showNotifications && window_.Visible() && batteryService_.ShouldNotify(view_.Battery()))
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
    case WindowEvent::Settings: stressRemaining_ = 0; ShowSettings(); break;
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
