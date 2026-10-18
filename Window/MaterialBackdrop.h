#pragma once
#include "../Core/Platform.h"
#include "../UI/IslandView.h"
#include <string>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

namespace winisland
{
class MaterialBackdrop
{
public:
    MaterialBackdrop() = default;
    MaterialBackdrop(const MaterialBackdrop&) = delete;
    MaterialBackdrop& operator=(const MaterialBackdrop&) = delete;
    ~MaterialBackdrop();
    void Initialize(HWND main);
    bool SetGlass(bool enabled);
    bool GlassAvailable() const noexcept { return glassAvailable_; }
    bool GlassActive() const noexcept { return glassActive_; }
    const std::wstring& Status() const noexcept { return status_; }
    void Sync(HWND main, const IslandView& view);
    void Hide() noexcept;
private:
    static LRESULT CALLBACK GlassProc(HWND window, UINT message, WPARAM wp, LPARAM lp) noexcept;
    void ReleaseSurface() noexcept;
    void CreateGlassResources();
    HINSTANCE instance_ = nullptr;
    bool classRegistered_ = false;
    winrt::Windows::System::DispatcherQueueController queue_{nullptr};
    winrt::Windows::UI::Composition::Compositor compositor_{nullptr};
    winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget target_{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual glassVisual_{nullptr};
    winrt::Windows::UI::Composition::CompositionRoundedRectangleGeometry glassGeometry_{nullptr};
    HWND shadow_ = nullptr, glass_ = nullptr;
    HDC memory_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ oldBitmap_ = nullptr;
    unsigned char* pixels_ = nullptr;
    SIZE size_{};
    RECT lastBounds_{};
    float lastRadius_ = -1;
    UINT lastDpi_ = 0;
    bool glassAvailable_ = false, glassActive_ = false;
    std::wstring status_ = L"默认使用深色半透明材质";
};
}
