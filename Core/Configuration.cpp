#include "Configuration.h"
#include "../Utils/DataDirectory.h"
#include <fstream>
#include <iterator>
#include <algorithm>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

namespace winisland
{
using namespace winrt::Windows::Data::Json;

Configuration ConfigurationStore::Load() noexcept
{
    try
    {
        const auto path = DataDirectory() / L"settings.json";
        if (!std::filesystem::exists(path)) return {};
        if (std::filesystem::file_size(path) > 65536) throw std::runtime_error("Settings file exceeds 64 KB");
        std::ifstream file(path, std::ios::binary);
        if (!file) throw std::runtime_error("Cannot open settings file");
        const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const auto json = JsonObject::Parse(winrt::to_hstring(bytes));
        const double version = json.GetNamedNumber(L"version", 1);
        if (version != 1 && version != 2) throw std::runtime_error("Unsupported settings version");
        Configuration config;
        config.layout.width = static_cast<float>(json.GetNamedNumber(L"width", config.layout.width));
        config.layout.height = static_cast<float>(json.GetNamedNumber(L"height", config.layout.height));
        config.layout.radius = static_cast<float>(json.GetNamedNumber(L"radius", config.layout.radius));
        config.layout.topGap = static_cast<float>(json.GetNamedNumber(L"topGap", config.layout.topGap));
        config.layout.expandedWidth = static_cast<float>(json.GetNamedNumber(L"expandedWidth", config.layout.expandedWidth));
        config.layout.expandedHeight = static_cast<float>(json.GetNamedNumber(L"expandedHeight", config.layout.expandedHeight));
        config.spring.stiffness = json.GetNamedNumber(L"stiffness", config.spring.stiffness);
        config.spring.damping = json.GetNamedNumber(L"damping", config.spring.damping);
        config.spring.mass = json.GetNamedNumber(L"mass", config.spring.mass);
        config.spring.speed = json.GetNamedNumber(L"speed", config.spring.speed);
        config.reducedMotion = json.GetNamedBoolean(L"reducedMotion", false);
        config.autoHideFullscreen = json.GetNamedBoolean(L"autoHideFullscreen", true);
        config.nativeGlass = json.GetNamedBoolean(L"nativeGlass", false);
        if (version == 1)
        {
            if (config.layout.width == 120) config.layout.width = 180;
            if (config.layout.height == 36) config.layout.height = 44;
            if (config.layout.radius == 18) config.layout.radius = 22;
            if (config.layout.expandedWidth == 380) config.layout.expandedWidth = 420;
            if (config.layout.expandedHeight == 240) config.layout.expandedHeight = 340;
        }
        config.layout.height = std::max(44.0f, config.layout.height);
        config.layout.radius = std::max(22.0f, config.layout.radius);
        config.layout.expandedHeight = std::max(340.0f, config.layout.expandedHeight);
        IslandView validateLayout(config.layout);
        Spring validateSpring;
        validateSpring.Configure(config.spring);
        logger_.Write("Settings loaded");
        return config;
    }
    catch (const std::exception& error) { logger_.Write(error.what()); }
    catch (...) { logger_.Write("Settings are invalid; using defaults"); }
    return {};
}

bool ConfigurationStore::Save(const Configuration& config) noexcept
{
    try
    {
        JsonObject json;
        const auto number = [&](const wchar_t* key, double value) { json.Insert(key, JsonValue::CreateNumberValue(value)); };
        number(L"version", 2);
        number(L"width", config.layout.width);
        number(L"height", config.layout.height);
        number(L"radius", config.layout.radius);
        number(L"topGap", config.layout.topGap);
        number(L"expandedWidth", config.layout.expandedWidth);
        number(L"expandedHeight", config.layout.expandedHeight);
        number(L"stiffness", config.spring.stiffness);
        number(L"damping", config.spring.damping);
        number(L"mass", config.spring.mass);
        number(L"speed", config.spring.speed);
        json.Insert(L"reducedMotion", JsonValue::CreateBooleanValue(config.reducedMotion));
        json.Insert(L"autoHideFullscreen", JsonValue::CreateBooleanValue(config.autoHideFullscreen));
        json.Insert(L"nativeGlass", JsonValue::CreateBooleanValue(config.nativeGlass));
        const auto folder = DataDirectory();
        std::filesystem::create_directories(folder);
        const auto temporary = folder / L"settings.json.tmp";
        const auto destination = folder / L"settings.json";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file.exceptions(std::ios::failbit | std::ios::badbit);
            file << winrt::to_string(json.Stringify()) << '\n';
            file.close();
        }
        CheckWin32(MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
            "Save settings");
        logger_.Write("Settings saved");
        return true;
    }
    catch (const std::exception& error) { logger_.Write(error.what()); }
    catch (...) { logger_.Write("Settings could not be saved"); }
    return false;
}
}
