# 终端模拟主窗口测试

`mock-host.exe` 默认验证原生标签、ConPTY、Qt 焦点信号过滤、关闭重开及主窗口无闪烁；设置 `P4VPATCH_DISABLE_TERMINAL=1` 则验证显式停用。**模拟宿主无法模拟 P4V 私有焦点回调；模拟通过不能证明真实 P4V 安全。** 本版本还在真实 P4V 中测试了输入光标、反复切换与关闭，以及正常退出。

在项目根目录、x64 Visual Studio Developer PowerShell 中构建（需 Qt 6.8.3 MSVC x64 SDK；见根目录 README）：

```powershell
cmake -S tests/terminal-host -B build-terminal-host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/.sdk/6.8.3/msvc2022_64"
cmake --build build-terminal-host
```

用 P4V 自带运行库测试（安装路径可调整，Qt DLL、插件、WebEngine 子进程及资源须来自同一安装）：

```powershell
$perforce = 'C:\Apps\Perforce'
$env:PATH = "$perforce;$env:PATH"
$env:QT_PLUGIN_PATH = "$perforce\plugins"
$env:QTWEBENGINEPROCESS_PATH = "$perforce\QtWebEngineProcess.exe"
$env:QTWEBENGINE_RESOURCES_PATH = "$perforce\P4VResources\resources"
$env:QTWEBENGINE_LOCALES_PATH = "$perforce\translations\qtwebengine_locales"
.\build-terminal-host\mock-host.exe
```

应输出 `PASS`。测试要求 Windows 10 1809+；无需 Node.js/npm。
