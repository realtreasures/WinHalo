#include "LayeredRenderer.h"
#include "../Utils/Win32Error.h"
#include <cmath>
#include <stdexcept>

namespace winisland
{
LayeredRenderer::~LayeredRenderer() { Reset(); }

void LayeredRenderer::Reset() noexcept
{
    if (memoryDc_ && previousBitmap_) SelectObject(memoryDc_, previousBitmap_);
    if (bitmap_) DeleteObject(bitmap_);
    if (memoryDc_) DeleteDC(memoryDc_);
    memoryDc_ = nullptr;
    bitmap_ = nullptr;
    previousBitmap_ = nullptr;
    pixels_ = nullptr;
    size_ = {};
}

void LayeredRenderer::Resize(SIZE pixelSize, UINT dpi)
{
    if (pixelSize.cx <= 0 || pixelSize.cy <= 0 || pixelSize.cx > 16384 || pixelSize.cy > 16384 || dpi == 0)
        throw std::invalid_argument("Invalid layered surface size or DPI");
    dpi_ = dpi;
    if (size_.cx == pixelSize.cx && size_.cy == pixelSize.cy) return;
    Reset();
    try
    {
        memoryDc_ = CreateCompatibleDC(nullptr);
        if (!memoryDc_) ThrowLastError("CreateCompatibleDC");
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = pixelSize.cx;
        info.bmiHeader.biHeight = -pixelSize.cy;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* data = nullptr;
        bitmap_ = CreateDIBSection(memoryDc_, &info, DIB_RGB_COLORS, &data, nullptr, 0);
        if (!bitmap_) ThrowLastError("CreateDIBSection");
        previousBitmap_ = SelectObject(memoryDc_, bitmap_);
        if (!previousBitmap_ || previousBitmap_ == HGDI_ERROR)
        {
            previousBitmap_ = nullptr;
            ThrowLastError("SelectObject");
        }
        pixels_ = static_cast<std::uint32_t*>(data);
        size_ = pixelSize;
    }
    catch (...) { Reset(); throw; }
}

void LayeredRenderer::Render(HWND window, const IslandView& view)
{
    if (!pixels_) throw std::logic_error("Layered surface has not been initialized");
    const float scale = static_cast<float>(dpi_) / 96.0f;
    for (LONG y = 0; y < size_.cy; ++y)
        for (LONG x = 0; x < size_.cx; ++x)
        {
            const auto alpha = static_cast<std::uint32_t>(std::lround(255.0f *
                view.Coverage(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, scale)));
            const auto color = static_cast<std::uint32_t>(view.Shade()) * alpha / 255;
            // 预乘 alpha，透明区严格为零，鼠标可交给下方其他进程。
            pixels_[static_cast<size_t>(y) * size_.cx + x] =
                (alpha << 24) | (color << 16) | (color << 8) | color;
        }
    POINT source{};
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    CheckWin32(UpdateLayeredWindow(window, nullptr, nullptr, &size_, memoryDc_,
        &source, 0, &blend, ULW_ALPHA), "UpdateLayeredWindow");
}

bool LayeredRenderer::HitTest(POINT point) const noexcept
{
    return pixels_ && point.x >= 0 && point.y >= 0 && point.x < size_.cx && point.y < size_.cy &&
        (pixels_[static_cast<size_t>(point.y) * size_.cx + point.x] >> 24) != 0;
}
}
