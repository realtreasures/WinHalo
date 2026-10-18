#pragma once
#include "IRenderer.h"
#include <cstdint>

namespace winisland
{
// 阶段二的按需软件绘制后端，只在布局或交互状态改变时提交位图。
class LayeredRenderer final : public IRenderer
{
public:
    LayeredRenderer() = default;
    ~LayeredRenderer() override;
    LayeredRenderer(const LayeredRenderer&) = delete;
    LayeredRenderer& operator=(const LayeredRenderer&) = delete;
    void Resize(SIZE pixelSize, UINT dpi) override;
    void Render(HWND window, const IslandView& view) override;
    bool HitTest(POINT clientPixel) const noexcept override;

private:
    void Reset() noexcept;
    HDC memoryDc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previousBitmap_ = nullptr;
    std::uint32_t* pixels_ = nullptr;
    SIZE size_{};
    UINT dpi_ = 96;
};
}
