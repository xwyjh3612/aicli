# AI CLI 桌面端与 Android 辅助端实现说明

更新时间：2026-08-15

## 1. 开发背景

AI CLI 主要在 Windows 电脑上使用。电脑端拥有项目目录、启动命令和实际执行权限，负责运行 Codex/Claude、显示原生终端、管理分组和绑定设备；Android App 在不方便坐在电脑前时用于查看运行进度、切换会话、继续输入，也可以向已绑定电脑安全发起新建项目请求。

固定要求如下：

- 电脑端不使用浏览器、WebView、Electron，也不嵌入独立 `wt.exe` 窗口；
- 电脑中央区域必须是真正的 Windows Terminal `TermControl`，键盘、快捷键、选择、复制、滚动和全屏 TUI 都在终端内部完成；
- PowerShell 只作为终端内部宿主，不作为产品功能单独展示；
- Android 使用 Java/XML，不使用 Kotlin；
- 手机首次绑定只在电脑确认一次，断网、App 重启和电脑重启后不重复确认；
- 手机输入必须进入电脑当前项目的同一个 ConPTY，不能只写入聊天记录；
- Codex 默认使用 `codex --yolo resume --last`；滚轮和右侧滚动条操作 TermControl 原生滚动历史，不自动打开 Codex Transcript，也不向终端注入方向键或 PgUp/PgDn；
- Claude 默认使用 `claude --dangerously-skip-permissions --continue`；
- 项目数据、设备授权和终端事件使用 SQLite 持久化；
- 构建依赖、Gradle 缓存、Windows Terminal 源码和大体积运行环境放在 F 盘。

## 2. 当前技术结构

```text
Qt 6.8 Widgets 桌面窗口
    ├─ 项目、分组、设置、设备和绑定模态框
    ├─ Windows Terminal TermControl
    │      └─ 自建 ITerminalConnection
    │             └─ Microsoft ConPTY 1.24 conpty.dll
    │                    └─ 同版本 x64/OpenConsole.exe
    │                           └─ powershell.exe
    │                                  └─ Codex / Claude / 自定义 CLI
    ├─ SQLite DataStore
    └─ HTTPS/WSS Remote Agent
             ⇅ TLS + 签名请求
        Android Java App
```

桌面端使用 C++20、Qt 6.8 Widgets、Win32/XAML Island、Windows Terminal 1.24 控件、同版本 Microsoft ConPTY、CivetWeb、OpenSSL 和系统 WinSQLite。Android 使用 Java 17、AndroidX、Material、OkHttp、Gson、ZXing 和 Android Keystore。

`TerminalConnection.dll` 和 `OpenConsoleProxy.dll` 不进入运行链路。桌面桥接层直接实现 Windows Terminal 要求的 `ITerminalConnection`，但进程传输固定使用官方 `Microsoft.Windows.Console.ConPTY 1.24.260710001` 包中的 `conpty.dll` 和 `x64/OpenConsole.exe`；Windows Terminal Control 只负责原生终端渲染、输入和 scrollback。

### 2.1 桌面 UI 整体架构（Qt 重构基线）

桌面业务、终端后端和远程 Agent 保持原 C++ 实现，界面层改为成熟的 Qt Widgets。Qt 只负责桌面控件和布局，不替换 Windows Terminal，不引入 WebView。

```text
QApplication
└─ MainWindow（无系统 caption 的 QMainWindow）
   ├─ TitleBarWidget       // 分组、绑定、状态、设置和窗口按钮
   ├─ Sidebar              // QLineEdit + 可拖动 ProjectListWidget
   ├─ WorkspaceStack
   │  ├─ TerminalHostWidget（原生 HWND）
   │  │     └─ XAML Island → Windows Terminal TermControl
   │  └─ ProjectPrestart   // 未启动项目的目录、命令和立即开始
   ├─ Footer               // 状态、git、Agent、内存和公网信息
   └─ WorkspaceScrim + frameless QDialog
         // 项目、设置、绑定、项目类型和分组模态

DesktopController
├─ DataStore / GitRepository / RemoteAgent
└─ XamlIslandAdapter / 多项目 ConPTY 会话
```

旧 Win32 owner-draw、`StyledInput`、`ModalHost` 和 GDI 项目列表不再链接进 `aicli` 目标；后续桌面 UI 功能直接使用 Qt 的输入框、下拉框、列表、布局、对话框和样式系统。

#### 2.1.1 单一主题源

- `ThemeManager` 持有唯一 `ThemePalette`，主题只有 `Dark` 和 `Light`，默认 `Dark`；
- `MainWindow::applyTheme()` 一次更新 `QApplication` 的 `QPalette`、全局 QSS、DWM 边框/圆角以及 TermControl 主题；
- 主窗口、项目卡片、输入框、下拉框、按钮、设置页和二层模态都读取同一组颜色 token，不在各页面保存独立 RGB；
- 设置模态打开时切换主题必须立即刷新底层窗口、当前模态和原生终端，不要求关闭设置或重启项目；
- Codex TUI 输入区属于终端单元格。终端桥接仍把可主题化背景映射到 ANSI 色表，主题切换后通过真实 ConPTY 尺寸变化请求 TUI 重绘；原始 VT 数据继续原样推送给 Android。

#### 2.1.2 自定义标题栏与系统窗口行为

- `MainWindow` 使用 `Qt::FramelessWindowHint`，同时给 HWND 保留 `WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU`；
- 标题栏左侧和中间区域由 Qt 控件构成，右侧最小化、最大化/还原和关闭按钮使用 Windows 风格交互；
- `WM_NCHITTEST` 提供窗口拖动和四边缩放，最大化、还原、最小化、Win+方向键和系统阴影继续由 Windows 完成；
- DWM 只扩展四周 1 像素边缘，提供系统阴影、主题边框和圆角，不生成第二层 caption；
- `nativeEvent()` 只使用当前消息携带的 `MSG.hwnd`，禁止在本机事件回调中再次调用 `winId()`，避免 HWND 创建和布局重入导致 `TextShaping.dll` 崩溃。

#### 2.1.3 Qt 模态层

- 项目、设置和绑定内容使用 owned、frameless `QDialog`，不再创建传统 Win32 新窗口；
- 打开模态时先显示独立 `WorkspaceScrim`，遮罩从 52 像素标题栏下方开始，只覆盖工作区和状态栏；夜间和日间使用不同半透明度；
- 模态内容保持不透明和可交互，scrim 不抢焦点；关闭模态不会销毁 `TerminalHostWidget`、XAML Island、TermControl 或 ConPTY；
- 项目类型和分组编辑作为设置页之上的二层 Qt 模态，仍使用同一主题和输入组件；
- 模态定位、最小化、最大化和 DPI 变化由 Qt 布局与窗口事件统一处理，不使用截屏、`BitBlt` 或透明恢复技巧。

#### 2.1.4 Qt 与原生终端边界

- `TerminalHostWidget` 设置 `WA_NativeWindow`，只提供稳定的子 HWND 和尺寸事件；
- XAML Island、TermControl、键盘输入、光标、选择、复制和 scrollback 全部留在 Windows Terminal 控件内部；
- Qt 不绘制假终端、不放置外部聊天输入框，也不把滚轮转换为方向键；
- `DesktopController` 是 UI 与业务的唯一中介，Qt 页面不直接管理 SQLite、Agent 或 ConPTY 生命周期；
- 多个项目仍各自持有独立终端会话，切换左侧项目只更换当前显示会话，不结束后台项目。

#### 2.1.5 验收条件

1. 夜间 → 白天 → 夜间时，标题栏、设置、输入框、项目列表和 TermControl 同步变化；
2. 设置打开时可切换主题，关闭后终端内容和焦点仍存在；
3. 最小化、恢复、最大化、还原、拖边缩放和系统快捷键正常；
4. 100% 与高 DPI 下标题栏、项目列表、模态和终端不重叠；
5. TermControl 支持输入、选择、复制、原生滚动、Codex inline TUI 和 Claude TUI；
6. 双击独立 Qt 入口后能通过包身份激活，并启动 `OpenConsole.exe → powershell.exe → 项目命令`；
7. Qt UI 崩溃日志中不得再出现因 HWND 重入引起的 `TextShaping.dll / 0xc000041d`。

## 3. 电脑端功能

### 3.1 主窗口

电脑端入口：

```text
C:\project\aitest\codex_tool\desktop\bin\Qt\Debug\aicli.exe
```

主窗口默认夜间模式，支持白天/夜间主题切换。窗口使用系统原生最小化、最大化/还原和关闭按钮，中央终端使用原生滚动、选择、复制粘贴和键盘焦点。

外观设置使用一张统一卡片，不显示额外说明段落。卡片每行左侧为 label、右侧为紧凑设置按钮：`应用主题 → 夜间模式/白天模式`、`任务栏缓冲动画 → 开启/关闭`、`缓冲样式 → 进度条/图标转圈`、`作用范围 → 当前项目/所有项目`。修改后立即生效；转圈样式保持主图标不动，由 UI 线程按帧更新 Overlay Icon，任务结束后立即清除。默认保持开启、使用进度条样式并作用于所有项目，Qt 和非 Qt 版本读取同一组 SQLite 设置。

标题栏信息固定包含：

```text
[AI CLI] [当前分组 (项目数) ▼] [+] [绑定手机] [已绑定 N 台] [Agent 状态] [内存] [公网状态] [设置]
```

当前分组使用下拉菜单直接切换，每个菜单项显示 `分组名称 (项目数)`；旁边的 `+` 只负责快速新增分组。标题栏不再放 Codex/Claude 全局过滤按钮。左侧 `AI CLI` 下方旧的 `Local Workplace` 已删除。

### 3.2 项目列表

左侧显示当前分组中的项目，支持：

- 使用 Qt `QLineEdit` 搜索项目名称和项目类型；
- 搜索提示使用类似 HTML `placeholder` 的灰色占位文字；
- 每个项目只用一行显示项目名称，不在列表里重复显示项目类型和分组；
- 状态只显示紧凑图标：运行中为绿色环形点、暂停为黄橙色双竖线、停止/取消/失败为黄色警告三角、未运行为灰色空心圆；
- 项目卡片由 Qt item delegate 绘制 2 像素圆角，鼠标经过时改变背景色和边框色；
- 项目支持上下拖动排序，但只有按住卡片左侧的三横线把手才会开始拖动；其他区域点击只选中项目；
- 双击项目或点击“立即开始”启动项目；
- 右键菜单包含打开项目文件夹、项目详情、移动分组、重新启动、暂停、继续、取消和删除；
- 删除项目只删除 SQLite 中的项目配置，不删除项目目录及其文件；
- 启动时恢复上次选择的分组和项目，并自动重新运行上次项目的启动命令；
- 左下 Git 状态卡只检查当前项目目录，显示“未初始化”“已初始化但尚无提交”或“已有提交”，兼容普通仓库、父目录仓库和 Git worktree。

### 3.3 新建和编辑项目

新建/编辑使用主窗口拥有的 frameless Qt 模态框，不显示独立任务栏窗口。模态框背景使用半透明 `WorkspaceScrim`；关闭模态框后恢复同一个终端控件，不把终端区域清空。

项目字段：

| 字段 | 行为 |
|---|---|
| 项目名称 | 必填，可编辑，作为项目 Tab/列表标题 |
| 项目类型 | 新建时从动态类型列表选择；编辑已有项目时只读 |
| 分组 | 从动态分组列表选择，后续可移动 |
| 项目目录 | 必须通过系统文件夹选择器选择存在的目录 |
| 启动命令 | 根据项目类型自动填入默认值，也允许当前项目覆盖 |
| 执行策略 | AI CLI 使用无逐条确认模式；Codex/Claude 默认命令已固定 |

项目创建后先写入 SQLite，右侧显示目录、启动命令、状态、“立即开始”和“编辑项目”。只有点击“立即开始”或双击项目时才创建真实 ConPTY 并运行命令。每个项目拥有独立的 TermControl、ConPTY 和 PowerShell 进程；启动项目 B 不得关闭已经运行的项目 A。

### 3.4 动态项目类型

设置页提供“管理项目类型”：

- `Codex` 和 `Claude` 是安装后自动创建的预置类型；
- 预置类型可以手工修改名称和默认启动命令，修改后仍保留预置保护标记；
- 预置类型不可删除，避免误删安装默认配置；
- 用户可以新增自定义类型，只填写类型名称和默认启动命令；
- 自定义类型可以重命名、修改默认命令和删除；
- 项目类型列表页不内嵌编辑输入框；新增和修改统一打开二层模态框，在同一模态框内填写名称与默认启动命令；
- 重命名类型时，SQLite 事务会同步更新已有项目；
- 类型仍被项目使用时禁止删除；
- 新建项目选择类型后自动填入该类型的默认命令；
- 编辑已有项目时类型保持只读，避免改变既有会话执行语义。

内置默认命令：

```text
Codex  : codex --yolo resume --last
Claude : claude --dangerously-skip-permissions --continue
```

### 3.5 动态分组

标题栏当前分组控件是直接切换用的下拉菜单，菜单项统一显示 `分组名 (项目数)`；标题栏 `+` 打开只包含分组名称输入框的新增模态框。完整管理入口位于“设置 → 分组管理”。分组功能包括：

- 永远存在 `默认分组`；
- 默认分组不可重命名、不可删除；
- 可以新增、重命名和切换分组；
- 新增和重命名使用设置页之上的第二层模态框，背景保留半透明遮罩；
- 分组重命名时同步更新已有项目；
- 非空分组禁止删除，必须先移动其中的项目；
- 项目右键菜单可以直接移动到任意分组；
- 当前分组写入 SQLite 设置，重启后恢复。

### 3.6 原生 AI 终端

右侧运行区域只有真正的 Windows Terminal `TermControl`。项目启动流程为：

```text
选择项目
  → 由官方 1.24 conpty.dll 创建 side-by-side OpenConsole
  → 启动 powershell.exe -NoLogo -NoExit
  → 设置 UTF-8 输入/输出
  → Set-Location 到项目目录
  → 在同一 PTY 中输入项目启动命令并回车
```

#### ConPTY 兼容性约束

以下结论已经通过实际缓冲区数据确认，后续不得再回退：

- `TermControl`、`Microsoft.Terminal.Control.dll`、`conpty.dll` 和 `OpenConsole.exe` 必须保持 1.24 同代版本；当前固定为 Terminal `1.24.11911.0` 与 ConPTY NuGet `1.24.260710001`；
- 运行目录必须同时包含 `conpty.dll` 和 `x64/OpenConsole.exe`；安装包也必须携带这两个文件；
- 桥接层使用一根安全匿名全双工 overlapped 管道，同一个客户端句柄同时传给 ConPTY 输入和输出；启动子进程时设置 `STARTF_USESTDHANDLES`；
- ConPTY 使用 grapheme 宽度测量，与 Windows Terminal 对中文、emoji 和组合字符的宽度模型保持一致；
- 客户端进程挂接完成后调用 `ConptyReleasePseudoConsole`，关闭时调用 `ConptyClosePseudoConsole`；
- 不使用 `TerminalConnection.dll`，因为其私有 WinRT 连接实现曾在当前系统上反复崩溃；
- 不得改回系统 `CreatePseudoConsole`/`conhost.exe`。系统 ConPTY 可以正常处理普通 PowerShell 输出，但会把 Codex inline TUI 的 history insertion 转换成当前视口整屏重绘，造成 `BufferHeight == ViewHeight`、滚动条最大值为零；
- 不得用 Transcript、覆盖层、假滚动条、PgUp/PgDn 注入或滚轮按键模拟掩盖 ConPTY 问题。

终端支持：

- 多个项目同时运行，每个项目的进程、终端缓冲区、光标、输入和滚动状态互相独立；
- 点击左侧项目只切换当前可见的 TermControl，未显示的项目继续在后台运行；
- “暂停”“取消”“重新启动”和删除项目只作用于目标项目，不影响其他正在运行的项目；
- 手机输入按项目 ID 写入对应 ConPTY，即使该项目不是电脑端当前可见项目也不得误发到其他终端；
- 直接点击获得焦点并输入；
- Enter、Backspace、Tab、Escape、方向键、Home/End、PageUp/PageDown；
- Ctrl+C、Ctrl+D、Ctrl+L；
- 中文输入、复制、粘贴、文本选择；
- Windows Terminal 原生滚动和全屏 TUI；
- Codex 恢复会话只使用 TermControl 原生 scrollback；鼠标滚轮、触控板滚动和 Ctrl+滚轮统一交给 TermControl 的原生 `OnMouseWheel` 处理，宿主不得自行计算缓冲偏移；不得由宿主自动打开 Codex Transcript，不显示 Transcript 的上下箭头、PgUp/PgDn 等分页提示，也不得把滚轮动作转换为发送给 CLI 的按键；
- 原生滚动条沿用 Windows Terminal 的 TermControl 模板：鼠标离开时保持细窄的 thumb 指示，指针靠近右侧滚动区域时展开为可抓取的完整 thumb/track；XAML Island 只同步视觉状态，不绘制第二条 Qt 滚动条、不覆盖终端内容。
- Codex/Claude ANSI、光标和弱化文字颜色；
- 最小化后恢复、窗口缩放和最大化；
- 终端输出同时写入 SQLite 事件并通过 WSS 推送给手机。

### 3.7 设置

设置窗口使用左侧固定导航、右侧功能内容的结构；左侧入口保持可见，右侧内容独立滚动。当前包含：

- 外观：白天/夜间主题，默认夜间，使用紧凑型切换按钮；点击后立即写入 SQLite 并同步刷新主窗口、标题栏和原生终端，切换期间不得残留浅色方形按钮边缘；
- Agent 监听地址，默认 `0.0.0.0`；
- HTTPS/WSS 端口，默认 `4280`；
- 本机/内网访问地址；
- 公网访问地址；
- 手机工作目录：项目和附件共用的上级目录；手机新建项目保存到 `conversation\项目名称`，手机图片保存到 `attachments\项目ID`；
- 手机访问服务：默认关闭；开启后启动 HTTPS/WSS Agent，关闭后停止监听、断开 WebSocket，并清理短期 token、认证 challenge、输入租约和待确认配对；
- 项目类型：动态 CLI 类型管理；
- 分组管理：显示 `分组名 (项目数)`，支持新增、改名和安全删除；
- Windows 防火墙“允许手机访问”；
- 保存设置并按手机访问服务开关启动或停止 Agent。

本机地址和公网地址只接受 `https://`。修改端口后需要重新执行一次防火墙配置。
手机访问服务关闭时不监听任何手机访问端口，“绑定手机”入口只提示先到当前设置页开启服务；重新启动软件后按已保存的开关状态恢复。

### 3.8 设备管理和绑定

点击“绑定手机”打开二维码模态框：

- 二维码由桌面端生成，五分钟有效；
- 二维码包含电脑 ID、配对 ID、随机 challenge、TLS 证书指纹、内网地址和公网地址；
- 手机必须使用 AI CLI App 内扫码入口；
- 手机提交设备名称、型号、随机 `phoneId`、公钥和 challenge 签名；
- 电脑显示手机信息，用户点击一次“确认绑定”；
- 绑定完成后标题栏更新已绑定设备数；
- 设备页显示已绑定和已撤销设备；
- 点击解绑会立即撤销短期 token、输入租约和该设备的 WSS 连接；
- 同一手机重新扫码后可以再次绑定。

## 4. Android 辅助端功能

Android App 使用 Java/XML，当前最低 Android 版本为 API 26。

### 4.1 主页面

- 页面使用原生 Java/XML + Material 3 深色视觉，不使用 WebView；整体由现代顶部栏、动态分组、会话抽屉、会话状态卡、终端内容区和消息输入区组成；
- 顶部栏使用紧凑的三行信息布局，高度固定为 `80dp`：第一行主标题显示电脑端设置的本机名称，手机端服务端备注只用于服务器列表，不覆盖此标题；第二行不显示状态文字，局域网使用网络节点图标、公网使用地球图标，并统一使用四态颜色：未连接为灰色、连接中为蓝色闪烁、已连接为绿色、连接失败为红色；未配置地址归入“未连接”，只有对已配置地址的实际探测失败才显示“连接失败”；当前实际使用的已连接链路在图标旁显示小型确认标识，不再显示“当前”文字；第三行显示 `项目名称 · 项目类型`，不再显示当前分组；顶部仍保留会话抽屉、扫码和设置入口；
- “设置 → 服务端管理”中的每台电脑显示同一套四态连接图标与文字；打开设置后立即探测全部已绑定服务端，之后每 15 秒刷新一次。任一已配置的局域网或公网地址通过证书指纹和电脑 ID 校验即为“已连接”，全部实际探测失败才为“连接失败”；地址未配置时保持“未连接”。
- 会话列表默认折叠，通过顶部按钮从左侧展开，项目根据运行、暂停、取消/失败和就绪状态使用不同颜色；
- 不再写死 Codex/Claude 两个页面或过滤按钮，项目类型、分组、目录和状态全部读取电脑端动态数据；
- 终端显示使用 Java VT 模拟器和自定义 `TerminalView`，直接消费按序号推送的 VT 数据；普通 `terminal.snapshot` 只作为旧版本和 HTTP 恢复兼容数据，不能混入实时屏幕造成重复内容；
- 分组使用下拉框切换，每项显示 `分组名 (项目数)`，并提供“全部”视图；
- 会话抽屉标题栏提供“创建项目”入口。手机只填写项目名称并选择电脑端返回的动态项目类型、项目分组；目录由电脑端“手机工作目录”下的 `conversation` 子目录生成，启动命令只取所选电脑端项目类型的默认命令。创建成功后刷新项目列表、选中新项目但不自动启动；
- 选择项目后加载 SQLite 历史事件；
- 建立 WSS 获取实时终端输出和状态；
- WSS 断线后按最后 `eventId`、`terminalSessionId` 和 `sequence` 补发遗漏事件并自动重连；
- 可输入文字或终端命令，回车后写入电脑同一个 PTY；
- 输入为空时右侧操作按钮显示 `+`；点击后隐藏软键盘，并在输入栏下方原位替换为终端扩展面板。Android 11 及以上使用 `WindowInsets.Type.ime()` 读取当前输入法真实高度；旧版 Android 使用 `adjustResize` 后页面高度差读取真实键盘高度，扣除系统导航栏后缓存为扩展面板高度。切换输入法或键盘高度变化后自动更新；输入法和扩展面板切换期间保持相同底部占位，终端与输入栏不得上下跳动。扩展面板提供换行显示切换、上下左右、回车、拍照和相册；再次点击上尖头或触摸输入框会收起面板并恢复软键盘；输入非空时同一按钮自动切换为发送图标；
- 拍照使用完整照片文件，相册使用系统图片选择器。App 在本机按最长边 2048 像素缩放并压缩到 5MB 以内，经 TLS、短期 token 和设备私钥逐请求签名上传；电脑再次校验 JPEG/PNG/WebP 文件头，保存至“手机工作目录”下对应项目的 `attachments` 目录，并把电脑本地绝对路径填入手机消息输入框，等待用户补充问题后手动发送；
- 主页面不提供暂停、继续、取消和重启按钮，会话控制以电脑端为主；
- 终端显示面占满会话页除顶部栏和消息输入区之外的剩余高度；消息输入栏是终端下方的正常布局节点，不作为浮层，也不覆盖终端内容；
- 未绑定、连接失败、分组为空等状态使用独立空状态页面，不显示失效的终端控件；
- 每个最近访问项目保留独立终端状态和滚动位置，最多缓存 6 个项目，屏幕历史限制约 1000 行；CLI 和模型密钥始终留在电脑端。

### 4.2 终端实时同步

电脑到手机的实时终端协议固定为：

```text
terminal.reset  // terminalSessionId、baseSequence、rows、columns
terminal.bootstrap // 最终渲染画面的末尾 200 行、历史 token 和对应 sequence
terminal.data   // terminalSessionId、递增 sequence、原始 VT data
terminal.resize // terminalSessionId、递增 sequence、rows、columns
```

- 协议版本为 `terminalProtocol: 2`。手机首次打开一个运行中的会话时，不再重放最多 12MB 的原始 ConPTY 历史；电脑读取 TermControl 最终渲染画面，只发送最后 200 个逻辑行作为首屏；
- Android 建立 WSS 时主动声明 `terminalTransport=packed-lz4-v1`。支持该能力的电脑把约 20ms 内按顺序产生的 `terminal.*` JSON 事件无损合并到一个二进制 WebSocket 包，并对重复的 ANSI/VT 重绘内容执行 LZ4 压缩；解包后交给现有事件处理器的每条 JSON 和原协议逐字节一致，不丢弃中间帧、不降低刷新率，也不改变颜色、光标、Working 动画或 TUI 键盘行为；
- 二进制传输采用显式能力协商。旧版电脑忽略参数并继续发送 JSON 文本帧；电脑只对主动声明能力的手机发送压缩包；Android 若收到损坏或不支持的二进制包，停止应用该包并自动以原 JSON 传输重新连接，不猜测或部分渲染终端数据；
- 单个压缩批次的目标未压缩大小为 512KB，最大解包安全上限为 4MB，待发送终端队列上限为 8MB。网络严重阻塞超过上限时关闭该 WSS，由现有 `terminalSessionId + sequence` 断线补发恢复，禁止静默丢帧；
- `terminal.bootstrap` 对应一个确定的 VT sequence，首屏之后只补发该 sequence 之后的新 `terminal.data`/`terminal.resize`，Working 动画和 TUI 中间重绘不会作为历史再次刷屏；
- 手机上滑到顶部时，通过签名 HTTPS 接口按历史 token 每次读取更早的 200 行；历史 token 固定首次连接时看到的画面，因此分页期间的新输出不会造成行偏移或重复；
- 手机终端最多保留 1000 个逻辑行。分页插入旧内容后保持当前视口位置，实时输出仍使用完整 ANSI/VT 颜色；首次 200 行是 TermControl 展平后的最终文字画面，新产生的实时内容继续保留终端颜色和 TUI 行为；
- HTTP 请求/响应、单条 WebSocket 文本消息和单个压缩后二进制消息均设置 1MB 安全上限；桌面端会主动拆分较大的 ConPTY 输出块和压缩批次，避免单帧越界；
- Android 对每个项目保存 `terminalSessionId` 和最后已应用 `sequence`；只接受同一会话且严格连续的下一条事件；
- 发现序号间隙时，手机重新建立 WSS，并把当前 session 和 sequence 发给电脑，请求只补发缺少的尾部数据；重复或旧序号直接忽略，因此补发不会造成内容重复；
- 手机发送聊天成功后，在 220ms 和 900ms 各执行一次带当前 sequence 的轻量 WSS 重连，覆盖“最后一帧丢失后没有下一帧触发 gap 检测”的情况；该机制适用于 Claude、Codex 和自定义 CLI；
- Claude 等 TUI 会使用 DECSET 2026 同步绘制。Android 正常等待同步绘制结束序列，但设置 180ms 最大刷新等待，避免结束序列延迟或丢失后页面永久停在旧画面；
- 只有用户原本接近底部时才自动跟随最新输出；用户正在上翻历史时保留滚动位置，不因模型思考输出被强制拉回底部；
- 切换项目时保存原项目横向、纵向滚动位置，恢复目标项目自己的终端状态，不重新拼接整段文本。

### 4.3 扫码和设置

- App 首次打开即生成随机 `phoneId`；
- P-256 私钥在 Android Keystore 内生成，不能从 App data 导出；
- Android 9 及以上设置为设备解锁后才可使用私钥；
- App 内置 ZXing 扫码页面，不调用浏览器或外部扫码应用；
- 设置页显示当前电脑 ID、当前地址、内网地址、公网地址、证书指纹和手机 ID；
- 可以保存 HTTPS 地址、重新扫码或清除当前电脑连接；
- 清除电脑连接不会删除 Android Keystore 私钥和 `phoneId`；
- 通知和震动开关已持久化。

配对完成后，主页面会比较完整连接配置，而不仅比较 URL；如果证书指纹、电脑 ID、内网地址或公网地址变化，会重新创建客户端并加载新的 TLS pin，避免继续使用旧证书配置。

## 5. 手机连接流程

### 5.1 首次绑定

```text
电脑点击“绑定手机”
  → 电脑生成 5 分钟二维码
  → 手机 App 扫码
  → 手机 Keystore 私钥签名配对 challenge
  → 电脑显示手机名称、型号和 phoneId
  → 电脑点击一次“确认绑定”
  → 电脑保存 phoneId、公钥和设备信息
  → 手机请求认证 challenge
  → 手机签名 challenge
  → 电脑签发 15 分钟短期 token
  → Android 加载会话并连接 WSS
```

### 5.2 后续重连

网络断开、手机 App 重启或电脑重启后不重新弹出电脑确认：

```text
手机读取已保存的电脑地址和证书指纹
  → 电脑返回新的随机认证 challenge
  → 手机用同一 Keystore 私钥签名
  → 电脑用已绑定公钥验签
  → 电脑签发新的短期 token
```

电脑重启会清空旧 token，但不会删除已绑定公钥，因此只重新认证，不重新配对。

## 6. 安全通信

该工具允许手机输入进入高权限 AI CLI，安全边界按“远程管理员终端”处理。

### 6.1 设备身份

- 手机身份由随机 `phoneId` 和 Android Keystore P-256 私钥共同组成；
- `phoneId` 不是秘密，只负责稳定标识设备；
- 私钥不通过二维码、HTTP、WSS、SQLite 或日志传输；
- 电脑只保存手机公钥、名称、型号、绑定时间、最后活动时间和最后序列号；
- 首次配对签名绑定电脑 ID、配对 ID、challenge、phoneId、设备信息和公钥，防止替换字段。
- 手机创建项目使用相同的 TLS pin、短期 token、设备签名和防重放校验；手机请求中不接受项目目录和启动命令，避免远程请求绕过电脑端配置写入任意路径或注入任意启动命令。

### 6.2 TLS 和证书固定

- Agent 只提供 HTTPS/WSS；
- 首次启动生成 P-256 自签名 TLS 证书；
- Android 从二维码获取 SHA-256 证书指纹并固定；
- 使用固定指纹时允许内网 IP/公网域名变化，但证书必须完全匹配；
- TLS pin 初始化失败时 Android 直接拒绝连接，不回退到不安全信任链；
- TLS PEM 同时包含私钥，Windows ACL 每次启动都会重新收紧，只允许当前文件所有者和 `SYSTEM` 完全访问。

### 6.3 请求签名和防重放

配对后的每个 HTTP 请求都包含：

```text
Authorization: Bearer <short-lived-token>
X-Aicli-Phone
X-Aicli-Request-Id
X-Aicli-Timestamp
X-Aicli-Sequence
X-Aicli-Signature
```

签名原文固定为：

```text
HTTP_METHOD
CANONICAL_PATH
REQUEST_ID
UNIX_TIMESTAMP
SEQUENCE
SHA256(BODY)
```

电脑端执行以下检查：

- token 存在且未过期；
- token 对应设备仍处于 active；
- `X-Aicli-Phone` 与 token 设备一致；
- 时间戳在电脑时间前后五分钟内；
- sequence 严格大于该设备最后接受的值；
- requestId 未使用过；
- 请求体 SHA-256 和 ECDSA 签名正确；
- 检查通过后，在同一个 SQLite 事务中记录 requestId、sequence 和 lastSeen。

### 6.4 输入租约

- 每个项目同一时刻只允许一台手机写入；
- 手机第一次输入后获得 30 秒租约；
- 同一手机继续输入会续期；
- 另一台手机在租约期间写入会收到 HTTP 409；
- 手机可主动 `release`；
- 设备解绑会立即清除该设备持有的所有租约。

### 6.5 解绑

解绑后立即执行：

- SQLite 中设备标记为 inactive；
- 清空该设备所有内存 token；
- 清空该设备输入租约；
- 向该设备的 WSS 发送关闭帧；
- 后续 challenge、token、签名 HTTP 请求和 WSS 都被拒绝；
- 手机若需再次使用，必须重新扫码并由电脑重新确认。

## 7. Agent 接口

| 接口 | 用途 | 身份要求 |
|---|---|---|
| `GET /api/health` | Agent 健康状态和电脑 ID | 无 |
| `POST /api/pair/request` | 提交手机公钥和配对签名 | 有效二维码 |
| `GET /api/pair/status` | 查询电脑是否确认 | 配对 ID |
| `GET /api/auth/challenge` | 获取后续认证 challenge | 已绑定设备 |
| `POST /api/auth/token` | 用设备私钥签名换短期 token | 已绑定设备 |
| `GET /api/conversations` | 获取项目/会话列表 | token + 请求签名 |
| `GET /api/project-creation/options` | 获取电脑端项目根目录、动态项目类型和分组 | token + 请求签名 |
| `POST /api/projects` | 按名称、类型 ID、分组 ID 在电脑端安全创建项目 | token + 请求签名 |
| `GET /api/conversations/{id}/events` | 获取历史和断线补发事件 | token + 请求签名 |
| `POST /api/conversations/{id}/input` | 写入原始 PTY 输入 | token + 签名 + 输入租约 |
| `POST /api/conversations/{id}/chat` | 写入文本并追加回车 | token + 签名 + 输入租约 |
| `POST /api/conversations/{id}/attachment` | 上传手机拍照/相册图片并返回电脑本地路径；单文件最大 6MB | token + 请求签名 |
| `POST /api/conversations/{id}/control` | 暂停/继续/取消/重启/release | token + 签名 + 输入租约 |
| `WSS /api/events` | 实时终端和状态事件 | token + 请求签名 |

WSS 只负责服务器向手机推送事件；手机输入和控制始终走单独签名的 HTTPS 请求。

## 8. SQLite

SQLite 当前表：

- `project_types`：动态项目类型和默认启动命令；
- `groups`：动态项目分组；
- `projects`：项目名称、类型、分组、目录、命令和状态；
- `devices`：手机公钥、设备信息、状态和最后序列；
- `device_requests`：已使用 requestId，用于防重放；
- `events`：终端输入、终端输出和会话控制事件；
- `settings`：主题、地址、端口、当前分组、上次项目、电脑 ID，以及任务栏缓冲动画开关、显示样式和作用范围。

数据库启用 WAL、`synchronous=NORMAL`、外键和五秒 busy timeout。事件文件超过约 100 MB 时，优先分批删除最早的 `terminal.output`，项目配置、设备授权和控制事件继续保留，并执行被动 checkpoint。

SQLite 不保存模型 API Key、Android 私钥或 TLS 私钥。TLS 私钥单独保存在受 ACL 保护的 PEM 文件中。

## 9. 公网访问

当前版本已经支持在设置中填写公网 HTTPS 地址，并把该地址写入绑定二维码。可用部署方式为：

```text
公网域名或公网 IP:端口
  → 路由器端口映射 / VPN / 反向隧道
  → Windows 电脑 Agent 的 HTTPS/WSS 端口
```

当前仓库没有部署一个托管的云 Relay 服务；`remote_address` 是公网直连、VPN 或自建反向代理地址。即使使用公网地址，仍然必须通过 TLS 指纹固定、设备签名、短期 token、防重放和输入租约。

公网使用前必须完成：

- 固定或可解析的公网地址；
- 路由器端口映射、VPN 或反向隧道；
- Windows 防火墙允许 Agent 端口；
- 不把 Broker/ConPTY 句柄或 SQLite 直接暴露到公网；
- 只暴露 Remote Agent 的 HTTPS/WSS 端口。

## 10. 权限说明

Codex/Claude 默认命令已经关闭 AI CLI 自身的逐条高风险确认。当前桌面包为 `mediumIL` 的 packaged classic app，`trusted_admin` 表示 AI CLI 执行策略，不自动等同于 Windows UAC 提权。

如果项目必须拥有真正的 Windows 管理员令牌，需要后续增加独立的高权限 Broker/Windows Service，由当前 UI 通过受保护本机 IPC 连接；不能简单把公网 Agent 直接放进管理员进程。当前版本不得在文档或 UI 中把 `--yolo`/`--dangerously-skip-permissions` 描述成已经获得 Windows Administrator token。

## 11. 构建和运行

### 11.1 桌面端

```powershell
$cmake='C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake -S desktop -B desktop\build-qt -G 'Visual Studio 17 2022' -A x64 `
  -DCMAKE_RUNTIME_OUTPUT_DIRECTORY='C:/project/aitest/codex_tool/desktop/bin/Qt' `
  -DAICLI_APPLICATION_USER_MODEL_ID='AiCli.Desktop.QtDev_jw3zr00rmb0ym!App' `
  -DAICLI_PACKAGE_IDENTITY_NAME='AiCli.Desktop.QtDev'
& $cmake --build desktop\build-qt --config Debug --target aicli -- /m:2
.\desktop\tools\register-runtime.ps1 -Output .\desktop\bin\Qt\Debug
.\desktop\bin\Qt\Debug\aicli.exe
```

Qt 6.8.3 MSVC x64 位于 `F:\Qt\6.8.3\msvc2022_64`。CMake 构建后会自动执行 `windeployqt` 和 `stage-runtime.ps1`，部署 Qt、Windows Terminal、WinUI、ConPTY、OpenSSL、PRI、XBF、清单与资源。`register-runtime.ps1` 使用独立 QtDev 包身份注册目录；直接双击 `aicli.exe` 会转发到有包身份的实例，以满足 WinUI/XAML Island 初始化要求。构建前只需停止正在使用同一输出目录的 `aicli.exe`，旧版 `desktop\bin\Debug` 与 QtDev 输出可以并存。

### 11.2 数据层测试

```powershell
& $cmake --build desktop\build-qt --config Debug --target aicli_data_store_tests -- /m:2
.\desktop\bin\Qt\Debug\aicli_data_store_tests.exe F:\codex_tool-target\desktop\tests\aicli-data-store-test.db
```

### 11.3 Android

```powershell
$env:JAVA_HOME='F:\android\Java\jdk-17.0.11'
$env:ANDROID_HOME='F:\android\sdk'
$env:ANDROID_SDK_ROOT='F:\android\sdk'
$env:GRADLE_USER_HOME='F:\android\.gradle'
F:\android\.gradle\wrapper\dists\gradle-8.2-bin\bbg7u40eoinfdyxsxr3z4i7ta\gradle-8.2\bin\gradle.bat --no-daemon :app:assembleDebug :app:assembleDebugAndroidTest
```

APK：

```text
android-app\app\build\outputs\apk\debug\app-debug.apk
```

### 11.4 Android 真机联调

局域网可直接使用二维码中的内网地址。开发机也可使用 ADB 反向映射：

```powershell
F:\android\sdk\platform-tools\adb.exe -s 607faa8d reverse tcp:4280 tcp:4280
.\tools\run-android-pairing-test.ps1 -LocalAddressOverride https://127.0.0.1:4280
```

## 12. 已完成验证

截至 2026-08-09 已完成：

- Qt 6.8 Widgets 桌面端 Debug clean build 成功，运行环境自动部署成功；
- 独立 `AiCli.Desktop.QtDev` 包身份注册成功，双击 Qt 入口可以启动有包身份的实例；
- Qt 主窗口、XAML Island、TermControl、匹配版 `OpenConsole.exe`、PowerShell 和 Codex 完整启动链路通过；
- 设置/项目/绑定 frameless Qt 模态与标题栏下方半透明 scrim 层级验证通过；
- Qt 本机事件 HWND 重入崩溃已修复，启动检查期间 Windows 应用日志无新增相关崩溃；
- Android 主 APK 和 AndroidTest APK 构建成功；
- 原生 TermControl、ConPTY、Codex 启动、键盘输入和滚动可用；
- PowerShell 200 行原生缓冲验证为 `buffer=205 / view=33 / scrollbar_max=172`；
- 默认 Codex 恢复会话在系统 `conhost.exe` 下为 `buffer=33 / view=33 / scrollbar_max=0`，改用匹配版本 `conpty.dll + OpenConsole.exe` 后为 `buffer=289 / view=33 / scrollbar_max=256`，确认聊天历史已经进入 TermControl 原生 scrollback；
- Android 扫码、手机公钥提交和电脑一次确认成功；
- 配对后销毁 Android 客户端并重新创建，仍可用 Keystore 私钥重新认证，不再次确认；
- HTTPS 会话列表和 SQLite 历史事件成功；
- WSS `hello` 和实时事件成功；
- 手机发送 Ctrl+L/Ctrl+C 类控制输入进入真实桌面 PTY，并广播 `terminal.input`；
- 电脑解绑手机后，原设备重新认证和会话请求被拒绝；
- 动态类型新增、重命名、预置类型可编辑/不可删除、使用中删除保护通过数据层测试；
- 分组新增、重命名、默认分组保护、非空删除保护通过数据层测试；
- 类型/分组重命名会事务性更新项目；
- 管理端标题栏分组下拉会显示每组项目数量并可直接切换；
- 设置页四个左侧导航、右侧独立滚动、分组/项目类型二层模态框和主题即时切换已完成启动检查；
- Android 主页面已改为动态分组和动态项目类型，Material 3 新页面 Debug APK 构建并覆盖安装成功；
- Android 已按 `terminalSessionId + sequence` 消费实时 VT 流，手机发送后主动补发尾部事件；Claude DECSET 2026 同步绘制增加 180ms 最大刷新等待，避免发送后看不到最新内容；
- Android 会话首屏已改为 TermControl 最终画面的末尾 200 行，向上每页加载 200 行、最多保留 1000 行；WSS 仅补发首屏 sequence 之后的 VT 增量，桌面 Qt/非 Qt 共用同一协议实现；
- requestId、sequence 重复请求和撤销设备请求被拒绝；
- TLS PEM ACL 只允许文件所有者和 `SYSTEM` 完全访问。

Android 真机测试结果：

```text
OK (1 test)
```

## 13. 当前边界和后续扩展

以下内容未伪装成已经完成：

- 桌面端运行期间已经支持多个项目终端同时后台常驻；应用进程退出时系统会关闭全部 ConPTY，重新打开后只自动恢复最后选择的项目，其他项目需要再次启动；
- 当前公网方案是 HTTPS 直连、端口映射、VPN 或反向隧道，仓库内没有托管 Relay 服务；
- 当前 Windows 进程是 `mediumIL`，尚未拆分真正的管理员 Broker/Service；
- Android 通知和震动设置已保存，但完整的后台通知调度尚未实现；
- Agent 已实现输入租约，但 Android 页面尚未显示租约持有设备和 30 秒倒计时；
- Android 当前只管理一个电脑连接配置；多电脑列表可作为后续扩展。

这些扩展不得破坏现有约束：桌面继续使用原生 TermControl，手机私钥继续留在 Android Keystore，公网入口继续只暴露经过认证的 HTTPS/WSS Agent。
