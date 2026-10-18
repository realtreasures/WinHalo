#include "MediaAppResolver.h"
#include "../Core/Platform.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <tlhelp32.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <algorithm>
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Propsys.lib")
#pragma comment(lib, "Gdi32.lib")

namespace winisland
{
namespace
{
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
bool Same(const std::wstring& a, const std::wstring& b)
{
    return !a.empty() && !b.empty() && CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
std::wstring ProcessPath(DWORD id)
{
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
    if (!process) return {};
    wchar_t path[32768]{}; DWORD size = static_cast<DWORD>(std::size(path));
    const bool valid = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE;
    CloseHandle(process);
    return valid ? std::wstring(path, size) : std::wstring{};
}
bool WindowSource(HWND window, const std::wstring& source)
{
    Ptr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&store)))) return false;
    PROPVARIANT value{};
    const HRESULT result = store->GetValue(PKEY_AppUserModel_ID, &value);
    const bool matches = SUCCEEDED(result) && value.vt == VT_LPWSTR && value.pwszVal && Same(value.pwszVal, source);
    PropVariantClear(&value);
    return matches;
}
struct WindowSearch { const MediaAppInfo& app; HWND found = nullptr; bool byExecutable = false; };
BOOL CALLBACK FindAppWindow(HWND window, LPARAM parameter)
{
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD id = 0; GetWindowThreadProcessId(window, &id);
    // GetWindowTextLength sends a synchronous message for same-process windows.
    // The media worker must never wait for its own UI (or a blocked fixture main thread).
    if (id == GetCurrentProcessId()) return TRUE;
    if (GetWindow(window, GW_OWNER) || (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) ||
        GetWindowTextLengthW(window) == 0) return TRUE;
    if (search.byExecutable ? Same(ProcessPath(id), search.app.executable) : WindowSource(window, search.app.source))
    {
        search.found = window;
        return FALSE;
    }
    return TRUE;
}
std::wstring FindExecutable(const std::wstring& source)
{
    MediaAppInfo identity; identity.source = source;
    WindowSearch search{identity};
    EnumWindows(FindAppWindow, reinterpret_cast<LPARAM>(&search));
    if (search.found)
    {
        DWORD id = 0; GetWindowThreadProcessId(search.found, &id);
        return ProcessPath(id);
    }
    const HANDLE processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processes == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    std::wstring result;
    if (Process32FirstW(processes, &entry)) do
    {
        if (Same(entry.szExeFile, std::filesystem::path(source).filename().wstring()))
        {
            const auto path = ProcessPath(entry.th32ProcessID);
            if (Same(path, source) || Same(entry.szExeFile, source)) { result = path; break; }
        }
    } while (Process32NextW(processes, &entry));
    CloseHandle(processes);
    return result;
}
void ReadIcon(HICON icon, MediaAppInfo& app)
{
    if (!icon) return;
    Ptr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return;
    Ptr<IWICBitmap> bitmap;
    Ptr<IWICBitmapScaler> scaler;
    Ptr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateBitmapFromHICON(icon, &bitmap)) || FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(bitmap.Get(), 32, 32, WICBitmapInterpolationModeFant)) ||
        FAILED(factory->CreateFormatConverter(&converter)) || FAILED(converter->Initialize(scaler.Get(),
            GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return;
    std::vector<unsigned char> pixels(32 * 32 * 4);
    if (SUCCEEDED(converter->CopyPixels(nullptr, 32 * 4, static_cast<UINT>(pixels.size()), pixels.data())))
    { app.iconBgra = std::move(pixels); app.iconWidth = app.iconHeight = 32; }
}
bool ActivateWindow(HWND target)
{
    return SetForegroundWindow(target) != FALSE;
}
}

MediaAppInfo ResolveMediaApp(const std::wstring& source)
{
    MediaAppInfo app; app.source = source; app.name = source.empty() ? L"播放器" : source;
    if (source.empty()) return app;
    Ptr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, 0, source.c_str(), IID_PPV_ARGS(&item))))
    {
        app.shellRegistered = true;
        PWSTR name = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)))
        { app.name = name; CoTaskMemFree(name); }
        Ptr<IShellItemImageFactory> image;
        HBITMAP bitmap = nullptr;
        if (SUCCEEDED(item.As(&image)) && SUCCEEDED(image->GetImage({32, 32}, SIIGBF_ICONONLY, &bitmap)))
        {
            Ptr<IWICImagingFactory> factory; Ptr<IWICBitmap> wic; Ptr<IWICFormatConverter> converter;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
                SUCCEEDED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUsePremultipliedAlpha, &wic)) &&
                SUCCEEDED(factory->CreateFormatConverter(&converter)) && SUCCEEDED(converter->Initialize(wic.Get(),
                    GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
            {
                UINT width = 0, height = 0; wic->GetSize(&width, &height);
                if (width > 0 && width <= 256 && height > 0 && height <= 256)
                {
                    app.iconBgra.resize(static_cast<size_t>(width) * height * 4);
                    if (SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(app.iconBgra.size()), app.iconBgra.data())))
                    { app.iconWidth = width; app.iconHeight = height; }
                    else app.iconBgra.clear();
                }
            }
            DeleteObject(bitmap);
        }
    }
    app.executable = FindExecutable(source);
    if (!app.executable.empty())
    {
        if (!app.shellRegistered) app.name = std::filesystem::path(app.executable).stem().wstring();
        if (app.iconBgra.empty())
        {
            SHFILEINFOW file{};
            if (SHGetFileInfoW(app.executable.c_str(), 0, &file, sizeof(file), SHGFI_ICON | SHGFI_SMALLICON))
            { ReadIcon(file.hIcon, app); DestroyIcon(file.hIcon); }
        }
    }
    return app;
}

bool OpenMediaApp(const MediaAppInfo& app)
{
    WindowSearch search{app};
    EnumWindows(FindAppWindow, reinterpret_cast<LPARAM>(&search));
    if (!search.found && !app.executable.empty())
    { search.byExecutable = true; EnumWindows(FindAppWindow, reinterpret_cast<LPARAM>(&search)); }
    if (search.found)
    {
        DWORD_PTR restored = 0;
        // Foreground activation fails while an asynchronous show is still queued.
        // Ask the target UI thread to restore first, with a bounded wait for hung players.
        if (!SendMessageTimeoutW(search.found, WM_SYSCOMMAND, SC_RESTORE, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &restored)) return false;
        return ActivateWindow(search.found);
    }
    // Never interpret an untrusted SMTC source string as a command or executable path.
    if (app.shellRegistered)
    {
        Ptr<IApplicationActivationManager> activation;
        DWORD process = 0;
        if (SUCCEEDED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&activation))) && SUCCEEDED(activation->ActivateApplication(app.source.c_str(), nullptr,
                AO_NONE, &process))) return true;
        const auto target = L"shell:AppsFolder\\" + app.source;
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    }
    // The executable was resolved from a running process, rather than supplied by metadata.
    if (!app.executable.empty() && GetFileAttributesW(app.executable.c_str()) != INVALID_FILE_ATTRIBUTES)
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", app.executable.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    return false;
}
}
