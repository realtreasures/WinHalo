#include "CompositionRenderer.h"
#include "MaterialTheme.h"
#include "../Utils/Win32Error.h"
#include <d2d1_1helper.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace winisland
{
namespace
{
void Check(HRESULT result, const char* operation)
{
    if (FAILED(result)) throw std::system_error(static_cast<int>(result), std::system_category(), operation);
}
D2D1_RECT_F Rect(ViewRect rect) { return D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom); }
}

CompositionRenderer::~CompositionRenderer() { ResetDevice(); }

void CompositionRenderer::ResetDevice()
{
    if (context_) context_->SetTarget(nullptr);
    targetBitmap_.Reset();
    brush_.Reset();
    mediaGlow_.Reset(); playGlyph_.Reset();
    context_.Reset();
    d2dDevice_.Reset();
    d2dFactory_.Reset();
    for (auto& entry : textCache_) entry = {};
    textCursor_ = 0;
    for (auto& label : mediaLabels_) label.Reset();
    artwork_.Reset();
    appIcon_.Reset(); iconApp_.reset();
    smallCoverClip_.Reset(); largeCoverClip_.Reset();
    artworkRevision_ = 0;
    mediaTextWidth_ = 0;
    titleFormat_.Reset(); bodyFormat_.Reset(); smallFormat_.Reset();
    mediaTitleFormat_.Reset(); mediaArtistFormat_.Reset(); writeFactory_.Reset();
    // 先解除 visual 对旧交换链的引用，再释放设备。所有操作限于 UI 线程。
    visual_.Reset(); target_.Reset(); composition_.Reset();
    if (frameReady_) { CloseHandle(frameReady_); frameReady_ = nullptr; }
    swapChain_.Reset();
    if (d3dContext_) { d3dContext_->ClearState(); d3dContext_->Flush(); }
    d3dContext_.Reset(); d3d_.Reset();
    regionValid_ = false;
    frameCredit_ = false;
    compositionPending_ = false;
    if (hitRegion_) { DeleteObject(hitRegion_); hitRegion_ = nullptr; }
}

void CompositionRenderer::CreateDevice(HWND window)
{
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &d3d_, nullptr, &d3dContext_);
    hardware_ = SUCCEEDED(result);
    if (FAILED(result))
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &d3d_, nullptr, &d3dContext_);
    Check(result, "D3D11CreateDevice");
    Ptr<IDXGIDevice> dxgiDevice;
    Check(d3d_.As(&dxgiDevice), "Query DXGI device");
    Ptr<IDXGIAdapter> adapter;
    Check(dxgiDevice->GetAdapter(&adapter), "GetAdapter");
    Ptr<IDXGIFactory2> factory;
    Check(adapter->GetParent(IID_PPV_ARGS(&factory)), "Get DXGI factory");
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = static_cast<UINT>(size_.cx);
    description.Height = static_cast<UINT>(size_.cy);
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    Ptr<IDXGISwapChain1> chain;
    Check(factory->CreateSwapChainForComposition(d3d_.Get(), &description, nullptr, &chain),
        "CreateSwapChainForComposition");
    Check(chain.As(&swapChain_), "Query swap chain 2");
    Check(swapChain_->SetMaximumFrameLatency(1), "SetMaximumFrameLatency");
    frameReady_ = swapChain_->GetFrameLatencyWaitableObject();
    if (!frameReady_) ThrowLastError("GetFrameLatencyWaitableObject");
    Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf()), "D2D1CreateFactory");
    Check(d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_), "Create D2D device");
    Check(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context_), "Create D2D context");
    Check(context_->CreateSolidColorBrush(D2D1::ColorF(0), &brush_), "Create brush");
    const D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(0.55f, 0.17f, 0.09f, 0.28f)},
        {1, D2D1::ColorF(0.55f, 0.17f, 0.09f, 0)}};
    Ptr<ID2D1GradientStopCollection> gradient;
    Check(context_->CreateGradientStopCollection(stops, 2, &gradient), "Media glow stops");
    Check(context_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({0, 0}, {0, 0}, 240, 140),
        gradient.Get(), &mediaGlow_), "Media glow brush");
    Check(d2dFactory_->CreatePathGeometry(&playGlyph_), "Media play glyph");
    Ptr<ID2D1GeometrySink> glyphSink;
    Check(playGlyph_->Open(&glyphSink), "Media glyph sink");
    glyphSink->BeginFigure({-8, -12}, D2D1_FIGURE_BEGIN_FILLED);
    glyphSink->AddLine({10, 0}); glyphSink->AddLine({-8, 12});
    glyphSink->EndFigure(D2D1_FIGURE_END_CLOSED);
    Check(glyphSink->Close(), "Media glyph close");
    context_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(writeFactory_.GetAddressOf())), "DWriteCreateFactory");
    const auto format = [&](float size, DWRITE_FONT_WEIGHT weight, Ptr<IDWriteTextFormat>& destination)
    {
        Check(writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &destination), "CreateTextFormat");
    };
    format(20, DWRITE_FONT_WEIGHT_SEMI_BOLD, titleFormat_);
    format(13, DWRITE_FONT_WEIGHT_NORMAL, bodyFormat_);
    format(11, DWRITE_FONT_WEIGHT_NORMAL, smallFormat_);
    format(22, DWRITE_FONT_WEIGHT_BOLD, mediaTitleFormat_);
    format(16, DWRITE_FONT_WEIGHT_NORMAL, mediaArtistFormat_);
    Check(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&composition_)), "DCompositionCreateDevice");
    Check(composition_->CreateTargetForHwnd(window, TRUE, &target_), "Create composition target");
    Check(composition_->CreateVisual(&visual_), "Create visual");
    Check(visual_->SetContent(swapChain_.Get()), "Set composition content");
    Check(target_->SetRoot(visual_.Get()), "Set composition root");
    CreateTargetBitmap();
    // 首帧完成后才将 visual 提交给 DWM，避免设备重建时暴露空交换链。
    compositionPending_ = true;
    ++deviceGeneration_;
    logger_.Write(hardware_ ? "Renderer: D3D11 hardware + Direct2D + DirectComposition" : "Renderer: WARP fallback + DirectComposition");
}

void CompositionRenderer::CreateTargetBitmap()
{
    Ptr<IDXGISurface> surface;
    Check(swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface)), "Get swap chain buffer");
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        static_cast<float>(dpi_), static_cast<float>(dpi_));
    Check(context_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &targetBitmap_), "Create target bitmap");
    context_->SetTarget(targetBitmap_.Get());
    context_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
}

void CompositionRenderer::Resize(SIZE pixelSize, UINT dpi)
{
    if (pixelSize.cx <= 0 || pixelSize.cy <= 0 || pixelSize.cx > 16384 || pixelSize.cy > 16384 || !dpi)
        throw std::invalid_argument("Invalid composition size or DPI");
    const bool changed = pixelSize.cx != size_.cx || pixelSize.cy != size_.cy;
    const bool dpiChanged = dpi != dpi_;
    size_ = pixelSize; dpi_ = dpi;
    if (!swapChain_ || (!changed && !dpiChanged)) return;
    context_->SetTarget(nullptr);
    targetBitmap_.Reset();
    if (changed)
    {
        const HRESULT result = swapChain_->ResizeBuffers(2, static_cast<UINT>(size_.cx), static_cast<UINT>(size_.cy),
            DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        if (Recover(result)) return;
        Check(result, "ResizeBuffers");
    }
    CreateTargetBitmap();
    regionValid_ = false;
}

bool CompositionRenderer::Recover(HRESULT result)
{
    if (result != DXGI_ERROR_DEVICE_REMOVED && result != DXGI_ERROR_DEVICE_RESET && result != D2DERR_RECREATE_TARGET)
        return false;
    if (++recoveryAttempts_ > 3) Check(result, "Graphics device recovery limit");
    logger_.Write("Graphics device lost; scheduling recreation");
    ResetDevice();
    recoveryPending_ = true;
    return true;
}

void CompositionRenderer::DrawBatteryIcon(const IslandView& view, float x, float y, float opacity, float scale)
{
    const auto& battery = view.Battery();
    brush_->SetColor(battery.low ? D2D1::ColorF(1.0f, 0.62f, 0.23f, opacity) :
        D2D1::ColorF(0.57f, 0.94f, 0.77f, opacity));
    context_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + 19 * scale, y + 10 * scale),
        2 * scale, 2 * scale), brush_.Get(), scale);
    context_->FillRectangle(D2D1::RectF(x + 19 * scale, y + 3 * scale, x + 21 * scale, y + 7 * scale), brush_.Get());
    if (battery.percent >= 0)
        context_->FillRectangle(D2D1::RectF(x + 2 * scale, y + 2 * scale,
            x + (2 + 15 * static_cast<float>(battery.percent) / 100) * scale, y + 8 * scale), brush_.Get());
    if (battery.charging)
    {
        brush_->SetColor(D2D1::ColorF(1, 1, 1, opacity));
        context_->DrawLine({x + 11 * scale, y - scale}, {x + 7 * scale, y + 5 * scale}, brush_.Get(), scale);
        context_->DrawLine({x + 7 * scale, y + 5 * scale}, {x + 12 * scale, y + 5 * scale}, brush_.Get(), scale);
        context_->DrawLine({x + 12 * scale, y + 5 * scale}, {x + 8 * scale, y + 11 * scale}, brush_.Get(), scale);
    }
}

void CompositionRenderer::UpdateWindowRegion(HWND window, const IslandView& view)
{
    const auto frame = view.Frame();
    if (regionValid_ && frame.width == submitted_.width && frame.height == submitted_.height &&
        frame.centerX == submitted_.centerX && frame.top == submitted_.top && frame.radius == submitted_.radius) return;
    const float scale = static_cast<float>(dpi_) / 96;
    const auto bounds = view.Bounds();
    // 额外留出一个物理像素供 Direct2D 抗锯齿，不将整块宿主暴露给鼠标。
    const int diameter = static_cast<int>(std::ceil(2 * std::min({frame.radius, frame.width / 2, frame.height / 2}) * scale));
    HRGN region = CreateRoundRectRgn(static_cast<int>(std::floor(bounds.left * scale)) - 1,
        static_cast<int>(std::floor(bounds.top * scale)) - 1,
        static_cast<int>(std::ceil(bounds.right * scale)) + 2,
        static_cast<int>(std::ceil(bounds.bottom * scale)) + 2, diameter, diameter);
    if (!region) ThrowLastError("CreateRoundRectRgn");
    HRGN hitCopy = CreateRectRgn(0, 0, 0, 0);
    if (!hitCopy || CombineRgn(hitCopy, region, nullptr, RGN_COPY) == ERROR)
    {
        if (hitCopy) DeleteObject(hitCopy);
        DeleteObject(region);
        ThrowLastError("Copy window region");
    }
    if (!SetWindowRgn(window, region, TRUE))
    {
        const DWORD error = GetLastError();
        DeleteObject(region);
        DeleteObject(hitCopy);
        SetLastError(error);
        ThrowLastError("SetWindowRgn");
    }
    // 成功后由 Windows 接管 HRGN，不能再 DeleteObject。
    if (hitRegion_) DeleteObject(hitRegion_);
    hitRegion_ = hitCopy;
    regionValid_ = true;
}

void CompositionRenderer::DrawCachedText(const std::wstring& value, ViewRect rect, IDWriteTextFormat* font)
{
    const float width = std::max(1.0f, rect.right - rect.left), height = std::max(1.0f, rect.bottom - rect.top);
    TextEntry* found = nullptr;
    for (auto& entry : textCache_)
        if (entry.layout && entry.value == value && entry.font == font && entry.width == width && entry.height == height)
        { found = &entry; break; }
    if (!found)
    {
        found = &textCache_[textCursor_++ % textCache_.size()];
        *found = {};
        Check(writeFactory_->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()), font,
            width, height, &found->layout), "Create cached text");
        Check(found->layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "Cached text wrapping");
        Ptr<IDWriteInlineObject> ellipsis;
        Check(writeFactory_->CreateEllipsisTrimmingSign(font, &ellipsis), "Cached text ellipsis");
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        Check(found->layout->SetTrimming(&trimming, ellipsis.Get()), "Cached text trimming");
        found->value = value; found->font = font; found->width = width; found->height = height;
    }
    context_->DrawTextLayout({rect.left, rect.top}, found->layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void CompositionRenderer::Draw(const IslandView& view)
{
    const auto frame = view.Frame();
    const auto bounds = view.Bounds();
    const auto& media = view.Media();
    context_->BeginDraw();
    context_->SetTransform(D2D1::Matrix3x2F::Identity());
    context_->Clear(D2D1::ColorF(0, 0.0f));
    const float tint = frame.hover * 0.015f;
    const float lineMix = std::clamp(1.0f - frame.opacity - frame.compactOpacity, 0.0f, 1.0f);
    const float music = view.Page() == IslandPage::Media && !view.TransientToast() ? frame.opacity : 0;
    const float backgroundOpacity = view.GlassActive() ? MaterialTheme::glassOpacity : MaterialTheme::opacity + music * (1 - MaterialTheme::opacity);
    brush_->SetColor(D2D1::ColorF((MaterialTheme::red + tint) * (1 - music) * (1 - lineMix) + 0.070f * lineMix,
        (MaterialTheme::green + tint) * (1 - music) * (1 - lineMix) + 0.078f * lineMix,
        (MaterialTheme::blue + tint) * (1 - music) * (1 - lineMix) + 0.090f * lineMix,
        backgroundOpacity + (0.97f - backgroundOpacity) * lineMix));
    context_->FillRoundedRectangle(D2D1::RoundedRect(Rect(bounds), frame.radius, frame.radius), brush_.Get());
    if (music > 0.001f)
    {
        mediaGlow_->SetCenter({frame.centerX, bounds.bottom + 65});
        mediaGlow_->SetRadiusX(frame.width * 0.7f);
        mediaGlow_->SetRadiusY(150);
        mediaGlow_->SetOpacity(music);
        context_->FillRoundedRectangle(D2D1::RoundedRect(Rect(bounds), frame.radius, frame.radius), mediaGlow_.Get());
    }
    brush_->SetColor(D2D1::ColorF(1, 1, 1, MaterialTheme::outlineOpacity + 0.04f * lineMix));
    context_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bounds.left + 0.5f, bounds.top + 0.5f,
        bounds.right - 0.5f, bounds.bottom - 0.5f), frame.radius, frame.radius), brush_.Get(), 1);
    brush_->SetColor(D2D1::ColorF(1, 1, 1, MaterialTheme::highlightOpacity + 0.10f * lineMix));
    context_->DrawLine({bounds.left + frame.radius, bounds.top + 1.5f},
        {bounds.right - frame.radius, bounds.top + 1.5f}, brush_.Get());
    // 内容随独立弹簧淡入；高度不足时不把文字、封面或均衡器挤进待机横线。
    const float heightFade = std::clamp((frame.height - 18.0f) / 18.0f, 0.0f, 1.0f);
    float alpha = frame.compactOpacity * heightFade * heightFade * (3.0f - 2.0f * heightFade);
    if (alpha < 0.001f && frame.opacity < 0.001f) return;
    if ((alpha > 0.001f && media.available) ||
        (frame.opacity > 0.001f && frame.height >= 190 && view.Page() == IslandPage::Media && !view.TransientToast()))
        PrepareMedia(view);
    const auto text = [&](const std::wstring& value, ViewRect rect, float brightness = 1.0f, IDWriteTextFormat* font = nullptr)
    {
        brush_->SetColor(D2D1::ColorF(brightness, brightness, brightness, alpha));
        DrawCachedText(value, rect, font ? font : bodyFormat_.Get());
    };
    const auto mint = [&] { brush_->SetColor(D2D1::ColorF(0.57f, 0.94f, 0.77f, alpha)); };
    const auto cover = [&](float x, float y, bool large)
    {
        const float size = large ? 72.0f : 28.0f;
        auto& clip = large ? largeCoverClip_ : smallCoverClip_;
        if (!clip) Check(d2dFactory_->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(D2D1::RectF(0, 0, size, size), large ? 14.0f : 7.0f, large ? 14.0f : 7.0f), &clip), "Cover clip");
        brush_->SetColor(D2D1::ColorF(0.11f, 0.16f, 0.16f, alpha));
        context_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + size, y + size),
            large ? 14.0f : 7.0f, large ? 14.0f : 7.0f), brush_.Get());
        if (artwork_ && media.available)
        {
            const auto image = artwork_->GetSize();
            const float fit = std::min(size / image.width, size / image.height);
            const float width = image.width * fit, height = image.height * fit;
            context_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::Matrix3x2F::Translation(x, y)), nullptr);
            context_->DrawBitmap(artwork_.Get(), D2D1::RectF(x + (size - width) / 2, y + (size - height) / 2,
                x + (size + width) / 2, y + (size + height) / 2), alpha, D2D1_INTERPOLATION_MODE_LINEAR);
            context_->PopLayer();
        }
        else
        {
            mint();
            context_->DrawLine({x + size * 0.57f, y + size * 0.24f}, {x + size * 0.57f, y + size * 0.68f}, brush_.Get(), large ? 2 : 1.5f);
            context_->DrawLine({x + size * 0.57f, y + size * 0.24f}, {x + size * 0.76f, y + size * 0.30f}, brush_.Get(), large ? 2 : 1.5f);
            context_->FillEllipse(D2D1::Ellipse({x + size * 0.46f, y + size * 0.70f}, size * 0.12f, size * 0.08f), brush_.Get());
        }
    };
    SYSTEMTIME local{}; GetLocalTime(&local);
    wchar_t clock[16]{}, date[48]{};
    swprintf_s(clock, L"%02u:%02u", local.wHour, local.wMinute);
    swprintf_s(date, L"%u 月 %u 日 · 轻触展开", local.wMonth, local.wDay);
    if (alpha > 0.001f)
    {
        context_->PushAxisAlignedClip(Rect(bounds), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const float centerY = bounds.top + frame.height / 2;
        if (media.available)
        {
            cover(bounds.left + 16, centerY - 14, false);
            brush_->SetColor(D2D1::ColorF(0.94f, 0.95f, 0.96f, alpha));
            const float y = centerY - (frame.hover > 0.1f ? 18.0f : 10.0f);
            context_->DrawTextLayout({bounds.left + 56, y}, mediaLabels_[4].Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if (frame.hover > 0.01f)
            {
                brush_->SetColor(D2D1::ColorF(0.65f, 0.69f, 0.73f, alpha * frame.hover));
                context_->DrawTextLayout({bounds.left + 56, centerY + 3}, mediaLabels_[5].Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            mint();
            for (int i = 0; i < 4; ++i)
            {
                const float h = 18 * static_cast<float>(view.EqualizerLevel(i));
                const float x = bounds.right - 34 + static_cast<float>(i) * 5;
                context_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, centerY - h / 2, x + 2.5f, centerY + h / 2), 1.25f, 1.25f), brush_.Get());
            }
        }
        else
        {
            mint();
            context_->DrawEllipse(D2D1::Ellipse({bounds.left + 26, centerY}, 7, 7), brush_.Get(), 1.2f);
            context_->DrawLine({bounds.left + 26, centerY - 4}, {bounds.left + 26, centerY}, brush_.Get(), 1.2f);
            context_->DrawLine({bounds.left + 26, centerY}, {bounds.left + 29, centerY + 2}, brush_.Get(), 1.2f);
            text(clock, {bounds.left + 46, centerY - 14, bounds.right - 38, centerY + 15}, 0.96f, titleFormat_.Get());
            if (frame.hover > 0.1f)
                text(L"轻触展开", {bounds.left + 110, centerY - 8, bounds.right - 12, centerY + 12}, 0.55f);
            if (view.Battery().available && frame.hover < 0.1f)
                DrawBatteryIcon(view, bounds.right - 33, centerY - 5, alpha);
        }
        context_->PopAxisAlignedClip();
    }
    if (frame.opacity < 0.001f) return;
    alpha = frame.opacity;
    context_->PushAxisAlignedClip(D2D1::RectF(bounds.left + 12, bounds.top + 12, bounds.right - 12, bounds.bottom - 12), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const auto button = [&](ViewRect rect, const std::wstring& title)
    {
        brush_->SetColor(D2D1::ColorF(1, 1, 1, alpha * 0.065f));
        context_->FillRoundedRectangle(D2D1::RoundedRect(Rect(rect), 10, 10), brush_.Get());
        text(title, {rect.left + 14, rect.top + 7, rect.right - 8, rect.bottom}, 0.82f);
    };
    const auto volumeControls = [&]
    {
        for (const auto action : {ViewAction::VolumeMute, ViewAction::VolumeDown, ViewAction::VolumeUp})
        {
            const auto rect = view.VolumeButton(action);
            brush_->SetColor(D2D1::ColorF(1, 1, 1, alpha * 0.065f));
            context_->FillRoundedRectangle(D2D1::RoundedRect(Rect(rect), 8, 8), brush_.Get());
            const float brightness = view.Volume().available ? 0.9f : 0.3f;
            brush_->SetColor(D2D1::ColorF(brightness, brightness, brightness, alpha));
            const float x = (rect.left + rect.right) / 2, y = (rect.top + rect.bottom) / 2;
            if (action == ViewAction::VolumeMute)
            {
                context_->FillRectangle(D2D1::RectF(x - 8, y - 3, x - 4, y + 3), brush_.Get());
                context_->DrawLine({x - 4, y - 3}, {x, y - 7}, brush_.Get(), 1.5f);
                context_->DrawLine({x, y - 7}, {x, y + 7}, brush_.Get(), 1.5f);
                context_->DrawLine({x, y + 7}, {x - 4, y + 3}, brush_.Get(), 1.5f);
                if (view.Volume().muted)
                {
                    context_->DrawLine({x + 3, y - 3}, {x + 8, y + 3}, brush_.Get(), 1.4f);
                    context_->DrawLine({x + 3, y + 3}, {x + 8, y - 3}, brush_.Get(), 1.4f);
                }
                else
                {
                    context_->DrawLine({x + 4, y - 4}, {x + 7, y}, brush_.Get(), 1.4f);
                    context_->DrawLine({x + 7, y}, {x + 4, y + 4}, brush_.Get(), 1.4f);
                }
            }
            else
            {
                context_->DrawLine({x - 4, y}, {x + 4, y}, brush_.Get(), 1.5f);
                if (action == ViewAction::VolumeUp) context_->DrawLine({x, y - 4}, {x, y + 4}, brush_.Get(), 1.5f);
            }
        }
    };
    const auto progress = [&](ViewRect rect, float amount)
    {
        brush_->SetColor(D2D1::ColorF(1, 1, 1, alpha * 0.10f));
        context_->FillRoundedRectangle(D2D1::RoundedRect(Rect(rect), 3, 3), brush_.Get());
        if (amount > 0)
        {
            rect.right = rect.left + (rect.right - rect.left) * std::clamp(amount, 0.0f, 1.0f);
            mint(); context_->FillRoundedRectangle(D2D1::RoundedRect(Rect(rect), 3, 3), brush_.Get());
        }
    };
    const auto percent = view.Volume().available ? (view.Volume().muted ? std::wstring(L"静音") :
        std::to_wstring(static_cast<int>(std::lround(view.Volume().level * 100))) + L"%") : std::wstring(L"无输出设备");
    if (view.BatteryToast())
    {
        DrawBatteryIcon(view, bounds.left + 26, bounds.top + 34, alpha, 1.5f);
        text(L"电量偏低 · " + std::to_wstring(view.Battery().percent) + L"%",
            {bounds.left + 66, bounds.top + 21, bounds.right - 24, bounds.top + 49}, 0.98f, titleFormat_.Get());
        text(L"请连接电源", {bounds.left + 66, bounds.top + 53, bounds.right - 24, bounds.top + 74}, 0.55f);
    }
    else if (view.VolumeToast())
    {
        volumeControls();
        text(percent, {bounds.left + 66, bounds.top + 22, bounds.right - 88, bounds.top + 43});
        progress({bounds.left + 66, bounds.top + 51, bounds.right - 94, bounds.top + 55}, view.Volume().muted ? 0 : view.VolumeLevel());
    }
    else if (frame.height >= 190)
    {
        const auto page = view.Page();
        if (page != IslandPage::Media)
        {
        const auto close = view.CloseButton();
        brush_->SetColor(D2D1::ColorF(1, 1, 1, alpha * 0.7f));
        const float x = (close.left + close.right) / 2, y = (close.top + close.bottom) / 2;
        context_->DrawLine({x - 5, y + 2}, {x, y - 3}, brush_.Get(), 1.5f);
        context_->DrawLine({x, y - 3}, {x + 5, y + 2}, brush_.Get(), 1.5f);
        }
        const wchar_t* heading = page == IslandPage::Home ? L"WinHalo" : (page == IslandPage::Media ? L"正在播放" :
            (page == IslandPage::Volume ? L"系统音量" : (page == IslandPage::Battery ? L"电池状态" : L"偏好设置")));
        if (page != IslandPage::Media)
            text(heading, {bounds.left + 24, bounds.top + 24, bounds.right - 65, bounds.top + 53}, 0.96f, titleFormat_.Get());
        if (page == IslandPage::Home)
        {
            text(date, {bounds.left + 24, bounds.top + 58, bounds.right - 24, bounds.top + 80}, 0.55f);
            int index = 0;
            for (const auto module : {IslandPage::Media, IslandPage::Volume, IslandPage::Battery, IslandPage::Settings})
            {
                const auto rect = view.ModuleButton(module);
                brush_->SetColor(D2D1::ColorF(1, 1, 1, alpha * 0.055f));
                context_->FillRoundedRectangle(D2D1::RoundedRect(Rect(rect), 16, 16), brush_.Get());
                brush_->SetColor(D2D1::ColorF(1, 1, 1, alpha * 0.055f));
                context_->DrawRoundedRectangle(D2D1::RoundedRect(Rect(rect), 16, 16), brush_.Get());
                const wchar_t* names[] = {L"音乐", L"音量", L"电池", L"设置"};
                const std::wstring subtitle = module == IslandPage::Media ? (media.available ? (media.playing ? L"正在播放" : L"已暂停") : L"暂无媒体会话") :
                    (module == IslandPage::Volume ? percent : (module == IslandPage::Battery ? (view.Battery().available ?
                        (view.Battery().percent >= 0 ? std::to_wstring(view.Battery().percent) + L"%" : L"电量未知") : L"无电池") : L"动画与系统"));
                text(names[index++], {rect.left + 16, rect.top + 16, rect.right - 16, rect.top + 40}, 0.95f);
                text(subtitle, {rect.left + 16, rect.top + 47, rect.right - 16, rect.bottom - 10}, 0.52f, smallFormat_.Get());
                mint(); context_->FillEllipse(D2D1::Ellipse({rect.right - 20, rect.top + 23}, 3, 3), brush_.Get());
            }
            text(L"常用操作，轻触即达", {bounds.left + 24, bounds.bottom - 44, bounds.right - 112, bounds.bottom - 20}, 0.4f);
        }
        else if (page == IslandPage::Media)
        {
            cover(bounds.left + 28, bounds.top + 24, true);
            for (size_t i : {size_t{0}, size_t{1}})
            {
                brush_->SetColor(D2D1::ColorF(i == 0 ? 0.96f : 0.58f, i == 0 ? 0.96f : 0.58f, i == 0 ? 0.96f : 0.58f, alpha));
                context_->DrawTextLayout({bounds.left + 116, bounds.top + (i == 0 ? 31.0f : 65.0f)}, mediaLabels_[i].Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            const auto time = [](double seconds)
            {
                const auto total = static_cast<int>(std::clamp(seconds, 0.0, 359999.0));
                wchar_t buffer[24]{}; swprintf_s(buffer, L"%d:%02d", total / 60, total % 60); return std::wstring(buffer);
            };
            text(media.available ? time(view.MediaPosition()) : L"--:--", {bounds.left + 24, bounds.top + 105, bounds.left + 62, bounds.top + 124}, 0.5f, smallFormat_.Get());
            text(media.durationSeconds > 0 ? time(media.durationSeconds) : L"--:--", {bounds.right - 49, bounds.top + 105, bounds.right - 12, bounds.top + 124}, 0.5f, smallFormat_.Get());
            ViewRect track{bounds.left + 64, bounds.top + 113, bounds.right - 64, bounds.top + 117};
            brush_->SetColor(D2D1::ColorF(1, 1, 1, alpha * 0.12f));
            context_->FillRoundedRectangle(D2D1::RoundedRect(Rect(track), 2, 2), brush_.Get());
            if (media.durationSeconds > 0)
            {
                track.right = track.left + (track.right - track.left) * static_cast<float>(view.MediaPosition() / media.durationSeconds);
                brush_->SetColor(D2D1::ColorF(1, 1, 1, alpha * 0.72f));
                context_->FillRoundedRectangle(D2D1::RoundedRect(Rect(track), 2, 2), brush_.Get());
            }
            for (const auto action : {ViewAction::MediaPrevious, ViewAction::MediaToggle, ViewAction::MediaNext})
            {
                const auto rect = view.MediaButton(action);
                const bool enabled = media.available && (action == ViewAction::MediaPrevious ? media.canPrevious :
                    (action == ViewAction::MediaNext ? media.canNext : media.canToggle));
                const float bright = enabled ? 0.92f : 0.28f;
                brush_->SetColor(D2D1::ColorF(bright, bright, bright, alpha));
                const float bx = (rect.left + rect.right) / 2, by = (rect.top + rect.bottom) / 2;
                if (action == ViewAction::MediaToggle && media.playing)
                {
                    context_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx - 9, by - 12, bx - 3, by + 12), 1.5f, 1.5f), brush_.Get());
                    context_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx + 3, by - 12, bx + 9, by + 12), 1.5f, 1.5f), brush_.Get());
                }
                else
                {
                    const float direction = action == ViewAction::MediaPrevious ? -1.0f : 1.0f;
                    context_->SetTransform(D2D1::Matrix3x2F::Scale(direction, 1) * D2D1::Matrix3x2F::Translation(bx, by));
                    context_->FillGeometry(playGlyph_.Get(), brush_.Get());
                    context_->SetTransform(D2D1::Matrix3x2F::Identity());
                    if (action != ViewAction::MediaToggle)
                        context_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx + direction * 15 - 2, by - 12,
                            bx + direction * 15 + 2, by + 12), 1.5f, 1.5f), brush_.Get());
                }
            }
            brush_->SetColor(D2D1::ColorF(1, 1, 1, alpha * 0.12f));
            const float badgeX = bounds.left + 34, badgeY = bounds.top + 160;
            context_->FillEllipse(D2D1::Ellipse({badgeX, badgeY}, 16, 16), brush_.Get());
            if (appIcon_ && media.available)
                context_->DrawBitmap(appIcon_.Get(), D2D1::RectF(badgeX - 14, badgeY - 14, badgeX + 14, badgeY + 14),
                    alpha, D2D1_INTERPOLATION_MODE_LINEAR);
            else
            {
                brush_->SetColor(D2D1::ColorF(1, 1, 1, alpha * 0.6f));
                context_->DrawLine({badgeX + 2, badgeY - 6}, {badgeX + 2, badgeY + 4}, brush_.Get(), 1.5f);
                context_->DrawLine({badgeX + 2, badgeY - 6}, {badgeX + 6, badgeY - 4}, brush_.Get(), 1.5f);
                context_->FillEllipse(D2D1::Ellipse({badgeX - 1, badgeY + 5}, 3, 2), brush_.Get());
            }
            if (media.sessions.size() > 1)
            {
                brush_->SetColor(D2D1::ColorF(0.14f, 0.14f, 0.14f, alpha));
                context_->FillEllipse(D2D1::Ellipse({badgeX + 13, badgeY + 13}, 8, 8), brush_.Get());
                text(media.sessions.size() > 9 ? L"9+" : std::to_wstring(media.sessions.size()),
                    {badgeX + 9, badgeY + 5, badgeX + 25, badgeY + 23}, 0.8f, smallFormat_.Get());
            }
            text(!media.appError.empty() ? L"打开失败" : (media.available ? (media.playing ? L"播放中" : L"已暂停") : L"无会话"),
                {bounds.right - 62, bounds.top + 153, bounds.right - 16, bounds.top + 174}, 0.42f, smallFormat_.Get());
        }
        else if (page == IslandPage::Volume)
        {
            text(L"默认音频输出", {bounds.left + 24, bounds.top + 58, bounds.right - 24, bounds.top + 82}, 0.5f);
            text(percent, {bounds.left + 24, bounds.top + 104, bounds.right - 24, bounds.top + 135}, 0.95f, titleFormat_.Get());
            progress({bounds.left + 24, bounds.top + 146, bounds.right - 24, bounds.top + 152}, view.Volume().muted ? 0 : view.VolumeLevel());
            volumeControls();
            text(view.Volume().available ? L"每次调整 5%" : L"连接音频输出设备后可使用", {bounds.left + 24, bounds.top + 223, bounds.right - 24, bounds.top + 245}, 0.5f);
        }
        else if (page == IslandPage::Battery)
        {
            const auto& battery = view.Battery();
            text(battery.available ? (battery.charging ? L"正在充电" : (battery.onBattery ? L"使用电池" : L"已连接电源")) : L"当前设备未提供电池状态",
                {bounds.left + 24, bounds.top + 58, bounds.right - 24, bounds.top + 82}, 0.5f);
            if (battery.available) DrawBatteryIcon(view, bounds.left + 24, bounds.top + 111, alpha, 2);
            text(battery.available ? (battery.percent >= 0 ? std::to_wstring(battery.percent) + L"%" : L"电量未知") : L"无电池",
                {bounds.left + 86, bounds.top + 105, bounds.right - 24, bounds.top + 140}, 0.95f, titleFormat_.Get());
            text(L"使用电池且电量 ≤ 20% 时提醒", {bounds.left + 24, bounds.top + 168, bounds.right - 24, bounds.top + 196}, 0.55f);
            text(L"接通电源后自动收起低电量提示", {bounds.left + 24, bounds.top + 202, bounds.right - 24, bounds.top + 230}, 0.4f);
        }
        else
        {
            text(view.SystemSettings() ? (view.SystemError().empty() ? L"启动、显示与材质" : view.SystemError()) : L"让灵动岛按你的节奏运行",
                {bounds.left + 24, bounds.top + 58, bounds.right - 24, bounds.top + 82}, 0.52f);
            const auto row = [&](ViewRect rect, const std::wstring& name, const std::wstring& value)
            {
                button(rect, name);
                text(value, {rect.right - 65, rect.top + 11, rect.right - 8, rect.bottom}, 0.9f);
            };
            row(view.SpeedButton(), view.SystemSettings() ? L"开机启动" : L"动画速度", view.SystemSettings() ?
                (view.StartupAvailable() ? (view.StartupEnabled() ? L"开启" : L"关闭") : L"不可用") : std::to_wstring(view.Parameters().speed).substr(0, 3) + L"×");
            row(view.MotionButton(), view.SystemSettings() ? L"全屏自动隐藏" : L"减少动态效果", view.SystemSettings() ?
                (view.AutoHideFullscreen() ? L"开启" : L"关闭") : (view.ReducedMotion() ? L"开启" : L"关闭"));
            if (view.SystemSettings())
            {
                row(view.GlassButton(), L"桌面模糊", view.GlassAvailable() ? (view.NativeGlassEnabled() ? L"开启" : L"关闭") : L"不可用");
                text(view.GlassStatus(), {bounds.left + 24, bounds.top + 242, bounds.right - 24, bounds.top + 265}, 0.42f, smallFormat_.Get());
            }
            button(view.SettingsPageButton(), view.SystemSettings() ? L"动画" : L"系统");
            text(view.SaveFailed() ? L"设置未能保存" : L"Ctrl+Alt+I", {bounds.left + 116, bounds.bottom - 45, bounds.right - 108, bounds.bottom - 22}, 0.4f, smallFormat_.Get());
        }
        if (page != IslandPage::Home && page != IslandPage::Settings && page != IslandPage::Media) button(view.BackButton(), L"首页");
        if (page != IslandPage::Media) button(view.SettingsButton(), page == IslandPage::Settings ? L"返回" : L"设置");
    }
    context_->PopAxisAlignedClip();
}

void CompositionRenderer::PrepareMedia(const IslandView& view)
{
    const auto& media = view.Media();
    const std::array<std::wstring, 6> strings = {
        media.available ? (media.title.empty() ? L"未提供标题" : media.title) : L"没有正在播放的媒体",
        media.available ? (media.artist.empty() ? L"未提供艺术家" : media.artist) : L"打开支持系统媒体控制的播放器",
        !media.error.empty() ? media.error : (media.available ? (media.playing ? L"正在播放" : L"已暂停") : L"播放器接入系统媒体控制后可用"),
        media.album,
        media.title.empty() ? L"未提供标题" : media.title,
        media.artist.empty() ? (media.playing ? L"正在播放" : L"已暂停") : media.artist};
    const float width = view.Layout().expandedWidth - 156;
    for (size_t i = 0; i < strings.size(); ++i)
    {
        if (mediaLabels_[i] && mediaText_[i] == strings[i] && mediaTextWidth_ == width) continue;
        mediaLabels_[i].Reset();
        const float textWidth = i >= 4 ? std::min(300.0f, view.Layout().expandedWidth) - 108 : (i >= 2 ? width + 116 : width);
        auto* font = i == 0 ? mediaTitleFormat_.Get() : (i == 1 ? mediaArtistFormat_.Get() : bodyFormat_.Get());
        Check(writeFactory_->CreateTextLayout(strings[i].c_str(), static_cast<UINT32>(strings[i].size()),
            font, textWidth, i == 0 ? 32.0f : 24.0f, &mediaLabels_[i]), "Create media text layout");
        Check(mediaLabels_[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "Media text wrapping");
        Ptr<IDWriteInlineObject> ellipsis;
        Check(writeFactory_->CreateEllipsisTrimmingSign(font, &ellipsis), "Media ellipsis");
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        Check(mediaLabels_[i]->SetTrimming(&trimming, ellipsis.Get()), "Media text trimming");
        mediaText_[i] = strings[i];
    }
    mediaTextWidth_ = width;
    if (media.app != iconApp_)
    {
        appIcon_.Reset(); iconApp_ = media.app;
        if (iconApp_ && iconApp_->iconWidth > 0 && iconApp_->iconWidth <= 256 &&
            iconApp_->iconHeight > 0 && iconApp_->iconHeight <= 256 &&
            iconApp_->iconBgra.size() == static_cast<size_t>(iconApp_->iconWidth) * iconApp_->iconHeight * 4)
        {
            const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            Check(context_->CreateBitmap(D2D1::SizeU(iconApp_->iconWidth, iconApp_->iconHeight), iconApp_->iconBgra.data(),
                iconApp_->iconWidth * 4, properties, &appIcon_), "Create media app icon");
        }
    }
    const bool valid = media.available && media.artworkWidth > 0 && media.artworkWidth <= 256 &&
        media.artworkHeight > 0 && media.artworkHeight <= 256 &&
        media.artworkBgra.size() == static_cast<size_t>(media.artworkWidth) * media.artworkHeight * 4;
    if (!valid) { artwork_.Reset(); artworkRevision_ = media.artworkRevision; return; }
    if (artwork_ && artworkRevision_ == media.artworkRevision) return;
    artwork_.Reset();
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    Check(context_->CreateBitmap(D2D1::SizeU(media.artworkWidth, media.artworkHeight), media.artworkBgra.data(),
        media.artworkWidth * 4, &properties, &artwork_), "Upload media artwork");
    artworkRevision_ = media.artworkRevision;
}

void CompositionRenderer::Render(HWND window, const IslandView& view)
{
    if (!swapChain_) CreateDevice(window);
    // 无阻塞探测；未就绪时保留脏标记，由消息循环等待 DXGI 信号后重试。
    const DWORD wait = frameCredit_ ? WAIT_OBJECT_0 : WaitForSingleObject(frameReady_, 0);
    if (wait == WAIT_TIMEOUT) { recoveryPending_ = true; return; }
    if (wait == WAIT_FAILED) ThrowLastError("Wait for DXGI frame");
    frameCredit_ = false;
    const auto begin = std::chrono::steady_clock::now();
    Draw(view);
    const HRESULT drawing = context_->EndDraw();
    if (Recover(drawing)) return;
    Check(drawing, "Direct2D EndDraw");
    UpdateWindowRegion(window, view);
    const HRESULT presented = swapChain_->Present(1, 0);
    if (Recover(presented)) return;
    Check(presented, "Present");
    if (compositionPending_)
    {
        Check(composition_->Commit(), "Commit first composition frame");
        Check(composition_->WaitForCommitCompletion(), "Wait for first composition commit");
        compositionPending_ = false;
    }
    submitted_ = view.Frame();
    recoveryPending_ = false;
    recoveryAttempts_ = 0;
    ++frames_;
    drawMilliseconds_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

bool CompositionRenderer::HitTest(POINT point) const noexcept
{
    // 与系统窗口区域使用同一份几何，避免边缘被区域包含、却又返回 HTTRANSPARENT。
    return hitRegion_ && PtInRegion(hitRegion_, point.x, point.y) != FALSE;
}

std::wstring CompositionRenderer::Diagnostics() const
{
    std::wostringstream text;
    text << (hardware_ ? L"D3D11 硬件渲染" : L"WARP 软件回退") << L"\n设备创建次数：" << deviceGeneration_
        << L"\n提交帧总数：" << frames_ << L"\n最近一次绘制及 Present："
        << std::fixed << std::setprecision(3) << drawMilliseconds_ << L" ms";
    return text.str();
}
}
