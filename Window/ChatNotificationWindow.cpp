#include "ChatNotificationWindow.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace winisland
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr wchar_t CardClass[] = L"WinHalo.ChatNotification";
constexpr wchar_t ShadowClass[] = L"WinHalo.ChatNotificationShadow";
constexpr UINT_PTR AnimationTimer = 1, ExpiryTimer = 2;
constexpr float Margin = 12, Width = 360, Height = 124, Radius = 24;
constexpr ULONGLONG AnimationMilliseconds = 220;
void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("Notification rendering failed"); }
float Ease(float t) { return 1.0f - std::pow(1.0f - std::clamp(t, 0.0f, 1.0f), 3.0f); }

struct Surface
{
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    SIZE size{};
    ~Surface() { Reset(); }
    void Reset()
    {
        if (dc && previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        dc = nullptr; bitmap = nullptr; previous = nullptr; size = {};
    }
    void Resize(SIZE wanted)
    {
        if (wanted.cx == size.cx && wanted.cy == size.cy) return;
        Reset();
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = wanted.cx;
        info.bmiHeader.biHeight = -wanted.cy;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        if (dc) bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap) { Reset(); throw std::runtime_error("Notification bitmap creation failed"); }
        previous = SelectObject(dc, bitmap);
        if (!previous || previous == HGDI_ERROR)
        { previous = nullptr; Reset(); throw std::runtime_error("Notification bitmap selection failed"); }
        size = wanted;
    }
};
}

struct ChatNotificationWindow::Impl
{
    HINSTANCE instance;
    HWND window = nullptr, shadow = nullptr;
    ChatNotification notification;
    OpenHandler open;
    std::function<void()> dismissed;
    RECT anchor{};
    UINT dpi = 96;
    POINT origin{};
    Surface cardSurface, shadowSurface;
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1DCRenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteFactory> writeFactory;
    ComPtr<IDWriteTextFormat> sourceFont, titleFont, bodyFont, fallbackFont;
    bool visible = false, suppressed = false, showBody = true, reducedMotion = false;
    bool hovering = false, closeHover = false, closing = false, animating = false;
    bool pressedClose = false, pressed = false;
    std::uint64_t revision = 0, pressedRevision = 0;
    size_t pending = 0;
    ULONGLONG deadline = 0, remaining = 5000, animationStart = 0;
    float opacity = 1, animationFrom = 0;

    explicit Impl(HINSTANCE value) : instance(value) {}
    ~Impl() { Close(); }
    float Scale() const { return static_cast<float>(dpi) / 96; }
    int Pixels(float dip) const { return static_cast<int>(std::lround(dip * Scale())); }
    RECT CardPixels() const { return { Pixels(Margin), Pixels(Margin), Pixels(Margin + Width), Pixels(Margin + Height) }; }
    bool CardHit(POINT point) const
    {
        const float x = static_cast<float>(point.x) / Scale() - Margin;
        const float y = static_cast<float>(point.y) / Scale() - Margin;
        if (x < 0 || y < 0 || x >= Width || y >= Height) return false;
        const float cx = std::clamp(x, Radius, Width - Radius);
        const float cy = std::clamp(y, Radius, Height - Radius);
        const float dx = x - cx, dy = y - cy;
        return dx * dx + dy * dy <= Radius * Radius;
    }
    bool CloseHit(POINT point) const
    {
        const float x = static_cast<float>(point.x) / Scale();
        const float y = static_cast<float>(point.y) / Scale();
        return x >= 331 && x <= 364 && y >= 20 && y <= 53;
    }
    static LRESULT CALLBACK ShadowProcedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (message == WM_NCHITTEST) return HTTRANSPARENT;
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    static LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
        try { return self->Message(hwnd, message, wParam, lParam); }
        catch (...) { self->HideNow(false); return 0; }
    }
    void EnsureWindow()
    {
        if (window) return;
        WNDCLASSEXW type{ sizeof(type) };
        type.hInstance = instance;
        type.lpfnWndProc = Procedure;
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.lpszClassName = CardClass;
        if (!RegisterClassExW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Notification window class creation failed");
        type.lpfnWndProc = ShadowProcedure; type.lpszClassName = ShadowClass;
        if (!RegisterClassExW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Notification shadow class creation failed");
        constexpr DWORD flags = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        shadow = CreateWindowExW(flags | WS_EX_TRANSPARENT, ShadowClass, L"", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
        window = CreateWindowExW(flags, CardClass, L"WinHalo 消息", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, instance, this);
        if (!window || !shadow) { Close(); throw std::runtime_error("Notification window creation failed"); }
    }
    void EnsureRenderer()
    {
        if (!factory) Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()));
        if (!target)
        {
            const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            Check(factory->CreateDCRenderTarget(&properties, &target));
            Check(target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush));
        }
        if (!writeFactory) Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(writeFactory.GetAddressOf())));
        auto font = [&](ComPtr<IDWriteTextFormat>& value, float size, DWRITE_FONT_WEIGHT weight)
        {
            if (value) return;
            Check(writeFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &value));
            Check(value->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP));
        };
        font(sourceFont, 10.5f, DWRITE_FONT_WEIGHT_MEDIUM);
        font(titleFont, 15.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        font(bodyFont, 12.5f, DWRITE_FONT_WEIGHT_NORMAL);
        font(fallbackFont, 18.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        Check(sourceFont->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));
        Check(titleFont->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));
        Check(bodyFont->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 18.0f, 14.0f));
    }
    void Layout()
    {
        MONITORINFO monitor{ sizeof(monitor) };
        GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTOPRIMARY), &monitor);
        const int width = Pixels(Width + Margin * 2), height = Pixels(Height + Margin * 2);
        const int x = (anchor.left + anchor.right - width) / 2;
        // The transparent margin occupies the 8 DIP gap; the painted card begins below the anchor.
        const int y = anchor.bottom + Pixels(8 - Margin);
        origin.x = std::clamp<LONG>(x, monitor.rcWork.left, std::max(monitor.rcWork.left, monitor.rcWork.right - width));
        origin.y = std::clamp<LONG>(y, monitor.rcWork.top, std::max(monitor.rcWork.top, monitor.rcWork.bottom - height));
        if (cardSurface.size.cx != width || cardSurface.size.cy != height)
        {
            // Unbind the DC render target before deleting a DPI-sized DIB/DC.
            brush.Reset(); target.Reset();
        }
        cardSurface.Resize({ width, height }); shadowSurface.Resize({ width, height });
    }
    void Color(float r, float g, float b, float alpha = 1) { brush->SetColor(D2D1::ColorF(r, g, b, alpha)); }
    void Text(const std::wstring& text, IDWriteTextFormat* format, D2D1_RECT_F rect,
        float red, float green, float blue)
    {
        if (text.empty()) return;
        ComPtr<IDWriteTextLayout> layout;
        const auto length = static_cast<UINT32>(std::min<size_t>(text.size(), 4096));
        Check(writeFactory->CreateTextLayout(text.data(), length, format,
            std::max(1.0f, rect.right - rect.left), std::max(1.0f, rect.bottom - rect.top), &layout));
        ComPtr<IDWriteInlineObject> ellipsis;
        Check(writeFactory->CreateEllipsisTrimmingSign(format, &ellipsis));
        DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        Check(layout->SetTrimming(&trim, ellipsis.Get()));
        Color(red, green, blue);
        target->DrawTextLayout({ rect.left, rect.top }, layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void Begin(Surface& surface)
    {
        RECT bounds{ 0, 0, surface.size.cx, surface.size.cy };
        Check(target->BindDC(surface.dc, &bounds));
        target->SetDpi(96, 96);
        target->BeginDraw();
        target->SetTransform(D2D1::Matrix3x2F::Scale(Scale(), Scale()));
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        target->Clear(D2D1::ColorF(0, 0));
    }
    void End()
    {
        const HRESULT result = target->EndDraw();
        if (FAILED(result)) { brush.Reset(); target.Reset(); Check(result); }
    }
    void Draw()
    {
        EnsureRenderer();
        Begin(shadowSurface);
        // A distinct WS_EX_TRANSPARENT shadow window lets all shadow pixels pass clicks through.
        for (int ring = 10; ring >= 1; --ring)
        {
            const float spread = static_cast<float>(ring);
            Color(0, 0, 0, 0.018f);
            target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(
                Margin - spread, Margin - spread + 2, Margin + Width + spread, Margin + Height + spread + 2),
                Radius + spread, Radius + spread), brush.Get());
        }
        End();
        Begin(cardSurface);
        const auto card = D2D1::RoundedRect(D2D1::RectF(Margin, Margin, Margin + Width, Margin + Height), Radius, Radius);
        Color(0.035f, 0.038f, 0.046f, 0.965f);
        target->FillRoundedRectangle(card, brush.Get());
        Color(1, 1, 1, 0.15f); target->DrawRoundedRectangle(card, brush.Get(), 1);
        Color(1, 1, 1, 0.055f);
        target->DrawLine({ Margin + 28, Margin + 1 }, { Margin + Width - 28, Margin + 1 }, brush.Get(), 1);
        const bool wechat = notification.app == ChatApp::WeChat;
        Color(wechat ? 0.08f : 0.13f, wechat ? 0.65f : 0.45f, wechat ? 0.34f : 0.91f, 0.16f);
        const auto iconRect = D2D1::RoundedRect(D2D1::RectF(29, 38, 73, 82), 13, 13);
        target->FillRoundedRectangle(iconRect, brush.Get());
        const auto app = notification.application;
        bool iconDrawn = false;
        if (app && app->iconWidth && app->iconHeight && app->iconWidth <= 1024 && app->iconHeight <= 1024 &&
            app->iconBgra.size() >= static_cast<size_t>(app->iconWidth) * app->iconHeight * 4)
        {
            ComPtr<ID2D1Bitmap> bitmap;
            const auto properties = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (SUCCEEDED(target->CreateBitmap(D2D1::SizeU(app->iconWidth, app->iconHeight), app->iconBgra.data(),
                app->iconWidth * 4, &properties, &bitmap)))
            {
                target->DrawBitmap(bitmap.Get(), D2D1::RectF(33, 42, 69, 78));
                iconDrawn = true;
            }
        }
        if (!iconDrawn)
        {
            Text(wechat ? L"微" : L"Q", fallbackFont.Get(), D2D1::RectF(40, 45, 67, 75),
                wechat ? 0.3f : 0.5f, wechat ? 0.95f : 0.75f, wechat ? 0.5f : 1.0f);
        }
        std::wstring source = notification.appName.empty() ? (wechat ? L"微信" : L"QQ") : notification.appName;
        if (notification.test) source += L" · 测试";
        if (pending) source += L" · +" + std::to_wstring(pending);
        Text(source, sourceFont.Get(), D2D1::RectF(86, 25, 326, 41), 0.57f, 0.61f, 0.66f);
        Text(notification.title.empty() ? L"收到一条新消息" : notification.title, titleFont.Get(),
            D2D1::RectF(86, 44, 326, 66), 0.97f, 0.98f, 1);
        Text(showBody && !notification.body.empty() ? notification.body : L"收到一条新消息",
            bodyFont.Get(), D2D1::RectF(86, 73, 338, 109), 0.74f, 0.77f, 0.82f);
        if (closeHover || (pressed && pressedClose))
        {
            Color(1, 1, 1, pressed ? 0.12f : 0.065f);
            target->FillEllipse(D2D1::Ellipse({ 347.5f, 36.5f }, 13.5f, 13.5f), brush.Get());
        }
        Color(0.64f, 0.67f, 0.72f);
        target->DrawLine({ 344, 33 }, { 351, 40 }, brush.Get(), 1.5f);
        target->DrawLine({ 351, 33 }, { 344, 40 }, brush.Get(), 1.5f);
        End();
    }
    void Present()
    {
        if (!visible || !window) return;
        const int offset = reducedMotion ? 0 : Pixels(10 * (1 - opacity));
        POINT position{ origin.x, origin.y + offset }, source{};
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(std::clamp(opacity, 0.0f, 1.0f) * 255)), AC_SRC_ALPHA };
        if (!UpdateLayeredWindow(shadow, nullptr, &position, &shadowSurface.size,
                shadowSurface.dc, &source, 0, &blend, ULW_ALPHA) ||
            !UpdateLayeredWindow(window, nullptr, &position, &cardSurface.size,
                cardSurface.dc, &source, 0, &blend, ULW_ALPHA))
            throw std::runtime_error("Notification presentation failed");
        SetWindowPos(shadow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    void ArmExpiry()
    {
        if (!visible || hovering || closing) return;
        KillTimer(window, ExpiryTimer);
        remaining = std::max<ULONGLONG>(remaining, 1);
        deadline = GetTickCount64() + remaining;
        SetTimer(window, ExpiryTimer, static_cast<UINT>(remaining), nullptr);
    }
    void Hover(bool enter, POINT point = {})
    {
        if (!visible || closing) return;
        if (enter && !hovering)
        {
            hovering = true;
            const auto now = GetTickCount64();
            remaining = deadline > now ? deadline - now : 1;
            KillTimer(window, ExpiryTimer);
            TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, window, 0 };
            TrackMouseEvent(&tracking);
        }
        else if (!enter && hovering) { hovering = false; ArmExpiry(); }
        const bool next = enter && CloseHit(point);
        if (closeHover != next) { closeHover = next; Draw(); Present(); }
    }
    void HideNow(bool notify)
    {
        const bool wasVisible = visible;
        visible = false; animating = false; closing = false; hovering = false; closeHover = false;
        pressed = false; ++revision;
        if (window)
        {
            KillTimer(window, AnimationTimer); KillTimer(window, ExpiryTimer);
            if (GetCapture() == window) ReleaseCapture();
            ShowWindow(window, SW_HIDE);
        }
        if (shadow) ShowWindow(shadow, SW_HIDE);
        notification = {}; open = {}; pending = 0;
        // Release DIB contents as well as the strings when a private message is hidden.
        brush.Reset(); target.Reset(); cardSurface.Reset(); shadowSurface.Reset();
        auto callback = notify && wasVisible ? dismissed : std::function<void()>{};
        if (callback) callback();
    }
    void Dismiss(bool immediate)
    {
        if (!visible) return;
        KillTimer(window, ExpiryTimer);
        pressed = false; ++revision;
        if (GetCapture() == window) ReleaseCapture();
        if (immediate || reducedMotion) { HideNow(true); return; }
        if (closing) return;
        closing = true; animating = true; animationStart = GetTickCount64(); animationFrom = opacity;
        SetTimer(window, AnimationTimer, 16, nullptr);
    }
    void Show(const ChatNotification& value, RECT at, UINT scale, int duration, bool body, bool reduced,
        size_t count, OpenHandler handler)
    {
        if (suppressed) return;
        const bool retainHover = visible && hovering;
        EnsureWindow();
        KillTimer(window, AnimationTimer); KillTimer(window, ExpiryTimer);
        if (GetCapture() == window) ReleaseCapture();
        notification = value; open = std::move(handler); anchor = at; dpi = std::clamp<UINT>(scale, 96, 768);
        showBody = body; reducedMotion = reduced; pending = count; ++revision;
        visible = true; closing = false; hovering = false; closeHover = false; pressed = false;
        remaining = static_cast<ULONGLONG>(std::clamp(duration, 1, 60)) * 1000;
        Layout(); Draw();
        animating = !reducedMotion; opacity = reducedMotion ? 1.0f : 0.0f;
        animationFrom = 0; animationStart = GetTickCount64();
        Present();
        ShowWindow(shadow, SW_SHOWNOACTIVATE); ShowWindow(window, SW_SHOWNOACTIVATE);
        ArmExpiry();
        // A notification update or a newly appearing card can be under a
        // stationary pointer. Do not require another WM_MOUSEMOVE to pause it.
        POINT pointer{};
        const bool hasPointer = GetCursorPos(&pointer) && ScreenToClient(window, &pointer);
        if (retainHover || (hasPointer && CardHit(pointer))) Hover(true, pointer);
        if (animating) SetTimer(window, AnimationTimer, 16, nullptr);
    }
    void Close()
    {
        dismissed = {};
        HideNow(false);
        if (window) DestroyWindow(window);
        if (shadow) DestroyWindow(shadow);
        window = nullptr; shadow = nullptr;
    }
    LRESULT Message(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto point = [&] { return POINT{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) }; };
        switch (message)
        {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_NCHITTEST:
        {
            POINT test = point(); ScreenToClient(hwnd, &test);
            return visible && CardHit(test) ? HTCLIENT : HTTRANSPARENT;
        }
        case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE;
        case WM_MOUSEMOVE: Hover(CardHit(point()), point()); return 0;
        case WM_MOUSELEAVE: Hover(false); return 0;
        case WM_LBUTTONDOWN:
            if (visible && !closing && CardHit(point()))
            { pressed = true; pressedClose = CloseHit(point()); pressedRevision = revision; SetCapture(hwnd); Draw(); Present(); }
            return 0;
        case WM_LBUTTONUP:
        {
            const bool activate = visible && !closing && pressed && revision == pressedRevision &&
                CardHit(point()) && CloseHit(point()) == pressedClose;
            const bool close = pressedClose;
            pressed = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            if (activate)
            {
                const ChatNotification copy = notification;
                auto callback = open;
                Dismiss(true);
                if (!close && callback) callback(copy);
            }
            else if (visible) { Draw(); Present(); }
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (pressed) { pressed = false; if (visible) { Draw(); Present(); } }
            return 0;
        case WM_TIMER:
            if (wParam == ExpiryTimer)
            {
                KillTimer(hwnd, ExpiryTimer);
                if (!visible || hovering || closing) return 0;
                const auto now = GetTickCount64();
                if (now < deadline) { remaining = deadline - now; ArmExpiry(); }
                else Dismiss(false);
                return 0;
            }
            if (wParam == AnimationTimer)
            {
                if (!visible || !animating) { KillTimer(hwnd, AnimationTimer); return 0; }
                const float time = static_cast<float>(GetTickCount64() - animationStart) / AnimationMilliseconds;
                opacity = closing ? animationFrom * (1 - Ease(time)) : Ease(time);
                if (time >= 1)
                {
                    animating = false; KillTimer(hwnd, AnimationTimer);
                    if (closing) { HideNow(true); return 0; }
                    opacity = 1;
                }
                Present();
                return 0;
            }
            break;
        case WM_DPICHANGED:
            dpi = std::clamp<UINT>(HIWORD(wParam), 96, 768);
            if (visible) { Layout(); Draw(); Present(); }
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PRINTCLIENT:
            if (cardSurface.dc && wParam)
                BitBlt(reinterpret_cast<HDC>(wParam), 0, 0, cardSurface.size.cx, cardSurface.size.cy,
                    cardSurface.dc, 0, 0, SRCCOPY);
            return 0;
        case WM_CLOSE: Dismiss(true); return 0;
        case WM_NCDESTROY: SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); window = nullptr; return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
};

ChatNotificationWindow::ChatNotificationWindow(HINSTANCE instance) : impl_(std::make_unique<Impl>(instance)) {}
ChatNotificationWindow::~ChatNotificationWindow() = default;
void ChatNotificationWindow::Show(const ChatNotification& message, RECT anchor, UINT dpi,
    int seconds, bool showBody, bool reducedMotion, size_t count, OpenHandler open)
{
    try { impl_->Show(message, anchor, dpi, seconds, showBody, reducedMotion, count, std::move(open)); }
    catch (...) { impl_->HideNow(false); throw; }
}
void ChatNotificationWindow::Dismiss(bool immediate) { impl_->Dismiss(immediate); }
void ChatNotificationWindow::SetAnchor(RECT anchor, UINT dpi)
{
    if (EqualRect(&impl_->anchor, &anchor) && dpi == impl_->dpi) return;
    impl_->anchor = anchor; impl_->dpi = std::clamp<UINT>(dpi, 96, 768);
    if (impl_->visible) { impl_->Layout(); impl_->Draw(); impl_->Present(); }
}
void ChatNotificationWindow::SetPendingCount(size_t count)
{
    if (impl_->pending == count) return;
    impl_->pending = count;
    if (impl_->visible) { impl_->Draw(); impl_->Present(); }
}
void ChatNotificationWindow::SetSuppressed(bool value)
{
    impl_->suppressed = value;
    if (value) impl_->HideNow(true);
}
void ChatNotificationWindow::SetDismissHandler(std::function<void()> handler) { impl_->dismissed = std::move(handler); }
bool ChatNotificationWindow::Visible() const noexcept { return impl_->visible; }
HWND ChatNotificationWindow::Handle() const noexcept { return impl_->window; }
void ChatNotificationWindow::Close() { impl_->Close(); }
}
