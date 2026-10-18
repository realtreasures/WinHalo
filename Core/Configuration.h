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
};

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
