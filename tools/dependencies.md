# 本地依赖配置

项目使用 `dependencies.json` 指定本机的依赖路径。它和个人磁盘规划文档
`tools/dependency-layout.md` 仅保存在本地，已加入 Git 忽略规则。
仓库中的 `dependencies.example.json` 提供依赖版本和配置结构，不包含任何机器的安装路径。

首次构建前，在仓库根目录执行：

```powershell
Copy-Item dependencies.example.json dependencies.json
```

按实际环境填写 `roots` 中的绝对路径。`packages` 中的路径相对于对应的根目录，
可根据实际安装结构调整；同一版本的依赖可以由多个项目共用。
CMake 和 PowerShell 构建脚本读取同一份本地配置。

以下环境变量可覆盖配置中的根目录：

| 环境变量 | 配置项 |
| --- | --- |
| `AICLI_ANDROID_ROOT` | `roots.android` |
| `AICLI_NATIVE_ROOT` | `roots.native` |
| `AICLI_TOOLS_ROOT` | `roots.tools` |
| `AICLI_QT_ROOT` | `roots.qt` |

`build` 是可选的本地设置：

- `cacheRoot`：PowerShell 构建工具的可写缓存目录，支持 `%LOCALAPPDATA%` 等环境变量。
  `AICLI_BUILD_CACHE_ROOT` 优先于此设置；未指定时使用用户本地应用数据目录下的 `BuildCache`。
- `readOnlyDrives`：本机仅存放依赖的盘符列表；构建工具拒绝将缓存或产物写入这些盘。
  模板默认为空，不对其他电脑施加磁盘规划。
- `includeDirectories`：需要额外指定的 C++ 头文件目录。通常由 Visual Studio 的 Windows SDK
  配置提供，仅在本机环境确实需要时填写。

请将 `cmake.exe` 加入 PATH，或将文档中的 `$cmake` 设置为实际可执行文件路径。
编译缓存、运行时暂存和本地配置均不应提交到 Git。
