# AI CLI 桌面端与 Android 辅助端实现说明

更新时间：2026-08-08

## 1. 开发背景

AI CLI 主要在 Windows 电脑上使用。电脑端负责创建项目、运行 Codex/Claude、显示原生终端、管理分组和绑定设备；Android App 只在不方便坐在电脑前时使用，用于查看运行进度、切换会话、继续输入和执行暂停/继续/取消/重启。

固定要求如下：

- 电脑端不使用浏览器、WebView、Electron，也不嵌入独立 `wt.exe` 窗口；
- 电脑中央区域必须是真正的 Windows Terminal `TermControl`，键盘、快捷键、选择、复制、滚动和全屏 TUI 都在终端内部完成；
- PowerShell 只作为终端内部宿主，不作为产品功能单独展示；
- Android 使用 Java/XML，不使用 Kotlin；
- 手机首次绑定只在电脑确认一次，断网、App 重启和电脑重启后不重复确认；
- 手机输入必须进入电脑当前项目的同一个 ConPTY，不能只写入聊天记录；
- Codex 默认使用 `codex --yolo --no-alt-screen resume --last`，使用官方稳定的内联 TUI 模式保留 Windows Terminal 原生滚动历史；滚轮和右侧滚动条只操作 TermControl 原生 scrollback，不自动打开 Codex Transcript，也不向终端注入方向键或 PgUp/PgDn；
- Claude 默认使用 `claude --dangerously-skip-permissions --continue`；
- 项目数据、设备授权和终端事件使用 SQLite 持久化；
- 构建依赖、Gradle 缓存、Windows Terminal 源码和大体积运行环境放在 F 盘。

## 2. 当前技术结构

```text
Windows 原生桌面窗口
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

桌面端使用 C++20、Win32/XAML Island、Windows Terminal 1.24 控件、同版本 Microsoft ConPTY、CivetWeb、OpenSSL 和系统 WinSQLite。Android 使用 Java 17、AndroidX、Material、OkHttp、Gson、ZXing 和 Android Keystore。

`TerminalConnection.dll` 和 `OpenConsoleProxy.dll` 不进入运行链路。桌面桥接层直接实现 Windows Terminal 要求的 `ITerminalConnection`，但进程传输固定使用官方 `Microsoft.Windows.Console.ConPTY 1.24.260710001` 包中的 `conpty.dll` 和 `x64/OpenConsole.exe`；Windows Terminal Control 只负责原生终端渲染、输入和 scrollback。

### 2.1 桌面 UI 整体架构（2026-08-08 重构基线）

主题切换、标题栏和模态层不再采用逐个控件修补的方式，固定按下面三层实现：

```text
DesktopFrame（唯一窗口边界和标题栏所有者）
├─ TitleBarHost       // 自绘左侧信息 + 三个系统风格按钮
├─ WorkspaceHost      // 项目列表、详情、TermControl、状态栏
└─ ModalHost          // 只覆盖 WorkspaceHost，标题栏始终保留
   └─ ModalSurface     // 设置/绑定/项目/二层编辑模态
```

#### 2.1.1 单一主题源

- `ThemeManager` 持有唯一 `ThemePalette`，主题只有 `Dark` 和 `Light` 两个状态；默认 `Dark`。
- 调色板 token 统一包含窗口、面板、输入框、边框、正文、弱化文字、选中、悬停、按下、标题栏、危险/警告/成功等颜色。
- 主窗口、设置窗口、二层模态、StyledInput 和 TermControl 都从同一份 palette 更新；视图不再保存自己的 RGB 常量。
- 切换主题采用一次事务：写入 SQLite → 更新 `ThemeManager` → 更新 DWM 边框/TermControl palette → 更新控件 → 一次性无擦除重绘根窗口。设置模态保持打开时，标题栏、工作区、原生终端和设置页必须在同一次操作中生效，不能延迟到关闭模态框。
- Codex TUI 的输入区域由终端单元格组成，不是独立输入框。主题更新后通过真实 ConPTY 行列变化要求 TUI 重绘；探测尺寸和恢复尺寸必须跨两个时间片发送，禁止连续同步调用后被 ConPTY 合并。显示链路只映射 Codex 固定的输入区真彩背景，其他 VT 序列和供手机同步的原始输出保持不变。
- 新建/编辑项目页的面板、输入框、下拉框和按钮全部读取同一 `ThemePalette`，禁止保留只适用于夜间模式的硬编码背景色。

#### 2.1.2 标题栏唯一所有者

- 根窗口使用无系统 caption 的客户区；保留 `WS_THICKFRAME` 负责调整大小。
- 左侧标题、分组下拉、绑定设备、设置等内容和右侧最小化/最大化/关闭 glyph 全部由 `DesktopFrame` 绘制和命中测试。
- 不调用 `DwmDefWindowProc` 处理 caption 按钮。根窗口只通过 `DwmExtendFrameIntoClientArea` 保留四周 1 像素合成边缘，让 DWM 提供原生窗口阴影和系统圆角；不得把 frame 扩展成第二条 caption，也不得让 DWM 覆盖自定义标题栏。
- 标题栏按钮的 hover/pressed/active 状态来自同一 palette，最大化时只切换 glyph，不创建第二套按钮 HWND。
- 非最大化窗口必须有清晰的 DWM 阴影和主题边框，使深色窗口叠在深色内容上时仍能辨认边界。

#### 2.1.3 ModalHost 生命周期

- 模态由两个线程内 owned popup 组成：一层 `WS_EX_LAYERED` 半透明 scrim 和一层不透明内容窗口；设置页从标题栏下方开始，标题栏始终只保留一套按钮。
- `ModalHost` 自己绘制确定性的 scrim 和面板，不使用 `BitBlt` 截屏，不依赖桌面当前前台窗口，也不通过截图恢复背景。透明度统一由 `ThemePalette::scrim_opacity` 管理，日间为 `0.34`，夜间为 `0.54`。
- 打开模态时保留底层 WorkspaceHost 和 TermControl，不销毁 TermControl、不重启 ConPTY；关闭时暂停一次宿主重绘，销毁两层窗口后只合并刷新其覆盖区域。
- 二层项目类型/分组编辑沿用同一 ModalHost 生命周期，不能再创建独立顶层窗口或嵌套 DWM caption。
- 窗口最小化、最大化、恢复和 `WM_DPICHANGED` 都只重新计算统一 `LayoutMetrics`，不改变模态状态。
- 项目列表的 owner-draw 行使用离屏双缓冲；列表 `WM_ERASEBKGND` 必须用当前主题的侧栏色填满整个客户区，包括最后一项以下的空白区域，禁止露出桌面或其他应用。

#### 2.1.4 统一几何指标

`LayoutMetrics::ForWindow()` 按 DPI 计算标题栏高度、底部状态栏高度、侧栏宽度、caption 按钮宽度、模态圆角和输入框圆角。布局函数只能读取这组指标，禁止在不同文件再次写入 `52/30/286` 等 magic number。

#### 2.1.5 验收条件

每次 UI 重构必须一次性验证以下序列，而不是只验证某个颜色：

1. 夜间 → 白天 → 夜间，标题栏、设置、输入框、TermControl 同步变化；
2. 设置打开时最小化 → 恢复、最大化 → 还原、关闭，模态仍在且标题栏按钮只有一套；
3. 关闭设置后，项目列表、详情和原生终端焦点恢复，已运行 ConPTY 不退出；
4. 100% 和高 DPI 下标题栏按钮、模态面板、滚动区域不重叠；
5. TermControl 仍支持输入、复制、原生滚动和 Codex 全屏 TUI。
6. 设置框保持打开时切换夜间/日间，已有 Codex 会话的输入区背景和新增项目页控件立即采用新主题，不允许要求重启项目或关闭设置框。

本节是后续桌面 UI 开发的约束。若新功能需要改变标题栏所有权、模态生命周期或主题 token，必须先修改本节设计，再实现代码，禁止在旧绘制路径上继续叠加补丁。

## 3. 电脑端功能

### 3.1 主窗口

电脑端入口：

```text
C:\project\aitest\codex_tool\desktop\bin\Debug\aicli.exe
```

主窗口默认夜间模式，支持白天/夜间主题切换。窗口使用系统原生最小化、最大化/还原和关闭按钮，中央终端使用原生滚动、选择、复制粘贴和键盘焦点。

标题栏信息固定包含：

```text
[AI CLI] [当前分组 (项目数) ▼] [+] [绑定手机] [已绑定 N 台] [Agent 状态] [内存] [公网状态] [设置]
```

当前分组使用下拉菜单直接切换，每个菜单项显示 `分组名称 (项目数)`；旁边的 `+` 只负责快速新增分组。标题栏不再放 Codex/Claude 全局过滤按钮。左侧 `AI CLI` 下方旧的 `Local Workplace` 已删除。

### 3.2 项目列表

左侧显示当前分组中的项目，支持：

- 使用公共 `StyledInput` 搜索项目名称和项目类型；
- 搜索提示使用类似 HTML `placeholder` 的灰色占位文字；
- 每个项目只用一行显示项目名称，不在列表里重复显示项目类型和分组；
- 状态只显示紧凑图标：运行中为绿色环形点、暂停为黄橙色双竖线、停止/取消/失败为黄色警告三角、未运行为灰色空心圆；
- 项目卡片使用 GDI+ 抗锯齿圆角，鼠标经过时改变背景色和边框色；
- 项目支持上下拖动排序，但只有按住卡片左侧的三横线把手才会开始拖动；其他区域点击只选中项目；
- 双击项目或点击“立即开始”启动项目；
- 右键菜单包含打开项目文件夹、项目详情、移动分组、重新启动、暂停、继续、取消和删除；
- 删除项目只删除 SQLite 中的项目配置，不删除项目目录及其文件；
- 启动时恢复上次选择的分组和项目，并自动重新运行上次项目的启动命令；
- 左下 Git 状态卡只检查当前项目目录，显示“未初始化”“已初始化但尚无提交”或“已有提交”，兼容普通仓库、父目录仓库和 Git worktree。

### 3.3 新建和编辑项目

新建/编辑使用主窗口内模态层，不创建独立顶级窗口。模态框背景使用半透明遮罩；关闭模态框后恢复同一个终端控件，不把终端区域清空。

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
Codex  : codex --yolo --no-alt-screen resume --last
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
- 项目类型：动态 CLI 类型管理；
- 分组管理：显示 `分组名 (项目数)`，支持新增、改名和安全删除；
- Windows 防火墙“允许手机访问”；
- 保存设置并重启 Agent。

本机地址和公网地址只接受 `https://`。修改端口后需要重新执行一次防火墙配置。

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

- 页面使用原生 Java/XML + Material 3 深色视觉，不使用 WebView；整体由现代顶部栏、动态分组、会话抽屉、会话状态卡、终端内容区、控制区和消息输入区组成；
- 顶部栏显示 AI CLI 和电脑连接状态，并提供会话抽屉、扫码和设置入口；
- 会话列表默认折叠，通过顶部按钮从左侧展开，项目根据运行、暂停、取消/失败和就绪状态使用不同颜色；
- 不再写死 Codex/Claude 两个页面或过滤按钮，项目类型、分组、目录和状态全部读取电脑端动态数据；
- 分组使用下拉框切换，每项显示 `分组名 (项目数)`，并提供“全部”视图；
- 选择项目后加载 SQLite 历史事件；
- 建立 WSS 获取实时终端输出和状态；
- WSS 断线后按最后 `eventId` 补发遗漏事件并自动重连；
- 可输入文字或终端命令，回车后写入电脑同一个 PTY；
- 支持暂停、继续、取消和重启；
- 终端显示面占满会话页除状态栏之外的剩余高度，控制栏和消息输入栏浮在终端底部；通过布局比例增加可视区域，不缩小终端字号；
- 未绑定、连接失败、分组为空等状态使用独立空状态页面，不显示失效的终端控件；
- 本地只保留有限长度的显示文本，CLI 和模型密钥始终留在电脑端。

### 4.2 扫码和设置

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
| `GET /api/conversations/{id}/events` | 获取历史和断线补发事件 | token + 请求签名 |
| `POST /api/conversations/{id}/input` | 写入原始 PTY 输入 | token + 签名 + 输入租约 |
| `POST /api/conversations/{id}/chat` | 写入文本并追加回车 | token + 签名 + 输入租约 |
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
- `settings`：主题、地址、端口、当前分组、上次项目和电脑 ID。

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
& $cmake -S desktop -B desktop\build -G 'Visual Studio 17 2022' -A x64
& $cmake --build desktop\build --config Debug --target aicli -- /m:2
.\desktop\tools\stage-runtime.ps1
.\desktop\bin\Debug\aicli.exe
```

构建前必须停止正在运行的 `aicli.exe`，否则链接器会因文件锁定报 `LNK1168`。

### 11.2 数据层测试

```powershell
& $cmake --build desktop\build --config Debug --target aicli_data_store_tests -- /m:2
.\desktop\bin\Debug\aicli_data_store_tests.exe F:\codex_tool-target\desktop\tests\aicli-data-store-test.db
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

2026-08-07 已完成：

- 桌面端 Debug 构建成功；
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
