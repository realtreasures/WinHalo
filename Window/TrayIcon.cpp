#include "TrayIcon.h"
#include <cwchar>

namespace winisland
{
bool TrayIcon::Create(HWND owner, HICON icon) noexcept
{
    Remove();
    data_ = {};
    data_.cbSize = sizeof(data_);
    data_.hWnd = owner;
    data_.uID = Identifier;
    data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data_.uCallbackMessage = CallbackMessage;
    data_.hIcon = icon;
    wcscpy_s(data_.szTip, L"WinHalo — 单击显示 / 隐藏，右键打开菜单");
    return Restore();
}

bool TrayIcon::Restore() noexcept
{
    // Explorer drops all registrations when its taskbar is recreated.
    registered_ = false;
    if (!data_.hWnd || !data_.hIcon) return false;
    // Also make a repeated TaskbarCreated delivery safe without duplicate IDs.
    Shell_NotifyIconW(NIM_DELETE, &data_);
    if (!Shell_NotifyIconW(NIM_ADD, &data_)) return false;
    data_.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &data_))
    {
        Shell_NotifyIconW(NIM_DELETE, &data_);
        return false;
    }
    registered_ = true;
    return true;
}

void TrayIcon::Remove() noexcept
{
    if (data_.hWnd) Shell_NotifyIconW(NIM_DELETE, &data_);
    data_ = {};
    registered_ = false;
}
}
