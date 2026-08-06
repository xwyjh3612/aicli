# AI CLI Mobile Architecture

## 1. 结论

桌面端采用方案 B：使用 C++/WinUI 3/Windows App SDK 作为原生窗口，并嵌入 Windows Terminal 的 Terminal Control。右侧终端内部使用 PowerShell 作为宿主，Codex、Claude 和项目启动命令都在该终端中运行，由 Terminal Control 负责显示和输入。

这不是给现有 egui 页面加一个控件，而是重做桌面端窗口。Windows Terminal 的终端控件主要是 C++/WinRT 组件，不能直接作为 Rust/egui 的普通控件使用，需要固定 Windows Terminal 源码版本并维护 C++ 构建集成。

方案可行，但实现难度为中高。阶段 0 必须先验证 Terminal Control 能独立编译、连接 ConPTY、接收键盘和显示 Codex TUI，验证通过后再开发完整页面。

不采用浏览器、WebView、Electron，也不通过 `SetParent` 强行嵌入独立的 `wt.exe` 窗口。独立启动 `wt.exe` 虽然输入体验最好，但应用无法可靠获得其 PTY 输入句柄，手机输入、暂停、重启和项目切换都会失去可靠控制。

### 1.1 开发范围冻结

当前只开发和测试电脑端。Android App、公网 Relay、手机扫码绑定和手机输入全部延期，直到电脑端完成真实 CLI、Terminal Control、项目管理和恢复测试后再开始。

电脑端阶段不依赖 Android 才能运行，先使用本机键盘、本机 Agent 和本机 SQLite 完成闭环。

## 2. 产品范围

### 2.1 桌面端

桌面端是主控制端，负责项目创建、分组、Codex/Claude 启动、内嵌终端、设备绑定、网络设置、SQLite 和运行状态。PowerShell 只作为终端内部宿主，界面不单独提示。

### 2.2 Android 端

Android 端只做辅助：切换 Codex/Claude、查看项目和实时终端事件、发送文字和控制键、暂停/继续/取消/重启、扫码绑定以及连接设置。Android 不运行 CLI，不保存模型密钥。

## 3. 固定技术栈

| 层 | 技术 |
|---|---|
| 桌面 UI | C++、WinUI 3、Windows App SDK |
| 终端 | Windows Terminal `TerminalControl`/`TerminalCore` 源码集成 |
| 进程 | Windows ConPTY、原生进程 API |
| Agent | 保留现有 Rust Agent 或迁移为 C++ 服务 |
| Broker | Rust/C++，负责 ConPTY 和进程生命周期 |
| 数据库 | SQLite |
| Android | Java、XML、AndroidX、ViewBinding、OkHttp |
| 身份 | Windows DPAPI/Credential Manager、Android Keystore、TLS |

推荐进程结构：

```text
AI CLI Desktop.exe       C++/WinUI 3 主窗口和 Terminal Control
AI CLI Agent.exe         HTTP/WebSocket、SQLite、设备授权、Relay
AI CLI Broker.exe        ConPTY、进程和输入队列
Android App              Java/XML
```

Rust/egui 不再负责桌面 UI，但现有 Rust Agent、Broker、SQLite 和协议可以逐步保留。

## 4. Terminal Control 结构

```text
WinUI TerminalControl
        |
        +-- TerminalCore：ANSI、光标、滚动、全屏 TUI、键盘
        |
        +-- ConPTY：Windows 伪控制台
                  |
                  +-- powershell.exe
                        |
                        +-- codex / claude / 项目启动命令
```

每个项目维护一个独立的内嵌终端会话。终端启动后进入项目目录，再执行项目配置中的启动命令。Codex、Claude 或自定义命令都是这个终端里的进程，不再额外创建独立的“PowerShell Tab”。

所有输入统一进入项目输入队列：

```text
桌面键盘或 Android 消息
        -> 项目输入队列
        -> Terminal Control / Broker
        -> ConPTY 输入句柄
        -> PowerShell 宿主中的 Codex / Claude / 项目命令
```

必须支持普通文本、Enter、Backspace、Tab、方向键、Home/End、PageUp/PageDown、Escape、Ctrl+C、Ctrl+D、Ctrl+L、复制和粘贴。不能再使用自绘聊天输入框转发命令。

## 5. 桌面整体布局

默认窗口为 1440 x 900，最小 1100 x 680，默认夜间主题。

```text
┌──────────────────────────────────────────────────────────────────────────┐
│ AI CLI | 分组 Tab | + | 设备 | 绑定手机 | 内存 | Agent | Relay | 设置      │
├──────────────────┬───────────────────────────────────────────────────────┤
│ 项目              │ Codex 新项目              ● 运行中                    │
│ [Codex] [Claude]  │ C:\project\demo · Codex CLI                         │
│ 搜索项目          ├───────────────────────────────────────────────────────┤
│                  │ [项目终端]                                             │
│ ● 项目 A          │                                                       │
│   运行中          │              Windows Terminal Control                 │
│                  │          原生 TUI / ANSI / 光标 / 输入                │
│ ○ 项目 B          │                                                       │
│   已暂停          │                                                       │
│                  ├───────────────────────────────────────────────────────┤
│ + 新建项目        │ Terminal ready · 点击终端直接输入                    │
└──────────────────┴───────────────────────────────────────────────────────┘
```

### 5.1 顶部栏

顶部只放全局信息：AI CLI、分组 Tab、添加分组、设备数、绑定手机、内存、Agent、Relay、主题和设置。Codex/Claude 不放在顶部全局栏，而是放在左侧作为项目类型过滤。

### 5.2 左侧项目栏

左侧显示当前分组中的项目，支持 Codex/Claude 过滤、搜索、状态点、最近活动时间和底部“新建项目”。项目 Tab 右键菜单固定包含：暂停、继续、取消、重新开始、恢复 CLI 历史、重命名、移动分组、编辑项目配置。

### 5.3 中央区域

中央必须是 Windows Terminal Control，不显示聊天气泡、自绘 Markdown 消息列表、普通文本滚动列表或自制命令行假界面。终端底部只显示焦点和连接状态，点击终端后键盘直接进入 PTY。

未运行项目时，中央区域显示项目启动面板：

```text
项目名称 / 类型
文件夹: C:\\project\\demo
启动命令: codex
状态: 未运行

[立即开始] [编辑项目]
```

项目运行后，启动面板被同一个项目的 Windows Terminal Control 替换。界面不单独提示 PowerShell，PowerShell 只作为终端内部宿主。

## 6. 项目模型

创建项目必须填写：

| 字段 | 要求 |
|---|---|
| 项目名称 | 必填，可修改 |
| 项目类型 | Codex 或 Claude |
| 启动命令 | 必填，例如 `codex`、`claude` 或完整路径和参数 |
| 项目目录 | 必填，使用系统文件夹选择器选择，必须是存在的目录 |
| 分组 | 默认分组或用户分组 |
| 执行策略 | 固定 `trusted_admin` |

创建成功后先写入 SQLite 并切换到项目详情面板，不立即执行命令。右侧显示项目目录、启动命令、当前状态和“立即开始”“编辑项目”。点击“立即开始”后才创建 Broker/ConPTY、创建 Terminal Control、进入项目目录并执行真实启动命令。启动失败时保留终端和错误信息，不生成假会话。

项目目录字段不能只提供普通文本输入。创建和编辑项目时必须提供原生文件夹选择器；选择结果回填到项目配置，目录不存在或未选择时不能保存项目。

`conversation` 字段：

```text
id
name
provider
executor
cwd
group_name
execution_policy
status
created_at
updated_at
```

状态固定为 `created`、`starting`、`running`、`paused`、`cancelled`、`failed`、`stopped`。

## 7. 分组和恢复

- 永远存在 `默认分组`，不能删除；
- 可以添加分组、移动项目和删除空分组；
- 当前分组、提供方过滤和项目 ID 写入设置；
- 重启桌面后恢复上次分组、项目和终端；
- Codex 使用 `codex resume`，Claude 使用 `claude -r`；
- 不复制 Codex/Claude 私有历史数据库；
- 关闭窗口不删除项目和事件。

## 8. Agent、Broker 和安全

### 8.1 Broker

Broker 创建 ConPTY、启动和终止 CLI、读取输出、接收输入、调整行列数并执行项目级 `trusted_admin`。Broker 不开放公网端口。

### 8.2 Agent

Agent 提供本地 HTTP/WebSocket、SQLite、Android 设备绑定、challenge-response、输入租约、请求去重、Relay 和事件补发。

### 8.3 Android 身份

Android 首次打开 App 生成随机 `phoneId`，私钥只存 Android Keystore。电脑生成二维码，手机 App 内扫码，电脑端确认一次。后续重连使用 challenge-response，不重复确认。撤销设备后立即拒绝请求。

### 8.4 网络

内网使用 TLS 或配对 token，公网只经 Relay。所有手机消息包含 `phoneId`、`conversationId`、`requestId`、序列号、时间戳和签名。WebSocket 断线按 `eventId` 补发。Relay 不保存命令、密钥和完整终端内容。

### 8.5 trusted_admin

`trusted_admin` 是创建项目时的一次授权。项目运行期间不逐条确认高风险命令，但拥有有效设备凭证的手机等价于该项目的远程管理员终端，必须在设备页和项目页明确显示风险。

### 8.6 安全方案沿用说明

桌面 UI 从 Rust/egui 改为 C++/WinUI 3/Terminal Control，不改变手机和电脑之间已经确定的安全模型。以下规则保持不变：

- Android 私钥只存 Android Keystore；
- `phoneId` 是 App 首次生成并持久化的设备唯一标识；
- 首次绑定使用 App 内扫码、电脑端一次人工确认；
- 后续连接使用电脑随机 challenge 和手机私钥签名，不重复确认；
- 每次会话使用短期 capability token，绑定 `phoneId`、电脑 ID、conversationId、权限和过期时间；
- TLS 负责传输加密，签名负责设备身份和消息完整性；
- `requestId`、序列号和时间窗口用于防重放和断线重试去重；
- 设备撤销后立即拒绝 challenge、WebSocket 和输入请求；
- 输入租约保证同一项目同一时刻只有一个控制端写入 PTY；
- Relay 只转发加密消息，不保存私钥、命令、模型密钥和完整终端内容；
- Agent、Broker 和 Terminal Control 只允许本机受保护 IPC，公网不能直接访问 Broker；
- 桌面端关闭窗口不会删除授权设备，但撤销设备必须立即生效。

Terminal Control 只是显示和键盘承载层，不拥有 Android 设备密钥、Relay 凭据或数据库写权限。手机请求仍必须先经过 Agent 的身份验证、权限检查和输入租约，再由 Broker 写入对应 ConPTY。

## 9. SQLite

`events` 保存聊天输入、助手输出、工具输出、终端输入、终端输出和控制事件。Terminal Control 的屏幕状态属于运行时状态，不依赖 SQLite 逐帧恢复。

终端和工具输出默认 100 MB 滚动上限；聊天内容、助手事件、项目配置和控制事件保留。SQLite 使用 WAL 和定期 checkpoint。

## 10. Android 页面

- 顶部 Codex/Claude 切换；
- 项目列表，左侧默认折叠；
- 当前项目实时终端事件；
- 文本输入和控制键；
- 暂停、继续、取消、重新开始；
- 扫码绑定；
- 连接、主题、通知和设备设置。

手机输入必须进入电脑上同一个项目 PTY，不能只写入数据库事件。

## 11. 开发阶段

### 阶段 0：Terminal Control 验证（当前第一目标）

安装 Visual Studio 2022 C++、Windows App SDK、Windows 11 SDK。创建最小 WinUI 3 程序，编译固定版本的 Windows Terminal Control，连接自建 ConPTY，验证 Codex/Claude 的键盘、ANSI、全屏 TUI、复制粘贴和窗口调整。

阶段 0 不通过时，不开始完整桌面 UI。

### 阶段 1：桌面终端核心

完成单项目、单 Codex Terminal Control、ConPTY、本地键盘、窗口调整、Ctrl+C、方向键和全屏 TUI。

### 阶段 2：项目和分组

完成项目创建表单、Codex/Claude、启动命令、目录、分组、右键菜单和 SQLite。

### 阶段 3：项目会话和多项目

完成项目启动面板、内嵌终端会话、多项目 PTY、项目切换、暂停、继续、取消、重启和上次恢复。PowerShell 作为内部终端宿主，不单独作为 UI 功能展示。

### 阶段 4：桌面端完整测试

在开始 Android 之前，必须完成以下桌面端验收：

- Codex 和 Claude 真实 CLI 启动；
- Windows Terminal Control 键盘输入、中文输入、复制粘贴和快捷键；
- 项目启动面板、内嵌终端和项目命令自动执行；
- 多项目、多分组和项目配置编辑；
- 暂停、继续、取消、重新开始和 CLI 历史恢复；
- 关闭窗口后恢复上次项目；
- SQLite 事件记录和终端输出上限；
- 夜间/白天主题、设置和设备页面；
- 管理员权限和本机 IPC；
- 桌面端在目标机器上连续运行测试。

阶段 4 未通过前，不创建 Android 业务页面，不接入公网 Relay。

### 阶段 5：Agent 和 Android

完成 HTTP/WebSocket、Java Android App、Keystore、扫码绑定、challenge-response、手机输入和事件补发。

### 阶段 6：公网和发布

完成 Relay、TLS、输入租约、日志、安装包、运行环境迁移和更新机制。

## 12. 验收标准

- 右侧是真正的 Windows Terminal Control，不是聊天面板或普通文本列表；
- 点击右侧终端即可直接输入，不需要额外聊天输入框；
- Codex 信任确认可以直接按 Enter；
- Codex/Claude 的方向键、Escape、Tab、Ctrl+C、复制和粘贴正常；
- 全屏 TUI 不乱码、不重叠、不出现外层滚动条；
- 新建项目必须选择类型、启动命令、目录和分组，并立即启动真实 CLI；
- 每个项目拥有一个内嵌 Terminal Control，PowerShell 宿主中运行 Codex/Claude 或配置命令；
- 右键项目可以暂停、继续、取消、重启和恢复历史；
- 手机和电脑输入进入同一个 PTY；
- 断网重连已绑定手机不需要重新确认；
- 关闭窗口后重新打开恢复上次项目；
- 不启动浏览器、WebView 或 Electron；
- Rust/Cargo、构建缓存和运行环境放在 F 盘；
- 模型密钥不写入 SQLite、日志或 Android 数据库。

## 13. 关键风险

1. Windows Terminal Control 不是普通稳定公共控件，需要固定源码版本和 C++ 构建补丁；
2. Windows App SDK 与 Terminal Control 版本必须匹配；
3. 多个 Terminal Control 和 ConPTY 会增加内存，需要限制并发项目数；
4. Android 远程输入必须经过签名、租约、去重和撤销检查；
5. `trusted_admin` 不是沙箱，必须持续显示风险；
6. 桌面端目标平台固定为 Windows，Android 独立支持。

## 14. 当前实现状态（2026-08-06）

- 已确认 Visual Studio 2022 Community 17.7、MSVC 14.37、Windows SDK 10.0.22621.0 可用；SDK 实际安装在 `D:\Windows Kits\10`，构建缓存继续放在 F 盘。
- 已下载并解包 Windows App SDK `1.7.250606001` 到 `F:\deps\windowsappsdk-1.7.250606001-pkg`，后续 WinUI 3 工程固定使用此版本。
- 系统已安装 Windows Terminal `1.24.11911.0`，包含 `Microsoft.Terminal.Control.dll`/`.winmd`；该二进制只用于接口和激活探测，不能替代发布时固定的源码集成。
- 已将 Windows Terminal `v1.20.11381.0` 源码解包到 `F:\deps\terminal-v1.20.11381.0`。该版本的 `TerminalControlLib` 依赖 Terminal 私有 NuGet feed；匿名恢复返回 401，必须改用公开等价包或配置凭据后才能编译源码控件。
- 已用公开 NuGet 包和补齐的 GSL/fmt submodule 编译通过 `TerminalControlLib` 与 `Microsoft.Terminal.Control.dll`。为兼容 VS 17.7，F 盘副本使用 GSL 3.1.0，并对 `til/math.h` 做了局部编译器兼容补丁。
- v1.20 的 TerminalControl 元数据仍基于 `Windows.UI.Xaml`（WinUI 2/UWP XAML），不是可直接放入 WinUI 3 树的 `Microsoft.UI.Xaml` 控件。`desktop/src/app/main.cpp` 已实现 `WindowsXamlManager` + `DesktopWindowXamlSource` 适配层，主窗口不再依赖浏览器、WebView 或独立 `wt.exe`。
- 已创建新的 `desktop` C++ 桌面入口并编译为工作区内的 `desktop\bin\Debug\aicli.exe`：深色终端式窗口、项目列表、分组、项目创建/编辑、文件夹选择、启动命令和未运行项目启动面板已落地；项目启动后通过 ConPTY/Terminal Control 创建内嵌终端，在内部终端宿主中进入项目目录并执行真实 `codex`/`claude` 命令。
- 已删除运行时对 `TerminalConnection.dll` 和 `OpenConsoleProxy.dll` 的依赖。`desktop/src/terminal/conpty_connection_bridge.cpp` 直接实现 Windows Terminal 的 `ITerminalConnection`，内部连接系统 ConPTY；`TermControl` 继续负责原生终端渲染、焦点、键盘、ANSI、选择和滚动。
- PowerShell 使用干净的 `powershell.exe -NoLogo -NoExit` 启动。PTY 建立后通过同一个终端输入通道依次写入 UTF-8 初始化、`Set-Location -LiteralPath` 和项目启动命令，不再使用 `-EncodedCommand`。
- `desktop/tools/stage-runtime.ps1` 只部署 TerminalControl、SettingsModel 和 WinUI 2 runtime，并主动清理旧的 TerminalConnection/OpenConsoleProxy 文件，避免旧 DLL 再次进入启动链路。
- 已实测 `aicli.exe` 启动、Codex TUI 渲染、终端直接获得焦点、回车、窗口缩放、取消和重新启动；主进程保持响应，应用事件日志未再出现新的 TerminalConnection 崩溃。
- Android、Relay、公网绑定和手机业务仍冻结，直到阶段 4 桌面验收全部通过。
