#include "../UI/IslandView.h"
#include <chrono>
#include <cmath>
#include <stdexcept>

using namespace winisland;
using namespace std::chrono_literals;

namespace
{
void Expect(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
void Settle(IslandView& view)
{
    for (int i = 0; i < 1000 && view.Animating(); ++i) view.Step(1.0 / 144);
    Expect(!view.Animating(), "Line transition did not settle");
}
}

void TestLineState()
{
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::time_point{} + 24h;
    const auto never = Clock::time_point::max();
    IslandView view;
    view.Configure({}, true);
    Expect(view.Line() && view.State() == IslandState::Line && !view.Expanded() &&
        view.Frame().width == 88 && view.Frame().height == 8 && !view.ShowsClock() &&
        !view.ContinuousFrames() && view.IdleDeadline() == never,
        "Startup line requested compact content or an idle timer");
    Expect(view.ActionAt(240, 20) == ViewAction::RevealCapsule,
        "Touching the line would open the full panel");

    view.SetPointerInside(true, start);
    Expect(!view.Line() && !view.Expanded() && view.IdleDeadline() == never &&
        view.ActionAt(240, 38) == ViewAction::None,
        "Pointer entry did not reveal only the capsule");
    Expect(!view.RefreshIdle(start + 60s) && !view.Line(), "Pointer-held capsule timed out");
    view.SetPointerInside(false, start + 60s);
    Expect(view.IdleDeadline() == start + 65s, "Leave did not start a full five-second deadline");
    Expect(!view.RefreshIdle(start + 64999ms) && !view.Line(), "Capsule collapsed before five seconds");
    Expect(view.RefreshIdle(start + 65s) && view.Line() && view.IdleDeadline() == never,
        "Capsule did not collapse exactly at five seconds");
    Expect(!view.RefreshIdle(start + 100s), "Settled line repeatedly requested work");

    view.SetPointerInside(true, start + 100s);
    view.SetPointerInside(false, start + 101s);
    view.SetPointerInside(true, start + 105999ms);
    Expect(!view.RefreshIdle(start + 106s) && !view.Line() && view.IdleDeadline() == never,
        "Re-entry failed to cancel the old deadline");
    view.SetPointerInside(false, start + 110s);
    // Repeated outside samples must not postpone collapse forever.
    view.SetPointerInside(false, start + 114s);
    Expect(view.IdleDeadline() == start + 115s && view.RefreshIdle(start + 115s),
        "Repeated outside samples restarted the idle countdown");

    view.RevealCapsule(start + 120s);
    view.SetIdlePaused(true, start + 121s);
    Expect(view.IdleDeadline() == never && !view.RefreshIdle(start + 200s) && !view.Line(),
        "An open menu allowed automatic collapse");
    view.SetIdlePaused(false, start + 200s);
    Expect(view.IdleDeadline() == start + 205s && !view.RefreshIdle(start + 204999ms) &&
        view.RefreshIdle(start + 205s), "Closing a menu did not grant a fresh five seconds");

    view.RevealCapsule(start + 210s);
    view.SetHostVisible(false, start + 211s);
    Expect(view.IdleDeadline() == never && !view.RefreshIdle(start + 300s) && !view.Line(),
        "A hidden capsule kept a running idle deadline");
    view.SetHostVisible(true, start + 300s);
    Expect(view.IdleDeadline() == start + 305s && !view.RefreshIdle(start + 304999ms) &&
        view.RefreshIdle(start + 305s), "Showing a hidden capsule did not restart its grace period");

    view.Toggle();
    Expect(view.Line() && !view.Expanded() && view.IdleDeadline() == never,
        "No-player panel hotkey changed the resting line");
    view.ShowSettings();
    Expect(view.Settings() && view.IdleDeadline() == never && !view.RefreshIdle(start + 500s),
        "Settings were automatically collapsed");
    view.Collapse();
    Expect(!view.Line() && !view.Expanded(), "Closing a panel skipped the capsule stage");

    // Every formerly public module route must preserve the idle clock, including
    // its already-running grace period. This also guards keyboard/tray bypasses.
    IslandView clock;
    clock.Configure({}, true);
    const auto tryOldEntrances = [&]
    {
        clock.Toggle(); clock.ShowMain(); clock.ShowPage(IslandPage::Home);
        clock.ShowPage(IslandPage::Media); clock.ShowPage(IslandPage::Volume);
        clock.ShowPage(IslandPage::Battery); clock.CycleDebugSize();
    };
    tryOldEntrances();
    Expect(clock.Line() && !clock.Expanded() && !clock.Animating() && clock.IdleDeadline() == never,
        "A legacy module route bypassed the no-player line policy");
    clock.RevealCapsule(start + 510s);
    const auto clockDeadline = clock.IdleDeadline();
    tryOldEntrances();
    Expect(!clock.Line() && !clock.Expanded() && clock.ShowsClock() && !clock.Animating() &&
        clock.ActionAt(240, 38) == ViewAction::None && clock.IdleDeadline() == clockDeadline &&
        !clock.RefreshIdle(start + 514999ms) && clock.RefreshIdle(start + 515s),
        "A no-player click/hotkey/navigation restarted the clock capsule deadline");
    VolumeSnapshot clockVolume; clockVolume.available = true; clockVolume.level = 0.4f;
    clock.SetVolume(clockVolume, true);
    tryOldEntrances();
    Expect(clock.VolumeToast() && !clock.Expanded() && clock.Frame().height == 88 &&
        clock.IdleDeadline() == never, "A disabled panel entrance dismissed a live volume notification");
    clock.DismissVolumeToast();
    clock.ShowSettings();
    clock.ShowPage(IslandPage::Volume); clock.ShowPage(IslandPage::Battery); clock.CycleDebugSize();
    Expect(clock.Settings(), "A removed module or debug size replaced the settings page");
    clock.ShowMain();
    Expect(!clock.Expanded() && !clock.Line() && clock.ShowsClock(),
        "Returning from settings without media did not restore the clock capsule");
    clock.ShowSettings(); clock.Toggle();
    Expect(!clock.Expanded() && clock.ShowsClock(), "Toggle could not close settings without a media session");

    MediaSnapshot media;
    media.available = media.playing = true;
    media.title = L"Hidden playback fixture";
    view.SetMedia(media);
    view.ShowLine();
    view.SetPointerInside(false, start + 599s);
    view.Configure({}, false);
    Settle(view);
    Expect(view.Line() && view.Frame().width == 88 && view.Frame().height == 8 &&
        !view.ContinuousFrames() && !view.ShowsClock(), "Playing media widened or animated the idle line");
    view.SetPointerInside(true, start + 600s);
    Settle(view);
    Expect(!view.Expanded() && view.ContinuousFrames(), "Media line touch skipped its animated compact player");
    view.Toggle(); Settle(view);
    Expect(view.Page() == IslandPage::Media && view.Expanded(), "Clicking the revealed media capsule lost its player");
    view.ShowLine(); Settle(view);
    view.SetPointerInside(false, start + 699s);

    VolumeSnapshot volume;
    volume.available = true; volume.level = 0.2f;
    view.SetVolume(volume, false);
    volume.level = 0.7f; view.SetVolume(volume, true); Settle(view);
    Expect(view.VolumeToast() && view.Frame().height == 88 && view.IdleDeadline() == never,
        "Volume notification did not temporarily replace the idle line");
    view.DismissVolumeToast(); Settle(view);
    Expect(view.Line() && view.Frame().height == 8 && !view.ContinuousFrames(),
        "Volume notification failed to restore the original line");
    volume.level = 0.4f; view.SetVolume(volume, true); Settle(view);
    view.SetPointerInside(true, start + 699s);
    view.DismissVolumeToast(); Settle(view);
    Expect(view.Line(), "Pointer state changed the notification's resting state before hit testing");
    Expect(view.SetPointerInside(true, start + 700s) && !view.Line() && view.IdleDeadline() == never,
        "Pointer held over a notification could not reveal the restored line");
    Settle(view);
    view.SetPointerInside(false, start + 700s);
    view.RevealCapsule(start + 700s);
    BatterySnapshot battery{true, true, false, 15, true};
    view.SetBattery(battery, true); Settle(view);
    Expect(view.BatteryToast() && view.IdleDeadline() == never && !view.RefreshIdle(start + 800s),
        "Low-battery notification ran the compact idle timer");
    view.DismissTransientToast(); Settle(view);
    Expect(!view.Line() && !view.Expanded() && view.Frame().height >= 48,
        "Low-battery notification did not restore the compact media state");
    view.SetPointerInside(true, start + 800s); view.SetPointerInside(false, start + 801s);
    Expect(!view.RefreshIdle(start + 805999ms) && view.RefreshIdle(start + 806s),
        "Restored compact notification state lost its five-second grace period");

    // Interrupt both directions while preserving finite, centered fixed-host geometry.
    for (int i = 0; i < 100; ++i)
    {
        view.RevealCapsule(start + 900s + std::chrono::seconds(i)); view.Step(1.0 / 144);
        const auto before = view.Frame();
        view.ShowLine();
        const auto after = view.Frame();
        Expect(before.width == after.width && before.height == after.height,
            "Line reversal jumped instead of preserving current animated geometry");
        view.Step(1.0 / 144);
        const auto frame = view.Frame();
        Expect(std::isfinite(frame.width) && std::isfinite(frame.height) && frame.width >= 88 &&
            frame.height >= 8 && frame.centerX == view.Layout().hostWidth / 2 && frame.top == view.Layout().insetTop,
            "Rapid line reversal changed its anchor or invalidated its geometry");
    }
    Settle(view);
}
