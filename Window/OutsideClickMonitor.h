#pragma once
#include "../Core/Platform.h"
#include <thread>
#include <mutex>
#include <condition_variable>

namespace winisland
{
class OutsideClickMonitor
{
public:
    struct Area { float left, top, width, height, radius; };
    ~OutsideClickMonitor() { Stop(); }
    bool Watch(HWND window, UINT message, Area area);
    void Stop() noexcept;
private:
    static LRESULT CALLBACK Hook(int code, WPARAM button, LPARAM data) noexcept;
    inline static thread_local OutsideClickMonitor* current_ = nullptr;
    std::jthread thread_;
    std::mutex mutex_;
    std::condition_variable started_;
    Area area_{};
    HWND window_ = nullptr;
    UINT message_ = 0;
    DWORD threadId_ = 0;
    bool ready_ = false, installed_ = false, posted_ = false;
};
}
