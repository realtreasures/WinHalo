#pragma once
#include "../Core/Platform.h"
#include "../UI/IslandView.h"

namespace winisland
{
// 第三阶段可替换为 GPU 渲染器；窗口管理无需依赖绘制实现。
// DirectComposition 后端需要另外实现跨进程鼠标穿透，不能仅依赖 HTTRANSPARENT。
class IRenderer
{
public:
    virtual ~IRenderer() = default;
    virtual void Resize(SIZE pixelSize, UINT dpi) = 0;
    virtual void Render(HWND window, const IslandView& view) = 0;
    virtual bool HitTest(POINT clientPixel) const noexcept = 0;
    virtual DWORD WindowStyle() const noexcept { return WS_EX_LAYERED; }
    virtual HANDLE FrameReadyHandle() const noexcept { return nullptr; }
    virtual bool NeedsFrame() const noexcept { return false; }
    virtual void NotifyFrameReady() noexcept {}
    virtual void ResetDevice() {}
};
}
