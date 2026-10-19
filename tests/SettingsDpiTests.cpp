#include "../Core/Platform.h"
#include "../Core/Configuration.h"
#include "../Window/SettingsWindow.h"
#include "../Window/SettingsPainter.h"
#include <dwmapi.h>
#include <roapi.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace winisland;

namespace
{
void Expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
struct RestoreForeground
{
    HWND original = GetForegroundWindow();
    ~RestoreForeground() { if (original && IsWindow(original)) SetForegroundWindow(original); }
};
struct Apartment
{
    Apartment() { Expect(SUCCEEDED(RoInitialize(RO_INIT_SINGLETHREADED)), "STA initialization failed"); }
    ~Apartment() { RoUninitialize(); }
};

void Pump(SettingsWindow& settings)
{
    MSG message{};
    unsigned int dispatched = 0;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        Expect(message.message != WM_QUIT, "Closing settings posted WM_QUIT");
        Expect(++dispatched < 4096, "Settings message queue failed to settle");
        if (settings.PreTranslate(message)) continue;
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
void Paint(SettingsWindow& settings)
{
    Pump(settings);
    Expect(RedrawWindow(settings.Handle(), nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW) != FALSE,
        "Settings redraw failed");
    Pump(settings);
}

class OwnedDib
{
public:
    OwnedDib(int width, int height) : width_(width), height_(height)
    {
        dc_ = CreateCompatibleDC(nullptr);
        if (!dc_) throw std::runtime_error("Could not create owned pixel-test DC");
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width_; info.bmiHeader.biHeight = -height_;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        void* data = nullptr;
        bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &data, nullptr, 0);
        if (!bitmap_ || !data) { if (bitmap_) DeleteObject(bitmap_); DeleteDC(dc_); throw std::runtime_error("Owned DIB allocation failed"); }
        pixels_ = static_cast<std::uint32_t*>(data);
        previous_ = SelectObject(dc_, bitmap_);
        if (!previous_ || previous_ == HGDI_ERROR)
        {
            DeleteObject(bitmap_); DeleteDC(dc_);
            throw std::runtime_error("Could not select owned pixel-test DIB");
        }
    }
    ~OwnedDib() { SelectObject(dc_, previous_); DeleteObject(bitmap_); DeleteDC(dc_); }
    OwnedDib(const OwnedDib&) = delete;
    OwnedDib& operator=(const OwnedDib&) = delete;
    HDC Dc() const noexcept { return dc_; }
    int Width() const noexcept { return width_; }
    int Height() const noexcept { return height_; }
    COLORREF Pixel(int x, int y) const noexcept
    {
        const auto value = pixels_[static_cast<size_t>(y) * static_cast<size_t>(width_) + static_cast<size_t>(x)];
        return RGB((value >> 16) & 255u, (value >> 8) & 255u, value & 255u);
    }
    void Print(HWND mainWindow, HWND control)
    {
        // Native owner-draw BUTTON does not implement PrintWindow reliably.
        // Print the owned settings window (which implements WM_PRINTCLIENT),
        // then copy the exact native child rectangle into this pixel fixture.
        RECT main{}, child{};
        Expect(GetWindowRect(mainWindow, &main) && GetWindowRect(control, &child),
            "Could not locate the owned navigation capture region");
        const int mainWidth = main.right - main.left, mainHeight = main.bottom - main.top;
        const int x = child.left - main.left, y = child.top - main.top;
        Expect(x >= 0 && y >= 0 && x + width_ <= mainWidth && y + height_ <= mainHeight,
            "Navigation capture region exceeds the owned settings window");
        OwnedDib complete(mainWidth, mainHeight);
        RECT rectangle{0, 0, mainWidth, mainHeight};
        FillRect(complete.Dc(), &rectangle, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        Expect(PrintWindow(mainWindow, complete.Dc(), PW_RENDERFULLCONTENT) != FALSE,
            "Owned settings PrintWindow failed during navigation capture");
        GdiFlush();
        Expect(BitBlt(dc_, 0, 0, width_, height_, complete.Dc(), x, y, SRCCOPY) != FALSE,
            "Could not copy the owned navigation rectangle from the settings render");
        GdiFlush();
    }
    void Save(const std::filesystem::path& path) const
    {
        BITMAPINFOHEADER information{}; information.biSize = sizeof(information);
        information.biWidth = width_; information.biHeight = -height_;
        information.biPlanes = 1; information.biBitCount = 32; information.biCompression = BI_RGB;
        const DWORD bytes = static_cast<DWORD>(width_) * static_cast<DWORD>(height_) * 4;
        BITMAPFILEHEADER header{}; header.bfType = 0x4D42;
        header.bfOffBits = static_cast<DWORD>(sizeof(header) + sizeof(information)); header.bfSize = header.bfOffBits + bytes;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(&header), sizeof(header));
        output.write(reinterpret_cast<const char*>(&information), sizeof(information));
        output.write(reinterpret_cast<const char*>(pixels_), bytes);
        Expect(output.good(), "Could not save owned pixel-test BMP");
    }
private:
    int width_, height_;
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    std::uint32_t* pixels_ = nullptr;
};

void TestAntialiasing(const std::filesystem::path& artifacts)
{
    OwnedDib image(96, 64);
    SettingsPainter painter;
    const RECT bounds{0, 0, 96, 64}, shape{8, 8, 88, 56};
    const COLORREF white = RGB(255, 255, 255), blue = RGB(48, 111, 235);
    Expect(painter.Begin(image.Dc(), bounds), "SettingsPainter could not bind an owned DIB");
    painter.Clear(white); painter.Rounded(shape, 18, blue);
    Expect(painter.End(), "SettingsPainter did not finish owned DIB drawing");
    GdiFlush();
    Expect(image.Pixel(48, 32) == blue && image.Pixel(8, 8) == white,
        "Owned AA fixture did not contain its solid fill and rounded exterior");
    unsigned int mixed = 0;
    for (int y = 8; y < 28; ++y)
        for (int x = 8; x < 28; ++x)
        {
            const auto color = image.Pixel(x, y);
            if (GetRValue(color) > 50 && GetRValue(color) < 253 && GetGValue(color) > 113 &&
                GetGValue(color) < 253 && GetBValue(color) >= 235) ++mixed;
        }
    image.Save(artifacts / L"settings-aa-curve.bmp");
    Expect(mixed >= 4, "Rounded curve has no blended edge pixels; antialiasing was not observed");
    std::cout << "owned_dib_antialiasing_mixed_edge_pixels=" << mixed << '\n';
}

std::pair<unsigned int, unsigned int> EdgePixels(const OwnedDib& image, UINT dpi)
{
    const int band = std::max(4, MulDiv(8, static_cast<int>(dpi), 96));
    unsigned int blue = 0, dark = 0;
    for (int y = 0; y < image.Height(); ++y)
    {
        if (y >= band && y < image.Height() - band) continue; // Avoid text and the center icon.
        for (int x = 0; x < image.Width(); ++x)
        {
            const auto color = image.Pixel(x, y);
            const int red = GetRValue(color), green = GetGValue(color), valueBlue = GetBValue(color);
            if (valueBlue > 150 && valueBlue > red + 40 && green > red + 20 && red < 170) ++blue;
            if (red < 90 && green < 90 && valueBlue < 90) ++dark;
        }
    }
    return {blue, dark};
}

void TestFocusDrawing(SettingsWindow& settings, UINT dpi, const std::filesystem::path& artifacts)
{
    const HWND navigation = GetDlgItem(settings.Handle(), 100);
    Expect(navigation && IsChild(settings.Handle(), navigation), "Owned navigation focus target is missing");
    SetFocus(navigation);
    SendMessageW(settings.Handle(), WM_LBUTTONDOWN, 0, 0); // Own main-window cue message; no child press/capture.
    SetFocus(navigation); Paint(settings);
    Expect(GetFocus() == navigation && (SendMessageW(navigation, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS),
        "Mouse-cue simulation did not hide the focused navigation outline");
    RECT rectangle{}; Expect(GetClientRect(navigation, &rectangle) != FALSE, "Could not read navigation client size");
    const auto name = L"settings-focus-dpi-" + std::to_wstring(dpi);
    OwnedDib mouse(rectangle.right, rectangle.bottom); mouse.Print(settings.Handle(), navigation);
    mouse.Save(artifacts / (name + L"-mouse.bmp"));
    const auto mouseEdges = EdgePixels(mouse, dpi);
    Expect(mouseEdges.first == 0 && mouseEdges.second == 0, "Mouse-cue navigation retained a blue ring or classic black focus dots");

    MSG tab{}; tab.hwnd = navigation; tab.message = WM_KEYDOWN; tab.wParam = VK_TAB; tab.lParam = 1;
    Expect(settings.PreTranslate(tab), "Owned synthetic Tab was not handled as dialog navigation");
    Paint(settings);
    const HWND next = GetFocus();
    Expect(next && next != navigation && IsChild(settings.Handle(), next), "Owned synthetic Tab did not move native control focus");
    Expect(!(SendMessageW(next, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS), "Keyboard navigation left native focus cues hidden");
    SetFocus(navigation); Paint(settings);
    OwnedDib keyboard(rectangle.right, rectangle.bottom); keyboard.Print(settings.Handle(), navigation);
    keyboard.Save(artifacts / (name + L"-keyboard.bmp"));
    const auto keyboardEdges = EdgePixels(keyboard, dpi);
    Expect(keyboardEdges.first > 8 && keyboardEdges.second == 0, "Keyboard navigation did not draw the blue focus border or retained black focus dots");

    SendMessageW(settings.Handle(), WM_LBUTTONDOWN, 0, 0); SetFocus(navigation); Paint(settings);
    OwnedDib after(rectangle.right, rectangle.bottom); after.Print(settings.Handle(), navigation);
    after.Save(artifacts / (name + L"-mouse-after.bmp"));
    const auto afterEdges = EdgePixels(after, dpi);
    Expect((SendMessageW(navigation, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) && afterEdges.first == 0 && afterEdges.second == 0,
        "Returning to mouse cues did not remove the keyboard focus border");
    std::cout << "owned_focus_message_simulation_dpi=" << dpi << ", blue_edge_pixels_mouse=" << mouseEdges.first
        << ", keyboard=" << keyboardEdges.first << ", mouse_after=" << afterEdges.first << '\n';
}

LOGFONTW ControlFont(HWND control)
{
    const auto font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    LOGFONTW value{};
    Expect(font && GetObjectW(font, static_cast<int>(sizeof(value)), &value) == static_cast<int>(sizeof(value)),
        "Could not inspect the native control font");
    return value;
}

void TestMemoryEdits(SettingsWindow& settings, Configuration& configuration)
{
    const HWND content = FindWindowExW(settings.Handle(), nullptr, L"WinHalo.SettingsContent", nullptr);
    const HWND width = GetDlgItem(content, 1020), language = GetDlgItem(content, 1004);
    Expect(width && language && configuration.language == L"en-US", "Owned settings edit fixture is missing");
    const float original = configuration.layout.width;
    const auto englishFont = ControlFont(width);
    SendMessageW(width, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"196"));
    SendMessageW(content, WM_COMMAND, MAKEWPARAM(1020, EN_KILLFOCUS), reinterpret_cast<LPARAM>(width));
    Paint(settings);
    Expect(configuration.layout.width == 196, "Numeric commit did not reach the memory-only configuration callback");
    SendMessageW(language, CB_SETCURSEL, 1, 0);
    SendMessageW(content, WM_COMMAND, MAKEWPARAM(1004, CBN_SELCHANGE), reinterpret_cast<LPARAM>(language));
    Paint(settings);
    const auto chineseFont = ControlFont(width);
    Expect(configuration.language == L"zh-CN" && configuration.layout.width == 196 &&
        _wcsicmp(chineseFont.lfFaceName, englishFont.lfFaceName) != 0,
        "Chinese language selection did not update the callback/font while preserving the numeric value");
    SendMessageW(language, CB_SETCURSEL, 2, 0);
    SendMessageW(content, WM_COMMAND, MAKEWPARAM(1004, CBN_SELCHANGE), reinterpret_cast<LPARAM>(language));
    Paint(settings);
    const auto restoredFont = ControlFont(width);
    Expect(configuration.language == L"en-US" && configuration.layout.width == 196 &&
        _wcsicmp(restoredFont.lfFaceName, englishFont.lfFaceName) == 0,
        "English language selection did not restore its font and retain the edited value");
    const auto originalText = std::to_wstring(original);
    SendMessageW(width, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(originalText.c_str()));
    SendMessageW(content, WM_COMMAND, MAKEWPARAM(1020, EN_KILLFOCUS), reinterpret_cast<LPARAM>(width));
    Paint(settings);
    Expect(configuration.layout.width == original, "Memory-only numeric test did not restore its original value");
    std::cout << "owned_message_edits=numeric_commit_196,Chinese_font,English_font,value_preservation,restored\n";
}

struct Control
{
    HWND window = nullptr;
    int id = 0;
    RECT rectangle{};
    std::wstring type, text;
    LRESULT selection = -1;
};
struct Enumeration
{
    HWND content = nullptr;
    std::vector<Control> controls;
    bool failed = false;
};
BOOL CALLBACK CollectControl(HWND window, LPARAM parameter) noexcept
{
    auto& state = *reinterpret_cast<Enumeration*>(parameter);
    if (GetParent(window) != state.content) return TRUE;
    try
    {
        Control control;
        control.window = window; control.id = GetDlgCtrlID(window);
        Expect(GetWindowRect(window, &control.rectangle) != FALSE, "Could not read native control bounds");
        wchar_t type[128]{}, text[1024]{};
        GetClassNameW(window, type, 128); GetWindowTextW(window, text, 1024);
        control.type = type; control.text = text;
        if (_wcsicmp(type, L"COMBOBOX") == 0)
        {
            Expect(SendMessageW(window, CB_GETDROPPEDSTATE, 0, 0) == FALSE,
                "A combo unexpectedly opened during layout");
            // GetWindowRect measures the closed selection control. A rectangle
            // from CB_GETDROPPEDCONTROLRECT includes the popup and is unsuitable
            // for testing row overlap.
            control.selection = SendMessageW(window, CB_GETCURSEL, 0, 0);
        }
        MapWindowPoints(HWND_DESKTOP, state.content, reinterpret_cast<POINT*>(&control.rectangle), 2);
        state.controls.push_back(std::move(control));
        return TRUE;
    }
    catch (...) { state.failed = true; return FALSE; }
}
std::vector<Control> Controls(HWND content)
{
    Enumeration result; result.content = content;
    EnumChildWindows(content, CollectControl, reinterpret_cast<LPARAM>(&result));
    Expect(!result.failed && result.controls.size() >= 30, "Native settings controls disappeared");
    std::sort(result.controls.begin(), result.controls.end(), [](const Control& a, const Control& b) { return a.id < b.id; });
    for (size_t index = 1; index < result.controls.size(); ++index)
        Expect(result.controls[index - 1].id != result.controls[index].id, "Duplicate native control IDs");
    return result.controls;
}
void SameContent(const std::vector<Control>& expected, const std::vector<Control>& actual)
{
    Expect(expected.size() == actual.size(), "Scrolling lost native controls");
    for (size_t index = 0; index < expected.size(); ++index)
    {
        const auto& before = expected[index]; const auto& after = actual[index];
        Expect(before.id == after.id && before.type == after.type && before.text == after.text &&
            before.selection == after.selection, "Scrolling changed a native control's identity or contents");
        Expect(before.window == after.window, "Scrolling unnecessarily recreated native controls");
    }
}
std::vector<int> VerifyLayout(HWND content, const std::vector<Control>& controls, UINT dpi)
{
    RECT client{}; Expect(GetClientRect(content, &client) != FALSE, "Cannot read content viewport");
    const LONG tolerance = std::max<LONG>(1, MulDiv(2, static_cast<int>(dpi), 96));
    std::map<int, std::vector<const Control*>> rows;
    for (const auto& control : controls)
    {
        const auto& rect = control.rectangle;
        Expect(IsWindow(control.window) && rect.right > rect.left && rect.bottom > rect.top,
            "A native settings control has invalid bounds");
        Expect(rect.left >= -tolerance && rect.right <= client.right + tolerance,
            "A native control exceeds the horizontal content viewport");
        if (_wcsicmp(control.type.c_str(), L"EDIT") == 0)
            Expect(!control.text.empty(), "A numeric edit lost its displayed value");
        const int row = control.id >= 1000 ? (control.id - 1000) / 4 : control.id + 10000;
        rows[row].push_back(&control);
    }
    // Include offscreen rows: scrolling translates them without changing layout.
    for (size_t first = 0; first < controls.size(); ++first)
        for (size_t second = first + 1; second < controls.size(); ++second)
        {
            RECT overlap{};
            if (IntersectRect(&overlap, &controls[first].rectangle, &controls[second].rectangle))
            {
                std::cerr << "Overlapping controls: " << controls[first].id << " and " << controls[second].id
                    << " at simulated DPI " << dpi << '\n';
                throw std::runtime_error("Native settings controls unexpectedly overlap");
            }
        }
    std::vector<std::pair<LONG, int>> order;
    for (auto& [id, row] : rows)
    {
        std::sort(row.begin(), row.end(), [](const Control* a, const Control* b)
            { return a->rectangle.left < b->rectangle.left; });
        LONG lowest = (std::numeric_limits<LONG>::max)();
        LONG highest = (std::numeric_limits<LONG>::min)();
        LONG top = (std::numeric_limits<LONG>::max)();
        for (const auto* control : row)
        {
            const LONG center = (control->rectangle.top + control->rectangle.bottom) / 2;
            lowest = std::min(lowest, center); highest = std::max(highest, center);
            top = std::min(top, control->rectangle.top);
        }
        Expect(highest - lowest <= tolerance, "Controls from one row no longer align vertically");
        if (row.size() == 3 && id < 10000)
            Expect((row[0]->id - 1000) % 4 == 1 && (row[1]->id - 1000) % 4 == 0 &&
                (row[2]->id - 1000) % 4 == 2, "A numeric row swapped minus, edit or plus controls");
        order.emplace_back(top, id);
    }
    std::sort(order.begin(), order.end());
    std::vector<int> result;
    for (const auto& entry : order) result.push_back(entry.second);
    return result;
}

void Capture(HWND window, const std::filesystem::path& path)
{
    Expect(SUCCEEDED(DwmFlush()), "Desktop compositor did not flush before capture");
    RECT rectangle{}; Expect(GetWindowRect(window, &rectangle) != FALSE, "Could not read capture bounds");
    const LONG width = rectangle.right - rectangle.left, height = rectangle.bottom - rectangle.top;
    struct Surface
    {
        HDC screen = GetDC(nullptr), memory = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ previous = nullptr;
        ~Surface()
        {
            if (previous && previous != HGDI_ERROR) SelectObject(memory, previous);
            if (bitmap) DeleteObject(bitmap);
            if (memory) DeleteDC(memory);
            if (screen) ReleaseDC(nullptr, screen);
        }
    } surface;
    Expect(surface.screen != nullptr, "Could not acquire screen for capture");
    surface.memory = CreateCompatibleDC(surface.screen);
    Expect(surface.memory != nullptr, "Could not create capture DC");
    BITMAPINFO information{};
    information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    information.bmiHeader.biWidth = width; information.bmiHeader.biHeight = -height;
    information.bmiHeader.biPlanes = 1; information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    surface.bitmap = CreateDIBSection(surface.screen, &information, DIB_RGB_COLORS, &pixels, nullptr, 0);
    Expect(surface.bitmap != nullptr && pixels, "Could not allocate capture pixels");
    surface.previous = SelectObject(surface.memory, surface.bitmap);
    Expect(surface.previous != nullptr && surface.previous != HGDI_ERROR, "Could not select capture bitmap");
    // Render the owned window into our DC even when a user's application is
    // in front. Screen BitBlt would silently capture that other application.
    Expect(PrintWindow(window, surface.memory, PW_RENDERFULLCONTENT) != FALSE,
        "Owned settings PrintWindow capture failed");
    GdiFlush();
    const DWORD bytes = static_cast<DWORD>(width) * static_cast<DWORD>(height) * 4;
    BITMAPFILEHEADER header{}; header.bfType = 0x4D42;
    header.bfOffBits = static_cast<DWORD>(sizeof(header) + sizeof(BITMAPINFOHEADER));
    header.bfSize = header.bfOffBits + bytes;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&information.bmiHeader), sizeof(BITMAPINFOHEADER));
    output.write(static_cast<const char*>(pixels), bytes);
    Expect(output.good(), "Could not save settings capture BMP");
}
RECT SuggestedRectangle(HWND window, UINT dpi)
{
    MONITORINFO monitor{sizeof(monitor)};
    Expect(GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE,
        "Could not locate settings test work area");
    const auto& work = monitor.rcWork;
    const LONG width = std::min<LONG>(MulDiv(900, static_cast<int>(dpi), 96), work.right - work.left - 24);
    const LONG height = std::min<LONG>(MulDiv(740, static_cast<int>(dpi), 96), work.bottom - work.top - 24);
    const LONG left = work.left + (work.right - work.left - width) / 2;
    const LONG top = work.top + (work.bottom - work.top - height) / 2;
    return {left, top, left + width, top + height};
}
}

int main()
{
    RestoreForeground foreground;
    try
    {
        if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
            Expect(GetLastError() == ERROR_ACCESS_DENIED, "Could not enable per-monitor DPI awareness");
        Apartment apartment;
        const auto artifacts = std::filesystem::current_path() / L"artifacts";
        std::filesystem::create_directories(artifacts);
        TestAntialiasing(artifacts);
        Configuration configuration;
        configuration.language = L"en-US"; configuration.reducedMotion = true;
        SettingsStatus status;
        unsigned int changes = 0, startupRequests = 0;
        SettingsWindow settings(GetModuleHandleW(nullptr));
        std::vector<int> rowOrder;
        size_t expectedControlCount = 0;
        DWORD warmedGdi = 0;
        for (unsigned int lifecycle = 0; lifecycle < 2; ++lifecycle)
        {
            settings.Show(configuration, status,
                [&](const Configuration& updated) { configuration = updated; ++changes; },
                [&](bool) { ++startupRequests; });
            Paint(settings);
            if (lifecycle == 0) TestMemoryEdits(settings, configuration);
            for (UINT dpi : std::array<UINT, 4>{96, 120, 144, 192})
            {
                RECT suggested = SuggestedRectangle(settings.Handle(), dpi);
                SendMessageW(settings.Handle(), WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
                Paint(settings);
                HWND content = FindWindowExW(settings.Handle(), nullptr, L"WinHalo.SettingsContent", nullptr);
                Expect(content && GetParent(content) == settings.Handle(), "Settings content window is missing");
                SendMessageW(content, WM_VSCROLL, SB_TOP, 0); Paint(settings);
                const auto initial = Controls(content);
                if (!expectedControlCount) expectedControlCount = initial.size();
                Expect(initial.size() == expectedControlCount, "DPI/lifecycle transition lost native controls");
                const auto currentOrder = VerifyLayout(content, initial, dpi);
                if (rowOrder.empty()) rowOrder = currentOrder;
                Expect(currentOrder == rowOrder, "Settings rows changed order at another DPI");
                if (lifecycle == 0)
                {
                    TestFocusDrawing(settings, dpi, artifacts);
                    SendMessageW(content, WM_VSCROLL, SB_TOP, 0); Paint(settings);
                    Capture(settings.Handle(), artifacts / (L"settings-render-dpi-" + std::to_wstring(dpi) + L"-top.bmp"));
                    // Four DPIs x fifty top/bottom pairs = two hundred pairs.
                    for (int cycle = 0; cycle < 50; ++cycle)
                    {
                        SendMessageW(content, WM_VSCROLL, SB_BOTTOM, 0); Paint(settings);
                        const auto bottom = Controls(content);
                        SameContent(initial, bottom); VerifyLayout(content, bottom, dpi);
                        SendMessageW(content, WM_VSCROLL, SB_TOP, 0); Paint(settings);
                        const auto top = Controls(content);
                        SameContent(initial, top); VerifyLayout(content, top, dpi);
                    }
                    SendMessageW(content, WM_VSCROLL, SB_BOTTOM, 0); Paint(settings);
                    Capture(settings.Handle(), artifacts / (L"settings-render-dpi-" + std::to_wstring(dpi) + L"-bottom.bmp"));
                }
                std::cout << "simulated_dpi=" << dpi << ", lifecycle=" << lifecycle + 1
                    << ", native_controls=" << initial.size() << '\n';
            }
            const HWND previous = settings.Handle();
            SendMessageW(previous, WM_CLOSE, 0, 0); Pump(settings);
            Expect(!IsWindow(previous) && !settings.Handle(), "WM_CLOSE did not destroy only settings");
            const DWORD gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            if (lifecycle == 0) warmedGdi = gdi;
            else Expect(gdi <= warmedGdi + 4, "Repeated create/close retained excessive GDI resources");
        }
        Expect(startupRequests == 0, "Layout test unexpectedly requested a startup-registry change");
        std::cout << "PASS: owned-DIB rounded-curve antialiasing and mouse/Tab focus-cue message simulation; "
            "memory-only numeric and language/font changes; same-process WM_DPICHANGED simulation at 100/125/150/200 percent; "
            "native row order and non-overlap; 200 top/bottom scroll pairs preserve controls; "
            "two create/close lifecycles without WM_QUIT; memory-only configuration callbacks=" << changes
            << "; no system DPI, external pointer/keyboard input, clipboard or startup changes; "
            "not a physical multi-monitor DPI validation\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
