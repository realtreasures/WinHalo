#pragma once
#include "../Graphics/IRenderer.h"
#include "../Utils/Logger.h"
#include "TrayIcon.h"
#include "MaterialBackdrop.h"
#include "OutsideClickMonitor.h"
#include "../Core/VisibilityPolicy.h"
#include <exception>
#include <functional>
#include <vector>

namespace winisland
{
class IslandWindow
{
public:
    using EventHandler = std::function<void(WindowEvent)>;
    static constexpr UINT ServiceMessage = WM_APP + 20;
    static constexpr UINT DesktopMessage = WM_APP + 22;
    static constexpr UINT OutsideClickMessage = WM_APP + 23;
    IslandWindow(HINSTANCE instance, IRenderer& renderer, IslandView& view, Logger& logger);
    ~IslandWindow();
    IslandWindow(const IslandWindow&) = delete;
    IslandWindow& operator=(const IslandWindow&) = delete;
    void Create(EventHandler handler, bool show = true);
    void Render();
    void RethrowWindowError() const;
    HWND Handle() const noexcept { return window_; }
    ViewAction ClickAction() const noexcept { return clickAction_; }
    void SetDiagnostics(std::function<std::wstring()> callback) { diagnostics_ = std::move(callback); }
    void SetServiceEventHandler(std::function<void()> callback) { serviceHandler_ = std::move(callback); }
    bool Visible() const noexcept { return window_ && IsWindowVisible(window_); }
    void EnsureVisible();
    void ShowFromTray(bool expand = false);
    void SetFullscreenHidden(bool value);
    bool ManuallyHidden() const noexcept { return visibility_.ManuallyHidden(); }
    void SetNativeGlass(bool enabled);
    void MediaChanged(const MediaSnapshot& snapshot);

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept;
    LRESULT ProcessMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void PositionOnMonitor(HMONITOR monitor);
    void ShowContextMenu(POINT screenPoint);
    void ShowTrayMenu(POINT screenPoint);
    void ToggleVisible();
    void ApplyVisibility();
    void ResetPointer();
    void SyncPointer(bool armTracking = true);
    void BeginMenu();
    void EndMenu();
    void SyncMediaInteraction();
    static BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) noexcept;

    HINSTANCE instance_;
    IRenderer& renderer_;
    IslandView& view_;
    Logger& logger_;
    EventHandler handler_;
    std::function<std::wstring()> diagnostics_;
    std::function<void()> serviceHandler_;
    TrayIcon tray_;
    VisibilityPolicy visibility_;
    MaterialBackdrop backdrop_;
    OutsideClickMonitor outsideClicks_;
    HWND sourceTooltip_ = nullptr;
    std::wstring sourceTip_;
    RECT sourceTipRect_{};
    bool sourceTipAdded_ = false;
    bool menuActive_ = false;
    unsigned menuDepth_ = 0;
    bool outsideHookFailed_ = false;
    HWND mediaPreviousFocus_ = nullptr;
    std::uint64_t mediaLaunchSession_ = 0, mediaLaunchRequest_ = 0;
    bool mediaLaunchPending_ = false;
    ViewAction clickAction_ = ViewAction::Toggle;
    HWND window_ = nullptr;
    HMONITOR monitor_ = nullptr;
    bool classRegistered_ = false;
    bool visibilityHotkey_ = false;
    bool quitHotkey_ = false;
    bool panelHotkey_ = false;
    bool pointerInside_ = false;
    bool pointerTracking_ = false;
    bool syncingPointer_ = false;
    bool lineClick_ = false;
    bool positioning_ = false;
    UINT taskbarCreatedMessage_ = 0;
    std::exception_ptr error_;
};
}
