#pragma once
#include "../Core/IslandState.h"
#include "../Animation/AnimationManager.h"
#include "../Services/MediaTypes.h"
#include "../Services/VolumeTypes.h"
#include "../Services/BatteryTypes.h"

namespace winisland
{
// 统一使用 DIP；窗口和渲染器仅在边界处转换为物理像素。
struct IslandLayout
{
    float hostWidth = 480.0f;
    float hostHeight = 400.0f;
    float width = 180.0f;
    float height = 44.0f;
    float radius = 22.0f;
    float insetTop = 16.0f;
    float topGap = 12.0f;
    float expandedWidth = 420.0f;
    float expandedHeight = 200.0f;
    float compactScale = 1.0f;
    float expandedScale = 1.0f;
    float offsetX = 0.0f, offsetY = 0.0f;
    float fontSize = 0.0f;
    float lineWidth = 88.0f;
};

struct CapsuleFrame
{
    float width, height, centerX, top, radius, opacity, iconOffset, textOffset, scale, hover;
    float compactOpacity;
};

struct ViewRect
{
    float left, top, right, bottom;
    bool Contains(float x, float y) const noexcept { return x >= left && x <= right && y >= top && y <= bottom; }
};

class IslandView
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr float LineWidth = 88.0f, LineHeight = 8.0f;
    static constexpr auto CapsuleIdleTime = std::chrono::seconds(5);
    explicit IslandView(IslandLayout layout = {});
    const IslandLayout& Layout() const noexcept { return layout_; }
    IslandState State() const noexcept { return settings_ ? IslandState::Settings :
        (expanded_ ? IslandState::Expanded : (Line() ? IslandState::Line : state_)); }
    bool Line() const noexcept { return line_ && !expanded_ && !TransientToast(); }
    bool RevealCapsule(Clock::time_point now = Clock::now());
    void ShowLine();
    bool SetPointerInside(bool inside, Clock::time_point now = Clock::now());
    void SetIdlePaused(bool paused, Clock::time_point now = Clock::now());
    void SetHostVisible(bool visible, Clock::time_point now = Clock::now());
    bool RefreshIdle(Clock::time_point now = Clock::now());
    Clock::time_point IdleDeadline() const noexcept { return idleDeadline_; }
    bool SetState(IslandState state) noexcept;
    void Toggle();
    void Collapse();
    void ShowSettings();
    void ShowMain();
    void ShowPage(IslandPage page);
    IslandPage Page() const noexcept { return page_; }
    bool ContinuousFrames() const noexcept;
    bool ShowsClock() const noexcept { return !Line() && !media_.available && !TransientToast() && (!expanded_ || page_ == IslandPage::Home); }
    double EqualizerLevel(int bar) const noexcept;
    void SetGlassOptions(bool enabled, bool available, bool active, std::wstring status = {});
    bool NativeGlassEnabled() const noexcept { return nativeGlass_; }
    bool GlassAvailable() const noexcept { return glassAvailable_; }
    bool GlassActive() const noexcept { return glassActive_; }
    const std::wstring& GlassStatus() const noexcept { return glassStatus_; }
    void ShowSystemSettings();
    bool SystemSettings() const noexcept { return systemSettings_; }
    void SetSystemOptions(bool startup, bool fullscreen, bool startupAvailable = true, std::wstring error = {});
    bool StartupEnabled() const noexcept { return startupEnabled_; }
    bool StartupAvailable() const noexcept { return startupAvailable_; }
    bool AutoHideFullscreen() const noexcept { return autoHideFullscreen_; }
    const std::wstring& SystemError() const noexcept { return systemError_; }
    void SetSaveFailed(bool value) noexcept { saveFailed_ = value; }
    bool SaveFailed() const noexcept { return saveFailed_; }
    void CycleDebugSize();
    void Configure(SpringParameters parameters, bool reducedMotion);
    void ApplyLayout(IslandLayout layout, bool autoHideIdle);
    void SetControlsEnabled(bool enabled) noexcept { controlsEnabled_ = enabled; }
    bool ControlsEnabled() const noexcept { return controlsEnabled_; }
    void ShowNotice(std::wstring title, std::wstring detail, bool link = false);
    bool Notice() const noexcept { return notice_; }
    bool LinkNotice() const noexcept { return notice_ && linkNotice_; }
    std::uint64_t NoticeRevision() const noexcept { return noticeRevision_; }
    const std::wstring& NoticeTitle() const noexcept { return noticeTitle_; }
    const std::wstring& NoticeDetail() const noexcept { return noticeDetail_; }
    ViewRect NoticeButton(bool open) const noexcept;
    bool Step(double seconds);
    bool Animating() const noexcept { return animation_.Active() || volumeLevel_.Active(); }
    bool Expanded() const noexcept { return expanded_; }
    bool Settings() const noexcept { return settings_; }
    bool VolumeToast() const noexcept { return volumeToast_; }
    bool BatteryToast() const noexcept { return batteryToast_; }
    bool TransientToast() const noexcept { return volumeToast_ || batteryToast_ || notice_; }
    void SetBattery(BatterySnapshot snapshot, bool showNotification);
    const BatterySnapshot& Battery() const noexcept { return battery_; }
    void DismissTransientToast();
    void SetMedia(MediaSnapshot snapshot);
    void SetVolume(VolumeSnapshot snapshot, bool showNotification);
    void DismissVolumeToast();
    const MediaSnapshot& Media() const noexcept { return media_; }
    const VolumeSnapshot& Volume() const noexcept { return volume_; }
    float VolumeLevel() const noexcept;
    double MediaPosition() const noexcept;
    bool ReducedMotion() const noexcept { return reducedMotion_; }
    const SpringParameters& Parameters() const noexcept { return parameters_; }
    CapsuleFrame Frame() const noexcept;
    ViewRect Bounds() const noexcept;
    ViewRect CloseButton() const noexcept;
    ViewRect SettingsButton() const noexcept;
    ViewRect SettingsPageButton() const noexcept;
    ViewRect SpeedButton() const noexcept;
    ViewRect MotionButton() const noexcept;
    ViewRect GlassButton() const noexcept;
    ViewRect BackButton() const noexcept;
    ViewRect ModuleButton(IslandPage page) const noexcept;
    ViewRect MediaButton(ViewAction action) const noexcept;
    ViewRect MediaSourceButton() const noexcept;
    ViewRect VolumeButton(ViewAction action) const noexcept;
    ViewAction ActionAt(float x, float y) const noexcept;
    float Coverage(float pixelX, float pixelY, float scale) const noexcept;
    unsigned char Shade() const noexcept { return state_ == IslandState::Hover ? 16 : 0; }

private:
    IslandLayout layout_;
    IslandState state_ = IslandState::Idle;
    AnimationManager animation_;
    Spring volumeLevel_;
    MediaSnapshot media_;
    VolumeSnapshot volume_;
    BatterySnapshot battery_;
    SpringParameters parameters_;
    bool expanded_ = false;
    bool settings_ = false;
    bool reducedMotion_ = false;
    bool saveFailed_ = false;
    bool volumeToast_ = false;
    bool batteryToast_ = false;
    bool systemSettings_ = false;
    bool startupEnabled_ = false;
    bool startupAvailable_ = true;
    bool autoHideFullscreen_ = true;
    bool line_ = true;
    bool pointerInside_ = false;
    bool idlePaused_ = false;
    bool hostVisible_ = true;
    bool autoHideIdle_ = true;
    bool controlsEnabled_ = true;
    bool notice_ = false, linkNotice_ = false;
    std::uint64_t noticeRevision_ = 0;
    std::wstring noticeTitle_, noticeDetail_;
    Clock::time_point idleDeadline_ = Clock::time_point::max();
    IslandPage page_ = IslandPage::Home;
    bool nativeGlass_ = false, glassAvailable_ = false, glassActive_ = false;
    std::wstring glassStatus_;
    std::wstring systemError_;
    unsigned int debugSize_ = 0;
    void Retarget();
    void ArmIdle(Clock::time_point now = Clock::now()) noexcept;
};
}
