#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace winisland
{
enum class MediaCommand { TogglePlayPause, Previous, Next };

struct MediaAppInfo
{
    std::wstring source, name, executable;
    bool shellRegistered = false;
    std::vector<unsigned char> iconBgra;
    unsigned int iconWidth = 0, iconHeight = 0;
};

struct MediaSessionChoice
{
    std::uint64_t id = 0;
    std::wstring source, name;
    bool playing = false;
};

struct MediaSnapshot
{
    bool available = false;
    bool playing = false;
    bool canToggle = false;
    bool canPrevious = false;
    bool canNext = false;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring source;
    std::wstring error;
    std::wstring appError;
    std::uint64_t appRequest = 0;
    std::uint64_t sessionId = 0;
    std::vector<MediaSessionChoice> sessions;
    std::shared_ptr<const MediaAppInfo> app;
    double positionSeconds = 0;
    double durationSeconds = 0;
    double playbackRate = 1;
    std::chrono::steady_clock::time_point capturedAt{};
    // WIC 解码为预乘 BGRA，可直接创建 Direct2D 位图。
    std::vector<unsigned char> artworkBgra;
    unsigned int artworkWidth = 0;
    unsigned int artworkHeight = 0;
    std::uint64_t artworkRevision = 0;
};
}
