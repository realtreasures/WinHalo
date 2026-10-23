#include "Configuration.h"
#include "../Utils/DataDirectory.h"
#include <fstream>
#include <iterator>
#include <algorithm>
#include <cmath>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

namespace winisland
{
using namespace winrt::Windows::Data::Json;

void NormalizeConfiguration(Configuration& config) noexcept
{
    const auto limit = [](float value, float low, float high, float fallback) {
        return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
    };
    auto& l = config.layout;
    l.width = limit(l.width, 120, 400, 180);
    l.height = limit(l.height, 32, 88, 44);
    l.compactScale = limit(l.compactScale, .5f, 2, 1);
    l.expandedScale = limit(l.expandedScale, .5f, 2, 1);
    l.expandedWidth = limit(l.expandedWidth, 320, 900, 420);
    l.expandedHeight = limit(l.expandedHeight, 200, 600, 200);
    l.offsetX = limit(l.offsetX, -8192, 8192, 0);
    l.offsetY = limit(l.offsetY, -8192, 8192, 0);
    l.fontSize = !std::isfinite(l.fontSize) || l.fontSize <= 0 ? 0 : std::clamp(l.fontSize, 10.0f, 28.0f);
    l.lineWidth = limit(l.lineWidth, 24, 240, 88);
    l.radius = std::max(22.0f, l.height / 2);
    l.insetTop = 16;
    l.topGap = limit(l.topGap, 0, 200, 12);
    // Resize the transparent host only on a settings change, never during a spring.
    l.hostWidth = std::max({480.0f, l.expandedWidth * l.expandedScale + 60,
        std::max(l.width, 300.0f) * l.compactScale + 80});
    l.hostHeight = std::max({400.0f, l.expandedHeight * l.expandedScale + l.insetTop + 48,
        std::max(l.height, 100.0f) * l.compactScale + l.insetTop + 48});
    if (config.language != L"system" && config.language != L"zh-CN" && config.language != L"en-US")
        config.language = L"system";
    if (config.monitorDevice.size() > 128) config.monitorDevice.clear();
    config.chatDurationSeconds = std::clamp(config.chatDurationSeconds, 2, 20);
    try { Spring spring; spring.Configure(config.spring); }
    catch (...) { config.spring = {}; }
}

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
        if (version != 1 && version != 2 && version != 3 && version != 4) throw std::runtime_error("Unsupported settings version");
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
        config.layout.compactScale = static_cast<float>(json.GetNamedNumber(L"compactScale", 1));
        config.layout.expandedScale = static_cast<float>(json.GetNamedNumber(L"expandedScale", 1));
        config.layout.offsetX = static_cast<float>(json.GetNamedNumber(L"offsetX", 0));
        config.layout.offsetY = static_cast<float>(json.GetNamedNumber(L"offsetY", 0));
        config.layout.fontSize = static_cast<float>(json.GetNamedNumber(L"fontSize", 0));
        config.layout.lineWidth = static_cast<float>(json.GetNamedNumber(L"lineWidth", 88));
        config.autoHideIdle = json.GetNamedBoolean(L"autoHideIdle", true);
        config.rightDrag = json.GetNamedBoolean(L"rightDrag", true);
        config.showNotifications = json.GetNamedBoolean(L"showNotifications", true);
        config.systemControls = json.GetNamedBoolean(L"systemControls", true);
        config.clipboardLinks = json.GetNamedBoolean(L"clipboardLinks", false);
        config.privacyIndicators = json.GetNamedBoolean(L"privacyIndicators", false);
        config.chatNotifications = json.GetNamedBoolean(L"chatNotifications", false);
        config.chatQQ = json.GetNamedBoolean(L"chatQQ", true);
        config.chatWeChat = json.GetNamedBoolean(L"chatWeChat", true);
        config.chatShowBody = json.GetNamedBoolean(L"chatShowBody", true);
        config.chatDurationSeconds = static_cast<int>(std::clamp(json.GetNamedNumber(L"chatDurationSeconds", 5), 2.0, 20.0));
        config.monitorDevice = json.GetNamedString(L"monitorDevice", L"");
        config.language = json.GetNamedString(L"language", L"system");
        if (version == 1)
        {
            if (config.layout.width == 120) config.layout.width = 180;
            if (config.layout.height == 36) config.layout.height = 44;
            if (config.layout.radius == 18) config.layout.radius = 22;
            if (config.layout.expandedWidth == 380) config.layout.expandedWidth = 420;
            if (config.layout.expandedHeight == 240) config.layout.expandedHeight = 340;
        }
        if (version < 3) config.layout.expandedHeight = 200;
        NormalizeConfiguration(config);
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
        number(L"version", 4);
        number(L"width", config.layout.width);
        number(L"height", config.layout.height);
        number(L"radius", config.layout.radius);
        number(L"topGap", config.layout.topGap);
        number(L"expandedWidth", config.layout.expandedWidth);
        number(L"expandedHeight", config.layout.expandedHeight);
        number(L"compactScale", config.layout.compactScale);
        number(L"expandedScale", config.layout.expandedScale);
        number(L"offsetX", config.layout.offsetX);
        number(L"offsetY", config.layout.offsetY);
        number(L"fontSize", config.layout.fontSize);
        number(L"lineWidth", config.layout.lineWidth);
        number(L"stiffness", config.spring.stiffness);
        number(L"damping", config.spring.damping);
        number(L"mass", config.spring.mass);
        number(L"speed", config.spring.speed);
        json.Insert(L"reducedMotion", JsonValue::CreateBooleanValue(config.reducedMotion));
        json.Insert(L"autoHideFullscreen", JsonValue::CreateBooleanValue(config.autoHideFullscreen));
        json.Insert(L"nativeGlass", JsonValue::CreateBooleanValue(config.nativeGlass));
        json.Insert(L"autoHideIdle", JsonValue::CreateBooleanValue(config.autoHideIdle));
        json.Insert(L"rightDrag", JsonValue::CreateBooleanValue(config.rightDrag));
        json.Insert(L"showNotifications", JsonValue::CreateBooleanValue(config.showNotifications));
        json.Insert(L"systemControls", JsonValue::CreateBooleanValue(config.systemControls));
        json.Insert(L"clipboardLinks", JsonValue::CreateBooleanValue(config.clipboardLinks));
        json.Insert(L"privacyIndicators", JsonValue::CreateBooleanValue(config.privacyIndicators));
        json.Insert(L"chatNotifications", JsonValue::CreateBooleanValue(config.chatNotifications));
        json.Insert(L"chatQQ", JsonValue::CreateBooleanValue(config.chatQQ));
        json.Insert(L"chatWeChat", JsonValue::CreateBooleanValue(config.chatWeChat));
        json.Insert(L"chatShowBody", JsonValue::CreateBooleanValue(config.chatShowBody));
        number(L"chatDurationSeconds", std::clamp(config.chatDurationSeconds, 2, 20));
        json.Insert(L"monitorDevice", JsonValue::CreateStringValue(config.monitorDevice));
        json.Insert(L"language", JsonValue::CreateStringValue(config.language));
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
