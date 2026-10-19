#include "SettingsPainter.h"
#include <d2d1helper.h>
#include <algorithm>
#include <cmath>
#pragma comment(lib, "D2d1.lib")

namespace winisland
{
namespace
{
D2D1_COLOR_F Color(COLORREF value) noexcept
{
    return D2D1::ColorF(static_cast<float>(GetRValue(value)) / 255.0f,
        static_cast<float>(GetGValue(value)) / 255.0f,
        static_cast<float>(GetBValue(value)) / 255.0f, 1.0f);
}
D2D1_RECT_F FloatRect(const RECT& value) noexcept
{
    return D2D1::RectF(static_cast<float>(value.left), static_cast<float>(value.top),
        static_cast<float>(value.right), static_cast<float>(value.bottom));
}
bool PositiveRect(const D2D1_RECT_F& value) noexcept
{
    return value.right > value.left && value.bottom > value.top;
}
float Radius(float requested, const D2D1_RECT_F& rect) noexcept
{
    return std::clamp(requested, 0.0f, std::min(rect.right - rect.left, rect.bottom - rect.top) * 0.5f);
}
bool PointValid(const D2D1_POINT_2F& point) noexcept
{
    return std::isfinite(point.x) && std::isfinite(point.y);
}
}

SettingsPainter::~SettingsPainter()
{
    Reset();
}

void SettingsPainter::Reset() noexcept
{
    if (drawing_) End();
    RestoreDc();
    DiscardTarget();
}

bool SettingsPainter::EnsureResources() noexcept
{
    if (!factory_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf())))
        return false;
    if (!roundStroke_)
    {
        const auto properties = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND,
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        if (FAILED(factory_->CreateStrokeStyle(properties, nullptr, 0, roundStroke_.GetAddressOf())))
            return false;
    }
    if (!target_)
    {
        const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        if (FAILED(factory_->CreateDCRenderTarget(&properties, target_.GetAddressOf())))
            return false;
    }
    if (!brush_ && FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(0), brush_.GetAddressOf())))
    {
        DiscardTarget();
        return false;
    }
    return true;
}

bool SettingsPainter::Begin(HDC dc, const RECT& pixelBounds) noexcept
{
    if (drawing_ || !dc || pixelBounds.right <= pixelBounds.left || pixelBounds.bottom <= pixelBounds.top)
        return false;
    if (!EnsureResources()) return false;
    const int saved = SaveDC(dc);
    if (!saved) return false;
    boundDc_ = dc;
    savedDc_ = saved;
    const HRESULT bound = target_->BindDC(dc, &pixelBounds);
    if (FAILED(bound))
    {
        RestoreDc();
        DiscardTarget();
        return false;
    }
    // The backing bitmap starts at (0,0), but callers use absolute DC pixels.
    // Fixing 96 DPI prevents the caller's existing P()/R() conversion from
    // applying monitor scaling twice.
    target_->SetDpi(96.0f, 96.0f);
    target_->SetTransform(D2D1::Matrix3x2F::Translation(-static_cast<float>(pixelBounds.left),
        -static_cast<float>(pixelBounds.top)));
    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    target_->BeginDraw();
    drawing_ = true;
    return true;
}

bool SettingsPainter::End() noexcept
{
    if (!drawing_) return false;
    drawing_ = false;
    const HRESULT result = target_->EndDraw();
    RestoreDc();
    if (FAILED(result))
    {
        // In particular D2DERR_RECREATE_TARGET discards every target-dependent
        // object. The factory and stroke style remain reusable next paint.
        DiscardTarget();
        return false;
    }
    return true;
}

void SettingsPainter::DiscardTarget() noexcept
{
    brush_.Reset();
    target_.Reset();
}

void SettingsPainter::RestoreDc() noexcept
{
    if (boundDc_ && savedDc_) RestoreDC(boundDc_, savedDc_);
    boundDc_ = nullptr;
    savedDc_ = 0;
}

void SettingsPainter::SetColor(COLORREF color) noexcept
{
    brush_->SetColor(Color(color));
}

void SettingsPainter::Clear(COLORREF color) noexcept
{
    if (drawing_ && color != CLR_INVALID) target_->Clear(Color(color));
}

void SettingsPainter::Rounded(const RECT& pixels, float radiusPixels, COLORREF fill,
    COLORREF outline, float strokePixels) noexcept
{
    if (!drawing_ || !std::isfinite(radiusPixels)) return;
    const auto rect = FloatRect(pixels);
    if (!PositiveRect(rect)) return;
    if (fill != CLR_INVALID)
    {
        SetColor(fill);
        const float radius = Radius(radiusPixels, rect);
        target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get());
    }
    if (outline != CLR_INVALID) OutlineRounded(pixels, radiusPixels, outline, strokePixels);
}

void SettingsPainter::OutlineRounded(const RECT& pixels, float radiusPixels, COLORREF color,
    float strokePixels) noexcept
{
    if (!drawing_ || color == CLR_INVALID || !std::isfinite(radiusPixels) ||
        !std::isfinite(strokePixels) || strokePixels <= 0) return;
    auto rect = FloatRect(pixels);
    if (!PositiveRect(rect)) return;
    const float stroke = std::min(strokePixels, std::min(rect.right - rect.left, rect.bottom - rect.top));
    const float inset = stroke * 0.5f;
    const float outerRadius = Radius(radiusPixels, rect);
    rect.left += inset; rect.top += inset; rect.right -= inset; rect.bottom -= inset;
    if (!PositiveRect(rect)) return;
    const float radius = Radius(std::max(0.0f, outerRadius - inset), rect);
    SetColor(color);
    target_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get(), stroke);
}

void SettingsPainter::Line(float x1, float y1, float x2, float y2, COLORREF color,
    float strokePixels) noexcept
{
    const D2D1_POINT_2F first{x1, y1}, second{x2, y2};
    if (!drawing_ || color == CLR_INVALID || !std::isfinite(strokePixels) || strokePixels <= 0 ||
        !PointValid(first) || !PointValid(second)) return;
    SetColor(color);
    target_->DrawLine(first, second, brush_.Get(), strokePixels, roundStroke_.Get());
}

void SettingsPainter::Ellipse(float centerX, float centerY, float radiusX, float radiusY,
    COLORREF fill, COLORREF outline, float strokePixels) noexcept
{
    if (!drawing_ || !PointValid({centerX, centerY}) || !std::isfinite(radiusX) ||
        !std::isfinite(radiusY) || radiusX <= 0 || radiusY <= 0) return;
    const D2D1_POINT_2F center{centerX, centerY};
    if (fill != CLR_INVALID)
    {
        SetColor(fill);
        target_->FillEllipse(D2D1::Ellipse(center, radiusX, radiusY), brush_.Get());
    }
    if (outline != CLR_INVALID && std::isfinite(strokePixels) && strokePixels > 0)
    {
        const float stroke = std::min(strokePixels, std::min(radiusX, radiusY) * 2);
        const float inset = stroke * 0.5f;
        if (radiusX <= inset || radiusY <= inset) return;
        SetColor(outline);
        target_->DrawEllipse(D2D1::Ellipse(center, radiusX - inset, radiusY - inset), brush_.Get(), stroke);
    }
}

void SettingsPainter::Polyline(const D2D1_POINT_2F* points, std::size_t count, COLORREF color,
    float strokePixels, bool closed) noexcept
{
    if (!drawing_ || !points || count < 2 || color == CLR_INVALID ||
        !std::isfinite(strokePixels) || strokePixels <= 0) return;
    for (std::size_t index = 0; index < count; ++index)
        if (!PointValid(points[index])) return;
    SetColor(color);
    for (std::size_t index = 1; index < count; ++index)
        target_->DrawLine(points[index - 1], points[index], brush_.Get(), strokePixels, roundStroke_.Get());
    if (closed) target_->DrawLine(points[count - 1], points[0], brush_.Get(), strokePixels, roundStroke_.Get());
}
}
