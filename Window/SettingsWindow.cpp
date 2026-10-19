#include "SettingsWindow.h"
#include "SettingsPainter.h"
#include "../Resource.h"
#include "../Utils/Win32Error.h"
#include <commctrl.h>
#include <oleacc.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Oleacc.lib")

namespace winisland
{
namespace
{
constexpr wchar_t SettingsClass[] = L"WinHalo.SettingsWindow";
constexpr wchar_t ContentClass[] = L"WinHalo.SettingsContent";
constexpr COLORREF Background = RGB(245, 246, 248);
constexpr COLORREF Sidebar = RGB(249, 250, 252);
constexpr COLORREF Ink = RGB(29, 36, 49);
constexpr COLORREF Muted = RGB(117, 126, 141);
constexpr COLORREF Accent = RGB(48, 111, 235);
constexpr COLORREF Border = RGB(232, 235, 240);
constexpr int FirstControl = 1000;
constexpr int FirstNavigation = 100;
constexpr int ResetControl = 110;
constexpr UINT ApplyScrollMessage = WM_APP + 61;
// oleacc.h declares these property IDs without defining storage in Oleacc.lib.
// Keep the two SDK values local instead of enabling INITGUID for every header.
constexpr MSAAPROPID AccessibleName{0x608d3df8, 0x8128, 0x4aa7, {0xa4, 0x28, 0xf5, 0x5e, 0x49, 0x26, 0x72, 0x91}};
constexpr MSAAPROPID AccessibleDescription{0x4d48dfe4, 0xbd3f, 0x491f, {0xa6, 0x48, 0x49, 0x2d, 0x6f, 0x20, 0xc5, 0x88}};

enum class Field
{
    Startup, Language, Monitor,
    CompactScale, ExpandedScale, Width, Height, ExpandedWidth, ExpandedHeight,
    OffsetX, OffsetY, FontSize, LineWidth,
    AutoHideIdle, AutoHideFullscreen, RightDrag, Notifications, SystemControls, ClipboardLinks, Privacy,
    Glass, ReducedMotion, SpringSpeed
};
enum class Kind { Toggle, Number, Language, Monitor };

struct Row
{
    Field field;
    Kind kind;
    const wchar_t* chinese;
    const wchar_t* english;
    const wchar_t* hintChinese;
    const wchar_t* hintEnglish;
    int y = 0;
    int height = 68;
    HWND control = nullptr, minus = nullptr, plus = nullptr;
};
struct Card
{
    int top = 0, bottom = 0;
    size_t first = 0, count = 0;
};
struct Display
{
    std::wstring device;
    RECT rectangle{};
    bool primary = false;
};
int ControlId(Field field, int part = 0) { return FirstControl + static_cast<int>(field) * 4 + part; }

struct BackBuffer
{
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    int width = 0, height = 0;
    ~BackBuffer() { Reset(); }
    void Reset() noexcept
    {
        if (dc && previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        dc = nullptr; bitmap = nullptr; previous = nullptr; width = height = 0;
    }
    HDC Acquire(HDC target, int requestedWidth, int requestedHeight)
    {
        if (dc && width == requestedWidth && height == requestedHeight) return dc;
        Reset();
        dc = CreateCompatibleDC(target);
        if (!dc) return nullptr;
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = std::max(1, requestedWidth);
        info.bmiHeader.biHeight = -std::max(1, requestedHeight);
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        bitmap = CreateDIBSection(target, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap) { Reset(); return nullptr; }
        previous = SelectObject(dc, bitmap);
        width = requestedWidth; height = requestedHeight;
        return dc;
    }
};

struct Surface
{
    HDC destination = nullptr, dc = nullptr;
    RECT bounds{};
    int saved = 0;
    Surface(HDC target, RECT client, BackBuffer& buffer) : destination(target), bounds(client)
    {
        dc = buffer.Acquire(target, client.right - client.left, client.bottom - client.top);
        if (!dc) dc = target;
        saved = SaveDC(dc);
        if (dc != target) SetViewportOrgEx(dc, -client.left, -client.top, nullptr);
        IntersectClipRect(dc, client.left, client.top, client.right, client.bottom);
    }
    ~Surface()
    {
        if (dc != destination)
            BitBlt(destination, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                dc, bounds.left, bounds.top, SRCCOPY);
        if (saved) RestoreDC(dc, saved);
    }
};

struct Placement { HWND handle; int x, y, width, height; };

void PositionBatch(const std::vector<Placement>& positions)
{
    constexpr UINT flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOREDRAW | SWP_NOCOPYBITS;
    HDWP batch = BeginDeferWindowPos(static_cast<int>(positions.size()));
    if (batch)
    {
        for (const auto& position : positions)
        {
            batch = DeferWindowPos(batch, position.handle, nullptr, position.x, position.y,
                position.width, position.height, flags);
            if (!batch) break;
        }
        if (batch && EndDeferWindowPos(batch)) return;
    }
    // Allocation failure must still leave a complete layout before repainting.
    for (const auto& position : positions)
        CheckWin32(SetWindowPos(position.handle, nullptr, position.x, position.y,
            position.width, position.height, flags), "Position settings control");
}
}

struct SettingsWindow::Impl
{
    explicit Impl(HINSTANCE module) : instance(module)
    {
        backgroundBrush = CreateSolidBrush(Background);
        cardBrush = CreateSolidBrush(RGB(255, 255, 255));
        editBrush = CreateSolidBrush(RGB(247, 249, 252));
        linePen = CreatePen(PS_SOLID, 1, Border);
        CoCreateInstance(__uuidof(CAccPropServices), nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IAccPropServices), reinterpret_cast<void**>(&accessibility));
    }
    ~Impl()
    {
        change = {}; startup = {};
        Close();
        if (accessibility) accessibility->Release();
        DeleteFonts();
        if (backgroundBrush) DeleteObject(backgroundBrush);
        if (cardBrush) DeleteObject(cardBrush);
        if (editBrush) DeleteObject(editBrush);
        if (linePen) DeleteObject(linePen);
        if (registeredMain) UnregisterClassW(SettingsClass, instance);
        if (registeredContent) UnregisterClassW(ContentClass, instance);
    }
    HINSTANCE instance = nullptr;
    HWND window = nullptr, content = nullptr;
    std::array<HWND, 3> navigation{};
    HWND resetButton = nullptr;
    Configuration configuration;
    SettingsStatus status;
    ChangeHandler change;
    StartupHandler startup;
    std::vector<Row> rows;
    std::vector<Card> cards;
    std::vector<Display> displays;
    std::array<int, 3> sections{};
    std::array<HFONT, 6> fonts{};
    HBRUSH backgroundBrush = nullptr, cardBrush = nullptr, editBrush = nullptr;
    HPEN linePen = nullptr;
    IAccPropServices* accessibility = nullptr;
    UINT dpi = 96;
    int scroll = 0, totalHeight = 0, contentWidth = 0, contentHeight = 0;
    int selectedSection = 0;
    bool english = false, syncing = false, closing = false;
    bool fontEnglish = false;
    bool registeredMain = false, registeredContent = false;
    bool displaysDirty = true, languageItemsDirty = true;
    bool layingOut = false, changingDpi = false, geometryDirty = true;
    bool keyboardNavigation = false, scrollPosted = false, scrollPending = false;
    int requestedScroll = 0, laidOutWidth = -1, laidOutHeight = -1, laidOutScroll = -1;
    UINT laidOutDpi = 0;
    double wheelFraction = 0;
    HWND hoveredControl = nullptr;
    BackBuffer mainBuffer, contentBuffer, buttonBuffer, toggleBuffer;
    SettingsPainter mainPainter, contentPainter, buttonPainter, togglePainter;
    SettingsPainter* shapePainter = nullptr;
    HDC shapeDC = nullptr;
    std::vector<Placement> outerPositions, contentPositions;
    std::wstring callbackError;

    int P(float value) const noexcept { return static_cast<int>(std::lround(value * static_cast<float>(dpi) / 96.0f)); }
    int P(int value) const noexcept { return MulDiv(value, static_cast<int>(dpi), 96); }
    int D(int value) const noexcept { return MulDiv(value, 96, static_cast<int>(dpi)); }
    RECT R(int left, int top, int right, int bottom) const noexcept { return {P(static_cast<float>(left)), P(static_cast<float>(top)), P(static_cast<float>(right)), P(static_cast<float>(bottom))}; }
    const wchar_t* T(const wchar_t* chinese, const wchar_t* other) const noexcept { return english ? other : chinese; }
    const wchar_t* Label(const Row& row) const noexcept { return T(row.chinese, row.english); }
    HFONT Font(size_t index) const noexcept { return fonts[index] ? fonts[index] : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)); }

    void KeyboardCues(bool visible)
    {
        if (keyboardNavigation == visible && window &&
            (((SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0) == visible)) return;
        keyboardNavigation = visible;
        if (window) SendMessageW(window, WM_CHANGEUISTATE, MAKEWPARAM(visible ? UIS_CLEAR : UIS_SET, UISF_HIDEFOCUS), 0);
        for (HWND button : navigation) if (button) InvalidateRect(button, nullptr, FALSE);
        for (const auto& row : rows)
            if (row.kind == Kind::Toggle || row.kind == Kind::Number)
                for (HWND control : {row.control, row.minus, row.plus})
                    if (control) InvalidateRect(control, nullptr, FALSE);
        if (resetButton) InvalidateRect(resetButton, nullptr, FALSE);
    }

    bool KeyboardFocus(HWND control) const
    {
        return keyboardNavigation && GetFocus() == control &&
            !(SendMessageW(control, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
    }

    void DeleteFonts()
    {
        for (auto& font : fonts) { if (font) DeleteObject(font); font = nullptr; }
    }
    void MakeFonts()
    {
        DeleteFonts();
        fontEnglish = english;
        laidOutWidth = -1;
        const std::array<int, 6> sizes{13, 11, 15, 24, 20, 13};
        for (size_t index = 0; index < fonts.size(); ++index)
            fonts[index] = CreateFontW(-P(static_cast<float>(sizes[index])), 0, 0, 0,
                index >= 2 && index <= 4 ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, english ? L"Segoe UI" : L"Microsoft YaHei UI");
        for (const auto& row : rows)
            for (HWND control : {row.control, row.minus, row.plus})
                if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(Font(0)), TRUE);
        for (HWND button : navigation) if (button) SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(Font(0)), TRUE);
        if (resetButton) SendMessageW(resetButton, WM_SETFONT, reinterpret_cast<WPARAM>(Font(0)), TRUE);
    }

    static LRESULT CALLBACK MainProc(HWND handle, UINT message, WPARAM wp, LPARAM lp) noexcept
    {
        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window = handle;
            SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        try { return self ? self->MainMessage(handle, message, wp, lp) : DefWindowProcW(handle, message, wp, lp); }
        catch (...) { return message == WM_CREATE ? -1 : 0; }
    }
    static LRESULT CALLBACK ContentProc(HWND handle, UINT message, WPARAM wp, LPARAM lp) noexcept
    {
        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->content = handle;
            SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        try { return self ? self->ContentMessage(handle, message, wp, lp) : DefWindowProcW(handle, message, wp, lp); }
        catch (...) { return message == WM_CREATE ? -1 : 0; }
    }
    static LRESULT CALLBACK ControlProc(HWND handle, UINT message, WPARAM wp, LPARAM lp,
        UINT_PTR identifier, DWORD_PTR data) noexcept
    {
        auto* self = reinterpret_cast<Impl*>(data);
        try
        {
            const int id = GetDlgCtrlID(handle);
            Row* row = self->FindRow(id);
            if (message == WM_NCDESTROY)
            {
                if (self->hoveredControl == handle) self->hoveredControl = nullptr;
                RemoveWindowSubclass(handle, ControlProc, identifier);
            }
            if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN) self->KeyboardCues(false);
            if (message == WM_MOUSEMOVE && self->hoveredControl != handle)
            {
                if (self->hoveredControl) InvalidateRect(self->hoveredControl, nullptr, FALSE);
                self->hoveredControl = handle;
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, handle, 0}; TrackMouseEvent(&tracking);
                InvalidateRect(handle, nullptr, FALSE);
            }
            if (message == WM_MOUSELEAVE && self->hoveredControl == handle)
            {
                self->hoveredControl = nullptr; InvalidateRect(handle, nullptr, FALSE);
            }
            if (message == WM_UPDATEUISTATE)
            {
                const LRESULT result = DefSubclassProc(handle, message, wp, lp);
                InvalidateRect(handle, nullptr, FALSE);
                return result;
            }
            if (message == WM_SETFOCUS)
            {
                if (row) self->EnsureRowVisible(*row);
                else if (id == ResetControl) self->ScrollTo(self->totalHeight - self->contentHeight);
                InvalidateRect(handle, nullptr, FALSE);
            }
            if (message == WM_KILLFOCUS || message == WM_ENABLE) InvalidateRect(handle, nullptr, FALSE);
            if (message == WM_MOUSEWHEEL && (!row || (row->kind != Kind::Monitor && row->kind != Kind::Language) ||
                !SendMessageW(handle, CB_GETDROPPEDSTATE, 0, 0)))
                return SendMessageW(self->content, message, wp, lp);
            if (row && row->kind == Kind::Number && handle == row->control && message == WM_KEYDOWN)
            {
                if (wp == VK_RETURN) { self->CommitNumber(*row); SendMessageW(handle, EM_SETSEL, 0, -1); return 0; }
                if (wp == VK_UP || wp == VK_DOWN) { self->Bump(*row, wp == VK_UP ? 1 : -1); return 0; }
            }
            if (row && row->kind == Kind::Toggle && message == WM_ERASEBKGND) return 1;
            if (row && row->kind == Kind::Toggle && (message == WM_PAINT || message == WM_PRINTCLIENT))
            {
                PAINTSTRUCT paint{};
                HDC dc = message == WM_PAINT ? BeginPaint(handle, &paint) : reinterpret_cast<HDC>(wp);
                self->PaintToggle(handle, dc);
                if (message == WM_PAINT) EndPaint(handle, &paint);
                return 0;
            }
        }
        catch (...) { return 0; }
        return DefSubclassProc(handle, message, wp, lp);
    }

    void RegisterClasses()
    {
        if (registeredMain && registeredContent) return;
        WNDCLASSEXW descriptor{};
        descriptor.cbSize = sizeof(descriptor);
        descriptor.hInstance = instance;
        descriptor.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        descriptor.lpfnWndProc = MainProc;
        descriptor.lpszClassName = SettingsClass;
        descriptor.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_WINHALO));
        descriptor.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SMALL));
        const bool mainCreated = RegisterClassExW(&descriptor) != 0;
        registeredMain = registeredMain || mainCreated;
        if (!mainCreated && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) ThrowLastError("Register settings window");
        descriptor.lpfnWndProc = ContentProc;
        descriptor.lpszClassName = ContentClass;
        descriptor.hIcon = descriptor.hIconSm = nullptr;
        const bool contentCreated = RegisterClassExW(&descriptor) != 0;
        registeredContent = registeredContent || contentCreated;
        if (!contentCreated && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) ThrowLastError("Register settings content");
    }
    void Show(const Configuration& value, const SettingsStatus& current, ChangeHandler changed, StartupHandler started)
    {
        change = std::move(changed); startup = std::move(started);
        Refresh(value, current);
        if (!window)
        {
            closing = false;
            RegisterClasses();
            INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES};
            InitCommonControlsEx(&common);
            POINT cursor{}; GetCursorPos(&cursor);
            const HMONITOR targetMonitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO monitor{sizeof(monitor)};
            CheckWin32(GetMonitorInfoW(targetMonitor, &monitor), "Get settings work area");
            dpi = GetDpiForSystem();
            if (HMODULE library = LoadLibraryExW(L"Shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            {
                using MonitorDpi = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
                const auto readDpi = reinterpret_cast<MonitorDpi>(GetProcAddress(library, "GetDpiForMonitor"));
                UINT horizontal = dpi, vertical = dpi;
                if (readDpi && SUCCEEDED(readDpi(targetMonitor, 0, &horizontal, &vertical))) dpi = horizontal;
                FreeLibrary(library);
            }
            const int desiredWidth = std::min(P(900), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left) - P(32));
            const int desiredHeight = std::min(P(740), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top) - P(32));
            const int x = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - desiredWidth) / 2;
            const int y = monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - desiredHeight) / 2;
            window = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, SettingsClass,
                T(L"WinHalo 设置", L"WinHalo Settings"), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                x, y, desiredWidth, desiredHeight, nullptr, nullptr, instance, this);
            if (!window) ThrowLastError("Create settings window");
            dpi = GetDpiForWindow(window);
            MakeFonts();
            UpdateControls(false);
            Layout();
        }
        ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(window);
    }
    void Refresh(const Configuration& value, const SettingsStatus& current)
    {
        const auto values = [](const Configuration& item)
        {
            return std::tie(item.layout.width, item.layout.height, item.layout.expandedWidth, item.layout.expandedHeight,
                item.layout.compactScale, item.layout.expandedScale, item.layout.offsetX, item.layout.offsetY,
                item.layout.fontSize, item.layout.lineWidth, item.spring.speed, item.reducedMotion, item.autoHideFullscreen,
                item.nativeGlass, item.autoHideIdle, item.rightDrag, item.showNotifications, item.systemControls,
                item.clipboardLinks, item.privacyIndicators, item.monitorDevice, item.language);
        };
        Configuration normalized = value;
        NormalizeConfiguration(normalized);
        const bool configurationChanged = values(normalized) != values(configuration);
        const bool statusChanged = std::tie(current.startupEnabled, current.startupAvailable, current.brightnessAvailable,
            current.privacyAvailable, current.startupError, current.brightnessStatus, current.privacyStatus, current.saveError) !=
            std::tie(status.startupEnabled, status.startupAvailable, status.brightnessAvailable,
                status.privacyAvailable, status.startupError, status.brightnessStatus, status.privacyStatus, status.saveError);
        if (configuration.monitorDevice != normalized.monitorDevice) displaysDirty = true;
        configuration = std::move(normalized);
        status = current;
        const bool nextEnglish = configuration.language == L"en-US" ||
            (configuration.language == L"system" && PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_CHINESE);
        const bool languageChanged = nextEnglish != english;
        english = nextEnglish;
        if (languageChanged) displaysDirty = languageItemsDirty = true;
        if (window && (configurationChanged || statusChanged || languageChanged))
        {
            if (languageChanged) callbackError.clear();
            UpdateControls(false);
            InvalidateRect(window, nullptr, FALSE);
            InvalidateRect(content, nullptr, FALSE);
        }
    }
    void Close()
    {
        if (!closing && window)
        {
            Row* row = FindRow(GetDlgCtrlID(GetFocus()));
            if (row && row->kind == Kind::Number && GetFocus() == row->control) CommitNumber(*row);
            if (accessibility)
            {
                const MSAAPROPID properties[] = {AccessibleName, AccessibleDescription};
                for (const auto& item : rows)
                    for (HWND control : {item.control, item.minus, item.plus})
                        if (control) accessibility->ClearHwndProps(control, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, properties, 2);
            }
        }
        closing = true;
        if (window) DestroyWindow(window);
        closing = false;
    }

    HWND AddControl(const wchar_t* type, const wchar_t* caption, DWORD style, int id)
    {
        HWND control = CreateWindowExW(0, type, caption, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | style,
            0, 0, 10, 10, content, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        if (!control) ThrowLastError("Create settings control");
        SetWindowSubclass(control, ControlProc, 1, reinterpret_cast<DWORD_PTR>(this));
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(Font(0)), FALSE);
        return control;
    }
    void AddRow(Field field, Kind kind, const wchar_t* chinese, const wchar_t* other,
        const wchar_t* hintChinese, const wchar_t* hintOther, int height = 68)
    {
        Row row{field, kind, chinese, other, hintChinese, hintOther};
        row.height = height;
        if (kind == Kind::Toggle)
            row.control = AddControl(L"BUTTON", L"", BS_AUTOCHECKBOX | BS_NOTIFY, ControlId(field));
        else if (kind == Kind::Number)
        {
            row.minus = AddControl(L"BUTTON", L"", BS_OWNERDRAW | BS_NOTIFY, ControlId(field, 1));
            row.control = AddControl(L"EDIT", L"", ES_RIGHT | ES_AUTOHSCROLL, ControlId(field));
            row.plus = AddControl(L"BUTTON", L"", BS_OWNERDRAW | BS_NOTIFY, ControlId(field, 2));
            SendMessageW(row.control, EM_SETLIMITTEXT, 24, 0);
        }
        else
            row.control = AddControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, ControlId(field));
        rows.push_back(row);
    }
    void BeginCard() { cards.push_back({0, 0, rows.size(), 0}); }
    void EndCard() { cards.back().count = rows.size() - cards.back().first; }
    void CreateRows()
    {
        geometryDirty = true;
        rows.reserve(24); cards.reserve(8);
        BeginCard();
        AddRow(Field::Startup, Kind::Toggle, L"开机启动", L"Launch at startup", L"登录 Windows 后自动运行 WinHalo", L"Start WinHalo when you sign in to Windows", 76);
        AddRow(Field::Language, Kind::Language, L"界面语言", L"Language", L"立即应用，无需重新启动", L"Applies immediately, no restart needed");
        AddRow(Field::Monitor, Kind::Monitor, L"显示器", L"Display", L"选择灵动岛显示的屏幕", L"Choose the screen for your island", 76);
        EndCard();
        BeginCard();
        AddRow(Field::CompactScale, Kind::Number, L"胶囊缩放", L"Capsule scale", L"整体缩放 · 0.5–2.0 倍", L"Overall scale · 0.5–2.0×");
        AddRow(Field::ExpandedScale, Kind::Number, L"展开缩放", L"Expanded scale", L"整体缩放 · 0.5–2.0 倍", L"Overall scale · 0.5–2.0×");
        AddRow(Field::Width, Kind::Number, L"胶囊宽度", L"Capsule width", L"基础宽度 · 120–400 DIP", L"Base width · 120–400 DIP");
        AddRow(Field::Height, Kind::Number, L"胶囊高度", L"Capsule height", L"基础高度 · 32–88 DIP", L"Base height · 32–88 DIP");
        AddRow(Field::ExpandedWidth, Kind::Number, L"展开宽度", L"Expanded width", L"基础宽度 · 320–900 DIP", L"Base width · 320–900 DIP");
        AddRow(Field::ExpandedHeight, Kind::Number, L"展开高度", L"Expanded height", L"基础高度 · 200–600 DIP", L"Base height · 200–600 DIP");
        EndCard();
        BeginCard();
        AddRow(Field::OffsetX, Kind::Number, L"水平偏移", L"Horizontal offset", L"相对顶部中心 · 左负右正", L"From top center · negative moves left");
        AddRow(Field::OffsetY, Kind::Number, L"垂直偏移", L"Vertical offset", L"相对默认位置 · 上负下正", L"From default position · negative moves up");
        AddRow(Field::FontSize, Kind::Number, L"字体大小", L"Font size", L"0 为自动；手动范围 10–28 DIP", L"0 = automatic; manual size 10–28 DIP");
        AddRow(Field::LineWidth, Kind::Number, L"待机横线长度", L"Resting line width", L"保持圆角与轻盈比例 · 24–240 DIP", L"Rounded, subtle and centered · 24–240 DIP");
        EndCard();
        BeginCard();
        AddRow(Field::Glass, Kind::Toggle, L"桌面模糊", L"Desktop blur", L"使用原生玻璃，不支持时自动回退", L"Native glass with automatic safe fallback");
        AddRow(Field::ReducedMotion, Kind::Toggle, L"减少动态效果", L"Reduce motion", L"立即切换形态，停止装饰动画", L"Instant transitions and no decorative animation");
        AddRow(Field::SpringSpeed, Kind::Number, L"动画速度", L"Animation speed", L"弹簧形变速度 · 0.2–3.0 倍", L"Spring transition speed · 0.2–3.0×");
        EndCard();
        BeginCard();
        AddRow(Field::AutoHideIdle, Kind::Toggle, L"空闲时收成横线", L"Collapse to line when idle", L"鼠标离开胶囊 5 秒后收起", L"Rest as a line after 5 seconds without pointer contact");
        AddRow(Field::AutoHideFullscreen, Kind::Toggle, L"全屏时自动隐藏", L"Hide in fullscreen", L"前台应用全屏时暂时隐藏灵动岛", L"Temporarily hide while the active app is fullscreen");
        AddRow(Field::RightDrag, Kind::Toggle, L"右键拖动位置", L"Drag with right mouse button", L"按住右键 400 ms 后拖动；短按打开设置", L"Hold the right button for 400 ms to drag; short click opens settings");
        EndCard();
        BeginCard();
        AddRow(Field::Notifications, Kind::Toggle, L"状态通知", L"Status notifications", L"显示 WinHalo 的音量、电量和设备状态通知", L"Show WinHalo volume, battery and device status notices");
        AddRow(Field::SystemControls, Kind::Toggle, L"系统控制", L"System controls", L"胶囊上滚轮调音量；Shift+滚轮调亮度", L"Wheel over the capsule for volume; Shift+wheel for brightness", 100);
        AddRow(Field::ClipboardLinks, Kind::Toggle, L"剪贴板链接", L"Clipboard links", L"复制网页链接时显示打开入口", L"Offer an open action after copying a web link");
        AddRow(Field::Privacy, Kind::Toggle, L"隐私活动提示", L"Privacy activity indicators", L"显示系统提供的摄像头和麦克风活动", L"Show camera and microphone activity reported by Windows", 88);
        EndCard();
        resetButton = AddControl(L"BUTTON", L"", BS_OWNERDRAW | BS_NOTIFY, ResetControl);
        UpdateControls(true);
    }

    Row* FindRow(int id)
    {
        if (id < FirstControl) return nullptr;
        const auto field = static_cast<Field>((id - FirstControl) / 4);
        for (auto& row : rows) if (row.field == field) return &row;
        return nullptr;
    }
    double Number(Field field) const noexcept
    {
        switch (field)
        {
        case Field::CompactScale: return configuration.layout.compactScale;
        case Field::ExpandedScale: return configuration.layout.expandedScale;
        case Field::Width: return configuration.layout.width;
        case Field::Height: return configuration.layout.height;
        case Field::ExpandedWidth: return configuration.layout.expandedWidth;
        case Field::ExpandedHeight: return configuration.layout.expandedHeight;
        case Field::OffsetX: return configuration.layout.offsetX;
        case Field::OffsetY: return configuration.layout.offsetY;
        case Field::FontSize: return configuration.layout.fontSize;
        case Field::LineWidth: return configuration.layout.lineWidth;
        case Field::SpringSpeed: return configuration.spring.speed;
        default: return 0;
        }
    }
    void SetNumber(Field field, double value)
    {
        const double limit = static_cast<double>((std::numeric_limits<float>::max)());
        const float result = static_cast<float>(std::clamp(value, -limit, limit));
        switch (field)
        {
        case Field::CompactScale: configuration.layout.compactScale = result; break;
        case Field::ExpandedScale: configuration.layout.expandedScale = result; break;
        case Field::Width: configuration.layout.width = result; break;
        case Field::Height: configuration.layout.height = result; break;
        case Field::ExpandedWidth: configuration.layout.expandedWidth = result; break;
        case Field::ExpandedHeight: configuration.layout.expandedHeight = result; break;
        case Field::OffsetX: configuration.layout.offsetX = result; break;
        case Field::OffsetY: configuration.layout.offsetY = result; break;
        case Field::FontSize: configuration.layout.fontSize = result; break;
        case Field::LineWidth: configuration.layout.lineWidth = result; break;
        case Field::SpringSpeed: configuration.spring.speed = value; break;
        default: break;
        }
    }
    bool Checked(Field field) const noexcept
    {
        switch (field)
        {
        case Field::Startup: return status.startupEnabled;
        case Field::AutoHideIdle: return configuration.autoHideIdle;
        case Field::AutoHideFullscreen: return configuration.autoHideFullscreen;
        case Field::RightDrag: return configuration.rightDrag;
        case Field::Notifications: return configuration.showNotifications;
        case Field::SystemControls: return configuration.systemControls;
        case Field::ClipboardLinks: return configuration.clipboardLinks;
        case Field::Privacy: return configuration.privacyIndicators;
        case Field::Glass: return configuration.nativeGlass;
        case Field::ReducedMotion: return configuration.reducedMotion;
        default: return false;
        }
    }
    void SetChecked(Field field, bool enabled)
    {
        switch (field)
        {
        case Field::AutoHideIdle: configuration.autoHideIdle = enabled; break;
        case Field::AutoHideFullscreen: configuration.autoHideFullscreen = enabled; break;
        case Field::RightDrag: configuration.rightDrag = enabled; break;
        case Field::Notifications: configuration.showNotifications = enabled; break;
        case Field::SystemControls: configuration.systemControls = enabled; break;
        case Field::ClipboardLinks: configuration.clipboardLinks = enabled; break;
        case Field::Privacy: configuration.privacyIndicators = enabled; break;
        case Field::Glass: configuration.nativeGlass = enabled; break;
        case Field::ReducedMotion: configuration.reducedMotion = enabled; break;
        default: break;
        }
    }
    static bool Fractional(Field field) noexcept
    {
        return field == Field::CompactScale || field == Field::ExpandedScale || field == Field::SpringSpeed;
    }
    std::wstring FormatNumber(Field field) const
    {
        wchar_t value[48]{};
        if (Fractional(field)) swprintf_s(value, L"%.2f", Number(field));
        else swprintf_s(value, L"%.0f", Number(field));
        return value;
    }
    static BOOL CALLBACK MonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM data) noexcept
    {
        auto* self = reinterpret_cast<Impl*>(data);
        try
        {
            MONITORINFOEXW info{}; info.cbSize = sizeof(info);
            if (GetMonitorInfoW(monitor, &info))
                self->displays.push_back({info.szDevice, info.rcMonitor, (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
            return TRUE;
        }
        catch (...) { return FALSE; }
    }
    void UpdateDisplays(Row& row)
    {
        displaysDirty = false;
        displays.clear(); displays.push_back({});
        EnumDisplayMonitors(nullptr, nullptr, MonitorProc, reinterpret_cast<LPARAM>(this));
        SendMessageW(row.control, CB_RESETCONTENT, 0, 0);
        SendMessageW(row.control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(L"主显示器（默认）", L"Primary display (default)")));
        int selection = 0;
        for (size_t index = 1; index < displays.size(); ++index)
        {
            const auto& display = displays[index];
            std::wstring label = T(L"显示器 ", L"Display ") + std::to_wstring(index) + L" · " +
                std::to_wstring(display.rectangle.right - display.rectangle.left) + L"×" +
                std::to_wstring(display.rectangle.bottom - display.rectangle.top);
            if (display.primary) label += T(L"（主屏）", L" (primary)");
            SendMessageW(row.control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (configuration.monitorDevice == display.device) selection = static_cast<int>(index);
        }
        if (!configuration.monitorDevice.empty() && selection == 0)
        {
            displays.push_back({configuration.monitorDevice});
            const auto label = std::wstring(T(L"已断开 · ", L"Disconnected · ")) + configuration.monitorDevice;
            selection = static_cast<int>(displays.size() - 1);
            SendMessageW(row.control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(row.control, CB_SETCURSEL, static_cast<WPARAM>(selection), 0);
        SendMessageW(row.control, CB_SETDROPPEDWIDTH, static_cast<WPARAM>(P(310)), 0);
    }
    void UpdateControls(bool forceNumbers)
    {
        if (!window) return;
        if (fonts[0] && fontEnglish != english) MakeFonts();
        const bool previousSync = syncing;
        syncing = true;
        SetWindowTextW(window, T(L"WinHalo 设置", L"WinHalo Settings"));
        const wchar_t* navigationChinese[] = {L"常规", L"外观", L"行为"};
        const wchar_t* navigationEnglish[] = {L"General", L"Appearance", L"Behavior"};
        for (size_t index = 0; index < navigation.size(); ++index)
            if (navigation[index]) SetWindowTextW(navigation[index], T(navigationChinese[index], navigationEnglish[index]));
        for (auto& row : rows)
        {
            if (accessibility)
            {
                accessibility->SetHwndPropStr(row.control, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, AccessibleName, Label(row));
                accessibility->SetHwndPropStr(row.control, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, AccessibleDescription, Hint(row).c_str());
            }
            if (row.kind == Kind::Toggle)
            {
                SetWindowTextW(row.control, Label(row));
                const LRESULT check = Checked(row.field) ? BST_CHECKED : BST_UNCHECKED;
                if (SendMessageW(row.control, BM_GETCHECK, 0, 0) != check) SendMessageW(row.control, BM_SETCHECK, static_cast<WPARAM>(check), 0);
                // Privacy capability is discovered after enabling its monitor;
                // an unknown/inactive capability must not prevent opting in.
                const bool available = row.field != Field::Startup || status.startupAvailable;
                if ((IsWindowEnabled(row.control) != FALSE) != available) EnableWindow(row.control, available);
                InvalidateRect(row.control, nullptr, FALSE);
            }
            else if (row.kind == Kind::Number)
            {
                if (forceNumbers || GetFocus() != row.control) SetWindowTextW(row.control, FormatNumber(row.field).c_str());
                const auto less = std::wstring(T(L"减小", L"Decrease ")) + Label(row);
                const auto more = std::wstring(T(L"增大", L"Increase ")) + Label(row);
                SetWindowTextW(row.minus, less.c_str()); SetWindowTextW(row.plus, more.c_str());
            }
            else if (row.kind == Kind::Language)
            {
                const int selection = configuration.language == L"zh-CN" ? 1 : (configuration.language == L"en-US" ? 2 : 0);
                if (languageItemsDirty || SendMessageW(row.control, CB_GETCOUNT, 0, 0) <= 0)
                {
                    SendMessageW(row.control, CB_RESETCONTENT, 0, 0);
                    for (const wchar_t* label : {T(L"跟随系统", L"System default"), L"简体中文", L"English"})
                        SendMessageW(row.control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
                    languageItemsDirty = false;
                }
                if (SendMessageW(row.control, CB_GETCURSEL, 0, 0) != selection)
                    SendMessageW(row.control, CB_SETCURSEL, static_cast<WPARAM>(selection), 0);
            }
            else if (displaysDirty || SendMessageW(row.control, CB_GETCOUNT, 0, 0) <= 0) UpdateDisplays(row);
        }
        if (resetButton) SetWindowTextW(resetButton, T(L"恢复默认", L"Restore defaults"));
        syncing = previousSync;
        if (content) InvalidateRect(content, nullptr, FALSE);
        for (HWND button : navigation) if (button) InvalidateRect(button, nullptr, FALSE);
    }
    void Notify()
    {
        NormalizeConfiguration(configuration);
        const bool nextEnglish = configuration.language == L"en-US" ||
            (configuration.language == L"system" && PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_CHINESE);
        if (english != nextEnglish) { english = nextEnglish; displaysDirty = languageItemsDirty = true; }
        callbackError.clear();
        try
        {
            // Pass a stable value: the callback may synchronously Refresh this window.
            const Configuration snapshot = configuration;
            if (change) change(snapshot);
        }
        catch (...)
        {
            callbackError = T(L"设置未能应用，请重试。", L"The settings could not be applied. Please try again.");
        }
        UpdateControls(true);
        Layout();
    }
    void CommitNumber(Row& row)
    {
        if (syncing || closing) return;
        wchar_t text[64]{}; GetWindowTextW(row.control, text, 64);
        wchar_t* end = nullptr;
        const double number = wcstod(text, &end);
        while (end && iswspace(*end)) ++end;
        if (end == text || !end || *end != L'\0' || !std::isfinite(number) ||
            std::abs(number) > static_cast<double>((std::numeric_limits<float>::max)()))
        {
            const bool previousSync = syncing; syncing = true;
            SetWindowTextW(row.control, FormatNumber(row.field).c_str());
            syncing = previousSync;
            return;
        }
        if (std::abs(number - Number(row.field)) < 0.0001)
        {
            const bool previousSync = syncing; syncing = true;
            SetWindowTextW(row.control, FormatNumber(row.field).c_str());
            syncing = previousSync;
            return;
        }
        SetNumber(row.field, Fractional(row.field) ? number : std::round(number));
        Notify();
    }
    void Bump(Row& row, int direction)
    {
        // Preserve a typed value before applying a step, without a second callback.
        wchar_t text[64]{}; GetWindowTextW(row.control, text, 64);
        wchar_t* end = nullptr;
        const double entered = wcstod(text, &end);
        while (end && iswspace(*end)) ++end;
        double current = end != text && end && !*end && std::isfinite(entered) ? entered : Number(row.field);
        current += direction * (Fractional(row.field) ? 0.1 : 1.0);
        if (row.field == Field::FontSize)
        {
            if (current > 0 && current < 10) current = direction > 0 ? 10 : 0;
        }
        SetNumber(row.field, current);
        Notify();
    }
    void Command(int id, int notification)
    {
        if (syncing || closing) return;
        if (id >= FirstNavigation && id < FirstNavigation + 3 && notification == BN_CLICKED)
        {
            ScrollTo(sections[static_cast<size_t>(id - FirstNavigation)] - 22);
            return;
        }
        if (id == ResetControl && notification == BN_CLICKED)
        {
            configuration = Configuration{};
            NormalizeConfiguration(configuration);
            english = configuration.language == L"en-US" ||
                (configuration.language == L"system" && PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_CHINESE);
            displaysDirty = languageItemsDirty = true;
            Notify();
            return;
        }
        Row* row = FindRow(id);
        if (!row) return;
        if (row->kind == Kind::Number)
        {
            if (notification == EN_KILLFOCUS && id == ControlId(row->field)) CommitNumber(*row);
            else if (notification == BN_CLICKED && id != ControlId(row->field))
                Bump(*row, id == ControlId(row->field, 1) ? -1 : 1);
        }
        else if (row->kind == Kind::Toggle && notification == BN_CLICKED)
        {
            const bool enabled = SendMessageW(row->control, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (row->field == Field::Startup)
            {
                try { if (status.startupAvailable && startup) startup(enabled); }
                catch (...) { callbackError = T(L"开机启动设置未能应用，请重试。", L"The startup setting could not be applied. Please try again."); }
                UpdateControls(false);
                InvalidateRect(window, nullptr, FALSE);
            }
            else { SetChecked(row->field, enabled); Notify(); }
        }
        else if (notification == CBN_SELCHANGE)
        {
            const auto index = SendMessageW(row->control, CB_GETCURSEL, 0, 0);
            if (row->kind == Kind::Language)
            {
                configuration.language = index == 1 ? L"zh-CN" : (index == 2 ? L"en-US" : L"system");
                english = configuration.language == L"en-US" ||
                    (configuration.language == L"system" && PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_CHINESE);
                displaysDirty = languageItemsDirty = true;
            }
            else if (index >= 0 && static_cast<size_t>(index) < displays.size())
                configuration.monitorDevice = displays[static_cast<size_t>(index)].device;
            Notify();
        }
    }

    void BuildGeometry()
    {
        int top = 106;
        for (size_t index = 0; index < cards.size(); ++index)
        {
            if (index == 0 || index == 1 || index == 4)
            {
                const size_t section = index == 0 ? 0 : (index == 1 ? 1 : 2);
                sections[section] = top; top += 34;
            }
            auto& card = cards[index]; card.top = top;
            for (size_t row = card.first; row < card.first + card.count; ++row)
            {
                rows[row].y = top; top += rows[row].height;
            }
            card.bottom = top; top += 18;
        }
        totalHeight = top + 88;
        geometryDirty = false;
    }
    void Layout()
    {
        if (!window || !content || layingOut || changingDpi) return;
        struct Guard { bool& value; Guard(bool& flag) : value(flag) { value = true; } ~Guard() { value = false; } } guard(layingOut);
        RECT client{}, current{}; GetClientRect(window, &client); GetWindowRect(content, &current);
        POINT origin{current.left, current.top}; ScreenToClient(window, &origin);
        const int sidebarWidth = P(174);
        const int width = std::max(1, static_cast<int>(client.right) - sidebarWidth);
        const int height = std::max(1L, client.bottom);
        const bool outerChanged = origin.x != sidebarWidth || origin.y != 0 || current.right - current.left != width ||
            current.bottom - current.top != height || laidOutDpi != dpi;
        if (outerChanged)
        {
            outerPositions.clear(); outerPositions.reserve(4);
            outerPositions.push_back({content, sidebarWidth, 0, width, height});
            for (size_t index = 0; index < navigation.size(); ++index)
                if (navigation[index]) outerPositions.push_back({navigation[index], P(16), P(116 + static_cast<int>(index) * 50), P(142), P(42)});
            PositionBatch(outerPositions);
        }
        // WM_SIZE from the batch is suppressed by the guard. Use the final size.
        LayoutContentNow();
        if (outerChanged) RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
        else InvalidateRect(window, nullptr, FALSE);
    }
    void LayoutContent()
    {
        if (!content || layingOut || changingDpi) return;
        struct Guard { bool& value; Guard(bool& flag) : value(flag) { value = true; } ~Guard() { value = false; } } guard(layingOut);
        LayoutContentNow();
    }
    void LayoutContentNow()
    {
        RECT client{}; GetClientRect(content, &client);
        const bool rebuild = geometryDirty;
        if (rebuild) BuildGeometry();
        contentHeight = D(client.bottom);
        scroll = std::clamp(scroll, 0, std::max(0, totalHeight - contentHeight));
        if (!rebuild && laidOutWidth == client.right && laidOutHeight == client.bottom && laidOutDpi == dpi && laidOutScroll == scroll) return;
        SCROLLINFO info{sizeof(info), SIF_PAGE | SIF_RANGE | SIF_POS};
        info.nMin = 0; info.nMax = std::max(0, totalHeight - 1);
        info.nPage = static_cast<UINT>(std::max(1, contentHeight)); info.nPos = scroll;
        SetScrollInfo(content, SB_VERT, &info, FALSE);
        GetClientRect(content, &client); contentWidth = D(client.right);
        const int right = contentWidth - 40;
        contentPositions.clear(); contentPositions.reserve(rows.size() * 3 + 1);
        for (const auto& row : rows)
        {
            const int centerY = row.y - scroll + row.height / 2;
            if (row.kind == Kind::Toggle)
                contentPositions.push_back({row.control, P(right - 48), P(centerY - 16), P(48), P(32)});
            else if (row.kind == Kind::Number)
            {
                contentPositions.push_back({row.minus, P(right - 146), P(centerY - 16), P(30), P(32)});
                contentPositions.push_back({row.control, P(right - 108), P(centerY - 11), P(70), P(24)});
                contentPositions.push_back({row.plus, P(right - 30), P(centerY - 16), P(30), P(32)});
            }
            else
            {
                const int width = std::min(238, std::max(165, contentWidth / 3));
                contentPositions.push_back({row.control, P(right - width), P(centerY - 14), P(width), P(260)});
            }
        }
        if (resetButton) contentPositions.push_back({resetButton, P(32), P(totalHeight - 86 - scroll), P(136), P(36)});
        PositionBatch(contentPositions);
        laidOutWidth = client.right; laidOutHeight = client.bottom; laidOutDpi = dpi; laidOutScroll = scroll;
        UpdateSection();
        // Parent-only invalidation excludes the WS_CLIPCHILDREN areas.
        RedrawWindow(content, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
    }
    void UpdateSection()
    {
        int next = 0;
        for (size_t index = 1; index < sections.size(); ++index)
            if (scroll + 90 >= sections[index]) next = static_cast<int>(index);
        if (next != selectedSection)
        {
            selectedSection = next;
            for (HWND button : navigation) if (button) InvalidateRect(button, nullptr, FALSE);
        }
    }
    void ScrollTo(int value)
    {
        scrollPending = false;
        const int next = std::clamp(value, 0, std::max(0, totalHeight - contentHeight));
        if (next == scroll) return;
        scroll = next;
        LayoutContent();
    }
    void QueueScroll(int value)
    {
        requestedScroll = std::clamp(value, 0, std::max(0, totalHeight - contentHeight));
        scrollPending = true;
        if (!scrollPosted)
        {
            scrollPosted = PostMessageW(content, ApplyScrollMessage, 0, 0) != FALSE;
            if (!scrollPosted) ScrollTo(requestedScroll);
        }
    }
    void EnsureRowVisible(const Row& row)
    {
        if (row.y < scroll + 8) ScrollTo(row.y - 16);
        else if (row.y + row.height > scroll + contentHeight - 8)
            ScrollTo(row.y + row.height - contentHeight + 16);
    }

    void Fill(HDC dc, RECT rect, COLORREF color)
    {
        const auto old = SelectObject(dc, GetStockObject(DC_BRUSH));
        SetDCBrushColor(dc, color); FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        SelectObject(dc, old);
    }
    template<class Draw> void Shapes(SettingsPainter& painter, HDC dc, const RECT& bounds, COLORREF background, Draw draw)
    {
        const bool antialiased = painter.Begin(dc, bounds);
        shapeDC = antialiased ? dc : nullptr;
        shapePainter = antialiased ? &painter : nullptr;
        if (antialiased) painter.Clear(background);
        else Fill(dc, bounds, background);
        try { draw(); }
        catch (...)
        {
            shapeDC = nullptr;
            shapePainter = nullptr;
            if (antialiased) painter.End();
            throw;
        }
        shapeDC = nullptr;
        shapePainter = nullptr;
        if (antialiased && !painter.End())
        {
            Fill(dc, bounds, background);
            draw();
        }
    }
    void Rounded(HDC dc, RECT rect, COLORREF color, int radius, COLORREF outline = CLR_INVALID)
    {
        if (shapeDC == dc)
        {
            shapePainter->Rounded(rect, static_cast<float>(P(radius)), color, outline);
            return;
        }
        const auto brush = SelectObject(dc, GetStockObject(DC_BRUSH));
        const auto pen = SelectObject(dc, outline == CLR_INVALID ? GetStockObject(NULL_PEN) : GetStockObject(DC_PEN));
        SetDCBrushColor(dc, color);
        if (outline != CLR_INVALID) SetDCPenColor(dc, outline);
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, P(static_cast<float>(radius * 2)), P(static_cast<float>(radius * 2)));
        SelectObject(dc, pen); SelectObject(dc, brush);
    }
    void Outline(HDC dc, RECT rect, int radius, COLORREF color, float width = 1)
    {
        if (shapeDC == dc) { shapePainter->OutlineRounded(rect, static_cast<float>(P(radius)), color, width); return; }
        const HPEN stroke = CreatePen(PS_SOLID, std::max(1, static_cast<int>(std::lround(width))), color);
        const auto oldPen = SelectObject(dc, stroke), oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, P(radius * 2), P(radius * 2));
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(stroke);
    }
    void Line(HDC dc, float x1, float y1, float x2, float y2, COLORREF color, float width = 1)
    {
        if (shapeDC == dc) { shapePainter->Line(x1, y1, x2, y2, color, width); return; }
        const HPEN stroke = CreatePen(PS_SOLID, std::max(1, static_cast<int>(std::lround(width))), color);
        const auto old = SelectObject(dc, stroke);
        MoveToEx(dc, static_cast<int>(std::lround(x1)), static_cast<int>(std::lround(y1)), nullptr);
        LineTo(dc, static_cast<int>(std::lround(x2)), static_cast<int>(std::lround(y2)));
        SelectObject(dc, old); DeleteObject(stroke);
    }
    void Circle(HDC dc, float x, float y, float radius, COLORREF color, float width)
    {
        if (shapeDC == dc) { shapePainter->Ellipse(x, y, radius, radius, CLR_INVALID, color, width); return; }
        const HPEN stroke = CreatePen(PS_SOLID, std::max(1, static_cast<int>(std::lround(width))), color);
        const auto oldPen = SelectObject(dc, stroke), oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, static_cast<int>(std::lround(x - radius)), static_cast<int>(std::lround(y - radius)),
            static_cast<int>(std::lround(x + radius)), static_cast<int>(std::lround(y + radius)));
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(stroke);
    }
    void Text(HDC dc, const std::wstring& value, RECT rect, size_t font = 0, COLORREF color = Ink,
        UINT flags = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS)
    {
        const auto previous = SelectObject(dc, Font(font));
        const int oldMode = SetBkMode(dc, TRANSPARENT);
        const COLORREF oldColor = SetTextColor(dc, color);
        DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &rect, flags | DT_NOPREFIX);
        SetTextColor(dc, oldColor); SetBkMode(dc, oldMode);
        SelectObject(dc, previous);
    }
    void PaintMain(HDC dc)
    {
        RECT visible{}; if (GetClipBox(dc, &visible) == NULLREGION) return;
        RECT client{}; GetClientRect(window, &client);
        Surface surface(dc, client, mainBuffer); dc = surface.dc;
        Shapes(mainPainter, dc, client, Background, [&] {
            Rounded(dc, R(0, 0, 174, D(client.bottom)), Sidebar, 0);
            const float x = static_cast<float>(P(174)) - .5f;
            Line(dc, x, 0, x, static_cast<float>(client.bottom), Border);
            Rounded(dc, R(22, 29, 45, 37), Accent, 4);
        });
        Text(dc, L"WinHalo", R(22, 48, 162, 82), 4);
        Text(dc, T(L"设置", L"SETTINGS"), R(23, 80, 158, 101), 1, Muted);
        const std::wstring error = !status.saveError.empty() ? status.saveError : callbackError;
        Text(dc, error.empty() ? T(L"你的桌面，恰到好处。", L"A little less. A little closer.") : error,
            R(22, D(client.bottom) - 77, 157, D(client.bottom) - 17), 1,
            error.empty() ? Muted : RGB(189, 72, 58), DT_LEFT | DT_WORDBREAK);
    }
    std::wstring Hint(const Row& row) const
    {
        if (row.field == Field::Startup && !status.startupError.empty()) return status.startupError;
        if (row.field == Field::Startup && !status.startupAvailable)
            return T(L"无法访问 Windows 开机启动设置", L"Windows startup settings are unavailable");
        if (row.field == Field::SystemControls && !status.brightnessStatus.empty())
            return std::wstring(T(row.hintChinese, row.hintEnglish)) + L"\n" + status.brightnessStatus;
        if (row.field == Field::SystemControls && !status.brightnessAvailable)
            return std::wstring(T(row.hintChinese, row.hintEnglish)) + L"\n" +
                T(L"当前屏幕无法调节亮度；音量控制仍可使用", L"Brightness is unavailable on this display; volume is still available");
        if (row.field == Field::Privacy && !configuration.privacyIndicators)
            return T(L"开启后读取系统活动状态，不采集音视频", L"Read system activity when enabled; no audio or video capture");
        if (row.field == Field::Privacy && !status.privacyStatus.empty()) return status.privacyStatus;
        if (row.field == Field::Privacy && !status.privacyAvailable)
            return T(L"等待 Windows 提供设备活动状态", L"Waiting for Windows device activity status");
        return T(row.hintChinese, row.hintEnglish);
    }
    void PaintContent(HDC dc)
    {
        RECT visible{}; if (GetClipBox(dc, &visible) == NULLREGION) return;
        RECT client{}; GetClientRect(content, &client);
        Surface surface(dc, client, contentBuffer); dc = surface.dc;
        Shapes(contentPainter, dc, client, Background, [&] {
            for (const auto& card : cards)
            {
                if (card.bottom < scroll || card.top > scroll + contentHeight) continue;
                Rounded(dc, R(32, card.top - scroll, contentWidth - 24, card.bottom - scroll), RGB(255, 255, 255), 12, Border);
                for (size_t index = card.first; index < card.first + card.count; ++index)
                {
                    const auto& row = rows[index]; const int y = row.y - scroll;
                    if (row.kind == Kind::Number)
                        Rounded(dc, R(contentWidth - 153, y + row.height / 2 - 16, contentWidth - 74, y + row.height / 2 + 16),
                            RGB(247, 249, 252), 7, Border);
                    if (index + 1 != card.first + card.count)
                    {
                        const float lineY = static_cast<float>(P(y + row.height)) + .5f;
                        Line(dc, static_cast<float>(P(52)), lineY, static_cast<float>(P(contentWidth - 44)), lineY, Border);
                    }
                }
            }
        });
        Text(dc, T(L"让 WinHalo 更像你", L"Make WinHalo yours"), R(32, 25 - scroll, contentWidth - 24, 61 - scroll), 3);
        Text(dc, T(L"外观与行为，按你的习惯调整。所有更改立即生效。", L"Appearance and behavior, tuned to you. Changes apply instantly."),
            R(33, 65 - scroll, contentWidth - 28, 88 - scroll), 1, Muted);
        const wchar_t* chinese[] = {L"常规", L"外观", L"行为"};
        const wchar_t* other[] = {L"General", L"Appearance", L"Behavior"};
        for (size_t index = 0; index < sections.size(); ++index)
            Text(dc, T(chinese[index], other[index]), R(34, sections[index] - scroll, contentWidth - 32, sections[index] + 27 - scroll), 2);
        for (const auto& card : cards)
        {
            if (card.bottom < scroll || card.top > scroll + contentHeight) continue;
            for (size_t index = card.first; index < card.first + card.count; ++index)
            {
                const auto& row = rows[index];
                const int y = row.y - scroll;
                const int right = contentWidth - 40;
                const int group = row.kind == Kind::Toggle ? 66 : (row.kind == Kind::Number ? 162 :
                    std::min(238, std::max(165, contentWidth / 3)) + 16);
                const int labelRight = right - group;
                Text(dc, Label(row), R(52, y + 12, labelRight, y + 36), 0, Ink);
                const auto hint = Hint(row);
                const bool unavailable = (row.field == Field::Startup && !status.startupAvailable) ||
                    (row.field == Field::Privacy && !status.privacyAvailable);
                const COLORREF hintColor = unavailable ? RGB(148, 116, 77) : Muted;
                Text(dc, hint, R(52, y + 38, labelRight, y + row.height - 9), 1, hintColor,
                    DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
            }
        }
        const std::wstring error = !status.saveError.empty() ? status.saveError : callbackError;
        Text(dc, error.empty() ? T(L"恢复默认不会更改 Windows 开机启动设置。", L"Restoring defaults does not change Windows startup registration.") : error,
            R(184, totalHeight - 80 - scroll, contentWidth - 24, totalHeight - 35 - scroll), 1,
            error.empty() ? Muted : RGB(189, 72, 58), DT_LEFT | DT_WORDBREAK);
    }
    void PaintToggle(HWND control, HDC dc)
    {
        RECT visible{}; if (GetClipBox(dc, &visible) == NULLREGION) return;
        RECT rect{}; GetClientRect(control, &rect);
        Surface surface(dc, rect, toggleBuffer); dc = surface.dc;
        const bool enabled = IsWindowEnabled(control) != FALSE;
        const bool checked = SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool pressed = (SendMessageW(control, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;
        const bool hover = hoveredControl == control;
        const COLORREF track = !enabled ? RGB(229, 232, 237) : checked ?
            (pressed ? RGB(30, 83, 191) : (hover ? RGB(38, 100, 222) : Accent)) :
            (hover || pressed ? RGB(185, 195, 210) : RGB(199, 207, 219));
        Shapes(togglePainter, dc, rect, RGB(255, 255, 255), [&] {
            Rounded(dc, R(2, 5, 46, 27), track, 11);
            const int x = checked ? 26 : 5;
            Rounded(dc, R(x, 8, x + 16, 24), RGB(255, 255, 255), 8);
            if (KeyboardFocus(control)) Outline(dc, R(0, 2, 48, 30), 11, Accent, std::max(1.5f, dpi / 96.0f));
        });
    }
    void DrawButton(const DRAWITEMSTRUCT& item)
    {
        RECT visible{}; if (GetClipBox(item.hDC, &visible) == NULLREGION) return;
        Surface surface(item.hDC, item.rcItem, buttonBuffer); HDC dc = surface.dc;
        const int id = static_cast<int>(item.CtlID);
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        const bool nav = id >= FirstNavigation && id < FirstNavigation + 3;
        const bool selected = nav && id - FirstNavigation == selectedSection;
        const bool hover = hoveredControl == item.hwndItem;
        const bool focused = !(item.itemState & ODS_NOFOCUSRECT) && KeyboardFocus(item.hwndItem);
        const RECT shape = item.rcItem;
        const Row* row = FindRow(id);
        Shapes(buttonPainter, dc, shape, nav ? Sidebar : (id == ResetControl ? Background : RGB(255, 255, 255)), [&] {
            if (nav) Rounded(dc, shape, selected ? RGB(229, 238, 255) : (pressed ? RGB(227, 233, 243) :
                (hover ? RGB(239, 243, 249) : Sidebar)), 8);
            else Rounded(dc, shape, disabled ? RGB(248, 249, 251) : (pressed ? RGB(226, 234, 248) :
                (hover ? RGB(238, 243, 252) : RGB(247, 249, 252))), 7, hover && !disabled ? RGB(188, 205, 234) : RGB(216, 222, 232));
            const COLORREF ink = disabled ? Muted : (nav && selected ? Accent : Ink);
            const float stroke = std::max(1.0f, dpi / 96.0f);
            if (nav)
            {
                const float y = (shape.top + shape.bottom) / 2.0f;
                const float x = static_cast<float>(P(18)), size = static_cast<float>(P(13));
                if (id == FirstNavigation)
                {
                    RECT icon{P(18), static_cast<LONG>(std::lround(y - size / 2)), P(31), static_cast<LONG>(std::lround(y + size / 2))};
                    Outline(dc, icon, 1, ink, stroke);
                    Line(dc, x + P(4), y - P(3), x + P(9), y - P(3), ink, stroke);
                    Line(dc, x + P(4), y + P(2), x + P(9), y + P(2), ink, stroke);
                }
                else if (id == FirstNavigation + 1) Circle(dc, x + size / 2, y, size / 2, ink, stroke);
                else for (int offset : {-4, 0, 4}) Line(dc, x, y + P(offset), x + size, y + P(offset), ink, stroke);
            }
            else if (row)
            {
                const float x = (shape.left + shape.right) / 2.0f, y = (shape.top + shape.bottom) / 2.0f;
                Line(dc, x - P(4), y, x + P(4), y, ink, std::max(1.5f, stroke));
                if ((id - FirstControl) % 4 == 2) Line(dc, x, y - P(4), x, y + P(4), ink, std::max(1.5f, stroke));
            }
            if (focused)
            {
                RECT focus = shape; InflateRect(&focus, -P(2), -P(2));
                Outline(dc, focus, nav ? 7 : 6, Accent, std::max(1.5f, dpi / 96.0f));
            }
        });
        if (nav || !row)
        {
            wchar_t caption[128]{}; GetWindowTextW(item.hwndItem, caption, 128);
            RECT text = shape;
            if (nav) text.left += P(43);
            Text(dc, caption, text, 0, disabled ? Muted : (selected ? Accent : Ink),
                (nav ? DT_LEFT : DT_CENTER) | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }

    LRESULT MainMessage(HWND handle, UINT message, WPARAM wp, LPARAM lp)
    {
        switch (message)
        {
        case WM_CREATE:
            dpi = GetDpiForWindow(handle);
            MakeFonts();
            content = CreateWindowExW(WS_EX_CONTROLPARENT, ContentClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                0, 0, 1, 1, handle, nullptr, instance, this);
            if (!content) return -1;
            for (size_t index = 0; index < navigation.size(); ++index)
            {
                navigation[index] = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | WS_CLIPSIBLINGS,
                    0, 0, 1, 1, handle, reinterpret_cast<HMENU>(static_cast<INT_PTR>(FirstNavigation + index)), instance, nullptr);
                if (!navigation[index]) return -1;
                SetWindowSubclass(navigation[index], ControlProc, 1, reinterpret_cast<DWORD_PTR>(this));
            }
            UpdateControls(false);
            return 0;
        case WM_SIZE: Layout(); return 0;
        case WM_DPICHANGED:
        {
            dpi = HIWORD(wp); MakeFonts();
            const auto* rectangle = reinterpret_cast<const RECT*>(lp);
            changingDpi = true;
            const BOOL positioned = SetWindowPos(handle, nullptr, rectangle->left, rectangle->top, rectangle->right - rectangle->left,
                rectangle->bottom - rectangle->top, SWP_NOACTIVATE | SWP_NOZORDER);
            changingDpi = false;
            CheckWin32(positioned, "Position settings after DPI change");
            Layout(); return 0;
        }
        case WM_GETMINMAXINFO:
        {
            auto* information = reinterpret_cast<MINMAXINFO*>(lp);
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor);
            information->ptMinTrackSize.x = std::min(P(690), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
            information->ptMinTrackSize.y = std::min(P(480), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
            return 0;
        }
        case WM_DISPLAYCHANGE: displaysDirty = true; UpdateControls(false); Layout(); return 0;
        case WM_COMMAND: Command(LOWORD(wp), HIWORD(wp)); return 0;
        case WM_DRAWITEM: DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE;
        case WM_MOUSEWHEEL: return SendMessageW(content, message, wp, lp);
        case WM_LBUTTONDOWN: KeyboardCues(false); break;
        case WM_ERASEBKGND: return 1;
        case WM_PRINTCLIENT:
            PaintMain(reinterpret_cast<HDC>(wp)); return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT paint{}; HDC dc = BeginPaint(handle, &paint);
            PaintMain(dc); EndPaint(handle, &paint); return 0;
        }
        case WM_CLOSE: Close(); return 0;
        case WM_NCDESTROY:
            window = content = resetButton = nullptr;
            navigation.fill(nullptr); rows.clear(); cards.clear();
            scroll = 0;
            scrollPosted = scrollPending = false; requestedScroll = 0; wheelFraction = 0;
            laidOutWidth = laidOutHeight = laidOutScroll = -1; laidOutDpi = 0;
            geometryDirty = true; hoveredControl = nullptr; selectedSection = 0;
            mainPainter.Reset(); contentPainter.Reset(); buttonPainter.Reset(); togglePainter.Reset();
            mainBuffer.Reset(); contentBuffer.Reset(); buttonBuffer.Reset(); toggleBuffer.Reset();
            SetWindowLongPtrW(handle, GWLP_USERDATA, 0);
            break;
        default: break;
        }
        return DefWindowProcW(handle, message, wp, lp);
    }
    LRESULT ContentMessage(HWND handle, UINT message, WPARAM wp, LPARAM lp)
    {
        switch (message)
        {
        case WM_CREATE: CreateRows(); return 0;
        case WM_SIZE: LayoutContent(); return 0;
        case WM_COMMAND: Command(LOWORD(wp), HIWORD(wp)); return 0;
        case WM_DRAWITEM: DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE;
        case ApplyScrollMessage:
            scrollPosted = false;
            if (scrollPending) ScrollTo(requestedScroll);
            return 0;
        case WM_VSCROLL:
        {
            SCROLLINFO information{sizeof(information), SIF_TRACKPOS | SIF_POS};
            GetScrollInfo(handle, SB_VERT, &information);
            int value = scrollPending ? requestedScroll : scroll;
            switch (LOWORD(wp))
            {
            case SB_LINEUP: value -= 36; break;
            case SB_LINEDOWN: value += 36; break;
            case SB_PAGEUP: value -= contentHeight - 44; break;
            case SB_PAGEDOWN: value += contentHeight - 44; break;
            case SB_TOP: value = 0; break;
            case SB_BOTTOM: value = totalHeight; break;
            case SB_THUMBPOSITION: case SB_THUMBTRACK: value = information.nTrackPos; break;
            default: return 0;
            }
            QueueScroll(value); return 0;
        }
        case WM_MOUSEWHEEL:
        {
            UINT lines = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            const int step = lines == WHEEL_PAGESCROLL ? std::max(36, contentHeight - 44) : static_cast<int>(std::min(lines, 12u)) * 22;
            const double next = (scrollPending ? requestedScroll : scroll) + wheelFraction -
                static_cast<double>(GET_WHEEL_DELTA_WPARAM(wp)) * step / WHEEL_DELTA;
            const int position = static_cast<int>(std::lround(next));
            wheelFraction = next - position;
            QueueScroll(position); return 0;
        }
        case WM_CTLCOLOREDIT:
        {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, Ink); SetBkColor(dc, RGB(247, 249, 252));
            return reinterpret_cast<LRESULT>(editBrush);
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            SetBkColor(reinterpret_cast<HDC>(wp), RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(cardBrush);
        case WM_ERASEBKGND: return 1;
        case WM_LBUTTONDOWN: KeyboardCues(false); break;
        case WM_PRINTCLIENT:
            PaintContent(reinterpret_cast<HDC>(wp)); return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT paint{}; HDC dc = BeginPaint(handle, &paint);
            PaintContent(dc); EndPaint(handle, &paint); return 0;
        }
        default: break;
        }
        return DefWindowProcW(handle, message, wp, lp);
    }
    bool PreTranslate(MSG& message)
    {
        if (!window || (message.hwnd != window && !IsChild(window, message.hwnd))) return false;
        if (message.message == WM_KEYDOWN)
        {
            KeyboardCues(true);
            const int id = GetDlgCtrlID(GetFocus());
            Row* row = FindRow(id);
            if (row && (row->kind == Kind::Language || row->kind == Kind::Monitor) &&
                SendMessageW(row->control, CB_GETDROPPEDSTATE, 0, 0)) return false;
            if (message.wParam == VK_ESCAPE) { Close(); return true; }
            if (message.wParam == VK_RETURN && row && row->kind == Kind::Number && GetFocus() == row->control)
            {
                CommitNumber(*row); SendMessageW(row->control, EM_SETSEL, 0, -1); return true;
            }
            if (message.wParam == VK_PRIOR || message.wParam == VK_NEXT)
            {
                ScrollTo(scroll + (message.wParam == VK_PRIOR ? -1 : 1) * std::max(36, contentHeight - 44)); return true;
            }
            if ((message.wParam == VK_HOME || message.wParam == VK_END) && GetKeyState(VK_CONTROL) < 0)
            {
                ScrollTo(message.wParam == VK_HOME ? 0 : totalHeight); return true;
            }
        }
        return IsDialogMessageW(window, &message) != FALSE;
    }
};

SettingsWindow::SettingsWindow(HINSTANCE instance) : impl_(std::make_unique<Impl>(instance)) {}
SettingsWindow::~SettingsWindow() = default;
void SettingsWindow::Show(const Configuration& configuration, const SettingsStatus& status,
    ChangeHandler change, StartupHandler startup)
{
    impl_->Show(configuration, status, std::move(change), std::move(startup));
}
void SettingsWindow::Refresh(const Configuration& configuration, const SettingsStatus& status) { impl_->Refresh(configuration, status); }
void SettingsWindow::Close() { impl_->Close(); }
HWND SettingsWindow::Handle() const noexcept { return impl_->window; }
bool SettingsWindow::PreTranslate(MSG& message) { return impl_->PreTranslate(message); }
}
