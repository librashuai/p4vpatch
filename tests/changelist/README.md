# Submitted Changelist 链接测试

只增强已核实的 `UIChangeWindow` → `Gui::ExpandableTextEdit`（`expanding QTE`）→ `QTextBrowser`（`markdownBrowser`），且窗口必须是 Submitted Changelist、内外控件均只读。测试使用模拟的同名 Qt 类，不连接 Perforce 或 Jira；HTTPS 由测试 handler 截获，不启动浏览器，也不修改系统剪贴板。

```powershell
.\build.ps1 -BuildDir build-launcher-check -LauncherTests -ChangelistTests
$env:PATH = "C:\Apps\Perforce;$PWD\.sdk\6.8.3\msvc2022_64\bin;$env:PATH"
$env:QT_PLUGIN_PATH = 'C:\Apps\Perforce\plugins'
$env:QT_QPA_PLATFORM_PLUGIN_PATH = 'C:\Apps\Perforce\plugins\platforms'
ctest --test-dir build-launcher-check --output-on-failure -R changelist-links
```

Qt Test 是额外的 SDK 测试依赖；生产 DLL 不依赖 Qt Test。优先使用 P4V 的 Qt 6.8.6 运行库，SDK bin 仅用于提供 Qt6Test.dll。测试平台为 offscreen。

覆盖：

- 已打开/新建窗口、重复安装、多 Jira 编号、中文/emoji、变长数字、错误前缀与非法后缀。
- 源 Description 和显示文档的纯文本/HTML、修改标记、undo、布局保持不变；初次应用不触发 textChanged。
- 手型、准确 URL、点击不在内嵌浏览器中导航；Shift 选择、拖选、空白区域不误打开。
- 内容刷新、整个 QTextDocument 更换、主题 Link 颜色、纵向/横向滚动命中测试。
- 已有 Markdown/HTML 链接、粗体和查找高亮不受影响；已有语法高亮器时保守跳过。
- Pending/Submit/其他窗口、不符合名称的控件、可编辑控件均跳过；模式切换时移除/恢复显示层格式。

实现使用 QSyntaxHighlighter 的布局格式，不写入 HTML anchor 或改变 Description 数据。显式 rehighlight 时阻止格式刷新信号并保留 modified 标记，点击由 viewport 事件过滤器处理。新增链接不进入复制出的 HTML，也不会新增 Qt 原生的“复制链接地址”菜单项；原有选择、复制和菜单继续由 P4V/Qt 处理。

真实 P4V 2026.2 / Qt 6.8.6 已在 Submitted Changelist 4684339 上进行功能独立加载检查：BR-339863 显示为链接颜色及下划线、悬停手型，源/显示文档、modified 标记及浏览器几何位置未变化，初次增强未产生 textChanged。浏览器打开行为由离线测试验证；没有为测试实际访问 Jira。模拟测试不替代真实 P4V 升级、复杂刷新与连接切换验证。
