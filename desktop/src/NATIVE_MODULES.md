# 原生 C++ / Win32 模块说明

默认桌面开发入口是原生版本。以下模块不依赖 Qt 控制器；Qt 和 Android 有各自的入口。

## 模块职责

| 模块 | 负责什么 | 后续扩展入口 |
| --- | --- | --- |
| `app/main.cpp` | 主窗口生命周期、模型加载、项目和终端操作的协调 | 主窗口级行为、把新功能接入已有模块 |
| `app/native_remote_bridge.h/.cpp` | Agent 回调注册、跨线程消息、请求完成与取消、消息载荷释放 | 新远程操作的消息派发及 `UiCallbacks` |
| `ui/project_editor_dialog.h/.cpp` | 新建/编辑项目表单、草稿、校验、下拉菜单、主题与字体刷新 | 新项目字段及表单交互 |
| `features/project_configuration.h/.cpp` | 项目类型的默认启动命令选择 | 默认命令规则；供表单和会话启动共用 |
| `ui/management_dialog.h/.cpp` | 管理窗口入口、外层消息循环、关闭与资源释放 | 管理窗口生命周期 |
| `ui/settings_content.cpp` | 设置页导航、页面内容、布局、滚动、更新状态 | 新设置页及页面布局 |
| `ui/management_controls.cpp` | 控件创建、主题应用、初始内容填充、控件文本读写 | 新控件及其初始值 |
| `ui/management_actions.cpp` | 设置保存、分组/类型操作、设备操作、防火墙设置 | 设置命令与持久化动作 |
| `ui/management_renderer.cpp` | 外层布局、按钮/列表绘制、管理窗口绘制 | 外观和绘制逻辑 |
| `ui/group_name_dialog.h/.cpp` | 分组名称输入窗口 | 分组命名交互 |
| `ui/project_type_editor_dialog.h/.cpp` | 项目类型名称、命令、默认类型开关窗口 | 类型编辑交互 |
| `ui/dialog_metrics.h` | 弹窗 DPI 缩放和字体创建 | 弹窗共用的尺寸/字体工具 |

`ui/internal/management_dialog_state.h` 是管理模块内部的状态、控件 ID、页面枚举和函数声明。
业务调用者使用公开的弹窗头文件，不要把内部状态作为外部接口。

## 扩展方式

- 增加项目字段：在模型与数据层定义字段，在 `ProjectEditorDialog` 中处理输入、布局和草稿；
  `Show()` 只在确认时返回结果，保存仍由调用者负责。取消不得覆盖调用者的结果。
- 增加设置页：在内部状态中增加页面/控件 ID，在 `management_controls.cpp` 创建控件，
  在 `settings_content.cpp` 接入导航、可见性、布局、滚动范围和页面内容；保存动作进入
  `management_actions.cpp`，按钮/列表的外观进入 `management_renderer.cpp`。
- 增加远程 UI 动作：在 `native_remote_bridge` 定义消息及派发，在 `UiCallbacks` 声明窗口侧动作，
  由主窗口提供实现。后台线程通过消息调用 UI，保持同步请求的完成事件、超时取消和载荷释放规则。
- `ProjectEditorOptions` 中的模型、主题和字体回调必须在编辑器生存期内有效；每个编辑器实例
  拥有自己的窗口和草稿。远程桥引用的 Agent、流发布器、会话桥也必须保持有效，退出时先停止后台服务。

## 构建与回归

在仓库根目录使用原生构建目录，明确关闭 Qt：

```powershell
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake -S desktop -B desktop/build -G 'Visual Studio 17 2022' -A x64 -DAICLI_USE_QT_UI=OFF
& $cmake --build desktop/build --config Debug --target aicli aicli_native_module_tests -- /m:2
& ./desktop/bin/Debug/aicli_native_module_tests.exe
```

`tests/native_module_tests.cpp` 使用临时数据库和自己的窗口，覆盖独立弹窗、嵌套项目草稿、
设置页切换、主题回调、项目取消，以及远程输入/按键/创建项目的完成与取消。
输入绘制、模态重绘、数据升级、终端进程关闭、休眠计时和更新检查仍有各自的回归测试。
