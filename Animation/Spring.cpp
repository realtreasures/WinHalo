#include "Spring.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace winisland
{
Spring::Spring(double initial) { Reset(initial); }

void Spring::Configure(SpringParameters parameters)
{
    if (!std::isfinite(parameters.stiffness) || !std::isfinite(parameters.damping) ||
        !std::isfinite(parameters.mass) || !std::isfinite(parameters.speed) ||
        parameters.stiffness < 10 || parameters.stiffness > 2000 ||
        parameters.damping < 1 || parameters.damping > 200 ||
        parameters.mass < 0.1 || parameters.mass > 10 || parameters.speed < 0.2 || parameters.speed > 3)
        throw std::invalid_argument("Spring parameters are out of range");
    parameters_ = parameters;
}

void Spring::SetTarget(double target)
{
    if (!std::isfinite(target)) throw std::invalid_argument("Spring target must be finite");
    target_ = target; // 保留位置和速度，使连续重定向保持连贯。
}

void Spring::Reset(double value) noexcept
{
    value_ = target_ = value;
    velocity_ = 0;
}

bool Spring::Active() const noexcept
{
    return std::abs(value_ - target_) > 0.001 || std::abs(velocity_) > 0.01;
}

bool Spring::Step(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0) throw std::invalid_argument("Invalid animation delta");
    if (!Active()) { Reset(target_); return false; }
    const double dt = std::min(seconds, 0.05) * parameters_.speed;
    if (dt == 0) return false;
    // 精确求解阻尼弹簧，覆盖欠阻尼、临界阻尼、过阻尼，无欧拉积分的帧率漂移。
    const double omega2 = parameters_.stiffness / parameters_.mass;
    const double decay = parameters_.damping / (2 * parameters_.mass);
    const double discriminant = decay * decay - omega2;
    const double offset = value_ - target_;
    if (std::abs(discriminant) < 1e-7 * omega2)
    {
        const double factor = velocity_ + decay * offset;
        const double attenuation = std::exp(-decay * dt);
        value_ = target_ + (offset + factor * dt) * attenuation;
        velocity_ = (velocity_ - decay * factor * dt) * attenuation;
    }
    else if (discriminant < 0)
    {
        const double frequency = std::sqrt(-discriminant);
        const double sine = std::sin(frequency * dt);
        const double cosine = std::cos(frequency * dt);
        const double attenuation = std::exp(-decay * dt);
        value_ = target_ + attenuation * (offset * cosine + (velocity_ + decay * offset) / frequency * sine);
        velocity_ = attenuation * (velocity_ * cosine - (decay * velocity_ + omega2 * offset) / frequency * sine);
    }
    else
    {
        const double root = std::sqrt(discriminant);
        const double r1 = -omega2 / (decay + root);
        const double r2 = -decay - root;
        const double a = (velocity_ - r2 * offset) / (r1 - r2);
        const double b = offset - a;
        const double first = a * std::exp(r1 * dt);
        const double second = b * std::exp(r2 * dt);
        value_ = target_ + first + second;
        velocity_ = r1 * first + r2 * second;
    }
    if (!Active()) Reset(target_);
    return true;
}
}
