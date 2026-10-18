#include "../Animation/Spring.h"
#include "../Animation/AnimationManager.h"
#include "../UI/IslandView.h"
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
}

void TestAnimation()
{
    for (double damping : {32.0, 40.0, 80.0})
    {
        Spring spring(0);
        spring.Configure({400, damping, 1, 1});
        spring.SetTarget(100);
        for (int i = 0; i < 1500; ++i) spring.Step(1.0 / 144);
        Expect(!spring.Active() && spring.Value() == 100 && spring.Velocity() == 0,
            "Under/critical/overdamped spring did not settle");
    }
    Spring sixty, highRefresh;
    sixty.SetTarget(100); highRefresh.SetTarget(100);
    for (int i = 0; i < 30; ++i) sixty.Step(1.0 / 60);
    for (int i = 0; i < 72; ++i) highRefresh.Step(1.0 / 144);
    Expect(std::abs(sixty.Value() - highRefresh.Value()) < 1e-8 &&
        std::abs(sixty.Velocity() - highRefresh.Velocity()) < 1e-8,
        "Spring trajectory changes with render rate");
    Spring interrupted(120);
    interrupted.SetTarget(380);
    interrupted.Step(0.03);
    const double position = interrupted.Value(), velocity = interrupted.Velocity();
    interrupted.SetTarget(120);
    Expect(interrupted.Value() == position && interrupted.Velocity() == velocity,
        "Retargeting discarded position or velocity");
    Spring pause, clamped;
    pause.SetTarget(100); clamped.SetTarget(100);
    pause.Step(10); clamped.Step(0.05);
    Expect(pause.Value() == clamped.Value(), "Long frame interval was not clamped");
    bool invalidRejected = false;
    try { pause.Configure({460, 32, 0, 1}); }
    catch (const std::invalid_argument&) { invalidRejected = true; }
    Expect(invalidRejected, "Zero spring mass was accepted");
    AnimationManager properties;
    AnimationValues zero{}, targets{};
    targets.fill(10);
    properties.Reset(zero);
    properties.Retarget(targets, false);
    properties.Step(0.03);
    for (size_t i = 0; i < targets.size(); ++i)
        Expect(properties.Get(static_cast<Property>(i)) > 0, "An animated property was not updated");
    IslandView view;
    view.RevealCapsule();
    for (int i = 0; i < 600; ++i) view.Step(1.0 / 144);
    for (int i = 0; i < 1000; ++i)
    {
        view.Toggle();
        view.Step(1.0 / 144);
        const auto frame = view.Frame();
        Expect(std::isfinite(frame.width) && std::isfinite(frame.height) && frame.width >= 16 &&
            frame.width <= view.Layout().hostWidth - 4 && frame.centerX == view.Layout().hostWidth / 2,
            "Rapid reversal invalidated capsule geometry or centering");
    }
    view.Collapse();
    for (int i = 0; i < 600; ++i) view.Step(1.0 / 144);
    Expect(!view.Animating() && view.Frame().width == view.Layout().width, "View did not stop after settling");
    view.Configure({}, true);
    view.Toggle();
    Expect(!view.Animating() && view.Frame().width == view.Layout().expandedWidth && view.Frame().opacity == 1,
        "Reduced motion did not apply targets immediately");
}
