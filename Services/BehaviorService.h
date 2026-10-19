#pragma once
#include <functional>
#include <memory>
#include <string>

namespace winisland
{
class Logger;

struct BehaviorSnapshot
{
    bool brightnessAvailable = false;
    int brightness = 0;
    bool cameraAvailable = false, microphoneAvailable = false;
    bool cameraActive = false, microphoneActive = false;
    std::wstring brightnessStatus, privacyStatus;
};

class BehaviorService
{
public:
    explicit BehaviorService(Logger& logger);
    ~BehaviorService();
    BehaviorService(const BehaviorService&) = delete;
    BehaviorService& operator=(const BehaviorService&) = delete;

    // The sink runs on the worker; marshal its value to the UI thread.
    void Start(std::function<void(BehaviorSnapshot)> sink);
    // monitorDevice is MONITORINFOEX::szDevice (e.g. \\.\DISPLAY1); empty selects primary.
    void Configure(std::wstring monitorDevice, bool brightnessEnabled, bool privacyEnabled);
    void AdjustBrightness(int delta);
    void Stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
