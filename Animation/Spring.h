#pragma once

namespace winisland
{
struct SpringParameters
{
    double stiffness = 460.0;
    double damping = 32.0;
    double mass = 1.0;
    double speed = 1.0;
};

class Spring
{
public:
    explicit Spring(double initial = 0.0);
    void Configure(SpringParameters parameters);
    void SetTarget(double target);
    void Reset(double value) noexcept;
    bool Step(double seconds);
    bool Active() const noexcept;
    double Value() const noexcept { return value_; }
    double Velocity() const noexcept { return velocity_; }
    double Target() const noexcept { return target_; }
private:
    SpringParameters parameters_;
    double value_ = 0.0;
    double velocity_ = 0.0;
    double target_ = 0.0;
};
}
