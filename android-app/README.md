# AI CLI Android

第一阶段 Android Java/XML 辅助客户端。

- 顶部 Codex/Claude Tab
- 设置入口
- 支持扫码入口、Android Keystore、HTTP/WebSocket 和会话列表

使用 `F:\android\Java\jdk-17.0.11`、`F:\android\sdk` 和 `F:\android\gradle\8.2` 中共享的 JDK、Android SDK 与 Gradle 构建。路径统一来自仓库根目录的 `dependencies.json`。

下载的 JAR/AAR 等依赖共用 `F:\android\repository\gradle\files-2.1`。Gradle 编译和转换缓存位于 `%LOCALAPPDATA%\BuildCache\gradle`，模拟器运行数据位于 `%LOCALAPPDATA%\BuildCache\android`；项目构建产物保留在 `app/build`。目录约定见 [共享依赖规划](../tools/dependency-layout.md)。

## 正式版构建

在项目根目录执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File android-app/tools/build-release.ps1
```

也可以直接双击项目根目录的 `build-android-release.bat` 一键完成正式版构建、
签名验证、发布复制和 SHA-256 生成。

脚本会生成并验证可安装的 Release APK。发布密钥保存在
`%LOCALAPPDATA%\AiCli\signing\ai-cli-release.p12`，随机密码使用当前 Windows
账户的 DPAPI 加密保存，不会写入 Git。后续正式版本必须继续使用同一把密钥，
否则 Android 无法覆盖升级已经安装的正式版。

输出文件位于：

`app/build/outputs/apk/release/AI-CLI-Android-<版本>-release.apk`

## 已实现

- 启动/返回前台自动检查 GitHub Release 更新；设置页支持手动检查和关闭自动检查。
- APK 下载进度、取消、大小/SHA-256/包名/版本/签名校验，以及系统安装授权流程。
- 更新发布规则见 [发布说明](../tools/releasing.md)。

- Java/XML 主页面
- Codex/Claude 顶部 Tab
- 设置入口
- 首次空状态扫码绑定入口
- Android Keystore EC P-256 设备密钥生成和 challenge 签名骨架
- 会话实时事件订阅和 Codex/Claude 聊天消息发送
- 相册一次选择多张图片，按选择顺序上传并追加图片路径；上传过程中可继续添加，完成后统一发送。
- 输入框左侧固定“＋”功能菜单，右侧独立发送按钮；图片上传后保留拍照和相册入口。
- 终端向上滑动后暂停跟随实时输出，滑回底部或点击底部可恢复自动跟随。

## 待接入

- App 内相机扫码和二维码校验
- Room 设备/电脑绑定表
- 公钥绑定确认 API、Relay 和输入租约
- conversation 事件的 Android 本地增量缓存
