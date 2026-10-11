# 键客 KeySentry

一款掌控你的键盘和窗口的轻量 Windows 工具，纯 C++17 / Win32 API 实现，无任何第三方依赖。

![License](https://img.shields.io/badge/license-MIT-green)
![Platform](https://img.shields.io/badge/platform-Windows-blue)
![Language](https://img.shields.io/badge/C%2B%2B-17-orange)

## 功能特性

### 🔒 锁定键状态提示
按下 Caps Lock / Num Lock / Scroll Lock 时，在屏幕底部显示当前状态浮层，1 秒后自动消失。

### ⌨️ 按键禁用
阻止特定按键的输入，实现按键屏蔽，避免误操作。

### 🔄 按键映射
将某个按键重新定义为另一个按键，自定义键盘布局。

### ⚡ 热键管理
- **自定义热键**：添加热键执行命令/程序，或通过热键模拟任意按键（提供虚拟键盘选择界面）
- **启用/停用**：一键启用或停用全部自定义热键

### 🕵️ 窗口隐藏（老板键）
- 快捷键一键隐藏/恢复指定窗口，保护隐私
- 鼠标中键、侧键、屏幕四角触发（带防抖）
- 闲置自动隐藏（1-120 分钟可调）
- 隐藏时自动静音、暂停媒体播放
- 支持窗口标题匹配 / 进程名匹配两种绑定方式
- **异常自愈**：程序崩溃后重启可自动恢复之前隐藏的窗口

### 📌 窗口置顶
- 对指定窗口/进程保持置顶，免疫 Win+D 等操作
- 持续监控：每 3 秒自动检测，锁定被意外取消置顶的窗口
- 一键修复窗口堆叠异常

### 🗂️ 启动时设置
启动时自动重置资源管理器文件夹选项（显示/隐藏文件、扩展名、系统文件）。

## 下载使用

前往 [Releases](../../releases) 页面下载最新版本（当前 **v1.30.0.1011**），解压后直接运行：

| 文件 | 适用系统 |
|------|---------|
| `KeySentry.exe`（32 位，380KB）**推荐** | Windows 7 SP1 及更高（32/64 位通用） |
| `KeySentry-x64.exe`（64 位，476KB） | Windows 10/11（64 位） |

两版本功能完全一致，均无需安装任何运行库。不确定选哪个？直接下载 `KeySentry.exe` 即可。

## 验证下载文件与安全说明

本工具的 exe 若**尚未进行代码签名**，从浏览器下载后可能被 Microsoft SmartScreen 拦截，属正常现象：

- **浏览器下载提示"可能有害/不常见"**：文件来自网络且无下载信誉，SmartScreen 会对未签名文件一律拦截
- **首次运行出现"Windows 已保护你的电脑 / 未知发布者"**：点击「更多信息 → 仍要运行」即可
- **解除单个文件拦截**：右键文件 → 属性 → 勾选「解除锁定」；或执行 `Unblock-File .\KeySentry.exe`

**校验下载完整性（SHA-256）**：Releases 页面为每个文件附有 SHA-256 值，下载后可比对：

```powershell
Get-FileHash .\KeySentry.exe -Algorithm SHA256
```

若确认是安全软件误报，可向微软提交申诉（类型选 "Software developer" → 误报），有助于清除该文件的判定：
<https://www.microsoft.com/en-us/wdsi/filesubmission>

> 本项目正在接入受信任的代码签名证书（OV/EV）。完成签名后的版本将不再出现上述下载与启动提示。

## 从源码构建

```
要求：Visual Studio 2019+（含 C++ 桌面开发工作负载）、CMake 3.15+
```

```powershell
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
# 产物位于 release\KeySentry.exe
```

如需对产物进行代码签名（消除下载与启动时的 SmartScreen 提示），在配置阶段传入证书即可自动签名，未传入则跳过：

```powershell
cmake -S . -B build -DKS_SIGN_PFX="D:/cert/KeySentry.pfx" -DKS_SIGN_PWD="证书密码"
cmake --build build --config Release
# 签名参数：SHA-256 + RFC3161 时间戳
```

发布时建议同时生成校验值并附在 Release 说明中：

```powershell
Get-FileHash .\release\KeySentry.exe, .\release\KeySentry-x64.exe -Algorithm SHA256 |
  Format-Table Hash, Path -AutoSize
```

## 使用注意事项

- **管理员权限**：键客本身**不需要**管理员权限，所有功能（热键注册、全局钩子、窗口置顶等）在普通用户权限下即可正常工作。仅在自定义热键中勾选"以管理员运行"时，会触发 UAC 提权启动子进程
- **单实例**：重复启动会自动激活已运行实例的设置窗口
- **配置文件**：默认为程序同目录的 `KeySentry.ini`；目录不可写时自动回退到 `%APPDATA%\KeySentry\`
- **安全软件**：如遇弹窗拦截或功能异常，请将键客添加到安全软件信任列表；浏览器下载拦截与启动提示的说明见[验证下载文件与安全说明](#验证下载文件与安全说明)

## 支持系统

Windows 7 / Windows 10 / Windows 11（32 位 & 64 位）

## 许可证

本项目基于 [MIT License](LICENSE) 开源。

作者：Marvin 翁敏峰
