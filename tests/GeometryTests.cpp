#include "../Graphics/LayeredRenderer.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace winisland;
void TestAnimation();
void TestServiceView();
void TestSystemBehavior();
void TestLineState();

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int main()
{
    HWND window = nullptr;
    try
    {
        TestAnimation();
        TestServiceView();
        TestSystemBehavior();
        TestLineState();
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        window = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            L"STATIC", L"WinHalo surface test", WS_POPUP, 0, 0, 480, 400, nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        Require(window != nullptr, "Test window creation failed");
        const DWORD baseline = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        {
            IslandView view;
            view.RevealCapsule();
            view.Configure({}, true);
            LayeredRenderer renderer;
            for (int cycle = 0; cycle < 12; ++cycle)
                for (UINT dpi : {96u, 120u, 144u, 192u})
                {
                    const float scale = static_cast<float>(dpi) / 96.0f;
                    const SIZE size{static_cast<LONG>(480 * scale), static_cast<LONG>(400 * scale)};
                    renderer.Resize(size, dpi);
                    view.ShowLine();
                    renderer.Render(window, view);
                    Require(renderer.HitTest({static_cast<LONG>(240 * scale), static_cast<LONG>(20 * scale)}),
                        "Visible line is not interactive at the monitor DPI");
                    Require(!renderer.HitTest({static_cast<LONG>(240 * scale), static_cast<LONG>(38 * scale)}) &&
                        !renderer.HitTest({static_cast<LONG>(190 * scale), static_cast<LONG>(20 * scale)}),
                        "Invisible compact area intercepts input around the line");
                    view.RevealCapsule();
                    renderer.Render(window, view);
                    Require(renderer.HitTest({static_cast<LONG>(240 * scale), static_cast<LONG>(38 * scale)}),
                        "Capsule center is not interactive");
                    Require(!renderer.HitTest({0, 0}), "Host corner is not transparent");
                    Require(!renderer.HitTest({static_cast<LONG>(240 * scale), static_cast<LONG>(100 * scale)}),
                        "Empty host area is not transparent");
                    Require(!renderer.HitTest({static_cast<LONG>(150 * scale), static_cast<LONG>(16 * scale)}),
                        "Rounded capsule corner is not transparent");
                    Require(!renderer.HitTest({-1, -1}) && !renderer.HitTest({size.cx, size.cy}),
                        "Out-of-bounds hit test failed");
                    Require(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= baseline + 2,
                        "GDI objects grew after surface resize");
                    Require(view.SetState(IslandState::Hover), "Hover state did not change");
                    Require(!view.SetState(IslandState::Hover), "Duplicate hover event was not deduplicated");
                    renderer.Render(window, view);
                    view.SetState(IslandState::Idle);
                }
        }
        Require(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == baseline,
            "Renderer did not release all GDI objects");
        for (float width : {-1.0f, std::numeric_limits<float>::quiet_NaN()})
        {
            bool rejected = false;
            try { IslandLayout layout; layout.width = width; IslandView invalid(layout); }
            catch (const std::invalid_argument&) { rejected = true; }
            Require(rejected, "Invalid layout was accepted");
        }
        DestroyWindow(window);
        std::cout << "PASS: spring damping modes, refresh-rate independence, interruption, pause handling, "
            "1000 reversals, reduced motion; media control capabilities and timeline extrapolation; "
            "volume sanitization, toast lifecycle, controls and settings preservation; "
            "battery decoding/hysteresis, system settings, visibility policy and isolated startup registry; "
            "media-only navigation, compact media takeover and conditional equalizer/progress frames; "
            "line reveal, exact five-second idle deadline, re-entry, pause, notifications and hidden restoration; "
            "100/125/150/200% surface geometry, alpha hit testing, hover deduplication, "
            "48 DPI resizes, GDI release and invalid layout rejection\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        if (window) DestroyWindow(window);
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
