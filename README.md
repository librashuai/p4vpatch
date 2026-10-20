# P4V 增强补丁（实验性）

功能：自动重新登录，以及 **View → Terminal** 打开 Log/Dashboard 同组的原生终端标签。模拟主窗口测试见 [`tests/terminal-host/README.md`](tests/terminal-host/README.md)。

适用于 Windows x64 上的 **P4V 2026.2 / Qt 6.8.6**。启动器启动 P4V，通过 `LoadLibraryW` 加载 `p4vpatch.dll`；DLL 在 Qt 应用中安装事件过滤器。**不会修改** P4V 可执行文件或其自带的 DLL。

## 构建

需要：x64 MSVC 工具链，以及 **Qt 6.8.3 MSVC x64 开发 SDK**（含 Qt Base、WebChannel、WebEngine、Positioning）。P4V 附带的是 Qt 6.8.6；本项目依赖 Qt 6.8 补丁版本之间的兼容性，仍需在真实登录弹窗中验证。`C:\Apps\Perforce` 下的 Qt DLL **只有运行时文件**，不能代替开发 SDK。不要使用 MinGW 版 Qt 编译注入 DLL。下载 SDK：

```bat
python -m pip install aqtinstall
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O .sdk --modules qtwebchannel qtwebengine qtpositioning
```

本工作区中的 SDK 位于 `.sdk\6.8.3\msvc2022_64`。安装 Visual Studio 的 C++ 桌面开发工具后，可直接在普通 PowerShell 中运行；脚本会自动初始化 x64 MSVC 环境，并在 PATH 中缺少 CMake 或 Ninja 时使用 Visual Studio 附带的版本：

```powershell
.\build.ps1
# SDK 位于其他位置时：.\build.ps1 -QtSdk 'D:\Qt\6.8.3\msvc2022_64'
```

生成文件：`build-msvc\p4vpatch.dll` 和 `build-msvc\p4vpatch-launcher.exe`。要双击启动，请将**这两个文件**复制到 `p4v.exe` 所在目录（例如 `C:\Apps\Perforce`），然后运行 `p4vpatch-launcher.exe`。不传参数时，启动器会运行其所在目录下的 `p4v.exe`。也可以将两个补丁文件放在一起，显式指定 P4V 路径：

```powershell
& '.\build-msvc\p4vpatch-launcher.exe' 'C:\Apps\Perforce\p4v.exe'
```

显式指定可执行文件路径后，还可以继续传入 P4V 的命令行参数。启动前请退出所有已运行的 P4V：如果新进程将请求转交给现有实例，现有实例不会被注入补丁。

启动器图标为 `assets/p4vpatch-launcher.ico`，由本机 P4V 可执行文件提取的图标（`assets/p4v-extracted.ico`）加上“+”标记生成。重新生成：先运行 `python -m pip install pefile pillow`，再在构建前运行 `python tools/make-icon.py 'C:\Apps\Perforce\p4v.exe'`。已在非 P4V 进程中使用 P4V 自带的 Qt 6.8.6 运行库成功加载 DLL；这只能验证导入依赖，**不能证明登录弹窗功能正常**。如果只需要编译启动器，可不安装 Qt SDK，使用 `cmake -S . -B build -DBUILD_PATCH=OFF`。

## 终端面板

启动后选择 **View → Terminal**，或点击 Log/Dashboard 旁的 Terminal 标签。此标签是真正加入 P4V `UILogTabWidget` 的第三页，不是另一个 dock 或绘制的假标签。WebEngine/xterm.js 显示 ConPTY 的终端（默认为 `cmd.exe`）；打开时会聚焦输入，关闭标签的 × 只隐藏页面，重开继续使用同一进程。新建终端会以当前 P4V workspace 的本地根目录为默认工作目录；未选择 workspace 或根目录不可用时回退到用户主目录。已打开的终端保持原工作目录，切换 workspace 后需重新启动 P4V 才能创建新会话。若只需登录补丁，可在启动前设置 `P4VPATCH_DISABLE_TERMINAL=1`。

**Patch 设置：** 打开 **Edit → Preferences → Patch**，在 **Terminal Shell** 中选择 `cmd.exe`（默认）、`pwsh`（从 PATH 查找），或使用 Browse 指定已有的 `pwsh.exe` 绝对路径。点击 P4V 原有的 **Apply/OK** 保存，**Cancel** 不保存。配置独立保存在 P4V 的 `ApplicationSettings.xml` 所在目录下的 `~/.p4qt/p4vpatch.ini`，不改写 P4V 的设置文件。已经打开的终端继续使用原有 shell，重新启动 P4V 后生效。

**兼容性处理：** P4V 2026.2 私有的焦点回调不能处理外来标签页；直接调用 Qt `addTab` 会导致崩溃。补丁只在安装的 Qt **6.8.6** 中校验并替换 Qt6Widgets 对 `QMetaObject::activate` 的一项导入，使 Terminal 页面存在期间不触发 `QApplication::focusChanged`；其余应用信号保持不变。导入签名/版本不匹配时**不显示终端入口**，而不是冒险继续。退出 P4V 前会先分离外来页面，避免宿主析构时崩溃。已在真实 P4V 中重复测试：实际屏幕可见终端提示符和输入光标、输入执行、Log/Dashboard/Terminal 切换、关闭重开、空闲等待、File → Exit 和 Connection → Close Connection；本轮无新崩溃转储，也没有操作期间主窗口 Hide/Show。模拟测试不替代这些真实测试。

**仍是针对特定 P4V/Qt 版本的非官方运行时补丁**，修改了 Qt DLL 在当前进程中的导入项。Terminal 页面存在期间，P4V 其他依赖应用级 `focusChanged` 信号的功能也可能受影响；P4V 升级、复杂布局切换及其他未覆盖的关闭途径仍可能不兼容。切勿在未保存的工作上首次试用。默认 DLL 为 `build-msvc\p4vpatch.dll`。

## 自动重新登录行为

当 `Perforce Password Required` 弹窗包含预期的用户和服务器提示，并且能唯一确定 Qt 密码输入框时，补丁会在按钮区域（或 Qt 布局中）添加 **Save** 按钮。输入密码并点击 Save 后，补丁把当前密码保存在**本次 P4V 进程内存中**，然后点击 P4V 原有的 OK 按钮进行登录。下次出现对应弹窗时，补丁自动填写密码并点击原有的确认按钮。补丁不会将密码写入磁盘。弹窗登录仍由 P4V 处理；后台续期会调用官方 `p4.exe login` 更新共享的 ticket 文件（见下文）。

密码以完整提示文本（包含用户和服务器）为键。每轮首次自动提交失败后，最多再重试 **3 次**（合计最多 4 次），两次自动提交之间至少间隔 **10 秒**；再次手动点击 Save 会重置计数。弹窗明确显示 `Password not valid` 或 P4V 弹出新的密码框时才重试；结果不明时不会盲目连续提交。P4V 接受并关闭弹窗、且之后 10 秒内没有再次弹出密码框时，清零本轮计数；下次 ticket 到期可重新自动提交。若耗尽次数或无法确认结果，保留原有弹窗供用户处理。弹窗接受并关闭只是登录成功的近似信号，并非服务器 ticket 验证；仍须在真实登录场景验证 P4V 的行为。

### 后台 ticket 检测与续期

手动点击 Save、且 P4V 接受登录后，补丁尝试为该连接启动一个 **30 秒**周期的 `QTimer`。P4V 在后台或最小化时仍可运行，无需切到前台，也不会强制激活窗口。通过密码框的完整提示严格解析服务器和用户名；不使用默认 `P4PORT/P4USER` 或最近连接猜测身份。

- 只调用 **P4V 所在目录**的 `p4.exe`，异步执行 `login -s`。ticket 剩余时间不超过 **60 秒**或明确过期时才尝试 `login`，随后再次 `login -s` 验证；短寿命 ticket 不会在每次检查时不断提前续期。
- ticket 路径按进程 `P4TICKETS`、Windows Perforce 用户/系统注册表设置、`~/p4tickets.txt` 的顺序解析，只接受绝对路径。首次手动 Save 前记录文件元数据，登录接受后要求文件被创建/更新，且 CLI 能验证该用户的有效 ticket，才允许自动提交密码。**这是共享路径的保守推断，不是 P4V 私有 ticket 缓存的直接确认**；文件没有更新、格式不识别或设置不明确时保留弹窗兜底。特殊 `P4CONFIG/P4ENVIRO` 配置应在启动前明确设置正确的 `P4TICKETS`。
- 从 P4V 的 `ApplicationSettings.xml` 读取 `UseIpSpecificTickets`，匹配其 host-specific / all-host 登录策略。后台暂只支持可打印 ASCII 用户名、服务器地址和密码；非 ASCII 凭据仍可用弹窗自动填写，避免猜测连接字符集。
- 密码仅通过子进程标准输入传递，不进入命令行、日志或临时文件。不输出 ticket，不自动确认 SSL 信任，不执行客户端 SSO/Extensions 脚本。网络故障及未知状态只退避检查，不视为密码过期；命令 **15 秒**超时，退避最长 **5 分钟**。
- 后台和弹窗**共用上述最多 4 次的提交预算与 10 秒最小间隔**。后台只有在续期后的 ticket 检查成功时才清零；手动 Save 也可重置。新密码弹窗优先取消正在运行的后台命令，避免并发提交和覆盖用户编辑。
- 监控绑定到 Save 弹窗所属的 `UIWorkspace2` 主窗口：窗口销毁、隐藏、标题变化或有对应密码弹窗时暂停；ticket 文件被删除时停用。无法找到可验证的宿主窗口时不启用后台续期。**此窗口/标题判断不是完整的 P4V 连接生命周期 API**；显式注销前建议禁用自动登录或退出补丁实例。

禁用后台续期但保留弹窗自动填写：启动前设置 `P4VPATCH_DISABLE_BACKGROUND_LOGIN=1`。诊断：设置 `P4VPATCH_LOGIN_DIAGNOSTICS=1` 后启动，状态日志写入 `%TEMP%/p4vpatch-login.txt`，不包含密码、ticket 或原始命令输出。新版本首次使用需在真实密码弹窗再次点击 Save；旧进程不会自动获得新代码或内存凭据。

离线测试见 [`tests/login/README.md`](tests/login/README.md)。已覆盖异步检查/续期、明确过期、错误密码上限、网络故障、超时取消、短寿命 ticket、验证阶段网络恢复、路径未更新，以及模拟主窗口最小化/切换/弹窗兜底。**这些测试不替代真实 P4V ticket 过期、磁盘 ticket 重载、MFA/SSO 及自定义连接配置验证。**

## 限制与安全提示

- 这是**尚未经过真实密码过期场景验证的运行时原型**。投入日常使用前，必须在测试环境核对弹窗控件结构、提示文本及确认按钮行为。
- 注入代码运行在 P4V 进程内部，可能导致 P4V 崩溃。升级 P4V 或 Qt 后必须重新验证；DLL 会拒绝其他 Qt 运行时版本。不要注入无关进程。
- 终端可以执行**当前用户权限下的任意命令**，仅加载 DLL 内嵌本地页面；不要用未知来源的 DLL。退出 P4V 会终止对应的 shell 进程，但它自行启动的其他进程不保证一并退出。WebEngine/ConPTY 仍有依赖系统、权限、编码和终端程序的兼容风险；它并不保证与 Windows Terminal 功能完全相同。
- 密码会留在进程内存中直到进程退出；管理员或内存转储可以读取。Qt 内部可能复制字符串，因此擦除旧值也不能保证安全清除所有副本。如果组织策略禁止客户端在内存中保存密码，请勿使用。
- MFA、SSO、密码过期、账户锁定或网络故障可能需要人工处理。三次自动尝试也可能导致账户被锁定。如 P4V 的控件或文案不同，补丁会跳过该弹窗，而不会猜测操作。
- 启动器不会绕过 Windows 安全策略；禁止 DLL 注入的策略可能阻止其工作。请以相同的用户身份和权限级别运行启动器与 P4V。
