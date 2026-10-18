#pragma once
#include <string>

namespace winisland
{
struct VolumeSnapshot
{
    bool available = false;
    bool muted = false;
    // 首次读取只更新界面；后续真实设备/音量变化才触发临时提示。
    bool notify = false;
    float level = 0;
    std::wstring deviceName;
    std::wstring error;
};
}
