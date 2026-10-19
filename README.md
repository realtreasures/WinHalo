# WinHalo

基于 `init.md` 的 Windows 原生灵动岛。保留原有 Visual Studio 2022 工程，使用 C++20；已实现横线待机、深色半透明时钟 / 音乐胶囊、可中断弹簧动画和系统 Now Playing，并保留音量、电池、托盘、全屏隐藏与开机启动设置。支持的 Windows 11 上可选择开启独立原生桌面模糊。

解决方案和应用现已正式更名为 WinHalo。重命名范围、构建结果、完整文件清单及旧配置 / 自启动项迁移说明见 [重命名记录](docs/RENAME_TO_WINHALO.md)。内部 `winisland` 命名空间保留，Animation、Core、Graphics、Services 等模块结构与算法不变。

## 运行

在 VS2022 打开 `WinHalo.sln`，选择 `Debug | x64` 或 `Release | x64`，将 WinHalo 设为启动项目，按 F5。提示外部修改时重新加载工程。

也可在 Developer PowerShell 中执行：

```powershell
MSBuild.exe .\WinHalo.sln /m /t:Build /p:Configuration=Debug /p:Platform=x64
MSBuild.exe .\WinHalo.sln /m /t:Build /p:Configuration=Release /p:Platform=x64
& .\x64\Release\WinHalo.exe
```

需要 VS2022 的“使用 C++ 的桌面开发”工作负载、MSVC v143、Windows 10 / 11 SDK。完整功能目标为 Windows 10 1809+ / Windows 11；系统媒体 API 从 Windows 10 1809 开始提供，Windows 10 实机兼容性仍待验收。工程选择本机已安装的 SDK，使用 C++20、Unicode、`/utf-8`、`/W4`、`/permissive-`、普通用户权限。Release 使用 `/MT` 静态 MSVC 运行时，Debug 使用 `/MTd`。C++/WinRT JSON 和媒体 API 来自 Windows SDK，无第三方包或模组加载器。参见 [系统媒体 API 要求](https://learn.microsoft.com/en-us/uwp/api/windows.media.control.globalsystemmediatransportcontrolssessionmanager)。

所有配置的程序名均为 `WinHalo.exe`：x64 输出到 `x64\Debug\` / `x64\Release\`，Win32 输出到 `Win32\Debug\` / `Win32\Release\`。构建解决方案时用 `Platform=x86` 选择 Win32，直接构建项目时用 `Platform=Win32`。重新编译前退出正在运行的版本，避免 LNK1168。

## 交互

- 默认在主显示器顶部水平居中，显示 `88 × 8 DIP`、圆角 `4 DIP` 的石墨灰横线，带细高光和柔和阴影。鼠标滑过或停在横线上自动唤出紧凑胶囊；存在媒体会话时，点击胶囊展开播放器。
- 无媒体时只在横线与 `180 × 44 DIP` 时钟胶囊之间切换，点击胶囊无操作。时钟显示时、分，每分钟更新一次，悬停轻微放大并预览月 / 日；四模块首页及其展开入口已取消。
- 鼠标停在紧凑胶囊内时保持打开，移出后完整等待 5 秒收成横线；重新进入取消倒计时，下次移出重新计算。收缩中碰到当前可见区域可平滑展开。完整面板与设置页不使用此计时，菜单打开或窗口隐藏时暂停，返回紧凑胶囊后重新获得 5 秒。
- 存在系统媒体会话时，胶囊变为 `300 × 48 DIP`，显示圆角小封面、截断歌名和均衡器。播放时均衡器跳动，暂停时静止；悬停显示歌手，会话结束恢复时钟。封面缺失或损坏时绘制音符图标。
- 有音乐会话时，点击直接展开为 `420 × 200 DIP` 横向播放器，暂停后仍可打开；无会话时快捷键、托盘和右键菜单也不能打开模块首页。全部会话退出后恢复时钟胶囊，再沿用离开 5 秒收回横线的规则。
- 音乐播放器采用近黑圆角底、底部暖色微光，左上大封面、右侧粗体歌名和灰色歌手，中部细进度与当前 / 总时长，底部大号上一首、播放 / 暂停、下一首图标。控制按播放器能力启用；进度暂不支持拖动定位。播放器不显示首页或设置底栏，右击胶囊直接打开独立设置窗口。
- 音乐卡片取消收起箭头；点击封面、文字或空白处打开 / 恢复选中的音乐软件，同时收缩回胶囊；再次点击胶囊可展开同一个播放器。播放按钮和软件切换保持卡片展开。点击卡片外部收缩，同时原点击正常传给下层窗口。鼠标移出不收缩。左下角显示软件图标，多个系统媒体会话时单击循环切换，角标显示会话数，悬停显示软件名和序号；图标缺失时使用音符回退。手动选择或控制后保持该会话，退出时切换至其他可用会话，全部退出后恢复时钟。仅打开但未注册 SMTC 的软件无法列入。
- 真实音量变化在收起且可见时弹出约 2 秒提示，可调整系统默认输出的静音与音量，每次调整 5%；首次读取不弹提示，展开页保持当前页面。
- 使用电池且电量不高于 20% 时，弹出约 5 秒电量提示；同一区间只提示一次，接通电源或恢复至 25% 后重新允许提示。低电量提示优先于音量，不打断展开页面。
- 音量和电池提示结束恢复原来的横线或紧凑胶囊；音乐播放、切歌或会话变化不会自动唤出横线。横线静止时停止媒体动画及持续提交帧，触碰后显示最新媒体数据。
- 右击胶囊或横线直接打开独立设置窗口，托盘也保留入口；提供外观、行为和动画偏好，修改后自动保存。
- 默认材质包含细描边、内高光和柔和阴影。桌面模糊默认关闭；支持的 Windows 11 上可在“设置 → 外观 → 桌面模糊”开启。不支持或创建失败时使用默认材质。
- 全屏自动隐藏默认开启，只检测胶囊所在显示器的前台全屏窗口。退出后恢复，手动隐藏保持；全屏期间主动显示可覆盖本次自动隐藏。
- 开机启动默认关闭，仅用户开启时注册当前用户登录启动项。建议在固定位置运行 Release 后开启；任务管理器中禁用的启动项需在其中恢复。
- `Ctrl+Alt+Space` 展开播放器 / 收起当前面板，无媒体且未展开时无操作；`Ctrl+Alt+I` 显示 / 隐藏，`Ctrl+Alt+Q` 退出。托盘单击显示 / 隐藏，右键提供播放器、设置和退出；无媒体时“展开播放器”及 Debug 展开测试禁用，设置入口仍可用。

展开胶囊、播放控制和切换会话不抢键盘焦点；主动点击音乐卡片打开软件时才切换到该软件。胶囊以外及阴影区域可跨进程点击穿透。播放器需要接入 SMTC，并提供相应能力和元数据；无会话或无音频输出时正常降级。浏览器媒体恢复浏览器窗口，具体标签页定位尚不支持；本轮未添加歌词功能。

新版材质与媒体实现、平台范围和测试说明见 [材质与 Now Playing](docs/MATERIAL_AND_NOW_PLAYING.md)。

## 独立设置窗口

功能、操作和本轮验证记录见 [独立设置说明](docs/SETTINGS.md)。

- 右击横线或胶囊直接打开独立设置窗口；托盘也保留设置入口。重复打开复用已有窗口，关闭设置不会退出 WinHalo。
- 外观支持胶囊 / 播放器缩放、基础宽高、播放器宽高、水平 / 垂直偏移、字体（0 自动）、显示器、横线长度，以及原生模糊、减少动态效果和动画速度。数值支持直接输入和加减，实时预览并自动保存。
- 行为支持开机启动、离开 5 秒收成横线、全屏隐藏、右键长按拖动、状态通知、系统控制、剪贴板链接提示、摄像头 / 麦克风活动提示、中文 / 英文 / 跟随系统。
- 右键短按打开设置；按住至少 400 ms 后拖动修改位置并保存。在胶囊上滚轮调音量，Shift + 滚轮调所选屏幕亮度，每格 5%。亮度需要硬件支持 WMI 或 DDC/CI，设置中显示检测结果。
- 剪贴板链接与设备活动提示默认关闭。链接仅接受完整 HTTP(S) 地址，先询问，只有点击“打开”才启动默认浏览器。
- 通知开关控制 WinHalo 自己的音量、低电量、亮度、链接和设备活动提示，未接入其他应用的 Windows 通知。设备活动只读取系统元数据；驱动或系统未提供报告时显示不可用 / 等待状态，不采集音视频。
- 设置窗口正常获得键盘焦点供输入；日常横线、胶囊及播放按钮继续不抢焦点。恢复默认不修改真实开机启动注册状态。
- 设置滚动采用批量布局与统一重绘，圆角、开关和图标使用抗锯齿绘制；鼠标点击导航不显示黑色焦点框，Tab 导航保留蓝色圆角焦点提示。

## 已完成的阶段

| 阶段 | 目标与实现 |
| --- | --- |
| 一：工程 | 原 `.sln` / `.vcxproj` 上建立模块，配置 C++20、v143、Unicode、静态运行时和 manifest |
| 二：窗口 | 无边框、置顶、不抢焦点、DIP 布局、DPI 和显示器事件；保留原软件后端作为回归测试 |
| 三：GPU | D3D11 BGRA 设备、DXGI composition 交换链、Direct2D context、DirectWrite 文本、DirectComposition visual；缓存绘制资源，处理缓冲区 resize 和设备丢失 |
| 四：弹簧 | 独立解析阻尼弹簧模型，覆盖欠阻尼 / 临界 / 过阻尼；保留速度中途重定向，限制异常帧间隔，收敛后停止刷新 |
| 五：交互 | 展开 / 收起、悬停、箭头和设置按钮、右键菜单、全局快捷键、JSON 设置 |
| 六：基础验证 | DXGI 帧同步、静止 / 隐藏时等待消息、四种 DPI 的几何与 GPU resize 测试、快速反向测试、GDI 数量检查、设备重建检查和 Debug 性能统计 |
| 系统集成扩展 | 系统媒体会话与封面、播放控制、默认输出音量 / 静音和临时提示、系统托盘、后台服务与 UI 消息合并、只读服务生命周期测试 |
| 日常使用扩展 | 电池 / 充电及低电量提示、全屏事件监听、手动隐藏与主动显示策略、系统设置分页、当前用户开机启动开关 |
| 材质与媒体扩展 | 深色材质、柔和阴影、时钟 / 音乐胶囊、四模块导航、圆角封面与专辑、平滑进度和均衡器、可选独立实时模糊 |
| 横线待机扩展 | 88 × 8 DIP 横线、触碰唤醒、紧凑胶囊离开 5 秒收起、重入取消，以及菜单 / 隐藏 / 通知的计时协调 |

阶段六的 120 / 144 Hz 实屏表现、混合 DPI 多屏热插拔和 Windows 10 实机兼容性仍需实机验收。没有虚报最终显示 FPS、长期 CPU / GPU 占用或内存数据。

## 技术说明

GPU 窗口使用 `WS_EX_NOREDIRECTIONBITMAP`，没有与 `WS_EX_LAYERED` 混用。交换链为 BGRA、预乘 alpha、双缓冲、flip sequential，通过 DirectComposition 合成。D3D11 硬件创建失败时尝试 WARP；本机验证使用硬件路径。

DirectComposition 的透明像素不会自动实现跨进程输入穿透。因此同步更新胶囊的原生 `HRGN`，让宿主空白区和圆角外侧从窗口区域中排除。`WM_NCHITTEST` 使用同一份区域的副本，避免区域边缘与命中测试产生矛盾。区域保留约一个物理像素的抗锯齿边缘，所以很窄的外轮廓也可命中。`SetWindowRgn` 成功后将区域所有权交给系统；自持的命中副本按 RAII 生命周期释放。宿主默认 480 × 400 DIP，仅在外观参数或 DPI 变化时调整；动画帧不修改宿主尺寸或位置。参见 [SetWindowRgn](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowrgn)。

帧循环等待 DXGI 的 frame latency 句柄与 Windows 消息。动画仅在帧可提交时推进，`Present(1, 0)` 使用同步呈现；形变、播放中的均衡器和平滑进度按帧绘制，暂停或隐藏时等待消息，时钟每分钟更新。动画中没有 Sleep 驱动的固定刷新计时器。等待信号被消费后将帧许可传给渲染器，避免重复等待造成停滞。参见 [DXGI 等待句柄](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject) 和 [composition 交换链要求](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforcomposition)。

弹簧精确求解 `m*x'' + c*x' + k*(x-target) = 0`，每次推进最多接收 50 ms 的真实时间，避免断点或系统恢复后的跳变。宽度、高度、中心 X、顶部 Y、圆角、内容透明度、图标偏移、文本偏移、内容缩放和悬停反馈分别使用弹簧。正常展开时 X 目标固定于宿主中心，所以始终水平居中。内容使用更高阻尼；透明度和最终布局限制在可显示范围内。

Direct2D 画刷、字体格式、静态文字布局和设备资源缓存复用。设备移除 / reset 或 D2D target 丢失时在下一帧重建，连续失败超过上限则记录错误并退出。Debug 的设备重建测试验证主动释放和重建路径；不等同于真实显卡驱动被重置的实机验收。

Debug 性能菜单显示最近一次采样的提交帧率、UI 帧处理耗时、弹簧更新耗时、绘制及 Present 耗时、设备创建次数和提交帧总数。提交帧率与显示器最终呈现帧率有区别；UI 帧耗时是墙钟时间，可包含 API 等待，不等同于 GPU 执行时长。

媒体服务使用 `GlobalSystemMediaTransportControlsSessionManager` 优先选择系统指定的当前媒体会话；系统未指定当前会话时枚举已有会话，优先正在播放的会话，否则选择一个未关闭的会话。服务监听会话、曲目信息、播放状态和时间轴变化。媒体调用和封面读取 / WIC 解码在独立 MTA 线程执行；异步请求可取消且有超时，封面读取上限为 5 MB，解码尺寸不超过 256 × 256。Direct2D 缓存封面位图和文字布局，设备重建后重新上传。播放且媒体详情可见时按帧同步绘制本地插值进度；播放中的收起胶囊按帧同步绘制均衡器。模块首页、设置、暂停和隐藏时不为媒体持续刷新；减少动态效果时关闭均衡器动画，媒体详情每秒更新进度。进度条暂不支持拖动定位。

媒体控制携带点击时显示的播放器身份。后台执行前核对选中会话的身份；已切换到其他播放器时拒绝旧命令并提示重试，避免控制错对象。

音量服务通过 Core Audio 读取 `eRender / eMultimedia` 默认输出，监听音量、静音和默认设备变化。音量回调仅写入原子状态并唤醒工作线程，设备绑定、查询、控制、撤销注册都在 MTA 线程完成。正常状态等待事件；设备缺失时每两秒重试。首次快照只更新 UI，后续真实变化才请求提示。服务通过独立快照和共享信箱向 UI 投递消息，每种快照仅保留最新状态，退出时先停止服务再释放窗口；日志写入使用互斥保护。

电池通过 `GetSystemPowerStatus` 读取，并注册电量和供电来源变化事件，处理 `WM_POWERBROADCAST`；没有每帧查询。未知电量显示“未知”，不会误报为 0%。参见 [电源状态](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-system_power_status) 和 [电源事件注册](https://learn.microsoft.com/en-us/windows/win32/power/registering-for-power-events)。

全屏监控使用 `SetWinEventHook` 的进程外通知监听前台切换及窗口状态 / 位置变化，事件合并后才检查 DWM 窗口边界。它不注入其他进程，没有持续轮询；带普通标题栏、最小化、被 DWM 隐藏的窗口及桌面 / 任务栏不会判为全屏。边界与当前显示器完整区域匹配的无标题栏窗口按全屏处理。创建时先保持胶囊隐藏，完成首次检测后再显示，避免全屏中启动闪现。参见 [SetWinEventHook](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwineventhook)。

开机启动注册于 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 的 `WinHalo` 值，内容为带引号的当前可执行文件路径；严格检查路径和 Windows Run 命令长度限制。自启动状态来自注册表，不用 JSON 冒充系统状态，启动时只读取，不自动注册。参见 [Run 注册表启动项](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys)。

## 配置和日志

默认存放于 `%LOCALAPPDATA%\WinHalo`：`settings.json`、`WinHalo.log`。日志在启动时超过 1 MB 则截断。无配置文件时使用默认值；修改设置后才保存。JSON 采用临时文件写入后原子替换，损坏、超大或非法参数回退到默认值并记录日志。

可在退出程序后创建 / 修改配置，重新启动生效：

```json
{
  "version": 2,
  "width": 180,
  "height": 44,
  "radius": 22,
  "topGap": 12,
  "expandedWidth": 420,
  "expandedHeight": 340,
  "stiffness": 460,
  "damping": 32,
  "mass": 1,
  "speed": 1,
  "reducedMotion": false,
  "autoHideFullscreen": true,
  "nativeGlass": false
}
```

尺寸单位为 DIP，实际胶囊顶端距离为 `topGap + insetTop + offsetY`，默认 28 DIP。宿主包络随设置计算，动画期间不改变。播放器基础高度至少 200 DIP。配置版本 3 保存新增选项；版本 1 / 2 保留系统偏好，并将旧设置面板高度迁移为播放器默认 200 DIP。测试可用 `WINHALO_DATA_DIR` 隔离配置和日志。

## 主要文件

| 文件 / 模块 | 职责 |
| --- | --- |
| `WinHalo.cpp` | 入口、WinRT apartment 生命周期、最终错误报告 |
| `Core/Application.h/.cpp` | 消息 / 帧循环、后台服务信箱、进度与临时提示期限、事件、动画测试、性能采样 |
| `Core/Configuration.h/.cpp` | Windows 原生 JSON 解析、参数验证、配置持久化 |
| `Core/IslandState.h`、`Core/Platform.h` | 状态 / 事件和平台头文件设置 |
| `Animation/Spring.h/.cpp`、`AnimationManager.h/.cpp` | 解析弹簧、独立属性、速度保留和收敛 |
| `Window/IslandWindow.h/.cpp` | 窗口、命中、DPI、显示器、菜单和快捷键 |
| `Window/TrayIcon.h/.cpp` | 托盘图标注册、移除、Explorer 任务栏重建后的恢复 |
| `Services/MediaService.h/.cpp`、`MediaTypes.h` | 系统媒体会话、播放命令、事件撤销、异步取消、封面解码和媒体快照 |
| `Services/BatteryService.h/.cpp`、`BatteryTypes.h` | 电源通知注册 / 释放、真实状态读取、未知值处理和低电量提示去重 |
| `Services/FullscreenMonitor.h/.cpp`、`Core/VisibilityPolicy.h` | 前台 / 窗口事件、DWM 边界、显示器匹配、自动和手动可见性策略 |
| `Services/StartupRegistration.h/.cpp` | 当前用户启动项读取和开关、路径引用及隔离测试键限制 |
| `tests/SystemBehaviorTests.cpp`、`system_smoke.py` | 电池 / 隐藏策略 / 注册表单元测试，真实全屏窗口和系统设置桌面验证 |
| `Services/VolumeService.h/.cpp`、`VolumeTypes.h` | Core Audio 默认输出、音量 / 静音命令、设备变化、回调及工作线程生命周期 |
| `Graphics/CompositionRenderer.h/.cpp` | D3D11 / DXGI / D2D / DirectWrite / DirectComposition、region、资源恢复 |
| `Graphics/MaterialTheme.h`、`Window/MaterialBackdrop.h/.cpp` | 默认材质参数、穿透阴影、独立原生背景与合成层圆角裁剪 |
| `Graphics/IRenderer.h`、`LayeredRenderer.h/.cpp` | 渲染接口、阶段二软件后端回归测试 |
| `UI/IslandView.h/.cpp` | 内容状态、布局、动画目标、按钮命中 |
| `Utils/Logger.h/.cpp`、`DataDirectory.h`、`Win32Error.h` | 日志、数据目录、API 错误 |
| `WinHalo.vcxproj`、`.filters`、`.manifest` | 模块、链接库、VS 分组、PerMonitorV2 和普通权限 |
| `tests/AnimationTests.cpp`、`GeometryTests.cpp`、`ServiceViewTests.cpp` | 弹簧、动画、软件 alpha / DPI / GDI，以及媒体按钮状态、进度和音量提示 / 设置页面的 UI fixture 测试 |
| `tests/CompositionTests.cpp` | GPU 缓冲区、区域、设备重建、JSON，以及封面 / 长标题 / 音量提示的实际绘制 fixture 测试 |
| `tests/LineStateTests.cpp`、`tests/line_smoke.py` | 横线状态、精确 5 秒截止时间、重入 / 菜单 / 隐藏 / 通知恢复，以及真实鼠标唤出、穿透和静态 CPU 检查 |
| `tests/ServicesTests.cpp`、`WinHalo.ServicesTests.vcxproj` | 真实系统媒体 / 音量服务的只读快照、重启、取消、幂等停止和停止后无回调测试 |
| `tests/VolumeServiceTests.cpp` | 音量服务专用只读生命周期及进程句柄释放测试 |
| `tests/MediaFixtureTests.cpp`、`WinHalo.MediaFixture.vcxproj` | 唯一身份的自有静音 SMTC 会话：真实媒体元数据、封面解码、能力及固定自身会话控制验证 |
| `tests/window_smoke.py` | 实际桌面上的跨进程点击、面板 / 设置、托盘通知与菜单、截图和静止 CPU 检查 |
| `tests/live_services_smoke.py` | 启用真实系统服务的只读桌面集成：三次启动 / 退出、面板、焦点、短时静止 CPU 和托盘通知恢复 |

所有实现均在上述源码中，没有伪代码。

## 自动验证

```powershell
MSBuild.exe .\tests\WinHalo.Tests.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64
& .\x64\Tests\Debug\WinHalo.Tests.exe
MSBuild.exe .\tests\WinHalo.GraphicsTests.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64
& .\x64\GraphicsTests\Debug\WinHalo.GraphicsTests.exe
MSBuild.exe .\tests\WinHalo.ServicesTests.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64
& .\x64\ServicesTests\Debug\WinHalo.ServicesTests.exe
MSBuild.exe .\tests\WinHalo.ServicesTests.vcxproj /m /t:Build /p:Configuration=Release /p:Platform=x64
& .\x64\ServicesTests\Release\WinHalo.ServicesTests.exe
MSBuild.exe .\tests\WinHalo.MediaFixture.vcxproj /m /t:Build /p:Configuration=Debug /p:Platform=x64
& .\x64\MediaFixture\Debug\WinHalo.MediaFixture.exe
MSBuild.exe .\tests\WinHalo.MediaFixture.vcxproj /m /t:Build /p:Configuration=Release /p:Platform=x64
& .\x64\MediaFixture\Release\WinHalo.MediaFixture.exe
python .\tests\window_smoke.py .\x64\Debug\WinHalo.exe
python .\tests\window_smoke.py .\x64\Release\WinHalo.exe
python .\tests\line_smoke.py .\x64\Release\WinHalo.exe
python .\tests\live_services_smoke.py .\x64\Release\WinHalo.exe
python .\tests\system_smoke.py .\x64\Debug\WinHalo.exe
python .\tests\system_smoke.py .\x64\Release\WinHalo.exe
python .\tests\material_smoke.py .\x64\Release\WinHalo.exe
python .\tests\media_ui_smoke.py .\x64\Release\WinHalo.exe .\x64\MediaFixture\Debug\WinHalo.MediaFixture.exe
```

Python 测试只依赖 Python 3.8+ 标准库。先退出已有 WinHalo，保持桌面解锁。脚本会临时创建下方窗口并移动鼠标进行真实点击，结束后关闭窗口并还原鼠标；运行期间请勿使用鼠标。测试配置位于项目内 `artifacts`，面板截图输出为 `artifacts/expanded.png` 和 `artifacts/settings.png`。桌面脚本设置 `WINHALO_DISABLE_SERVICES=1`，隔离用户真实媒体会话和系统音量；正常运行请勿设置此变量。托盘测试模拟托盘通知和菜单选择，不等同于真实点击 Shell 图标或重启 Explorer。

`live_services_smoke.py` 会移除上述禁用变量，启用真实系统媒体 / 音量服务，再执行三次启动和退出、主面板展开 / 收起、焦点检查、每次一秒的静止 CPU 采样及模拟托盘通知恢复。它不发送媒体播放命令，不调整音量 / 静音；适合检查真实无播放器会话、有默认音频输出时的应用集成，截图为 `artifacts/live-services.png`。短时 CPU 采样不能代表长期资源占用。

本机环境：VS2022 Community、MSVC 14.44.35207、Windows SDK 10.0.26100.0，真实桌面缩放 150%。已有基础验证覆盖：

- Debug / Release x64 编译与运行。
- 欠阻尼 / 临界 / 过阻尼收敛，60 / 144 次每秒推进的轨迹一致性，目标切换保留速度，异常帧间隔处理，1000 次反向，减少动态效果。
- 软件后端四种 DPI、48 次尺寸切换、命中边界、越界、GDI 释放。
- GPU 后端四种 DPI、24 次缓冲区尺寸切换、展开 / 收起区域、GDI 数量有界、三次设备重建；JSON 保存 / 读取及损坏回退。
- 实际胶囊展开、箭头收起、宿主尺寸不变、跨进程透明 / 圆角点击、启动 / 点击不抢焦点、100 次快速反向、设置保存、减少动态效果、菜单和正常退出。

四种 DPI 的自动测试是后端缓冲区 / 几何验证；仅 150% 做了实际桌面交互。自动测试的短时静止 CPU 采样不代表长期功耗。真实多屏切换、系统缩放热变化、高刷新率 / VRR、远程桌面、Windows 10 和显卡驱动移除仍需实机检查。

### 前轮验证记录

以下保留系统集成和日常使用扩展阶段的验证数据。新版材质与媒体的数据见 [本次验证](docs/MATERIAL_AND_NOW_PLAYING.md#本次验证结果)。

本轮系统集成的服务测试为只读：`WinHalo.ServicesTests` 的 Debug / Release x64 编译和运行通过，覆盖媒体 / 音量同时启动、首帧快照合法性、6 次重启、停止耗时有界、重复停止、停止后无回调和 16 次媒体立即取消；本机观察到无播放器会话并验证正常降级。独立音量测试覆盖 20 次等待首帧重启和 20 次立即停止，确认首次快照不请求提示、接收回调位于工作线程，测试前后进程句柄均为 224。测试没有发送播放命令，也没有调整系统音量或静音。

`WinHalo.MediaFixture` 的 Debug / Release x64 编译和运行通过，本机没有跳过检查。测试创建唯一 AppUserModelID 的自有静音 `MediaPlayer` / SMTC 会话，真实验证标题 / 艺术家、播放能力和缩略图，并经 `MediaService` 读取元数据、解码 16 × 16 预乘 BGRA 封面。通过生产 `MediaService.Command` 队列和自身唯一身份发送播放 / 暂停、上一首 / 下一首请求，验证播放状态变化和前后切换回调；错误身份被拒绝且播放状态、切换计数不变。这些请求不控制用户的其他会话。此结果不代表第三方播放器均支持相同功能。

前轮单元测试和 GPU fixture 已通过：UI fixture 覆盖不可用 / 暂停状态、按钮能力、音量提示与设置页面保留；GPU fixture 检查预乘透明封面、长标题截断、更换封面、16 次内容更新及设备重建后的重新上传，音量 / 静音提示的像素与命中区域和收起恢复。Debug / Release 桌面回归已验证托盘通知显示恢复、10 次菜单选择以及 100 次快速展开 / 收起，GDI 对象数前后均为 16（首次打开菜单从 12 增至 16，此后没有增长）。

最终 Release 的真实服务 smoke 通过三次启动 / 退出、展开 / 收起和托盘恢复。三个一秒静止 CPU 采样均为 0 ms；这仅表示该短时采样期间无可测的 CPU 时间，不代表长期功耗。测试没有发送播放器命令或修改系统音量。

上述 UI / GPU fixture 中构造的媒体和音量快照只用于界面验证；自有 SMTC fixture 使用真实 Windows 媒体 API；正常启动的数据来自真实系统服务。第三方播放器的播放 / 暂停和前后切歌、实际系统音量和静音控制、设备拔插 / 默认设备更换、真实托盘图标与 Explorer 重启仍需人工验收。界面 fixture 或模拟托盘通知通过，不代表这些真实系统操作已完成验收。

本阶段 Debug / Release x64 编译通过，单元测试和 GPU fixture 通过。验证覆盖电池解码与低电量去重、未知 / 无电池处理、充电取消提示、隐藏期间供电切换后的提示重置、提示优先级、系统设置命中、JSON 新字段与旧配置兼容、真实全屏窗口的位置事件和恢复、全屏中启动保持隐藏、手动隐藏保持及托盘主动显示覆盖。电源通知与全屏事件监听经过 20 次启停及重复停止检查，进程句柄数量保持有界；本机真实电池读取结果为 99%。注册表测试仅在 `HKCU\Software\WinHalo\Tests\` 下的独立键验证开关及带引号的路径，同时确认其他值不被修改；没有启用用户真实开机启动项，也没有重启或注销 Windows。

`system_smoke.py` 临时创建自己的全屏窗口，完成后退出程序、关闭窗口并恢复前台；请在桌面解锁时运行。测试专用 `WINHALO_STARTUP_TEST_KEY` 只接受 `Software\WinHalo\Tests\` 下的键，正常运行勿设置；`WINHALO_DISABLE_SERVICES` 也会隔离电池与全屏监控，保证旧桌面回归确定性。系统页截图为 `artifacts/system-settings.png`。真实低电量 / 拔插电源、登录启动、不同游戏的独占全屏和混合 DPI 多显示器仍需实机验收。

## 常见问题

- 找不到 v143 / SDK：在 VS Installer 补装 C++ 桌面工作负载及 Windows SDK。
- LNK1168：先退出运行中的 WinHalo，再生成。
- 系统临时目录磁盘空间不足：释放空间，或仅为当前构建会话把 TEMP / TMP 指向项目内文件夹：

```powershell
New-Item -ItemType Directory .\artifacts\tmp -Force | Out-Null
$env:TEMP = (Resolve-Path .\artifacts\tmp).Path
$env:TMP = $env:TEMP
```

- 热键冲突：查看日志，改用胶囊或右键菜单。当前不提供热键重映射。
- 无法保存设置：面板会提示，检查数据目录写权限。
- 没有曲目信息：先在支持 Windows 系统媒体控制的播放器中播放。播放器未提供系统会话时无法读取；无标题 / 艺术家 / 封面也会使用占位提示，不代表窗口故障。
- 音量按钮不可用：检查默认音频输出及 Windows 音频服务；设备恢复后服务会重试绑定。按钮调整系统默认输出，不是应用音量混合器中的单个程序。
- 隐藏后找不到胶囊：单击通知区域 WinHalo 图标，或右键图标选择“显示灵动岛”；也可按 `Ctrl+Alt+I`。图标可能在通知区域的折叠菜单内。
- 不希望全屏时隐藏：进入“设置 → 系统”，关闭“全屏自动隐藏”。无标题栏窗口覆盖整屏会被视为全屏。
- 开机启动开启失败：检查程序是否仍在原位置、路径是否过长，以及当前用户注册表写权限。系统设置页会显示失败说明；不需要提升整个应用权限。
- GPU 初始化失败：查看日志 / 错误框；目前会尝试 WARP，仍失败则退出，软件 layered 后端只用于回归测试。

## 后续功能

后续先做真实低电量、电源切换、登录启动、游戏和多屏兼容性验收，再考虑计时器与隐私可控的剪贴板通知。AI 模块保持可选，不影响基础功能。
