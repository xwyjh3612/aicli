# 参与 AiCli / Contributing

欢迎使用反馈、文档修正和代码贡献。无需先提交代码，第一次安装或配对时遇到的问题也很有帮助。

## 提交反馈

在 [Issues](https://github.com/xwyjh3612/aicli/issues/new/choose) 选择问题反馈或功能建议。请提供应用版本、系统版本、操作步骤、预期结果与实际结果。图片和日志请移除密码、令牌、配对二维码及私密会话内容。

功能建议请描述实际场景和现在遇到的困难。较大的改动建议先开 Issue 讨论，避免重复工作。

## 修改与验证

- 默认桌面开发目标是原生 C++ / Win32；Qt 与 Android 的改动请明确说明范围。
- 开始前阅读 [项目协作约定](AGENTS.md) 和 [原生模块说明](desktop/src/NATIVE_MODULES.md)。
- 构建原生桌面端时显式使用 `AICLI_USE_QT_UI=OFF`，构建步骤见 [README](README.md)。
- 本机依赖配置、构建缓存、证书和签名材料不提交到 Git。
- 运行与修改相关的测试，在 PR 中说明问题、修改后的行为与验证结果。文档修正通常无需编译。
- 提交 PR 前运行 `git diff --check`，并确认没有带入无关文件。

## English

Bug reports, documentation fixes, and code contributions are welcome. Use [Issues](https://github.com/xwyjh3612/aicli/issues/new/choose) to describe your workflow, app and OS versions, reproduction steps, and expected versus actual behavior. Please remove credentials, pairing QR codes, and private conversations from attachments.

The default desktop target is native C++ / Win32. Read [AGENTS.md](AGENTS.md) and the [native module guide](desktop/src/NATIVE_MODULES.md), and configure with `AICLI_USE_QT_UI=OFF`. State Qt or Android scope explicitly. Keep machine-specific dependency paths and signing materials out of Git. Discuss substantial changes in an issue first, run relevant checks, and describe validation in your PR.
