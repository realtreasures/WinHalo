#pragma once

namespace winisland
{
// 逻辑状态独立于后续的动画过渡状态。
enum class IslandState { Idle, Hover, Expanded, Settings, Line };
enum class WindowEvent { PointerEnter, PointerLeave, Click, TogglePanel, Settings, StressTest, DebugSize, RecreateDevice,
    PowerChanged, DesktopChanged, OutsideClick, MenuOpened, MenuClosed, WindowShown, WindowHidden };
enum class ViewAction { Toggle, Collapse, Settings, Back, Speed, ReducedMotion,
    MediaToggle, MediaPrevious, MediaNext, VolumeDown, VolumeUp, VolumeMute, None,
    SystemSettings, AnimationSettings, Startup, AutoHideFullscreen, OpenMedia, OpenVolume, OpenBattery, Home, NativeGlass,
    OpenMediaApp, NextMediaSession, RevealCapsule };
enum class IslandPage { Home, Media, Volume, Battery, Settings };
}
