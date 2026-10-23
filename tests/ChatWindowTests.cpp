#include "../Window/ChatNotificationWindow.h"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace winisland;
namespace
{
void Require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
int Pixel(int dip, UINT dpi) { return MulDiv(dip, static_cast<int>(dpi), 96); }
LPARAM Point(int x, int y) { return MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y)); }
void Click(HWND window, int x, int y)
{
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, Point(x, y));
    SendMessageW(window, WM_LBUTTONUP, 0, Point(x, y));
}
std::vector<std::uint32_t> Pixels(HWND window, SIZE size)
{
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size.cx; info.bmiHeader.biHeight = -size.cy;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    void* data = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &data, nullptr, 0);
    Require(dc && bitmap && data, "capture allocation failed");
    HGDIOBJ old = SelectObject(dc, bitmap);
    SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    const auto begin = static_cast<std::uint32_t*>(data);
    std::vector<std::uint32_t> pixels(begin, begin + static_cast<size_t>(size.cx) * size.cy);
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
    return pixels;
}
void NoQuit()
{
    MSG message{};
    Require(!PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE), "popup posted WM_QUIT");
}
void SavePreview(const std::vector<std::uint32_t>& pixels, SIZE size, UINT dpi)
{
    std::filesystem::create_directories(L"artifacts");
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size() * sizeof(std::uint32_t));
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info); info.biWidth = size.cx; info.biHeight = -size.cy;
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    std::ofstream output(std::filesystem::path(L"artifacts") /
        (L"chat-window-dpi-" + std::to_wstring(dpi) + L".bmp"), std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size() * sizeof(std::uint32_t)));
    Require(output.good(), "preview write failed");
}
}

int wmain()
{
    try
    {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const HWND originalForeground = GetForegroundWindow();
        const DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        ChatNotificationWindow window(GetModuleHandleW(nullptr));
        ChatNotification value;
        value.id = 42; value.app = ChatApp::WeChat; value.appName = L"微信";
        value.title = L"WinHalo 测试联系人";
        value.body = L"这是隔离测试消息，不访问 QQ、微信或真实通知。第二行会保持清晰并自动截断。";
        value.test = true;
        int dismissed = 0, opened = 0;
        window.SetDismissHandler([&] { ++dismissed; });
        auto open = [&](const ChatNotification& item)
        { Require(item.id == value.id, "wrong notification opened"); ++opened; };
        const RECT anchor{ 90, 60, 490, 80 };
        for (const UINT dpi : { 96u, 120u, 144u, 192u })
        {
            window.Show(value, anchor, dpi, 5, true, true, 2, open);
            const HWND hwnd = window.Handle();
            Require(hwnd && window.Visible() && IsWindowVisible(hwnd), "card did not show");
            const auto ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            Require((ex & (WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST)) ==
                (WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST), "popup styles changed");
            Require(!(ex & WS_EX_APPWINDOW), "card appeared as app window");
            Require(SendMessageW(hwnd, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE, "card can activate");
            Require(GetForegroundWindow() == originalForeground, "notification stole foreground focus");
            RECT rectangle{}; GetWindowRect(hwnd, &rectangle);
            const SIZE size{ rectangle.right - rectangle.left, rectangle.bottom - rectangle.top };
            Require(size.cx == Pixel(384, dpi) && size.cy == Pixel(148, dpi), "DPI surface size incorrect");
            Require(SendMessageW(hwnd, WM_NCHITTEST, 0, Point(rectangle.left, rectangle.top)) == HTTRANSPARENT,
                "transparent margin intercepted input");
            Require(SendMessageW(hwnd, WM_NCHITTEST, 0,
                Point(rectangle.left + Pixel(12, dpi), rectangle.top + Pixel(12, dpi))) == HTTRANSPARENT,
                "rounded corner intercepted input");
            Require(SendMessageW(hwnd, WM_NCHITTEST, 0,
                Point(rectangle.left + Pixel(180, dpi), rectangle.top + Pixel(65, dpi))) == HTCLIENT,
                "card body lost input");
            const auto pixels = Pixels(hwnd, size);
            SavePreview(pixels, size, dpi);
            auto alpha = [&](int x, int y) { return pixels[static_cast<size_t>(y) * size.cx + x] >> 24; };
            Require(alpha(0, 0) == 0 && alpha(Pixel(180, dpi), Pixel(5, dpi)) == 0,
                "card exterior is not transparent");
            Require(alpha(Pixel(180, dpi), Pixel(120, dpi)) >= 240, "card body lost premultiplied alpha");
            size_t intermediate = 0;
            for (int y = Pixel(12, dpi); y < Pixel(35, dpi); ++y)
                for (int x = Pixel(12, dpi); x < Pixel(35, dpi); ++x)
                { const auto a = alpha(x, y); if (a > 0 && a < 180) ++intermediate; }
            Require(intermediate > 0, "rounded corner lacks antialiasing");
            const HWND shadow = FindWindowW(L"WinHalo.ChatNotificationShadow", L"");
            Require(shadow && (GetWindowLongPtrW(shadow, GWL_EXSTYLE) & WS_EX_TRANSPARENT),
                "shadow does not pass input through");
            Click(hwnd, Pixel(348, dpi), Pixel(36, dpi));
            Require(!window.Visible() && opened == 0, "close triggered app launch");
            std::cout << "PASS simulated DPI " << dpi << ": alpha, AA, hit testing, styles, focus, close\n";
        }
        Require(dismissed == 4, "dismiss callback not invoked once per card");
        window.Show(value, anchor, 96, 5, true, true, 0, open);
        Click(window.Handle(), 170, 68);
        Require(opened == 1 && dismissed == 5 && !window.Visible(), "body click did not open and dismiss");
        window.Show(value, anchor, 96, 5, true, true, 0, open);
        SendMessageW(window.Handle(), WM_LBUTTONDOWN, MK_LBUTTON, Point(170, 68));
        ++value.id;
        window.Show(value, anchor, 96, 5, true, true, 0, open);
        SendMessageW(window.Handle(), WM_LBUTTONUP, 0, Point(170, 68));
        Require(opened == 1 && window.Visible(), "stale mouse-up launched replacement notification");
        window.SetSuppressed(true);
        Require(!window.Visible(), "suppression did not hide card");
        window.Show(value, anchor, 96, 5, true, true, 0, open);
        Require(!window.Visible(), "suppressed card became visible");
        window.SetSuppressed(false);
        Require(!window.Visible(), "unsuppression replayed private message");
        window.Show(value, anchor, 96, 1, false, true, 0, open);
        // Synchronous message simulation does not move the user's pointer. No message
        // pumping here: real TME_LEAVE depends on the user's actual cursor position.
        SendMessageW(window.Handle(), WM_MOUSEMOVE, 0, Point(170, 68));
        value.body = L"Updated while the pointer remains over the card";
        window.Show(value, anchor, 96, 1, false, true, 0, open);
        Sleep(1100);
        SendMessageW(window.Handle(), WM_TIMER, 2, 0);
        Require(window.Visible(), "notification update lost stationary hover or stale expiry dismissed hovered card");
        SendMessageW(window.Handle(), WM_MOUSELEAVE, 0, 0);
        SendMessageW(window.Handle(), WM_TIMER, 2, 0);
        Require(window.Visible(), "leave failed to resume remaining display time");
        Sleep(1100);
        SendMessageW(window.Handle(), WM_TIMER, 2, 0);
        Require(!window.Visible(), "resumed expiry failed to dismiss");
        window.Show(value, anchor, 96, 5, false, false, 0, open);
        Sleep(240);
        SendMessageW(window.Handle(), WM_TIMER, 1, 0);
        Require(window.Visible(), "entry animation unexpectedly dismissed card");
        window.Dismiss(false);
        Require(window.Visible(), "exit animation did not retain card until completion");
        Sleep(240);
        SendMessageW(window.Handle(), WM_TIMER, 1, 0);
        Require(!window.Visible(), "exit animation did not complete");
        window.Close(); window.Close(); NoQuit();
        Require(GetForegroundWindow() == originalForeground, "test stole foreground focus");
        const DWORD gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        Require(gdiAfter <= gdiBefore + 4, "popup leaked GDI resources");
        std::cout << "PASS body/close/revision, suppression, paused deadline, no quit; GDI "
            << gdiBefore << " -> " << gdiAfter << "\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
