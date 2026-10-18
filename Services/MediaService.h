#pragma once
#include "MediaTypes.h"
#include <functional>
#include <memory>
#include <thread>

namespace winisland
{
class Logger;
struct MediaServiceState;

class MediaService
{
public:
    explicit MediaService(Logger& logger);
    ~MediaService();
    MediaService(const MediaService&) = delete;
    MediaService& operator=(const MediaService&) = delete;

    // 生命周期由 UI 线程控制；sink 在后台线程调用。
    void Start(std::function<void(MediaSnapshot)> sink);
    void Stop() noexcept;
    // 指定界面看到的播放器身份，排队期间切换会话时拒绝控制新播放器。
    void Command(MediaCommand command, std::wstring expectedSource = {}, std::uint64_t expectedSession = 0);
    void NextSession(std::uint64_t expectedSession);
    void OpenApp(std::uint64_t expectedSession);

private:
    Logger& logger_;
    std::shared_ptr<MediaServiceState> state_;
    std::jthread worker_;
};
}
