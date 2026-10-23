# QQ / 微信消息卡片

WinHalo 使用 Windows `UserNotificationListener` 接收进入通知中心的新消息，只展示 QQ / 微信来源。读取范围以 Windows 公开通知为准，通知没有正文时显示“收到一条新消息”；不读取聊天数据库、不获取聊天历史。Windows 授予的是通知访问权限，WinHalo 在应用内筛选聊天来源。隐私开关隐藏正文时，卡片只显示来源和新消息提示。

聊天卡片是胶囊下方的独立窗口，不替换音乐播放器；深色圆角卡片显示应用图标、发送者和最多两行预览。默认五秒后消失，鼠标悬停暂停倒计时；关闭按钮仅关闭 WinHalo 卡片，保留系统通知。点击卡片打开对应应用，首版不保证跳到具体会话。被展示的应用可能因用户主动点击而取得焦点；卡片本身出现、悬停或关闭不抢键盘焦点。

连续通知去重并进入有上限的内存队列，同一通知更新时刷新内容。开始监听时建立基线，不补弹通知中心旧消息。全屏、锁屏或手动隐藏期间暂停显示并丢弃过期消息。消息标题、正文、通知标识和图标只在内存中处理，不写入日志或配置；退出后不保留消息队列。

## 启用与授权

1. 注册下文身份包，关闭并重新启动其固定目录中的 `WinHalo.exe`。
2. 右击横线 / 胶囊打开设置，在“聊天消息”中开启功能，点击申请通知访问。功能默认关闭。安装脚本不替用户申请通知权限，也不启用开机启动。
3. 在 Windows 系统对话框中允许 WinHalo 访问通知，启用 QQ / 微信来源。可设置是否展示正文及卡片停留时间。
4. 保证 QQ / 微信的消息会进入 Windows 通知中心，再接收一条新消息验证。

拒绝权限后不会反复弹出申请；需要在 Windows 设置的通知访问页面手动允许。撤销后应用停止读取、清空待展示消息，并在设置中显示权限状态。重新授权或重启后只处理之后的新消息。“测试弹窗”使用虚构消息，能够在没有包身份或读取权限时预览样式；此预览不证明真实聊天消息已经接入。

## 身份包与基础免安装版

基础胶囊、音乐、设置仍可直接运行 EXE。系统通知监听还需要匹配的包身份、能力声明及用户授权。当前方案采用 Windows 支持的 external-location / sparse identity package：程序仍在用户选择的目录运行，额外注册一个小型 MSIX 身份包。最低身份平台为 Windows 10 build 19041，当前分发为 x64；Windows 10 仍需真实设备验收。

身份常量必须同时出现在 `WinHalo.manifest` 和 `packaging/notifications/AppxManifest.xml`：

| 字段 | 值 |
| --- | --- |
| Package Name | `WinHalo.NotificationIdentity` |
| Publisher / 签名证书 Subject | `CN=WinHalo Development` |
| Application Id | `WinHalo` |
| 能力 | `userNotificationListener`、`runFullTrust`、`unvirtualizedResources` |

包应用声明为 `win32App` / `mediumIL`，不设置 `EntryPoint`；`AppListEntry="default"` 提供可识别的应用入口和图标。使用自有正式证书前，证书 Subject 必须与以上两个清单同时匹配；更换发布者会改变包身份，不能只替换签名文件。

## 生成可分发测试包

在 Windows PowerShell 5.1 或更新版本执行。需先构建包含身份清单的新版本 EXE，安装 Windows SDK；默认工具路径为 `D:\Windows Kits\10\bin\10.0.26100.0\x64`，可通过 `-SdkBinDirectory` 修改。不需要第三方打包工具。

```powershell
.\packaging\notifications\Build-NotificationPackage.ps1 `
  -ExePath .\artifacts\chat-notifications\Release\WinHalo.exe `
  -DevelopmentCertificate
```

脚本默认输出到 `artifacts/chat-package` 的独立 GUID 目录，不覆盖或修改原 EXE，不更改源码，不导入证书、不注册包、不读取真实消息。它检查 EXE 嵌入身份，生成真实 PNG 图标，复制并签名暂存 EXE；先用完整布局执行 MakeAppx 正常校验，再对只含清单和图标的轻量身份包按微软 external-location 指南使用 `/nv` 打包，最后签名 MSIX。最终 `/nv` 允许 EXE 位于包外，不能将其描述成“完整包无需任何语义检查”。

开发模式使用 .NET `CertificateRequest` 在进程内生成 90 天有效的自签代码签名证书。PFX 和 DPAPI 加密密码只在输出目录 `.private/<GUID>` 下保存，目录 ACL 限当前用户和 SYSTEM；没有把证书加入任何 Windows 证书存储。私钥不进入分发包。开发签名不等同于受信任的正式发布签名，90 天过期后应生成新测试包。

也可以使用已有 PFX，密码通过 `SecureString` 提供，不写入脚本：

```powershell
$signingPassword = Read-Host 'PFX password' -AsSecureString
.\packaging\notifications\Build-NotificationPackage.ps1 `
  -ExePath .\artifacts\chat-notifications\Release\WinHalo.exe `
  -CertificatePath D:\Signing\WinHalo.pfx -CertificatePassword $signingPassword
```

ZIP 严格只含 `WinHalo.exe`、`WinHalo.Notifications.msix`、公钥 `WinHalo.Signing.cer`、安装 / 卸载脚本和中文说明。没有 PFX、调试符号、源码、个人配置或消息。具体输出 ZIP 路径和公钥 SHA-1 指纹会打印在构建结果中。

## 用户安装与卸载

将 ZIP 完整解压到固定目录，退出旧 WinHalo。程序保留在该目录，轻量身份包由 Windows 安装到其应用卷（本机为 `D:\WindowsApps`）；二者不是同一个位置。

2026-10-23 本机安装发现：把测试公钥加入 `CurrentUser\TrustedPeople` 已成功，但随后 `Add-AppxPackage` 仍以 `0x800B0109` 失败，身份包未注册成功。原先“只加入当前用户证书即可安装”的说明不适用于此次环境。微软 MSIX 排障文档要求把自签包证书加入 **LocalMachine\TrustedPeople**；这是整台电脑的信任变更，需要管理员权限及单独明确同意，不等于通知读取授权。[微软证书信任排障](https://learn.microsoft.com/en-us/windows/msix/msix-troubleshooting-guide#certificate-not-trusted-0x800b0109)

推荐分两步操作：先核对本包公钥指纹，由管理员在已提升的 PowerShell 中导入这张公钥，再由实际使用 WinHalo 的账户在普通 PowerShell 中注册包。若管理员是另一账户，这样可以避免把应用登记到错误账户。

```powershell
# 第一步：明确同意本机信任后，由管理员在本包目录的已提升终端执行。
Import-Certificate -FilePath .\WinHalo.Signing.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople
# 第二步：回到实际使用 WinHalo 的账户，在本包目录的普通终端执行。
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install-Notifications.ps1
```

同一账户通过 UAC 提升时，也可显式使用 `-TrustMachineDevelopmentCertificate` 一次完成本包机器证书信任和当前账户包注册。该开关要求调用终端已经提升，不会自动发起 UAC，不写入 Root、不导入私钥。若提升时改用另一个管理员账户，则应使用上面的两步流程。

旧开关 `-TrustDevelopmentCertificate` 仍只表示导入 `CurrentUser\TrustedPeople`，不会悄悄升级为机器范围。自签测试包缺少匹配的机器信任时，安装器会在任何证书导入前明确停止并解释原因；两个信任开关不能同时使用。机器公钥已受信任时，不带开关即可由普通用户注册。使用系统已信任的正式签名证书也无需信任开关。

安装校验绑定的 EXE / MSIX 哈希及公钥指纹，只从包中提取三个已知 PNG 到 `Assets`（系统从 external location 读取图标），然后使用 `Add-AppxPackage -ExternalLocation` 为当前用户注册包。失败时保留可读错误，不会自动取得通知权限或启动 QQ / 微信。安装脚本不修改系统执行策略；示例的 `Bypass` 只作用于这一 PowerShell 进程。

同一版本或其他目录已注册时安装脚本停止，避免悄悄替换用户正在使用的程序位置。更新时先退出 WinHalo、卸载旧身份，再运行新目录安装脚本；当前测试包不提供自动升级。注册后请勿直接移动目录。

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Uninstall-Notifications.ps1
# 显式移除当前账户的这张测试证书信任：
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Uninstall-Notifications.ps1 -RemoveDevelopmentCertificate
# 显式移除本机范围的这张测试证书信任：只在同账户已提升终端中执行。
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Uninstall-Notifications.ps1 -RemoveMachineDevelopmentCertificate
```

卸载按完整名称和发布者精确匹配当前用户的身份包，不操作其他包。默认不移除证书；两个证书移除开关会核对公钥文件、预期指纹和自签发布者，分别只操作 `CurrentUser\TrustedPeople` 或 `LocalMachine\TrustedPeople` 中这张开发证书。机器移除开关必须在已经提升的终端运行，脚本不自动提权；预检失败时不会先卸载包。两种移除可明确同时选择。删除机器信任会影响同机其他账户使用这张测试证书签名的包，普通包卸载不会顺带执行此操作。如果管理员是另一账户，应由实际使用者先卸载身份包，再由管理员只删除已核对指纹的机器证书。卸载不会删除任何 EXE、个人配置或原始聊天通知；应用文件由用户另行移除。

## 验收范围

原生队列 / 来源筛选 / 权限状态模拟以及自有窗口的绘制、关闭、悬停和无焦点回归，验证的是应用逻辑与虚构消息，不等同于真实 QQ / 微信接入。对权限拒绝、访问撤销、软件退出、连续消息、高 DPI 和启动基线应分别检查。

当前已完成 x64 Debug / Release 构建，`chat_smoke.py` 在两个配置下通过。通知策略测试及禁用服务的八次生命周期测试通过；卡片窗口原生测试完成四种 DPI 的绘制与交互模拟。设置窗口测试完成四种 DPI、55 个控件、两个窗口生命周期、200 次上下滚动往返及 16 次设置回调。这些结果覆盖自有窗口和示例数据，尚未证明真实 QQ / 微信通知能够端到端接入。

本机已在用户明确同意后完成安装：开发证书加入 `CurrentUser\TrustedPeople` 与 `LocalMachine\TrustedPeople`，EXE / MSIX 签名验证为 `Valid`，身份包注册状态为 `Ok`。通过 Windows 包管理接口移到 D 盘后，组件实际位于 `D:\WindowsApps\WinHalo.NotificationIdentity_0.1.0.0_x64__hs1g2w89pn138`；Windows 返回的 `C:\Program Files\WindowsApps` 同名路径为指向该 D 盘目录的 Junction。程序目录为 `D:\nouseful\WinHalo\dist\WinHalo-Chat`。

安装后使用隔离配置和禁用真实服务的实例验证，进程已取得 `WinHalo.NotificationIdentity_0.1.0.0_x64__hs1g2w89pn138` 身份，测试实例正常退出。此次安装没有代用户申请通知访问或读取真实消息。接下来由用户启动固定目录的程序，在设置中开启聊天消息并主动授权；QQ / 微信真实消息端到端验收仍待完成，不能把安装成功或示例卡片显示当作接入成功的证据。

微软文档：[通知监听与授权](https://learn.microsoft.com/en-us/windows/apps/develop/notifications/app-notifications/notification-listener)、[external-location 身份包与安装](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/grant-identity-to-nonpackaged-apps)。
