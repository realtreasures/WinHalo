#include "AnimationManager.h"
#include <algorithm>

namespace winisland
{
void AnimationManager::Reset(const AnimationValues& values) noexcept
{
    for (size_t i = 0; i < springs_.size(); ++i) springs_[i].Reset(values[i]);
}

void AnimationManager::Retarget(const AnimationValues& targets, bool immediate)
{
    for (size_t i = 0; i < springs_.size(); ++i)
        if (immediate) springs_[i].Reset(targets[i]);
        else springs_[i].SetTarget(targets[i]);
}

void AnimationManager::Configure(SpringParameters parameters)
{
    for (size_t i = 0; i < springs_.size(); ++i)
    {
        auto tuned = parameters;
        // 内容使用更高阻尼，避免透明度和文字位置过度振荡。
        if (i >= static_cast<size_t>(Property::Opacity)) tuned.damping = std::max(40.0, tuned.damping);
        springs_[i].Configure(tuned);
    }
}

bool AnimationManager::Step(double seconds)
{
    bool changed = false;
    for (auto& spring : springs_) changed = spring.Step(seconds) || changed;
    return changed;
}

bool AnimationManager::Active() const noexcept
{
    for (const auto& spring : springs_) if (spring.Active()) return true;
    return false;
}

float AnimationManager::Get(Property property) const noexcept
{
    return static_cast<float>(springs_[static_cast<size_t>(property)].Value());
}
}
