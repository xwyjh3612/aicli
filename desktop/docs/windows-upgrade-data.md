# Windows 原生版升级与数据

1.0.0.6 修复了已安装的旧版（包括 1.0.0.4、1.0.0.5）在部分电脑上的升级问题。

## 原因

Windows 会将打包桌面应用新建的 AppData 文件重定向到应用包的私有目录。开发电脑如果已经有真实目录下的数据库，通常不会触发相同的重定向。这解释了为何开发电脑升级正常，而干净安装的电脑会出现问题。

旧安装程序先执行 `Remove-AppxPackage`，可能清除私有目录中的数据库。旧更新器还把重定向前的下载路径交给安装程序，导致应用外的进程报告找不到路径。

## 新版行为

- 在原 Windows 用户身份下备份并原位注册应用包，不先卸载旧包。
- 安装时用 SQLite 在线备份 API 保存数据库，包括 WAL 中已提交的数据；同时备份配对证书和上传附件。失败会停止注册。
- 仅原生 C++ 版禁用 AppData 文件写入虚拟化；首次启动在打开数据库前迁移旧私有目录。保留两个来源的备份，按 Windows 原有规则让私有目录优先。
- 迁移成功后记录标记，避免后续启动用陈旧数据覆盖用户的新数据。迁移失败时停止启动，不创建空数据库。
- 更新器校验下载大小及 SHA-256 后，通过文件句柄获取实际路径，再打开安装程序。

正式版数据目录：`%LOCALAPPDATA%\AI CLI`。

旧版私有目录：`%LOCALAPPDATA%\Packages\AiCli.Desktop.Release_jw3zr00rmb0ym\LocalCache\Local\AI CLI`。

升级备份：`%LOCALAPPDATA%\AI CLI\backups\upgrade-{随机标识}\stable` 或 `package`。备份包含敏感数据和手机配对私钥，仅保存在当前电脑，不进入仓库或发布包。

## 已经遇到问题的电脑

旧版更新器自身无法运行修复程序时，需要从 GitHub Release **手动下载 1.0.0.6 安装包，直接覆盖安装一次**。不要先卸载软件。之后的自动升级使用修复后的流程。

如果旧私有目录仍存在，新版首次启动会迁移其中的数据。如果之前的卸载已删除唯一的数据副本，新版不能凭空恢复。请先保留上述两个目录及已有备份，在关闭软件后检查；不要把另一台电脑的数据库直接覆盖过来。

确认备份内容后才手动恢复；数据库、手机配对证书 `agent-server.pem` 和上传附件应配套保留。备份数据库是完整快照，不要与其他数据库的 `aicli.db-wal` / `aicli.db-shm` 混用。

## 验证

`aicli_data_upgrade_tests` 覆盖实时 WAL 备份、两个数据来源冲突时的保留、迁移幂等、损坏数据库失败处理、全新安装，以及含空格和中文的安装包路径。

`desktop/tools/test-data-upgrade-package.ps1` 使用独立测试包复现 AppData 重定向，验证应用外进程能启动真实下载文件，以及改变安装目录后的原位升级与数据迁移。它不注册或启动正式版应用。

参考：[Microsoft 的桌面应用文件系统重定向说明](https://learn.microsoft.com/en-us/windows/msix/desktop/desktop-to-uwp-behind-the-scenes)、[文件写入虚拟化设置](https://learn.microsoft.com/en-us/uwp/schemas/appxpackage/uapmanifestschema/element-desktop6-filesystemwritevirtualization)。
