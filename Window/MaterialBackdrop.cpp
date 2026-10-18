#include "MaterialBackdrop.h"
#include "../Graphics/MaterialTheme.h"
#include "../Utils/Win32Error.h"
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <DispatcherQueue.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.h>

namespace winisland
{
namespace { constexpr wchar_t GlassClass[] = L"WinHalo.MaterialGlassWindow"; }

LRESULT CALLBACK MaterialBackdrop::GlassProc(HWND window, UINT message, WPARAM wp, LPARAM lp) noexcept
{
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT)
    {
        PAINTSTRUCT paint{}; BeginPaint(window, &paint);
        EndPaint(window, &paint); return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

MaterialBackdrop::~MaterialBackdrop()
{
    try { if (target_) target_.Root(nullptr); } catch (...) {}
    glassVisual_ = nullptr; glassGeometry_ = nullptr; target_ = nullptr;
    try { if (compositor_) compositor_.Close(); } catch (...) {}
    compositor_ = nullptr;
    try { if (queue_) queue_.ShutdownQueueAsync(); } catch (...) {}
    if (shadow_) DestroyWindow(shadow_);
    if (glass_) DestroyWindow(glass_);
    ReleaseSurface();
    if (classRegistered_) UnregisterClassW(GlassClass, instance_);
}

void MaterialBackdrop::ReleaseSurface() noexcept
{
    if (memory_ && oldBitmap_) SelectObject(memory_, oldBitmap_);
    if (bitmap_) DeleteObject(bitmap_);
    if (memory_) DeleteDC(memory_);
    memory_ = nullptr; bitmap_ = nullptr; oldBitmap_ = nullptr; pixels_ = nullptr;
    size_ = {};
}

void MaterialBackdrop::Initialize(HWND main)
{
    if (shadow_) return;
    instance_ = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(main, GWLP_HINSTANCE));
    // Layered + TRANSPARENT makes the entire shadow pass mouse input across processes.
    shadow_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"STATIC", L"WinHalo material shadow", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance_, nullptr);
    if (!shadow_) ThrowLastError("Create shadow window");
    WNDCLASSW wc{}; wc.hInstance = instance_; wc.lpfnWndProc = GlassProc; wc.lpszClassName = GlassClass;
    classRegistered_ = RegisterClassW(&wc) != 0;
    if (!classRegistered_) { status_ = L"桌面模糊不可用，使用默认材质"; return; }
    glass_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        GlassClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance_, nullptr);
    if (!glass_) { status_ = L"桌面模糊不可用，使用默认材质"; return; }
    const BOOL enable = TRUE;
    if (SUCCEEDED(DwmSetWindowAttribute(glass_, DWMWA_USE_HOSTBACKDROPBRUSH, &enable, sizeof(enable))))
    {
        glassAvailable_ = true;
        status_ = L"仅模糊胶囊内部的桌面背景";
    }
    else status_ = L"此系统不支持桌面模糊，使用默认材质";
}

void MaterialBackdrop::CreateGlassResources()
{
    if (target_) return;
    if (!winrt::Windows::System::DispatcherQueue::GetForCurrentThread())
    {
        const DispatcherQueueOptions options{sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
        winrt::check_hresult(CreateDispatcherQueueController(options,
            reinterpret_cast<PDISPATCHERQUEUECONTROLLER*>(winrt::put_abi(queue_))));
    }
    compositor_ = winrt::Windows::UI::Composition::Compositor();
    const auto interop = compositor_.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
    winrt::check_hresult(interop->CreateDesktopWindowTarget(glass_, TRUE,
        reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(winrt::put_abi(target_))));
    glassVisual_ = compositor_.CreateSpriteVisual();
    glassVisual_.Brush(compositor_.CreateHostBackdropBrush());
    glassGeometry_ = compositor_.CreateRoundedRectangleGeometry();
    glassVisual_.Clip(compositor_.CreateGeometricClip(glassGeometry_));
}

bool MaterialBackdrop::SetGlass(bool enabled)
{
    glassActive_ = false;
    if (!glass_ || !glassAvailable_) return false;
    try
    {
        // 默认关闭时不创建 WinRT compositor、背景画刷或 dispatcher queue。
        if (enabled) CreateGlassResources();
        if (target_) target_.Root(enabled ? glassVisual_ : nullptr);
    }
    catch (...)
    {
        glassAvailable_ = false; status_ = L"桌面模糊启动失败，使用默认材质";
        ShowWindow(glass_, SW_HIDE); return false;
    }
    glassActive_ = enabled;
    if (!enabled) ShowWindow(glass_, SW_HIDE);
    return true;
}

void MaterialBackdrop::Hide() noexcept
{
    if (shadow_) ShowWindow(shadow_, SW_HIDE);
    if (glass_) ShowWindow(glass_, SW_HIDE);
}

void MaterialBackdrop::Sync(HWND main, const IslandView& view)
{
    if (!shadow_) return;
    if (!IsWindowVisible(main)) { Hide(); return; }
    RECT host{}; CheckWin32(GetWindowRect(main, &host), "Backdrop main bounds");
    const UINT dpi = GetDpiForWindow(main);
    const float scale = static_cast<float>(dpi) / 96;
    const auto bounds = view.Bounds(); const auto frame = view.Frame();
    RECT capsule{static_cast<LONG>(std::floor(bounds.left * scale)), static_cast<LONG>(std::floor(bounds.top * scale)),
        static_cast<LONG>(std::ceil(bounds.right * scale)), static_cast<LONG>(std::ceil(bounds.bottom * scale))};
    const float radius = std::min({frame.radius, frame.width / 2, frame.height / 2}) * scale;
    const bool changed = !EqualRect(&capsule, &lastBounds_) || radius != lastRadius_ || dpi != lastDpi_;
    if (glassActive_)
    {
        const int width = capsule.right - capsule.left, height = capsule.bottom - capsule.top;
        if (changed || !IsWindowVisible(glass_))
        {
            HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
                static_cast<int>(std::ceil(radius * 2)), static_cast<int>(std::ceil(radius * 2)));
            if (!region) ThrowLastError("Create glass region");
            if (!SetWindowRgn(glass_, region, FALSE)) { DeleteObject(region); ThrowLastError("Set glass region"); }
            glassVisual_.Size({static_cast<float>(width), static_cast<float>(height)});
            glassGeometry_.Size({static_cast<float>(width), static_cast<float>(height)});
            glassGeometry_.CornerRadius({radius, radius});
        }
        CheckWin32(SetWindowPos(glass_, main, host.left + capsule.left, host.top + capsule.top, width, height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW), "Position glass underlay");
    }
    else if (glass_) ShowWindow(glass_, SW_HIDE);
    const SIZE required{host.right - host.left, host.bottom - host.top};
    if (!memory_ || required.cx != size_.cx || required.cy != size_.cy)
    {
        ReleaseSurface(); size_ = required;
        memory_ = CreateCompatibleDC(nullptr);
        if (!memory_) ThrowLastError("Shadow DC");
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = size_.cx; info.bmiHeader.biHeight = -size_.cy;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        bitmap_ = CreateDIBSection(memory_, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels_), nullptr, 0);
        if (!bitmap_) ThrowLastError("Shadow DIB");
        oldBitmap_ = SelectObject(memory_, bitmap_);
        lastRadius_ = -1;
    }
    if (changed || lastRadius_ < 0)
    {
        std::memset(pixels_, 0, static_cast<size_t>(size_.cx) * size_.cy * 4);
        const float spread = MaterialTheme::shadowSpread * scale;
        const float halfWidth = static_cast<float>(capsule.right - capsule.left) / 2;
        const float halfHeight = static_cast<float>(capsule.bottom - capsule.top) / 2;
        const float centerX = static_cast<float>(capsule.left + capsule.right) / 2;
        const float centerY = static_cast<float>(capsule.top + capsule.bottom) / 2 + 3 * scale;
        const int left = std::max(0, static_cast<int>(capsule.left) - static_cast<int>(spread));
        const int right = std::min(static_cast<int>(size_.cx), static_cast<int>(capsule.right) + static_cast<int>(spread));
        const int top = std::max(0, static_cast<int>(capsule.top) - static_cast<int>(spread));
        const int bottom = std::min(static_cast<int>(size_.cy), static_cast<int>(capsule.bottom) + static_cast<int>(spread + 3 * scale));
        for (int y = top; y < bottom; ++y)
            for (int x = left; x < right; ++x)
            {
                const float qx = std::abs(x + 0.5f - centerX) - (halfWidth - radius);
                const float qy = std::abs(y + 0.5f - centerY) - (halfHeight - radius);
                // 中心完全透明，让默认半透明材质透出桌面。
                if (qx < 0 && qy < 0) continue;
                const float outsideX = std::max(qx, 0.0f), outsideY = std::max(qy, 0.0f);
                const float distance = std::sqrt(outsideX * outsideX + outsideY * outsideY) + std::min(std::max(qx, qy), 0.0f) - radius;
                if (distance <= -2 * scale || distance >= spread) continue;
                const float strength = std::clamp(1 - std::max(0.0f, distance) / spread, 0.0f, 1.0f);
                pixels_[(static_cast<size_t>(y) * size_.cx + x) * 4 + 3] =
                    static_cast<unsigned char>(255 * MaterialTheme::shadowOpacity * strength * strength);
            }
        lastBounds_ = capsule; lastRadius_ = radius; lastDpi_ = dpi;
        POINT destination{host.left, host.top}, source{};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        CheckWin32(UpdateLayeredWindow(shadow_, nullptr, &destination, &size_, memory_, &source, 0, &blend, ULW_ALPHA), "Update shadow");
    }
    CheckWin32(SetWindowPos(shadow_, glassActive_ ? glass_ : main, host.left, host.top, 0, 0,
        SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW), "Position shadow");
}
}
