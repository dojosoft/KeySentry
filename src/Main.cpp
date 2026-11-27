// ===== 键客 KeySentry 主入口文件 =====
// 功能：Windows 桌面应用主入口，负责窗口创建、消息循环、
//       系统托盘、热键注册、配置应用等核心逻辑

#include <Windows.h>
#include <ShellAPI.h>
#include <ShlObj.h>
#include <CommCtrl.h>
#include <Sddl.h>
#include <algorithm>
#include <set>

#include "Resource.h"
#include "Config.h"
#include "Lang.h"
#include "KeyboardHook.h"
#include "Overlay.h"
#include "BossKey.h"
#include "WinDGuard.h"
#include "SettingsDialog.h"
#include "FluentTheme.h"
#include "Utils.h"
#include "Version.h"

// ===== 应用常量 =====
static const wchar_t* APP_NAME = L"键客";                    // 应用显示名称
// 开机自启注册表值名：使用稳定的英文名，应用改名/多语言后不会残留旧条目
static const wchar_t* AUTO_START_VALUE_NAME = L"KeySentry";
static const wchar_t* MAIN_WND_CLASS = L"KeySentryMainWnd";  // 主窗口类名
// 单实例命名对象前缀。实际名称会追加当前用户 SID：
// 固定名称的 Global 对象可能被其他用户抢先创建并持有，导致本程序被拒绝启动（拒绝服务）；
// 按用户隔离后，不同用户之间不会互相影响。
static const wchar_t* MUTEX_PREFIX = L"Global\\KeySentrySingleInstance";
// 回退用：标准用户可能没有创建 Global 命名对象的权限，
// 完全放弃单实例保护会导致双开（双开会误恢复对方隐藏的窗口与窗口样式）
static const wchar_t* LOCAL_MUTEX_PREFIX = L"Local\\KeySentrySingleInstance";

// 生成带当前用户 SID 的命名对象名。同一用户仍可抢占（同一用户本就能终止进程，
// 不属于安全边界）；此修复针对跨用户预创建 Global 对象导致的启动拒绝。
static std::wstring BuildUserScopedMutexName(const wchar_t* prefix) {
    std::wstring sidText = L"unknown";
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        BYTE buffer[256] = {};
        DWORD needed = 0;
        if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &needed)) {
            auto* user = reinterpret_cast<TOKEN_USER*>(buffer);
            LPWSTR sid = nullptr;
            if (ConvertSidToStringSidW(user->User.Sid, &sid)) {
                sidText = sid;
                LocalFree(sid);
            }
        }
        CloseHandle(token);
    }
    return std::wstring(prefix) + L"_" + sidText;
}

// ===== 热键 ID 常量 =====
static constexpr int BOSS_HOTKEY_ID = 100;          // 老板键（窗口隐藏/恢复）热键 ID
static constexpr int BOSS_CLOSE_HOTKEY_ID = 101;    // 一键关闭热键 ID
static constexpr int CUSTOM_HOTKEY_ID_START = 200;  // 自定义热键 ID 起始值，每个自定义热键依次递增

// ===== 定时器间隔常量 =====
static constexpr UINT TIMER_WIND_CHECK_MS = 3000;    // Win+D 守护检查间隔（毫秒）
static constexpr UINT TIMER_CORNER_CHECK_MS = 2000;  // 鼠标角落检测间隔（毫秒）
static constexpr UINT TIMER_AUTO_HIDE_MS = 5000;     // 自动隐藏空闲检测间隔（毫秒）
static constexpr UINT TIMER_NUMLOCK_DELAY_MS = 2000;  // 启动后延迟开启 NumLock 的时间（毫秒）
// 静音键按下后，键盘钩子早于系统完成静音切换，故轮询等待系统真正生效后再显示浮层，
// 保证浮层显示的静音状态与系统实际状态一致（约 500ms 上限后强制按真实状态显示）
static constexpr UINT TIMER_MUTE_REFRESH_MS = 50;     // 静音状态轮询间隔（毫秒）
static constexpr UINT TIMER_HOOK_WATCH_MS = 1000;     // 键盘钩子探活/待释放按键重试间隔（毫秒）
// 启动时音频服务/默认端点可能尚未就绪，静音恢复失败后按此间隔静默重试
static constexpr UINT TIMER_MUTE_RESTORE_RETRY_MS = 5000;
static constexpr int MUTE_RESTORE_RETRY_MAX = 24;     // 最多重试约 2 分钟，仍失败则留待下次启动

static constexpr int  MUTE_REFRESH_MAX_TRIES = 6;     // 最大轮询次数（约 300ms，避免长延迟与过多 COM 查询）

// ===== 角落检测常量 =====
static constexpr int CORNER_THRESHOLD = 10;              // 鼠标距屏幕角落的像素阈值
static constexpr ULONGLONG CORNER_DEBOUNCE_MS = 1000;    // 角落触发防抖间隔（毫秒），避免短时间内重复触发
// 鼠标中键/侧键切换老板键的防抖间隔：连击或消息排队时只处理一次，
// 避免"隐藏-恢复-隐藏"连续翻转，以及恢复操作被后续消息覆盖
static constexpr ULONGLONG BOSS_MOUSE_TOGGLE_DEBOUNCE_MS = 400;

// ===== 全局状态变量 =====
static AppConfig g_config;          // 全局配置对象，保存所有用户设置
static KeyboardHook g_hook;         // 键盘钩子管理器，负责拦截和处理键盘事件
static OverlayWindow g_overlay;     // 悬浮提示窗口，用于显示锁定键状态等
static BossKey g_bossKey;           // 老板键管理器，负责窗口隐藏/恢复逻辑
static WinDGuard g_winDGuard;       // Win+D 守护管理器，防止 Win+D 最小化指定窗口
HWND g_mainWnd = nullptr;           // 主窗口句柄，全局可访问
std::atomic<bool> g_suppressHotkeys{false}; // 按键捕获期间为 true，各模态循环据此丢弃 WM_HOTKEY
static HINSTANCE g_hInst = nullptr; // 应用实例句柄
static std::wstring g_iniPath;      // 配置文件（INI）路径
// 配置文件的最后成功读写时间与大小：用于在退出时识别“运行期间被外部程序修改”，
// 避免退出保存静默覆盖用户手改的内容。
// 只比较时间戳会漏掉两类外部写入：写入方保留了原时间戳（复制/同步工具），
// 以及文件系统时间戳精度（FAT 2 秒）内的快速修改，故同时记录文件大小
static FILETIME g_configStamp = {};
static ULONGLONG g_configStampSize = 0;
static bool g_configStampValid = false;
static NOTIFYICONDATAW g_nid = {};  // 系统托盘图标数据结构
static bool g_settingsOpen = false; // 设置对话框是否已打开
static HWND g_settingsWnd = nullptr;   // 设置对话框窗口句柄
// 自定义热键确认框打开期间禁止再打开设置：确认框会进入模态消息循环，
// 若此时应用新配置，确认框持有的旧配置快照/引用会失效
static bool g_modalHotkeyConfirm = false;
// 当前进程是否提权运行（启动时缓存）：用于决定子进程是否需要降权启动
static bool g_processElevated = false;
// 托盘图标是否可见。初值必须为 false：图标要等启动流程走到 AddTrayIcon 才真正添加，
// 这期间产生的气泡提示若按"图标已存在"处理，会以 NIM_MODIFY 打到不存在的图标上被静默丢弃
static bool g_trayIconVisible = false;
// 托盘图标被老板键隐藏期间产生的气泡提示：气泡依附于图标，无图标时无法显示，
// 先缓存下来，等图标恢复后再补发，避免"保护未生效"这类一次性告警被静默丢弃
static std::wstring g_pendingTrayText;
static bool g_pendingTrayWarning = false;
static ULONGLONG g_lastCornerTick = 0; // 上次角落触发的时间戳，用于防抖
static ULONGLONG g_lastAutoHideTry = 0; // 上次空闲自动隐藏尝试的时间戳（同上，防止无窗口时反复重试）
static UINT g_msgTaskbarCreated = 0;  // TaskbarCreated 消息ID，用于 explorer 重启后恢复托盘图标
static bool g_numLockTimerArmed = false; // NumLock 延迟定时器是否已启动，避免反复"应用"不断推迟
static bool g_muteExpectState = false;   // 静音键按下后，系统应达到的目标静音状态
static int  g_muteRefreshTries = 0;      // 静音状态轮询已尝试次数
static bool g_numLockFeatureWasOn = false; // 上次应用配置时 NumLock 守护（禁用 NumLock / 强制开启）是否已启用
// 系统会话结束（关机/注销）清理是否已完成。
// WM_ENDSESSION 已卸载钩子并保存配置，随后主动销毁主窗口；
// 该标记用于让 WM_DESTROY 跳过会阻塞关机的重复进程关闭，
// 并让 wWinMain 跳过退出保存（避免在关机路径弹出"配置冲突"对话框）
static bool g_sessionEnding = false;
static int g_muteRestoreRetries = 0;      // 启动静音恢复的静默重试次数

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

// ===== 模拟按键引发的自身热键抑制 =====
// SendInput 注入的按键会被系统同样匹配到 RegisterHotKey（低级钩子的
// LLKHF_INJECTED 标记不影响热键分发）。若模拟目标恰好是本程序注册的热键，
// 就会再次进入 WM_HOTKEY；目标与触发键相同时形成无限循环。
// 这里记录本次模拟的组合与有效期，WM_HOTKEY 据此丢弃对应的自身注入消息。
static UINT g_simHotkeyMod = 0;
static UINT g_simHotkeyVK = 0;
static ULONGLONG g_simHotkeyUntil = 0;
static constexpr ULONGLONG SIM_HOTKEY_SUPPRESS_MS = 250;

// 判断 WM_HOTKEY 是否由正在进行的模拟按键产生
// lParam 低字为修饰键、高字为虚拟键码；比较时忽略仅用于注册的 MOD_NOREPEAT
static bool IsSelfSimulatedHotkey(LPARAM lp) {
    if (g_simHotkeyUntil == 0 || GetTickCount64() > g_simHotkeyUntil) return false;
    const UINT mods = (UINT)LOWORD((DWORD_PTR)lp) & ~(UINT)MOD_NOREPEAT;
    const UINT vk = (UINT)HIWORD((DWORD_PTR)lp);
    return vk == g_simHotkeyVK && mods == (g_simHotkeyMod & ~(UINT)MOD_NOREPEAT);
}

// 校验 WM_HOTKEY 携带的组合是否仍是当前配置。
// 配置变更/功能关闭后，消息队列中可能残留变更前产生的 WM_HOTKEY：
// 只按热键 ID 分发会让旧按键触发新配置的动作，甚至执行另一条自定义命令（R12-M1）
static bool HotkeyMatchesConfig(UINT expectedMod, UINT expectedVK, LPARAM lp) {
    const UINT mask = MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;
    if ((expectedMod & mask) == 0 || expectedVK == 0) return false;
    const UINT mods = (UINT)LOWORD((DWORD_PTR)lp) & mask;
    const UINT vk = (UINT)HIWORD((DWORD_PTR)lp);
    return vk == expectedVK && mods == (expectedMod & mask);
}

// 模拟按键发送失败提示：仅首次失败时走托盘气泡，避免热键连发时刷屏
static bool s_simulateFailedWarned = false;

static void WarnSimulateFailedOnce() {
    if (s_simulateFailedWarned) return;
    s_simulateFailedWarned = true;
    ShowTrayNotification(L"模拟按键发送失败：目标窗口可能以管理员权限运行，或输入被安全软件拦截。", true);
}

// 发送成功后复位：否则同一会话内后续新的失败不再提示
static void ResetSimulateFailedWarn() { s_simulateFailedWarned = false; }

// ===== QuoteCommandLineArg =====
// 按 Windows CreateProcess 的解析规则为一个参数加引号：
//   - 仅含普通字符时原样返回；
//   - 反斜杠只有在引号前或参数结尾时才需要加倍；
//   - 参数内的双引号用反斜杠转义。
// 用于 argv[0]（可执行文件路径），避免带空格的路径被拆成多段。
static std::wstring QuoteCommandLineArg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;

    std::wstring out;
    out.reserve(arg.size() + 8);
    out.push_back(L'"');
    for (auto it = arg.begin(); ; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            // 结尾反斜杠必须加倍，否则会转义收尾引号
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

// 判断窗口是否属于本进程：窗口类名是全局字符串，其他进程可以注册同名类，
// 单实例激活前必须校验归属，避免把激活消息发给伪造窗口
static bool IsOwnProcessWindow(HWND wnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(wnd, &pid);
    if (pid == 0) return false;
    const std::wstring other = ProcessUtils::GetProcessPath(pid);
    const std::wstring self = ProcessUtils::GetModulePath();
    return !other.empty() && !self.empty() && _wcsicmp(other.c_str(), self.c_str()) == 0;
}

// 路径拆分统一使用 ProcessUtils::PathDirectory/PathFileName（R13-A13）

// 规范化用于比较的路径（展开 8.3 短名等）。
// 不同启动方式可能得到同一目录的不同字符串表示，直接比较会误判为不同实例（R10-A2）
static std::wstring NormalizePathForCompare(const std::wstring& path) {
    if (path.empty()) return path;
    const DWORD needed = GetLongPathNameW(path.c_str(), nullptr, 0);
    if (needed == 0) return path;
    std::wstring buf((size_t)needed, L'\0');
    const DWORD len = GetLongPathNameW(path.c_str(), &buf[0], needed);
    if (len == 0 || len >= needed) return path;
    buf.resize(len);
    return buf;
}

// 判断窗口是否属于“本产品”的任一构建（x86/x64）。
// 单实例判定不能要求完整 exe 路径相等：KeySentry-x86.exe 与 KeySentry-x64.exe
// 文件名不同，但共享同一份 INI/恢复/锁文件，必须视为同一实例；否则两个
// 构建可同时运行并互相覆盖状态（R9-H2）。路径完全相等仍只用于是否发送
// 激活消息的安全校验（IsOwnProcessWindow）
static bool IsSameProductWindow(HWND wnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(wnd, &pid);
    if (pid == 0) return false;
    const std::wstring other = ProcessUtils::GetProcessPath(pid);
    const std::wstring self = ProcessUtils::GetModulePath();
    if (other.empty() || self.empty()) return false;
    const std::wstring otherNorm = NormalizePathForCompare(other);
    const std::wstring selfNorm = NormalizePathForCompare(self);
    // 必须位于同一目录：不同目录下的同名程序可能是不同分发/版本
    if (_wcsicmp(ProcessUtils::PathDirectory(otherNorm).c_str(),
                 ProcessUtils::PathDirectory(selfNorm).c_str()) != 0) {
        return false;
    }
    return ProcessUtils::IsProductExecutableName(otherNorm);
}

// 枚举回调：查找属于本产品的 KeySentry 主窗口
static BOOL CALLBACK FindSameProductMainWindowProc(HWND hwnd, LPARAM lp) {
    wchar_t cls[128] = {};
    if (GetClassNameW(hwnd, cls, (int)(sizeof(cls) / sizeof(cls[0]))) == 0) return TRUE;
    if (_wcsicmp(cls, MAIN_WND_CLASS) != 0) return TRUE;
    if (!IsSameProductWindow(hwnd)) return TRUE;
    *reinterpret_cast<HWND*>(lp) = hwnd;
    return FALSE;
}

// 查找本产品的主窗口。
// 不能用 FindWindowW：它只返回其中一个同名类窗口，其他进程伪造同名类窗口时
// 会让合法实例漏判并绕过单实例保护（R10-M1）
static HWND FindSameProductMainWindow() {
    HWND found = nullptr;
    EnumWindows(FindSameProductMainWindowProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

// ===== SimulateTargetKeyPress =====
// 功能：模拟单次按键或组合键（自定义热键的"模拟按键"模式）
// 流程：
//   1. 释放当前物理按住的修饰键——热键触发瞬间用户通常还按着 Ctrl/Alt 等，
//      不释放会导致目标程序收到"修饰键+目标键"的组合而非纯净输入
//   2. 按下目标组合的修饰键与主键，短暂停留后抬起
//   3. 恢复 Ctrl/Shift——用户仍物理按住时保持修饰键有效
//      注意：Alt 不恢复（单独恢复会激活程序菜单栏），
//            Win 不恢复（恢复后用户物理释放会误开开始菜单）
static void SimulateTargetKeyPress(UINT vk, UINT mods) {
    if (vk == 0) return;

    // 注入的按键会再次匹配系统热键：先登记本次组合，WM_HOTKEY 中忽略它，
    // 避免模拟目标等于本程序热键时被再次触发（相同组合会无限循环）
    g_simHotkeyMod = mods;
    g_simHotkeyVK = vk;
    g_simHotkeyUntil = GetTickCount64() + SIM_HOTKEY_SUPPRESS_MS;

    // 构造一个键盘输入事件（扩展键自动附加 KEYEVENTF_EXTENDEDKEY）
    auto fillKey = [](INPUT& in, UINT keyVk, bool keyUp) {
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = (WORD)keyVk;
        in.ki.wScan = (WORD)MapVirtualKeyW(keyVk, MAPVK_VK_TO_VSC);
        in.ki.dwFlags = keyUp ? KEYEVENTF_KEYUP : 0;
        if (NeedsExtendedKeyFlag(keyVk)) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    };

    // 统一检查 SendInput 返回值：提权窗口的 UIPI 或安全软件都可能拒绝注入，
    // 静默失败会让用户以为热键没反应
    bool sendFailed = false;
    auto sendChecked = [&sendFailed](const INPUT* inputs, UINT count) {
        if (count == 0) return true;
        if (SendInput(count, const_cast<INPUT*>(inputs), sizeof(INPUT)) == count) return true;
        sendFailed = true;
        return false;
    };

    // 先原子地进入模拟态并复位物理修饰键跟踪：从此刻起钩子会记录用户是否
    // 中途松开 Ctrl/Shift。顺序必须在读取 heldAgg 之前，缩小竞态窗口
    g_hook.BeginSimulationTracking();

    // 记录热键触发瞬间的物理修饰键状态（注入按键前检查才准确）
    const UINT aggMods[3] = { VK_CONTROL, VK_SHIFT, VK_MENU }; // 聚合修饰键（不区分左右）
    bool heldAgg[3] = {};
    for (int i = 0; i < 3; i++) {
        heldAgg[i] = (GetAsyncKeyState(aggMods[i]) & 0x8000) != 0;
    }
    bool heldLWin = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0;
    bool heldRWin = (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;

    // 第一步：释放按住的修饰键
    INPUT rel[5] = {};
    int n = 0;
    for (int i = 0; i < 3; i++) {
        if (heldAgg[i]) {
            rel[n].type = INPUT_KEYBOARD;
            rel[n].ki.wVk = (WORD)aggMods[i];
            rel[n].ki.dwFlags = KEYEVENTF_KEYUP;
            n++;
        }
    }
    if (heldLWin) { rel[n].type = INPUT_KEYBOARD; rel[n].ki.wVk = VK_LWIN; rel[n].ki.dwFlags = KEYEVENTF_KEYUP; n++; }
    if (heldRWin) { rel[n].type = INPUT_KEYBOARD; rel[n].ki.wVk = VK_RWIN; rel[n].ki.dwFlags = KEYEVENTF_KEYUP; n++; }
    if (n > 0) {
        sendChecked(rel, (UINT)n);
        // 保留目标程序处理修饰键释放的最小间隔，同时缩短 UI 线程阻塞
        Sleep(10);
    }

    // 第二步：按下目标组合所需修饰键与主键
    // 修饰键顺序固定为 Ctrl/Shift/Alt/Win，与常见组合键习惯一致
    INPUT down[5] = {};
    UINT dn = 0;
    if (mods & MOD_CONTROL) fillKey(down[dn++], VK_CONTROL, false);
    if (mods & MOD_SHIFT)   fillKey(down[dn++], VK_SHIFT, false);
    if (mods & MOD_ALT)     fillKey(down[dn++], VK_MENU, false);
    if (mods & MOD_WIN)     fillKey(down[dn++], VK_LWIN, false);
    fillKey(down[dn++], vk, false);
    const bool pressed = sendChecked(down, dn);
    if (!pressed) {
        // 部分事件可能已插入：逐个抬起本次尝试按下的键，避免修饰键/目标键卡住
        INPUT undo[5] = {};
        UINT undoN = 0;
        for (UINT i = 0; i < dn; ++i) fillKey(undo[undoN++], (UINT)down[i].ki.wVk, true);
        SendInput(undoN, undo, sizeof(INPUT));
    }

    if (pressed) {
        // 抬起：主键先抬，修饰键按注入逆序抬起，避免组合状态错乱
        INPUT up[5] = {};
        UINT un = 0;
        fillKey(up[un++], vk, true);
        if (mods & MOD_WIN)     fillKey(up[un++], VK_LWIN, true);
        if (mods & MOD_ALT)     fillKey(up[un++], VK_MENU, true);
        if (mods & MOD_SHIFT)   fillKey(up[un++], VK_SHIFT, true);
        if (mods & MOD_CONTROL) fillKey(up[un++], VK_CONTROL, true);
        // 组合键留出少量间隔，确保目标程序先处理修饰键按下再收到主键
        if (mods != 0) Sleep(10);
        if (!sendChecked(up, un)) {
            // 抬起批次可能部分插入：对全部键补发 keyup（幂等），
            // 避免修饰键或主键残留在按下状态
            INPUT undo[5] = {};
            UINT undoN = 0;
            for (UINT i = 0; i < un; ++i) {
                fillKey(undo[undoN++], (UINT)up[i].ki.wVk, true);
            }
            SendInput(undoN, undo, sizeof(INPUT));
        }
    }

    // 第三步：恢复 Ctrl/Shift（仅限仍需保持的聚合修饰键，Alt/Win 见函数头注释）。
    // 若钩子在模拟期间观察到对应物理键已抬起，则不再补发 keydown：
    // 否则用户先松开、我们再注入按下，会让修饰键停留在按下状态
    INPUT re[2] = {};
    int m = 0;
    if (heldAgg[0] && !g_hook.WasControlReleasedDuringSimulation()) {
        re[m].type = INPUT_KEYBOARD;
        re[m].ki.wVk = VK_CONTROL;
        m++;
    }
    if (heldAgg[1] && !g_hook.WasShiftReleasedDuringSimulation()) {
        re[m].type = INPUT_KEYBOARD;
        re[m].ki.wVk = VK_SHIFT;
        m++;
    }
    // 仅在确实需要恢复修饰键时才等待：无修饰键场景无需阻塞 UI 线程
    if (m > 0) {
        Sleep(10);
        sendChecked(re, (UINT)m);
    }

    SetHookSimulating(false);
    if (sendFailed) WarnSimulateFailedOnce();
    else ResetSimulateFailedWarn();
}

// ===== GetIniPath =====
// 功能：获取配置文件 INI 的完整路径
// 逻辑：优先使用 exe 所在目录（便携模式）；若目录不可写（如 Program Files），
//       回退到 %APPDATA%\KeySentry\，避免配置保存静默失败
static std::wstring GetIniPath() {
    // 动态缓冲取 exe 目录：固定 MAX_PATH 在长路径安装目录下会被截断，
    // 导致配置文件写到错误位置
    std::wstring path = ProcessUtils::GetModuleDir();
    std::wstring iniPath = path + L"KeySentry.ini";

    // 预先计算 AppData 路径：用于“程序移动/权限变化后仍能找到旧配置”
    std::wstring appDataIni;
    const std::wstring appData = ProcessUtils::GetAppDataDir();
    if (!appData.empty()) {
        std::wstring dir = appData + L"\\KeySentry";
        CreateDirectoryW(dir.c_str(), nullptr);
        appDataIni = dir + L"\\KeySentry.ini";
    }

    // 在 exe 目录创建唯一临时测试文件探测可写性，
    // FILE_FLAG_DELETE_ON_CLOSE 保证关闭句柄即自动删除，不残留空文件。
    // 文件名由系统生成：固定名可能被预置重解析点，导致探测结果与实际写入不一致（R8-M12）
    std::wstring testPath;
    wchar_t testName[MAX_PATH] = {};
    if (!path.empty() && GetTempFileNameW(path.c_str(), L"ksw", 0, testName) != 0) {
        testPath = testName;
    } else {
        testPath = path + L".~wtest.tmp";
    }
    HANDLE hTest = CreateFileW(testPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE |
                                   FILE_FLAG_OPEN_REPARSE_POINT,
                               nullptr);
    if (hTest == INVALID_HANDLE_VALUE) {
        // GetTempFileName 已创建的文件由本函数负责清理（打开失败时不会自动删除）
        DeleteFileW(testPath.c_str());
    }
    if (hTest != INVALID_HANDLE_VALUE) {
        CloseHandle(hTest);  // 关闭即删除测试文件
        // exe 目录可写：优先用旧 AppData 配置（若 exe 旁还没有配置文件），
        // 避免程序从只读目录移到可写目录后设置“丢失”
        return ProcessUtils::MigrateOrPreferFile(iniPath, appDataIni);
    }

    // 不可写：回退到 %APPDATA%\KeySentry\KeySentry.ini；
    // 若 AppData 还没有配置而 exe 旁有，复制过去，保证仍能保存
    if (!appDataIni.empty()) {
        return ProcessUtils::MigrateOrPreferFile(appDataIni, iniPath);
    }
    return iniPath;  // 获取 AppData 失败时仍返回原路径
}

// ===== SetAutoStart =====
// 功能：设置或取消开机自启动
// 逻辑：通过写入/删除注册表 Run 键值实现，优先使用 64 位注册表视图

// 判断指定 Run 值的数据是否看起来由本程序写入（包含 KeySentry 可执行文件名）。
// 旧版本曾用中文值名“键客”，清理前必须确认归属，避免误删同名第三方自启项
static bool RunValueLooksLikeOurs(HKEY hKey, const wchar_t* valueName) {
    wchar_t buf[4096] = {};
    DWORD cbData = sizeof(buf);
    DWORD type = 0;
    if (RegQueryValueExW(hKey, valueName, nullptr, &type, (BYTE*)buf, &cbData) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return false;
    }
    buf[(sizeof(buf) / sizeof(buf[0])) - 1] = L'\0';
    // 解析出命令行中的可执行文件并比较文件名，避免“NotKeySentry.exe”这类
    // 仅包含相同子串的第三方自启项被误删
    const wchar_t* p = buf;
    while (*p == L' ' || *p == L'\t') ++p;
    std::wstring exe;
    if (*p == L'"') {
        const wchar_t* end = wcschr(p + 1, L'"');
        if (!end) return false;
        exe.assign(p + 1, end);
    } else {
        const wchar_t* end = p;
        while (*end && *end != L' ' && *end != L'\t') ++end;
        exe.assign(p, end);
    }
    return ProcessUtils::IsProductExecutableName(exe);
}

static void SetAutoStart(bool enable) {
    HKEY hKey = nullptr;
    REGSAM access = KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY;  // 优先尝试 64 位注册表视图
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                       L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                       0, access, &hKey) != ERROR_SUCCESS) {
        access = KEY_QUERY_VALUE | KEY_SET_VALUE;  // 回退到默认视图
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                           L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                           0, access, &hKey) != ERROR_SUCCESS) return;
    }

    if (enable) {
        // 写入自启动键值：应用名 -> exe 路径
        // 动态缓冲取路径，避免安装目录超过 MAX_PATH 时写入被截断的路径
        std::wstring exePath = ProcessUtils::GetModulePath();
        // 取不到自身路径时不能写入 ""（会生成一个无效自启项，
        // 并可能连带删除原本可用的旧值）
        if (exePath.empty()) {
            ShowTrayNotification(L"开机自启设置失败：无法获取程序路径。", true);
            RegCloseKey(hKey);
            return;
        }
        // Run 键按命令行解析，路径含空格（如 Program Files）必须加引号，
        // 否则会被拆成 "C:\Program.exe ..." 导致自启失败或启动错误程序
        std::wstring value = L"\"" + exePath + L"\"";
        // 同名值可能属于其他程序：不能直接覆盖（R13-M4）。
        // 仅当值不存在或确实指向本程序时才写入，否则保留第三方条目并提示用户
        const bool valueExists =
            RegQueryValueExW(hKey, AUTO_START_VALUE_NAME, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
        if (valueExists && !RunValueLooksLikeOurs(hKey, AUTO_START_VALUE_NAME)) {
            ShowTrayNotification(L"检测到同名开机自启项属于其他程序，已保留该条目，键客未写入自启。", true);
            RegCloseKey(hKey);
            return;
        }
        // 读取现有值，与当前值相同则跳过写入，避免每次应用都写注册表
        wchar_t curVal[MAX_PATH * 2 + 4] = {};
        DWORD cbData = sizeof(curVal);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, AUTO_START_VALUE_NAME, nullptr, &type, (BYTE*)curVal, &cbData) == ERROR_SUCCESS &&
            type == REG_SZ) {
            // 注册表值异常（未以 NUL 结尾）时强制截断，避免 wcscmp 越界读取
            curVal[(sizeof(curVal) / sizeof(curVal[0])) - 1] = L'\0';
            if (wcscmp(curVal, value.c_str()) == 0) {
                RegCloseKey(hKey);
                return;
            }
        }
        const LSTATUS writeStatus = RegSetValueExW(hKey, AUTO_START_VALUE_NAME, 0, REG_SZ,
                                                   (const BYTE*)value.c_str(),
                                                   (DWORD)((value.size() + 1) * sizeof(wchar_t)));
        if (writeStatus != ERROR_SUCCESS) {
            // 启动早期托盘图标尚未创建：告警会先缓存，AddTrayIcon 后补发
            ShowTrayNotification(L"开机自启设置失败：注册表写入被拒绝。", true);
        }
        // 清理旧版本使用中文应用名写入的值，避免同一程序出现两条自启项。
        // 仅在新值写入成功且旧值确实指向本程序时才删除：
        // 新值写入失败时删除旧值会让用户同时失去两条自启项
        if (writeStatus == ERROR_SUCCESS &&
            wcscmp(APP_NAME, AUTO_START_VALUE_NAME) != 0 &&
            RunValueLooksLikeOurs(hKey, APP_NAME)) {
            RegDeleteValueW(hKey, APP_NAME);
        }
    } else {
        // 仅当键值存在且确实指向本程序时才删除：
        // 同名值可能属于其他程序，无条件删除会破坏第三方自启项（R13-M4）
        if (RegQueryValueExW(hKey, AUTO_START_VALUE_NAME, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS &&
            RunValueLooksLikeOurs(hKey, AUTO_START_VALUE_NAME)) {
            RegDeleteValueW(hKey, AUTO_START_VALUE_NAME);
        }
        if (wcscmp(APP_NAME, AUTO_START_VALUE_NAME) != 0 &&
            RunValueLooksLikeOurs(hKey, APP_NAME)) {
            RegDeleteValueW(hKey, APP_NAME);  // 兼容旧版本写入的中文值名
        }
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
    wcsncpy_s(g_nid.szTip, Tr(L"键客 KeySentry"), _TRUNCATE); // 鼠标悬停提示文字
    g_trayIconVisible = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
    // 图标（重新）可见后补发此前被缓存的气泡提示（只保留最后一条，一次性告警不堆叠）
    if (g_trayIconVisible && !g_pendingTrayText.empty()) {
        std::wstring pending = g_pendingTrayText;
        bool pendingWarning = g_pendingTrayWarning;
        g_pendingTrayText.clear();
        g_pendingTrayWarning = false;
        ShowTrayNotification(pending, pendingWarning);
    }
}

// ===== RemoveTrayIcon =====
// 功能：移除系统托盘图标
static void RemoveTrayIcon() {
    if (g_trayIconVisible) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayIconVisible = false;
    }
}

// ===== ShowTrayNotification =====
// 功能：弹出托盘气泡通知（用于非阻塞式一次性提示）
// 逻辑：复用已存在的托盘图标句柄，附加 NIF_INFO 后 NIM_MODIFY，
//       随后恢复原 uFlags，避免影响后续托盘图标操作
void ShowTrayNotification(const std::wstring& text, bool warning) {
    // 主窗口已销毁（进程退出/静态析构阶段）时不再触碰托盘与全局缓存，
    // 避免在全局对象析构顺序不确定的情况下访问已销毁的 std::wstring
    if (!g_mainWnd || !IsWindow(g_mainWnd)) return;
    if (!g_trayIconVisible) {
        // 气泡依附于托盘图标：启动早期图标尚未添加、或已移除时无处可显示，
        // 缓存到最后一条，待图标重新添加（AddTrayIcon）后补发
        g_pendingTrayText = text;
        g_pendingTrayWarning = warning;
        return;
    }
    // 显示前统一翻译：词典覆盖整句消息与拼接消息中的固定片段
    const std::wstring display = TranslateMessageText(text);
    g_nid.uFlags = NIF_INFO;
    wcsncpy_s(g_nid.szInfoTitle, Tr(APP_NAME), _TRUNCATE);
    wcsncpy_s(g_nid.szInfo, display.c_str(), _TRUNCATE);
    g_nid.dwInfoFlags = warning ? NIIF_WARNING : NIIF_INFO;
    g_nid.uTimeout = 5000;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;  // 恢复标志位
}

// ===== 刷新托盘图标提示文字 =====
// 语言切换后调用，让鼠标悬停提示同步显示当前语言
static void UpdateTrayTip() {
    if (!g_trayIconVisible) return;
    wcsncpy_s(g_nid.szTip, Tr(L"键客 KeySentry"), _TRUNCATE);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
}

// ===== 主题切换后刷新本线程窗口 =====
// 重新装饰以更新列表颜色/系统控件主题，再整窗重绘；悬浮提示窗有独立配色，跳过
static BOOL CALLBACK RefreshThemeWindowProc(HWND hwnd, LPARAM) {
    wchar_t cls[64] = L"";
    GetClassNameW(hwnd, cls, 64);
    if (_wcsicmp(cls, L"KeySentryOverlayWnd") == 0) return TRUE;
    Fluent::ApplyWindowChrome(hwnd);
    Fluent::DecorateDialog(hwnd);
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    return TRUE;
}

static void RefreshThemeAllWindows() {
    EnumThreadWindows(GetCurrentThreadId(), RefreshThemeWindowProc, 0);
}

// ===== ShowTrayMenu =====
// 功能：显示系统托盘右键菜单
// 逻辑：根据当前状态动态构建菜单项（设置、恢复/隐藏窗口、关于、退出）
static void ShowTrayMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);  // 获取鼠标当前位置

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SHOW, Tr(L"设置"));
    // 根据老板键当前状态显示"恢复"或"隐藏"菜单项
    if (g_bossKey.IsActive()) {
        AppendMenuW(hMenu, MF_STRING, IDM_TRAY_RESTORE, Tr(L"恢复隐藏窗口"));
    } else if (g_config.bossKeyEnabled) {
        AppendMenuW(hMenu, MF_STRING, IDM_TRAY_HIDE, Tr(L"隐藏窗口"));
    }
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);  // 分隔线
    // 外观子菜单：跟随系统 / 浅色 / 深色（勾选当前项）
    HMENU hTheme = CreatePopupMenu();
    if (hTheme) {
        AppendMenuW(hTheme, MF_STRING | (g_config.appTheme == 0 ? MF_CHECKED : 0),
                    IDM_THEME_SYSTEM, Tr(L"跟随系统"));
        AppendMenuW(hTheme, MF_STRING | (g_config.appTheme == 1 ? MF_CHECKED : 0),
                    IDM_THEME_LIGHT, Tr(L"浅色"));
        AppendMenuW(hTheme, MF_STRING | (g_config.appTheme == 2 ? MF_CHECKED : 0),
                    IDM_THEME_DARK, Tr(L"深色"));
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hTheme, Tr(L"外观"));
    }
    // 语言子菜单：中文 / English（语言名用各自文字显示，便于识别）
    HMENU hLang = CreatePopupMenu();
    if (hLang) {
        AppendMenuW(hLang, MF_STRING | (g_config.appLanguage == 0 ? MF_CHECKED : 0),
                    IDM_LANG_ZH, L"中文");
        AppendMenuW(hLang, MF_STRING | (g_config.appLanguage == 1 ? MF_CHECKED : 0),
                    IDM_LANG_EN, L"English");
        // 语言菜单项故意使用与当前界面相反的语言：中文界面显示 "Language"，
        // 英文界面显示 "语言"，便于用户在当前语言下找到切换入口
        const wchar_t* langMenuText =
            (GetAppLanguage() == AppLanguage::Chinese) ? L"Language" : L"语言";
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hLang, langMenuText);
    }
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_ABOUT, Tr(L"关于"));
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, Tr(L"退出"));

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

// ===== NotifyExplorerSettingChanged =====
// 功能：通知资源管理器刷新文件夹选项
// 逻辑：只向资源管理器自己的顶层窗口发送 WM_SETTINGCHANGE，而不是 HWND_BROADCAST。
//       广播的超时是按"每个顶层窗口"分别计的，遇到多个响应缓慢的第三方窗口会串行累加，
//       在承载低级键鼠钩子的 UI 线程上表现为秒级卡顿；而这次变更实际只需要资源管理器知道。
//       取不到资源管理器进程（未运行或被第三方外壳替换）时退化为小超时广播兜底。
static void NotifyExplorerSettingChanged() {
    const wchar_t* param = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";

    DWORD explorerPid = 0;
    HWND shellWnd = GetShellWindow();
    if (shellWnd) GetWindowThreadProcessId(shellWnd, &explorerPid);

    if (explorerPid) {
        struct Ctx {
            DWORD pid;
            const wchar_t* param;
            ULONGLONG deadline;
        } ctx = { explorerPid, param, GetTickCount64() + 150 };
        // 整轮共享一次预算：预算耗尽后回调返回 FALSE 提前结束枚举，避免按窗口累加
        auto sendToExplorer = [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != c->pid) return TRUE;
            ULONGLONG now = GetTickCount64();
            if (now >= c->deadline) return FALSE;
            UINT timeout = (UINT)std::min<ULONGLONG>(150, c->deadline - now);
            DWORD_PTR result = 0;
            SendMessageTimeoutW(hwnd, WM_SETTINGCHANGE, 0, (LPARAM)c->param,
                                SMTO_ABORTIFHUNG, timeout, &result);
            return TRUE;
        };
        EnumWindows(sendToExplorer, reinterpret_cast<LPARAM>(&ctx));
        return;
    }

    // 兜底：资源管理器未运行（例如被替换成第三方外壳）时仍广播，超时压到最小
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)param,
                        SMTO_ABORTIFHUNG, 50, &result);
}

// ===== ApplyExplorerSettings =====
// 功能：根据配置重置 Windows 资源管理器文件夹选项
// 逻辑：修改注册表 HKCU\...\Explorer\Advanced 下的三个值，
//       然后广播 WM_SETTINGCHANGE 通知资源管理器刷新
// 配置值含义：0=不改变, 1=开启隐藏, 2=关闭隐藏（即显示）
static void ApplyExplorerSettings() {
    // 配置读取失败时 explorer 设置同样是默认值，不能据此改写用户的资源管理器选项
    if (g_config.LoadFailed()) return;
    // 若三项均为"不改变"，则无需任何操作
    if (g_config.explorerHideHidden == 0 && g_config.explorerHideExt == 0 && g_config.explorerHideOS == 0)
        return;

    // 与 SetAutoStart 使用一致的注册表视图策略：x86 构建在 64 位 Windows 上
    // 默认会访问 WOW64 重定向后的 32 位视图，而资源管理器读取的是 64 位视图，
    // 写入会"成功"但不生效。优先显式 64 位视图，失败再回退默认视图（R8-M6）
    static const wchar_t* kExplorerAdvanced =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
    HKEY hKey = nullptr;
    LSTATUS openStatus = RegOpenKeyExW(HKEY_CURRENT_USER, kExplorerAdvanced, 0,
                                       KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &hKey);
    if (openStatus != ERROR_SUCCESS) {
        openStatus = RegOpenKeyExW(HKEY_CURRENT_USER, kExplorerAdvanced, 0,
                                   KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey);
    }
    if (openStatus != ERROR_SUCCESS) {
        // 组策略/ACL 可能禁止写入：必须提示，否则用户会以为设置已经生效
        ShowTrayNotification(L"无法打开资源管理器设置注册表项，文件夹选项设置未生效。", true);
        return;
    }

    bool changed = false;
    int writeFailures = 0;

    // 仅当当前注册表值与目标值不同时才写入，避免每次启动都无谓写注册表并全系统广播
    auto applyDword = [&](const wchar_t* name, DWORD target) {
        DWORD cur = 0, curSize = sizeof(cur), curType = 0;
        bool needWrite = true;
        if (RegQueryValueExW(hKey, name, nullptr, &curType, (BYTE*)&cur, &curSize) == ERROR_SUCCESS &&
            curType == REG_DWORD && cur == target) {
            needWrite = false;  // 值已符合预期，无需改动
        }
        if (needWrite) {
            if (RegSetValueExW(hKey, name, 0, REG_DWORD, (BYTE*)&target, sizeof(target)) != ERROR_SUCCESS) {
                // 写入失败（组策略/权限）不能被当作已生效
                writeFailures++;
                return;
            }
            changed = true;
        }
    };

    // 隐藏文件和文件夹：Hidden=2(隐藏) / 1(显示)
    if (g_config.explorerHideHidden == 1) applyDword(L"Hidden", 2);
    else if (g_config.explorerHideHidden == 2) applyDword(L"Hidden", 1);

    // 隐藏已知文件类型扩展名：HideFileExt=1(隐藏) / 0(显示)
    if (g_config.explorerHideExt == 1) applyDword(L"HideFileExt", 1);
    else if (g_config.explorerHideExt == 2) applyDword(L"HideFileExt", 0);

    // 隐藏受保护的操作系统文件：ShowSuperHidden=0(隐藏) / 1(显示)
    if (g_config.explorerHideOS == 1) applyDword(L"ShowSuperHidden", 0);
    else if (g_config.explorerHideOS == 2) applyDword(L"ShowSuperHidden", 1);

    RegCloseKey(hKey);

    // 广播设置变更通知，让资源管理器刷新
    // 注意：不使用 "Policy"（会被识别为组策略变更，非管理员权限时可能被 Explorer 忽略）
    // 改用注册表子键路径作为通知参数，并配合 SHChangeNotify 确保非管理员也能生效
    if (changed) {
        // 只通知资源管理器自己的窗口，避免向全系统广播造成按窗口累加的超时阻塞
        NotifyExplorerSettingChanged();
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    }
    if (writeFailures > 0) {
        ShowTrayNotification(L"部分文件夹选项设置写入失败（可能被组策略或权限限制），这些设置未生效。", true);
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
// 统一复用设置层的实现，避免两处独立维护导致前缀顺序/名称不一致（R8-A4）
static std::wstring FormatHKName(UINT mod, UINT vk) {
    return SettingsDialog::FormatHotKey(mod, vk);
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
    // 遍历危险快捷键列表，检查是否完全匹配。
    // 描述是固定文案（不含用户数据），在此翻译；调用方随后拼入的用户描述保持原样
    for (const auto& ds : g_dangerousShortcuts) {
        if (ds.mod == cleanMod && ds.vk == vk) {
            return FormatHKName(mod, vk) + L"（" + Tr(ds.desc) + L"）";
        }
    }
    // 额外检查：Alt+字母 可能与菜单快捷键冲突
    if (!(cleanMod & MOD_WIN) && vk != 0) {
        if ((cleanMod & MOD_ALT) && !(cleanMod & (MOD_CONTROL | MOD_SHIFT))) {
            if (vk >= 'A' && vk <= 'Z') {
                return FormatHKName(mod, vk) + Tr(L"（Alt+字母 可能与菜单快捷键冲突）");
            }
        }
        // 额外检查：Ctrl+数字 可能与输入法冲突
        if ((cleanMod & MOD_CONTROL) && !(cleanMod & (MOD_ALT | MOD_SHIFT))) {
            if (vk >= '0' && vk <= '9') {
                return FormatHKName(mod, vk) + Tr(L"（Ctrl+数字 可能与输入法冲突）");
            }
        }
    }
    return L"";
}

// 冲突/失败提示中的用户字段展示上限：配置文件可手改写入超长描述，
// 不截断会生成超大消息并拖慢整段翻译/绘制（R10-M3）
static constexpr size_t MAX_MESSAGE_USER_FIELD = 120;
static std::wstring ShortenForMessage(const std::wstring& s) {
    return (s.size() <= MAX_MESSAGE_USER_FIELD)
               ? s
               : s.substr(0, MAX_MESSAGE_USER_FIELD) + L"…";
}

// ===== AddHotkeyConflict =====
// 功能：向冲突信息字符串中追加一条热键冲突记录
// 参数：conflicts - 冲突信息字符串引用，prefix - 冲突类型前缀，
//       mod/vk - 热键修饰键和虚拟键码，detail - 冲突详情
// 前缀是固定文案，在此按当前语言翻译；detail 可能含用户描述，保持原样（R10-M2）
static constexpr int MAX_CONFLICT_LINES = 20;
// 本轮冲突列表是否因行数上限被截断：置真后由 PreCheckHotkeyConflicts
// 在末尾追加“仅显示前 N 条”提示，避免用户以为冲突只有这些
static bool s_conflictLinesTruncated = false;
static void AddHotkeyConflict(std::wstring& conflicts, const wchar_t* prefix, UINT mod, UINT vk, const std::wstring& detail) {
    // 行数上限：极端配置（大量冲突/超长描述）不应生成超大确认消息（R10-M3）
    if (static_cast<int>(std::count(conflicts.begin(), conflicts.end(), L'\n')) >= MAX_CONFLICT_LINES) {
        s_conflictLinesTruncated = true;
        return;
    }
    conflicts += L"  ";
    conflicts += Tr(prefix);
    conflicts += FormatHKName(mod, vk);
    if (!detail.empty()) {
        conflicts += L" - ";
        conflicts += detail;
    }
    conflicts += L"\n";
}

// ===== ShowAppMessage =====
// 功能：显示应用级提示框，保证用户一定看得见、可操作
// 背景：主窗口 g_mainWnd 常驻隐藏（仅用于接收消息）。以它作为 MessageBox 的
//       owner 时，对话框会按不可见窗口的位置摆放，可能落到屏幕外或被其他窗口
//       遮挡；其模态循环还会让设置对话框等本线程窗口全部无法接收输入，表现为
//       "点击应用后设置窗口卡死、整机操作迟滞"。
// 因此：优先挂到当前可见的设置对话框上；否则以无 owner + 强制置前/置顶显示
// 返回值：与 MessageBox 相同（IDOK / IDYES / IDNO ...），便于需要分支的确认框复用
static int ShowAppMessage(const std::wstring& text, UINT flags = MB_OK | MB_ICONWARNING,
                          const wchar_t* caption = APP_NAME) {
    HWND owner = nullptr;
    if (g_settingsWnd && IsWindow(g_settingsWnd) && IsWindowVisible(g_settingsWnd)) {
        owner = g_settingsWnd;  // 应用设置时以设置对话框为 owner，位置与层级正常
    }
    return (int)Fluent::ShowMessageBox(owner, text.c_str(), caption,
                                       flags | MB_SETFOREGROUND | MB_TOPMOST);
}

// ===== 确认框内容构建 =====
// 清洗来自配置文件的用户字段：描述/命令/参数/工作目录都可能被外部程序改写，
// 其中的换行/制表符会在确认框里伪造出额外的字段行，让用户看到的确认内容
// 与实际执行内容不一致。这里统一把控制字符替换为空格（R8-H6）。
// 另外过滤双向/不可见格式字符（U+202E 等）：它们能改变确认框中的视觉顺序，
// 把危险命令显示成看似无害的内容，从而绕过"执行前确认"（R11-M4）
static std::wstring SanitizeConfirmField(const std::wstring& s) {
    std::wstring r;
    r.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L'\r' || c == L'\n' || c == L'\t' || c == L'\v' || c == L'\f' ||
            c == L'\u2028' || c == L'\u2029') {
            r += L' ';
            continue;
        }
        const unsigned u = (unsigned)c;
        // 软连字符、双向控制符、零宽连接符、不可见运算符、BOM：
        // 路径/命令中正常不会出现，直接删除而不是替换为空格
        if (u == 0x00AD || u == 0x061C || u == 0x180E ||
            (u >= 0x200B && u <= 0x200F) ||
            (u >= 0x202A && u <= 0x202E) ||
            (u >= 0x2060 && u <= 0x2064) ||
            (u >= 0x2066 && u <= 0x2069) ||
            u == 0xFEFF) {
            continue;
        }
        r += c;
    }
    return r;
}

// ===== ShowAppMessageRaw =====
// 功能：显示不做语言翻译的提示框（原样展示文本）
// 说明：确认框包含用户字段，走 Fluent::ShowMessageBox 的片段替换会改写用户数据，
//       造成“显示的确认内容”与“实际执行内容”不一致，因此确认框必须走本函数；
//       固定标签由调用方先经 Tr() 翻译，用户字段保持原样
static int ShowAppMessageRaw(const std::wstring& text, UINT flags = MB_OK | MB_ICONWARNING,
                             const wchar_t* caption = APP_NAME) {
    HWND owner = nullptr;
    if (g_settingsWnd && IsWindow(g_settingsWnd) && IsWindowVisible(g_settingsWnd)) {
        owner = g_settingsWnd;
    }
    return (int)Fluent::ShowMessageBoxRaw(owner, text.c_str(), caption,
                                          flags | MB_SETFOREGROUND | MB_TOPMOST);
}

// ===== 自定义热键确认对话框 =====
// 使用可滚动的只读编辑框完整展示将参与执行的字段：
// 系统 MessageBox 对超长文本没有滚动能力，若在调用方截断，用户看到的
// 内容会少于实际执行内容，危险后缀可藏在省略部分（R15-H04）。
namespace {

constexpr wchar_t kHotkeyConfirmClass[] = L"KeySentryHotkeyConfirmWnd";
constexpr int kConfirmIdHeader = 4001;
constexpr int kConfirmIdEdit = 4002;
constexpr int kConfirmIdSubtitle = 4003;
constexpr int kConfirmIdAdminNote = 4004;

struct HotkeyConfirmState {
    std::wstring body;
    std::wstring adminNote;
    HFONT adminNoteFont = nullptr;  // 管理员提示使用的小号粗体
    bool confirmed = false;
};

struct HotkeyConfirmLayout {
    RECT icon = {};
    RECT header = {};
    RECT subtitle = {};
    RECT adminNote = {};
    RECT edit = {};
    RECT ok = {};
    RECT cancel = {};
    int dividerY = 0;
    int dividerLeft = 0;
    int dividerRight = 0;
    bool hasAdminNote = false;
};

HotkeyConfirmLayout ComputeHotkeyConfirmLayout(HWND hwnd) {
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    const int margin = ScaleUi(18);
    const int iconSize = ScaleUi(24);
    const int iconGap = ScaleUi(10);
    const int headerH = ScaleUi(26);
    const int subtitleH = ScaleUi(20);
    const int gap = ScaleUi(10);
    const int buttonH = ScaleUi(32);
    const int buttonW = ScaleUi(96);

    HotkeyConfirmLayout layout;
    const int headerX = margin + iconSize + iconGap;

    layout.icon = { margin, margin + (headerH - iconSize) / 2,
                    margin + iconSize, margin + (headerH - iconSize) / 2 + iconSize };
    layout.header = { headerX, margin, rc.right - margin, margin + headerH };
    layout.subtitle = { headerX, margin + headerH + ScaleUi(2),
                        rc.right - margin, margin + headerH + ScaleUi(2) + subtitleH };
    layout.dividerY = margin + headerH + ScaleUi(2) + subtitleH + gap;
    layout.dividerLeft = margin;
    layout.dividerRight = rc.right - margin;

    const int buttonTop = rc.bottom - margin - buttonH;
    int editTop = layout.dividerY + ScaleUi(12);
    layout.hasAdminNote = GetDlgItem(hwnd, kConfirmIdAdminNote) != nullptr;
    if (layout.hasAdminNote) {
        // 管理员提示单独占一行，放在内容文本框上方
        const int noteTop = layout.dividerY + ScaleUi(10);
        layout.adminNote = { margin, noteTop, rc.right - margin,
                             noteTop + ScaleUi(20) };
        editTop = layout.adminNote.bottom + ScaleUi(10);
    }
    const int editH = buttonTop - gap - editTop;
    layout.edit = { margin, editTop, rc.right - margin,
                    editTop + (editH > 0 ? editH : 0) };
    layout.cancel = { rc.right - margin - buttonW, buttonTop,
                      rc.right - margin, buttonTop + buttonH };
    layout.ok = { rc.right - margin - buttonW * 2 - ScaleUi(8), buttonTop,
                  rc.right - margin - buttonW - ScaleUi(8), buttonTop + buttonH };
    return layout;
}

void ApplyHotkeyConfirmFonts(HWND hwnd) {
    HFONT ui = Fluent::UiFont();
    HFONT header = Fluent::UiFontSemibold();
    if (HWND h = GetDlgItem(hwnd, kConfirmIdHeader))
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(header), TRUE);
    if (HWND h = GetDlgItem(hwnd, kConfirmIdSubtitle))
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(Fluent::SmallFont()), TRUE);
    if (HWND h = GetDlgItem(hwnd, kConfirmIdAdminNote)) {
        auto* st = reinterpret_cast<HotkeyConfirmState*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (st && !st->adminNoteFont) {
            st->adminNoteFont = Fluent::CreateFontAtScale(12, FW_BOLD, GetUiScale());
        }
        HFONT font = (st && st->adminNoteFont) ? st->adminNoteFont : Fluent::SmallFont();
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    if (HWND h = GetDlgItem(hwnd, kConfirmIdEdit))
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(ui), TRUE);
    if (HWND h = GetDlgItem(hwnd, IDOK))
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(ui), TRUE);
    if (HWND h = GetDlgItem(hwnd, IDCANCEL))
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(ui), TRUE);
}

void LayoutHotkeyConfirm(HWND hwnd) {
    const HotkeyConfirmLayout layout = ComputeHotkeyConfirmLayout(hwnd);
    auto move = [](HWND h, const RECT& r) {
        if (h) {
            MoveWindow(h, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
        }
    };
    move(GetDlgItem(hwnd, kConfirmIdHeader), layout.header);
    move(GetDlgItem(hwnd, kConfirmIdSubtitle), layout.subtitle);
    if (layout.hasAdminNote) {
        move(GetDlgItem(hwnd, kConfirmIdAdminNote), layout.adminNote);
    }
    move(GetDlgItem(hwnd, kConfirmIdEdit), layout.edit);
    move(GetDlgItem(hwnd, IDOK), layout.ok);
    move(GetDlgItem(hwnd, IDCANCEL), layout.cancel);
}

LRESULT CALLBACK HotkeyConfirmWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<HotkeyConfirmState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    case WM_CREATE: {
        if (!st) return -1;
        HWND header = CreateWindowExW(
            0, L"STATIC", Tr(L"确定要执行以下操作？"),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kConfirmIdHeader)),
            g_hInst, nullptr);
        HWND subtitle = CreateWindowExW(
            0, L"STATIC", Tr(L"请核对命令、参数与工作目录，确认无误后再执行。"),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kConfirmIdSubtitle)),
            g_hInst, nullptr);
        HWND adminNote = nullptr;
        if (!st->adminNote.empty()) {
            adminNote = CreateWindowExW(
                0, L"STATIC", st->adminNote.c_str(),
                WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kConfirmIdAdminNote)),
                g_hInst, nullptr);
        }
        HWND edit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", st->body.c_str(),
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
                ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kConfirmIdEdit)),
            g_hInst, nullptr);
        HWND ok = CreateWindowExW(
            0, L"BUTTON", Tr(L"执行"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
            g_hInst, nullptr);
        HWND cancel = CreateWindowExW(
            0, L"BUTTON", Tr(L"取消"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)),
            g_hInst, nullptr);
        if (!header || !subtitle || !edit || !ok || !cancel) return -1;
        if (!st->adminNote.empty() && !adminNote) return -1;

        // 多行编辑框默认有长度上限：确认内容最长可达数万字符，必须放开
        SendMessageW(edit, EM_SETLIMITTEXT, 0, 0);
        // 左右留出内边距，文本不贴着边框
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELONG(ScaleUi(10), ScaleUi(10)));
        SendMessageW(edit, EM_SETSEL, 0, 0);
        Fluent::DecorateDialog(hwnd);
        ApplyHotkeyConfirmFonts(hwnd);
        LayoutHotkeyConfirm(hwnd);
        // 默认焦点放在“取消”：确认框用于执行命令，误触回车不应直接执行
        SetFocus(cancel);
        return 0;
    }
    case WM_SIZE:
        LayoutHotkeyConfirm(hwnd);
        return 0;
    case WM_ERASEBKGND:
        // 与设置界面一致：浅色/深色下使用主题内容面，高对比度交还系统
        if (Fluent::IsHighContrast()) break;
        {
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, Fluent::SurfaceBrush());
        }
        return 1;
    case WM_PAINT: {
        // 高对比度下交给系统绘制，不使用主题色
        if (Fluent::IsHighContrast()) break;
        PAINTSTRUCT ps = {};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, Fluent::SurfaceBrush());

        const HotkeyConfirmLayout layout = ComputeHotkeyConfirmLayout(hwnd);
        HICON icon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APP));
        if (icon) {
            DrawIconEx(hdc, layout.icon.left, layout.icon.top, icon,
                       layout.icon.right - layout.icon.left,
                       layout.icon.bottom - layout.icon.top, 0, nullptr, DI_NORMAL);
        }

        // 标题与内容之间的细分隔线，弱化层次但保持轻量
        HGDIOBJ oldPen = SelectObject(hdc, Fluent::GridPen());
        MoveToEx(hdc, layout.dividerLeft, layout.dividerY, nullptr);
        LineTo(hdc, layout.dividerRight, layout.dividerY);
        SelectObject(hdc, oldPen);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        if (Fluent::IsHighContrast()) break;
        {
            HDC hdc = reinterpret_cast<HDC>(wp);
            HWND ctl = reinterpret_cast<HWND>(lp);
            const int ctlId = ctl ? GetDlgCtrlID(ctl) : 0;
            COLORREF color = Fluent::TextColor();
            if (ctlId == kConfirmIdSubtitle) color = Fluent::TextDisabledColor();
            else if (ctlId == kConfirmIdAdminNote) color = Fluent::LinkColor();
            SetBkColor(hdc, Fluent::SurfaceColor());
            SetTextColor(hdc, color);
        }
        return reinterpret_cast<LRESULT>(Fluent::SurfaceBrush());
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            if (st) st->confirmed = true;
            DestroyWindow(hwnd);
            return 0;
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DPICHANGED: {
        // 跨显示器拖动：按新 DPI 重建字体与布局，与本程序其他窗口保持一致
        const UINT dpi = HIWORD(wp);
        RECT work = {};
        DpiUtils::GetWorkAreaForWindow(hwnd, &work);
        UiScaleOverrideStorage() = DpiUtils::ScaleForDpi(dpi, &work);
        Fluent::RebuildFontsForDpi();
        if (st && st->adminNoteFont) {
            DeleteObject(st->adminNoteFont);
            st->adminNoteFont = nullptr;
        }
        ApplyHotkeyConfirmFonts(hwnd);
        const RECT* suggested = reinterpret_cast<const RECT*>(lp);
        if (suggested) {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        LayoutHotkeyConfirm(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_NCDESTROY:
        if (st && st->adminNoteFont) {
            DeleteObject(st->adminNoteFont);
            st->adminNoteFont = nullptr;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 返回 false 表示对话框无法创建：调用方必须按拒绝执行处理
bool ShowHotkeyConfirmDialog(HWND owner, const std::wstring& body,
                             const std::wstring& adminNote) {
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = HotkeyConfirmWndProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // 高对比度下由系统擦除背景；普通模式在 WM_ERASEBKGND 中使用主题内容面
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = kHotkeyConfirmClass;
        if (!RegisterClassExW(&wc)) return false;
        classRegistered = true;
    }

    RECT work = {};
    if (!DpiUtils::GetWorkAreaForWindow(owner, &work)) {
        work.right = GetSystemMetrics(SM_CXSCREEN);
        work.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    const UINT dpi = owner ? DpiUtils::GetDpiForWindowSafe(owner)
                           : DpiUtils::GetSystemDpi();
    ScopedUiScale scopedScale(DpiUtils::ScaleForDpi(dpi, &work));

    int width = ScaleUi(560);
    int height = ScaleUi(380);
    const int maxW = work.right - work.left - ScaleUi(40);
    const int maxH = work.bottom - work.top - ScaleUi(40);
    if (maxW > 0) width = std::min(width, maxW);
    if (maxH > 0) height = std::min(height, maxH);
    width = std::max(width, ScaleUi(320));
    height = std::max(height, ScaleUi(240));

    HotkeyConfirmState st;
    st.body = body;
    st.adminNote = adminNote;
    const int x = work.left + (work.right - work.left - width) / 2;
    const int y = work.top + (work.bottom - work.top - height) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        kHotkeyConfirmClass, Tr(L"确认"),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, width, height, owner, nullptr, g_hInst, &st);
    if (!hwnd) return false;

    Fluent::ApplyWindowChrome(hwnd);
    const BOOL ownerWasEnabled = owner ? IsWindowEnabled(owner) : FALSE;
    if (owner) EnableWindow(owner, FALSE);

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    UpdateWindow(hwnd);
    // ShowWindow 之后重新设置默认焦点，保证 Esc/回车按“取消”语义处理
    if (HWND cancel = GetDlgItem(hwnd, IDCANCEL)) SetFocus(cancel);

    MSG msg;
    while (IsWindow(hwnd)) {
        const BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
        if (ret <= 0) {
            if (ret == 0) PostQuitMessage((int)msg.wParam);
            break;
        }
        // 确认期间丢弃热键：避免叠出第二个确认框或把后台窗口隐藏
        if (msg.message == WM_HOTKEY) continue;
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_ESCAPE) {
                DestroyWindow(hwnd);
                continue;
            }
            if (msg.wParam == VK_RETURN) {
                // 只有焦点在“执行”上时回车才确认；默认焦点是“取消”
                if (GetFocus() == GetDlgItem(hwnd, IDOK)) st.confirmed = true;
                DestroyWindow(hwnd);
                continue;
            }
            if (msg.wParam == VK_TAB) {
                const bool back = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                HWND next = GetNextDlgTabItem(hwnd, GetFocus(), back);
                if (next) SetFocus(next);
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // GetMessage 异常返回时窗口可能仍存在：销毁后再返回，避免栈上的 st 悬垂
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    if (owner && IsWindow(owner) && ownerWasEnabled) EnableWindow(owner, TRUE);
    return st.confirmed;
}

}  // namespace

// ===== 执行前确认 =====
// 功能：弹出自定义热键的执行前确认框，返回用户是否确认
// 说明：弹框期间抑制其他热键/鼠标触发，避免嵌套确认框或窗口被隐藏
static bool ConfirmCustomHotkey(const std::wstring& body,
                                const std::wstring& adminNote) {
    const bool prevSuppress = g_suppressHotkeys;
    g_suppressHotkeys = true;
    g_modalHotkeyConfirm = true;
    HWND owner = nullptr;
    if (g_settingsWnd && IsWindow(g_settingsWnd) && IsWindowVisible(g_settingsWnd)) {
        owner = g_settingsWnd;
    }
    // 对话框创建失败必须按“取消”处理（fail-closed），不能退化为无确认执行
    const bool confirmed = ShowHotkeyConfirmDialog(owner, body, adminNote);
    g_modalHotkeyConfirm = false;
    g_suppressHotkeys = prevSuppress;
    return confirmed;
}

// 记录配置文件最后写入时间与大小：程序自身每次成功保存后刷新，
// 退出时据此判断文件是否被外部程序改过
static void RefreshConfigStamp() {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (GetFileAttributesExW(g_iniPath.c_str(), GetFileExInfoStandard, &data)) {
        g_configStamp = data.ftLastWriteTime;
        g_configStampSize = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        g_configStampValid = true;
    } else {
        g_configStampValid = false;
        g_configStampSize = 0;
    }
}

// 配置文件在载入后是否被外部修改：时间戳变新、大小变化，
// 或载入时不存在而运行期间被创建。
// 大小比较用于覆盖“时间戳未变但内容已变”的外部写入（见 g_configStampSize 注释）
static bool ConfigChangedExternally() {
    if (g_configStampValid) {
        WIN32_FILE_ATTRIBUTE_DATA data = {};
        // 文件被删除/暂时不可读：保留原有语义，不视为需要冲突提示的修改
        if (!GetFileAttributesExW(g_iniPath.c_str(), GetFileExInfoStandard, &data)) return false;
        const ULONGLONG size = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        return CompareFileTime(&data.ftLastWriteTime, &g_configStamp) > 0 ||
               size != g_configStampSize;
    }
    return ProcessUtils::FileExistsNonEmpty(g_iniPath);
}

// 保存配置并刷新时间戳：所有运行期保存共用，避免外部编辑检测误报
static bool SaveConfigAndRefreshStamp() {
    const bool ok = g_config.Save(g_iniPath);
    if (ok) RefreshConfigStamp();
    return ok;
}

// 正常退出路径：先检测外部编辑冲突，再保存；失败时明确提示一次。
// 关机/注销路径不允许弹窗，由调用方写调试输出即可。
static void SaveConfigOnExit() {
    // 主窗口在 WM_DESTROY 后已销毁，不能再作为 MessageBox owner
    // （无效 owner 会让 MessageBox 直接失败，冲突提示/失败提示都弹不出来）
    HWND owner = (g_mainWnd && IsWindow(g_mainWnd)) ? g_mainWnd : nullptr;
    // 启动时已判定配置文件不可读：本次运行不会写回（LoadFailed 时 Save 拒绝覆盖），
    // 退出时再调用保存只会得到与真实原因无关的“目录只读/磁盘已满”提示，
    // 与启动提示“本次不会自动保存”矛盾。这里直接跳过保存与弹窗。
    if (g_config.LoadFailed()) {
        OutputDebugStringW((L"[KeySentry] 配置读取失败，退出时跳过保存: " + g_iniPath + L"\n").c_str());
        return;
    }
    const bool externalChanged = ConfigChangedExternally();
    if (externalChanged) {
        // 固定标签逐段翻译，配置路径原样拼接后走 raw 显示：
        // 安装路径可能包含词典词，整段翻译会把路径改写（R13-A1）
        std::wstring msg =
            Tr(L"配置文件已被其他程序修改：\n") + g_iniPath +
            Tr(L"\n\n是否用当前设置覆盖外部修改？\n") +
            Tr(L"选择“否”将保留外部修改，本次运行中的设置不会写入。");
        if (Fluent::ShowMessageBoxRaw(owner, msg.c_str(), Tr(L"配置冲突"),
                                      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
            return;
        }
    }
    if (!SaveConfigAndRefreshStamp()) {
        OutputDebugStringW((L"[KeySentry] 退出时配置保存失败: " + g_iniPath + L"\n").c_str());
        std::wstring text = Tr(L"配置无法写入配置文件：\n") + g_iniPath +
                            Tr(L"\n\n部分设置可能无法在下次启动时恢复。\n") +
                            Tr(L"请检查该目录是否只读、磁盘是否已满。");
        Fluent::ShowMessageBoxRaw(owner, text.c_str(), Tr(L"配置保存失败"),
                                  MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }
}

// 配置读取失败的启动提示：区分编码问题与权限/占用，
// 避免用户面对笼统的"读取失败"无从下手（R11-M3）
static std::wstring ConfigLoadFailureText() {
    if (g_config.LoadFailedEncoding()) {
        return L"配置文件编码不是 UTF-16LE（常见于记事本另存为 UTF-8/ANSI）。\n"
               L"本次以默认设置运行且不会覆盖原文件；"
               L"请将 KeySentry.ini 另存为“UTF-16 LE”后重启程序。";
    }
    return L"配置文件读取失败，本次以默认设置运行；为避免覆盖原文件，本次不会自动保存。";
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
std::wstring PreCheckHotkeyConflicts(const AppConfig& newConfig, bool occupancyOnly) {
    std::wstring conflicts;
    s_conflictLinesTruncated = false;  // 每次预检独立统计，避免上次结果影响本次提示

    // occupancyOnly 用于“打开设置时”的提醒：只检查是否被其他程序占用，
    // 不检查逻辑冲突与危险快捷键，避免每次进入设置都弹出大量建议（R16）
    if (!occupancyOnly) {
    // --- 第一阶段：逻辑冲突检查 ---

    // 检查窗口隐藏热键与一键关闭热键是否相同
    // "已配置"的判定必须与 ApplyConfig 一致（mod/vk 缺一不可）：否则会给
    // 根本不会被注册的条目报冲突
    if (newConfig.bossKeyEnabled && newConfig.bossKeyMod != 0 && newConfig.bossKeyVK != 0 &&
        newConfig.bossKeyCloseMod != 0 && newConfig.bossKeyCloseVK != 0) {
        if (newConfig.bossKeyMod == newConfig.bossKeyCloseMod && newConfig.bossKeyVK == newConfig.bossKeyCloseVK) {
            AddHotkeyConflict(conflicts, L"窗口隐藏与一键关闭热键冲突: ", newConfig.bossKeyMod, newConfig.bossKeyVK, L"");
        }
    }

    // 检查自定义热键与窗口隐藏热键冲突
    if (newConfig.bossKeyEnabled && newConfig.bossKeyMod != 0 && newConfig.bossKeyVK != 0 &&
        newConfig.customHotkeysEnabled) {
        for (const auto& hk : newConfig.customHotkeys) {
            if (hk.mod != 0 && hk.vk != 0 && hk.mod == newConfig.bossKeyMod && hk.vk == newConfig.bossKeyVK) {
                AddHotkeyConflict(conflicts, L"自定义热键与窗口隐藏冲突: ", hk.mod, hk.vk, L"");
            }
        }
    }

    // 检查自定义热键与一键关闭热键冲突
    if (newConfig.bossKeyCloseMod != 0 && newConfig.bossKeyCloseVK != 0 && newConfig.customHotkeysEnabled) {
        for (const auto& hk : newConfig.customHotkeys) {
            if (hk.mod != 0 && hk.vk != 0 && hk.mod == newConfig.bossKeyCloseMod && hk.vk == newConfig.bossKeyCloseVK) {
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
                if (a.mod != 0 && a.vk != 0 && a.mod == b.mod && a.vk == b.vk) {
                    AddHotkeyConflict(conflicts, L"自定义热键互相冲突: ", a.mod, a.vk, L"");
                }
            }
        }
    }

    // --- 第二阶段：危险快捷键冲突检查 ---

    // 检查窗口隐藏热键是否与常用快捷键冲突
    if (newConfig.bossKeyEnabled && newConfig.bossKeyMod != 0 && newConfig.bossKeyVK != 0) {
        std::wstring danger = CheckDangerousShortcut(newConfig.bossKeyMod, newConfig.bossKeyVK);
        if (!danger.empty()) {
            AddHotkeyConflict(conflicts, L"窗口隐藏可能与常用快捷键冲突: ", newConfig.bossKeyMod, newConfig.bossKeyVK, danger);
        }
    }

    // 检查一键关闭热键是否与常用快捷键冲突
    if (newConfig.bossKeyCloseMod != 0 && newConfig.bossKeyCloseVK != 0) {
        std::wstring danger = CheckDangerousShortcut(newConfig.bossKeyCloseMod, newConfig.bossKeyCloseVK);
        if (!danger.empty()) {
            AddHotkeyConflict(conflicts, L"一键关闭热键可能与常用快捷键冲突: ", newConfig.bossKeyCloseMod, newConfig.bossKeyCloseVK, danger);
        }
    }

    // 检查自定义热键是否与常用快捷键冲突
    if (newConfig.customHotkeysEnabled) {
        for (const auto& hk : newConfig.customHotkeys) {
            if (hk.mod != 0 && hk.vk != 0) {
                std::wstring danger = CheckDangerousShortcut(hk.mod, hk.vk);
                if (!danger.empty()) {
                    std::wstring name = danger;
                    if (!hk.DisplayName().empty()) name += L" [" + ShortenForMessage(hk.DisplayName()) + L"]";
                    AddHotkeyConflict(conflicts, L"自定义热键可能与常用快捷键冲突: ", hk.mod, hk.vk, name);
                }
            }
        }
    }

    }  // !occupancyOnly

    // --- 第三阶段：热键注册占用检测 ---

    if (occupancyOnly) {
        // 打开设置时的占用检测必须是非破坏性的：不注销任何已生效热键，
        // 否则用户关闭设置但未点“应用”时，热键会一直失效，直到再次应用。
        // 已由当前配置注册的组合直接视为可用；其余组合用临时 ID 试注册。
        auto isActive = [&](UINT mod, UINT vk) {
            if (g_config.bossKeyEnabled && g_config.bossKeyMod == mod &&
                g_config.bossKeyVK == vk) return true;
            if (g_config.bossKeyCloseMod == mod && g_config.bossKeyCloseVK == vk) return true;
            if (g_config.customHotkeysEnabled) {
                for (const auto& hk : g_config.customHotkeys) {
                    if (hk.mod == mod && hk.vk == vk) return true;
                }
            }
            return false;
        };
        int nextTempId = 0x9000;
        auto occupied = [&](UINT mod, UINT vk) -> bool {
            if (mod == 0 || vk == 0 || isActive(mod, vk)) return false;
            const int id = nextTempId++;
            if (RegisterHotKey(g_mainWnd, id, mod | MOD_NOREPEAT, vk) ||
                RegisterHotKey(g_mainWnd, id, mod, vk)) {
                UnregisterHotKey(g_mainWnd, id);
                return false;
            }
            return true;
        };

        if (newConfig.bossKeyEnabled && newConfig.bossKeyMod != 0 &&
            newConfig.bossKeyVK != 0 &&
            occupied(newConfig.bossKeyMod, newConfig.bossKeyVK)) {
            AddHotkeyConflict(conflicts, L"窗口隐藏被其他程序占用: ",
                              newConfig.bossKeyMod, newConfig.bossKeyVK, L"");
        }
        if (newConfig.bossKeyCloseMod != 0 && newConfig.bossKeyCloseVK != 0 &&
            occupied(newConfig.bossKeyCloseMod, newConfig.bossKeyCloseVK)) {
            AddHotkeyConflict(conflicts, L"一键关闭热键被其他程序占用: ",
                              newConfig.bossKeyCloseMod, newConfig.bossKeyCloseVK, L"");
        }
        if (newConfig.customHotkeysEnabled) {
            for (const auto& hk : newConfig.customHotkeys) {
                if (hk.mod == 0 || hk.vk == 0 || !occupied(hk.mod, hk.vk)) continue;
                std::wstring name;
                if (!hk.DisplayName().empty())
                    name = L" [" + ShortenForMessage(hk.DisplayName()) + L"]";
                AddHotkeyConflict(conflicts, L"自定义热键被其他程序占用: ",
                                  hk.mod, hk.vk, name);
            }
        }
        if (s_conflictLinesTruncated) {
            conflicts += std::wstring(Tr(L"（仅显示前 20 条冲突）")) + L"\n";
        }
        return conflicts;
    }

    // 先注销当前所有热键，以便尝试注册新配置的热键
    g_bossKey.Unregister();
    UnregisterHotKey(g_mainWnd, BOSS_CLOSE_HOTKEY_ID);
    for (size_t i = 0; i < MAX_CUSTOM_HOTKEYS; i++) {
        UnregisterHotKey(g_mainWnd, CUSTOM_HOTKEY_ID_START + (int)i);
    }

    // 尝试注册窗口隐藏热键，检测是否被其他程序占用
    // 条件与 ApplyConfig 保持一致（mod/vk 缺一不可）：手改 INI 造成 VKCode=0 时，
    // 用 vk=0 注册必然失败，会被误报为"被其他程序占用"
    if (newConfig.bossKeyEnabled && newConfig.bossKeyMod != 0 && newConfig.bossKeyVK != 0) {
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

    // 尝试注册一键关闭热键（条件与 ApplyConfig 一致，避免 mod/vk 缺一时误报占用）
    if (newConfig.bossKeyCloseMod != 0 && newConfig.bossKeyCloseVK != 0) {
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
            // 条件与 ApplyConfig 一致：mod/vk 缺一即不注册，避免误报"被占用"
            if (hk.mod != 0 && hk.vk != 0) {
                UINT customMod = hk.mod | MOD_NOREPEAT;
                int id = CUSTOM_HOTKEY_ID_START + (int)i;
                if (!RegisterHotKey(g_mainWnd, id, customMod, hk.vk)) {
                    if (!RegisterHotKey(g_mainWnd, id, hk.mod, hk.vk)) {
                        std::wstring name;
                        if (!hk.DisplayName().empty()) name = L" [" + ShortenForMessage(hk.DisplayName()) + L"]";
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

    // 记录恢复阶段是否有热键注册失败，避免恢复静默失败导致已生效热键失效
    bool restoreFailed = false;

    // 与第三阶段/ApplyConfig 保持一致：mod/vk 缺一即视为未配置，否则恢复阶段
    // 注册必然失败，会在每次"应用"时误报"未能重新注册"
    if (g_config.bossKeyEnabled && g_config.bossKeyMod != 0 && g_config.bossKeyVK != 0) {
        UINT bossMod = g_config.bossKeyMod | MOD_NOREPEAT;
        if (!g_bossKey.Register(g_mainWnd, BOSS_HOTKEY_ID, bossMod, g_config.bossKeyVK)) {
            if (!g_bossKey.Register(g_mainWnd, BOSS_HOTKEY_ID, g_config.bossKeyMod, g_config.bossKeyVK)) {
                restoreFailed = true;
            }
        }
    }

    // 与 ApplyConfig 保持一致：仅当修饰键与主键均有效时才视为已配置，
    // 否则 CloseVK==0 时注册必然失败，会误报"未能重新注册"警告
    if (g_config.bossKeyCloseMod != 0 && g_config.bossKeyCloseVK != 0) {
        UINT closeMod = g_config.bossKeyCloseMod | MOD_NOREPEAT;
        if (!RegisterHotKey(g_mainWnd, BOSS_CLOSE_HOTKEY_ID, closeMod, g_config.bossKeyCloseVK)) {
            if (!RegisterHotKey(g_mainWnd, BOSS_CLOSE_HOTKEY_ID, g_config.bossKeyCloseMod, g_config.bossKeyCloseVK)) {
                restoreFailed = true;
            }
        }
    }

    if (g_config.customHotkeysEnabled) {
        for (size_t i = 0; i < g_config.customHotkeys.size() && i < MAX_CUSTOM_HOTKEYS; i++) {
            const auto& hk = g_config.customHotkeys[i];
            // 与预检/ApplyConfig 保持一致：mod/vk 缺一即视为未配置，避免对无效条目注册失败误报
            if (hk.mod != 0 && hk.vk != 0) {
                UINT customMod = hk.mod | MOD_NOREPEAT;
                int id = CUSTOM_HOTKEY_ID_START + (int)i;
                if (!RegisterHotKey(g_mainWnd, id, customMod, hk.vk)) {
                    if (!RegisterHotKey(g_mainWnd, id, hk.mod, hk.vk)) {
                        restoreFailed = true;
                    }
                }
            }
        }
    }

    // 恢复失败时提示用户，避免热键静默失效且用户无感知。
    // 占用检测模式下由调用方统一展示占用列表，避免连续弹两个对话框（R16）
    if (restoreFailed && !occupancyOnly) {
        ShowAppMessage(L"部分已生效的热键在冲突检测后未能重新注册，\n请重启程序或重新应用设置。");
    }

    // 冲突被行数上限截断时明确告知，避免用户误以为只存在已显示的冲突。
    // 固定文案在此翻译，整段消息仍由调用方按 raw 方式显示（用户数据不翻译）
    if (s_conflictLinesTruncated) {
        conflicts += std::wstring(Tr(L"（仅显示前 20 条冲突）")) + L"\n";
    }

    return conflicts;
}

// ===== 热键注册失败提示去重 =====
// ApplyConfig 在每次点击"应用/确定"时都会执行，若某热键被其他程序长期占用，
// 反复弹窗会造成困扰；仅在提示内容发生变化时再次弹窗。
// 全部热键注册成功时会清空该集合（见 ApplyConfig），避免永久静默。
static std::set<std::wstring> g_warnedHotkeyMessages;

// 含用户字段（热键描述/命令）的失败提示：原样展示，禁止片段翻译改写用户数据（R9-M5）。
// 标题仍需按当前语言翻译（R10-A1）
static void WarnHotkeyOnceRaw(const std::wstring& msg) {
    if (!g_warnedHotkeyMessages.insert(msg).second) return;
    ShowAppMessageRaw(msg, MB_OK | MB_ICONWARNING, Tr(APP_NAME));
}

// 创建功能定时器并检查结果：SetTimer 失败时功能会静默不生效，
// 用户却以为已开启；同一功能只提示一次，成功创建后复位（R13-A8）
static void SetFeatureTimer(UINT id, UINT elapse, const wchar_t* failText) {
    static std::set<UINT> warned;
    if (SetTimer(g_mainWnd, id, elapse, nullptr)) {
        warned.erase(id);
        return;
    }
    OutputDebugStringW((std::wstring(L"[KeySentry] 定时器创建失败: ") + failText + L"\n").c_str());
    if (warned.insert(id).second) ShowTrayNotification(failText, true);
}

// ===== ApplyConfig =====
// 功能：将当前全局配置 g_config 应用到各功能模块
// 逻辑：依次配置键盘钩子、悬浮窗、NumLock 守护、老板键、一键关闭热键、
//       自定义热键、鼠标钩子、角落检测、自动隐藏、Win+D 守护、开机自启动等
static void ApplyConfig() {
    // --- 键盘钩子相关配置 ---
    g_hook.SetShowOverlay(g_config.showLockKeyOverlay);            // 是否显示锁定键状态提示
    g_hook.SetShowMuteOverlay(g_config.showMuteOverlay);           // 是否显示静音状态提示
    g_overlay.SetPrimaryOnly(g_config.overlayPrimaryOnly);         // 提示是否仅在主显示器显示
    g_overlay.SetAppearance(g_config.overlayPosition, g_config.overlayTheme,
                            g_config.overlayCorner, g_config.overlayOpacity,
                            g_config.overlayDelay);  // 浮窗位置/主题/边框/透明度/延时
    // 两项提示都关闭时主动收起浮层，否则"保持显示"档位下已显示的浮层会一直留在屏幕上
    // 仅关闭其中一类提示时也要收起对应的已显示浮层：
    // "保持显示"档位下浮层不会自己消失，用户会以为设置未生效
    g_overlay.HideIfDisabled(g_config.showLockKeyOverlay, g_config.showMuteOverlay);
    g_hook.SetDisableSpecifiedKeysEnabled(g_config.disableSpecifiedKeysEnabled);  // 是否启用禁用指定按键
    g_hook.SetDisableNumLock(g_config.disableNumLock);             // 是否禁用 NumLock 切换
    g_hook.SetDisabledKeys(g_config.disabledKeyCodes);             // 设置被禁用的按键列表
    g_hook.SetKeyRemapEnabled(g_config.keyRemapEnabled);           // 是否启用按键重映射
    g_hook.SetKeyRemappings(g_config.keyRemappings);               // 设置按键重映射表

    // 键盘钩子按需安装：所有键相关功能均关闭时卸载钩子，降低常驻开销
    bool needsHook = g_config.showLockKeyOverlay || g_config.showMuteOverlay ||
                     g_config.disableSpecifiedKeysEnabled || g_config.disableNumLock ||
                     g_config.keyRemapEnabled;
    if (needsHook) {
        // 安装失败时仅提示一次；安装成功后复位，允许后续新的失败再次提示
        static bool hookWarned = false;
        if (g_hook.Install(g_mainWnd)) {
            hookWarned = false;
            // 周期探活：钩子可能被系统静默移除，需要自动重装
            SetFeatureTimer(TIMER_HOOK_WATCH, TIMER_HOOK_WATCH_MS,
                            L"键盘钩子探活定时器创建失败，钩子被系统移除后可能无法自动恢复。");
        } else if (!hookWarned) {
            hookWarned = true;
            ShowAppMessage(L"键盘钩子安装失败，部分功能可能不可用。\n请检查是否有安全软件拦截。");
        }
    } else {
        g_hook.Uninstall();
        KillTimer(g_mainWnd, TIMER_HOOK_WATCH);
    }

    // --- NumLock 守护 ---
    // disableNumLock 隐含 forceNumLockOn 的功能（确保 NumLock 开启才能锁定），
    // 因此当两者同时启用时只需在 disableNumLock 分支处理
    // 使用延迟定时器，避免开机自启动时键盘子系统尚未初始化导致 keybd_event 失效
    bool numLockFeatureOn = g_config.disableNumLock || g_config.forceNumLockOn;
    // 本轮功能是否已真正生效：SetTimer 失败时保持 false，
    // 让下一次"应用"仍会重试，而不是永久跳过
    bool numLockFeatureStarted = numLockFeatureOn;
    if (numLockFeatureOn) {
        // 仅在开关从关→开（或启动首次）时武装定时器：否则每次点击"应用"都会在
        // 2 秒后强制开启 NumLock，即使本次修改的是毫不相关的设置
        if (!g_numLockFeatureWasOn && !g_numLockTimerArmed) {
            if (SetTimer(g_mainWnd, TIMER_NUMLOCK_DELAY, TIMER_NUMLOCK_DELAY_MS, nullptr)) {
                g_numLockTimerArmed = true;
            } else {
                numLockFeatureStarted = false;
            }
        }
    } else {
        KillTimer(g_mainWnd, TIMER_NUMLOCK_DELAY);  // 功能关闭时清理可能残留的延迟定时器
        g_numLockTimerArmed = false;
    }
    g_numLockFeatureWasOn = numLockFeatureStarted;

    // --- 老板键和热键注册 ---
    g_bossKey.Unregister();  // 先注销旧的老板键注册
    // 先结算"异步隐藏待确认"：该状态下 m_active 为 false 但窗口可能已被隐藏，
    // 若直接按 IsActive 判断会漏掉恢复，窗口会一直藏着直到下次启动才被恢复文件找回
    g_bossKey.VerifyPendingHide();
    if (!g_config.bossKeyEnabled && g_bossKey.IsActive()) {
        g_bossKey.Deactivate();  // 老板键被禁用时，恢复已隐藏的窗口
    }
    UnregisterHotKey(g_mainWnd, BOSS_CLOSE_HOTKEY_ID);  // 注销一键关闭热键
    for (size_t i = 0; i < MAX_CUSTOM_HOTKEYS; i++) {
        UnregisterHotKey(g_mainWnd, CUSTOM_HOTKEY_ID_START + (int)i);  // 注销所有自定义热键
    }

    // 本轮所有热键是否都注册成功：成功则复位提示状态（冲突解决后再出现仍要提示）
    bool hotkeyAllOk = true;
    // 三类热键的注册失败集中为一条提醒：窗口隐藏、关闭程序、自定义热键，
    // 避免多条托盘气泡互相覆盖导致用户漏看（R16）
    std::vector<std::wstring> failedHotkeyLines;

    // 注册窗口隐藏热键（老板键）
    // 兜底：mod/vk 为 0 时不注册，避免手改 INI 产生的裸键全局劫持普通按键
    if (g_config.bossKeyEnabled && g_config.bossKeyMod != 0 && g_config.bossKeyVK != 0) {
        UINT bossMod = g_config.bossKeyMod | MOD_NOREPEAT;  // 优先带 MOD_NOREPEAT 注册
        if (!g_bossKey.Register(g_mainWnd, BOSS_HOTKEY_ID, bossMod, g_config.bossKeyVK)) {
            if (!g_bossKey.Register(g_mainWnd, BOSS_HOTKEY_ID, g_config.bossKeyMod, g_config.bossKeyVK)) {
                // 启动路径没有热键冲突预检，注册失败必须提示，否则用户会以为热键已生效
                hotkeyAllOk = false;
                failedHotkeyLines.push_back(
                    Tr(L"窗口隐藏热键: ") +
                    FormatHKName(g_config.bossKeyMod, g_config.bossKeyVK));
            }
        }
    }
    // 窗口列表始终同步：老板键还可由鼠标中键/侧键、屏幕角落触发，
    // 即使热键无效（mod/vk 为 0）也必须更新，否则会沿用上一次的窗口列表
    g_bossKey.SetWindows(g_config.bossKeyWindows);

    // 注册一键关闭热键（修饰键与主键缺一不可，裸键会全局劫持普通按键）
    if (g_config.bossKeyCloseMod != 0 && g_config.bossKeyCloseVK != 0) {
        UINT closeMod = g_config.bossKeyCloseMod | MOD_NOREPEAT;
        if (!RegisterHotKey(g_mainWnd, BOSS_CLOSE_HOTKEY_ID, closeMod, g_config.bossKeyCloseVK)) {
            if (!RegisterHotKey(g_mainWnd, BOSS_CLOSE_HOTKEY_ID, g_config.bossKeyCloseMod, g_config.bossKeyCloseVK)) {
                hotkeyAllOk = false;
                failedHotkeyLines.push_back(
                    Tr(L"关闭程序热键: ") +
                    FormatHKName(g_config.bossKeyCloseMod, g_config.bossKeyCloseVK));
            }
        }
    }

    // 注册自定义热键
    // 注册失败的条目聚合为一条提示，避免失效/冲突条目较多时连环弹窗
    if (g_config.customHotkeysEnabled) {
        for (size_t i = 0; i < g_config.customHotkeys.size() && i < MAX_CUSTOM_HOTKEYS; i++) {
            const auto& hk = g_config.customHotkeys[i];
            if (hk.mod == 0 || hk.vk == 0) continue;  // 兜底：非法条目不注册
            UINT customMod = hk.mod | MOD_NOREPEAT;
            BOOL regOk = RegisterHotKey(g_mainWnd, CUSTOM_HOTKEY_ID_START + (int)i, customMod, hk.vk);
            if (!regOk) {
                regOk = RegisterHotKey(g_mainWnd, CUSTOM_HOTKEY_ID_START + (int)i, hk.mod, hk.vk);
                if (!regOk) {
                    std::wstring name = hk.DisplayName();
                    if (name.empty()) name = Tr(L"(未命名热键)");
                    else name = ShortenForMessage(name);
                    failedHotkeyLines.push_back(
                        Tr(L"自定义热键: ") + name + L" (" +
                        FormatHKName(hk.mod, hk.vk) + L")");
                }
            }
        }
    }
    if (!failedHotkeyLines.empty()) {
        hotkeyAllOk = false;
        // 热键名称来自用户填写的描述：固定标签先翻译，名称原样拼接，
        // 整段不走片段翻译，避免用户数据被改写（R9-M5）
        std::wstring msg = Tr(L"以下热键注册失败，可能被其他程序占用：\n");
        size_t showCount = std::min(failedHotkeyLines.size(), (size_t)10);
        for (size_t i = 0; i < showCount; ++i) msg += L"• " + failedHotkeyLines[i] + L"\n";
        if (failedHotkeyLines.size() > showCount) {
            msg += Tr(L"……等共 ") + std::to_wstring(failedHotkeyLines.size()) + Tr(L" 个");
        }
        WarnHotkeyOnceRaw(msg);
    }

    // 全部注册成功：复位提示状态，使同一冲突在解决后又出现时仍会提示，
    // 不会因为"本进程曾经提示过"而永久静默
    if (hotkeyAllOk) g_warnedHotkeyMessages.clear();

    // --- 老板键附加功能配置 ---
    g_bossKey.SetMuteEnabled(g_config.bossKeyMute);                  // 隐藏窗口时是否同时静音
    g_bossKey.SetHideCurrentWindow(g_config.bossKeyHideCurrent);     // 是否隐藏当前焦点窗口
    g_bossKey.SetSendPauseBeforeHide(g_config.bossKeySendPause);     // 隐藏前是否发送 Pause 键暂停游戏

    // --- 鼠标按键触发老板键 ---
    if (g_config.bossKeyEnabled && AnyMouseButtonEnabled()) {
        // 安装失败时提示一次；安装成功后复位，允许后续新的失败再次提示
        static bool mouseHookWarned = false;
        if (g_bossKey.InstallMouseHook(g_mainWnd)) {
            mouseHookWarned = false;
        } else if (!mouseHookWarned) {
            mouseHookWarned = true;
            ShowAppMessage(L"鼠标钩子安装失败，鼠标侧键触发老板键可能不可用。\n请检查是否有安全软件拦截。");
        }
        g_bossKey.UpdateMouseHookSettings(
            g_config.bossKeyMiddleButton,
            g_config.bossKeySideButton1,
            g_config.bossKeySideButton2);
    } else {
        g_bossKey.UninstallMouseHook();  // 卸载鼠标钩子
    }

    // --- 屏幕角落触发老板键 ---
    if (g_config.bossKeyEnabled && AnyCornerEnabled()) {
        SetFeatureTimer(TIMER_CORNER_CHECK, TIMER_CORNER_CHECK_MS,
                        L"屏幕角落检测定时器创建失败，角落触发可能不可用。");
    } else {
        KillTimer(g_mainWnd, TIMER_CORNER_CHECK);
    }

    // --- 自动隐藏（空闲时触发老板键）---
    if (g_config.bossKeyEnabled && g_config.bossKeyAutoHide) {
        SetFeatureTimer(TIMER_AUTO_HIDE, TIMER_AUTO_HIDE_MS,
                        L"闲置自动隐藏定时器创建失败，自动隐藏可能不可用。");
    } else {
        KillTimer(g_mainWnd, TIMER_AUTO_HIDE);
    }

    // --- Win+D 守护 ---
    g_winDGuard.Configure(g_config.winDGuardEnabled, g_config.winDGuardWindows);

    // --- 开机自启动 ---
    // 配置读取失败时 autoStart 只是默认值，不能据此删除用户已有的自启项
    if (!g_config.LoadFailed()) SetAutoStart(g_config.autoStart);

    // --- Win+D 守护定时器 ---
    // 守护关闭后若仍有待释放窗口（之前因目标繁忙未能恢复样式），保留定时器继续收尾，
    // 否则这些第三方窗口会一直保持置顶/无最小化按钮直到下次启动
    if ((g_config.winDGuardEnabled && !g_config.winDGuardWindows.empty()) ||
        g_winDGuard.HasPendingRelease()) {
        SetFeatureTimer(TIMER_WIND_CHECK, TIMER_WIND_CHECK_MS,
                        L"窗口置顶守护定时器创建失败，Win+D 保护可能不可用。");
    } else {
        KillTimer(g_mainWnd, TIMER_WIND_CHECK);
    }
}

// ===== ApplySettingsCallback =====
// 功能：设置对话框中"应用"按钮的回调函数
// 逻辑：保存配置到 INI 文件（原子写入），然后应用新配置
static void ApplySettingsCallback() {
    // 落盘失败必须提示：否则界面显示"已应用"，重启后设置全部丢失。
    // 同一会话只提示一次，避免反复点"应用"时连环弹窗
    static bool saveFailedWarned = false;
    if (!SaveConfigAndRefreshStamp()) {
        if (!saveFailedWarned) {
            saveFailedWarned = true;
            std::wstring text;
            bool pathMessageShown = false;
            // 启动时读取失败会令 Save 直接拒绝写回：必须按真实原因提示，
            // 否则编码问题会被误报成"目录只读/磁盘已满"（R11-M3）
            if (g_config.LoadFailedEncoding()) {
                text = L"配置文件编码不是 UTF-16LE（常见于记事本另存为 UTF-8/ANSI），"
                       L"本次设置无法写入磁盘。\n"
                       L"请将 KeySentry.ini 另存为“UTF-16 LE”后重启程序。\n\n"
                       L"本次设置仅在当前运行期间生效。";
            } else if (g_config.LoadFailed()) {
                text = L"配置文件在启动时读取失败，本次设置无法写入磁盘。\n"
                       L"请检查文件权限或被其他程序占用后重启程序。\n\n"
                       L"本次设置仅在当前运行期间生效。";
            } else {
                // 配置路径原样拼接，固定文案逐段翻译后走 raw 显示，
                // 避免安装路径中的词典词被片段替换（R13-A1）
                text = Tr(L"设置无法写入配置文件：\n") + g_iniPath +
                       Tr(L"\n\n本次设置仅在当前运行期间生效，重启后可能丢失。\n") +
                       Tr(L"请检查该目录是否只读、磁盘是否已满。");
                ShowAppMessageRaw(text, MB_OK | MB_ICONWARNING, Tr(L"配置保存失败"));
                pathMessageShown = true;
            }
            // 编码/权限分支为纯固定文案，交给常规翻译路径
            if (!pathMessageShown) ShowAppMessage(text);
        }
    } else {
        saveFailedWarned = false;
    }
    ApplyConfig();
    ApplyExplorerSettings();
}

// ===== 保存运行期偏好 =====
// 配置读取失败时本次会话按"不可写"处理：启动提示已说明不会自动保存，
// 若这里仍尝试保存并提示失败，会与启动提示矛盾并误导用户排查权限（R8-M7）。
// 仅在当前会话生效并一次性说明，不弹"保存失败"
static void SavePreferenceWithFeedback(const wchar_t* failText) {
    if (g_config.LoadFailed()) {
        static bool notified = false;
        if (!notified) {
            notified = true;
            // 读取失败既可能是权限/占用，也可能是编码不符：统一说明"仅当前会话生效"，
            // 具体原因由启动提示给出，避免这里误报为"不可写"（R11-M3）
            ShowTrayNotification(L"配置文件在启动时读取失败：本次偏好仅在当前会话生效，不会保存。", true);
        }
        return;
    }
    if (!SaveConfigAndRefreshStamp()) {
        ShowTrayNotification(failText, true);
    }
}

// ===== 外观 / 语言偏好变更 =====
// 设置窗口的小图标按钮已把新值写入 g_config，这里负责立即应用并保存；
// 与设置页的「应用」解耦，快捷开关不受「取消」回滚。
static void UiPreferenceChanged() {
    Fluent::SetThemeMode((Fluent::ThemeMode)g_config.appTheme);
    SetAppLanguage((AppLanguage)g_config.appLanguage);
    RefreshThemeAllWindows();
    // 浮层文本在显示时生成，语言切换后立即重绘正在显示的浮层（R9-M6）
    g_overlay.RefreshContent();
    UpdateTrayTip();
    SavePreferenceWithFeedback(L"外观或语言设置保存失败，重启后可能恢复原设置。");
}

// ===== OpenSettings =====
// 功能：打开设置对话框
// 逻辑：若设置对话框已打开则激活现有窗口，否则创建新的设置对话框
static void OpenSettings(HWND hwnd) {
    // 自定义热键确认框打开期间不允许改配置：确认框执行的是打开前的快照，
    // 中途应用新配置会让确认内容与实际执行内容不一致
    if (g_modalHotkeyConfirm) {
        ShowTrayNotification(L"请先处理当前热键确认框，再打开设置。", false);
        return;
    }
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
    SettingsDialog::SetUiPreferenceCallback(UiPreferenceChanged); // 外观/语言快捷开关
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
    // 鼠标是否"重新进入"角落的闩锁：触发过一次后必须离开角落才会再次触发，
    // 否则恢复窗口后只要鼠标仍停在角落，下一次定时器就会立刻再次隐藏
    static bool s_cornerArmed = true;

    // 功能整体未启用（老板键关闭或四个角落都未勾选）时复位闩锁：
    // 否则重新启用后若鼠标恰好停在角落，必须先移开再移回来才会触发
    if (!g_config.bossKeyEnabled ||
        (!g_config.bossKeyCornerTL && !g_config.bossKeyCornerTR &&
         !g_config.bossKeyCornerBL && !g_config.bossKeyCornerBR)) {
        s_cornerArmed = true;
        return;
    }

    // 设置对话框打开或热键捕获取键期间暂停角落检测，避免操作设置时窗口被隐藏
    if (g_settingsOpen || g_suppressHotkeys) {
        s_cornerArmed = true;
        return;
    }

    POINT pt;
    if (!GetCursorPos(&pt)) return;

    // 使用鼠标所在位置的显示器，而非前台窗口的显示器
    // 多显示器环境下，鼠标可能在副屏，前台窗口可能在主屏
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    // 取不到显示器信息时必须跳过本轮检测：失败时 rcWork 全为 0，
    // screenW/screenH 变成 0，下面的"右下角"等判断会恒成立，
    // 导致鼠标在任何位置都被判为在角落并反复触发隐藏
    if (!GetMonitorInfoW(hMon, &mi)) return;
    int screenW = mi.rcWork.right - mi.rcWork.left;
    int screenH = mi.rcWork.bottom - mi.rcWork.top;
    if (screenW <= 0 || screenH <= 0) return;
    int screenLeft = mi.rcWork.left;
    int screenTop = mi.rcWork.top;
    const int threshold = CORNER_THRESHOLD;

    // 判断鼠标是否在四个角落的阈值范围内
    bool inCorner = false;
    if (pt.x - screenLeft <= threshold && pt.y - screenTop <= threshold && g_config.bossKeyCornerTL) inCorner = true;           // 左上角
    else if (pt.x - screenLeft >= screenW - threshold && pt.y - screenTop <= threshold && g_config.bossKeyCornerTR) inCorner = true;  // 右上角
    else if (pt.x - screenLeft <= threshold && pt.y - screenTop >= screenH - threshold && g_config.bossKeyCornerBL) inCorner = true;  // 左下角
    else if (pt.x - screenLeft >= screenW - threshold && pt.y - screenTop >= screenH - threshold && g_config.bossKeyCornerBR) inCorner = true;  // 右下角

    if (!inCorner) {
        s_cornerArmed = true;  // 鼠标已离开角落，允许下次触发
        return;
    }

    // 鼠标一直停留在角落（未离开过）时不再重复触发
    if (!s_cornerArmed) return;

    // 防抖检查：距离上次触发不足 CORNER_DEBOUNCE_MS 毫秒则跳过
    ULONGLONG now = GetTickCount64();
    if (now - g_lastCornerTick < CORNER_DEBOUNCE_MS) return;
    g_lastCornerTick = now;
    s_cornerArmed = false;

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
    // 设置对话框打开/热键捕获期间不自动隐藏，避免用户正在配置时窗口被隐藏
    if (g_settingsOpen || g_suppressHotkeys) return;
    // 未启用自动隐藏或老板键已激活时跳过
    if (!g_config.bossKeyAutoHide || g_bossKey.IsActive()) return;

    // 冷却期检查：上次取消激活后 60 秒内不再自动触发
    ULONGLONG now = GetTickCount64();
    ULONGLONG lastDeactivate = g_bossKey.GetLastDeactivateTick();
    if (lastDeactivate > 0 && now - lastDeactivate < 60000) return;
    // 上次尝试没有可隐藏窗口（如绑定程序未运行）时，60 秒内不再重试，
    // 避免空闲期间每 5 秒全量枚举窗口
    if (g_lastAutoHideTry > 0 && now - g_lastAutoHideTry < 60000) return;

    // 获取系统最后输入时间，计算空闲时长
    LASTINPUTINFO lii = { sizeof(lii) };
    if (!GetLastInputInfo(&lii)) return;

    DWORD currentTick = GetTickCount();
    DWORD idleMs = currentTick - lii.dwTime;
    ULONGLONG thresholdMs = (ULONGLONG)g_config.bossKeyAutoHideTime * 60 * 1000;  // 配置的分钟数转毫秒

    if (idleMs >= thresholdMs) {
        g_lastAutoHideTry = now;
        HandleBossToggle();  // 空闲超时，触发隐藏
    }
}

// ===== AboutWndProc =====
// 功能："关于"对话框的窗口过程
// 逻辑：处理确定/取消按钮关闭窗口、WM_CLOSE 消息、静态控件和按钮的透明背景
struct AboutDpiState {
    HFONT title = nullptr;
    HFONT normal = nullptr;
    HFONT caption = nullptr;
    float scale = 1.0f;
};

static LRESULT CALLBACK AboutWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wp), &rc, Fluent::SurfaceBrush());
        return 1;
    }
    switch (msg) {
    case WM_DPICHANGED: {
        // 拖到不同 DPI 显示器：重建字体并按比例缩放子控件（R13-A11）
        RECT* suggested = reinterpret_cast<RECT*>(lp);
        if (suggested) {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        auto* st = reinterpret_cast<AboutDpiState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (st) {
            RECT work = {};
            DpiUtils::GetWorkAreaForWindow(hwnd, &work);
            const float newScale = DpiUtils::ScaleForDpi(HIWORD(wp), &work);
            const float oldScale = (st->scale > 0.0f) ? st->scale : newScale;
            HFONT newTitle = CreateUiFontAtScale(20, FW_BOLD, newScale);
            HFONT newNormal = CreateUiFontAtScale(14, FW_NORMAL, newScale);
            HFONT newSmall = CreateUiFontAtScale(13, FW_NORMAL, newScale);
            if (newTitle) { if (st->title) DeleteObject(st->title); st->title = newTitle; }
            if (newNormal) { if (st->normal) DeleteObject(st->normal); st->normal = newNormal; }
            if (newSmall) { if (st->caption) DeleteObject(st->caption); st->caption = newSmall; }
            st->scale = newScale;
            UiScaleOverrideStorage() = newScale;

            struct ChildCtx { const AboutDpiState* st; float ratio; };
            ChildCtx childCtx{ st, (oldScale > 0.0f) ? (newScale / oldScale) : 1.0f };
            EnumChildWindows(hwnd, [](HWND child, LPARAM param) -> BOOL {
                auto* c = reinterpret_cast<ChildCtx*>(param);
                RECT rc = {};
                if (GetWindowRect(child, &rc)) {
                    MapWindowPoints(HWND_DESKTOP, GetParent(child),
                                    reinterpret_cast<POINT*>(&rc), 2);
                    const int x = (int)(rc.left * c->ratio + 0.5f);
                    const int y = (int)(rc.top * c->ratio + 0.5f);
                    const int w = (int)((rc.right - rc.left) * c->ratio + 0.5f);
                    const int h = (int)((rc.bottom - rc.top) * c->ratio + 0.5f);
                    SetWindowPos(child, nullptr, x, y, w, h,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
                const LONG_PTR id = GetWindowLongPtrW(child, GWLP_ID);
                HFONT font = nullptr;
                if (id == 1004) font = c->st->title;
                else if (id == 1005 || id == IDOK) font = c->st->normal;
                else if (id == 1001 || id == 1006 || id == 1007) font = c->st->caption;
                if (font) SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
                return TRUE;
            }, reinterpret_cast<LPARAM>(&childCtx));
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    }
    case WM_NCDESTROY: {
        auto* st = reinterpret_cast<AboutDpiState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (st) {
            if (st->title) DeleteObject(st->title);
            if (st->normal) DeleteObject(st->normal);
            if (st->caption) DeleteObject(st->caption);
            delete st;        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            DestroyWindow(hwnd);  // 点击确定或取消，销毁对话框
            return 0;
        }
        // GitHub发布页链接点击（自绘按钮发送 BN_CLICKED，旧静态控件为 STN_CLICKED）
        if ((HIWORD(wp) == BN_CLICKED || HIWORD(wp) == STN_CLICKED) && LOWORD(wp) == 1001) {
            ShellExecuteW(hwnd, L"open", L"https://github.com/dojosoft/KeySentry", nullptr, nullptr, SW_SHOW);
            return 0;
        }
        break;
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (!dis) break;
        // 应用图标：在内容面底色上按 alpha 绘制，四周保持透明
        if (dis->CtlID == 1002) {
            FillRect(dis->hDC, &dis->rcItem, Fluent::SurfaceBrush());
            HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));
            if (icon) {
                const int iconW = GetSystemMetrics(SM_CXICON);
                const int iconH = GetSystemMetrics(SM_CYICON);
                const int x = dis->rcItem.left +
                              (dis->rcItem.right - dis->rcItem.left - iconW) / 2;
                const int y = dis->rcItem.top +
                              (dis->rcItem.bottom - dis->rcItem.top - iconH) / 2;
                DrawIconEx(dis->hDC, x, y, icon, iconW, iconH, 0, nullptr, DI_NORMAL);
            }
            return 0;
        }
        // 分隔线：系统蚀刻线用 3D 明暗双线绘制，深色主题下会显出一条亮边，
        // 改由自绘单像素细线，颜色与 Fluent 边框一致
        if (dis->CtlID == 1003) {
            FillRect(dis->hDC, &dis->rcItem, Fluent::SurfaceBrush());
            const int y = dis->rcItem.top + (dis->rcItem.bottom - dis->rcItem.top) / 2;
            RECT line = { dis->rcItem.left, y, dis->rcItem.right, y + 1 };
            HBRUSH brush = CreateSolidBrush(Fluent::GridLineColor());
            FillRect(dis->hDC, &line, brush);
            DeleteObject(brush);
            return 0;
        }
        // GitHub 链接行：仅绘制链接文字，整行可点击
        if (dis->CtlID != 1001) break;
        HDC hdc = dis->hDC;
        FillRect(hdc, &dis->rcItem, Fluent::SurfaceBrush());

        HFONT font = reinterpret_cast<HFONT>(SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0));
        HGDIOBJ oldFont = SelectObject(hdc, font ? font : Fluent::SmallFont());
        SetBkMode(hdc, TRANSPARENT);

        // 键盘焦点/按下态反馈：链接是自绘按钮，需要自己绘制
        const bool linkPressed = (dis->itemState & ODS_SELECTED) != 0;
        const bool linkFocused = (dis->itemState & ODS_FOCUS) != 0;
        SetTextColor(hdc, linkPressed ? Fluent::TextColor() : Fluent::LinkColor());

        RECT tr = dis->rcItem;
        DrawTextW(hdc, L"GitHub", -1, &tr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (linkFocused) {
            RECT focusRect = dis->rcItem;
            InflateRect(&focusRect, -ScaleUi(1), -ScaleUi(1));
            DrawFocusRect(hdc, &focusRect);
        }
        SelectObject(hdc, oldFont);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case DM_GETDEFID:
        // 非对话框窗口：向 IsDialogMessage 声明默认按钮，Enter 才可关闭
        return MAKELRESULT(IDOK, DC_HASDEFID);
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        HWND ctrl = (HWND)lp;
        SetTextColor(hdc, IsWindowEnabled(ctrl) ? Fluent::TextColor() : Fluent::TextDisabledColor());
        return (LRESULT)Fluent::SurfaceBrush();
    }
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)Fluent::SurfaceBrush();
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
        wc.hbrBackground = nullptr;   // 背景由 WM_ERASEBKGND 按主题绘制
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // 注册失败时不得标记成功：否则窗口类始终未注册，
        // CreateWindowExW 会持续返回空且不再重试（已存在的类视为成功）
        if (!WindowUtils::RegisterClassOnce(wc)) return;
        registered = true;
    }

    // 计算窗口大小和居中位置（按目标显示器 DPI 放大）
    HWND targetForDpi = (parent && IsWindowVisible(parent)) ? parent : nullptr;
    RECT targetWork = {};
    DpiUtils::GetWorkAreaForWindow(targetForDpi, &targetWork);
    UINT targetDpi = 0;
    if (targetForDpi) {
        targetDpi = DpiUtils::GetDpiForWindowSafe(targetForDpi);
    } else {
        POINT pt = { 0, 0 };
        GetCursorPos(&pt);
        targetDpi = DpiUtils::GetDpiForMonitorSafe(
            MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY));
    }
    ScopedUiScale scopedScale(DpiUtils::ScaleForDpi(targetDpi, &targetWork));

    RECT rc = { 0, 0, ScaleUi(360), ScaleUi(240) };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int winX = 0, winY = 0;
    // 在目标显示器工作区内居中，避免关于窗口总是出现在主屏
    WindowUtils::CenterWindowInWorkArea(targetWork, &winX, &winY, &winW, &winH);

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, ABOUT_CLASS,
                                 L"关于 键客",
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                                 winX, winY,  // 居中并夹紧到工作区
                                 winW, winH,
                                 parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    // 窗口创建失败（极端资源耗尽）：直接返回，避免以空句柄创建子控件产生孤儿窗口
    if (!hwnd) return;

    // 字体由窗口持有：WM_DPICHANGED 时重建，WM_NCDESTROY 统一释放
    auto* dpiState = new AboutDpiState();
    dpiState->scale = GetUiScale();
    dpiState->title = CreateUiFont(20, FW_BOLD);    // 标题字体：20px 粗体
    dpiState->normal = CreateUiFont(14, FW_NORMAL); // 正文字体：14px
    dpiState->caption = CreateUiFont(13, FW_NORMAL);  // 小号字体：13px
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dpiState));

    // 顶部图标 + 名称/版本：按"图标 + 间距 + 最宽文字"的组宽整体水平居中，
    // 避免文字较窄时整块偏左
    HDC hdcMeasureTop = GetDC(hwnd);
    HGDIOBJ oldTopFont = SelectObject(hdcMeasureTop, dpiState->title);
    SIZE szTitle = {};
    GetTextExtentPoint32W(hdcMeasureTop, L"键客 KeySentry",
                          (int)wcslen(L"键客 KeySentry"), &szTitle);
    SelectObject(hdcMeasureTop, dpiState->normal);
    SIZE szVer = {};
    GetTextExtentPoint32W(hdcMeasureTop, L"v" KS_VERSION_STRING_W,
                          (int)wcslen(L"v" KS_VERSION_STRING_W), &szVer);
    SelectObject(hdcMeasureTop, oldTopFont);
    ReleaseDC(hwnd, hdcMeasureTop);

    const int iconSize = ScaleUi(48);
    const int iconGap = ScaleUi(14);
    const int textW = (szTitle.cx > szVer.cx ? szTitle.cx : szVer.cx);
    const int groupW = iconSize + iconGap + textW;
    const int groupX = (ScaleUi(360) - groupW) / 2;
    const int textX = groupX + iconSize + iconGap;

    // 应用图标：SS_ICON 静态控件会忽略父窗口的背景刷，深色模式下图标四周
    // 会被系统填成白色方块；改用自绘静态控件，在内容面底色上按 alpha 绘制图标
    CreateWindowExW(0, L"STATIC", L"",
                    WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
                    groupX, ScaleUi(20), iconSize, iconSize,
                    hwnd, (HMENU)1002, GetModuleHandleW(nullptr), nullptr);

    // 标题文字
    HWND titleCtrl = CreateWindowExW(0, L"STATIC", L"键客 KeySentry",
                                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       textX, ScaleUi(20), textW + ScaleUi(4), ScaleUi(32),
                                       hwnd, (HMENU)1004, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(titleCtrl, WM_SETFONT, (WPARAM)dpiState->title, TRUE);

    // 版本号
    HWND verCtrl = CreateWindowExW(0, L"STATIC", L"v" KS_VERSION_STRING_W,
                                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     textX, ScaleUi(55), textW + ScaleUi(4), ScaleUi(24),
                                     hwnd, (HMENU)1005, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(verCtrl, WM_SETFONT, (WPARAM)dpiState->normal, TRUE);

    // 分隔线：系统 SS_ETCHEDHORZ 使用 3D 明暗双线（WinXP 风格），
    // 深色主题下会露出亮边，改用自绘静态控件画单像素细线
    CreateWindowExW(0, L"STATIC", L"",
                    WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
                    ScaleUi(20), ScaleUi(88), ScaleUi(320), ScaleUi(2),
                    hwnd, (HMENU)1003, GetModuleHandleW(nullptr), nullptr);

    // 应用描述（横向居中）
    HWND descCtrl = CreateWindowExW(0, L"STATIC",
                                      L"一款掌控你的键盘和窗口的轻量工具。",
                                      WS_CHILD | WS_VISIBLE | SS_CENTER,
                                      ScaleUi(20), ScaleUi(100), ScaleUi(320), ScaleUi(24),
                                      hwnd, (HMENU)1006, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(descCtrl, WM_SETFONT, (WPARAM)dpiState->caption, TRUE);

    // 版权信息独立一行，文字居中
    const wchar_t* copyText = L"© 2026 Marvin 翁敏峰";
    const wchar_t* gitText = L"GitHub";
    HDC hdcMeasure = GetDC(hwnd);
    HGDIOBJ oldFont = SelectObject(hdcMeasure, dpiState->caption);
    SIZE szCopy = {}, szGit = {};
    GetTextExtentPoint32W(hdcMeasure, copyText, (int)wcslen(copyText), &szCopy);
    GetTextExtentPoint32W(hdcMeasure, gitText, (int)wcslen(gitText), &szGit);
    SelectObject(hdcMeasure, oldFont);
    ReleaseDC(hwnd, hdcMeasure);

    HWND copyCtrl = CreateWindowExW(0, L"STATIC", copyText,
                                      WS_CHILD | WS_VISIBLE | SS_CENTER,
                                      ScaleUi(20), ScaleUi(122), ScaleUi(320), ScaleUi(24),
                                      hwnd, (HMENU)1007, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(copyCtrl, WM_SETFONT, (WPARAM)dpiState->caption, TRUE);

    // GitHub 链接：独立一行并居中（仅文字，无图标）
    {
        const int linkW = szGit.cx + ScaleUi(12);
        HWND githubCtrl = CreateWindowExW(0, L"BUTTON", gitText,
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                          (ScaleUi(360) - linkW) / 2, ScaleUi(150),
                                          linkW, ScaleUi(24),
                                          hwnd, (HMENU)1001, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(githubCtrl, WM_SETFONT, (WPARAM)dpiState->caption, TRUE);
    }

    // 确定按钮：距底边 12px，符合 Windows 常规对话框的底部留白
    HWND okBtn = CreateWindowExW(0, L"BUTTON", L"确定",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
                                   ScaleUi(140), ScaleUi(196), ScaleUi(80), ScaleUi(32),
                                   hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(okBtn, WM_SETFONT, (WPARAM)dpiState->normal, TRUE);

    // Fluent 外观：Win11 窗口圆角 + 强调色主按钮
    Fluent::ApplyWindowChrome(hwnd);
    Fluent::DecorateButton(okBtn, Fluent::ButtonKind::Primary);

    // 英文模式下翻译关于窗口内的标题、描述与按钮
    ApplyLanguageToWindowTree(hwnd);

    // 进入模态消息循环，阻塞直到对话框关闭
    {
        ModalGuard guard(parent);
        // 与设置对话框一致：暂停自动隐藏与角落检测，避免用户在"关于"框
        // 停留时后台窗口被老板键藏起来。需保存并还原原值——从设置对话框
        // 内点"关于"时该标志本来就已经是 true
        bool prevSettingsOpen = g_settingsOpen;
        g_settingsOpen = true;
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        RunModalLoop(hwnd);
        g_settingsOpen = prevSettingsOpen;
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
    // 托盘菜单同样受设置窗口状态约束：设置/关于的模态循环会把发给主窗口的
    // 命令正常派发，若不拦截，配置过程中可通过托盘隐藏窗口或直接退出，
    // 造成未应用的修改被丢弃
    case WM_COMMAND:
        // 热键确认框的模态循环仍会把托盘命令派发到主窗口。除“设置/退出”外，
        // 其余命令一律忽略，避免在确认框返回前执行隐藏/主题切换等操作。
        // 退出是用户明确的终止意图，任何模态状态下都应允许。
        if (g_modalHotkeyConfirm && LOWORD(wp) != IDM_TRAY_SHOW &&
            LOWORD(wp) != IDM_TRAY_EXIT) return 0;
        switch (LOWORD(wp)) {
        case IDM_TRAY_SHOW:
            OpenSettings(hwnd);      // "设置"菜单项
            return 0;
        case IDM_TRAY_RESTORE:
            if (g_settingsOpen) return 0;
            // 菜单项语义是"恢复"而非"切换"：菜单打开期间状态可能被热键/鼠标改变，
            // 再按切换处理会执行相反操作（把刚恢复的窗口重新隐藏）（R12-M2）
            g_bossKey.VerifyPendingHide();
            if (g_bossKey.IsActive()) {
                g_hook.SetSimulatingInput(true);
                g_bossKey.Deactivate();
                g_hook.SetSimulatingInput(false);
            }
            return 0;
        case IDM_TRAY_HIDE:
            // 老板键已禁用时不再响应托盘"隐藏"（菜单本不应出现，防止状态变化后的旧菜单）
            if (g_settingsOpen || !g_config.bossKeyEnabled) return 0;
            // 同样按显式语义处理：仅在当前未激活时执行隐藏（R12-M2）
            g_bossKey.VerifyPendingHide();
            if (!g_bossKey.IsActive()) {
                g_hook.SetSimulatingInput(true);
                g_bossKey.Activate();
                g_hook.SetSimulatingInput(false);
            }
            return 0;
        case IDM_TRAY_ABOUT:
            if (g_settingsOpen) return 0;
            ShowAboutDialog(hwnd);   // "关于"菜单项
            return 0;
        case IDM_THEME_SYSTEM:
        case IDM_THEME_LIGHT:
        case IDM_THEME_DARK: {
            // 设置窗口打开时其工作副本会在应用/取消时覆盖主题，统一交给设置页处理
            if (g_settingsOpen) return 0;
            const int mode = (LOWORD(wp) == IDM_THEME_SYSTEM) ? 0
                           : (LOWORD(wp) == IDM_THEME_LIGHT) ? 1 : 2;
            if (g_config.appTheme != mode) {
                g_config.appTheme = mode;
                SavePreferenceWithFeedback(L"主题已切换，但配置保存失败，重启后可能恢复原主题。");
                Fluent::SetThemeMode((Fluent::ThemeMode)mode);
                RefreshThemeAllWindows();
            }
            return 0;
        }
        case IDM_LANG_ZH:
        case IDM_LANG_EN: {
            // 设置窗口打开时其工作副本会在应用/取消时覆盖语言，统一交给设置页处理
            if (g_settingsOpen) return 0;
            const int lang = (LOWORD(wp) == IDM_LANG_EN) ? 1 : 0;
            if (g_config.appLanguage != lang) {
                g_config.appLanguage = lang;
                SetAppLanguage((AppLanguage)lang);
                // 与设置页快捷开关一致：立即重绘正在显示的浮层（R9-M6）
                g_overlay.RefreshContent();
                UpdateTrayTip();
                SavePreferenceWithFeedback(L"语言已切换，但配置保存失败，重启后可能恢复原语言。");
            }
            return 0;
        }
        case IDM_TRAY_EXIT: {
            // 退出是明确的终止意图：即使设置窗口处于模态状态也直接退出。
            // DestroyWindow 会走 WM_DESTROY 统一清理并发送 WM_QUIT，
            // 设置的模态循环收到退出消息后会自行销毁窗口并返回。
            DestroyWindow(hwnd);     // "退出"菜单项：销毁主窗口触发退出
            return 0;
        }
        }
        break;

    // --- 映射按键抬起补发失败通知 ---
    case WM_KEY_REMAP_RELEASE_FAILED:
        ShowTrayNotification(L"按键映射的抬起事件发送失败，可能有按键保持按下；请检查目标窗口权限或安全软件拦截。", true);
        return 0;

    // --- 锁定键状态通知 ---
    // 由键盘钩子发送，wp 为虚拟键码，lp 为状态：0=关闭, 1=开启, 3=已锁定且开启
    case WM_LOCKKEY_NOTIFY: {
        int vkCode = (int)wp;
        bool locked = (lp == 3);
        bool state = lp != 0;
        g_overlay.Show(vkCode, state, locked);  // 显示锁定键状态悬浮提示
        return 0;
    }

    // --- 静音状态通知 ---
    case WM_MUTE_NOTIFY: {
        // 键盘钩子在系统应用静音切换之前触发，此刻查询到的仍是切换前的旧状态；
        // 先算出切换后的目标状态，再轮询等待系统真正生效后再显示，避免浮层与系统不一致
        bool preMuted = false;
        int preVolume = 0;
        // 查询失败（无音频输出设备等）时不启动轮询：默认值会推出与实际相反的
        // 目标状态，浮层内容必然错误
        if (!g_overlay.GetAudioState(preMuted, preVolume)) return 0;
        g_muteExpectState = !preMuted;
        g_muteRefreshTries = 0;
        SetTimer(g_mainWnd, TIMER_MUTE_REFRESH, TIMER_MUTE_REFRESH_MS, nullptr);
        return 0;
    }

    // --- 定时器回调 ---
    case WM_TIMER:
        if (wp == TIMER_WIND_CHECK) {
            g_winDGuard.CheckNewWindows();  // Win+D 守护：检查并恢复被最小化的受保护窗口
            // 守护已关闭且待释放窗口已全部恢复：停止只为收尾保留的定时器
            if (!g_config.winDGuardEnabled && !g_winDGuard.HasPendingRelease()) {
                KillTimer(g_mainWnd, TIMER_WIND_CHECK);
            }
        } else if (wp == TIMER_CORNER_CHECK) {
            CheckCornerHide();  // 角落检测：检查鼠标是否在屏幕角落
        } else if (wp == TIMER_AUTO_HIDE) {
            CheckAutoHide();  // 自动隐藏：检查用户空闲时间是否超时
        } else if (wp == TIMER_NUMLOCK_DELAY) {
            KillTimer(g_mainWnd, TIMER_NUMLOCK_DELAY);  // 一次性定时器，执行后立即销毁
            g_numLockTimerArmed = false;                // 允许下次启用时重新启动定时器
            // 启动时仅强制开启 NumLock，不弹锁定键浮窗：
            // 这是程序自动初始化行为，用户并未按下 NumLock，无需状态提示
            ForceNumLockOn();
        } else if (wp == TIMER_BOSSKEY_VERIFY) {
            // 老板键异步隐藏的延迟确认（一次性）：迟到的隐藏生效后重新进入激活态
            KillTimer(g_mainWnd, TIMER_BOSSKEY_VERIFY);
            g_bossKey.VerifyPendingHide();
        } else if (wp == TIMER_MUTE_RESTORE_RETRY) {
            // 启动静音恢复的静默重试：等待音频服务/默认端点就绪后自动解除静音。
            // 老板键激活期间不能解除静音，等恢复窗口后再继续
            if (g_bossKey.IsActive()) return 0;
            ++g_muteRestoreRetries;
            const bool done = g_bossKey.RetryPendingMuteRestore();
            if (done || g_muteRestoreRetries >= MUTE_RESTORE_RETRY_MAX) {
                KillTimer(g_mainWnd, TIMER_MUTE_RESTORE_RETRY);
            }
        } else if (wp == TIMER_MUTE_REFRESH) {
            // 轮询等待系统完成静音切换：状态达到目标值（或超时）后按真实状态显示浮层
            bool curMuted = false;
            int curVolume = 0;
            bool ok = g_overlay.GetAudioState(curMuted, curVolume);
            ++g_muteRefreshTries;
            // 查询失败（设备切换等瞬时状态）时继续重试；达到上限后直接放弃本次提示，
            // 绝不按默认值显示错误的静音/音量状态
            if (g_muteRefreshTries >= MUTE_REFRESH_MAX_TRIES) {
                KillTimer(g_mainWnd, TIMER_MUTE_REFRESH);
                if (ok) g_overlay.ShowMute();
            } else if (ok && curMuted == g_muteExpectState) {
                KillTimer(g_mainWnd, TIMER_MUTE_REFRESH);
                g_overlay.ShowMute();
            }
        } else if (wp == TIMER_HOOK_WATCH) {
            g_hook.EnsureAlive();
        } else {
            break;  // 未知定时器，交给 DefWindowProcW 处理
        }
        return 0;

    // --- 热键触发 ---
    case WM_HOTKEY:
        // 捕获/热键确认框打开期间丢弃热键：确认框的模态循环仍会把 WM_HOTKEY
        // 派发到主窗口，不拦截会叠出第二个确认框，或把后台窗口隐藏
        if (g_suppressHotkeys) return 0;
        // 自身"模拟按键"注入的组合会被系统再次匹配到热键：忽略它，
        // 否则模拟目标等于本程序热键时会被再次执行（相同组合无限循环）。
        // 只吞注入产生的一条消息，随后立即恢复正常响应
        if (IsSelfSimulatedHotkey(lp)) {
            g_simHotkeyUntil = 0;
            return 0;
        }
        if (wp == BOSS_HOTKEY_ID) {
            // 设置对话框打开时不响应老板键，避免配置过程中窗口被隐藏
            if (g_settingsOpen) return 0;
            // 老板键可能已被禁用/改键：队列中的旧消息不得再触发隐藏（R12-M1）
            if (!g_config.bossKeyEnabled ||
                !HotkeyMatchesConfig(g_config.bossKeyMod, g_config.bossKeyVK, lp)) return 0;
            // 老板键触发：切换窗口隐藏/恢复
            HandleBossToggle();
        } else if (wp == BOSS_CLOSE_HOTKEY_ID) {
            // 一键关闭热键是用户明确的终止意图：设置窗口打开时也直接退出，
            // 由主窗口销毁流程统一清理并结束设置模态循环
            // 一键关闭热键可能已被改键/清空：仅执行当前配置对应的组合（R12-M1）
            if (!HotkeyMatchesConfig(g_config.bossKeyCloseMod, g_config.bossKeyCloseVK, lp)) return 0;
            // 一键关闭热键触发：销毁主窗口退出程序
            DestroyWindow(hwnd);
        } else if (wp >= CUSTOM_HOTKEY_ID_START && wp < CUSTOM_HOTKEY_ID_START + MAX_CUSTOM_HOTKEYS) {
            // 设置对话框打开时不响应自定义热键：与老板键/一键关闭保持一致。
            // 用户在设置界面里按到自定义热键时不应启动程序或模拟按键，
            // 否则确认框会盖在设置窗口上，未保存的配置也可能被打断
            if (g_settingsOpen) return 0;
            // 自定义热键触发：执行关联的命令
            int idx = (int)wp - CUSTOM_HOTKEY_ID_START;
            if (idx >= 0 && idx < (int)g_config.customHotkeys.size()) {
                // 值拷贝而非引用：下面的确认框/错误框会进入模态消息循环，
                // 期间设置窗口可能应用新配置使 vector 重新分配。
                // 拷贝同时保证“确认什么就执行什么”
                const CustomHotkey hk = g_config.customHotkeys[idx];
                // 同一 ID 可能已对应改键后的新条目：旧组合的消息不得执行新命令（R12-M1）
                if (!HotkeyMatchesConfig(hk.mod, hk.vk, lp)) return 0;
                // 模拟按键模式：释放修饰键后发送被模拟的按键或组合键。
                // 该动作没有“确认后执行”配置，按规则不弹任何确认框
                if (hk.simulateKey && hk.targetVK != 0) {
                    SimulateTargetKeyPress(hk.targetVK, hk.targetMod);
                    return 0;
                }
                if (!hk.command.empty()) {
                    // 执行前确认：仅当用户为该热键勾选“确认后执行”时才弹出，
                    // 与其他任何状态无关
                    if (hk.confirmBeforeRun) {
                        // 固定标签按当前语言翻译；用户字段只清洗控制字符、原样展示，
                        // 且整段消息不做片段翻译，保证“确认什么就执行什么”（R8-H6）。
                        // 字段不做截断：确认框使用可滚动编辑框完整展示，
                        // 避免危险后缀藏在省略部分（R15-H04）
                        std::wstring confirmMsg;
                        if (!hk.description.empty()) {
                            confirmMsg += Tr(L"描述：");
                            confirmMsg += SanitizeConfirmField(hk.description) + L"\n";
                        }
                        confirmMsg += hk.openPath ? Tr(L"文件/目录：") : Tr(L"程序：");
                        confirmMsg += SanitizeConfirmField(hk.command) + L"\n";
                        if (!hk.openPath && !hk.parameters.empty()) {
                            confirmMsg += Tr(L"参数：");
                            confirmMsg += SanitizeConfirmField(hk.parameters) + L"\n";
                        }
                        if (!hk.openPath && !hk.workDir.empty()) {
                            confirmMsg += Tr(L"工作目录：");
                            confirmMsg += SanitizeConfirmField(hk.workDir) + L"\n";
                        }
                        // 管理员提示单独传给对话框，在文本框上方单独一行显示，
                        // 不再混入程序/参数等执行内容
                        std::wstring adminNote;
                        if (!hk.openPath && hk.runAsAdmin) {
                            adminNote = g_processElevated
                                ? Tr(L"注意：当前已以管理员身份运行，子进程将以管理员权限运行。")
                                : Tr(L"注意：将以管理员权限运行");
                        }
                        // 走确认封装：确认框必须置前/置顶，否则从全屏程序里触发时
                        // 会被前台窗口盖住，UI 线程停在看不到的模态循环里，表现为"热键没反应"
                        if (!ConfirmCustomHotkey(confirmMsg, adminNote)) return 0;
                    }

                    // 热键动作失败提示：弹框期间抑制热键，避免再次触发；
                    // 消息含用户命令/路径时只翻译固定标签，命令原样展示
                    auto showHotkeyError = [](const std::wstring& msg) {
                        const bool prevSuppress = g_suppressHotkeys;
                        g_suppressHotkeys = true;
                        ShowAppMessageRaw(msg, MB_OK | MB_ICONERROR, Tr(L"错误"));
                        g_suppressHotkeys = prevSuppress;
                    };

                    // 统一规范化命令（去掉用户误粘贴的首尾引号），
                    // CreateProcess、ShellExecute 与打开路径共用同一份路径
                    std::wstring command = hk.command;
                    if (command.size() >= 2 && command.front() == L'"' && command.back() == L'"') {
                        const std::wstring unquoted = command.substr(1, command.size() - 2);
                        if (GetFileAttributesW(unquoted.c_str()) != INVALID_FILE_ATTRIBUTES)
                            command = unquoted;
                    }

                    // 打开文件/目录：交给 Shell 用默认关联程序打开
                    if (hk.openPath) {
                        if (command.empty()) return 0;
                        const bool opened = (INT_PTR)ShellExecuteW(
                            nullptr, L"open", command.c_str(), nullptr, nullptr,
                            SW_SHOWNORMAL) > 32;
                        if (!opened) {
                            showHotkeyError(std::wstring(Tr(L"打开失败: ")) +
                                            SanitizeConfirmField(command));
                        }
                        return 0;
                    }

                    // 根据配置设置启动窗口状态
                    int showCmd = SW_SHOWNORMAL;
                    if (hk.windowState == HotkeyWindowState::Minimized) showCmd = SW_SHOWMINIMIZED;
                    else if (hk.windowState == HotkeyWindowState::Maximized) showCmd = SW_SHOWMAXIMIZED;
                    else if (hk.windowState == HotkeyWindowState::Hidden) showCmd = SW_HIDE;

                    const wchar_t* workDir = hk.workDir.empty() ? nullptr : hk.workDir.c_str();

                    if (hk.runAsAdmin) {
                        // 勾选管理员：交给 Shell 的 runas 动词，由系统弹出 UAC。
                        // ShellExecuteEx 要求可执行文件与参数分开，而命令框允许
                        // 直接写完整命令行（与未勾选管理员时一致），这里按
                        // Windows 解析规则拆分第一个参数，避免整串被当成文件名
                        std::wstring file = command;
                        std::wstring params = hk.parameters;
                        const DWORD cmdAttrs = GetFileAttributesW(command.c_str());
                        const bool cmdIsFile = (cmdAttrs != INVALID_FILE_ATTRIBUTES) &&
                                               !(cmdAttrs & FILE_ATTRIBUTE_DIRECTORY);
                        if (!cmdIsFile) {
                            int argc = 0;
                            LPWSTR* argv = CommandLineToArgvW(command.c_str(), &argc);
                            if (argv && argc > 0) {
                                file = argv[0];
                                std::wstring rest;
                                for (int i = 1; i < argc; ++i) {
                                    if (!rest.empty()) rest += L" ";
                                    rest += QuoteCommandLineArg(argv[i]);
                                }
                                if (!hk.parameters.empty()) {
                                    if (!rest.empty()) rest += L" ";
                                    rest += hk.parameters;
                                }
                                params = rest;
                            }
                            if (argv) LocalFree(argv);
                        }
                        // SEE_MASK_FLAG_NO_UI 抑制 Shell 自己的错误框，
                        // 除用户取消 UAC 外由本程序统一提示
                        SHELLEXECUTEINFOW sei = {};
                        sei.cbSize = sizeof(sei);
                        sei.fMask = SEE_MASK_FLAG_NO_UI;
                        sei.lpVerb = L"runas";
                        sei.lpFile = file.c_str();
                        sei.lpParameters = params.empty() ? nullptr : params.c_str();
                        sei.lpDirectory = workDir;
                        sei.nShow = showCmd;
                        if (!ShellExecuteExW(&sei)) {
                            const DWORD err = GetLastError();
                            if (err != ERROR_CANCELLED) {  // 用户取消 UAC 不算失败
                                showHotkeyError(std::wstring(Tr(L"执行失败: ")) +
                                                SanitizeConfirmField(command));
                            }
                        }
                    } else {
                        // 正常启动：批处理/可执行文件走 CreateProcess，
                        // 文档/快捷方式等交给 Shell 按默认关联打开
                        DWORD launchError = ERROR_SUCCESS;
                        auto startProcess = [&](const wchar_t* application,
                                                std::wstring& cmdLine) -> bool {
                            STARTUPINFOW si = {};
                            si.cb = sizeof(si);
                            si.dwFlags = STARTF_USESHOWWINDOW;
                            si.wShowWindow = (WORD)showCmd;
                            PROCESS_INFORMATION pi = {};
                            if (!CreateProcessW(application, cmdLine.data(), nullptr, nullptr,
                                                FALSE, 0, nullptr, workDir, &si, &pi)) {
                                launchError = GetLastError();
                                return false;
                            }
                            CloseHandle(pi.hProcess);
                            CloseHandle(pi.hThread);
                            return true;
                        };

                        // 构建命令行：优先用 lpApplicationName 明确指定可执行文件，
                        // 避免整串加引号导致参数错位，以及无引号重试命中
                        // C:\Program.exe 这类路径劫持（Win32 命令行解析规则）
                        const DWORD cmdAttrs = GetFileAttributesW(command.c_str());
                        const bool cmdIsFile = (cmdAttrs != INVALID_FILE_ATTRIBUTES) &&
                                               !(cmdAttrs & FILE_ATTRIBUTE_DIRECTORY);
                        const bool cmdLooksLikePath =
                            command.find_first_of(L"\\/") != std::wstring::npos;
                        bool launched = false;

                        std::wstring ext;
                        if (cmdIsFile) {
                            const size_t dot = command.find_last_of(L'.');
                            if (dot != std::wstring::npos) {
                                ext = command.substr(dot);
                                CharLowerBuffW(ext.data(), (DWORD)ext.size());
                            }
                        }

                        if (cmdIsFile && (ext == L".bat" || ext == L".cmd")) {
                            // 批处理必须经 cmd.exe 执行
                            wchar_t sysDir[MAX_PATH] = {};
                            GetSystemDirectoryW(sysDir, MAX_PATH);
                            const std::wstring cmdExe = std::wstring(sysDir) + L"\\cmd.exe";
                            std::wstring cmdLine = L"cmd.exe /c \"" + command + L"\"";
                            if (!hk.parameters.empty()) cmdLine += L" " + hk.parameters;
                            launched = startProcess(cmdExe.c_str(), cmdLine);
                        } else if (cmdIsFile) {
                            // 可执行文件路径 + 参数：argv[0] 按 Windows 规则引用
                            std::wstring cmdLine = QuoteCommandLineArg(command);
                            if (!hk.parameters.empty()) cmdLine += L" " + hk.parameters;
                            launched = startProcess(command.c_str(), cmdLine);
                            // 非 PE 文件（快捷方式/文档）：CreateProcess 会报格式错误，
                            // 交给 Shell 按默认关联打开
                            if (!launched && (launchError == ERROR_BAD_EXE_FORMAT ||
                                              launchError == ERROR_NOT_SUPPORTED)) {
                                launched = (INT_PTR)ShellExecuteW(
                                    nullptr, L"open", command.c_str(),
                                    hk.parameters.empty() ? nullptr : hk.parameters.c_str(),
                                    workDir, showCmd) > 32;
                                if (!launched) launchError = GetLastError();
                            }
                        } else if (!cmdLooksLikePath ||
                                   command.find_first_of(L" \t") != std::wstring::npos) {
                            // 兼容“命令框里写成完整命令行”（如 cmd.exe /c foo，
                            // 或命令行中直接带输出文件路径）与 PATH 中的裸程序名：
                            // 没有可校验的文件路径时按原样执行一次。
                            // 不能仅用“含路径分隔符”排除：完整命令行里的参数
                            // 常常就是绝对路径，否则这类配置会被静默跳过
                            std::wstring rawLine = command;
                            if (!hk.parameters.empty()) rawLine += L" " + hk.parameters;
                            launched = startProcess(nullptr, rawLine);
                        }

                        // CreateProcess 无法处理的 shell 协议（如 ms-settings:、https://）
                        // 交给 Shell。错误 UI 被抑制，失败仍由统一提示展示
                        if (!launched && launchError != ERROR_ELEVATION_REQUIRED) {
                            SHELLEXECUTEINFOW sei = {};
                            sei.cbSize = sizeof(sei);
                            sei.fMask = SEE_MASK_FLAG_NO_UI;
                            sei.lpVerb = L"open";
                            sei.lpFile = command.c_str();
                            sei.lpParameters = hk.parameters.empty()
                                ? nullptr : hk.parameters.c_str();
                            sei.lpDirectory = workDir;
                            sei.nShow = showCmd;
                            if (ShellExecuteExW(&sei)) {
                                launched = true;
                                launchError = ERROR_SUCCESS;
                            } else {
                                launchError = GetLastError();
                            }
                        }

                        if (!launched) {
                            if (launchError == ERROR_ELEVATION_REQUIRED) {
                                // 目标程序清单要求管理员权限：提示用户勾选管理员选项
                                showHotkeyError(Tr(L"该程序要求管理员权限，无法以标准权限启动。\n"
                                                   L"请编辑该热键并勾选「以管理员运行（UAC）」。"));
                            } else {
                                showHotkeyError(std::wstring(Tr(L"执行失败: ")) +
                                                SanitizeConfirmField(command));
                            }
                        }
                    }
                }
            }
        }
        return 0;

    // --- 鼠标按键触发老板键 ---
    // 由鼠标钩子发送，中键或侧键按下时触发
    case WM_BOSS_MOUSE_TOGGLE: {
        // 设置对话框或热键确认框打开期间不响应，避免配置/确认过程中窗口被隐藏
        if (g_settingsOpen || g_suppressHotkeys) return 0;
        // 防抖：连击或 UI 线程繁忙导致消息排队时只处理一次，
        // 否则排队的多次切换会在短时间内连续隐藏/恢复，最终状态不可预期
        static ULONGLONG s_lastMouseToggleTick = 0;
        ULONGLONG now = GetTickCount64();
        if (now - s_lastMouseToggleTick < BOSS_MOUSE_TOGGLE_DEBOUNCE_MS) return 0;
        s_lastMouseToggleTick = now;
        HandleBossToggle();
        return 0;
    }

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

    // --- 系统设置变化 ---
    // 跟随系统时，Windows 切换浅色/深色模式或主题色后实时刷新界面
    case WM_SETTINGCHANGE:
        if (g_config.appTheme == 0 && lp &&
            wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
            Fluent::SetThemeMode(Fluent::ThemeMode::System);
            RefreshThemeAllWindows();
        }
        // 运行中开启/关闭高对比度：自绘控件与列表需要立即换成系统配色，
        // 否则重启前会一直沿用旧的浅色板文字/背景组合
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"HighContrast") == 0) {
            Fluent::RefreshHighContrastCache();
            RefreshThemeAllWindows();
        }
        break;

    // --- 显示器变化（增减/分辨率/主屏切换）---
    case WM_DISPLAYCHANGE:
        // 已显示的浮层按新的显示器布局重新定位；新增显示器上的浮层由
        // SetAppearance 补显示，避免"保持显示"档位下副屏一直没有浮层
        if (g_config.showLockKeyOverlay || g_config.showMuteOverlay) {
            g_overlay.SetAppearance(g_config.overlayPosition, g_config.overlayTheme,
                                    g_config.overlayCorner, g_config.overlayOpacity,
                                    g_config.overlayDelay);
        }
        break;

    // --- 系统会话结束 ---
    // Windows 关机/注销时触发，执行清理并保存配置
    case WM_ENDSESSION:
        if (wp) {
            // 标记会话结束：后续 WM_DESTROY 不再重复执行会阻塞关机的进程关闭，
            // wWinMain 也不再走带弹窗的退出保存（R11-M1）
            g_sessionEnding = true;
            KillTimer(hwnd, TIMER_WIND_CHECK);
            KillTimer(hwnd, TIMER_CORNER_CHECK);
            KillTimer(hwnd, TIMER_AUTO_HIDE);
            KillTimer(hwnd, TIMER_NUMLOCK_DELAY);  // 清理 NumLock 延迟定时器
            KillTimer(hwnd, TIMER_MUTE_REFRESH);   // 清理静音状态刷新定时器
            KillTimer(hwnd, TIMER_BOSSKEY_VERIFY); // 清理老板键延迟确认定时器
            KillTimer(hwnd, TIMER_HOOK_WATCH);     // 清理键盘钩子探活定时器
            KillTimer(hwnd, TIMER_MUTE_RESTORE_RETRY); // 清理启动静音恢复重试定时器
            // 先卸载钩子再做窗口恢复/进程关闭：这些操作会跨进程同步调用，
            // 若仍持有低级钩子，整个系统的键鼠输入会随 UI 线程一起被拖住
            g_hook.Uninstall();             // 卸载键盘钩子
            g_bossKey.UninstallMouseHook(); // 卸载鼠标钩子
            g_bossKey.Unregister();         // 注销老板键
            UnregisterHotKey(hwnd, BOSS_CLOSE_HOTKEY_ID);
            for (size_t i = 0; i < MAX_CUSTOM_HOTKEYS; i++) {
                UnregisterHotKey(hwnd, CUSTOM_HOTKEY_ID_START + (int)i);
            }
            // 先结算"异步隐藏待确认"，否则该状态下 m_active 为 false 会跳过恢复，
            // 窗口在关机后仍保持隐藏
            g_bossKey.VerifyPendingHide();
            if (g_bossKey.IsActive()) {
                if (g_config.bossKeyCloseOnExit) {
                    // 关机路径用短超时（500ms），且禁止强制终止，避免阻塞关机或丢失数据
                    g_bossKey.CloseBoundProcesses(500, false);
                } else {
                    // Deactivate() 内部会调用 ShowWindows() 完成窗口恢复并清除恢复文件
                    g_bossKey.Deactivate();
                }
            }
            // 关机路径同样先解除本程序造成的静音：会话结束后进程会被终止，
            // 留到析构阶段再处理已经来不及
            g_bossKey.RestoreSystemMute();
            g_winDGuard.Cleanup();
            // 关机/注销路径不允许弹窗；同时不能静默覆盖运行期间被外部修改的配置：
            // 检测到外部修改时保留磁盘内容，仅记录调试输出（R9-M4）
            if (ConfigChangedExternally()) {
                OutputDebugStringW((L"[KeySentry] 关机时检测到配置文件被外部修改，跳过保存: " +
                                    g_iniPath + L"\n").c_str());
            } else if (!SaveConfigAndRefreshStamp()) {
                OutputDebugStringW((L"[KeySentry] 关机时配置保存失败: " + g_iniPath + L"\n").c_str());
            }
            // 清理完成后主动退出：WM_ENDSESSION 返回后系统随时可能终止进程，
            // 但关机被延长时继续运行会处于"钩子/热键已全部卸载"的残缺状态；
            // DestroyWindow 走 WM_DESTROY 的统一收尾并发送 WM_QUIT（R11-M1）
            if (IsWindow(hwnd)) DestroyWindow(hwnd);
            PostQuitMessage(0);  // 兜底：窗口已销毁/销毁失败时也结束消息循环
        }
        return TRUE;  // MSDN 规定：wParam 为 TRUE 时必须返回 TRUE

    // --- 窗口销毁 ---
    // 程序退出时执行完整清理：停止定时器、恢复窗口、卸载钩子、
    // 注销热键、移除托盘图标、销毁悬浮窗、发送退出消息
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_WIND_CHECK);
        KillTimer(hwnd, TIMER_CORNER_CHECK);
        KillTimer(hwnd, TIMER_AUTO_HIDE);
        KillTimer(hwnd, TIMER_NUMLOCK_DELAY);  // 清理 NumLock 延迟定时器
        KillTimer(hwnd, TIMER_MUTE_REFRESH);   // 清理静音状态刷新定时器
        KillTimer(hwnd, TIMER_BOSSKEY_VERIFY); // 清理老板键延迟确认定时器
        KillTimer(hwnd, TIMER_HOOK_WATCH);     // 清理键盘钩子探活定时器
        KillTimer(hwnd, TIMER_MUTE_RESTORE_RETRY); // 清理启动静音恢复重试定时器
        // 先卸载钩子：恢复窗口/关闭进程可能阻塞 UI 线程，不能让系统输入继续挂在钩子上
        g_hook.Uninstall();             // 卸载键盘钩子
        g_bossKey.UninstallMouseHook(); // 卸载鼠标钩子
        g_bossKey.Unregister();         // 注销老板键
        UnregisterHotKey(hwnd, BOSS_CLOSE_HOTKEY_ID);
        for (size_t i = 0; i < MAX_CUSTOM_HOTKEYS; i++) {
            UnregisterHotKey(hwnd, CUSTOM_HOTKEY_ID_START + (int)i);  // 注销所有自定义热键
        }
        // 同样先结算"异步隐藏待确认"，避免漏掉已生效的隐藏。
        // 会话结束路径已由 WM_ENDSESSION 按关机要求完成关闭/恢复：
        // 这里不能再重复执行会阻塞关机的进程关闭（R11-M1）
        if (!g_sessionEnding) {
            g_bossKey.VerifyPendingHide();
            if (g_bossKey.IsActive()) {
                if (g_config.bossKeyCloseOnExit) {
                    g_bossKey.CloseBoundProcesses();  // 配置了退出时关闭关联进程
                } else {
                    g_bossKey.Deactivate();  // Deactivate() 内部会调用 ShowWindows() 并清除恢复文件
                }
            }
        }
        // 释放缓存的音频 COM 接口：必须赶在 wWinMain 中的 CoUninitialize 之前，
        // 否则全局 g_bossKey 的析构会在 COM 已反初始化的线程上 Release
        // 先解除本程序造成的系统静音：CloseBoundProcesses 可能保留了激活态，
        // 若留到析构阶段的 Deactivate 才处理，COM 已反初始化会静默失败
        g_bossKey.RestoreSystemMute();
        g_bossKey.ReleaseAudio();
        g_winDGuard.Cleanup();
        // 直接删除：若图标已被老板键隐藏，托盘区本就没有条目，无需先补加再删
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
    // 先初始化 UI 缩放系数（基于系统 DPI），保证后续字体与窗口尺寸一致
    InitUiScale();

    // 注册 TaskbarCreated 消息，用于 explorer 重启后恢复托盘图标
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // --- 单实例检测 ---
    // 统一以 Local（当前会话）命名互斥体为主判定：Global 命名需要额外权限，
    // 普通实例创建失败会退回 Local，而提权实例只建 Global，两者互不可见导致可双开。
    // 本地创建成功后再额外探测 Global，兼容旧版本/已提权运行中的实例。
    // 名称追加当前用户 SID：避免其他用户预创建 Global 对象造成启动拒绝
    const std::wstring localMutexName = BuildUserScopedMutexName(LOCAL_MUTEX_PREFIX);
    const std::wstring globalMutexName = BuildUserScopedMutexName(MUTEX_PREFIX);
    SetLastError(0);
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, localMutexName.c_str());
    const DWORD localMutexErr = GetLastError();
    const bool localExists = (hMutex && localMutexErr == ERROR_ALREADY_EXISTS);
    // ACCESS_DENIED 说明同名对象已存在但无法打开（例如由其他账户的提权实例创建）
    const bool localDenied = (!hMutex && localMutexErr == ERROR_ACCESS_DENIED);
    if (!hMutex && !localDenied) {
        // 既无法创建也无法判定：不能带着未知的单实例状态继续启动
        const DWORD err = localMutexErr ? localMutexErr : GetLastError();
        const std::wstring msg = L"初始化单实例保护失败（错误 " +
                                 std::to_wstring(err) + L"），程序将退出。";
        Fluent::ShowMessageBox(nullptr, msg.c_str(), APP_NAME, MB_OK | MB_ICONERROR);
        return 0;
    }
    bool alreadyRunning = localExists || localDenied;
    HANDLE hGlobalMutex = nullptr;
    bool globalExists = false;
    bool globalDenied = false;
    if (!alreadyRunning) {
        SetLastError(0);
        hGlobalMutex = CreateMutexW(nullptr, TRUE, globalMutexName.c_str());
        const DWORD globalMutexErr = GetLastError();
        globalExists = (hGlobalMutex && globalMutexErr == ERROR_ALREADY_EXISTS);
        globalDenied = (!hGlobalMutex && globalMutexErr == ERROR_ACCESS_DENIED);
        alreadyRunning = globalExists || globalDenied;
    }
    if (!alreadyRunning) {
        // 兼容升级前的旧版本：旧实例使用不含 SID 的互斥体名，新名称不会与它冲突。
        // 以主窗口作为补充判定，避免升级期间短暂双开并互相覆盖配置/恢复文件。
        HWND legacyInstance = FindSameProductMainWindow();
        if (legacyInstance) {
            if (hMutex) CloseHandle(hMutex);
            if (hGlobalMutex) CloseHandle(hGlobalMutex);
            if (IsOwnProcessWindow(legacyInstance)) {
                SendMessageTimeoutW(legacyInstance, WM_TRAYICON, 0, WM_LBUTTONDBLCLK,
                                    SMTO_ABORTIFHUNG, 1000, nullptr);
            }
            return 0;
        }
    }
    if (alreadyRunning) {
        // 不能只凭命名互斥体就退出：同用户进程可以先创建同名互斥体并持有，
        // 从而阻止本程序启动（本地拒绝服务）。这里确认候选实例确实存在本程序的
        // 主窗口；正在启动的实例可能尚未创建窗口，给 5 秒等待窗口出现。
        // 冷启动/安全软件扫描时窗口创建可能明显晚于 2 秒，等待过短会误判为
        // “互斥体被抢占”并启动第二个实例，共享 INI/恢复/锁文件（R13-A2）
        HWND existing = nullptr;
        for (int attempt = 0; attempt < 100; ++attempt) {
            existing = FindSameProductMainWindow();
            if (existing) break;
            Sleep(50);
        }
        if (existing) {
            // 已有实例运行（x86/x64 任一构建），尝试激活其设置窗口
            if (hMutex) CloseHandle(hMutex);
            if (hGlobalMutex) CloseHandle(hGlobalMutex);
            // 窗口类名是全局字符串，必须校验窗口属于本程序（同一 exe），
            // 避免向伪造同名类窗口发消息
            if (IsOwnProcessWindow(existing)) {
                // 超时不宜过长：已有实例卡在模态框时会让第二个进程长时间挂起；
                // 若活跃实例收到消息会正常打开设置，超时仅影响本次激活尝试
                SendMessageTimeoutW(existing, WM_TRAYICON, 0, WM_LBUTTONDBLCLK, SMTO_ABORTIFHUNG, 1000, nullptr);
            }
            return 0;
        }
        // 窗口长时间未出现（冷启动过慢，或提权实例的窗口无法被低权限进程识别）：
        // 若互斥体仍被其他进程持有，启动第二个实例会共享并互相覆盖 INI/恢复文件，
        // 因此按“已有实例”处理并提示，而不是当成互斥体被抢占（R14-M8）
        HANDLE heldProbe = nullptr;
        if (localExists && hMutex) heldProbe = hMutex;
        else if (globalExists && hGlobalMutex) heldProbe = hGlobalMutex;
        if (heldProbe && WaitForSingleObject(heldProbe, 0) == WAIT_TIMEOUT) {
            if (hMutex) CloseHandle(hMutex);
            if (hGlobalMutex) CloseHandle(hGlobalMutex);
            Fluent::ShowMessageBox(nullptr, L"另一个键客实例正在启动或运行，请稍后重试。",
                                   APP_NAME, MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        // 权限不足以打开同名互斥体时没有可等待句柄，无法证明“没有实例”。
        // 不能把“无法判断”当成“可以启动”，否则双开会互相覆盖状态文件（R15-M02）
        if (localDenied || globalDenied) {
            if (hMutex) CloseHandle(hMutex);
            if (hGlobalMutex) CloseHandle(hGlobalMutex);
            Fluent::ShowMessageBox(
                nullptr,
                L"另一个键客实例正在运行，但当前权限无法确认其状态。\n"
                L"请先退出该实例，或以相同权限重试。",
                APP_NAME, MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        // 互斥体没有对应的本程序窗口：按被抢占处理，释放后继续单实例启动
        if (hMutex) CloseHandle(hMutex);
        if (hGlobalMutex) CloseHandle(hGlobalMutex);
        hGlobalMutex = nullptr;
        SetLastError(0);
        hMutex = CreateMutexW(nullptr, TRUE, localMutexName.c_str());
        if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
            // 释放与重建之间可能有其他实例抢先启动：按已有实例处理
            const DWORD err = GetLastError();
            if (hMutex) CloseHandle(hMutex);
            const std::wstring msg = (err == ERROR_ALREADY_EXISTS)
                ? std::wstring(L"另一个键客实例已启动，请稍后重试。")
                : (L"无法获取单实例保护（错误 " + std::to_wstring(err) + L"），程序将退出。");
            Fluent::ShowMessageBox(nullptr, msg.c_str(), APP_NAME,
                                   MB_OK | (err == ERROR_ALREADY_EXISTS ? MB_ICONINFORMATION
                                                                        : MB_ICONERROR));
            return 0;
        }
    }

    g_hInst = hInst;
    g_iniPath = GetIniPath();       // 获取配置文件路径
    g_config.Load(g_iniPath);       // 加载配置
    RefreshConfigStamp();           // 记录载入时的文件时间，退出时用于检测外部编辑
    // 提权状态用于决定启动子进程时是否需要降权为标准用户令牌；
    // 启动确认只由热键自身的“确认后执行”配置决定
    g_processElevated = ProcessUtils::IsElevated();
    // 应用外观主题与界面语言（须在创建任何窗口前完成，字体/画刷按主题生成）
    Fluent::SetThemeMode((Fluent::ThemeMode)g_config.appTheme);
    SetAppLanguage((AppLanguage)g_config.appLanguage);

    // --- 初始化通用控件（Tab 控件等）---
    INITCOMMONCONTROLSEX icc = { sizeof(icc),
        ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS |
        ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    // 初始化 COM 库（用于音频控制等功能）
    CoInitGuard coInit;

    // --- 注册主窗口类 ---
    WNDCLASSW wc = {};
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = MAIN_WND_CLASS;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    if (!WindowUtils::RegisterClassOnce(wc)) {
        Fluent::ShowMessageBox(nullptr, L"窗口类注册失败", APP_NAME, MB_OK | MB_ICONERROR);
        // 互斥体创建可能失败（返回 NULL），清理前需判空
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        if (hGlobalMutex) { ReleaseMutex(hGlobalMutex); CloseHandle(hGlobalMutex); }
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
        Fluent::ShowMessageBox(nullptr, L"主窗口创建失败", APP_NAME, MB_OK | MB_ICONERROR);
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        if (hGlobalMutex) { ReleaseMutex(hGlobalMutex); CloseHandle(hGlobalMutex); }
        return 1;
    }

    // --- 创建悬浮提示窗口 ---
    if (!g_overlay.Create(hInst)) {
        Fluent::ShowMessageBox(nullptr, L"提示窗口创建失败", APP_NAME, MB_OK | MB_ICONERROR);
    }

    // --- 应用配置并添加托盘图标 ---
    // 键盘钩子由 ApplyConfig 按需安装，此处不再无条件安装
    // 先恢复上次异常退出遗留的窗口样式（Win+D 守护移除了 WS_MINIMIZEBOX，
    // 程序被强杀时析构不会执行，需在启动时兜底还原，避免永久破坏其他进程窗口）
    WinDGuard::RecoverOrphanedStyles();

    // 恢复上次异常退出时被隐藏的窗口（必须在 ApplyConfig 之前完成）：
    // ApplyConfig 会安装键盘/鼠标低级钩子，而恢复窗口需要跨进程同步调用，
    // 先装钩子再做恢复会让系统输入悬挂在尚未就绪的 UI 线程上
    int recoveredWindows = g_bossKey.RecoverOrphanedWindows();
    // 开机自启动时音频服务/默认端点可能尚未就绪，启动恢复静音可能暂时失败：
    // 静默重试一段时间，避免开机就弹“音频设备暂不可用”的提示
    if (g_bossKey.HasPendingMuteRestore()) {
        g_muteRestoreRetries = 0;
        SetTimer(g_mainWnd, TIMER_MUTE_RESTORE_RETRY, TIMER_MUTE_RESTORE_RETRY_MS, nullptr);
    }

    ApplyConfig();
    ApplyExplorerSettings();  // 启动时重置资源管理器文件夹选项
    // 启动早期缓存的一次性告警（如窗口保护失败）会在 AddTrayIcon 中补发；
    // 托盘气泡是覆盖式的，若紧接着再发启动提示，告警会被瞬间顶掉而看不见。
    // 记录该状态，让启动提示让位于告警（只保留一条）
    const bool hadPendingNotice = !g_pendingTrayText.empty();
    AddTrayIcon(g_mainWnd);
    // 配置文件存在但读不出来：按具体原因提示（编码问题会给出另存为指引），
    // 明确本次以默认值运行且不会覆盖原文件（R11-M3）
    if (g_config.LoadFailed()) {
        ShowTrayNotification(ConfigLoadFailureText(), true);
    }

    // --- 显示启动通知气泡 ---
    // 同一时刻只发一条：优先级 缓存告警（已由 AddTrayIcon 补发）> 崩溃恢复提示 > 常规启动提示
    if (!hadPendingNotice && !g_config.LoadFailed() &&
        (recoveredWindows > 0 || !g_config.disableStartupNotification)) {
        g_nid.uFlags = NIF_INFO;
        wcsncpy_s(g_nid.szInfoTitle, Tr(APP_NAME), _TRUNCATE);
        std::wstring infoText;
        if (recoveredWindows > 0) {
            wchar_t buf[128] = L"";
            swprintf_s(buf, L"检测到上次异常退出，已恢复 %d 个被隐藏的窗口。", recoveredWindows);
            infoText = buf;
            g_nid.dwInfoFlags = NIIF_WARNING;
        } else {
            infoText = L"键客 KeySentry 已启动，如需设置请点击托盘图标。";
            g_nid.dwInfoFlags = NIIF_INFO;
        }
        infoText = TranslateMessageText(infoText);
        wcsncpy_s(g_nid.szInfo, infoText.c_str(), _TRUNCATE);
        g_nid.uTimeout = 5000;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;  // 恢复标志位
    }

    // --- 主消息循环 ---
    MSG msg = {};
    BOOL getRet = 0;
    while ((getRet = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // GetMessage 返回 -1 表示消息循环出错：主动销毁主窗口走完整清理，
    // 避免跳过 WM_DESTROY 后在 COM 已反初始化时释放全局对象
    if (getRet == -1 && g_mainWnd && IsWindow(g_mainWnd)) {
        DestroyWindow(g_mainWnd);
    }

    // --- 退出清理 ---
    // 会话结束路径已在 WM_ENDSESSION 中无弹窗地完成保存与外部修改检测；
    // 若在此再次保存，会在关机过程中弹出"配置冲突/保存失败"对话框（R11-M1）
    if (!g_sessionEnding) {
        SaveConfigOnExit();  // 检测外部编辑冲突；保存失败时明确提示
    } else {
        OutputDebugStringW(L"[KeySentry] 会话结束路径已完成配置保存，跳过退出保存\n");
    }

    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    if (hGlobalMutex) {
        ReleaseMutex(hGlobalMutex);
        CloseHandle(hGlobalMutex);
    }

    return (getRet == -1) ? 1 : (int)msg.wParam;
}
