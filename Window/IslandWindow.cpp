#include "IslandWindow.h"
#include "../Utils/Win32Error.h"
#include "../Utils/Localization.h"
#include "../Resource.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <windowsx.h>
#include <commctrl.h>
#pragma comment(lib, "Comctl32.lib")

namespace winisland
{
namespace
{
constexpr wchar_t WindowClass[] = L"WinHalo.NativeIslandWindow";
constexpr int VisibilityHotkey = 1;
constexpr int QuitHotkey = 2;
constexpr int PanelHotkey = 3;
constexpr UINT_PTR RightHoldTimer = 1;
constexpr UINT RightHoldMilliseconds = 400;
enum TrayCommand : UINT { TrayToggle = 1101, TrayExpand, TraySettings, TrayExit };
struct MenuDeleter { void operator()(HMENU menu) const noexcept { if (menu) DestroyMenu(menu); } };
using Menu = std::unique_ptr<std::remove_pointer_t<HMENU>, MenuDeleter>;
struct WindowDeleter { void operator()(HWND window) const noexcept { if (window) DestroyWindow(window); } };
using MenuOwner = std::unique_ptr<std::remove_pointer_t<HWND>, WindowDeleter>;
LONG ToPixels(float dip, UINT dpi)
{
    return static_cast<LONG>(std::lround(dip * static_cast<float>(dpi) / 96.0f));
}

MONITORINFOEXW MonitorInfo(HMONITOR monitor)
{
    MONITORINFOEXW info{}; info.cbSize = sizeof(info);
    CheckWin32(GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info)), "GetMonitorInfoW");
    return info;
}

void ClampIslandPosition(LONG& x, LONG& y, const RECT& work, const IslandView& view, UINT dpi)
{
    const auto bounds = view.Bounds();
    const LONG left = ToPixels(bounds.left, dpi), right = ToPixels(bounds.right, dpi);
    const LONG top = ToPixels(bounds.top, dpi), bottom = ToPixels(bounds.bottom, dpi);
    x = std::clamp(x, work.left - left, std::max(work.left - left, work.right - right));
    y = std::clamp(y, work.top - top, std::max(work.top - top, work.bottom - bottom));
}
}

IslandWindow::IslandWindow(HINSTANCE instance, IRenderer& renderer, IslandView& view, Logger& logger)
    : instance_(instance), renderer_(renderer), view_(view), logger_(logger) {}

IslandWindow::~IslandWindow()
{
    if (window_)
    {
        KillTimer(window_, RightHoldTimer);
        if (clipboardListening_) RemoveClipboardFormatListener(window_);
    }
    outsideClicks_.Stop();
    if (sourceTooltip_) DestroyWindow(sourceTooltip_);
    backdrop_.Hide();
    tray_.Remove();
    if (window_) DestroyWindow(window_);
    if (classRegistered_) UnregisterClassW(WindowClass, instance_);
}

void IslandWindow::Create(EventHandler handler, bool show)
{
    handler_ = std::move(handler);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!windowClass.hCursor) ThrowLastError("LoadCursorW");
    windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_WINHALO));
    if (!windowClass.hIcon) ThrowLastError("LoadIconW");
    windowClass.lpszClassName = WindowClass;
    // 不设置背景画刷或类菜单，避免系统绘制不透明背景。
    if (!RegisterClassExW(&windowClass)) ThrowLastError("RegisterClassExW");
    classRegistered_ = true;
    window_ = CreateWindowExW(renderer_.WindowStyle() | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        WindowClass, L"WinHalo", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance_, this);
    if (!window_) ThrowLastError("CreateWindowExW");
    PositionOnMonitor(PreferredMonitor());
    backdrop_.Initialize(window_);
    SetNativeGlass(false);
    RethrowWindowError();
    visibilityHotkey_ = RegisterHotKey(window_, VisibilityHotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'I') != FALSE;
    quitHotkey_ = RegisterHotKey(window_, QuitHotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q') != FALSE;
    panelHotkey_ = RegisterHotKey(window_, PanelHotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SPACE) != FALSE;
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (!taskbarCreatedMessage_) ThrowLastError("RegisterWindowMessageW (TaskbarCreated)");
    if (!tray_.Create(window_, windowClass.hIcon)) logger_.Write("System tray unavailable; visibility hotkey remains available when registered");
    if (!visibilityHotkey_) logger_.Write(tray_.Registered() ?
        "Ctrl+Alt+I is unavailable; use the system tray to restore the island" :
        "Ctrl+Alt+I and system tray are unavailable; Hide menu item disabled");
    if (!quitHotkey_) logger_.Write("Ctrl+Alt+Q is unavailable; use the Exit menu item");
    if (!panelHotkey_) logger_.Write("Ctrl+Alt+Space is unavailable; use the media capsule or system tray");
    if (show) ApplyVisibility();
    else if (handler_) handler_(WindowEvent::WindowHidden);
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_WIN95_CLASSES}; InitCommonControlsEx(&common);
    sourceTooltip_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, instance_, nullptr);
    if (sourceTooltip_) SendMessageW(sourceTooltip_, TTM_SETMAXTIPWIDTH, 0, 300);
    SyncClipboardListener();
}

void IslandWindow::ApplyPreferences(const Configuration& configuration)
{
    const bool positionChanged = !preferencesApplied_ ||
        preferences_.monitorDevice != configuration.monitorDevice ||
        preferences_.layout.offsetX != configuration.layout.offsetX || preferences_.layout.offsetY != configuration.layout.offsetY ||
        preferences_.layout.topGap != configuration.layout.topGap ||
        preferences_.layout.hostWidth != configuration.layout.hostWidth || preferences_.layout.hostHeight != configuration.layout.hostHeight;
    if (rightButtonDown_ && !configuration.rightDrag) FinishRightPress(true);
    preferences_ = configuration;
    preferencesApplied_ = true;
    if (!configuration.systemControls) wheelRemainder_ = 0;
    if (!window_) return;
    SyncClipboardListener();
    if (positionChanged) PositionOnMonitor(PreferredMonitor());
    else Render();
}

void IslandWindow::SyncClipboardListener()
{
    if (!window_) return;
    if (preferences_.clipboardLinks && !clipboardListening_)
    {
        clipboardListening_ = AddClipboardFormatListener(window_) != FALSE;
        if (!clipboardListening_) logger_.Write("Clipboard change listener unavailable");
    }
    else if (!preferences_.clipboardLinks && clipboardListening_)
    {
        RemoveClipboardFormatListener(window_);
        clipboardListening_ = false;
    }
}

void IslandWindow::Render()
{
    if (window_)
    {
        renderer_.Render(window_, view_);
        backdrop_.Sync(window_, view_);
        SyncMediaInteraction();
        // A shrinking region can leave the cursor outside without another mouse
        // message. Use the displayed shape, including during an animation.
        SyncPointer();
    }
}

void IslandWindow::SyncMediaInteraction()
{
    const bool active = Visible() && view_.Expanded() && view_.Page() == IslandPage::Media && !menuActive_;
    if (active && !outsideHookFailed_)
    {
        RECT host{}; GetWindowRect(window_, &host);
        const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96;
        const auto frame = view_.Frame();
        const auto bounds = view_.Bounds();
        if (!outsideClicks_.Watch(window_, OutsideClickMessage, {host.left + bounds.left * scale,
            host.top + bounds.top * scale, frame.width * scale, frame.height * scale,
            std::min({frame.radius, frame.width / 2, frame.height / 2}) * scale}))
        {
            outsideHookFailed_ = true;
            logger_.Write("Outside click hook unavailable; Ctrl+Alt+Space still collapses the player");
        }
    }
    else outsideClicks_.Stop();
    if (!sourceTooltip_) return;
    TOOLINFOW tool{}; tool.cbSize = sizeof(tool); tool.hwnd = window_; tool.uId = 1;
    tool.uFlags = TTF_SUBCLASS;
    if (!active)
    {
        if (sourceTipAdded_) SendMessageW(sourceTooltip_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        sourceTipAdded_ = false;
        return;
    }
    const auto& media = view_.Media();
    std::wstring tip = media.app ? media.app->name : (media.source.empty() ? UiText(L"播放器", L"Player") : media.source);
    const auto current = std::find_if(media.sessions.begin(), media.sessions.end(), [&](const MediaSessionChoice& choice) { return choice.id == media.sessionId; });
    if (media.sessions.size() > 1 && current != media.sessions.end())
        tip += L" · " + std::to_wstring(current - media.sessions.begin() + 1) + L"/" + std::to_wstring(media.sessions.size()) +
            UiText(L"\n点击切换播放器", L"\nClick to switch players");
    else tip += UiText(L"\n当前只有一个播放器", L"\nOne player available");
    if (!media.appError.empty()) tip += L"\n" + media.appError;
    const auto button = view_.MediaSourceButton(); const UINT dpi = GetDpiForWindow(window_);
    tool.rect = {ToPixels(button.left, dpi), ToPixels(button.top, dpi), ToPixels(button.right, dpi), ToPixels(button.bottom, dpi)};
    if (!sourceTipAdded_ || tip != sourceTip_)
    {
        sourceTip_ = std::move(tip); tool.lpszText = sourceTip_.data();
        SendMessageW(sourceTooltip_, sourceTipAdded_ ? TTM_UPDATETIPTEXTW : TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        sourceTipAdded_ = true;
    }
    if (!EqualRect(&tool.rect, &sourceTipRect_))
    {
        sourceTipRect_ = tool.rect;
        SendMessageW(sourceTooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
    }
}

void IslandWindow::SetNativeGlass(bool enabled)
{
    backdrop_.SetGlass(enabled);
    view_.SetGlassOptions(enabled, backdrop_.GlassAvailable(), backdrop_.GlassActive(), backdrop_.Status());
    Render();
}

void IslandWindow::MediaChanged(const MediaSnapshot& snapshot)
{
    if (!mediaLaunchPending_) return;
    if (snapshot.sessionId == mediaLaunchSession_ && snapshot.appRequest == mediaLaunchRequest_) return;
    if ((snapshot.sessionId != mediaLaunchSession_ || !snapshot.appError.empty()) &&
        GetForegroundWindow() == window_ && IsWindow(mediaPreviousFocus_))
        SetForegroundWindow(mediaPreviousFocus_);
    mediaLaunchPending_ = false;
}

void IslandWindow::RethrowWindowError() const
{
    if (error_) std::rethrow_exception(error_);
}

HMONITOR IslandWindow::PreferredMonitor() const
{
    if (!preferences_.monitorDevice.empty())
    {
        std::vector<HMONITOR> monitors;
        CheckWin32(EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&monitors)), "EnumDisplayMonitors");
        for (const auto monitor : monitors)
            if (_wcsicmp(MonitorInfo(monitor).szDevice, preferences_.monitorDevice.c_str()) == 0) return monitor;
    }
    return MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY);
}

void IslandWindow::ResizeRendererIfNeeded()
{
    RECT client{}; CheckWin32(GetClientRect(window_, &client), "GetClientRect");
    const SIZE size{client.right - client.left, client.bottom - client.top};
    const UINT dpi = GetDpiForWindow(window_);
    if (!dpi) ThrowLastError("GetDpiForWindow");
    if (size.cx == rendererSize_.cx && size.cy == rendererSize_.cy && dpi == rendererDpi_) return;
    renderer_.Resize(size, dpi);
    rendererSize_ = size;
    rendererDpi_ = dpi;
}

void IslandWindow::PositionOnMonitor(HMONITOR monitor)
{
    if (positioning_ || !window_) return;
    if (!monitor) monitor = PreferredMonitor();
    MONITORINFOEXW info{}; info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info)))
    {
        monitor = MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY);
        info = MonitorInfo(monitor);
    }
    positioning_ = true;
    try
    {
        // Moving to another monitor updates GetDpiForWindow synchronously.
        // Ignore its nested WM_DPICHANGED until this layout is complete.
        if (monitor_ != monitor)
            CheckWin32(SetWindowPos(window_, HWND_TOPMOST, info.rcMonitor.left, info.rcMonitor.top,
                0, 0, SWP_NOSIZE | SWP_NOACTIVATE), "SetWindowPos (monitor)");
        monitor_ = monitor;
        const UINT dpi = GetDpiForWindow(window_);
        if (!dpi) ThrowLastError("GetDpiForWindow");
        const auto& layout = view_.Layout();
        const SIZE size{ToPixels(layout.hostWidth, dpi), ToPixels(layout.hostHeight, dpi)};
        LONG x = info.rcMonitor.left + (info.rcMonitor.right - info.rcMonitor.left - size.cx) / 2 + ToPixels(preferences_.layout.offsetX, dpi);
        LONG y = info.rcMonitor.top + ToPixels(layout.topGap + preferences_.layout.offsetY, dpi);
        ClampIslandPosition(x, y, info.rcWork, view_, dpi);
        RECT current{}; CheckWin32(GetWindowRect(window_, &current), "GetWindowRect");
        if (current.left != x || current.top != y || current.right - current.left != size.cx || current.bottom - current.top != size.cy)
            CheckWin32(SetWindowPos(window_, HWND_TOPMOST, x, y, size.cx, size.cy, SWP_NOACTIVATE), "SetWindowPos (layout)");
        ResizeRendererIfNeeded();
        Render();
        positioning_ = false;
    }
    catch (...) { positioning_ = false; throw; }
}

void IslandWindow::BeginRightPress()
{
    if (!preferences_.rightDrag || !Visible()) return;
    POINT point{}; CheckWin32(GetCursorPos(&point), "GetCursorPos");
    POINT client = point; CheckWin32(ScreenToClient(window_, &client), "ScreenToClient");
    if (!renderer_.HitTest(client)) return;
    RECT host{}; CheckWin32(GetWindowRect(window_, &host), "GetWindowRect");
    const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96;
    rightButtonDown_ = true;
    rightDragging_ = rightPressCancelled_ = false;
    rightPressTime_ = GetTickCount64();
    rightPressPoint_ = point;
    dragAnchorX_ = (point.x - host.left) / scale;
    dragAnchorY_ = (point.y - host.top) / scale;
    SetCapture(window_);
    if (!SetTimer(window_, RightHoldTimer, RightHoldMilliseconds, nullptr))
    {
        const DWORD error = GetLastError();
        FinishRightPress(true);
        SetLastError(error);
        ThrowLastError("SetTimer (right hold)");
    }
}

void IslandWindow::UpdateRightDrag()
{
    if (!rightButtonDown_ || !window_) return;
    POINT point{}; CheckWin32(GetCursorPos(&point), "GetCursorPos");
    if (!rightDragging_)
    {
        if (GetTickCount64() - rightPressTime_ < RightHoldMilliseconds) return;
        const UINT dpi = GetDpiForWindow(window_);
        if (std::abs(point.x - rightPressPoint_.x) < GetSystemMetricsForDpi(SM_CXDRAG, dpi) &&
            std::abs(point.y - rightPressPoint_.y) < GetSystemMetricsForDpi(SM_CYDRAG, dpi)) return;
        rightDragging_ = true;
        KillTimer(window_, RightHoldTimer);
        BeginMenu(); // Pause idle timeout and outside-click tracking only while moving.
    }
    if (positioning_) return;
    positioning_ = true;
    try
    {
        const UINT previousDpi = GetDpiForWindow(window_);
        LONG x = point.x - ToPixels(dragAnchorX_, previousDpi);
        LONG y = point.y - ToPixels(dragAnchorY_, previousDpi);
        CheckWin32(SetWindowPos(window_, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE), "Move island");
        const UINT dpi = GetDpiForWindow(window_);
        const auto& layout = view_.Layout();
        const SIZE size{ToPixels(layout.hostWidth, dpi), ToPixels(layout.hostHeight, dpi)};
        x = point.x - ToPixels(dragAnchorX_, dpi);
        y = point.y - ToPixels(dragAnchorY_, dpi);
        monitor_ = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
        ClampIslandPosition(x, y, MonitorInfo(monitor_).rcWork, view_, dpi);
        CheckWin32(SetWindowPos(window_, HWND_TOPMOST, x, y, size.cx, size.cy, SWP_NOACTIVATE), "Move island (layout)");
        ResizeRendererIfNeeded();
        Render();
        positioning_ = false;
    }
    catch (...) { positioning_ = false; throw; }
}

bool IslandWindow::FinishRightPress(bool cancelled)
{
    if (!rightButtonDown_) return false;
    const bool moved = rightDragging_;
    rightButtonDown_ = rightDragging_ = false;
    rightPressCancelled_ = cancelled;
    KillTimer(window_, RightHoldTimer);
    if (GetCapture() == window_) ReleaseCapture();
    if (moved && cancelled) PositionOnMonitor(PreferredMonitor());
    if (moved) EndMenu();
    if (moved && !cancelled)
    {
        RECT host{}; CheckWin32(GetWindowRect(window_, &host), "GetWindowRect");
        const auto info = MonitorInfo(monitor_);
        const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96;
        preferences_.layout.offsetX = (host.left - (info.rcMonitor.left +
            (info.rcMonitor.right - info.rcMonitor.left - (host.right - host.left)) / 2)) / scale;
        preferences_.layout.offsetY = (host.top - info.rcMonitor.top) / scale - view_.Layout().topGap;
        preferences_.monitorDevice = info.szDevice;
        if (positionHandler_) positionHandler_(preferences_.layout.offsetX, preferences_.layout.offsetY, preferences_.monitorDevice);
    }
    SyncPointer();
    return moved;
}

void IslandWindow::ResetPointer()
{
    if (pointerTracking_ && window_)
    {
        TRACKMOUSEEVENT tracking{ sizeof(TRACKMOUSEEVENT), TME_CANCEL | TME_LEAVE, window_, 0 };
        TrackMouseEvent(&tracking);
    }
    pointerTracking_ = false;
    lineClick_ = false;
    pressAction_ = ViewAction::None;
    pressNoticeRevision_ = 0;
    if (pointerInside_)
    {
        pointerInside_ = false;
        if (handler_) handler_(WindowEvent::PointerLeave);
    }
}

void IslandWindow::SyncPointer(bool armTracking)
{
    if (syncingPointer_ || !window_) return;
    syncingPointer_ = true;
    try
    {
        // Pointer events can render synchronously. Recheck after that render,
        // while the guard prevents recursively entering this synchronization.
        for (unsigned pass = 0; pass < 3; ++pass)
        {
            POINT point{};
            const bool cursorAvailable = GetCursorPos(&point) != FALSE;
            const bool inside = !menuActive_ && Visible() && cursorAvailable &&
                ScreenToClient(window_, &point) && renderer_.HitTest(point);
            if (inside && armTracking && !pointerTracking_)
            {
                TRACKMOUSEEVENT tracking{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, window_, 0 };
                CheckWin32(TrackMouseEvent(&tracking), "TrackMouseEvent");
                pointerTracking_ = true;
            }
            // A toast may restore the line under a stationary cursor. Notify
            // again only when the shape has actually shrunk to the line.
            if (inside == pointerInside_ && !(inside && view_.Line() &&
                view_.Frame().height <= IslandView::LineHeight + 2.0f)) break;
            pointerInside_ = inside;
            if (handler_) handler_(inside ? WindowEvent::PointerEnter : WindowEvent::PointerLeave);
        }
        syncingPointer_ = false;
    }
    catch (...) { syncingPointer_ = false; throw; }
}

void IslandWindow::BeginMenu()
{
    if (++menuDepth_ != 1) return;
    menuActive_ = true;
    if (handler_) handler_(WindowEvent::MenuOpened);
    ResetPointer();
    SyncMediaInteraction();
}

void IslandWindow::EndMenu()
{
    if (!menuDepth_ || --menuDepth_) return;
    menuActive_ = false;
    if (handler_) handler_(WindowEvent::MenuClosed);
    SyncPointer();
    SyncMediaInteraction();
}

void IslandWindow::ToggleVisible()
{
    visibility_.UserToggle();
    ApplyVisibility();
}

void IslandWindow::EnsureVisible()
{
    visibility_.UserShow();
    ApplyVisibility();
}

void IslandWindow::SetFullscreenHidden(bool value)
{
    visibility_.SetFullscreen(value);
    ApplyVisibility();
}

void IslandWindow::ApplyVisibility()
{
    if (!window_ || visibility_.ShouldShow() == Visible()) return;
    if (!visibility_.ShouldShow() && rightButtonDown_) FinishRightPress(true);
    ResetPointer();
    if (visibility_.ShouldShow())
    {
        PositionOnMonitor(monitor_);
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        backdrop_.Sync(window_, view_);
        if (handler_) handler_(WindowEvent::WindowShown);
    }
    else
    {
        backdrop_.Hide();
        ShowWindow(window_, SW_HIDE);
        if (handler_) handler_(WindowEvent::WindowHidden);
    }
    SyncMediaInteraction();
    SyncPointer();
}

void IslandWindow::ShowFromTray(bool expand)
{
    if (expand && !view_.Media().available) return;
    EnsureVisible();
    if (expand && !view_.Expanded() && handler_) handler_(WindowEvent::TogglePanel);
}

BOOL CALLBACK IslandWindow::CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) noexcept
{
    // EnumDisplayMonitors 的回调也不能让 C++ 异常越过系统边界。
    try { reinterpret_cast<std::vector<HMONITOR>*>(data)->push_back(monitor); return TRUE; }
    catch (...) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
}

void IslandWindow::ShowTrayMenu(POINT point)
{
    Menu menu(CreatePopupMenu());
    if (!menu) ThrowLastError("CreatePopupMenu (tray)");
    CheckWin32(AppendMenuW(menu.get(), MF_STRING, TrayToggle, Visible() ?
        UiText(L"隐藏灵动岛", L"Hide island") : UiText(L"显示灵动岛", L"Show island")), "AppendMenuW");
    CheckWin32(AppendMenuW(menu.get(), MF_STRING | (view_.Media().available ? 0 : MF_GRAYED),
        TrayExpand, UiText(L"展开播放器", L"Open player")), "AppendMenuW");
    CheckWin32(AppendMenuW(menu.get(), MF_STRING, TraySettings, UiText(L"设置", L"Settings")), "AppendMenuW");
    CheckWin32(AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr), "AppendMenuW");
    CheckWin32(AppendMenuW(menu.get(), MF_STRING, TrayExit, UiText(L"退出", L"Exit")), "AppendMenuW");
    UINT command = 0;
    {
        // A hidden NOACTIVATE island cannot be the foreground menu owner.
        // This empty, zero-size tool window exists only for the user's tray menu.
        MenuOwner owner(CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"WinHalo tray menu", WS_POPUP,
            point.x, point.y, 0, 0, nullptr, nullptr, instance_, nullptr));
        if (!owner) ThrowLastError("CreateWindowExW (tray menu)");
        const HWND previousForeground = GetForegroundWindow();
        ShowWindow(owner.get(), SW_SHOWNOACTIVATE);
        SetForegroundWindow(owner.get());
        // The temporary owner receives its own menu-loop messages, so explicitly
        // pause the island rather than relying on the island's WindowProc.
        BeginMenu();
        try
        {
            command = static_cast<UINT>(TrackPopupMenuEx(menu.get(),
                TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, point.x, point.y, owner.get(), nullptr));
        }
        catch (...) { EndMenu(); throw; }
        // Required by the shell menu contract so a second menu dismisses normally.
        PostMessageW(owner.get(), WM_NULL, 0, 0);
        if (GetForegroundWindow() == owner.get() && IsWindow(previousForeground))
            SetForegroundWindow(previousForeground);
        EndMenu();
    }
    if (!window_) return; // A quit hotkey can run inside TrackPopupMenu's message loop.
    switch (command)
    {
    case TrayToggle: ToggleVisible(); break;
    case TrayExpand: ShowFromTray(true); break;
    case TraySettings:
        if (handler_) handler_(WindowEvent::Settings);
        break;
    case TrayExit: CheckWin32(DestroyWindow(window_), "DestroyWindow"); break;
    default: break;
    }
}

LRESULT CALLBACK IslandWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
    auto* self = reinterpret_cast<IslandWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        self = static_cast<IslandWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->window_ = window;
        SetLastError(ERROR_SUCCESS);
        if (!SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)) && GetLastError())
            return FALSE;
    }
    if (!self) return DefWindowProcW(window, message, wParam, lParam);
    try
    {
        const auto result = self->ProcessMessage(message, wParam, lParam);
        if (message == WM_NCDESTROY)
        {
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            self->window_ = nullptr;
        }
        return result;
    }
    catch (...)
    {
        self->error_ = std::current_exception();
        self->logger_.Write("Window operation failed; terminating safely");
        // 退出在下一轮消息循环处理，避免在嵌套系统回调中销毁窗口。
        PostMessageW(window, WM_CLOSE, 0, 0);
        PostQuitMessage(1);
        return 0;
    }
}

LRESULT IslandWindow::ProcessMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    if (taskbarCreatedMessage_ && message == taskbarCreatedMessage_)
    {
        if (!tray_.Restore())
        {
            logger_.Write("System tray registration failed after TaskbarCreated");
            // Never leave a hidden island without a working restore entry point.
            if (!visibilityHotkey_) EnsureVisible();
        }
        return 0;
    }
    switch (message)
    {
    case OutsideClickMessage:
        if (!menuActive_ && handler_) handler_(WindowEvent::OutsideClick);
        return 0;
    case WM_ENTERMENULOOP:
        BeginMenu(); return 0;
    case WM_EXITMENULOOP:
        EndMenu(); return 0;
    case DesktopMessage:
        if (handler_) handler_(WindowEvent::DesktopChanged);
        return 0;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMPOWERSTATUSCHANGE || wParam == PBT_POWERSETTINGCHANGE ||
            wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND)
        {
            if (handler_) handler_(WindowEvent::PowerChanged);
        }
        return TRUE;
    case ServiceMessage:
        if (serviceHandler_) serviceHandler_();
        return 0;
    case WM_CLIPBOARDUPDATE:
        if (clipboardListening_ && preferences_.clipboardLinks && clipboardHandler_) clipboardHandler_();
        return 0;
    case TrayIcon::CallbackMessage:
        if (HIWORD(lParam) != TrayIcon::Identifier) return 0;
        switch (LOWORD(lParam))
        {
        case NIN_SELECT:
        case NIN_KEYSELECT: ToggleVisible(); break;
        case WM_CONTEXTMENU:
        {
            POINT point{ GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam) };
            if (point.x == -1 && point.y == -1) GetCursorPos(&point);
            ShowTrayMenu(point);
            break;
        }
        default: break;
        }
        return 0;
    case WM_NCHITTEST:
    {
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        CheckWin32(ScreenToClient(window_, &point), "ScreenToClient");
        return renderer_.HitTest(point) ? HTCLIENT : HTTRANSPARENT;
    }
    case WM_MOUSEACTIVATE:
    {
        // Only explicitly opening software grants activation. Playback and source controls
        // keep the original app's keyboard focus, as does opening the collapsed capsule.
        POINT point{}; GetCursorPos(&point); ScreenToClient(window_, &point);
        const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96;
        const auto& app = view_.Media().app;
        if (HIWORD(lParam) == WM_LBUTTONDOWN && view_.Expanded() && view_.Page() == IslandPage::Media && app &&
            (!app->executable.empty() || app->shellRegistered) &&
            view_.ActionAt(point.x / scale, point.y / scale) == ViewAction::OpenMediaApp)
        {
            mediaPreviousFocus_ = GetForegroundWindow();
            mediaLaunchSession_ = view_.Media().sessionId; mediaLaunchRequest_ = view_.Media().appRequest;
            mediaLaunchPending_ = true;
            return MA_ACTIVATE;
        }
        return MA_NOACTIVATE;
    }
    case WM_MOUSEMOVE:
        if (rightButtonDown_) UpdateRightDrag();
        SyncPointer();
        return 0;
    case WM_MOUSELEAVE:
        pointerTracking_ = false;
        // Re-registering from a leave notification can immediately generate
        // another leave while native cursor routing is still being updated.
        SyncPointer(false);
        return 0;
    case WM_LBUTTONDOWN:
    {
        // A click that starts on the line only reveals the capsule. A later
        // click after hover has revealed it follows the normal expansion path.
        lineClick_ = view_.Line() || view_.Frame().height < 18.0f;
        const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96;
        pressAction_ = view_.ActionAt(static_cast<float>(GET_X_LPARAM(lParam)) / scale,
            static_cast<float>(GET_Y_LPARAM(lParam)) / scale);
        pressNoticeRevision_ = view_.NoticeRevision();
        return 0;
    }
    case WM_LBUTTONUP:
    {
        const UINT dpi = GetDpiForWindow(window_);
        const float scale = static_cast<float>(dpi) / 96;
        clickAction_ = lineClick_ ? ViewAction::RevealCapsule : view_.ActionAt(
            static_cast<float>(GET_X_LPARAM(lParam)) / scale, static_cast<float>(GET_Y_LPARAM(lParam)) / scale);
        // A clipboard update may replace the island while a mouse button is
        // held. Opening a link requires a complete click on that same notice.
        if (clickAction_ == ViewAction::OpenLink && (pressAction_ != ViewAction::OpenLink ||
            pressNoticeRevision_ != view_.NoticeRevision())) clickAction_ = ViewAction::None;
        lineClick_ = false;
        pressAction_ = ViewAction::None;
        pressNoticeRevision_ = 0;
        if (handler_) handler_(WindowEvent::Click);
        return 0;
    }
    case WM_RBUTTONDOWN:
        rightPressCancelled_ = false;
        BeginRightPress();
        return 0;
    case WM_RBUTTONUP:
    {
        const bool cancelled = rightPressCancelled_;
        const bool moved = FinishRightPress(false);
        if (moved || cancelled) return 0;
        // DefWindowProc produces WM_CONTEXTMENU for a short right click.
        return DefWindowProcW(window_, message, wParam, lParam);
    }
    case WM_TIMER:
        if (wParam == RightHoldTimer)
        {
            KillTimer(window_, RightHoldTimer);
            UpdateRightDrag();
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_CANCELMODE:
        FinishRightPress(true);
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_CAPTURECHANGED:
        if (rightButtonDown_ && reinterpret_cast<HWND>(lParam) != window_) FinishRightPress(true);
        return 0;
    case WM_MOUSEWHEEL:
    {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        CheckWin32(ScreenToClient(window_, &point), "ScreenToClient");
        if (!preferences_.systemControls || !Visible() || !renderer_.HitTest(point))
            return DefWindowProcW(window_, message, wParam, lParam);
        const bool shift = (GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) != 0;
        if (shift != wheelShift_) wheelRemainder_ = 0;
        wheelShift_ = shift;
        wheelRemainder_ += GET_WHEEL_DELTA_WPARAM(wParam);
        const int steps = wheelRemainder_ / WHEEL_DELTA;
        wheelRemainder_ %= WHEEL_DELTA;
        if (steps && wheelHandler_) wheelHandler_(steps, shift);
        return 0;
    }
    case WM_CONTEXTMENU:
    {
        if (!rightDragging_ && handler_) handler_(WindowEvent::Settings);
        return 0;
    }
    case WM_HOTKEY:
        if (wParam == VisibilityHotkey) ToggleVisible();
        else if (wParam == QuitHotkey) CheckWin32(DestroyWindow(window_), "DestroyWindow");
        else if (wParam == PanelHotkey && handler_) handler_(WindowEvent::TogglePanel);
        return 0;
    case WM_DPICHANGED:
        if (!positioning_)
        {
            PositionOnMonitor(PreferredMonitor());
            if (handler_) handler_(WindowEvent::DesktopChanged);
        }
        return 0;
    case WM_DISPLAYCHANGE:
        PositionOnMonitor(PreferredMonitor());
        if (handler_) handler_(WindowEvent::DesktopChanged);
        return 0;
    case WM_SETTINGCHANGE:
        if (!positioning_ && !rightDragging_) PositionOnMonitor(PreferredMonitor());
        if (handler_) handler_(WindowEvent::DesktopChanged);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        BeginPaint(window_, &paint);
        EndPaint(window_, &paint);
        return 0; // DirectComposition 内容由 DWM 保存，不在此重复提交。
    }
    case WM_CLOSE: CheckWin32(DestroyWindow(window_), "DestroyWindow"); return 0;
    case WM_DESTROY:
        KillTimer(window_, RightHoldTimer);
        if (clipboardListening_) RemoveClipboardFormatListener(window_);
        clipboardListening_ = false;
        backdrop_.Hide();
        tray_.Remove();
        if (visibilityHotkey_) UnregisterHotKey(window_, VisibilityHotkey);
        if (quitHotkey_) UnregisterHotKey(window_, QuitHotkey);
        if (panelHotkey_) UnregisterHotKey(window_, PanelHotkey);
        visibilityHotkey_ = quitHotkey_ = panelHotkey_ = false;
        PostQuitMessage(error_ ? 1 : 0);
        return 0;
    default: return DefWindowProcW(window_, message, wParam, lParam);
    }
}
}
