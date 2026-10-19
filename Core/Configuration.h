#pragma once
#include "../UI/IslandView.h"
#include "../Utils/Logger.h"
#include <filesystem>

namespace winisland
{
struct Configuration
{
    IslandLayout layout;
    SpringParameters spring;
    bool reducedMotion = false;
    bool autoHideFullscreen = true;
    bool nativeGlass = false;
    bool autoHideIdle = true;
    bool rightDrag = true;
    bool showNotifications = true;
    bool systemControls = true;
    bool clipboardLinks = false;
    bool privacyIndicators = false;
    std::wstring monitorDevice;
    std::wstring language = L"system";
};

// Shared numeric limits and migration are applied before live updates and saves.
void NormalizeConfiguration(Configuration& configuration) noexcept;

class ConfigurationStore
{
public:
    explicit ConfigurationStore(Logger& logger) : logger_(logger) {}
    Configuration Load() noexcept;
    bool Save(const Configuration& configuration) noexcept;
private:
    Logger& logger_;
};
}
