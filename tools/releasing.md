# 发布 Win32 与 Android 更新

客户端读取固定地址：

`https://github.com/xwyjh3612/aicli/releases/latest/download/update-manifest.json`

每个正式 Release 同时提供 Windows 安装器、APK、各自的 `.sha256` 文件和一份 `update-manifest.json`。两端可以使用不同版本号；Android 是否升级以递增的 `versionCode` 为准，Windows 按版本数字逐段比较。已发布的版本和安装包不覆盖。

## 发布步骤

1. 修改 `desktop/package/AppxManifest.xml` 的版本号；修改 Android 时递增 `android-app/app/build.gradle` 中的 `versionCode` 和 `versionName`。以后只更新某一端时，另一端使用该端已发布的安装包即可。
2. 运行相关测试。更新元数据测试为 `aicli_update_manifest_tests` 和 Gradle 的 `:app:testDebugUnitTest`；Android 改动同时运行 `:app:lintDebug`。
3. 执行根目录 `build-installer.bat` 和 `build-android-release.bat --no-pause`。只改一端时只构建该端，另一端在 `release/` 中保留当前安装包及校验文件。
4. Android 必须使用原有正式签名密钥；现有构建脚本从用户本机的 DPAPI 保护文件加载。不要提交密钥、密码或 token。
5. 提交源码和约定保留的安装包并推送到 GitHub 默认分支；更新 `.gitignore` 的安装包白名单以保留新版本。自动更新通过 Releases 分发，仓库内文件便于直接下载和历史留存。
6. 准备 UTF-8 发布说明，例如 `release/notes.md`。运行发布工具（Python 3.11+）：

```powershell
# 只生成并检查本地清单，不访问 GitHub
python tools/publish-release.py --tag v1.2.0

# 上传草稿的全部附件，检查远端大小与摘要后发布为 latest
python tools/publish-release.py --tag v1.2.0 --notes-file release/notes.md --publish
```

工具优先使用 `GH_TOKEN` / `GITHUB_TOKEN`，否则使用 Git Credential Manager 已保存的 GitHub 凭据。凭据只发送到 GitHub API，不写入产物或日志。需要仓库 Release 写入权限。

上传失败时保持草稿，现有稳定版继续提供更新；使用同一提交、同一 tag 重试即可。工具拒绝覆盖正式 Release，拒绝未提交或尚未推送的源码，也拒绝构建校验文件缺失或不匹配的安装包。

## 清单格式

```json
{
  "schemaVersion": 1,
  "windows": {
    "version": "1.0.0.4",
    "url": "https://github.com/xwyjh3612/aicli/releases/download/v1.2.0/AI-CLI-Setup-1.0.0.4-x64.exe",
    "sha256": "安装包的 64 位小写 SHA-256",
    "size": 123456
  },
  "android": {
    "version": "1.2.0",
    "versionCode": 3,
    "url": "https://github.com/xwyjh3612/aicli/releases/download/v1.2.0/AI-CLI-Android-1.2.0-release.apk",
    "sha256": "APK 的 64 位小写 SHA-256",
    "size": 123456
  }
}
```

示例中的大小与摘要仅解释格式；正式清单必须由工具根据实际产物生成。客户端只接受本仓库的 HTTPS Release 下载地址，并拒绝不匹配的文件名、非法版本、非法大小与摘要。Windows 只接受 x64 Win32 安装器。

## 验证

发布后用不带 GitHub 凭据的请求读取固定清单地址；确认两端版本、附件地址、文件大小和 SHA-256。两端“检查更新”应能返回“当前已是最新版本”。测试升级时使用同签名、较低版本的隔离测试安装；不要覆盖正在执行任务的生产桌面程序。

当前更新安装需要用户确认。Android 的“允许安装未知应用”和系统安装确认由 Android 控制，普通应用无法静默绕过。GitHub 网络不可达时可稍后重试，或直接下载 Release 附件手动覆盖安装。
