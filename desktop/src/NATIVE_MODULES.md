# 原生 C++ / Win32 模块说明

默认桌面开发入口是原生版本。以下模块不依赖 Qt 控制器；Qt 和 Android 有各自的入口。

## 模块职责

| 模块 | 负责什么 | 后续扩展入口 |
| --- | --- | --- |
| `app/main.cpp` | 主窗口生命周期、模型加载、项目和终端操作的协调 | 主窗口级行为、把新功能接入已有模块 |
| `app/native_remote_bridge.h/.cpp` | Agent 回调注册、跨线程消息、请求完成与取消、消息载荷释放 | 新远程操作的消息派发及 `UiCallbacks` |
| `ui/project_editor_dialog.h/.cpp` | 新建/编辑项目表单、草稿、校验、下拉菜单、主题与字体刷新 | 新项目字段及表单交互 |
| `features/project_configuration.h/.cpp` | 项目类型的默认启动命令选择 | 默认命令规则；供表单和会话启动共用 |
| `ui/project_list_view.h/.cpp`、`ui/internal/project_list_drag.inl` | 项目列表、像素滚动、拖动捕获、让位与落位动画 | 列表交互、拖动反馈；通过回调连接模型 |
| `features/project_order.h`、`ui/internal/project_drag_section.inl` | 组内排序计划、隐藏项目保留、排序保存与视图更新 | 排序规则；保存完整分组，保留当前选择和滚动位置 |
| `features/project_group_views.h/.cpp` | 已启动独立排序、最近/常用系统视图、使用记录持久化 | 系统视图筛选、自动排名和旧数据初始化；不写入用户分组 |
| `terminal/terminal_context_menu.h/.cpp`、`terminal/codex_composer_state.h` | CLI 右键菜单、Codex 输入区识别、清空输入和打开链接的入口 | 新终端菜单操作；由 `xaml_island_adapter` 调用终端编辑和链接接口 |
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
$cmake = 'cmake'
& $cmake -S desktop -B desktop/build -G 'Visual Studio 17 2022' -A x64 -DAICLI_USE_QT_UI=OFF
& $cmake --build desktop/build --config Debug --target aicli aicli_native_module_tests aicli_project_reorder_tests aicli_project_group_views_tests -- /m:2
& ./desktop/bin/Debug/aicli_native_module_tests.exe
& ./desktop/bin/Debug/aicli_project_reorder_tests.exe
& ./desktop/bin/Debug/aicli_project_group_views_tests.exe
```

`tests/native_module_tests.cpp` 使用临时数据库和自己的窗口，覆盖独立弹窗、嵌套项目草稿、
设置页切换、主题回调、项目取消，以及远程输入/按键/创建项目的完成与取消。
输入绘制、模态重绘、数据升级、终端进程关闭、休眠计时和更新检查仍有各自的回归测试。

`tests/project_reorder_tests.cpp` 用独立原生窗口及临时数据库验证拖动阈值、让位绘制、
取消、边缘像素滚动、当前项目和视口保留，以及“已启动”视图中隐藏项目的排序持久化。

`tests/project_group_views_tests.cpp` 使用临时数据库验证已启动项目跨组排序、原分组顺序保留、
最近/常用排名、使用去重、重启持久化、写入失败回滚和已有启动记录的初始化。

`aicli_terminal_context_menu_tests` 构建到独立的 `build/terminal-menu-tests/Debug` 目录。
运行 `desktop/tools/test-terminal-context-menu.ps1`，使用自己的临时包和 PowerShell 会话验证
真实鼠标右键和菜单点击、中文多行选区复制、粘贴、全选、复制全部、取消、键盘菜单、
深层 XAML 窗口的右键路由及项目切换。
链接回归覆盖普通网址、OSC 8 显示文字链接、原始查询参数和锚点、普通文本及项目切换。
自动回归会短暂显示自己的测试窗口，完成菜单操作后恢复鼠标位置和之前的前台窗口。
测试结束撤销本次注册的测试包；加 `-Interactive` 可手动检查菜单，关闭测试窗口后完成清理。

加 `-Codex` 验证真实 Codex 的中文、多行、光标在中间、80 行草稿、6000 字符文本和重复清空。
测试使用临时目录里的新会话，以临时外部编辑器读取草稿作为独立校验；换行先验证 Shift+Enter 编辑路径。
加 `-Interactive -Codex` 打开同一终端实现的手动体验窗口，使用新的测试会话，不恢复已有会话。
加 `-Headless` 可在锁屏时验证菜单可用性、真实终端的链接和 Codex 草稿，不使用系统鼠标、剪贴板或抢占前台；这不替代真实点击及复制/粘贴验证。
