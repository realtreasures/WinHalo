#pragma once
#include "IRenderer.h"
#include "../Utils/Logger.h"
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <array>
#include <chrono>
#include <string>

namespace winisland
{
class CompositionRenderer final : public IRenderer
{
public:
    explicit CompositionRenderer(Logger& logger) : logger_(logger) {}
    ~CompositionRenderer() override;
    void Resize(SIZE pixelSize, UINT dpi) override;
    void Render(HWND window, const IslandView& view) override;
    bool HitTest(POINT clientPixel) const noexcept override;
    DWORD WindowStyle() const noexcept override { return WS_EX_NOREDIRECTIONBITMAP; }
    HANDLE FrameReadyHandle() const noexcept override { return frameReady_; }
    bool NeedsFrame() const noexcept override { return recoveryPending_; }
    void NotifyFrameReady() noexcept override { frameCredit_ = true; }
    void ResetDevice() override;
    std::wstring Diagnostics() const;
    double LastDrawMilliseconds() const noexcept { return drawMilliseconds_; }
    unsigned long long Frames() const noexcept { return frames_; }
private:
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    void CreateDevice(HWND window);
    void CreateTargetBitmap();
    void Draw(const IslandView& view);
    void DrawCachedText(const std::wstring& value, ViewRect rect, IDWriteTextFormat* font);
    void DrawBatteryIcon(const IslandView& view, float x, float y, float opacity, float scale = 1.0f);
    void PrepareMedia(const IslandView& view);
    void UpdateWindowRegion(HWND window, const IslandView& view);
    bool Recover(HRESULT error);
    Logger& logger_;
    Ptr<ID3D11Device> d3d_;
    Ptr<ID3D11DeviceContext> d3dContext_;
    Ptr<IDXGISwapChain2> swapChain_;
    Ptr<ID2D1Factory1> d2dFactory_;
    Ptr<ID2D1Device> d2dDevice_;
    Ptr<ID2D1DeviceContext> context_;
    Ptr<ID2D1Bitmap1> targetBitmap_;
    Ptr<ID2D1SolidColorBrush> brush_;
    Ptr<ID2D1RadialGradientBrush> mediaGlow_;
    Ptr<ID2D1PathGeometry> playGlyph_;
    Ptr<IDWriteFactory> writeFactory_;
    Ptr<IDWriteTextFormat> titleFormat_, bodyFormat_, smallFormat_, mediaTitleFormat_, mediaArtistFormat_;
    struct TextEntry
    {
        std::wstring value;
        float width = 0, height = 0;
        IDWriteTextFormat* font = nullptr;
        Ptr<IDWriteTextLayout> layout;
    };
    std::array<TextEntry, 32> textCache_;
    size_t textCursor_ = 0;
    std::array<Ptr<IDWriteTextLayout>, 6> mediaLabels_;
    std::array<std::wstring, 6> mediaText_;
    Ptr<ID2D1RoundedRectangleGeometry> smallCoverClip_, largeCoverClip_;
    Ptr<ID2D1Bitmap1> artwork_;
    Ptr<ID2D1Bitmap1> appIcon_;
    std::shared_ptr<const MediaAppInfo> iconApp_;
    std::uint64_t artworkRevision_ = 0;
    float mediaTextWidth_ = 0;
    Ptr<IDCompositionDevice> composition_;
    Ptr<IDCompositionTarget> target_;
    Ptr<IDCompositionVisual> visual_;
    HANDLE frameReady_ = nullptr;
    HRGN hitRegion_ = nullptr;
    SIZE size_{};
    UINT dpi_ = 96;
    CapsuleFrame submitted_{};
    bool regionValid_ = false;
    bool recoveryPending_ = false;
    bool frameCredit_ = false;
    bool compositionPending_ = false;
    unsigned int recoveryAttempts_ = 0;
    unsigned int deviceGeneration_ = 0;
    unsigned long long frames_ = 0;
    double drawMilliseconds_ = 0;
    bool hardware_ = true;
};
}
