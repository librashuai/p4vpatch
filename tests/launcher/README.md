# Launcher 离线测试

覆盖稳定版本比较、GitHub Release/manifest 解析、URL 限制、大小/hash/x64 PE 检查、不执行 DLL 的版本读取、暂存与备份、锁定文件拒绝更新、更新失败保留旧文件，以及真实欢迎页的新版强制显示/手动更新状态机。

要求关闭所有 P4V；安装测试只在临时目录操作，不修改 P4V 安装目录。运行中保护测试仅创建并结束测试自己编译的同名假进程，不启动或结束真实 P4V。GUI 测试使用内存 transport，不连接 GitHub、不自动下载 DLL、不启动 P4V。脚本测试验证 ZIP/清单/hash 和草稿→正式发布顺序，Git/GitHub CLI 均为内存替身，不执行联网发布。

```powershell
.\build.ps1 -BuildDir build-launcher-check -LauncherTests
$perforce = 'C:\Apps\Perforce'
$env:PATH = "$perforce;$env:PATH"
$env:QT_PLUGIN_PATH = "$perforce\plugins"
$env:QT_QPA_PLATFORM_PLUGIN_PATH = "$perforce\plugins\platforms"
ctest --test-dir build-launcher-check --output-on-failure -R 'launcher-'
```

可用 Qt SDK 6.8.3 做额外开发测试，但部署验证必须使用 P4V 的 Qt 6.8.6 DLL、平台及 TLS 插件。测试程序不是正式 Launcher，可通过 PATH 找到运行库；正式 GUI Launcher 默认与 P4V 同目录部署；开发目录已复制所需 Qt 运行库/插件时，也可指定外部 P4V，启动前仍校验目标版本。回归测试覆盖外部 P4V 不改变本地补丁路径、目标 Qt 不匹配拒绝操作、子进程不继承开发目录插件路径，以及缺少 P4V 时仍能打开欢迎页。

主题测试使用临时 XML/QSS，验证浅色/深色与字体读取、实际皮肤加载、缺失/损坏设置回退，以及详情/空白更新区默认收起。布局回归通过延迟的内存请求验证进度条显隐不改变窗口及周边控件几何位置，并验证默认窗口和手动调整大小后展开/收起详情均保持顶部布局位置不变。可额外指定本机安装路径并生成 UI 预览（只读资源、不启动 P4V）：

```powershell
$env:P4VPATCH_TEST_P4V_PATH = "$perforce\p4v.exe"
$env:P4VPATCH_UI_SNAPSHOT_DIR = "$PWD\build-launcher-check\ui-preview"
ctest --test-dir build-launcher-check --output-on-failure -R launcher-welcome
```

发布前人工检查：

- 首次欢迎页正常，无控制台；界面可拖动，检查/下载/注入时不冻结。
- 勾选无新版时直接启动；新 Release 能强制展示欢迎页，未点击更新不下载二进制。
- 不兼容的 Launcher/P4V/Qt、缺失/损坏清单均显示说明，不安装。
- 离线、GitHub 限流、下载中断能继续使用旧版；损坏 DLL 不覆盖旧版。
- P4V 运行时拒绝更新，不结束进程；锁定/只读文件、不可写目录显示错误。
- 用户确认更新后版本正确、保留 `.bak`；更新成功不自动启动；手动启动可使用新版。
- 完整 ZIP 的手动安装和 `.bak` 回退；在真实 P4V 中验证登录及 Terminal。

这些测试不证明网络发布链路、权限组合或真实 P4V 私有接口的兼容性；不会真实创建 GitHub Release。
