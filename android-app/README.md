# AI CLI Android

第一阶段 Android Java/XML 辅助客户端。

- 顶部 Codex/Claude Tab
- 设置入口
- 支持扫码入口、Android Keystore、HTTP/WebSocket 和会话列表

当前环境使用缓存的 Gradle 8.2 和 JDK 17 构建。

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
