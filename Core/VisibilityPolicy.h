#pragma once
namespace winisland
{
class VisibilityPolicy
{
public:
    bool ShouldShow() const noexcept { return !manualHidden_ && (!fullscreenHidden_ || fullscreenOverride_); }
    bool ManuallyHidden() const noexcept { return manualHidden_; }
    void SetFullscreen(bool hidden) noexcept
    {
        if (!hidden || !fullscreenHidden_) fullscreenOverride_ = false;
        fullscreenHidden_ = hidden;
    }
    // 显式显示在当前这次全屏期间有效，不被菜单恢复前台产生的重复事件撤销。
    void UserShow() noexcept { manualHidden_ = false; fullscreenOverride_ = fullscreenHidden_; }
    void UserToggle() noexcept
    {
        if (ShouldShow()) { manualHidden_ = true; fullscreenOverride_ = false; }
        else UserShow();
    }
private:
    bool manualHidden_ = false;
    bool fullscreenHidden_ = false;
    bool fullscreenOverride_ = false;
};
}
