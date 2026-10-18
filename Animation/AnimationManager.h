#pragma once
#include "Spring.h"
#include <array>
#include <cstddef>

namespace winisland
{
enum class Property : size_t { Width, Height, CenterX, Top, Radius, Opacity, IconOffset, TextOffset, Scale, Hover, CompactOpacity, Count };
using AnimationValues = std::array<double, static_cast<size_t>(Property::Count)>;

class AnimationManager
{
public:
    void Reset(const AnimationValues& values) noexcept;
    void Retarget(const AnimationValues& targets, bool immediate);
    void Configure(SpringParameters parameters);
    bool Step(double seconds);
    bool Active() const noexcept;
    float Get(Property property) const noexcept;
private:
    std::array<Spring, static_cast<size_t>(Property::Count)> springs_;
};
}
