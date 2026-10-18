#include "../UI/IslandView.h"
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

using namespace winisland;

namespace
{
void Expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void Settle(IslandView& view)
{
    for (int i = 0; i < 1000 && view.Animating(); ++i) view.Step(1.0 / 144);
    Expect(!view.Animating(), "Service view animation did not settle");
}

ViewAction Hit(IslandView& view, ViewRect button)
{
    return view.ActionAt((button.left + button.right) / 2, (button.top + button.bottom) / 2);
}
}

void TestServiceView()
{
    IslandView mediaView;
    mediaView.Configure({}, true);
    mediaView.ShowPage(IslandPage::Media);
    MediaSnapshot media;
    media.canPrevious = media.canToggle = media.canNext = true;
    mediaView.SetMedia(media);
    for (const auto action : {ViewAction::MediaPrevious, ViewAction::MediaToggle, ViewAction::MediaNext})
        Expect(Hit(mediaView, mediaView.MediaButton(action)) == ViewAction::None,
            "No media session left an enabled control or toggled the panel");
    media.available = true;
    mediaView.SetMedia(media);
    Expect(mediaView.ActionAt(80, 76) == ViewAction::OpenMediaApp &&
        Hit(mediaView, mediaView.CloseButton()) == ViewAction::OpenMediaApp,
        "Card body or removed close arrow did not route to opening the player");
    media.sessionId = 7;
    media.sessions = {{7, L"Fixture A", L"A", true}, {9, L"Fixture B", L"B", false}};
    mediaView.SetMedia(media);
    Expect(Hit(mediaView, mediaView.MediaSourceButton()) == ViewAction::NextMediaSession,
        "Multi-player source icon did not switch sessions");
    media.sessions.resize(1); mediaView.SetMedia(media);
    Expect(Hit(mediaView, mediaView.MediaSourceButton()) == ViewAction::None,
        "Single-player source icon accidentally launched the app");
    for (const auto action : {ViewAction::MediaPrevious, ViewAction::MediaToggle, ViewAction::MediaNext})
        Expect(Hit(mediaView, mediaView.MediaButton(action)) == action,
            "Enabled media control did not resolve its action");
    media.canPrevious = media.canToggle = media.canNext = false;
    mediaView.SetMedia(media);
    for (const auto action : {ViewAction::MediaPrevious, ViewAction::MediaToggle, ViewAction::MediaNext})
        Expect(Hit(mediaView, mediaView.MediaButton(action)) == ViewAction::None,
            "Disabled media control toggled the panel instead of doing nothing");
    // One unsupported capability must not disable the supported controls.
    media.canToggle = true;
    mediaView.SetMedia(media);
    Expect(Hit(mediaView, mediaView.MediaButton(ViewAction::MediaToggle)) == ViewAction::MediaToggle &&
        Hit(mediaView, mediaView.MediaButton(ViewAction::MediaNext)) == ViewAction::None,
        "Media control capabilities were not independent");

    media.durationSeconds = 100;
    media.positionSeconds = -4;
    mediaView.SetMedia(media);
    Expect(mediaView.MediaPosition() == 0, "Negative media position was not clamped");
    media.positionSeconds = 160;
    mediaView.SetMedia(media);
    Expect(mediaView.MediaPosition() == 100, "Media progress exceeded the track duration");
    media.durationSeconds = -1;
    mediaView.SetMedia(media);
    Expect(mediaView.MediaPosition() == 0, "Invalid negative track duration exposed progress");
    media.durationSeconds = 100;
    for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()})
    {
        media.positionSeconds = invalid;
        mediaView.SetMedia(media);
        Expect(mediaView.MediaPosition() == 0, "Non-finite media progress reached the renderer");
    }
    media.positionSeconds = 15;
    media.durationSeconds = 180;
    media.playing = true;
    media.playbackRate = 1.5;
    media.capturedAt = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    mediaView.SetMedia(media);
    const auto before = std::chrono::steady_clock::now();
    const double progress = mediaView.MediaPosition();
    const auto after = std::chrono::steady_clock::now();
    const double lower = 15 + std::chrono::duration<double>(before - media.capturedAt).count() * 1.5;
    const double upper = 15 + std::chrono::duration<double>(after - media.capturedAt).count() * 1.5;
    Expect(progress >= lower - 1e-6 && progress <= upper + 1e-6,
        "Playing track did not extrapolate progress at the supplied playback rate");
    media.playing = false;
    mediaView.SetMedia(media);
    Expect(mediaView.MediaPosition() == 15, "Paused track progress continued to advance");
    media.playing = true;
    media.capturedAt = {};
    mediaView.SetMedia(media);
    Expect(mediaView.MediaPosition() == 15, "Missing capture time advanced media progress");
    media.positionSeconds = 95;
    media.capturedAt = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    media.durationSeconds = 100;
    mediaView.SetMedia(media);
    Expect(mediaView.MediaPosition() == 100, "Extrapolated progress exceeded the end of a track");

    IslandView volumeView;
    volumeView.RevealCapsule();
    Settle(volumeView);
    VolumeSnapshot volume;
    volume.available = true;
    volume.level = 0.42f;
    volumeView.SetVolume(volume, false);
    Expect(!volumeView.VolumeToast() && !volumeView.Expanded() && !volumeView.Animating() &&
        volumeView.Frame().width == volumeView.Layout().width && volumeView.VolumeLevel() == 0.42f,
        "First endpoint snapshot animated or opened a toast on a static island");
    struct VolumeCase { float input, expected; };
    for (const VolumeCase value : {VolumeCase{-0.4f, 0}, VolumeCase{1.6f, 1},
        VolumeCase{std::numeric_limits<float>::quiet_NaN(), 0},
        VolumeCase{std::numeric_limits<float>::infinity(), 0},
        VolumeCase{-std::numeric_limits<float>::infinity(), 0}})
    {
        volume.level = value.input;
        volumeView.SetVolume(volume, false);
        Expect(volumeView.Volume().level == value.expected && volumeView.VolumeLevel() == value.expected &&
            std::isfinite(volumeView.VolumeLevel()) && !volumeView.VolumeToast(),
            "Out-of-range or non-finite volume escaped sanitization");
    }
    volume.level = 0.25f;
    volumeView.SetVolume(volume, false);
    volume.level = 0.8f;
    volumeView.SetVolume(volume, true);
    Expect(volumeView.VolumeToast() && !volumeView.Expanded() && volumeView.Animating() &&
        volumeView.VolumeLevel() == 0.25f,
        "Volume notification did not open a temporary toast with a continuous level animation");
    volumeView.Step(0.025);
    Expect(volumeView.VolumeLevel() > 0.25f && volumeView.VolumeLevel() < 0.8f,
        "Volume level jumped directly to its target instead of animating");
    Settle(volumeView);
    Expect(volumeView.Frame().height > volumeView.Layout().height &&
        volumeView.Frame().height < volumeView.Layout().expandedHeight && volumeView.VolumeLevel() == 0.8f,
        "Volume toast did not settle into a compact notification");
    for (const auto action : {ViewAction::VolumeMute, ViewAction::VolumeDown, ViewAction::VolumeUp})
        Expect(Hit(volumeView, volumeView.VolumeButton(action)) == action,
            "Volume toast control did not resolve its action");
    volumeView.DismissVolumeToast();
    Settle(volumeView);
    Expect(!volumeView.VolumeToast() && !volumeView.Expanded() &&
        volumeView.Frame().width == volumeView.Layout().width &&
        volumeView.Frame().height == volumeView.Layout().height,
        "Dismissing the notification did not restore the collapsed capsule");
    volumeView.DismissVolumeToast();
    Expect(!volumeView.Animating(), "Dismissing an absent volume toast restarted animation");

    volumeView.ShowPage(IslandPage::Volume);
    Settle(volumeView);
    for (const auto action : {ViewAction::VolumeMute, ViewAction::VolumeDown, ViewAction::VolumeUp})
        Expect(Hit(volumeView, volumeView.VolumeButton(action)) == action,
            "Expanded volume control did not resolve its action");
    volume.available = false;
    volumeView.SetVolume(volume, true);
    for (const auto action : {ViewAction::VolumeMute, ViewAction::VolumeDown, ViewAction::VolumeUp})
        Expect(Hit(volumeView, volumeView.VolumeButton(action)) == ViewAction::None,
            "Unavailable endpoint left a live volume control");
    volume.available = true;
    volumeView.SetVolume(volume, false);
    volumeView.ShowSettings();
    Settle(volumeView);
    const auto settingsFrame = volumeView.Frame();
    volume.level = 0.3f;
    volumeView.SetVolume(volume, true);
    Expect(volumeView.Settings() && volumeView.Expanded() && !volumeView.VolumeToast() &&
        !volumeView.Animating() &&
        volumeView.State() == IslandState::Settings && volumeView.Frame().width == settingsFrame.width &&
        volumeView.Frame().height == settingsFrame.height &&
        Hit(volumeView, volumeView.SpeedButton()) == ViewAction::Speed,
        "A volume notification replaced or resized the settings panel");
    volumeView.Collapse();
    Settle(volumeView);
    volumeView.SetVolume(volume, true);
    Expect(volumeView.VolumeToast(), "Collapsed volume notification was not shown");
    volume.available = false;
    volumeView.SetVolume(volume, true);
    Settle(volumeView);
    Expect(!volumeView.VolumeToast() && !volumeView.Expanded() &&
        volumeView.Frame().height == volumeView.Layout().height,
        "Removing the audio endpoint left a stranded volume toast");

    IslandView reducedView;
    reducedView.Configure({}, true);
    reducedView.ShowPage(IslandPage::Volume);
    volume.available = true;
    volume.level = 0.2f;
    reducedView.SetVolume(volume, false);
    volume.level = 0.85f;
    reducedView.SetVolume(volume, true);
    Expect(reducedView.VolumeLevel() == 0.85f && !reducedView.Animating() && !reducedView.VolumeToast(),
        "Reduced motion did not apply a changed volume immediately");
    reducedView.Collapse();
    volume.level = 0.4f;
    reducedView.SetVolume(volume, true);
    Expect(reducedView.VolumeToast() && reducedView.VolumeLevel() == 0.4f && !reducedView.Animating(),
        "Reduced motion animated the volume notification");
    reducedView.DismissVolumeToast();
    Expect(!reducedView.Animating() && reducedView.Frame().height == reducedView.Layout().height,
        "Reduced motion did not dismiss the volume notification immediately");

    IslandView navigation;
    navigation.Configure({}, true);
    navigation.ShowMain();
    Expect(navigation.Page() == IslandPage::Home && navigation.Expanded(), "Expand did not open the module home");
    for (const auto module : {IslandPage::Media, IslandPage::Volume, IslandPage::Battery, IslandPage::Settings})
    {
        const auto expected = module == IslandPage::Media ? ViewAction::OpenMedia : (module == IslandPage::Volume ? ViewAction::OpenVolume :
            (module == IslandPage::Battery ? ViewAction::OpenBattery : ViewAction::Settings));
        Expect(Hit(navigation, navigation.ModuleButton(module)) == expected, "Module card routed to the wrong page");
    }
    navigation.ShowPage(IslandPage::Media);
    Expect(Hit(navigation, navigation.BackButton()) == ViewAction::None &&
        Hit(navigation, navigation.SettingsButton()) == ViewAction::None,
        "Focused player retained invisible home or settings hit targets");
    navigation.ShowPage(IslandPage::Volume);
    Expect(Hit(navigation, navigation.BackButton()) == ViewAction::Home, "Volume detail has no return-to-home action");
    navigation.SetGlassOptions(false, true, false);
    navigation.ShowSystemSettings();
    Expect(Hit(navigation, navigation.GlassButton()) == ViewAction::NativeGlass, "Native glass setting is not reachable");
    navigation.SetGlassOptions(false, false, false);
    Expect(Hit(navigation, navigation.GlassButton()) == ViewAction::None, "Unavailable glass effect remained interactive");
    navigation.Collapse();
    MediaSnapshot compact;
    compact.available = compact.playing = true;
    compact.title = L"Compact fixture"; compact.artist = L"Artist"; compact.album = L"Album";
    navigation.SetMedia(compact);
    Expect(navigation.Frame().width == 300 && navigation.Frame().height == 48 && !navigation.ContinuousFrames(),
        "Reduced motion did not immediately apply the media capsule or stopped animation policy");
    navigation.Toggle();
    Expect(navigation.Page() == IslandPage::Media && navigation.Frame().width == 420 && navigation.Frame().height == 200,
        "Opening an active media capsule did not directly show the compact player");
    navigation.ShowSettings(); navigation.ShowMain();
    Expect(navigation.Page() == IslandPage::Media, "Returning from settings did not restore the current player");
    compact.playing = false; navigation.SetMedia(compact);
    navigation.Collapse(); navigation.Toggle();
    Expect(navigation.Page() == IslandPage::Media, "Paused media opened the module menu instead of the player");
    compact.playing = true; navigation.SetMedia(compact); navigation.Collapse();
    navigation.Configure({}, false);
    Expect(navigation.ContinuousFrames() && navigation.EqualizerLevel(0) >= 0.22 && navigation.EqualizerLevel(0) <= 1,
        "Visible playing capsule cannot animate its equalizer");
    navigation.ShowPage(IslandPage::Home); Settle(navigation);
    Expect(!navigation.ContinuousFrames(), "Module home continuously renders during playback");
    navigation.ShowPage(IslandPage::Media);
    Expect(navigation.ContinuousFrames(), "Playing detail has no smooth progress frames");
    compact.playing = false; navigation.SetMedia(compact); Settle(navigation);
    Expect(!navigation.ContinuousFrames() && navigation.EqualizerLevel(0) == 0.25,
        "Paused media kept requesting equalizer/progress frames");
    navigation.Collapse(); navigation.SetMedia({}); Settle(navigation);
    Expect(navigation.ShowsClock() && navigation.Frame().width == navigation.Layout().width,
        "Ending the media session did not restore the clock capsule");
    navigation.Toggle(); Settle(navigation);
    Expect(navigation.Page() == IslandPage::Home && navigation.Frame().height == navigation.Layout().expandedHeight,
        "Opening the clock lost the module home when no media session exists");
    navigation.SetMedia(compact); navigation.ShowMain(); Settle(navigation);
    navigation.SetMedia({}); Settle(navigation);
    Expect(!navigation.Expanded() && navigation.ShowsClock(), "Last player exit left an expanded empty player");
}
