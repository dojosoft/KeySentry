// ===== 键客 KeySentry 主入口文件 =====
// 功能：Windows 桌面应用主入口，负责窗口创建、消息循环、
//       系统托盘、热键注册、配置应用等核心逻辑

#include <Windows.h>
#include <ShellAPI.h>
#include <ShlObj.h>
#include <CommCtrl.h>

#include "Resource.h"
#include "Config.h"
#include "KeyboardHook.h"
#include "Overlay.h"
#include "BossKey.h"
#include "WinDGuard.h"
#include "SettingsDialog.h"
#include "Utils.h"

// ===== 应用常量 =====
static const wchar_t* APP_NAME = L"键客";                    // 应用显示名称
static const wchar_t* MAIN_WND_CLASS = L"KeySentryMainWnd";  // 主窗口类名
static const wchar_t* MUTEX_NAME = L"Global\\KeySentrySingleInstance";  // 单实例互斥体名称，防止多开

// ===== 热键 ID 常量 =====
static constexpr int BOSS_HOTKEY_ID = 100;          // 老板键（窗口隐藏/恢复）热键 ID
static constexpr int BOSS_CLOSE_HOTKEY_ID = 101;    // 一键关闭热键 ID
static constexpr int CUSTOM_HOTKEY_ID_START = 200;  // 自定义热键 ID 起始值，每个自定义热键依次递增

// ===== 定时器间隔常量 =====
static constexpr UINT TIMER_WIND_CHECK_MS = 3000;    // Win+D 守护检查间隔（毫秒）
static constexpr UINT TIMER_CORNER_CHECK_MS = 2000;  // 鼠标角落检测间隔（毫秒）
static constexpr UINT TIMER_AUTO_HIDE_MS = 5000;     // 自动隐藏空闲检测间隔（毫秒）
static constexpr UINT TIMER_NUMLOCK_DELAY_MS = 2000;  // 启动后延迟开启 NumLock 的时间（毫秒）

// ===== 角落检测常量 =====
static constexpr int CORNER_THRESHOLD = 10;              // 鼠标距屏幕角落的像素阈值
static constexpr ULONGLONG CORNER_DEBOUNCE_MS = 1000;    // 角落触发防抖间隔（毫秒），避免短时间内重复触发

// ===== 全局状态变量 =====
static AppConfig g_config;          // 全局配置对象，保存所有用户设置
static KeyboardHook g_hook;         // 键盘钩子管理器，负责拦截和处理键盘事件
static OverlayWindow g_overlay;     // 悬浮提示窗口，用于显示锁定键状态等
static BossKey g_bossKey;           // 老板键管理器，负责窗口隐藏/恢复逻辑
static WinDGuard g_winDGuard;       // Win+D 守护管理器，防止 Win+D 最小化指定窗口
HWND g_mainWnd = nullptr;           // 主窗口句柄，全局可访问
static HINSTANCE g_hInst = nullptr; // 应用实例句柄
static std::wstring g_iniPath;      // 配置文件（INI）路径
static NOTIFYICONDATAW g_nid = {};  // 系统托盘图标数据结构
static bool g_settingsOpen = false; // 设置对话框是否已打开
static HWND g_settingsWnd = nullptr;   // 设置对话框窗口句柄
static bool g_trayIconVisible = true;  // 托盘图标是否可见
static ULONGLONG g_lastCornerTick = 0; // 上次角落触发的时间戳，用于防抖
static UINT g_msgTaskbarCreated = 0;  // TaskbarCreated 消息ID，用于 explorer 重启后恢复托盘图标

// ===== SetSettingsWnd =====
// 功能：设置设置对话框的窗口句柄，供外部模块调用
void SetSettingsWnd(HWND wnd) {
    g_settingsWnd = wnd;
}

// ===== SetHookSimulating =====
// 功能：设置键盘钩子的模拟输入标志，避免钩子拦截自身发送的按键
void SetHookSimulating(bool sim) {
    g_hook.SetSimulatingInput(sim);
}

// ===== GetIniPath =====
// 功能：获取配置文件 INI 的完整路径
// 逻辑：优先使用 exe 所在目录（便携模式）；若目录不可写（如 Program Files），
//       回退到 %APPDATA%\KeySentry\，避免配置保存静默失败
static std::wstring GetIniPath() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);  // 获取当前 exe 完整路径
    std::wstring path(exePath);
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) path = path.substr(0, pos + 1);  // 截取目录部分
    std::wstring iniPath = path + L"KeySentry.ini";

    // 检测 exe 目录是否可写（OPEN_ALWAYS 不截断已有文件内容）
    HANDLE hTest = CreateFileW(iniPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hTest != INVALID_HANDLE_VALUE) {
        CloseHandle(hTest);
        return iniPath;
    }

    // 不可写：回退到 %APPDATA%\KeySentry\KeySentry.ini
    wchar_t appData[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) {
        std::wstring dir = std::wstring(appData) + L"\\KeySentry";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\KeySentry.ini";
    }
    return iniPath;  // 获取 AppData 失败时仍返回原路径
}

// ===== SetAutoStart =====
// 功能：设置或取消开机自启动
// 逻辑：通过写入/删除注册表 Run 键值实现，优先使用 64 位注册表视图
static void SetAutoStart(bool enable) {
    HKEY hKey = nullptr;
    REGSAM access = KEY_SET_VALUE | KEY_WOW64_64KEY;  // 优先尝试 64 位注册表视图
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                       L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                       0, access, &hKey) != ERROR_SUCCESS) {
        access = KEY_SET_VALUE;  // 回退到默认视图
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                           L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                           0, access, &hKey) != ERROR_SUCCESS) return;
    }

    if (enable) {
        // 写入自启动键值：应用名 -> exe 路径
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        RegSetValueExW(hKey, APP_NAME, 0, REG_SZ,
                        (const BYTE*)exePath, (DWORD)((wcslen(exePath) + 1) * sizeof(wchar_t)));
    } else {
        // 删除自启动键值
        RegDeleteValueW(hKey, APP_NAME);
    }
    RegCloseKey(hKey);
}

// ===== AddTrayIcon =====
// 功能：添加系统托盘图标
// 逻辑：初始化 NOTIFYICONDATA 结构，设置图标、提示文字和回调消息，调用 Shell_NotifyIconW 添加
static void AddTrayIcon(HWND hwnd) {
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);          // 结构体大小
    g_nid.hWnd = hwnd;                     // 接收回调消息的窗口
    g_nid.uID = 1;                         // 图标标识符
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;  // 标记：图标、提示文字、回调消息
    g_nid.uCallbackMessage = WM_TRAYICON;  // 托盘事件的自定义消息
    g_nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));  // 加载应用图标
    wcscpy_s(g_nid.szTip, APP_NAME);       // 鼠标悬停提示文字
    g_trayIconVisible = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
}

// ===== RemoveTrayIcon =====
// 功能：移除系统托盘图标
static void RemoveTrayIcon() {
    if (g_trayIconVisible) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayIconVisible = false;
    }
}

// ===== ShowTrayIcon =====
// 功能：重新显示系统托盘图标（在图标被系统资源管理器重启等情况隐藏后调用）
static void ShowTrayIcon() {
    if (!g_trayIconVisible) {
        g_trayIconVisible = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
    }
}

// ===== ShowTrayMenu =====
// 功能：显示系统托盘右键菜单
// 逻辑：根据当前状态动态构建菜单项（设置、恢复/隐藏窗口、关于、退出）
static void ShowTrayMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);  // 获取鼠标当前位置

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SHOW, L"设置");
    // 根据老板键当前状态显示"恢复"或"隐藏"菜单项
    if (g_bossKey.IsActive()) {
        AppendMenuW(hMenu, MF_STRING, IDM_TRAY_RESTORE, L"恢复隐藏窗口");
    } else if (g_config.bossKeyEnabled) {
        AppendMenuW(hMenu, MF_STRING, IDM_TRAY_HIDE, L"隐藏窗口");
    }
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);  // 分隔线
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_ABOUT, L"关于");
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, L"退出");

    SetForegroundWindow(hwnd);  // 确保菜单能正确获得焦点和关闭
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);  // 发送空消息确保菜单能正常关闭
    DestroyMenu(hMenu);
}

// ===== ForceNumLockOn =====
// 功能：强制开启 NumLock 键
// 逻辑：检测当前 NumLock 状态，若未开启则使用 keybd_event 模拟按下并释放 NumLock 键
//       包含扫描码参数，确保键盘固件也正确响应
static void ForceNumLockOn() {
    if (!(GetKeyState(VK_NUMLOCK) & 0x0001)) {  // 检查 NumLock 是否未开启
        BYTE scanCode = (BYTE)MapVirtualKeyW(VK_NUMLOCK, MAPVK_VK_TO_VSC);
        keybd_event(VK_NUMLOCK, scanCode, KEYEVENTF_EXTENDEDKEY, 0);              // 按下 NumLock
        keybd_event(VK_NUMLOCK, scanCode, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0); // 释放 NumLock
    }
}

// ===== ApplyExplorerSettings =====
// 功能：根据配置重置 Windows 资源管理器文件夹选项
// 逻辑：修改注册表 HKCU\...\Explorer\Advanced 下的三个值，
//       然后广播 WM_SETTINGCHANGE 通知资源管理器刷新
// 配置值含义：0=不改变, 1=开启隐藏, 2=关闭隐藏（即显示）
static void ApplyExplorerSettings() {
    // 若三项均为"不改变"，则无需任何操作
    if (g_config.explorerHideHidden == 0 && g_config.explorerHideExt == 0 && g_config.explorerHideOS == 0)
        return;

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                      0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return;  // 静默失败，不阻塞启动
    }

    bool changed = false;
    DWORD val;

    // 隐藏文件和文件夹：Hidden=2(隐藏) / 1(显示)
    if (g_config.explorerHideHidden == 1) { val = 2; changed = true; RegSetValueExW(hKey, L"Hidden", 0, REG_DWORD, (BYTE*)&val, sizeof(val)); }
    else if (g_config.explorerHideHidden == 2) { val = 1; changed = true; RegSetValueExW(hKey, L"Hidden", 0, REG_DWORD, (BYTE*)&val, sizeof(val)); }

    // 隐藏已知文件类型扩展名：HideFileExt=1(隐藏) / 0(显示)
    if (g_config.explorerHideExt == 1) { val = 1; changed = true; RegSetValueExW(hKey, L"HideFileExt", 0, REG_DWORD, (BYTE*)&val, sizeof(val)); }
    else if (g_config.explorerHideExt == 2) { val = 0; changed = true; RegSetValueExW(hKey, L"HideFileExt", 0, REG_DWORD, (BYTE*)&val, sizeof(val)); }

    // 隐藏受保护的操作系统文件：ShowSuperHidden=0(隐藏) / 1(显示)
    if (g_config.explorerHideOS == 1) { val = 0; changed = true; RegSetValueExW(hKey, L"ShowSuperHidden", 0, REG_DWORD, (BYTE*)&val, sizeof(val)); }
    else if (g_config.explorerHideOS == 2) { val = 1; changed = true; RegSetValueExW(hKey, L"ShowSuperHidden", 0, REG_DWORD, (BYTE*)&val, sizeof(val)); }

    RegCloseKey(hKey);

    // 广播设置变更通知，让资源管理器刷新
    // 注意：不使用 "Policy"（会被识别为组策略变更，非管理员权限时可能被 Explorer 忽略）
    // 改用注册表子键路径作为通知参数，并配合 SHChangeNotify 确保非管理员也能生效
    if (changed) {
        DWORD_PTR dwResult = 0;
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                            (LPARAM)L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                            SMTO_ABORTIFHUNG, 1000, &dwResult);
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    }
}

// ===== AnyCornerEnabled =====
// 功能：检查是否有任意一个屏幕角落触发老板键功能被启用
static bool AnyCornerEnabled() {
    return g_config.bossKeyCornerTL || g_config.bossKeyCornerTR ||
           g_config.bossKeyCornerBL || g_config.bossKeyCornerBR;
}

// ===== AnyMouseButtonEnabled =====
// 功能：检查是否有任意一个鼠标按键触发老板键功能被启用
static bool AnyMouseButtonEnabled() {
    return g_config.bossKeyMiddleButton || g_config.bossKeySideButton1 || g_config.bossKeySideButton2;
}

// ===== FormatHKName =====
// 功能：将热键的修饰键和虚拟键码格式化为可读字符串
// 示例：MOD_CONTROL + 'C' -> "Ctrl+C"
static std::wstring FormatHKName(UINT mod, UINT vk) {
    std::wstring r;
    if (mod & MOD_CONTROL) r += L"Ctrl+";
    if (mod & MOD_ALT) r += L"Alt+";
    if (mod & MOD_SHIFT) r += L"Shift+";
    if (mod & MOD_WIN) r += L"Win+";
    r += AppConfig::VKToName((int)vk);  // 虚拟键码转可读名称
    return r;
}

// ===== DangerousShortcut 结构体 =====
// 功能：定义一条"危险快捷键"记录，包含修饰键、虚拟键码和功能描述
// 用于检测用户设置的热键是否与系统或常用应用快捷键冲突
struct DangerousShortcut {
    UINT mod;               // 修饰键组合（MOD_CONTROL、MOD_ALT 等）
    UINT vk;                // 虚拟键码
    const wchar_t* desc;    // 快捷键功能描述
};

// ===== g_dangerousShortcuts =====
// 功能：预定义的危险快捷键列表，涵盖常用的系统快捷键和应用程序快捷键
// 当用户设置的热键与这些快捷键冲突时，会给出警告提示
static const DangerousShortcut g_dangerousShortcuts[] = {
    // Ctrl 组合键
    { MOD_CONTROL, 'C', L"复制" },
    { MOD_CONTROL, 'V', L"粘贴" },
    { MOD_CONTROL, 'X', L"剪切" },
    { MOD_CONTROL, 'Z', L"撤销" },
    { MOD_CONTROL, 'Y', L"重做" },
    { MOD_CONTROL, 'A', L"全选" },
    { MOD_CONTROL, 'S', L"保存" },
    { MOD_CONTROL, 'P', L"打印" },
    { MOD_CONTROL, 'F', L"查找" },
    { MOD_CONTROL, 'N', L"新建" },
    { MOD_CONTROL, 'O', L"打开" },
    { MOD_CONTROL, 'W', L"关闭窗口" },
    { MOD_CONTROL, 'Q', L"退出" },
    { MOD_CONTROL, 'R', L"刷新" },
    { MOD_CONTROL, 'T', L"新标签" },
    { MOD_CONTROL, 'L', L"地址栏" },
    { MOD_CONTROL, 'H', L"历史" },
    { MOD_CONTROL, 'J', L"下载" },
    { MOD_CONTROL, 'E', L"搜索" },
    { MOD_CONTROL, 'I', L"收藏夹" },
    { MOD_CONTROL, 'D', L"添加书签" },
    { MOD_CONTROL, 'G', L"查找下一个" },
    { MOD_CONTROL, 'B', L"加粗" },
    { MOD_CONTROL, 'U', L"下划线" },
    { MOD_CONTROL, 'K', L"插入链接" },
    { MOD_CONTROL, 'M', L"缩进" },
    { MOD_CONTROL, VK_SPACE, L"输入法切换" },
    { MOD_CONTROL, VK_RETURN, L"换行/发送" },
    { MOD_CONTROL, VK_TAB, L"切换标签" },
    { MOD_CONTROL, VK_ESCAPE, L"开始菜单" },
    { MOD_CONTROL, VK_F4, L"关闭标签" },
    // Ctrl+Shift 组合键
    { MOD_CONTROL | MOD_SHIFT, VK_ESCAPE, L"任务管理器" },
    { MOD_CONTROL | MOD_SHIFT, 'N', L"新建文件夹" },
    { MOD_CONTROL | MOD_SHIFT, 'T', L"恢复标签" },
    { MOD_CONTROL | MOD_SHIFT, VK_TAB, L"反向切换标签" },
    // Alt 组合键
    { MOD_ALT, VK_TAB, L"切换窗口" },
    { MOD_ALT, VK_F4, L"关闭窗口" },
    { MOD_ALT, VK_SPACE, L"系统菜单" },
    { MOD_ALT, VK_RETURN, L"属性" },
    { MOD_ALT, VK_LEFT, L"后退" },
    { MOD_ALT, VK_RIGHT, L"前进" },
    { MOD_ALT, 'F', L"文件菜单" },
    { MOD_ALT, 'E', L"编辑菜单" },
    { MOD_ALT, 'V', L"查看菜单" },
    { MOD_ALT, VK_ESCAPE, L"切换窗口" },
    { MOD_ALT, VK_PRINT, L"窗口截图" },
    // 单功能键
    { 0, VK_F1, L"帮助" },
    { 0, VK_F2, L"重命名" },
    { 0, VK_F3, L"查找" },
    { 0, VK_F4, L"地址栏" },
    { 0, VK_F5, L"刷新" },
    { 0, VK_F11, L"全屏" },
    // Shift 组合键
    { MOD_SHIFT, VK_F10, L"右键菜单" },
    { MOD_SHIFT, VK_DELETE, L"永久删除" },
    { MOD_SHIFT, VK_INSERT, L"粘贴" },
};

// ===== CheckDangerousShortcut =====
// 功能：检查指定热键是否与危险快捷键列表冲突
// 参数：mod - 修饰键组合，vk - 虚拟键码
// 返回：若冲突则返回冲突描述字符串，否则返回空字符串
// 逻辑：先查预定义列表，再对 Alt+字母 和 Ctrl+数字 做额外启发式检查
static std::wstring CheckDangerousShortcut(UINT mod, UINT vk) {
    UINT cleanMod = mod & ~(MOD_NOREPEAT);  // 去除 MOD_NOREPEAT 标志，仅保留实际修饰键
    // 遍历危险快捷键列表，检查是否完全匹配
    for (const auto& ds : g_dangerousShortcuts) {
        if (ds.mod == cleanMod && ds.vk == vk) {
            return FormatHKName(mod, vk) + L"（" + ds.desc + L"）";
        }
    }
    // 额外检查：Alt+字母 可能与菜单快捷键冲突
    if (!(cleanMod & MOD_WIN) && vk != 0) {
        if ((cleanMod & MOD_ALT) && !(cleanMod & (MOD_CONTROL | MOD_SHIFT))) {
            if (vk >= 'A' && vk <= 'Z') {
                return FormatHKName(mod, vk) + L"（Alt+字母 可能与菜单快捷键冲突）";
            }
        }
        // 额外检查：Ctrl+数字 可能与输入法冲突
        if ((cleanMod & MOD_CONTROL) && !(cleanMod & (MOD_ALT | MOD_SHIFT))) {
            if (vk >= '0' && vk <= '9') {
                return FormatHKName(mod, vk) + L"（Ctrl+数字 可能与输入法冲突）";
            }
        }
    }
    return L"";
}

// ===== AddHotkeyConflict =====
// 功能：向冲突信息字符串中追加一条热键冲突记录
// 参数：conflicts - 冲突信息字符串引用，prefix - 冲突类型前缀，
//       mod/vk - 热键修饰键和虚拟键码，detail - 冲突详情
static void AddHotkeyConflict(std::wstring& conflicts, const wchar_t* prefix, UINT mod, UINT vk, const std::wstring& detail) {
    conflicts += L"  ";
    conflicts += prefix;
    conflicts += FormatHKName(mod, vk);
    if (!detail.empty()) {
        conflicts += L" - ";
        conflicts += detail;
    }
    conflicts += L"\n";
}

// ===== PreCheckHotkeyConflicts =====
// 功能：预检查新配置中的热键冲突，用于设置对话框中的实时冲突检测
// 参数：newConfig - 待检查的新配置
// 返回：冲突信息字符串，为空表示无冲突
// 逻辑：
//   1. 检查老板键与一键关闭热键之间的冲突
//   2. 检查自定义热键与老板键/一键关闭热键的冲突
//   3. 检查自定义热键之间的互相冲突
//   4. 检查所有热键与危险快捷键的冲突
//   5. 尝试注册热键检测是否被其他程序占用
//   6. 检测完成后恢复当前配置的热键注册
std::wstring PreCheckHotkeyConflicts(const AppConfig& newConfig) {
    std::wstring conflicts;

    // --- 第一阶段：逻辑冲突检查 ---

    // 检查窗口隐藏热键与一键关闭热键是否相同
    if (newConfig.bossKeyEnabled && (newConfig.bossKeyCloseMod != 0 || newConfig.bossKeyCloseVK != 0)) {
        if (newConfig.bossKeyMod == newConfig.bossKeyCloseMod && newConfig.bossKeyVK == newConfig.bossKeyCloseVK) {
            AddHotkeyConflict(conflicts, L"窗口隐藏与一键关闭热键冲突: ", newConfig.bossKeyMod, newConfig.bossKeyVK, L"");
        }
    }

    // 检查自定义热键与窗口隐藏热键冲突
    if (newConfig.bossKeyEnabled && newConfig.customHotkeysEnabled) {
        for (const auto& hk : newConfig.customHotkeys) {
            if ((hk.mod != 0 || hk.vk != 0) && hk.mod == newConfig.bossKeyMod && hk.vk == newConfig.bossKeyVK) {
                AddHotkeyConflict(conflicts, L"自定义热键与窗口隐藏冲突: ", hk.mod, hk.vk, L"");
            }
        }
    }

    // 检查自定义热键与一键关闭热键冲突
    if ((newConfig.bossKeyCloseMod != 0 || newConfig.bossKeyCloseVK != 0) && newConfig.customHotkeysEnabled) {
        for (const auto& hk : newConfig.customHotkeys) {
            if ((hk.mod != 0 || hk.vk != 0) && hk.mod == newConfig.bossKeyCloseMod && hk.vk == newConfig.bossKeyCloseVK) {
                AddHotkeyConflict(conflicts, L"自定义热键与一键关闭热键冲突: ", hk.mod, hk.vk, L"");
            }
        }
    }

    // 检查自定义热键之间的互相冲突
    if (newConfig.customHotkeysEnabled) {
        for (size_t i = 0; i < newConfig.customHotkeys.size(); i++) {
            for (size_t j = i + 1; j < newConfig.customHotkeys.size(); j++) {
                const auto& a = newConfig.customHotkeys[i];
                const auto& b = newConfig.customHotkeys[j];
                if ((a.mod != 0 || a.vk != 0) && a.mod == b.mod && a.vk == b.vk) {
                    AddHotkeyConflict(conflicts, L"自定义热键互相冲突: ", a.mod, a.vk, L"");
                }
            }
        }
    }

    // --- 第二阶段：危险快捷键冲突检查 ---

    // 检查窗口隐藏热键是否与常用快捷键冲突
    if (newConfig.bossKeyEnabled) {
        std::wstring danger = CheckDangerousShortcut(newConfig.bossKeyMod, newConfig.bossKeyVK);
        if (!danger.empty()) {
            AddHotkeyConflict(conflicts, L"窗口隐藏可能与常用快捷键冲突: ", newConfig.bossKeyMod, newConfig.bossKeyVK, danger);
        }
    }

    // 检查一键关闭热键是否与常用快捷键冲突
    if (newConfig.bossKeyCloseMod != 0 || newConfig.bossKeyCloseVK != 0) {
        std::wstring danger = CheckDangerousShortcut(newConfig.bossKeyCloseMod, newConfig.bossKeyCloseVK);
        if (!danger.empty()) {
            AddHotkeyConflict(conflicts, L"一键关闭热键可能与常用快捷键冲突: ", newConfig.bossKeyCloseMod, newConfig.bossKeyCloseVK, danger);
        }
    }

    // 检查自定义热键是否与常用快捷键冲突
    if (newConfig.customHotkeysEnabled) {
        for (const auto& hk : newConfig.customHotkeys) {
            if (hk.mod != 0 || hk.vk != 0) {
                std::wstring danger = CheckDangerousShortcut(hk.mod, hk.vk);
                if (!danger.empty()) {
                    std::wstring name = danger;
                    if (!hk.DisplayName().empty()) name += L" [" + hk.DisplayName() + L"]";
                    AddHotkeyConflict(conflicts, L"自定义热键可能与常用快捷键冲突: ", hk.mod, hk.vk, name);
                }
            }
        }
    }

    // --- 第三阶段：热键注册占用检测 ---

    // 先注销当前所有热键，以便尝试注册新配置的热键
    g_bossKey.Unregister();
    UnregisterHotKey(g_mainWnd, BOSS_CLOSE_HOTKEY_ID);
    for (size_t i = 0; i < MAX_CUSTOM_HOTKEYS; i++) {
        UnregisterHotKey(g_mainWnd, CUSTOM_HOTKEY_ID_START + (int)i);
    }

    // 尝试注册窗口隐藏热键，检测是否被其他程序占用
    if (newConfig.bossKeyEnabled) {
        UINT bossMod = newConfig.bossKeyMod | MOD_NOREPEAT;  // 优先尝试带 MOD_NOREPEAT 注册
        if (!RegisterHotKey(g_mainWnd, BOSS_HOTKEY_ID, bossMod, newConfig.bossKeyVK)) {
            if (!RegisterHotKey(g_mainWnd, BOSS_HOTKEY_ID, newConfig.bossKeyMod, newConfig.bossKeyVK)) {
                // 带/不带 MOD_NOREPEAT 均注册失败，说明热键被占用
                AddHotkeyConflict(conflicts, L"窗口隐藏被其他程序占用: ", newConfig.bossKeyMod, newConfig.bossKeyVK, L"");
            } else {
                UnregisterHotKey(g_mainWnd, BOSS_HOTKEY_ID);  // 检测完毕，注销
            }
        } else {
            UnregisterHotKey(g_mainWnd, BOSS_HOTKEY_ID);  // 检测完毕，注销
        }
    }

    // 尝试注册一键关闭热键
    if (newConfig.bossKeyCloseMod != 0 || newConfig.bossKeyCloseVK != 0) {
        UINT closeMod = newConfig.bossKeyCloseMod | MOD_NOREPEAT;
        if (!RegisterHotKey(g_mainWnd, BOSS_CLOSE_HOTKEY_ID, closeMod, newConfig.bossKeyCloseVK)) {
            if (!RegisterHotKey(g_mainWnd, BOSS_CLOSE_HOTKEY_ID, newConfig.bossKeyCloseMod, newConfig.bossKeyCloseVK)) {
                AddHotkeyConflict(conflicts, L"一键关闭热键被其他程序占用: ", newConfig.bossKeyCloseMod, newConfig.bossKeyCloseVK, L"");
            } else {
                UnregisterHotKey(g_mainWnd, BOSS_CLOSE_HOTKEY_ID);
            }
        } else {
            UnregisterHotKey(g_mainWnd, BOSS_CLOSE_HOTKEY_ID);
        }
    }

    // 尝试注册所有自定义热键
    if (newConfig.customHotkeysEnabled) {
        for (size_t i = 0; i < newConfig.customHotkeys.size() && i < MAX_CUSTOM_HOTKEYS; i++) {
            const auto& hk = newConfig.customHotkeys[i];
            if (hk.mod != 0 || hk.vk != 0) {
                UINT customMod = hk.mod | MOD_NOREPEAT;
                int id = CUSTOM_HOTKEY_ID_START + (int)i;
                if (!RegisterHotKey(g_mainWnd, id, customMod, hk.vk)) {
                    if (!RegisterHotKey(g_mainWnd, id, hk.mod, hk.vk)) {
                        std::wstring name;
                        if (!hk.DisplayName().empty()) name = L" [" + hk.DisplayName() + L"]";
                        AddHotkeyConflict(conflicts, L"自定义热键被其他程序占用: ", hk.mod, hk.vk, name);
                    } else {
                        UnregisterHotKey(g_mainWnd, id);
                    }
                } else {
                    UnregisterHotKey(g_mainWnd, id);
                }
            }
        }
    }

    // --- 第四阶段：恢复当前配置的热键注册 ---
    // 检测完毕后，将热键注册恢复为当前生效的配置

    if (g_config.bossKeyEnabled) {
        UINT bossMod = g_config.bossKeyMod | MOD_NOREPEAT;
        if (!g_bossKey.Register(g_mainWnd, BOSS_HOTKEY_ID, bossMod, g_config.bossKeyVK)) {
            g_bossKey.Register(g_mainWnd, BOSS_HOTKEY_ID, g_config.bossKeyMod, g_config.bossKeyVK);
        }
    }

    if (g_config.bossKeyCloseMod != 0 || g_config.bossKeyCloseVK != 0) {
        UINT closeMod = g_config.bossKeyCloseMod | MOD_NOREPEAT;
        if (!RegisterHotKey(g_mainWnd, BOSS_CLOSE_HOTKEY_ID, closeMod, g_config.bossKeyCloseVK)) {
            RegisterHotKey(g_mainWnd, BOSS_CLOSE_HOTKEY_ID, g_config.bossKeyCloseMod, g_config.bossKeyCloseVK);
        }
    }

    if (g_config.customHotkeysEnabled) {
        for (size_t i = 0; i < g_config.customHotkeys.size() && i < MAX_CUSTOM_HOTKEYS; i++) {
            const auto& hk = g_config.customHotkeys[i];
            if (hk.mod != 0 || hk.vk != 0) {
                UINT customMod = hk.mod | MOD_NOREPEAT;
                int id = CUSTOM_HOTKEY_ID_START + (int)i;
                if (!RegisterHotKey(g_mainWnd, id, customMod, hk.vk)) {
                    RegisterHotKey(g_mainWnd, id, hk.mod, hk.vk);
                }
            }
        }
    }

    return conflicts;
}

// ===== ApplyConfig =====
// 功能：将当前全局配置 g_config 应用到各功能模块
// 逻辑：依次配置键盘钩子、悬浮窗、NumLock 守护、老板键、一键关闭热键、
//       自定义热键、鼠标钩子、角落检测、自动隐藏、Win+D 守护、开机自启动等
static void ApplyConfig() {
    // --- 键盘钩子相关配置 ---
    g_hook.SetShowOverlay(g_config.showLockKeyOverlay);            // 是否显示锁定键状态提示
    g_overlay.SetPrimaryOnly(g_config.overlayPrimaryOnly);         // 提示是否仅在主显示器显示
    g_hook.SetDisableSpecifiedKeysEnabled(g_config.disableSpecifiedKeysEnabled);  // 是否启用禁用指定按键
    g_hook.SetDisableNumLock(g_config.disableNumLock);             // 是否禁用 NumLock 切换
    g_hook.SetForceInsertMode(g_config.forceInsertMode);           // 是否强制 Insert 模式
    g_hook.SetDisabledKeys(g_config.disabledKeyCodes);             // 设置被禁用的按键列表
    g_hook.SetKeyRemapEnabled(g_config.keyRemapEnabled);           // 是否启用按键重映射
    g_hook.SetKeyRemappings(g_config.keyRemappings);               // 设置按键重映射表
    g_hook.SetDisabledHotkeys(g_config.disabledHotkeys);           // 设置被禁用的系统热键列表

    // --- NumLock 守护 ---
    // disableNumLock 隐含 forceNumLockOn 的功能（确保 NumLock 开启才能锁定），
    // 因此当两者同时启用时只需在 disableNumLock 分支处理
    // 使用延迟定时器，避免开机自启动时键盘子系统尚未初始化导致 keybd_event 失效
    if (g_config.disableNumLock || g_config.forceNumLockOn) {
        SetTimer(g_mainWnd, TIMER_NUMLOCK_DELAY, TIMER_NUMLOCK_DELAY_MS, nullptr);
    }

    // --- 老板键和热键注册 ---
    g_bossKey.Unregister();  // 先注销旧的老板键注册
    if (!g_config.bossKeyEnabled && g_bossKey.IsActive()) {
        g_bossKey.Deactivate();  // 老板键被禁用时，恢复已隐藏的窗口
    }
    UnregisterHotKey(g_mainWnd, BOSS_CLOSE_HOTKEY_ID);  // 注销一键关闭热键
    for (size_t i = 0; i < MAX_CUSTOM_HOTKEYS; i++) {
        UnregisterHotKey(g_mainWnd, CUSTOM_HOTKEY_ID_START + (int)i);  // 注销所有自定义热键
    }

    // 注册窗口隐藏热键（老板键）
    if (g_config.bossKeyEnabled) {
        UINT bossMod = g_config.bossKeyMod | MOD_NOREPEAT;  // 优先带 MOD_NOREPEAT 注册
        if (!g_bossKey.Register(g_mainWnd, BOSS_HOTKEY_ID, bossMod, g_config.bossKeyVK)) {
            g_bossKey.Register(g_mainWnd, BOSS_HOTKEY_ID, g_config.bossKeyMod, g_config.bossKeyVK);  // 回退注册
        }
        g_bossKey.SetWindows(g_config.bossKeyWindows);  // 设置老板键关联的窗口列表
    }

    // 注册一键关闭热键
    if (g_config.bossKeyCloseMod != 0 || g_config.bossKeyCloseVK != 0) {
        UINT closeMod = g_config.bossKeyCloseMod | MOD_NOREPEAT;
        if (!RegisterHotKey(g_mainWnd, BOSS_CLOSE_HOTKEY_ID, closeMod, g_config.bossKeyCloseVK)) {
            if (!RegisterHotKey(g_mainWnd, BOSS_CLOSE_HOTKEY_ID, g_config.bossKeyCloseMod, g_config.bossKeyCloseVK)) {
                MessageBoxW(g_mainWnd, L"关闭热键注册失败，可能被其他程序占用", APP_NAME, MB_OK | MB_ICONWARNING);
            }
        }
    }

    // 注册自定义热键
    if (g_config.customHotkeysEnabled) {
        for (size_t i = 0; i < g_config.customHotkeys.size() && i < MAX_CUSTOM_HOTKEYS; i++) {
            const auto& hk = g_config.customHotkeys[i];
            if (hk.mod != 0 || hk.vk != 0) {
                UINT customMod = hk.mod | MOD_NOREPEAT;
                if (!RegisterHotKey(g_mainWnd, CUSTOM_HOTKEY_ID_START + (int)i, customMod, hk.vk)) {
                    if (!RegisterHotKey(g_mainWnd, CUSTOM_HOTKEY_ID_START + (int)i, hk.mod, hk.vk)) {
                        std::wstring msg = L"自定义热键 " + hk.DisplayName() + L" 注册失败，可能被其他程序占用";
                        MessageBoxW(g_mainWnd, msg.c_str(), APP_NAME, MB_OK | MB_ICONWARNING);
                    }
                }
            }
        }
    }

    // --- 老板键附加功能配置 ---
    g_bossKey.SetMuteEnabled(g_config.bossKeyMute);                  // 隐藏窗口时是否同时静音
    g_bossKey.SetHideCurrentWindow(g_config.bossKeyHideCurrent);     // 是否隐藏当前焦点窗口
    g_bossKey.SetSendPauseBeforeHide(g_config.bossKeySendPause);     // 隐藏前是否发送 Pause 键暂停游戏

    // --- 鼠标按键触发老板键 ---
    if (g_config.bossKeyEnabled && AnyMouseButtonEnabled()) {
        g_bossKey.InstallMouseHook(g_mainWnd);  // 安装鼠标钩子
        g_bossKey.UpdateMouseHookSettings(
            g_config.bossKeyMiddleButton,
            g_config.bossKeySideButton1,
            g_config.bossKeySideButton2);
    } else {
        g_bossKey.UninstallMouseHook();  // 卸载鼠标钩子
    }

    // --- 屏幕角落触发老板键 ---
    if (g_config.bossKeyEnabled && AnyCornerEnabled()) {
        SetTimer(g_mainWnd, TIMER_CORNER_CHECK, TIMER_CORNER_CHECK_MS, nullptr);  // 启动角落检测定时器
    } else {
        KillTimer(g_mainWnd, TIMER_CORNER_CHECK);
    }

    // --- 自动隐藏（空闲时触发老板键）---
    if (g_config.bossKeyEnabled && g_config.bossKeyAutoHide) {
        SetTimer(g_mainWnd, TIMER_AUTO_HIDE, TIMER_AUTO_HIDE_MS, nullptr);  // 启动自动隐藏检测定时器
    } else {
        KillTimer(g_mainWnd, TIMER_AUTO_HIDE);
    }

    // --- Win+D 守护 ---
    g_winDGuard.Configure(g_config.winDGuardEnabled, g_config.winDGuardWindows);

    // --- 开机自启动 ---
    SetAutoStart(g_config.autoStart);

    // --- Win+D 守护定时器 ---
    if (g_config.winDGuardEnabled && !g_config.winDGuardWindows.empty()) {
        SetTimer(g_mainWnd, TIMER_WIND_CHECK, TIMER_WIND_CHECK_MS, nullptr);  // 启动 Win+D 守护定时器
    } else {
        KillTimer(g_mainWnd, TIMER_WIND_CHECK);
    }
}

// ===== ApplySettingsCallback =====
// 功能：设置对话框中"应用"按钮的回调函数
// 逻辑：保存配置到 INI 文件，刷新 INI 缓存，然后应用新配置
static void ApplySettingsCallback() {
    g_config.Save(g_iniPath);
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, g_iniPath.c_str());  // 刷新 INI 文件缓存到磁盘
    ApplyConfig();
    ApplyExplorerSettings();
}

// ===== OpenSettings =====
// 功能：打开设置对话框
// 逻辑：若设置对话框已打开则激活现有窗口，否则创建新的设置对话框
static void OpenSettings(HWND hwnd) {
    if (g_settingsOpen) {
        // 设置对话框已打开，尝试激活现有窗口
        if (g_settingsWnd && IsWindow(g_settingsWnd)) {
            SetForegroundWindow(g_settingsWnd);
            ShowWindow(g_settingsWnd, SW_RESTORE);
        } else {
            SetForegroundWindow(hwnd);
        }
        return;
    }
    g_settingsOpen = true;
    SettingsDialog::SetApplyCallback(ApplySettingsCallback);  // 设置"应用"回调
    SettingsDialog::SetCaptureModeCallback([](bool enabled) {
        g_hook.SetCaptureMode(enabled);  // 设置热键捕获模式回调
    });
    SettingsDialog::Show(hwnd, g_config);  // 显示设置对话框（模态）
    g_settingsOpen = false;
    g_settingsWnd = nullptr;
}

// ===== HandleBossToggle =====
// 功能：切换老板键状态（隐藏/恢复窗口）
// 设置模拟输入标志，避免键盘钩子拦截 BossKey 自身发送的按键（如媒体暂停）
static void HandleBossToggle() {
    g_hook.SetSimulatingInput(true);
    g_bossKey.Toggle();
    g_hook.SetSimulatingInput(false);
}

// ===== CheckCornerHide =====
// 功能：检测鼠标是否移至屏幕角落，若在角落则触发老板键隐藏窗口
// 逻辑：获取鼠标位置，判断是否在四个角落的阈值范围内，
//       使用防抖机制避免短时间内重复触发
static void CheckCornerHide() {
    POINT pt;
    if (!GetCursorPos(&pt)) return;

    // 使用鼠标所在位置的显示器，而非前台窗口的显示器
    // 多显示器环境下，鼠标可能在副屏，前台窗口可能在主屏
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);
    int screenW = mi.rcWork.right - mi.rcWork.left;
    int screenH = mi.rcWork.bottom - mi.rcWork.top;
    int screenLeft = mi.rcWork.left;
    int screenTop = mi.rcWork.top;
    const int threshold = CORNER_THRESHOLD;

    // 判断鼠标是否在四个角落的阈值范围内
    bool inCorner = false;
    if (pt.x - screenLeft <= threshold && pt.y - screenTop <= threshold && g_config.bossKeyCornerTL) inCorner = true;           // 左上角
    else if (pt.x - screenLeft >= screenW - threshold && pt.y - screenTop <= threshold && g_config.bossKeyCornerTR) inCorner = true;  // 右上角
    else if (pt.x - screenLeft <= threshold && pt.y - screenTop >= screenH - threshold && g_config.bossKeyCornerBL) inCorner = true;  // 左下角
    else if (pt.x - screenLeft >= screenW - threshold && pt.y - screenTop >= screenH - threshold && g_config.bossKeyCornerBR) inCorner = true;  // 右下角

    if (!inCorner) return;

    // 防抖检查：距离上次触发不足 CORNER_DEBOUNCE_MS 毫秒则跳过
    ULONGLONG now = GetTickCount64();
    if (now - g_lastCornerTick < CORNER_DEBOUNCE_MS) return;
    g_lastCornerTick = now;

    // 仅在老板键未激活时触发隐藏
    if (!g_bossKey.IsActive()) {
        HandleBossToggle();
    }
}

// ===== CheckAutoHide =====
// 功能：检测用户空闲时间，若超过设定阈值则自动触发老板键隐藏窗口
// 逻辑：检查自上次老板键取消激活后是否已过 60 秒冷却期，
//       再通过 GetLastInputInfo 获取系统空闲时间，超过阈值则触发
static void CheckAutoHide() {
    // 未启用自动隐藏或老板键已激活时跳过
    if (!g_config.bossKeyAutoHide || g_bossKey.IsActive()) return;

    // 冷却期检查：上次取消激活后 60 秒内不再自动触发
    ULONGLONG now = GetTickCount64();
    ULONGLONG lastDeactivate = g_bossKey.GetLastDeactivateTick();
    if (lastDeactivate > 0 && now - lastDeactivate < 60000) return;

    // 获取系统最后输入时间，计算空闲时长
    LASTINPUTINFO lii = { sizeof(lii) };
    if (!GetLastInputInfo(&lii)) return;

    DWORD currentTick = GetTickCount();
    DWORD idleMs = currentTick - lii.dwTime;
    ULONGLONG thresholdMs = (ULONGLONG)g_config.bossKeyAutoHideTime * 60 * 1000;  // 配置的分钟数转毫秒

    if (idleMs >= thresholdMs) {
        HandleBossToggle();  // 空闲超时，触发隐藏
    }
}

// ===== AboutWndProc =====
// 功能："关于"对话框的窗口过程
// 逻辑：处理确定/取消按钮关闭窗口、WM_CLOSE 消息、静态控件和按钮的透明背景
static LRESULT CALLBACK AboutWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            DestroyWindow(hwnd);  // 点击确定或取消，销毁对话框
            return 0;
        }
        // GitHub发布页链接点击
        if (HIWORD(wp) == STN_CLICKED && LOWORD(wp) == 1001) {
            ShellExecuteW(hwnd, L"open", L"https://github.com/dojosoft/KeySentry", nullptr, nullptr, SW_SHOW);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        // GitHub链接文本设为蓝色
        HWND ctrl = (HWND)lp;
        if (GetDlgCtrlID(ctrl) == 1001) {
            SetTextColor(hdc, RGB(0, 102, 204));
        }
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_SETCURSOR:
        // 在GitHub链接上显示手型光标
        if ((HWND)wp == GetDlgItem(hwnd, 1001)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ===== ShowAboutDialog =====
// 功能：显示"关于"对话框
// 逻辑：注册窗口类（仅一次），创建包含应用图标、名称、版本号、
//       描述、版权信息和确定按钮的模态对话框
void ShowAboutDialog(HWND parent) {
    static const wchar_t* ABOUT_CLASS = L"KeySentryAboutDlg";
    static bool registered = false;
    if (!registered) {
        // 注册"关于"对话框的窗口类（仅首次调用时注册）
        WNDCLASSW wc = {};
        wc.lpfnWndProc = AboutWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = ABOUT_CLASS;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    // 计算窗口大小和居中位置
    RECT rc = { 0, 0, 360, 240 };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, ABOUT_CLASS,
                                 L"关于 键客",
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                 (screenW - winW) / 2, (screenH - winH) / 2,  // 居中显示
                                 winW, winH,
                                 parent, nullptr, GetModuleHandleW(nullptr), nullptr);

    // 创建 UI 字体
    GdiObjectGuard fontTitle(CreateUiFont(20, FW_BOLD));   // 标题字体：20px 粗体
    GdiObjectGuard fontNormal(CreateUiFont(14, FW_NORMAL)); // 正文字体：14px
    GdiObjectGuard fontSmall(CreateUiFont(13, FW_NORMAL));  // 小号字体：13px

    // 应用图标
    HICON hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));
    HWND iconCtrl = CreateWindowExW(0, L"STATIC", L"",
                                      WS_CHILD | WS_VISIBLE | SS_ICON | SS_CENTERIMAGE,
                                      20, 20, 48, 48,
                                      hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(iconCtrl, STM_SETICON, (WPARAM)hIcon, 0);

    // 标题文字
    HWND titleCtrl = CreateWindowExW(0, L"STATIC", L"键客 KeySentry",
                                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       80, 20, 260, 32,
                                       hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(titleCtrl, WM_SETFONT, (WPARAM)fontTitle.get(), TRUE);

    // 版本号
    HWND verCtrl = CreateWindowExW(0, L"STATIC", L"v1.8.0.0901",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     80, 55, 260, 24,
                                     hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(verCtrl, WM_SETFONT, (WPARAM)fontNormal.get(), TRUE);

    // 分隔线
    HWND lineCtrl = CreateWindowExW(0, L"STATIC", L"",
                                      WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                                      20, 88, 320, 2,
                                      hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);

    // 应用描述（横向居中）
    HWND descCtrl = CreateWindowExW(0, L"STATIC",
                                      L"一款掌控你的键盘和窗口的轻量工具。",
                                      WS_CHILD | WS_VISIBLE | SS_CENTER,
                                      20, 100, 320, 24,
                                      hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(descCtrl, WM_SETFONT, (WPARAM)fontSmall.get(), TRUE);

    // 版权信息 + 分隔符 + GitHub链接（同一行，动态测量文本宽度后整体居中）
    const wchar_t* copyText = L"© 2026 Marvin 翁敏峰";
    const wchar_t* sepText = L"|";
    const wchar_t* gitText = L"GitHub";
    HDC hdcMeasure = GetDC(hwnd);
    HGDIOBJ oldFont = SelectObject(hdcMeasure, fontSmall.get());
    SIZE szCopy = {}, szSep = {}, szGit = {};
    GetTextExtentPoint32W(hdcMeasure, copyText, (int)wcslen(copyText), &szCopy);
    GetTextExtentPoint32W(hdcMeasure, sepText, (int)wcslen(sepText), &szSep);
    GetTextExtentPoint32W(hdcMeasure, gitText, (int)wcslen(gitText), &szGit);
    SelectObject(hdcMeasure, oldFont);
    ReleaseDC(hwnd, hdcMeasure);
    const int gap = 6;  // 元素间距
    int rowY = 128;
    int totalW = szCopy.cx + gap + szSep.cx + gap + szGit.cx;
    int startX = (360 - totalW) / 2;

    HWND copyCtrl = CreateWindowExW(0, L"STATIC", copyText,
                                      WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      startX, rowY, szCopy.cx + 4, 24,
                                      hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(copyCtrl, WM_SETFONT, (WPARAM)fontSmall.get(), TRUE);

    HWND sepCtrl = CreateWindowExW(0, L"STATIC", sepText,
                                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     startX + szCopy.cx + gap, rowY, szSep.cx + 4, 24,
                                     hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(sepCtrl, WM_SETFONT, (WPARAM)fontSmall.get(), TRUE);

    // GitHub链接
    HWND githubCtrl = CreateWindowExW(0, L"STATIC", gitText,
                                        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY,
                                        startX + szCopy.cx + gap + szSep.cx + gap, rowY, szGit.cx + 4, 24,
                                        hwnd, (HMENU)1001, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(githubCtrl, WM_SETFONT, (WPARAM)fontSmall.get(), TRUE);

    // 确定按钮
    HWND okBtn = CreateWindowExW(0, L"BUTTON", L"确定",
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
                                   140, 165, 80, 32,
                                   hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(okBtn, WM_SETFONT, (WPARAM)fontNormal.get(), TRUE);

    // 进入模态消息循环，阻塞直到对话框关闭
    {
        ModalGuard guard(parent);
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        RunModalLoop(hwnd);
    }
}

// ===== MainWndProc =====
// 功能：主窗口的窗口过程，处理所有消息
// 逻辑：分发处理托盘事件、菜单命令、锁定键通知、定时器、热键、
//       鼠标老板键触发、窗口关闭/销毁等消息
static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    // --- 托盘图标事件 ---
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP) {
            ShowTrayMenu(hwnd);  // 右键点击托盘图标：显示右键菜单
        } else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            OpenSettings(hwnd);  // 左键双击托盘图标：打开设置
        }
        return 0;

    // --- 菜单命令 ---
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_TRAY_SHOW:
            OpenSettings(hwnd);      // "设置"菜单项
            return 0;
        case IDM_TRAY_RESTORE:
            HandleBossToggle();      // "恢复隐藏窗口"菜单项
            return 0;
        case IDM_TRAY_HIDE:
            HandleBossToggle();      // "隐藏窗口"菜单项
            return 0;
        case IDM_TRAY_ABOUT:
            ShowAboutDialog(hwnd);   // "关于"菜单项
            return 0;
        case IDM_TRAY_EXIT: {
            DestroyWindow(hwnd);     // "退出"菜单项：销毁主窗口触发退出
            return 0;
        }
        }
        break;

    // --- 锁定键状态通知 ---
    // 由键盘钩子发送，wp 为虚拟键码，lp 为状态：
    //   0=关闭, 1=开启, 2=查询当前状态, 3=已锁定且开启
    case WM_LOCKKEY_NOTIFY: {
        int vkCode = (int)wp;
        bool locked = (lp == 3);
        bool state;
        if (lp == 2) {
            state = (GetKeyState(vkCode) & 0x0001) != 0;
        } else {
            state = lp != 0;
        }
        g_overlay.Show(vkCode, state, locked);  // 显示锁定键状态悬浮提示
        return 0;
    }

    // --- 静音状态通知 ---
    case WM_MUTE_NOTIFY: {
        if (g_config.showMuteOverlay) {
            bool isMuted = false;
            int volLevel = 0;
            g_overlay.GetAudioState(isMuted, volLevel);  // 获取当前音频状态
            g_overlay.ShowMute(isMuted);  // 显示静音状态悬浮提示
        }
        return 0;
    }

    // --- 热键被屏蔽通知 ---
    // 来自 KeyboardHook 的延迟通知，wParam=修饰键，lParam=虚拟键码
    case WM_HOTKEY_BLOCKED: {
#ifdef _DEBUG
        // 调试模式下记录被屏蔽的热键
        wchar_t buf[64];
        swprintf_s(buf, L"[KeySentry] Hotkey blocked: mod=0x%X, vk=0x%X\n",
                   (UINT)wParam, (UINT)lParam);
        OutputDebugStringW(buf);
#endif
        return 0;
    }

    // --- 定时器回调 ---
    case WM_TIMER:
        if (wp == TIMER_WIND_CHECK) {
            g_winDGuard.CheckNewWindows();  // Win+D 守护：检查并恢复被最小化的受保护窗口
        } else if (wp == TIMER_CORNER_CHECK) {
            CheckCornerHide();  // 角落检测：检查鼠标是否在屏幕角落
        } else if (wp == TIMER_AUTO_HIDE) {
            CheckAutoHide();  // 自动隐藏：检查用户空闲时间是否超时
        } else if (wp == TIMER_NUMLOCK_DELAY) {
            KillTimer(g_mainWnd, TIMER_NUMLOCK_DELAY);  // 一次性定时器，执行后立即销毁
            ForceNumLockOn();
            if (g_config.disableNumLock && g_config.showLockKeyOverlay) {
                PostMessageW(g_mainWnd, WM_LOCKKEY_NOTIFY, (WPARAM)VK_NUMLOCK, 3);
            }
        } else {
            break;  // 未知定时器，交给 DefWindowProcW 处理
        }
        return 0;

    // --- 热键触发 ---
    case WM_HOTKEY:
        if (wp == BOSS_HOTKEY_ID) {
            // 老板键触发：切换窗口隐藏/恢复
            HandleBossToggle();
        } else if (wp == BOSS_CLOSE_HOTKEY_ID) {
            // 一键关闭热键触发：销毁主窗口退出程序
            DestroyWindow(hwnd);
        } else if (wp >= CUSTOM_HOTKEY_ID_START && wp < CUSTOM_HOTKEY_ID_START + MAX_CUSTOM_HOTKEYS) {
            // 自定义热键触发：执行关联的命令
            int idx = (int)wp - CUSTOM_HOTKEY_ID_START;
            if (idx >= 0 && idx < (int)g_config.customHotkeys.size()) {
                const auto& hk = g_config.customHotkeys[idx];
                if (!hk.command.empty()) {
                    // 若配置了运行前确认，弹出确认对话框
                    if (hk.confirmBeforeRun) {
                        std::wstring msg = L"确定要执行: " + hk.DisplayName() + L" ?";
                        if (MessageBoxW(hwnd, msg.c_str(), L"确认", MB_YESNO | MB_ICONQUESTION) != IDYES)
                            return 0;
                    }

                    // 根据配置设置启动窗口状态
                    int showCmd = SW_SHOWNORMAL;
                    if (hk.windowState == HotkeyWindowState::Minimized) showCmd = SW_SHOWMINIMIZED;
                    else if (hk.windowState == HotkeyWindowState::Maximized) showCmd = SW_SHOWMAXIMIZED;
                    else if (hk.windowState == HotkeyWindowState::Hidden) showCmd = SW_HIDE;

                    bool launched = false;

                    // 非管理员模式：尝试使用 CreateProcess 启动（更精确的控制）
                    if (!hk.runAsAdmin) {
                        STARTUPINFOW si = {};
                        si.cb = sizeof(si);
                        si.dwFlags = STARTF_USESHOWWINDOW;
                        si.wShowWindow = (WORD)showCmd;
                        PROCESS_INFORMATION pi = {};

                        // 构建命令行字符串
                        std::wstring cmdLine = L"\"" + hk.command + L"\"";
                        if (!hk.parameters.empty()) {
                            cmdLine += L" " + hk.parameters;
                        }

                        const wchar_t* workDir = hk.workDir.empty() ? nullptr : hk.workDir.c_str();
                        if (CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                                           0, nullptr, workDir, &si, &pi)) {
                            CloseHandle(pi.hProcess);
                            CloseHandle(pi.hThread);
                            launched = true;
                        }
                    }

                    // CreateProcess 失败或需要管理员权限：使用 ShellExecute 启动
                    if (!launched) {
                        INT_PTR result = (INT_PTR)ShellExecuteW(
                            nullptr,
                            hk.runAsAdmin ? L"runas" : L"open",  // runas 触发 UAC 提权
                            hk.command.c_str(),
                            hk.parameters.c_str(),
                            hk.workDir.empty() ? nullptr : hk.workDir.c_str(),
                            showCmd);
                        if (result <= 32) {
                            // ShellExecute 返回值 <= 32 表示执行失败
                            std::wstring msg = L"执行失败: " + hk.command;
                            MessageBoxW(hwnd, msg.c_str(), L"错误", MB_OK | MB_ICONERROR);
                        }
                    }
                }
            }
        }
        return 0;

    // --- 鼠标按键触发老板键 ---
    // 由鼠标钩子发送，中键或侧键按下时触发
    case WM_BOSS_MOUSE_TOGGLE:
        HandleBossToggle();
        return 0;

    // --- 窗口关闭 ---
    // 点击关闭按钮时隐藏窗口而非退出，程序继续在托盘运行
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    // --- 系统电源事件 ---
    // 系统从睡眠/休眠恢复时，NumLock 状态可能被 BIOS/UEFI 重置
    // 需要重新强制开启 NumLock
    // 注意：仅处理 PBT_APMRESUMEAUTOMATIC，因为它覆盖所有唤醒场景
    // （用户手动唤醒时，系统先发 PBT_APMRESUMEAUTOMATIC 再发 PBT_APMRESUMESUSPEND，
    //  若同时处理两者会导致 ForceNumLockOn 被调用两次）
    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC) {
            if (g_config.disableNumLock || g_config.forceNumLockOn) {
                ForceNumLockOn();
            }
            return TRUE;
        }
        break;

    // --- 系统会话结束 ---
    // Windows 关机/注销时触发，执行清理并保存配置
    case WM_ENDSESSION:
        if (wp) {
            KillTimer(hwnd, TIMER_WIND_CHECK);
            KillTimer(hwnd, TIMER_CORNER_CHECK);
            KillTimer(hwnd, TIMER_AUTO_HIDE);
            if (g_bossKey.IsActive()) {
                if (g_config.bossKeyCloseOnExit) {
                    // 关机路径用短超时（500ms），避免阻塞系统关机
                    g_bossKey.CloseBoundProcesses(500);
                } else {
                    // Deactivate() 内部已调用 SaveRecoverFile()，无需重复
                    g_bossKey.Deactivate();
                }
            }
            g_winDGuard.Cleanup();
            g_hook.Uninstall();             // 卸载键盘钩子
            g_bossKey.UninstallMouseHook(); // 卸载鼠标钩子
            g_bossKey.Unregister();         // 注销老板键
            UnregisterHotKey(hwnd, BOSS_CLOSE_HOTKEY_ID);
            for (size_t i = 0; i < MAX_CUSTOM_HOTKEYS; i++) {
                UnregisterHotKey(hwnd, CUSTOM_HOTKEY_ID_START + (int)i);
            }
            g_config.Save(g_iniPath);  // 保存配置
        }
        return TRUE;  // MSDN 规定：wParam 为 TRUE 时必须返回 TRUE

    // --- 窗口销毁 ---
    // 程序退出时执行完整清理：停止定时器、恢复窗口、卸载钩子、
    // 注销热键、移除托盘图标、销毁悬浮窗、发送退出消息
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_WIND_CHECK);
        KillTimer(hwnd, TIMER_CORNER_CHECK);
        KillTimer(hwnd, TIMER_AUTO_HIDE);
        if (g_bossKey.IsActive()) {
            if (g_config.bossKeyCloseOnExit) {
                g_bossKey.CloseBoundProcesses();  // 配置了退出时关闭关联进程
            } else {
                g_bossKey.Deactivate();  // Deactivate() 内部已调用 SaveRecoverFile()，无需重复
            }
        }
        g_winDGuard.Cleanup();
        g_hook.Uninstall();             // 卸载键盘钩子
        g_bossKey.UninstallMouseHook(); // 卸载鼠标钩子
        g_bossKey.Unregister();         // 注销老板键
        UnregisterHotKey(hwnd, BOSS_CLOSE_HOTKEY_ID);
        for (size_t i = 0; i < MAX_CUSTOM_HOTKEYS; i++) {
            UnregisterHotKey(hwnd, CUSTOM_HOTKEY_ID_START + (int)i);  // 注销所有自定义热键
        }
        if (!g_trayIconVisible) ShowTrayIcon();  // 确保托盘图标可见后再移除
        RemoveTrayIcon();
        g_overlay.Destroy();  // 销毁悬浮提示窗口
        PostQuitMessage(0);   // 发送退出消息，结束消息循环
        return 0;
    }

    // --- explorer 重启后恢复托盘图标 ---
    // 当 explorer.exe 崩溃重启时，系统广播 TaskbarCreated 消息，
    // 所有托盘图标会被清除，需要重新添加
    if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated) {
        AddTrayIcon(hwnd);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ===== wWinMain =====
// 功能：应用程序入口点
// 流程：
//   1. 单实例检测（互斥体），若已运行则激活已有实例
//   2. 加载配置文件
//   3. 初始化通用控件和 COM
//   4. 注册主窗口类并创建主窗口
//   5. 创建悬浮提示窗口
//   6. 安装键盘钩子
//   7. 应用配置（注册热键、启动定时器等）
//   8. 添加系统托盘图标
//   9. 显示启动通知
//  10. 进入消息循环
//  11. 退出时保存配置、释放互斥体
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // 注册 TaskbarCreated 消息，用于 explorer 重启后恢复托盘图标
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // --- 单实例检测 ---
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已有实例运行，尝试激活其托盘图标双击事件（打开设置）
        if (hMutex) CloseHandle(hMutex);
        HWND existing = FindWindowW(MAIN_WND_CLASS, nullptr);
        if (existing) {
            SendMessageTimeoutW(existing, WM_TRAYICON, 0, WM_LBUTTONDBLCLK, SMTO_ABORTIFHUNG, 5000, nullptr);
        }
        return 0;
    }

    g_hInst = hInst;
    g_iniPath = GetIniPath();       // 获取配置文件路径
    g_config.Load(g_iniPath);       // 加载配置

    // --- 初始化通用控件（Tab 控件等）---
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TAB_CLASSES };
    InitCommonControlsEx(&icc);

    // 初始化 COM 库（用于音频控制等功能）
    CoInitGuard coInit;

    // --- 注册主窗口类 ---
    WNDCLASSW wc = {};
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = MAIN_WND_CLASS;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    if (!RegisterClassW(&wc)) {
        MessageBoxW(nullptr, L"窗口类注册失败", APP_NAME, MB_OK | MB_ICONERROR);
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 1;
    }

    // --- 创建主窗口 ---
    // 主窗口不可见，仅作为消息接收窗口存在
    g_mainWnd = CreateWindowExW(0, MAIN_WND_CLASS,
                                 L"键客 KeySentry —— 重新掌控你的键盘",
                                 WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 400, 300,
                                 nullptr, nullptr, hInst, nullptr);

    if (!g_mainWnd) {
        MessageBoxW(nullptr, L"主窗口创建失败", APP_NAME, MB_OK | MB_ICONERROR);
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 1;
    }

    // --- 创建悬浮提示窗口 ---
    if (!g_overlay.Create(hInst)) {
        MessageBoxW(nullptr, L"提示窗口创建失败", APP_NAME, MB_OK | MB_ICONERROR);
    }

    // --- 安装键盘钩子 ---
    if (!g_hook.Install(g_mainWnd)) {
        MessageBoxW(nullptr, L"键盘钩子安装失败，部分功能可能不可用。\n请检查是否有安全软件拦截。", APP_NAME, MB_OK | MB_ICONWARNING);
    }

    // --- 应用配置并添加托盘图标 ---
    ApplyConfig();
    ApplyExplorerSettings();  // 启动时重置资源管理器文件夹选项
    AddTrayIcon(g_mainWnd);

    // --- 显示启动通知气泡 ---
    if (!g_config.disableStartupNotification) {
        g_nid.uFlags = NIF_INFO;
        wcscpy_s(g_nid.szInfoTitle, APP_NAME);
        wcscpy_s(g_nid.szInfo, L"键客 KeySentry 已启动，如需设置请点击托盘图标。");
        g_nid.dwInfoFlags = NIIF_INFO;
        g_nid.uTimeout = 5000;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;  // 恢复标志位
    }

    // --- 恢复上次异常退出时被隐藏的窗口 ---
    // 老板键隐藏窗口后程序若崩溃/被强制结束，普通退出路径无法恢复，
    // 启动时读取恢复文件（存在即上次异常退出）并恢复窗口
    {
        int recovered = g_bossKey.RecoverOrphanedWindows();
        if (recovered > 0) {
            g_nid.uFlags = NIF_INFO;
            wcscpy_s(g_nid.szInfoTitle, APP_NAME);
            swprintf_s(g_nid.szInfo, L"检测到上次异常退出，已恢复 %d 个被隐藏的窗口。", recovered);
            g_nid.dwInfoFlags = NIIF_INFO;
            g_nid.uTimeout = 5000;
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;  // 恢复标志位
        }
    }

    // --- 主消息循环 ---
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // --- 退出清理 ---
    g_config.Save(g_iniPath);  // 保存最终配置
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, g_iniPath.c_str());  // 刷新 INI 缓存

    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    return (int)msg.wParam;
}
