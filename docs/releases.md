# GitHub Release 发布与更新手册

## 用户：安装和更新

要求 Windows x64、**P4V 2026.2 / Qt 6.8.6**。这是非官方运行时补丁，首次使用不要保留未保存的工作。

1. 在 [GitHub Releases](https://github.com/librashuai/p4vpatch/releases/latest) 下载 `p4vpatch-v版本-windows-x64.zip`，不要下载 GitHub 自动生成的 Source code 包。
2. 退出所有 P4V 和旧 Launcher，将 ZIP 中 `p4vpatch-launcher.exe`、`p4vpatch.dll` 一起解压到 `p4v.exe` 所在目录。不覆盖 P4V 的任何 Qt 文件，不需要复制 Qt SDK。
3. 双击 `p4vpatch-launcher.exe`。首次显示欢迎页，检查 GitHub 最新稳定 Release；默认使用同目录 P4V，否则使用已保存的路径，也可点击“选择 P4V…”；确认后点击“启动 P4V”。
4. 可勾选“无更新时直接启动”。以后仍会先检查更新；**只要发现比本机补丁或 Launcher 更新的版本，就显示欢迎页，不直接启动，更不静默安装**。
5. 欢迎页展示 Release 说明。点击“更新补丁”并确认后才下载 DLL，检查大小、SHA-256、x64 PE、版本资源和兼容性，然后替换。也可忽略更新，手动启动原版本。
6. 更新要求关闭所有 P4V；不会强制终止进程。下载过程中也不要自行启动 P4V。成功后原 DLL 保留为 `p4vpatch.dll.bak`，需要再点击“启动 P4V”使用新版。

默认仓库：`librashuai/p4vpatch`；只检查 `/releases/latest` 的稳定版 `vMAJOR.MINOR.PATCH`，不使用 draft/prerelease，也不自动降级。

### 界面与主题

启动及重新选择 P4V 时，Launcher 只读 `~/.p4qt/ApplicationSettings.xml` 的 `DarkTheme` 与 `Fonts/AppFont`，跟随 P4V 的浅色/深色主题和界面字体。优先加载所选 P4V 安装中的 `P4VResources/skins/Default.qss` 或 `DarkTheme.qss` 及配套图片资源；开发目录找不到皮肤时使用相近的内置配色。不会修改 P4V 的设置或主题文件，也不会在 Release 包中重新分发这些资源。

默认只显示版本、简短状态和启动入口；有新版才展开更新说明及相关操作。完整 P4V/补丁路径、兼容版本、网络错误原文可点击“详情”查看。新版提醒和手动确认更新的规则不变。

### Launcher 自身更新

程序只在线替换 **补丁 DLL**，不替换运行中的 EXE。完整 ZIP 始终包含配套的 Launcher。

- 更新清单要求更高的 Launcher 版本时，“更新补丁”禁用；点击“打开 Release”，下载完整 ZIP，退出 Launcher 和 P4V 后手动替换两个文件。
- DLL 已更新但 Launcher 仍落后于 Release 版本时，下次依然会提示；更新 Launcher 后恢复“无新版本时直接启动”。
- 新 Release 没有清单、清单损坏或无法下载时，也会显示新版提醒，并提供 Release 页面入口。

### 离线、权限和恢复

- 网络超时、GitHub API 限流、代理或 TLS 错误不阻止使用已安装的补丁；“检查更新”可重试。已发现的新版会缓存在配置中，下次离线仍显示欢迎页提醒；离线无法得知服务器上尚未发现的新版。
- 检查和下载不需要 GitHub token；当前仅支持公开仓库。默认不发送遥测，但 GitHub 会收到正常 API/资源请求及 User-Agent。
- 程序不自动提权。如果安装目录不可写，更新失败并保留原版本；可由管理员手动部署两个文件，或按组织规定授予目录写权限。不建议长期用管理员权限运行 P4V/终端。
- 回退：退出 P4V 和 Launcher，把 `p4vpatch.dll.bak` 复制为 `p4vpatch.dll`；若同时升级过 Launcher，使用上一版 ZIP 恢复配套两个文件。
- 每个安装目录同时只允许一个 Launcher，但允许已有 P4V/P4VC 运行时再次启动 P4V；补丁只注入本次新建的进程，不改动已有实例。仅更新 DLL 时阻止任何 `p4v.exe` 进程存在，防止替换正在使用的补丁。检查、启动或更新出错时，主页面及详情中的错误文字显示为红色。
- Launcher 依赖自身目录中的 P4V Qt DLL，以及 `plugins/platforms/qwindows.dll` 和 TLS 插件。Qt 缺失时可能在程序入口前由 Windows 报错；仅复制 EXE/DLL 到任意目录仍然不够。开发测试可另行复制完整所需运行库/插件，再选择外部 `p4v.exe`。补丁始终读取、注入和更新 Launcher 旁边的 DLL，不使用外部 P4V 目录中的同名补丁。启动前校验目标 P4V 版本及其 Core/Gui/Widgets Qt 版本；子进程使用目标 P4V 安装中的插件。找不到 P4V 时仍可打开欢迎页，点击“选择 P4V…”指定路径。P4V/Qt 升级后必须重新验证兼容性。
- 启动完成只代表 DLL 被加载，不代表所有补丁功能初始化成功。仍须在真实 P4V 中验证登录和终端功能。

强制显示欢迎页、传递 P4V 参数：

```powershell
.\p4vpatch-launcher.exe --welcome
.\p4vpatch-launcher.exe -- -p server:1666 -u user
# 可指定外部 p4v.exe，再跟 P4V 参数（Launcher 目录须具备匹配的 Qt 运行时）：
.\build-msvc\p4vpatch-launcher.exe --welcome 'C:\Apps\Perforce\p4v.exe' -p server:1666
```

配置位于当前用户的 `QStandardPaths::AppConfigLocation` 下的 `launcher.ini`（包含组织名 `p4vpatch` 和应用名 `p4vpatch-launcher`，实际路径由 Qt 决定），只保存跳过欢迎页选项、所选 P4V 路径及最新 Release 元数据缓存，不保存凭据。使用 `--welcome` 无需删除配置。

## 维护者：发布准备

环境：Windows x64、Visual Studio C++ 工具链、CMake/Ninja、Qt 6.8.3 MSVC x64 SDK、PowerShell 5.1 或更新版本。发布还需要 Git、[GitHub CLI](https://cli.github.com/)、仓库写权限和 `gh auth login`。

版本唯一来源为仓库根目录 `VERSION`，例如 `0.1.0`。它会生成 EXE/DLL 的 Windows 版本资源和 Launcher 版本。仅接受三段数字，每段 0..65535；不使用前导零、版本后缀或四段版本。

1. 修改 `VERSION`，编写发布说明，例如 `docs/release-notes/0.1.0.md`。说明新功能、风险、支持的 P4V/Qt 版本和回退方法。
2. 构建并测试：

   ```powershell
   .\build.ps1 -BuildDir build-release -LauncherTests
   # 为测试设置 P4V 的 Qt 运行时，具体命令见 tests/launcher/README.md
   ```

3. 在真实 P4V 中验证欢迎页、联网/离线、手动更新、运行中拒绝替换、登录与终端。不把离线单元测试当作真实 P4V 兼容性验证。
4. 提交所有源码、版本和说明变更，并 push 到目标 GitHub 仓库。发布脚本要求干净工作区，且当前 commit 已存在于目标仓库。

### 只生成发布包（不联网发布）

```powershell
.\tools\package-release.ps1
# 可指定 SDK、构建目录、fork 仓库：
.\tools\package-release.ps1 -QtSdk D:\Qt\6.8.3\msvc2022_64 `
    -BuildDir build-release -Repository librashuai/p4vpatch
# 复用已构建的当前版本（会检查版本资源和构建元数据）：
.\tools\package-release.ps1 -BuildDir build-release -SkipBuild
```

输出为 `dist/v版本/`：

| 文件 | 用途 |
| --- | --- |
| `p4vpatch-v版本-windows-x64.zip` | 完整用户包：Launcher、补丁、README、发布手册 |
| `p4vpatch.dll` | Launcher 在线更新使用的独立 DLL |
| `p4vpatch-update.json` | 更新协议、版本、兼容性、DLL 大小和 SHA-256 |
| `SHA256SUMS.txt` | ZIP、清单、DLL 的校验值 |

不包含 Qt/P4V 运行时，不重新分发 Perforce 文件。脚本拒绝覆盖现有输出目录，重新打包前须明确删除对应的 `dist/v版本`。不要改完二进制后复用旧清单。

### 创建 GitHub Release

```powershell
# 默认创建草稿，不对用户可见：
.\tools\publish-release.ps1 -NotesFile docs\release-notes\0.1.0.md

# 或：上传完全部资源后立即发布，并标记为 latest：
.\tools\publish-release.ps1 -NotesFile docs\release-notes\0.1.0.md -Publish
```

脚本将：校验工作区与远端 commit → 构建/打包 → 创建 `vVERSION` 草稿并上传四个资源 → 仅在显式传入 `-Publish` 时公开。可传 `-QtSdk`、`-BuildDir`、`-OutputRoot`、`-Repository`、`-SkipBuild`。

- 默认 `-MinimumLauncherVersion 0.1.0`。若更改更新协议或补丁需要新版 Launcher，发布时传入真正的最低版本，例如 `-MinimumLauncherVersion 0.2.0`。不能高于当前 Release 版本。不要无故每版都提高最低版本。
- **四个资源必须在同一 Release 下，名称不可更改**。标签必须为 `vVERSION`；公开后将其设为最新稳定版，不勾选 prerelease。
- 默认草稿路径：在 GitHub 核对资源和说明后手动发布并标记 latest。草稿可能已占用标签；上传失败后先检查 GitHub，修复/删除未发布草稿后再重试，禁止公开资源不完整的 Release。
- 现有远端标签会被脚本拒绝，不静默覆盖已发布资源。修复已公开版本应递增 `VERSION`；不可原地更换 DLL。
- fork 必须在构建、打包、发布时使用一致的 `-Repository owner/repo`，否则 Launcher 仍会从原仓库检查更新。仅支持公开仓库。
- 发布脚本不会自动替你提交、push 本地分支或操作本机 P4V 安装目录。

### 更新协议（schema 1）

```json
{
  "schema": 1,
  "version": "0.1.0",
  "minimum_launcher_version": "0.1.0",
  "p4v_version": "2026.2",
  "qt_version": "6.8.6",
  "dll": {
    "url": "https://github.com/librashuai/p4vpatch/releases/download/v0.1.0/p4vpatch.dll",
    "size": 123456,
    "sha256": "实际文件的64位小写十六进制SHA256"
  }
}
```

清单由脚本自动生成，示例 size/hash 不是可部署数据。DLL 与清单 URL 必须属于编译时指定仓库的对应版本；HTTPS 重定向用于 GitHub CDN，不接受降级到 HTTP。

兼容性由 `src/version.h.in` 中的 P4V/Qt 常量生成到清单；改变支持范围需要同时审查注入模块的版本检查与私有 Qt hook，不能只修改清单放行。

### 安全边界

HTTPS、仓库/版本 URL 限制、大小限制、SHA-256、PE/版本资源检查能防止损坏和误装，**不等价于发布者数字签名**：有权修改 GitHub Release 的人也能修改 DLL 和清单。当前信任仓库维护者、GitHub 账户权限和 TLS；尚未实现签名清单/Authenticode 强制验证。保护维护者账户及发布权限；更高安全要求的组织应独立审核/签名发布文件，或手动部署。
