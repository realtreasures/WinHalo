#include "IslandView.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace winisland
{
IslandView::IslandView(IslandLayout layout) : layout_(layout)
{
    const float values[] = { layout.hostWidth, layout.hostHeight, layout.width,
        layout.height, layout.radius, layout.insetTop, layout.topGap, layout.expandedWidth, layout.expandedHeight };
    for (float value : values)
        if (!std::isfinite(value)) throw std::invalid_argument("Island layout must be finite");
    if (layout.width < 16 || layout.height < 16 || layout.hostWidth < layout.width + 4 ||
        layout.hostHeight < layout.height + layout.insetTop + 2 || layout.radius < 0 ||
        layout.insetTop < 2 || layout.topGap < 0 || layout.hostWidth > 2048 || layout.hostHeight > 2048 ||
        layout.expandedWidth < 300 || layout.expandedWidth > layout.hostWidth - 16 ||
        layout.expandedHeight < 200 || layout.expandedHeight * layout.expandedScale > layout.hostHeight - layout.insetTop - 16)
        throw std::invalid_argument("Invalid island layout");
    animation_.Reset({layout.lineWidth, LineHeight, layout.hostWidth / 2, layout.insetTop,
        LineHeight / 2, 0, 18, 12, 0.94, 0, 0});
    animation_.Configure(parameters_);
}

bool IslandView::SetState(IslandState state) noexcept
{
    if (state_ == state) return false;
    state_ = state;
    Retarget();
    return true;
}

void IslandView::ArmIdle(Clock::time_point now) noexcept
{
    idleDeadline_ = autoHideIdle_ && !line_ && !expanded_ && !TransientToast() && !pointerInside_ && !idlePaused_ && hostVisible_ ?
        now + CapsuleIdleTime : Clock::time_point::max();
}

bool IslandView::RevealCapsule(Clock::time_point now)
{
    if (expanded_ || TransientToast() || !line_) return false;
    line_ = false;
    ArmIdle(now);
    Retarget();
    return true;
}

void IslandView::ShowLine()
{
    expanded_ = settings_ = volumeToast_ = batteryToast_ = systemSettings_ = notice_ = false;
    line_ = autoHideIdle_;
    pointerInside_ = false;
    state_ = IslandState::Idle;
    page_ = IslandPage::Home;
    debugSize_ = 0;
    idleDeadline_ = Clock::time_point::max();
    Retarget();
}

bool IslandView::SetPointerInside(bool inside, Clock::time_point now)
{
    // A notification can finish with the pointer still over the restored line.
    if (pointerInside_ == inside && !(inside && Line())) return false;
    pointerInside_ = inside;
    state_ = inside ? IslandState::Hover : IslandState::Idle;
    if (inside && !expanded_ && !TransientToast()) line_ = false;
    ArmIdle(now);
    Retarget();
    return true;
}

void IslandView::SetIdlePaused(bool paused, Clock::time_point now)
{
    if (idlePaused_ == paused) return;
    idlePaused_ = paused;
    ArmIdle(now);
}

void IslandView::SetHostVisible(bool visible, Clock::time_point now)
{
    if (hostVisible_ == visible) return;
    hostVisible_ = visible;
    if (!visible)
    {
        pointerInside_ = false;
        state_ = IslandState::Idle;
        Retarget();
    }
    ArmIdle(now);
}

bool IslandView::RefreshIdle(Clock::time_point now)
{
    if (idleDeadline_ == Clock::time_point::max() || now < idleDeadline_) return false;
    ShowLine();
    return true;
}

void IslandView::Retarget()
{
    float width = expanded_ ? layout_.expandedWidth : layout_.width;
    float height = expanded_ ? layout_.expandedHeight : layout_.height;
    // Retained for older diagnostic fixtures; settings are opened in their own window by Application.
    if (expanded_ && settings_) height = std::min(340.0f, layout_.hostHeight - layout_.insetTop - 16);
    if (!expanded_ && media_.available) { width = std::max(300.0f, layout_.width); height = std::max(48.0f, height); }
    if (expanded_ && debugSize_ == 1) { width = 340; height = 160; }
    if (expanded_ && debugSize_ == 2) { width = 260; height = 65; }
    if (TransientToast()) { width = std::min(300.0f, layout_.expandedWidth); height = 88; }
    if (Line()) { width = layout_.lineWidth; height = LineHeight; }
    const bool content = expanded_ || TransientToast();
    if (!content && !Line() && state_ == IslandState::Hover) { width += 8; height += media_.available ? 16 : 12; }
    if (!Line())
    {
        const float scale = expanded_ ? layout_.expandedScale : (TransientToast() ? 1.0f : layout_.compactScale);
        width *= scale; height *= scale;
    }
    animation_.Retarget({width, height, layout_.hostWidth / 2, layout_.insetTop,
        Line() ? LineHeight / 2 : std::min(height / 2, content ? 28.0f * (expanded_ ? layout_.expandedScale : 1.0f) : layout_.radius * layout_.compactScale), content ? 1.0 : 0.0,
        content ? 0.0 : 18.0, content ? 0.0 : 12.0, content ? 1.0 : 0.94,
        !Line() && state_ == IslandState::Hover ? 1.0 : 0.0,
        !Line() && !content ? 1.0 : 0.0}, reducedMotion_);
}

void IslandView::Toggle()
{
    if (expanded_) { Collapse(); return; }
    if (!media_.available) return;
    volumeToast_ = batteryToast_ = systemSettings_ = notice_ = false;
    expanded_ = true;
    line_ = false;
    page_ = IslandPage::Media;
    settings_ = false;
    debugSize_ = 0;
    ArmIdle();
    Retarget();
}

void IslandView::Collapse()
{
    volumeToast_ = batteryToast_ = systemSettings_ = notice_ = false;
    expanded_ = settings_ = false;
    line_ = false;
    page_ = IslandPage::Home;
    debugSize_ = 0;
    ArmIdle();
    Retarget();
}

void IslandView::ShowSettings()
{
    volumeToast_ = batteryToast_ = systemSettings_ = false;
    expanded_ = settings_ = true;
    line_ = false;
    page_ = IslandPage::Settings;
    debugSize_ = 0;
    ArmIdle();
    Retarget();
}

void IslandView::ShowMain()
{
    if (media_.available) ShowPage(IslandPage::Media);
    else if (expanded_) Collapse();
}

void IslandView::ShowPage(IslandPage page)
{
    if (page == IslandPage::Settings) { ShowSettings(); return; }
    if (page == IslandPage::Home) { ShowMain(); return; }
    // Playback and explicit settings are the only expanded surfaces. Legacy
    // module actions cannot reopen the removed dashboard or its detail pages.
    if (page != IslandPage::Media || !media_.available) return;
    volumeToast_ = batteryToast_ = systemSettings_ = false;
    expanded_ = true;
    line_ = false;
    settings_ = false;
    page_ = page;
    debugSize_ = 0;
    ArmIdle();
    Retarget();
}

void IslandView::CycleDebugSize()
{
    if (!media_.available) return;
    volumeToast_ = batteryToast_ = systemSettings_ = false;
    expanded_ = true;
    line_ = false;
    settings_ = false;
    page_ = IslandPage::Media;
    debugSize_ = (debugSize_ + 1) % 3;
    ArmIdle();
    Retarget();
}

void IslandView::Configure(SpringParameters parameters, bool reducedMotion)
{
    animation_.Configure(parameters);
    auto volumeParameters = parameters;
    volumeParameters.damping = std::max(volumeParameters.damping, 44.0);
    volumeLevel_.Configure(volumeParameters);
    parameters_ = parameters;
    reducedMotion_ = reducedMotion;
    if (reducedMotion_) volumeLevel_.Reset(volumeLevel_.Target());
    Retarget();
}

void IslandView::ApplyLayout(IslandLayout layout, bool autoHideIdle)
{
    IslandView validate(layout);
    layout_ = layout;
    autoHideIdle_ = autoHideIdle;
    if (!autoHideIdle_) line_ = false;
    ArmIdle();
    Retarget();
}

void IslandView::ShowNotice(std::wstring title, std::wstring detail, bool link)
{
    if (expanded_) return;
    volumeToast_ = batteryToast_ = false;
    notice_ = true; linkNotice_ = link;
    ++noticeRevision_;
    noticeTitle_ = std::move(title); noticeDetail_ = std::move(detail);
    ArmIdle(); Retarget();
}

ViewRect IslandView::NoticeButton(bool open) const noexcept
{
    const auto b = Bounds();
    const float x = b.right - (open ? 138.0f : 72.0f);
    return {x, b.top + 52, x + 58, b.top + 78};
}

bool IslandView::Step(double seconds)
{
    const bool changed = animation_.Step(seconds);
    return volumeLevel_.Step(seconds) || changed;
}

void IslandView::SetMedia(MediaSnapshot snapshot)
{
    const bool ended = media_.available && !snapshot.available;
    media_ = std::move(snapshot);
    if (ended && expanded_ && page_ == IslandPage::Media) { Collapse(); return; }
    Retarget();
}

bool IslandView::ContinuousFrames() const noexcept
{
    return !Line() && media_.available && media_.playing && !reducedMotion_ && !TransientToast() &&
        (!expanded_ || page_ == IslandPage::Media);
}

double IslandView::EqualizerLevel(int bar) const noexcept
{
    if (!media_.playing || reducedMotion_) return 0.25;
    const double time = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return 0.22 + 0.78 * (0.5 + 0.5 * std::sin(time * (4.1 + bar * 0.7) + bar * 1.8));
}

void IslandView::SetGlassOptions(bool enabled, bool available, bool active, std::wstring status)
{
    nativeGlass_ = enabled; glassAvailable_ = available; glassActive_ = active; glassStatus_ = std::move(status);
}

void IslandView::SetVolume(VolumeSnapshot snapshot, bool showNotification)
{
    const bool hadToast = TransientToast();
    const bool hadEndpoint = volume_.available;
    snapshot.level = std::isfinite(snapshot.level) ? std::clamp(snapshot.level, 0.0f, 1.0f) : 0.0f;
    volume_ = std::move(snapshot);
    if (!volume_.available) volumeToast_ = false;
    else if (showNotification && !expanded_ && !batteryToast_ && !notice_) volumeToast_ = true;
    if (!hadEndpoint || reducedMotion_ || settings_ || (!expanded_ && !volumeToast_)) volumeLevel_.Reset(volume_.level);
    else volumeLevel_.SetTarget(volume_.level);
    if (hadToast != TransientToast()) ArmIdle();
    Retarget();
}

void IslandView::DismissVolumeToast()
{
    if (!volumeToast_) return;
    volumeToast_ = false;
    ArmIdle();
    Retarget();
}

void IslandView::ShowSystemSettings()
{
    ShowSettings();
    systemSettings_ = true;
}

void IslandView::SetSystemOptions(bool startup, bool fullscreen, bool startupAvailable, std::wstring error)
{
    startupEnabled_ = startup;
    startupAvailable_ = startupAvailable;
    autoHideFullscreen_ = fullscreen;
    systemError_ = std::move(error);
}

void IslandView::SetBattery(BatterySnapshot snapshot, bool showNotification)
{
    const bool hadToast = TransientToast();
    snapshot.percent = snapshot.percent >= 0 && snapshot.percent <= 100 ? snapshot.percent : -1;
    if (!snapshot.available) snapshot = {};
    snapshot.low = snapshot.available && snapshot.onBattery && !snapshot.charging && snapshot.percent >= 0 && snapshot.percent <= 20;
    battery_ = snapshot;
    if (!snapshot.low) batteryToast_ = false;
    else if (showNotification && !expanded_ && !notice_)
    {
        batteryToast_ = true;
        volumeToast_ = false;
    }
    if (hadToast != TransientToast()) ArmIdle();
    Retarget();
}

void IslandView::DismissTransientToast()
{
    if (!TransientToast()) return;
    volumeToast_ = batteryToast_ = notice_ = false;
    ArmIdle();
    Retarget();
}

float IslandView::VolumeLevel() const noexcept
{
    return std::clamp(static_cast<float>(volumeLevel_.Value()), 0.0f, 1.0f);
}

double IslandView::MediaPosition() const noexcept
{
    double value = media_.positionSeconds;
    if (media_.playing && media_.capturedAt != std::chrono::steady_clock::time_point{})
        value += std::chrono::duration<double>(std::chrono::steady_clock::now() - media_.capturedAt).count() * media_.playbackRate;
    if (!std::isfinite(value)) return 0;
    return std::clamp(value, 0.0, std::max(0.0, media_.durationSeconds));
}

CapsuleFrame IslandView::Frame() const noexcept
{
    return {std::clamp(animation_.Get(Property::Width), 16.0f, layout_.hostWidth - 4),
        std::clamp(animation_.Get(Property::Height), 4.0f, layout_.hostHeight - layout_.insetTop - 2),
        animation_.Get(Property::CenterX), animation_.Get(Property::Top),
        std::max(0.0f, animation_.Get(Property::Radius)),
        std::clamp(animation_.Get(Property::Opacity), 0.0f, 1.0f),
        animation_.Get(Property::IconOffset), animation_.Get(Property::TextOffset),
        animation_.Get(Property::Scale), std::clamp(animation_.Get(Property::Hover), 0.0f, 1.0f),
        std::clamp(animation_.Get(Property::CompactOpacity), 0.0f, 1.0f)};
}

ViewRect IslandView::Bounds() const noexcept
{
    const auto frame = Frame();
    return {frame.centerX - frame.width / 2, frame.top, frame.centerX + frame.width / 2, frame.top + frame.height};
}

ViewRect IslandView::CloseButton() const noexcept
{
    const auto bounds = Bounds();
    if (page_ == IslandPage::Media)
        return {bounds.right - 38, bounds.top + 8, bounds.right - 12, bounds.top + 30};
    return {bounds.right - 50, bounds.top + 20, bounds.right - 20, bounds.top + 50};
}
ViewRect IslandView::SettingsButton() const noexcept
{
    const auto bounds = Bounds();
    return {bounds.right - 98, bounds.bottom - 56, bounds.right - 24, bounds.bottom - 24};
}
ViewRect IslandView::SettingsPageButton() const noexcept
{
    const auto bounds = Bounds();
    return {bounds.left + 24, bounds.bottom - 56, bounds.left + 98, bounds.bottom - 24};
}
ViewRect IslandView::SpeedButton() const noexcept
{
    const auto bounds = Bounds();
    return {bounds.left + 24, bounds.top + 88, bounds.right - 24, bounds.top + 130};
}
ViewRect IslandView::MotionButton() const noexcept
{
    const auto bounds = Bounds();
    return {bounds.left + 24, bounds.top + 138, bounds.right - 24, bounds.top + 180};
}

ViewRect IslandView::GlassButton() const noexcept
{
    const auto bounds = Bounds();
    return {bounds.left + 24, bounds.top + 188, bounds.right - 24, bounds.top + 230};
}
ViewRect IslandView::BackButton() const noexcept
{
    const auto bounds = Bounds();
    return {bounds.left + 24, bounds.bottom - 56, bounds.left + 104, bounds.bottom - 24};
}
ViewRect IslandView::ModuleButton(IslandPage page) const noexcept
{
    const auto bounds = Bounds();
    const int index = page == IslandPage::Media ? 0 : (page == IslandPage::Volume ? 1 : (page == IslandPage::Battery ? 2 : 3));
    const float width = (bounds.right - bounds.left - 60) / 2;
    const float height = (bounds.bottom - bounds.top - 168) / 2;
    const float x = bounds.left + 24 + (index % 2) * (width + 12);
    const float y = bounds.top + 94 + (index / 2) * (height + 12);
    return {x, y, x + width, y + height};
}

ViewRect IslandView::MediaButton(ViewAction action) const noexcept
{
    const auto b = Bounds();
    const float scale = std::min((b.right - b.left) / 420.0f, (b.bottom - b.top) / 200.0f);
    const float top = (b.top + b.bottom - 200 * scale) / 2;
    const float cx = (b.left + b.right) / 2;
    const float offset = action == ViewAction::MediaPrevious ? -86.0f : (action == ViewAction::MediaNext ? 86.0f : 0.0f);
    return {cx + (offset - 26) * scale, top + 136 * scale, cx + (offset + 26) * scale, top + 184 * scale};
}

ViewRect IslandView::MediaSourceButton() const noexcept
{
    const auto bounds = Bounds();
    const float scale = std::min((bounds.right - bounds.left) / 420.0f, (bounds.bottom - bounds.top) / 200.0f);
    const float left = (bounds.left + bounds.right - 420 * scale) / 2;
    const float top = (bounds.top + bounds.bottom - 200 * scale) / 2;
    return {left + 14 * scale, top + 138 * scale, left + 58 * scale, top + 182 * scale};
}

ViewRect IslandView::VolumeButton(ViewAction action) const noexcept
{
    const auto bounds = Bounds();
    const float y = volumeToast_ ? bounds.top + 23 : bounds.top + 174;
    if (volumeToast_)
    {
        if (action == ViewAction::VolumeMute) return {bounds.left + 20, y, bounds.left + 54, y + 32};
        const float x = bounds.right - (action == ViewAction::VolumeDown ? 84.0f : 48.0f);
        return {x, y, x + 28, y + 32};
    }
    const float x = bounds.left + (action == ViewAction::VolumeMute ? 24.0f :
        (action == ViewAction::VolumeDown ? 64.0f : 98.0f));
    return {x, y, x + (action == ViewAction::VolumeMute ? 34.0f : 28.0f), y + 30};
}

ViewAction IslandView::ActionAt(float x, float y) const noexcept
{
    if (Line()) return ViewAction::RevealCapsule;
    if (notice_)
    {
        if (Frame().opacity < .9f || Frame().height < 80) return ViewAction::None;
        if (linkNotice_ && NoticeButton(true).Contains(x, y)) return ViewAction::OpenLink;
        if (NoticeButton(false).Contains(x, y)) return ViewAction::DismissNotice;
        return ViewAction::None;
    }
    if (!expanded_ && !TransientToast() && !media_.available) return ViewAction::None;
    const auto expandAction = media_.available || expanded_ ? ViewAction::Toggle : ViewAction::None;
    if (Frame().opacity < 0.65f) return expandAction;
    if (batteryToast_) return expandAction;
    if (volumeToast_)
    {
        for (const auto action : {ViewAction::VolumeMute, ViewAction::VolumeDown, ViewAction::VolumeUp})
            if (VolumeButton(action).Contains(x, y)) return volume_.available && controlsEnabled_ ? action : ViewAction::None;
        return expandAction;
    }
    if (!expanded_) return expandAction;
    if (page_ != IslandPage::Media && CloseButton().Contains(x, y)) return ViewAction::Collapse;
    if (settings_)
    {
        if (SettingsPageButton().Contains(x, y)) return systemSettings_ ? ViewAction::AnimationSettings : ViewAction::SystemSettings;
        if (systemSettings_)
        {
            if (SpeedButton().Contains(x, y)) return startupAvailable_ ? ViewAction::Startup : ViewAction::None;
            if (MotionButton().Contains(x, y)) return ViewAction::AutoHideFullscreen;
            if (GlassButton().Contains(x, y)) return glassAvailable_ ? ViewAction::NativeGlass : ViewAction::None;
            if (SettingsButton().Contains(x, y)) return ViewAction::Back;
            return ViewAction::None;
        }
        if (SpeedButton().Contains(x, y)) return ViewAction::Speed;
        if (MotionButton().Contains(x, y)) return ViewAction::ReducedMotion;
        if (SettingsButton().Contains(x, y)) return ViewAction::Back;
        return ViewAction::Toggle;
    }
    if (page_ != IslandPage::Media && SettingsButton().Contains(x, y)) return ViewAction::Settings;
    if (page_ == IslandPage::Home)
    {
        for (const auto page : {IslandPage::Media, IslandPage::Volume, IslandPage::Battery, IslandPage::Settings})
            if (ModuleButton(page).Contains(x, y))
                return page == IslandPage::Media ? ViewAction::OpenMedia : (page == IslandPage::Volume ? ViewAction::OpenVolume :
                    (page == IslandPage::Battery ? ViewAction::OpenBattery : ViewAction::Settings));
        return ViewAction::None;
    }
    if (page_ != IslandPage::Media && BackButton().Contains(x, y)) return ViewAction::Home;
    if (page_ == IslandPage::Battery) return ViewAction::None;
    if (page_ == IslandPage::Volume)
    {
        for (const auto action : {ViewAction::VolumeMute, ViewAction::VolumeDown, ViewAction::VolumeUp})
            if (VolumeButton(action).Contains(x, y)) return volume_.available ? action : ViewAction::None;
        return ViewAction::None;
    }
    if (MediaSourceButton().Contains(x, y))
        return media_.available && media_.sessions.size() > 1 ? ViewAction::NextMediaSession : ViewAction::None;
    for (const auto action : {ViewAction::MediaPrevious, ViewAction::MediaToggle, ViewAction::MediaNext})
    {
        if (!MediaButton(action).Contains(x, y)) continue;
        const bool enabled = media_.available && (action == ViewAction::MediaPrevious ? media_.canPrevious :
            (action == ViewAction::MediaNext ? media_.canNext : media_.canToggle));
        return enabled ? action : ViewAction::None;
    }
    return media_.available ? ViewAction::OpenMediaApp : ViewAction::None;
}

float IslandView::Coverage(float pixelX, float pixelY, float scale) const noexcept
{
    // 圆角矩形的有符号距离：外侧一个像素内渐变，内部完全不透明。
    const auto frame = Frame();
    const float halfWidth = frame.width * scale * 0.5f;
    const float halfHeight = frame.height * scale * 0.5f;
    const float radius = std::min({ frame.radius * scale, halfWidth, halfHeight });
    const float x = std::abs(pixelX - frame.centerX * scale) - (halfWidth - radius);
    const float y = std::abs(pixelY - (frame.top * scale + halfHeight)) - (halfHeight - radius);
    const float outsideX = std::max(x, 0.0f);
    const float outsideY = std::max(y, 0.0f);
    const float distance = std::sqrt(outsideX * outsideX + outsideY * outsideY)
        + std::min(std::max(x, y), 0.0f) - radius;
    return std::clamp(0.5f - distance, 0.0f, 1.0f);
}
}
