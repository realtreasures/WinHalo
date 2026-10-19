#include "../Graphics/CompositionRenderer.h"
#include "../Core/Configuration.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <dwmapi.h>
#include <roapi.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#pragma comment(lib, "dwmapi.lib")

using namespace winisland;

namespace
{
void Expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void Submit(CompositionRenderer& renderer, HWND window, const IslandView& view)
{
    const auto baseline = renderer.Frames();
    renderer.Render(window, view);
    for (int attempts = 0; renderer.Frames() == baseline && attempts < 3; ++attempts)
    {
        Expect(WaitForSingleObject(renderer.FrameReadyHandle(), 2000) == WAIT_OBJECT_0, "Frame latency wait failed");
        renderer.NotifyFrameReady();
        renderer.Render(window, view);
    }
    Expect(renderer.Frames() > baseline, "Composition did not submit a frame");
    Expect(GetForegroundWindow() != window, "Rendering activated the fixture window");
}

void FlushDesktop()
{
    // Wait for the compositor rather than guessing when a submitted frame is visible.
    Expect(SUCCEEDED(DwmFlush()) && SUCCEEDED(DwmFlush()), "Desktop composition flush failed");
}

COLORREF ScreenPixel(HWND window, int x, int y)
{
    POINT point{x, y};
    Expect(ClientToScreen(window, &point) != FALSE, "Convert test pixel to screen failed");
    HDC screen = GetDC(nullptr);
    Expect(screen != nullptr, "Acquire test screen DC failed");
    const COLORREF color = GetPixel(screen, point.x, point.y);
    ReleaseDC(nullptr, screen);
    Expect(color != CLR_INVALID, "Read composed test pixel failed");
    return color;
}

MediaSnapshot FixtureMedia()
{
    // Deliberately synthetic: no system media manager, player or audio endpoint is used.
    MediaSnapshot media;
    media.available = media.canToggle = media.canPrevious = media.canNext = true;
    media.title = L"超长测试标题 " + std::wstring(120, L'W');
    media.album = L"Fixture album";
    media.artist = L"Fixture artist " + std::wstring(100, L'M');
    media.source = L"Local renderer fixture";
    auto app = std::make_shared<MediaAppInfo>();
    app->source = media.source; app->name = L"Synthetic app A";
    app->iconWidth = app->iconHeight = 4;
    for (int i = 0; i < 16; ++i) app->iconBgra.insert(app->iconBgra.end(), {28, 200, 70, 255});
    media.app = app; media.sessionId = 1;
    media.sessions = {{1, media.source, app->name, false}, {2, L"Synthetic B", L"Synthetic app B", true}};
    media.positionSeconds = 90;
    media.durationSeconds = 240;
    media.artworkWidth = media.artworkHeight = 4;
    media.artworkRevision = 1;
    // Half-transparent premultiplied BGRA; every color channel is <= alpha.
    for (int i = 0; i < 16; ++i)
        media.artworkBgra.insert(media.artworkBgra.end(), {16, 80, 112, 128});
    return media;
}

void TestLineContent(CompositionRenderer& renderer, HWND window, IslandView& view)
{
    renderer.Resize({480, 400}, 96);
    view.Configure({}, true);
    view.ShowLine();
    Submit(renderer, window, view);
    RECT rect{};
    Expect(GetWindowRect(window, &rect) != FALSE, "Read line fixture bounds failed");
    struct Backdrop
    {
        HWND window = nullptr;
        ~Backdrop() { if (window) DestroyWindow(window); }
    } backdrop;
    backdrop.window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"WinHalo line backdrop",
        WS_POPUP | SS_WHITERECT, rect.left, rect.top, 480, 400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Expect(backdrop.window != nullptr, "Create line test backdrop failed");
    Expect(SetWindowPos(backdrop.window, HWND_TOPMOST, rect.left, rect.top, 480, 400,
        SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE, "Show line test backdrop failed");
    Expect(UpdateWindow(backdrop.window) != FALSE, "Paint line test backdrop failed");
    Expect(SetWindowPos(window, HWND_TOPMOST, rect.left, rect.top, 480, 400,
        SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE, "Show line fixture failed");
    FlushDesktop();
    const auto lineColor = ScreenPixel(window, 240, 20);
    Expect(GetRValue(lineColor) < 80 && GetGValue(lineColor) < 80 && GetBValue(lineColor) < 90,
        "Resting line did not render its dark graphite material");
    Expect(ScreenPixel(window, 240, 35) == RGB(255, 255, 255) && ScreenPixel(window, 190, 20) == RGB(255, 255, 255),
        "Resting line painted into the transparent host area");
    auto media = FixtureMedia();
    media.playing = true;
    media.artworkBgra.clear();
    for (int pixel = 0; pixel < 16; ++pixel)
        media.artworkBgra.insert(media.artworkBgra.end(), {180, 30, 60, 255});
    for (int update = 0; update < 16; ++update)
    {
        media.title = L"Hidden track " + std::to_wstring(update);
        media.artworkRevision = 100 + update;
        media.positionSeconds = update;
        view.SetMedia(media);
        Submit(renderer, window, view);
        FlushDesktop();
        Expect(view.Line() && !view.ContinuousFrames(), "Music update woke or animated the resting line");
        Expect(ScreenPixel(window, 240, 20) == lineColor && ScreenPixel(window, 240, 35) == RGB(255, 255, 255),
            "Artwork, equalizer or clock content polluted the resting line");
        Expect(renderer.HitTest({240, 20}) && !renderer.HitTest({240, 35}) && !renderer.HitTest({190, 20}),
            "Music update changed the resting line hit region");
    }
    renderer.ResetDevice();
    Submit(renderer, window, view);
    FlushDesktop();
    Expect(ScreenPixel(window, 240, 20) == lineColor, "Device recreation changed or polluted the resting line");
    view.RevealCapsule();
    Submit(renderer, window, view);
    FlushDesktop();
    const auto cover = ScreenPixel(window, 120, 40);
    Expect(GetRValue(cover) < 100 && GetGValue(cover) < 75 && GetBValue(cover) > 140,
        "Revealed capsule did not prepare the latest artwork after hidden media updates and device reset");
    ShowWindow(window, SW_HIDE);
}

void TestDynamicContent(CompositionRenderer& renderer, HWND window, IslandView& view)
{
    renderer.Resize({480, 400}, 96);
    view.Configure({}, true);
    auto media = FixtureMedia();
    view.SetMedia(media);
    view.ShowPage(IslandPage::Media);
    Submit(renderer, window, view);
    MONITORINFO monitor{sizeof(MONITORINFO)};
    Expect(GetMonitorInfoW(MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY), &monitor) != FALSE,
        "Locate fixture monitor failed");
    const HWND foreground = GetForegroundWindow();
    Expect(SetWindowPos(window, HWND_TOPMOST, monitor.rcWork.left + 40, monitor.rcWork.top + 120,
        480, 400, SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE, "Show fixture without activation failed");
    Expect(GetForegroundWindow() == foreground, "Renderer fixture stole foreground focus");
    FlushDesktop();
    const COLORREF translucent = ScreenPixel(window, 80, 76);
    if (!(GetRValue(translucent) > 100 && GetGValue(translucent) > 65 && GetBValue(translucent) < 65))
    {
        POINT point{80, 76}; ClientToScreen(window, &point);
        std::cerr << "Cover pixel RGB=" << static_cast<unsigned>(GetRValue(translucent)) << ','
            << static_cast<unsigned>(GetGValue(translucent)) << ',' << static_cast<unsigned>(GetBValue(translucent))
            << " hit=" << WindowFromPoint(point) << " fixture=" << window
            << " height=" << view.Frame().height << " opacity=" << view.Frame().opacity << '\n';
    }
    Expect(GetRValue(translucent) > 100 && GetGValue(translucent) > 65 && GetBValue(translucent) < 65,
        "Premultiplied fixture cover was not drawn over the cover background");
    const auto firstIcon = ScreenPixel(window, 64, 176);
    Expect(GetGValue(firstIcon) > 180 && GetRValue(firstIcon) < 100, "Software icon was not drawn in the source control");
    Expect(GetRValue(ScreenPixel(window, 425, 35)) < 40, "Music player still drew its old collapse arrow");
    // A long title must remain visible while trimming before the close-button gutter.
    bool titleVisible = false;
    for (int y = 47; y < 76; ++y)
        for (int x = 145; x < 380; x += 3)
            if (GetRValue(ScreenPixel(window, x, y)) > 150) titleVisible = true;
    Expect(titleVisible, "Long media title disappeared instead of being trimmed");
    for (int y = 47; y < 76; ++y)
        for (int x = 418; x < 425; ++x)
            Expect(GetRValue(ScreenPixel(window, x, y)) < 80,
                "Long media title overflowed the trimmed layout into the close-button gutter");

    media.artworkRevision = 2;
    auto app = std::make_shared<MediaAppInfo>(*media.app);
    app->iconBgra.clear();
    for (int i = 0; i < 16; ++i) app->iconBgra.insert(app->iconBgra.end(), {220, 80, 20, 255});
    media.app = app;
    media.artworkBgra.clear();
    for (int i = 0; i < 16; ++i)
        media.artworkBgra.insert(media.artworkBgra.end(), {180, 30, 60, 255});
    view.SetMedia(media);
    Submit(renderer, window, view);
    FlushDesktop();
    const auto expectBlueCover = [&]
    {
        const COLORREF color = ScreenPixel(window, 80, 76);
        Expect(GetRValue(color) < 100 && GetGValue(color) < 75 && GetBValue(color) > 140,
            "Changed artwork revision did not replace or restore the fixture cover");
        const auto icon = ScreenPixel(window, 64, 176);
        Expect(GetBValue(icon) > 180 && GetRValue(icon) < 60,
            "Switched software icon was not uploaded or restored after device reset");
    };
    expectBlueCover();
    const DWORD dynamicBaseline = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const auto firstDynamicFrame = renderer.Frames();
    for (int update = 0; update < 16; ++update)
    {
        media.available = update % 4 != 0;
        media.playing = update % 2 == 0;
        media.canNext = update % 3 != 0;
        media.positionSeconds = 60 + update;
        media.title = L"Fixture update " + std::to_wstring(update) + std::wstring(120, L'W');
        view.SetMedia(media);
        view.ShowPage(IslandPage::Media);
        Submit(renderer, window, view);
        Expect(renderer.HitTest({210, 110}) == media.available && !renderer.HitTest({0, 0}) &&
            view.Expanded() == media.available,
            "Dynamic media updates failed to collapse an ended session or restore its player");
        Expect(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= dynamicBaseline,
            "Repeated media content updates leaked GDI resources");
    }
    Expect(renderer.Frames() >= firstDynamicFrame + 16, "Dynamic media updates were not submitted");
    FlushDesktop();
    expectBlueCover();
    // Same revision and same fixture bytes must be uploaded again to the new D2D device.
    renderer.ResetDevice();
    Submit(renderer, window, view);
    FlushDesktop();
    expectBlueCover();
    Expect(renderer.HitTest({210, 110}), "Device recreation lost the media panel region");

    VolumeSnapshot volume;
    volume.available = true;
    volume.level = 0.2f;
    view.Collapse();
    view.RevealCapsule();
    view.SetMedia({});
    view.SetVolume(volume, false);
    Submit(renderer, window, view);
    volume.level = 0.64f;
    view.SetVolume(volume, true);
    Submit(renderer, window, view);
    Expect(renderer.HitTest({110, 58}) && renderer.HitTest({240, 80}) && !renderer.HitTest({210, 140}),
        "Volume toast submitted an incorrect visible or interactive region");
    FlushDesktop();
    const COLORREF filled = ScreenPixel(window, 180, 69);
    Expect(GetRValue(filled) > 110 && GetGValue(filled) > 200 && GetBValue(filled) > 150,
        "Volume toast level was not rendered into the filled bar");
    volume.muted = true;
    view.SetVolume(volume, true);
    Submit(renderer, window, view);
    FlushDesktop();
    const COLORREF muted = ScreenPixel(window, 180, 69);
    Expect(GetRValue(muted) < 80 && GetGValue(muted) < 80 && GetBValue(muted) < 80,
        "Muted volume toast retained a filled level bar");
    view.DismissVolumeToast();
    Submit(renderer, window, view);
    Expect(renderer.HitTest({240, 38}) && !renderer.HitTest({110, 58}) && !renderer.HitTest({240, 80}),
        "Dismissed volume toast did not restore the capsule hit region");
    BatterySnapshot battery{true, true, false, 15, true};
    view.SetBattery(battery, false);
    Submit(renderer, window, view);
    FlushDesktop();
    const auto idleBattery = ScreenPixel(window, 300, 37);
    Expect(GetRValue(idleBattery) > 220 && GetGValue(idleBattery) > 120 && GetBValue(idleBattery) < 100,
        "Collapsed low battery icon was not rendered");
    view.SetBattery(battery, true);
    Submit(renderer, window, view);
    FlushDesktop();
    Expect(renderer.HitTest({110, 58}) && !renderer.HitTest({210, 140}), "Low battery toast region is wrong");
    const auto warning = ScreenPixel(window, 120, 55);
    Expect(GetRValue(warning) > 220 && GetGValue(warning) > 120 && GetBValue(warning) < 100,
        "Low battery toast warning icon was not rendered");
    battery.onBattery = false; battery.charging = true;
    view.SetBattery(battery, false);
    Submit(renderer, window, view);
    Expect(!renderer.HitTest({110, 58}) && !view.BatteryToast(), "Charging did not dismiss the battery toast");
    view.ShowSystemSettings();
    view.SetSystemOptions(true, false);
    Submit(renderer, window, view);
    FlushDesktop();
    Expect(renderer.HitTest({210, 110}) && view.SystemSettings(), "System settings did not render");

    // A small player must scale its content clip with the controls. The left
    // side of the source icon lies within 12 DIP of the panel edge, so a
    // fixed 12-DIP clip would remove this otherwise visible part of the icon.
    auto smallLayout = IslandLayout{};
    smallLayout.expandedWidth = 320;
    smallLayout.expandedHeight = 200;
    smallLayout.expandedScale = .5f;
    view.ApplyLayout(smallLayout, true);
    view.Configure({}, true);
    view.SetMedia(FixtureMedia());
    view.ShowPage(IslandPage::Media);
    Submit(renderer, window, view);
    FlushDesktop();
    const auto smallBounds = view.Bounds();
    const float fit = std::min((smallBounds.right - smallBounds.left) / 420.0f,
        (smallBounds.bottom - smallBounds.top) / 200.0f);
    const float originX = (smallBounds.left + smallBounds.right - 420 * fit) / 2;
    const float originY = (smallBounds.top + smallBounds.bottom - 200 * fit) / 2;
    const POINT sourceLeft{static_cast<LONG>(std::lround(originX + 24 * fit)),
        static_cast<LONG>(std::lround(originY + 160 * fit))};
    const auto smallIcon = ScreenPixel(window, sourceLeft.x, sourceLeft.y);
    Expect(GetGValue(smallIcon) > 180 && GetRValue(smallIcon) < 100 && GetBValue(smallIcon) < 70,
        "Small player clipped the left side of its green source icon");
    Expect(renderer.HitTest(sourceLeft), "Visible small source icon lost its native hit region");
    for (const auto action : {ViewAction::MediaPrevious, ViewAction::MediaToggle, ViewAction::MediaNext})
    {
        const auto button = view.MediaButton(action);
        const float x = (button.left + button.right) / 2, y = (button.top + button.bottom) / 2;
        Expect(view.ActionAt(x, y) == action, "Small player control did not match its scaled hit target");
        Expect(renderer.HitTest({static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y))}),
            "Small player control lies outside the visible native region");
    }
    const auto sourceButton = view.MediaSourceButton();
    Expect(view.ActionAt((sourceButton.left + sourceButton.right) / 2,
        (sourceButton.top + sourceButton.bottom) / 2) == ViewAction::NextMediaSession,
        "Small player source control did not select its second synthetic session");
    smallLayout.fontSize = 28;
    view.ApplyLayout(smallLayout, true);
    Submit(renderer, window, view);
    FlushDesktop();
    Expect(view.Expanded() && view.Page() == IslandPage::Media,
        "Large font configuration changed the small player's page");

    // Later line-content checks use default geometry and automatic fonts.
    view.ApplyLayout(IslandLayout{}, true);
    view.SetMedia({});
    view.ShowLine();
    Submit(renderer, window, view);
    ShowWindow(window, SW_HIDE);
    // Other desktop applications may switch foreground during the pixel scan;
    // the renderer's NOACTIVATE window must never become the foreground window.
    Expect(GetForegroundWindow() != window, "Renderer fixture became the foreground window");
}
}

int main()
{
    HWND window = nullptr;
    bool initialized = false;
    try
    {
        Expect(SUCCEEDED(RoInitialize(RO_INIT_SINGLETHREADED)), "RoInitialize failed");
        initialized = true;
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const auto data = std::filesystem::current_path() / L"artifacts" / L"gpu-test-data";
        std::filesystem::create_directories(data);
        Expect(SetEnvironmentVariableW(L"WINHALO_DATA_DIR", data.c_str()) != FALSE, "Set test directory failed");
        Logger logger;
        ConfigurationStore store(logger);
        Configuration config;
        config.spring.speed = 1.5;
        config.reducedMotion = true;
        config.autoHideFullscreen = false;
        config.nativeGlass = true;
        Expect(store.Save(config), "JSON settings save failed");
        const auto loaded = store.Load();
        Expect(loaded.spring.speed == 1.5 && loaded.reducedMotion && loaded.layout.width == 180 && !loaded.autoHideFullscreen && loaded.nativeGlass,
            "JSON settings round trip failed");
        {
            std::ofstream corrupt(data / L"settings.json", std::ios::trunc);
            corrupt << "{invalid json";
        }
        const auto defaults = store.Load();
        Expect(defaults.spring.speed == 1 && !defaults.reducedMotion && defaults.autoHideFullscreen, "Malformed settings did not fall back to defaults");
        {
            std::ofstream legacy(data / L"settings.json", std::ios::trunc);
            legacy << "{\"version\":1,\"reducedMotion\":true}";
        }
        Expect(store.Load().autoHideFullscreen && !store.Load().nativeGlass, "Legacy settings did not default to safe material and fullscreen hiding");
        {
            std::ofstream legacy(data / L"settings.json", std::ios::trunc);
            legacy << "{\"version\":1,\"width\":120,\"height\":36,\"radius\":18,\"expandedWidth\":380,\"expandedHeight\":240,\"speed\":1.5,\"autoHideFullscreen\":false}";
        }
        const auto upgraded = store.Load();
        Expect(upgraded.layout.width == 180 && upgraded.layout.height == 44 && upgraded.layout.radius == 22 &&
            upgraded.layout.expandedWidth == 420 && upgraded.layout.expandedHeight == 200 && upgraded.spring.speed == 1.5 && !upgraded.autoHideFullscreen,
            "Version 1 migration lost preferences or kept obsolete default dimensions");
        window = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            L"STATIC", L"WinHalo composition test", WS_POPUP, 0, 0, 480, 400, nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        Expect(window != nullptr, "GPU test window creation failed");
        IslandView view;
        view.Configure({}, true);
        view.RevealCapsule();
        {
            CompositionRenderer renderer(logger);
            renderer.Resize({480, 400}, 96);
            Submit(renderer, window, view);
            const DWORD baseline = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            for (int cycle = 0; cycle < 6; ++cycle)
                for (UINT dpi : {96u, 120u, 144u, 192u})
                {
                    const float scale = static_cast<float>(dpi) / 96;
                    renderer.Resize({static_cast<LONG>(480 * scale), static_cast<LONG>(400 * scale)}, dpi);
                    view.ShowLine();
                    Submit(renderer, window, view);
                    const auto line = view.Frame();
                    Expect(line.width == 88 && line.height == 8 && line.radius == 4 && line.compactOpacity == 0,
                        "Resting line dimensions changed across DPI");
                    Expect(renderer.HitTest({static_cast<LONG>(240 * scale), static_cast<LONG>(20 * scale)}) &&
                        !renderer.HitTest({static_cast<LONG>(240 * scale), static_cast<LONG>(28 * scale)}) &&
                        !renderer.HitTest({static_cast<LONG>(190 * scale), static_cast<LONG>(20 * scale)}),
                        "GPU resting line hit region failed across DPI");
                    view.RevealCapsule();
                    Submit(renderer, window, view);
                    Expect(renderer.HitTest({static_cast<LONG>(240 * scale), static_cast<LONG>(38 * scale)}),
                        "GPU capsule center hit failed");
                    Expect(!renderer.HitTest({0, 0}) && !renderer.HitTest({static_cast<LONG>(240 * scale), static_cast<LONG>(100 * scale)}),
                        "GPU empty host hit failed");
                    view.Configure({}, true);
                    view.ShowSettings();
                    Submit(renderer, window, view);
                    Expect(renderer.HitTest({static_cast<LONG>(240 * scale), static_cast<LONG>(110 * scale)}),
                        "GPU expanded hit area failed");
                    view.Collapse();
                    view.RevealCapsule();
                    Submit(renderer, window, view);
                    Expect(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= baseline + 2,
                        "GPU window regions leaked after layout updates");
                }
            for (int cycle = 0; cycle < 3; ++cycle)
            {
                renderer.ResetDevice();
                Submit(renderer, window, view);
                Expect(renderer.HitTest({480, 76}), "Device recreation lost geometry");
            }
            TestDynamicContent(renderer, window, view);
            TestLineContent(renderer, window, view);
            std::wcout << renderer.Diagnostics() << L'\n';
        }
        DestroyWindow(window); window = nullptr;
        RoUninitialize(); initialized = false;
        std::cout << "PASS: JSON round trip and malformed fallback; composition 100/125/150/200% DPI, "
            "24 buffer resizes, line/capsule/expanded hit regions, bounded GDI resources, device recreations; "
            "synthetic premultiplied covers, revision changes and post-reset cover restoration, long title trimming, "
            "16 dynamic media updates, rendered volume/mute and battery toast, system settings and legacy JSON; "
            "small-player source-icon pixels and scaled controls, large-font submission; "
            "graphite line, transparent host and 16 hidden media updates with latest-cover reveal (no real media or audio session)\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        if (window) DestroyWindow(window);
        if (initialized) RoUninitialize();
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
