# WinHalo 重命名记录

## 范围与方案

将解决方案、主 C++ 项目及配套文件、入口和资源文件、四个测试项目统一更名为 WinHalo；更新用户可见名称、运行时应用标识、测试引用及使用文档。保留解决方案 / 项目 GUID、模块目录、资源数字 ID、链接库、C++20 / v143 / SDK 设置和原有业务实现。

主项目在公共属性组显式设置 `TargetName=WinHalo`，适用于 Debug / Release 和 x64 / Win32。解决方案中的 x86 映射保持为项目的 Win32。主项目和测试项目的中间目录均增加 `$(ProjectName)` 子目录，避免与旧构建缓存共用目录（MSB8028）；输出目录不变，`.gitignore` 补充 Win32 输出目录。所有源码、清单、图标、资源及测试相对引用均已检查，无自定义包含 / 链接库目录需要修改，链接依赖保持原样。应用默认配置目录为 `%LOCALAPPDATA%\WinHalo`，日志为 `WinHalo.log`，清单和 EXE 版本信息使用 WinHalo。

`winisland` 是跨模块使用的内部 C++ 命名空间，与产品展示、EXE 名称和文件系统路径无关，因此保留；`IslandView`、`IslandWindow` 等描述灵动岛领域概念的类型也保留。窗口类名、资源符号和测试媒体身份等带产品前缀的标识及所有调用方同步更名，资源数字 ID 不变。未修改动画算法、渲染参数或交互逻辑。

## 修改前状态与备份

实施前已运行 `git status --short` 和 `git status --porcelain=v1 --untracked-files=all`。当前目录及其父目录没有 Git 仓库，命令返回 `fatal: not a git repository`，因此无法判断原有未提交改动；没有初始化仓库或执行任何 Git 清理 / 重置。

修改前对 85 个源码、配置、文档和图标文件保存了逐字节副本及 SHA-256。备份位于 `artifacts/rename-backup-20261018-142902/original/`，`baseline.json` 记录原始校验值，`changes.json` 记录首次实施的文件映射。应用代码仅作名称替换；版本资源及构建公共属性为本次新增内容。

初次重命名保留了 `.vs`、旧构建输出和历史测试产物。随后工作区整体移动到了 `D:\nouseful\WinHalo`，工程继续使用相对路径；移动目录后，中间文件内残留的旧绝对路径需要通过重新生成缓存清除，处理记录见下文。

## 旧版本迁移与 Visual Studio

- 在 VS2022 关闭旧解决方案，再打开 `WinHalo.sln`；若提示文件从外部修改，请重新加载。旧 VS 会话和正在运行的旧版应用没有被强制关闭。
- 如需保留旧设置，退出新旧应用后，将 `%LOCALAPPDATA%\WinIsland\settings.json` 复制到 `%LOCALAPPDATA%\WinHalo\settings.json`。若目标文件已存在，请先自行备份并决定保留哪份设置。JSON 格式和参数不变；程序不自动移动旧目录。
- 自启动仍使用标准 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，应用值名现为 `WinHalo`，内容由当前 EXE 的绝对路径动态生成并加引号。若曾启用旧版自启动，请先在旧版关闭，再在新版启用；也可自行删除 Run 下的旧 `WinIsland` 值。此次重命名未修改用户真实 Run 键。
- 测试 / 自定义数据目录环境变量改为 `WINHALO_DATA_DIR`；服务隔离变量改为 `WINHALO_DISABLE_SERVICES`；隔离注册表变量改为 `WINHALO_STARTUP_TEST_KEY`，只接受 `Software\WinHalo\Tests\` 下的子键。外部脚本若使用旧变量或旧 EXE 路径，需要同步更新。
- 旧二进制、`.vs` 缓存和备份中的旧名称属于历史文件，不会参与新工程编译。无需修改这些二进制缓存。

## 验证结果

使用本机 VS2022 Community 17.14、v143 工具集和 Windows SDK 执行构建。初次重命名后主程序四种配置均构建成功，均为零警告、零错误；此前 MSB8028 中间目录共用警告已通过目录隔离解决。后续整体移动目录时的缓存处理和复验另见下文。

| 解决方案配置 | 项目平台 | 输出文件 | 构建 / 启动 / 退出 |
| --- | --- | --- | --- |
| Debug / x64 | x64 | `x64/Debug/WinHalo.exe` | 通过 |
| Release / x64 | x64 | `x64/Release/WinHalo.exe` | 通过 |
| Debug / x86 | Win32 | `Win32/Debug/WinHalo.exe` | 通过 |
| Release / x86 | Win32 | `Win32/Release/WinHalo.exe` | 通过 |

逐个运行四个最终 EXE，通过进程 ID 定位各自的 `WinHalo.NativeIslandWindow`，确认窗口可见、标题为 `WinHalo`、成功加载隔离配置、写入 `WinHalo.log`，发送关闭消息后均以 0 正常退出。运行检查使用 `WINHALO_DATA_DIR` 将数据放在 `artifacts/rename-validation/runtime-*/`，并设置 `WINHALO_DISABLE_SERVICES=1`；未操作鼠标、未写用户真实配置目录或真实自启动项。此检查验证启动与退出，不代表完整桌面交互 / 第三方播放器验收。

四个 EXE 的 `ProductName`、`InternalName`、`FileDescription` 均为 `WinHalo`，`OriginalFilename` 为 `WinHalo.exe`；新增版本资源使用与原清单一致的 `0.1.0.0`，未更改清单版本。

现有测试项目的 Debug / Release x64 共 8 个配置均编译成功，均为零警告、零错误：

| 测试项目 | Debug | Release |
| --- | --- | --- |
| `WinHalo.Tests` | 通过 | 通过 |
| `WinHalo.ServicesTests` | 通过 | 通过 |
| `WinHalo.GraphicsTests` | 屏幕像素检查失败 | 屏幕像素检查失败 |
| `WinHalo.MediaFixture` | 退出码 0，部分控制检查跳过 | 退出码 0，部分控制检查跳过 |

单元测试覆盖动画阻尼 / 刷新率 / 反向 / 减少动态效果、DPI / 几何 / 命中、模块导航、媒体 / 音量 / 电池状态、配置及隔离自启动注册表；服务测试覆盖真实系统媒体 / 音量服务生命周期，不发送用户播放器或音量命令。

图形测试在读取预乘封面像素时得到白色，命中了另一个 `Chrome_RenderWidgetHostHWND` 窗口，而不是测试窗口。在相同桌面上运行保留的旧 `WinIsland.GraphicsTests.exe`，得到相同错误和外部窗口句柄（旧版 / 新版退出码均为 1）。因此本次无法完成依赖无遮挡桌面的像素检查；未因此修改渲染实现，需在无遮挡桌面重跑图形测试。对照日志为 `artifacts/rename-validation/graphics-original-comparison.log` 和 `graphics-renamed-comparison.log`。

媒体 fixture 验证了断开 / 关闭对象的 HRESULT 处理、自有静音 SMTC 会话注册、能力与元数据；生产服务选中了其他会话，现有测试因此主动跳过后续自有会话控制检查，未向用户播放器发送命令。不能将退出码 0 解释为所有媒体控制检查均已完成。

静态检查确认全部项目 / 筛选文件 XML 可解析、源文件与清单 / 图标等相对引用存在、Python 测试脚本可编译。85 个原始文件的逐一比较确认，所有 C++ 源码与头文件只发生 `WinIsland` / `WINISLAND` 产品名称替换，图标逐字节不变；内部 `winisland` 命名空间、动画和渲染算法保持原样。

完整构建、测试、版本信息和运行检查日志位于 `artifacts/rename-validation/`；最终原始文件变更校验清单位于备份目录的 `final-changes.json`。

## 项目目录移动后的 RC2135 修复

项目移动到 `D:\nouseful\WinHalo` 后，`x64/Debug/obj/WinHalo/WinHalo_manifest.rc` 仍引用 `D:\nouseful\WinIsland\x64\Debug\obj\WinHalo\WinHalo.exe.embed.manifest`，造成 RC2135。该文件由构建工具生成，不是应用源码资源文件；MSBuild 实际计算出的 `ProjectDir`、`SolutionDir`、`OutDir`、`IntDir` 和 `TargetPath` 均已指向新目录，项目配置没有写死旧路径。

确认没有活动的 C++ 编译进程后，已将四个主程序配置和八个测试配置的 12 个 `obj` 生成目录移入 `artifacts/relocation-cache-backup-20261018-153117/`，保留原缓存及本说明的修改前副本；没有修改源码、项目配置、应用清单或用户设置。测试缓存供以后构建时自动重新生成。本次修复前再次检查 Git，当前目录仍无 Git 仓库。

在新工作目录重新构建 Debug / Release × x64 / Win32 四种主程序配置，全部成功，均为零警告、零错误。原报错的 Debug x64 又单独执行了一次普通增量构建，同样通过；新生成的 `WinHalo_manifest.rc` 已引用 `D:\nouseful\WinHalo\x64\Debug\obj\WinHalo\WinHalo.exe.embed.manifest`，对应文件存在。验证记录位于 `artifacts/relocation-validation/`。

后续再次移动工程目录时，应关闭 VS 中的旧解决方案，打开新路径下的 `.sln`，并清理构建输出中的 `obj` 生成缓存后重新生成；不要手动编辑生成的 `*_manifest.rc` 或修改已正确使用相对路径的项目设置。

## 初次重命名完整修改文件清单

共修改 / 重命名 40 个原有文件，新增本说明文件 1 个；备份和构建日志位于忽略的 `artifacts/` 中。

### 文件重命名（13 个）

| 原文件 | 新文件 |
| --- | --- |
| `tests/WinIsland.GraphicsTests.vcxproj` | `tests/WinHalo.GraphicsTests.vcxproj` |
| `tests/WinIsland.MediaFixture.vcxproj` | `tests/WinHalo.MediaFixture.vcxproj` |
| `tests/WinIsland.ServicesTests.vcxproj` | `tests/WinHalo.ServicesTests.vcxproj` |
| `tests/WinIsland.Tests.vcxproj` | `tests/WinHalo.Tests.vcxproj` |
| `WinIsland.cpp` | `WinHalo.cpp` |
| `WinIsland.h` | `WinHalo.h` |
| `WinIsland.ico` | `WinHalo.ico` |
| `WinIsland.manifest` | `WinHalo.manifest` |
| `WinIsland.rc` | `WinHalo.rc` |
| `WinIsland.sln` | `WinHalo.sln` |
| `WinIsland.vcxproj` | `WinHalo.vcxproj` |
| `WinIsland.vcxproj.filters` | `WinHalo.vcxproj.filters` |
| `WinIsland.vcxproj.user` | `WinHalo.vcxproj.user` |

### 原路径内容更新（27 个）

- `.gitignore`
- `Core/Application.cpp`
- `docs/MATERIAL_AND_NOW_PLAYING.md`
- `Graphics/CompositionRenderer.cpp`
- `init.md`
- `README.md`
- `Resource.h`
- `Services/StartupRegistration.cpp`
- `Services/StartupRegistration.h`
- `tests/CompositionTests.cpp`
- `tests/GeometryTests.cpp`
- `tests/line_smoke.py`
- `tests/live_services_smoke.py`
- `tests/material_smoke.py`
- `tests/media_switch_smoke.py`
- `tests/media_ui_smoke.py`
- `tests/MediaFixtureTests.cpp`
- `tests/ServicesTests.cpp`
- `tests/system_smoke.py`
- `tests/SystemBehaviorTests.cpp`
- `tests/VolumeServiceTests.cpp`
- `tests/window_smoke.py`
- `Utils/DataDirectory.h`
- `Utils/Logger.cpp`
- `Window/IslandWindow.cpp`
- `Window/MaterialBackdrop.cpp`
- `Window/TrayIcon.cpp`

### 新增说明

- `docs/RENAME_TO_WINHALO.md`

