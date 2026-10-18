#pragma once
namespace winisland
{
// 默认材质不依赖系统模糊；数值集中在此，使用预乘 alpha 合成。
struct MaterialTheme
{
    static constexpr float red = 0.035f, green = 0.045f, blue = 0.058f;
    static constexpr float opacity = 0.90f;
    static constexpr float glassOpacity = 0.82f;
    static constexpr float outlineOpacity = 0.14f;
    static constexpr float highlightOpacity = 0.10f;
    static constexpr float shadowOpacity = 0.30f;
    static constexpr float shadowSpread = 12.0f;
};
}
