#include "../Core/Platform.h"
#include "../Core/Configuration.h"
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <winrt/base.h>

using namespace winisland;
namespace
{
void Expect(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
}

void TestSettings()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    const auto folder = std::filesystem::temp_directory_path() / (L"WinHaloSettingsTest-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);
    wchar_t oldDirectory[32768]{};
    const DWORD oldLength = GetEnvironmentVariableW(L"WINHALO_DATA_DIR", oldDirectory, 32768);
    struct Cleanup {
        std::filesystem::path folder; std::wstring previous;
        ~Cleanup() {
            SetEnvironmentVariableW(L"WINHALO_DATA_DIR", previous.empty() ? nullptr : previous.c_str());
            std::error_code ignored; std::filesystem::remove_all(folder, ignored);
            winrt::uninit_apartment();
        }
    } cleanup{folder, oldLength && oldLength < 32768 ? oldDirectory : L""};
    SetEnvironmentVariableW(L"WINHALO_DATA_DIR", folder.c_str());
    Logger logger;
    ConfigurationStore store(logger);
    Configuration c;
    c.layout.compactScale = .5f; c.layout.expandedScale = 2;
    c.layout.expandedWidth = 900; c.layout.expandedHeight = 600;
    c.layout.offsetX = -77; c.layout.offsetY = 25; c.layout.lineWidth = 120;
    c.layout.fontSize = 19; c.monitorDevice = L"\\\\.\\DISPLAY2";
    c.language = L"en-US"; c.autoHideIdle = false; c.clipboardLinks = true;
    c.privacyIndicators = true; c.rightDrag = false; c.systemControls = false; c.showNotifications = false;
    NormalizeConfiguration(c);
    Expect(c.layout.hostWidth >= 1860 && c.layout.hostHeight >= 1264, "Scaled player does not fit its fixed host");
    Expect(store.Save(c), "Settings save failed");
    const auto copy = store.Load();
    Expect(copy.layout.offsetX == -77 && copy.layout.lineWidth == 120 && copy.layout.fontSize == 19 &&
        copy.layout.compactScale == .5f && copy.layout.expandedScale == 2 && copy.layout.expandedHeight == 600 &&
        copy.monitorDevice == c.monitorDevice && copy.language == L"en-US" && !copy.autoHideIdle && copy.clipboardLinks &&
        copy.privacyIndicators && !copy.rightDrag && !copy.systemControls && !copy.showNotifications,
        "Appearance or behavior settings did not survive restart");
    c.layout.width = std::numeric_limits<float>::infinity(); c.layout.height = -1;
    c.layout.expandedScale = 99; c.layout.fontSize = 3; c.layout.lineWidth = -100;
    NormalizeConfiguration(c);
    Expect(c.layout.width == 180 && c.layout.height == 32 && c.layout.expandedScale == 2 &&
        c.layout.fontSize == 10 && c.layout.lineWidth == 24, "Invalid settings escaped normalization");
    std::ofstream(folder / L"settings.json") << "{\"version\":2,\"expandedHeight\":340,\"autoHideFullscreen\":false}";
    const auto migrated = store.Load();
    Expect(migrated.layout.expandedHeight == 200 && !migrated.autoHideFullscreen && migrated.autoHideIdle,
        "Old embedded-settings height or behavior did not migrate");
    std::ofstream(folder / L"settings.json") << "invalid json";
    Expect(store.Load().layout.width == 180, "Corrupt settings did not recover defaults");

    IslandView view;
    view.Configure({}, true);
    auto layout = IslandLayout{}; layout.lineWidth = 120;
    view.ApplyLayout(layout, false);
    view.SetPointerInside(false);
    Expect(!view.Line() && view.IdleDeadline() == IslandView::Clock::time_point::max(),
        "Disabling idle hide did not retain the capsule");
    view.ApplyLayout(layout, true); view.ShowLine();
    Expect(view.Line() && view.Frame().width == 120, "Configured hidden width was ignored");
    MediaSnapshot media; media.available = media.canToggle = true; view.SetMedia(media);
    layout.expandedScale = .5f; view.ApplyLayout(layout, true); view.RevealCapsule(); view.Toggle();
    Expect(view.Frame().width == 210 && view.Frame().height == 100 && view.Media().available,
        "Live scaling lost the current session or dimensions");
    const auto play = view.MediaButton(ViewAction::MediaToggle);
    Expect(view.ActionAt((play.left + play.right) / 2, (play.top + play.bottom) / 2) == ViewAction::MediaToggle,
        "Scaled player button no longer matches its hit target");
    view.Collapse(); view.ShowNotice(L"Link", L"example.com", true);
    const auto open = view.NoticeButton(true);
    Expect(view.ActionAt((open.left + open.right) / 2, (open.top + open.bottom) / 2) == ViewAction::OpenLink &&
        view.IdleDeadline() == IslandView::Clock::time_point::max(), "Link prompt cannot confirm or pauses incorrectly");
    view.DismissTransientToast();
    Expect(!view.Notice() && !view.Expanded(), "Dismissing notice changed the player state");
}
