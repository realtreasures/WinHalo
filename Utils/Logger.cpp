#include "Logger.h"
#include "DataDirectory.h"
#include "../Core/Platform.h"
#include <filesystem>
#include <iomanip>
#include <shlobj.h>
#include <string>

namespace winisland
{
Logger::Logger()
{
    try
        {
            const auto folder = DataDirectory();
            std::filesystem::create_directories(folder);
            const auto path = folder / L"WinHalo.log";
            std::error_code error;
            const auto bytes = std::filesystem::file_size(path, error);
            file_.open(path, !error && bytes > 1024 * 1024 ? std::ios::trunc : std::ios::app);
        }
    catch (...) { OutputDebugStringW(L"WinHalo: log file unavailable\n"); }
}

void Logger::Write(std::string_view message) noexcept
{
    try
    {
        const std::lock_guard lock(mutex_);
        const std::string line = std::string(message) + '\n';
        OutputDebugStringA(line.c_str());
        if (file_)
        {
            SYSTEMTIME time{};
            GetLocalTime(&time);
            file_ << std::setfill('0') << time.wYear << '-' << std::setw(2) << time.wMonth
                << '-' << std::setw(2) << time.wDay << ' ' << std::setw(2) << time.wHour
                << ':' << std::setw(2) << time.wMinute << ':' << std::setw(2) << time.wSecond
                << " " << message << '\n';
            file_.flush();
        }
    }
    catch (...) {} // 日志故障不能使窗口过程抛出异常。
}
}
