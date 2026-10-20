# 后台登录离线测试

测试仅使用临时目录里的合成凭据和 `fake-p4.exe`，不连接服务器，不修改用户的 P4V 配置或真实 ticket。模拟窗口测试使用真实 `PasswordWatcher` 代码，仅替换 options 解析器和终端/Preferences 安装函数。

在初始化了 MSVC x64 的命令行里执行：

```bat
cmake -S . -B build-login-tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%CD%\.sdk\6.8.3\msvc2022_64" -DBUILD_LOGIN_TESTS=ON
cmake --build build-login-tests
set "PATH=%CD%\.sdk\6.8.3\msvc2022_64\bin;%PATH%"
ctest --test-dir build-login-tests --output-on-failure
```

`background-login` 覆盖提示解析、ticket 状态解析、提前续期、到期续期、初始共享验证失败、路径未更新、错误密码预算、未知/网络错误、验证阶段网络恢复、超时、GUI 取消、短寿命 ticket 和作用域暂停/恢复。

`password-watcher` 使用模拟 `UIWorkspace2` 和密码框，验证 Save 原有 OK 回调、最小化时续期、窗口标题切换时暂停、不覆盖用户编辑、GUI 自动填写兜底和隐藏窗口时暂停。CTest 为该测试设置 `QT_QPA_PLATFORM=offscreen`。

不覆盖真实服务器触发器、MFA/SSO、P4V 私有连接生命周期/缓存、ticket 配置解析的所有变体或真实过期场景；不能据此声称真实 P4V 已完成后台重新登录验证。
