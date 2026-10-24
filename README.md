# AiCli

AiCli 是一个面向 Windows 的本地 AI 编程工作区，同时提供 Android 伴随客户端。它把 Codex、Claude 和其他命令行 AI 工具组织成可管理的项目，并在内嵌终端中运行；配对 Android 手机后，还可以通过局域网访问电脑上的项目、会话和实时终端。

当前仓库的主维护目标是非 Qt Win32 桌面版本。仓库仍保留 Qt UI 构建路径和 Android 客户端，但新功能和验证应优先覆盖非 Qt Win32 版本。

## 功能

- 管理项目、分组、项目类型、工作目录和启动命令。
- 内置 Codex、Claude 类型，也支持 Gemini 或其他自定义 CLI。
- 使用 Windows Terminal TermControl、XAML Island 和 Microsoft ConPTY 提供原生终端。
- 优先使用 PowerShell 7，未检测到或启动失败时回退 Windows PowerShell 5.1。
- 自动恢复 Claude 当前目录的最近会话；没有会话时创建新会话。
- 通过标题协议和通用终端输出检测 CLI 工作状态，在项目列表及任务栏显示动画。
- 使用 SQLite 保存项目、类型、分组、设备、设置和会话事件。
- Android 端支持扫码配对、HTTPS/WSS、终端同步、消息输入和多张图片上传；左侧“＋”随时展开功能菜单。
- 设置页显示应用版本和实际使用的 PowerShell 环境。

桌面端当前版本为 1.0.0.3，Android 端当前版本为 1.1.0。

## 目录结构

~~~text
.
├─ desktop/                    Windows 桌面端（C++20）
│  ├─ src/                     应用、终端、Agent、数据和 UI 源码
│  ├─ tests/                   桌面端测试
│  ├─ tools/                   构建、运行时暂存和辅助脚本
│  ├─ installer/               Inno Setup 安装脚本
│  ├─ package/                 Appx 清单和打包资源
│  └─ assets/                  图标资源
├─ android-app/                Android Java/XML 客户端
├─ tools/                      仓库级辅助工具
├─ build-installer.bat         构建非 Qt Windows 安装包
└─ build-android-release.bat   构建 Android Release APK
~~~

## 环境要求

### Windows 桌面端

- Windows 10 20H1（10.0.19041）或更高版本，x64。
- Visual Studio 2022、MSVC 和 CMake 3.26+。
- Windows 10/11 SDK；当前工程使用 10.0.22621.0。
- Windows App SDK/WinRT、ConPTY、OpenSSL、CivetWeb 和 QR Code 依赖。
- PowerShell 7 推荐但不是必需；软件支持常规安装和 Microsoft Store 版，并可回退到 Windows PowerShell 5.1。

共享依赖路径统一声明在根目录 `dependencies.json`：Android/Java 使用现有 `F:/android`，C/C++ 通用依赖使用 `F:/deps`，Qt 和公共工具分别使用 `F:/Qt`、`F:/tools`。同一版本跨项目共用一份。编译缓存和产物位于 C 盘，具体目录见 [共享依赖规划](tools/dependency-layout.md)。

### Android 客户端

- JDK 17。
- Android SDK 35，最低 API 26。
- Gradle 8.2 和 Android Gradle Plugin 8.2.2。

## 构建非 Qt 桌面端

在仓库根目录执行：

~~~powershell
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake -S desktop -B desktop\build -G 'Visual Studio 17 2022' -A x64 `
    '-DAICLI_USE_QT_UI=OFF' `
    '-DAICLI_SKIP_PACKAGED_ACTIVATION=OFF' `
    '-DAICLI_PACKAGE_IDENTITY_NAME=AiCli.Desktop.Debug' `
    '-DAICLI_APPLICATION_USER_MODEL_ID=AiCli.Desktop.Debug_jw3zr00rmb0ym!App' `
    '-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=C:/project/ai_code/codex_tool/desktop/bin'
& $cmake --build desktop\build --config Debug --target aicli -- /m:2
& .\desktop\tools\register-runtime.ps1 -Output .\desktop\bin\Debug
~~~

日常非 Qt 编译统一使用 `desktop/build`。Debug 使用独立的
`AiCli.Desktop.Debug` 包身份，注册后可与已安装的 Release 版本同时运行。
正式安装包仍使用安装包脚本的独立构建目录和 packaged activation 流程。

Qt UI 可通过 AICLI_USE_QT_UI=ON 构建。当前 Qt 开发环境和运行时注册细节参见 desktop/README.md。

## 构建 Windows 安装包

构建当前主维护的非 Qt 安装包：

~~~powershell
.\build-installer.bat
~~~

或直接执行：

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\desktop\tools\build-windows-installer.ps1
~~~

脚本会构建 Release、暂存 Windows Terminal/WinUI/ConPTY 运行时、验证包身份，再通过 Inno Setup 生成安装程序和 SHA-256 文件。输出位于：

~~~text
release\AI-CLI-Setup-<版本>-x64.exe
release\AI-CLI-Setup-<版本>-x64.exe.sha256
~~~

构建 Qt 安装包时向脚本添加 -Qt 参数。

## 构建 Android Release

~~~powershell
.\build-android-release.bat --no-pause
~~~

或：

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\android-app\tools\build-release.ps1
~~~

脚本会构建并验证签名 APK，生成 SHA-256，然后复制到 release/。正式签名配置使用以下环境变量：

~~~text
AICLI_RELEASE_KEYSTORE
AICLI_RELEASE_STORE_PASSWORD
AICLI_RELEASE_KEY_ALIAS
AICLI_RELEASE_KEY_PASSWORD
~~~

不要将 keystore、密码或 DPAPI 密钥文件提交到 Git。Android 正式版本必须继续使用同一签名密钥，才能覆盖升级已安装的应用。

## 测试

桌面端提供以下 CMake 测试目标：

- aicli_data_store_tests：SQLite 数据、默认类型迁移、项目与设备数据。
- aicli_terminal_stream_tests：终端 VT 流编码、快照和事件发布。
- aicli_terminal_working_detector_tests：Codex/Claude 状态检测、通用输出兜底和输入过滤。
- aicli_powershell_runtime_tests：PowerShell 7、Store 版入口和 5.1 回退。
- aicli_qt_project_list_tests：Qt UI 开启时的项目列表测试。

构建非 Qt 测试：

~~~powershell
& $cmake --build desktop\build --config Debug --target aicli_data_store_tests aicli_terminal_stream_tests aicli_terminal_working_detector_tests aicli_powershell_runtime_tests -- /m:2
~~~

运行示例：

~~~powershell
& .\desktop\bin\Debug\aicli_terminal_working_detector_tests.exe
& .\desktop\bin\Debug\aicli_terminal_stream_tests.exe
& .\desktop\bin\Debug\aicli_powershell_runtime_tests.exe
& .\desktop\bin\Debug\aicli_data_store_tests.exe "$env:TEMP\aicli-data-store-test.db"
~~~

aicli_data_store_tests 必须传入一个临时 SQLite 数据库路径。

## 终端运行时

桌面端将 Windows Terminal TermControl 与匹配版本的 Microsoft ConPTY/OpenConsole 并置部署。当前使用 ConPTY 1.24.260710001 和 x64 OpenConsole.exe。

不要随意替换为系统 CreatePseudoConsole/conhost.exe。Codex 等全屏 TUI 会使用内联历史和复杂 VT 重绘，匹配的 OpenConsole 运行时用于保留正确的滚动和绘制行为。

PowerShell 7 检测支持常规安装和 Microsoft Store 版。如未自动识别到，请把 pwsh.exe 所在文件夹加入 PATH。安装、升级或修改 PATH 后，请完全退出并重启 AiCli。

## Android 配对与远程 Agent

桌面端 Agent 默认监听 4280 端口并提供 HTTPS/WSS：

1. 在桌面设置中配置监听地址、端口和对外显示地址。
2. 使用 Android 客户端扫描桌面端二维码。
3. 在桌面端确认设备绑定。
4. 使用配对设备访问项目、会话和实时终端。

仓库不自动部署公网 Relay。公网访问需要自行配置 VPN、端口映射或反向代理。请只在可信网络开放 Agent 端口。

## 数据与安全

- 桌面数据保存在当前 Windows 用户的本地 AiCli 数据目录。
- Android 配对使用设备密钥和签名请求，并支持重放保护、输入租约和设备撤销。
- codex --yolo 和 claude --dangerously-skip-permissions 只关闭各 CLI 自身的确认，不会赋予应用 Windows 管理员权限。
- packaged desktop 当前运行在 mediumIL，不应将远程 Agent 当作管理员权限代理。
- 不要提交证书、二维码内容、签名材料或其他本地凭据。

## 开发约定

- 非 Qt Win32 是当前主维护路径；除非任务明确要求，不要修改 qt_* 源文件。
- 调整终端链路前先阅读 desktop/README.md 和运行时暂存脚本。
- 桌面版本号以 desktop/package/AppxManifest.xml 为单一来源，CMake 会生成 UI 使用的版本常量。
- 构建缓存、暂存运行时和签名材料不应提交到 Git；指定的 `release` 安装包和校验文件保留在版本管理中，便于其他电脑直接安装。
- 提交前建议运行 git diff --check 和 git status --short。

## 相关文档

- desktop/README.md：桌面构建和 Windows Terminal 运行时细节。
- android-app/README.md：Android 客户端和 Release 构建说明。
- tools/dependency-layout.md：F 盘共享依赖和 C 盘缓存规划。
- ai-cli-mobile-architecture.md：桌面 Agent 与移动端架构。
- 开发上下文备份-2026-08-18.md：历史开发上下文备份。

## License

仓库当前未提供独立 LICENSE 文件。如需分发或二次开发，请先确认项目所有者授权及第三方依赖许可证。
