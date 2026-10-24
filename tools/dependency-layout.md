# 本机共享依赖与构建目录

F 盘用于可复用的 SDK、JDK、工具和第三方库。按依赖名称和版本存放，同一个版本只保留一份，各项目从共享目录引用。项目名称不进入共享依赖路径。

| 内容 | 位置 |
| --- | --- |
| Android SDK（平台、Build Tools、模拟器程序、系统镜像） | `F:\android\sdk` |
| Java JDK | `F:\android\Java\jdk-<版本>` |
| Gradle 程序 | `F:\android\gradle\<版本>` |
| Java/Android 其他公共工具 | `F:\android\<工具名>\<版本>` |
| C/C++ 第三方库、通用 WinRT 头文件 | `F:\deps\<依赖名>\<版本>` |
| Windows Terminal 1.24 x64 运行时依赖 | `F:\deps\windows-terminal\1.24.11911.0\x64` |
| Qt | `F:\Qt\<版本>\<工具链>` |
| Inno Setup 等公共构建工具 | `F:\tools\<工具名>` |
| Gradle 下载的公共 JAR、AAR、POM 等依赖 | `F:\android\repository\gradle\files-2.1` |
| Gradle 解析元数据、编译/转换缓存、守护进程状态 | `%LOCALAPPDATA%\BuildCache\gradle` |
| Android 模拟器数据和用户配置 | `%LOCALAPPDATA%\BuildCache\android` |
| 本项目 Android Gradle 状态和构建产物 | `android-app\.gradle`、`android-app\app\build` |
| 本项目桌面编译与运行时暂存 | `desktop\build*`、`desktop\bin` |
| Cargo 共用编译缓存 | `%LOCALAPPDATA%\BuildCache\cargo\target`，由当前账户的 `CARGO_TARGET_DIR` 指定 |
| Android 本项目签名密钥、DPAPI 密码文件 | `%LOCALAPPDATA%\AiCli\signing` |
| 可直接安装的发布包 | 仓库 `release` |

当前 Java 和 Android SDK 直接复用 `F:\android` 中已安装的版本。Gradle 安装目录从原来的 `.gradle\wrapper\dists` 中提取到 `F:\android\gradle`；可重建的 Gradle 状态移到 C 盘，已下载的依赖保留在 F 盘的公共仓库中。

Gradle 的 `caches\modules-2\files-2.1` 使用目录链接指向 F 盘的公共依赖仓库，同一版本的 JAR、AAR 等跨项目共用一份。编译后的类、转换结果和解析状态留在 C 盘。项目不会各自下载一套 SDK、JDK 或 Gradle。Windows Terminal 源码构建使用 `%LOCALAPPDATA%\BuildCache\third-party` 下共用的工作副本，避免向 F 盘的共享源码写入 `obj`、`bin` 等编译状态。

仓库根目录的 `dependencies.json` 是路径的统一来源。PowerShell 脚本通过 `tools/dependency-paths.ps1` 读取它，CMake 也读取同一份配置。其他项目可以使用相同的共享目录，只需声明所需版本。

Windows Terminal 使用微软 [1.24.11911.0 官方 Release](https://github.com/microsoft/terminal/releases/tag/v1.24.11911.0) 的 x64 ZIP 解压目录作为共享运行时来源。它和同版本的 WinRT 头文件各保存一份，构建时引用共享目录并将运行时暂存到 C 盘。

在仓库根目录准备 Android 构建环境：

```powershell
. .\tools\dependency-paths.ps1
$dependencies = Set-AicliAndroidBuildEnvironment
& (Join-Path $dependencies.gradle 'bin\gradle.bat') -p android-app :app:assembleDebug
```

可通过 `AICLI_ANDROID_ROOT`、`AICLI_NATIVE_ROOT`、`AICLI_TOOLS_ROOT`、`AICLI_QT_ROOT` 调整各类共享依赖的根目录。`AICLI_BUILD_CACHE_ROOT` 调整 C 盘缓存位置；构建脚本拒绝将可写缓存配置到 F 盘。

当前账户的 `GRADLE_USER_HOME`、`ANDROID_AVD_HOME`、`ANDROID_USER_HOME` 和 `CARGO_TARGET_DIR` 使用上述 C 盘目录，覆盖旧的 F 盘设置。新终端会读取更新后的设置；仓库构建脚本立即使用统一配置。

签名密钥及其密码文件始终保留在本机，并继续使用原有密钥以支持覆盖安装；它们不进入 Git。
