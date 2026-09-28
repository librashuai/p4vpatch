# P4V 自动重新登录（实验性功能）

适用于 Windows x64 上的 **P4V 2026.2 / Qt 6.8.6**。启动器启动 P4V，通过 `LoadLibraryW` 加载 `p4vpatch.dll`；DLL 在 Qt 应用中安装事件过滤器。**不会修改** P4V 可执行文件或其自带的 DLL。

## 构建

需要：x64 MSVC 工具链，以及 **Qt 6.8.3 MSVC x64 开发 SDK**（头文件、CMake 配置和导入库）。P4V 附带的是 Qt 6.8.6；目前找到的可公开下载的 6.8 开发 SDK 版本是 6.8.3。本项目依赖 Qt 6.8 补丁版本之间的兼容性，仍需在真实登录弹窗中验证。`C:\Apps\Perforce` 下的 Qt DLL **只有运行时文件**，不能代替开发 SDK。不要使用 MinGW 版 Qt 编译注入 DLL。下载 SDK：

```bat
python -m pip install aqtinstall
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O .sdk --archives qtbase
```

本工作区中的 SDK 位于 `.sdk\6.8.3\msvc2022_64`。在 x64 Visual Studio Developer PowerShell 中运行（需安装 CMake 和 Ninja）：

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

## 功能行为

当 `Perforce Password Required` 弹窗包含预期的用户和服务器提示，并且能唯一确定 Qt 密码输入框时，补丁会在按钮区域（或 Qt 布局中）添加 **Save** 按钮。输入密码并点击 Save 后，补丁把当前密码保存在**本次 P4V 进程内存中**，然后点击 P4V 原有的 OK 按钮进行登录。下次出现对应弹窗时，补丁自动填写密码并点击原有的确认按钮。补丁不会将密码或 ticket 写入磁盘；P4V 自身的 ticket 处理方式不变。

密码以完整提示文本（包含用户和服务器）为键。每个保存项最多自动提交 **3 次**，跨多次弹窗累计；再次手动点击 Save 会重置计数。次数用尽后，保留 P4V 原有弹窗，交由用户处理。如果密码错误提示显示在同一个弹窗中，仅当明确出现 `Password not valid` 时才会继续重试，两次尝试之间至少间隔 1.5 秒。计数不会因“看起来已登录”而自动清零：目前没有可靠的 P4V 专属登录成功信号，此限制可防止错误密码引发无限重试。需要重新允许自动尝试时，请再次点击 Save。

## 限制与安全提示

- 这是**尚未经过真实密码过期场景验证的运行时原型**。投入日常使用前，必须在测试环境核对弹窗控件结构、提示文本及确认按钮行为。
- 注入代码运行在 P4V 进程内部，可能导致 P4V 崩溃。升级 P4V 或 Qt 后必须重新验证；DLL 会拒绝其他 Qt 运行时版本。不要注入无关进程。
- 密码会留在进程内存中直到进程退出；管理员或内存转储可以读取。Qt 内部可能复制字符串，因此擦除旧值也不能保证安全清除所有副本。如果组织策略禁止客户端在内存中保存密码，请勿使用。
- MFA、SSO、密码过期、账户锁定或网络故障可能需要人工处理。三次自动尝试也可能导致账户被锁定。如 P4V 的控件或文案不同，补丁会跳过该弹窗，而不会猜测操作。
- 启动器不会绕过 Windows 安全策略；禁止 DLL 注入的策略可能阻止其工作。请以相同的用户身份和权限级别运行启动器与 P4V。
