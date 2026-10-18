#include "StartupRegistration.h"
#include <stdexcept>
#include <vector>

namespace winisland
{
namespace
{
constexpr wchar_t RunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t TestPrefix[] = L"Software\\WinHalo\\Tests\\";
struct RegistryKey
{
    HKEY value = nullptr;
    ~RegistryKey() { if (value) RegCloseKey(value); }
};
StartupStatus Failure(const wchar_t* message)
{
    StartupStatus status;
    status.available = false;
    status.error = message;
    return status;
}
}

StartupRegistration::StartupRegistration(std::wstring testKey) : key_(RunKey)
{
    if (testKey.empty())
    {
        wchar_t buffer[512]{};
        const DWORD length = GetEnvironmentVariableW(L"WINHALO_STARTUP_TEST_KEY", buffer, 512);
        if (length && length < 512) testKey.assign(buffer, length);
        else if (length) throw std::invalid_argument("Startup test key is too long");
    }
    if (!testKey.empty())
    {
        if (testKey.find(TestPrefix) != 0 || testKey.size() <= wcslen(TestPrefix) || testKey.size() > 500)
            throw std::invalid_argument("Startup test registry path must stay under Software\\WinHalo\\Tests");
        key_ = std::move(testKey);
    }
}

std::wstring StartupRegistration::CommandForPath(const std::wstring& path)
{
    if (path.empty() || path.find_first_of(L"\"\r\n") != std::wstring::npos || path.find(L'\0') != std::wstring::npos)
        throw std::invalid_argument("Invalid startup executable path");
    auto command = L"\"" + path + L"\"";
    if (command.size() > 260) throw std::invalid_argument("Startup command exceeds Windows Run key limit");
    return command;
}

std::wstring StartupRegistration::ExecutableCommand()
{
    std::vector<wchar_t> buffer(512);
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!length) throw std::runtime_error("Cannot read executable path");
        if (length < buffer.size()) return CommandForPath(std::wstring(buffer.data(), length));
        if (buffer.size() >= 32768) throw std::runtime_error("Executable path is too long");
        buffer.resize(buffer.size() * 2);
    }
}

StartupStatus StartupRegistration::Read() const noexcept
{
    try
    {
        DWORD bytes = 0;
        LSTATUS result = RegGetValueW(HKEY_CURRENT_USER, key_.c_str(), L"WinHalo", RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
        if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) return {};
        if (result != ERROR_SUCCESS || bytes > 65536 || bytes < sizeof(wchar_t))
            return Failure(L"无法读取开机启动设置");
        std::vector<wchar_t> command(bytes / sizeof(wchar_t) + 1, 0);
        result = RegGetValueW(HKEY_CURRENT_USER, key_.c_str(), L"WinHalo", RRF_RT_REG_SZ, nullptr, command.data(), &bytes);
        if (result != ERROR_SUCCESS) return Failure(L"无法读取开机启动设置");
        StartupStatus status;
        status.enabled = std::wstring(command.data()) == ExecutableCommand();
        if (!status.enabled) status.error = L"启动路径已变化，开启开关可更新";
        return status;
    }
    catch (...) { return Failure(L"程序路径过长或开机启动设置不可用"); }
}

StartupStatus StartupRegistration::SetEnabled(bool enabled) const noexcept
{
    try
    {
        RegistryKey key;
        LSTATUS result;
        if (enabled)
        {
            const auto command = ExecutableCommand();
            result = RegCreateKeyExW(HKEY_CURRENT_USER, key_.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                nullptr, &key.value, nullptr);
            if (result == ERROR_SUCCESS)
                result = RegSetValueExW(key.value, L"WinHalo", 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        }
        else
        {
            result = RegOpenKeyExW(HKEY_CURRENT_USER, key_.c_str(), 0, KEY_SET_VALUE, &key.value);
            if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) return {};
            if (result == ERROR_SUCCESS) result = RegDeleteValueW(key.value, L"WinHalo");
            if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
        }
        if (result != ERROR_SUCCESS) return Failure(L"开机启动设置未能更新，请检查权限");
        return Read();
    }
    catch (...) { return Failure(L"开机启动设置未能更新，程序路径可能过长"); }
}
}
