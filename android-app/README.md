# AI CLI Android

第一阶段 Android Java/XML 辅助客户端。

- 顶部 Codex/Claude Tab
- 设置入口
- 支持扫码入口、Android Keystore、HTTP/WebSocket 和会话列表

当前环境使用缓存的 Gradle 8.2 和 JDK 17 构建。

## 正式版构建

在项目根目录执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File android-app/tools/build-release.ps1
```

也可以直接双击项目根目录的 `build-android-release.bat` 一键完成正式版构建、
签名验证、发布复制和 SHA-256 生成。

脚本会生成并验证可安装的 Release APK。发布密钥保存在
`F:\Android\AI-CLI\signing\ai-cli-release.p12`，随机密码使用当前 Windows
账户的 DPAPI 加密保存，不会写入 Git。后续正式版本必须继续使用同一把密钥，
否则 Android 无法覆盖升级已经安装的正式版。

输出文件位于：

`app/build/outputs/apk/release/AI-CLI-Android-<版本>-release.apk`

## 已实现

- Java/XML 主页面
- Codex/Claude 顶部 Tab
- 设置入口
- 首次空状态扫码绑定入口
- Android Keystore EC P-256 设备密钥生成和 challenge 签名骨架
- 会话实时事件订阅和 Codex/Claude 聊天消息发送

## 待接入

- App 内相机扫码和二维码校验
- Room 设备/电脑绑定表
- 公钥绑定确认 API、Relay 和输入租约
- conversation 事件的 Android 本地增量缓存
