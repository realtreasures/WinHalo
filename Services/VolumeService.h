#pragma once
#include "VolumeTypes.h"
#include <functional>
#include <memory>

namespace winisland
{
class Logger;

class VolumeService
{
public:
    explicit VolumeService(Logger& logger);
    ~VolumeService();
    VolumeService(const VolumeService&) = delete;
    VolumeService& operator=(const VolumeService&) = delete;

    // sink 在工作线程调用；接收端应投递消息给 UI，不直接操作窗口。
    void Start(std::function<void(VolumeSnapshot)> sink);
    void Stop() noexcept;
    // 仅投递命令，Core Audio 调用始终在 MTA 工作线程执行。
    void SetLevel(float level);
    void ToggleMute();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
