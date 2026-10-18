#pragma once
#include <fstream>
#include <string_view>
#include <mutex>

namespace winisland
{
class Logger
{
public:
    Logger();
    void Write(std::string_view message) noexcept;
private:
    std::ofstream file_;
    std::mutex mutex_;
};
}
