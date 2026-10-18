#pragma once
#include "Win32Error.h"
#include <filesystem>
#include <memory>
#include <shlobj.h>
#include <vector>

namespace winisland
{
inline std::filesystem::path DataDirectory()
{
    const DWORD required = GetEnvironmentVariableW(L"WINHALO_DATA_DIR", nullptr, 0);
    if (required)
    {
        std::vector<wchar_t> buffer(required);
        const DWORD copied = GetEnvironmentVariableW(L"WINHALO_DATA_DIR", buffer.data(), required);
        if (!copied || copied >= required) ThrowLastError("WINHALO_DATA_DIR");
        return std::filesystem::path(buffer.data());
    }
    PWSTR directory = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &directory);
    const auto release = [](wchar_t* value) { CoTaskMemFree(value); };
    std::unique_ptr<wchar_t, decltype(release)> owned(directory, release);
    if (FAILED(result)) throw std::system_error(static_cast<int>(result), std::system_category(), "LocalAppData");
    return std::filesystem::path(owned.get()) / L"WinHalo";
}
}
