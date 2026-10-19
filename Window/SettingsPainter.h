#pragma once
#include "../Core/Platform.h"
#include <d2d1.h>
#include <wrl/client.h>
#include <cstddef>

namespace winisland
{
// A UI-thread painter for the settings window, independent of the island GPU
// renderer. All coordinates and stroke widths are physical HDC pixels.
class SettingsPainter
{
public:
    SettingsPainter() = default;
    ~SettingsPainter();
    SettingsPainter(const SettingsPainter&) = delete;
    SettingsPainter& operator=(const SettingsPainter&) = delete;

    // The caller must finish a scope before drawing GDI text to the same DC.
    // Paint a complete opaque background with Clear() before the shapes.
    // RECT may have a nonzero origin; drawing coordinates remain HDC-relative.
    bool Begin(HDC dc, const RECT& pixelBounds) noexcept;
    bool End() noexcept;
    void Reset() noexcept;
    void Clear(COLORREF color) noexcept;
    void Rounded(const RECT& pixels, float radiusPixels, COLORREF fill,
        COLORREF outline = CLR_INVALID, float strokePixels = 1.0f) noexcept;
    void OutlineRounded(const RECT& pixels, float radiusPixels, COLORREF color,
        float strokePixels = 1.0f) noexcept;
    void Line(float x1, float y1, float x2, float y2, COLORREF color,
        float strokePixels = 1.0f) noexcept;
    void Ellipse(float centerX, float centerY, float radiusX, float radiusY,
        COLORREF fill, COLORREF outline = CLR_INVALID, float strokePixels = 1.0f) noexcept;
    void Polyline(const D2D1_POINT_2F* points, std::size_t count, COLORREF color,
        float strokePixels = 1.0f, bool closed = false) noexcept;

private:
    bool EnsureResources() noexcept;
    void DiscardTarget() noexcept;
    void RestoreDc() noexcept;
    void SetColor(COLORREF color) noexcept;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> roundStroke_;
    HDC boundDc_ = nullptr;
    int savedDc_ = 0;
    bool drawing_ = false;
};
}
