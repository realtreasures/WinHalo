#include "IslandWindow.h"
#include "../Utils/Win32Error.h"
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
enum MenuCommand : UINT { Center = 1001, NextMonitor, Hide, About, Exit,
    TogglePanel, Settings, Performance, StressTest, DebugSize, RecreateDevice };
enum TrayCommand : UINT { TrayToggle = 1101, TrayExpand, TraySettings, TrayExit };
struct MenuDeleter { void operator()(HMENU menu) const noexcept { if (menu) DestroyMenu(menu); } };
using Menu = std::unique_ptr<std::remove_pointer_t<HMENU>, MenuDeleter>;
struct WindowDeleter { void operator()(HWND window) const noexcept { if (window) DestroyWindow(window); } };
using MenuOwner = std::unique_ptr<std::remove_pointer_t<HWND>, WindowDeleter>;
LONG ToPixels(float dip, UINT dpi)
{
    return static_cast<LONG>(std::lround(dip * static_cast<float>(dpi) / 96.0f));
}
}

IslandWindow::IslandWindow(HINSTANCE instance, IRenderer& renderer, IslandView& view, Logger& logger)
    : instance_(instance), renderer_(renderer), view_(view), logger_(logger) {}

IslandWindow::~IslandWindow()
{
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
    PositionOnMonitor(MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY));
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
    if (!panelHotkey_) logger_.Write("Ctrl+Alt+Space is unavailable; use the capsule or context menu");
    if (show) ApplyVisibility();
    else if (handler_) handler_(WindowEvent::WindowHidden);
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_WIN95_CLASSES}; InitCommonControlsEx(&common);
    sourceTooltip_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, instance_, nullptr);
    if (sourceTooltip_) SendMessageW(sourceTooltip_, TTM_SETMAXTIPWIDTH, 0, 300);
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
    std::wstring tip = media.app ? media.app->name : (media.source.empty() ? L"播放器" : media.source);
    const auto current = std::find_if(media.sessions.begin(), media.sessions.end(), [&](const MediaSessionChoice& choice) { return choice.id == media.sessionId; });
    if (media.sessions.size() > 1 && current != media.sessions.end())
        tip += L" · " + std::to_wstring(current - media.sessions.begin() + 1) + L"/" + std::to_wstring(media.sessions.size()) + L"\n点击切换播放器";
    else tip += L"\n当前只有一个播放器";
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

void IslandWindow::PositionOnMonitor(HMONITOR monitor)
{
    if (positioning_ || !window_) return;
    MONITORINFO info{ sizeof(MONITORINFO) };
    if (!monitor || !GetMonitorInfoW(monitor, &info))
    {
        monitor = MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY);
        CheckWin32(GetMonitorInfoW(monitor, &info), "GetMonitorInfoW");
    }
    monitor_ = monitor;
    // 先把窗口移到目标屏幕，GetDpiForWindow 随即获得该屏幕 DPI。
    // WM_DPICHANGED 在 SetWindowPos 内同步触发，防止嵌套定位。
    positioning_ = true;
    try
    {
        CheckWin32(SetWindowPos(window_, HWND_TOPMOST, info.rcMonitor.left, info.rcMonitor.top,
            0, 0, SWP_NOSIZE | SWP_NOACTIVATE), "SetWindowPos (monitor)");
        const UINT dpi = GetDpiForWindow(window_);
        if (!dpi) ThrowLastError("GetDpiForWindow");
        const auto& layout = view_.Layout();
        const SIZE size{ ToPixels(layout.hostWidth, dpi), ToPixels(layout.hostHeight, dpi) };
        const LONG x = info.rcMonitor.left + (info.rcMonitor.right - info.rcMonitor.left - size.cx) / 2;
        const LONG y = info.rcMonitor.top + ToPixels(layout.topGap, dpi);
        CheckWin32(SetWindowPos(window_, HWND_TOPMOST, x, y, size.cx, size.cy, SWP_NOACTIVATE), "SetWindowPos (layout)");
        renderer_.Resize(size, dpi);
        Render();
        positioning_ = false;
    }
    catch (...) { positioning_ = false; throw; }
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

void IslandWindow::ShowContextMenu(POINT point)
{
    Menu menu(CreatePopupMenu());
    if (!menu) ThrowLastError("CreatePopupMenu");
    const auto add = [&](UINT flags, UINT command, const wchar_t* text)
    {
        CheckWin32(AppendMenuW(menu.get(), flags, command, text), "AppendMenuW");
    };
    add(MF_STRING, Center, L"重新居中");
    add(MF_STRING, NextMonitor, L"移到下一显示器");
    add(MF_STRING | ((visibilityHotkey_ || tray_.Registered()) ? 0 : MF_GRAYED), Hide, L"隐藏\tCtrl+Alt+I");
    add(MF_SEPARATOR, 0, nullptr);
    add(MF_STRING, About, L"关于 WinHalo");
    add(MF_STRING, Exit, L"退出\tCtrl+Alt+Q");
    add(MF_SEPARATOR, 0, nullptr);
    add(MF_STRING | ((view_.Expanded() || view_.Media().available) ? 0 : MF_GRAYED),
        TogglePanel, view_.Expanded() ? L"收起" : L"展开播放器");
    add(MF_STRING, Settings, L"设置");
#ifdef _DEBUG
    add(MF_SEPARATOR, 0, nullptr);
    add(MF_STRING, Performance, L"性能数据");
    add(MF_STRING | (view_.Media().available ? 0 : MF_GRAYED), StressTest, L"连续展开 / 收起测试");
    add(MF_STRING | (view_.Media().available ? 0 : MF_GRAYED), DebugSize, L"切换测试尺寸");
    add(MF_STRING, RecreateDevice, L"重建图形设备");
#endif
    // 菜单自身接受输入；窗口仍保留 NOACTIVATE，不主动抢占其他应用焦点。
    BeginMenu();
    UINT command = 0;
    try
    {
        command = static_cast<UINT>(TrackPopupMenuEx(menu.get(),
            TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, point.x, point.y, window_, nullptr));
    }
    catch (...) { EndMenu(); throw; }
    EndMenu();
    switch (command)
    {
    case TogglePanel:
        if ((view_.Expanded() || view_.Media().available) && handler_) handler_(WindowEvent::TogglePanel);
        break;
    case Settings: if (handler_) handler_(WindowEvent::Settings); break;
    case StressTest: if (view_.Media().available && handler_) handler_(WindowEvent::StressTest); break;
    case DebugSize: if (view_.Media().available && handler_) handler_(WindowEvent::DebugSize); break;
    case RecreateDevice: if (handler_) handler_(WindowEvent::RecreateDevice); break;
    case Performance:
        if (diagnostics_) MessageBoxW(window_, diagnostics_().c_str(), L"WinHalo 性能数据", MB_OK);
        break;
    case Center:
        PositionOnMonitor(monitor_);
        if (handler_) handler_(WindowEvent::DesktopChanged);
        break;
    case NextMonitor:
    {
        std::vector<HMONITOR> monitors;
        CheckWin32(EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&monitors)), "EnumDisplayMonitors");
        if (!monitors.empty())
        {
            auto next = std::find(monitors.begin(), monitors.end(), monitor_);
            if (next == monitors.end() || ++next == monitors.end()) next = monitors.begin();
            ResetPointer();
            PositionOnMonitor(*next);
            if (handler_) handler_(WindowEvent::DesktopChanged);
        }
        break;
    }
    case Hide: if (visibilityHotkey_ || tray_.Registered()) ToggleVisible(); break;
    case About:
        MessageBoxW(window_, L"WinHalo\n原生 Windows 灵动岛\n\n"
            L"触碰顶部横线唤出胶囊。\n"
            L"无媒体时显示时间；有媒体时单击胶囊展开播放器。\n"
            L"移开鼠标 5 秒后胶囊收回横线。\n"
            L"在设置中调整动画速度或减少动态效果。",
            L"关于 WinHalo", MB_OK | MB_ICONINFORMATION);
        break;
    case Exit: CheckWin32(DestroyWindow(window_), "DestroyWindow"); break;
    default: break; // 用户取消菜单。
    }
}

void IslandWindow::ShowTrayMenu(POINT point)
{
    Menu menu(CreatePopupMenu());
    if (!menu) ThrowLastError("CreatePopupMenu (tray)");
    CheckWin32(AppendMenuW(menu.get(), MF_STRING, TrayToggle, Visible() ? L"隐藏灵动岛" : L"显示灵动岛"), "AppendMenuW");
    CheckWin32(AppendMenuW(menu.get(), MF_STRING | (view_.Media().available ? 0 : MF_GRAYED),
        TrayExpand, L"展开播放器"), "AppendMenuW");
    CheckWin32(AppendMenuW(menu.get(), MF_STRING, TraySettings, L"设置"), "AppendMenuW");
    CheckWin32(AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr), "AppendMenuW");
    CheckWin32(AppendMenuW(menu.get(), MF_STRING, TrayExit, L"退出"), "AppendMenuW");
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
        EnsureVisible();
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
        SyncPointer();
        return 0;
    case WM_MOUSELEAVE:
        pointerTracking_ = false;
        // Re-registering from a leave notification can immediately generate
        // another leave while native cursor routing is still being updated.
        SyncPointer(false);
        return 0;
    case WM_LBUTTONDOWN:
        // A click that starts on the line only reveals the capsule. A later
        // click after hover has revealed it follows the normal expansion path.
        lineClick_ = view_.Line() || view_.Frame().height < 18.0f;
        return 0;
    case WM_LBUTTONUP:
    {
        const UINT dpi = GetDpiForWindow(window_);
        const float scale = static_cast<float>(dpi) / 96;
        clickAction_ = lineClick_ ? ViewAction::RevealCapsule : view_.ActionAt(
            static_cast<float>(GET_X_LPARAM(lParam)) / scale, static_cast<float>(GET_Y_LPARAM(lParam)) / scale);
        lineClick_ = false;
        if (handler_) handler_(WindowEvent::Click);
        return 0;
    }
    case WM_CONTEXTMENU:
    {
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (point.x == -1 && point.y == -1)
        {
            const auto& layout = view_.Layout();
            const UINT dpi = GetDpiForWindow(window_);
            point = { ToPixels(layout.hostWidth / 2, dpi), ToPixels(layout.insetTop + layout.height, dpi) };
            CheckWin32(ClientToScreen(window_, &point), "ClientToScreen");
        }
        ShowContextMenu(point);
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
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            PositionOnMonitor(MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST));
            if (handler_) handler_(WindowEvent::DesktopChanged);
        }
        return 0;
    case WM_DISPLAYCHANGE:
        PositionOnMonitor(MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY));
        if (handler_) handler_(WindowEvent::DesktopChanged);
        return 0;
    case WM_SETTINGCHANGE:
        if (!positioning_) PositionOnMonitor(monitor_);
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
