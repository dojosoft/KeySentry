#pragma once
#include <string>
#include <vector>
#include <Windows.h>
#include "Utils.h"

// 自定义热键最大数量
static constexpr int MAX_CUSTOM_HOTKEYS = 100;
// 配置文件版本号，用于兼容性检查
static constexpr int CONFIG_VERSION = 2;

// ============================================================
// MatchMode: 窗口匹配模式
//   Window  - 按窗口标题匹配
//   Process - 按进程名/路径匹配
// ============================================================
enum class MatchMode {
    Window,
    Process
};

// ============================================================
// HotkeyWindowState: 自定义热键启动程序时的窗口状态
// ============================================================
enum class HotkeyWindowState {
    Normal,     // 正常显示
    Minimized,  // 最小化
    Maximized,  // 最大化
    Hidden      // 隐藏
};

// ============================================================
// ConfigUtils: 配置序列化/反序列化的辅助工具
// ============================================================
namespace ConfigUtils {
    // 转义字段中的特殊字符（| -> \p, ; -> \s, \ -> \\），用于序列化
    static inline std::wstring EscapeField(const std::wstring& s) {
        std::wstring r;
        for (wchar_t c : s) {
            if (c == L'|') r += L"\\p";
            else if (c == L';') r += L"\\s";
            else if (c == L'\\') r += L"\\\\";
            else r += c;
        }
        return r;
    }

    // 反转义字段，还原特殊字符
    static inline std::wstring UnescapeField(const std::wstring& s) {
        std::wstring r;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == L'\\' && i + 1 < s.size()) {
                if (s[i + 1] == L'p') { r += L'|'; ++i; }
                else if (s[i + 1] == L's') { r += L';'; ++i; }
                else if (s[i + 1] == L'\\') { r += L'\\'; ++i; }
                else r += s[i];
            } else {
                r += s[i];
            }
        }
        return r;
    }

    // 将整数值安全地限制在 HotkeyWindowState 枚举范围内
    static inline HotkeyWindowState ClampWindowState(int v) {
        if (v < 0 || v > 3) return HotkeyWindowState::Normal;
        return (HotkeyWindowState)v;
    }

    // 将整数值安全地限制在 MatchMode 枚举范围内
    static inline MatchMode ClampMatchMode(int v) {
        if (v < 0 || v > 1) return MatchMode::Window;
        return (MatchMode)v;
    }

    // 安全地将宽字符串解析为整数，解析失败返回默认值
    static inline int SafeParseInt(const std::wstring& s, int defVal = 0) {
        if (s.empty()) return defVal;
        try {
            size_t pos = 0;
            int val = std::stoi(s, &pos);
            return (pos > 0) ? val : defVal;
        } catch (...) {
            return defVal;
        }
    }
}

// ============================================================
// CustomHotkey: 自定义热键配置
// 记录热键组合、关联的命令、参数、工作目录、音效等信息
// ============================================================
struct CustomHotkey {
    UINT mod = 0;                       // 修饰键（MOD_CONTROL/MOD_ALT/MOD_SHIFT/MOD_WIN 的组合）
    UINT vk = 0;                        // 虚拟键码
    std::wstring command;               // 要执行的程序路径
    std::wstring parameters;            // 命令行参数
    std::wstring workDir;               // 工作目录
    std::wstring sound;                 // 触发时播放的音效文件路径
    std::wstring description;           // 热键描述/显示名称
    HotkeyWindowState windowState = HotkeyWindowState::Normal; // 启动后窗口状态
    int opacity = -1;                   // 窗口透明度（-1 表示不设置）
    bool autoStart = false;             // 是否随程序启动自动执行
    bool confirmBeforeRun = false;      // 执行前是否需要确认
    bool runAsAdmin = false;            // 是否以管理员权限运行

    // 将自定义热键序列化为管道分隔的字符串
    std::wstring Serialize() const {
        return std::to_wstring(mod) + L"|" + std::to_wstring(vk) + L"|" +
               ConfigUtils::EscapeField(command) + L"|" + ConfigUtils::EscapeField(parameters) + L"|" +
               ConfigUtils::EscapeField(workDir) + L"|" + ConfigUtils::EscapeField(sound) + L"|" +
               ConfigUtils::EscapeField(description) + L"|" +
               std::to_wstring((int)windowState) + L"|" +
               std::to_wstring(opacity) + L"|" +
               IniUtils::BoolToStr(autoStart) + L"|" +
               IniUtils::BoolToStr(confirmBeforeRun) + L"|" +
               IniUtils::BoolToStr(runAsAdmin);
    }

    // 从管道分隔的字符串反序列化自定义热键
    static CustomHotkey Deserialize(const std::wstring& str) {
        CustomHotkey hk;
        std::vector<size_t> pipes;
        for (size_t i = 0; i < str.size(); i++) {
            if (str[i] == L'|') pipes.push_back(i);
        }
        if (pipes.size() < 11) return hk;
        hk.mod = (UINT)ConfigUtils::SafeParseInt(str.substr(0, pipes[0]));
        hk.vk = (UINT)ConfigUtils::SafeParseInt(str.substr(pipes[0] + 1, pipes[1] - pipes[0] - 1));
        hk.command = ConfigUtils::UnescapeField(str.substr(pipes[1] + 1, pipes[2] - pipes[1] - 1));
        hk.parameters = ConfigUtils::UnescapeField(str.substr(pipes[2] + 1, pipes[3] - pipes[2] - 1));
        hk.workDir = ConfigUtils::UnescapeField(str.substr(pipes[3] + 1, pipes[4] - pipes[3] - 1));
        hk.sound = ConfigUtils::UnescapeField(str.substr(pipes[4] + 1, pipes[5] - pipes[4] - 1));
        hk.description = ConfigUtils::UnescapeField(str.substr(pipes[5] + 1, pipes[6] - pipes[5] - 1));
        hk.windowState = ConfigUtils::ClampWindowState(ConfigUtils::SafeParseInt(str.substr(pipes[6] + 1, pipes[7] - pipes[6] - 1)));
        hk.opacity = ConfigUtils::SafeParseInt(str.substr(pipes[7] + 1, pipes[8] - pipes[7] - 1), -1);
        hk.autoStart = ConfigUtils::SafeParseInt(str.substr(pipes[8] + 1, pipes[9] - pipes[8] - 1)) != 0;
        hk.confirmBeforeRun = ConfigUtils::SafeParseInt(str.substr(pipes[9] + 1, pipes[10] - pipes[9] - 1)) != 0;
        hk.runAsAdmin = ConfigUtils::SafeParseInt(str.substr(pipes[10] + 1)) != 0;
        return hk;
    }

    // 获取热键的显示名称：优先使用描述，其次使用命令路径
    std::wstring DisplayName() const {
        if (!description.empty()) return description;
        if (!command.empty()) return command;
        return L"";
    }
};

// ============================================================
// BoundWindowInfo: 绑定窗口信息
// 用于老板键和 WinDGuard 功能中指定要操作的窗口
// ============================================================
struct BoundWindowInfo {
    std::wstring title;             // 窗口标题（用于标题匹配）
    std::wstring processName;       // 进程可执行文件名（用于进程匹配）
    DWORD pid = 0;                  // 进程ID
    std::wstring processPath;       // 进程完整路径（用于进程匹配）
    MatchMode matchMode = MatchMode::Window; // 匹配模式

    // 序列化为管道分隔的字符串
    std::wstring Serialize() const {
        return std::to_wstring((int)matchMode) + L"|" + ConfigUtils::EscapeField(title) + L"|" +
               ConfigUtils::EscapeField(processName) + L"|" + std::to_wstring(pid) + L"|" + ConfigUtils::EscapeField(processPath);
    }

    // 从管道分隔的字符串反序列化
    static BoundWindowInfo Deserialize(const std::wstring& str) {
        BoundWindowInfo info;
        size_t p0 = str.find(L'|');
        if (p0 == std::wstring::npos) { info.title = ConfigUtils::UnescapeField(str); return info; }
        info.matchMode = ConfigUtils::ClampMatchMode(ConfigUtils::SafeParseInt(str.substr(0, p0)));
        size_t p1 = str.find(L'|', p0 + 1);
        if (p1 == std::wstring::npos) { info.title = ConfigUtils::UnescapeField(str.substr(p0 + 1)); return info; }
        info.title = ConfigUtils::UnescapeField(str.substr(p0 + 1, p1 - p0 - 1));
        size_t p2 = str.find(L'|', p1 + 1);
        if (p2 == std::wstring::npos) { info.processName = ConfigUtils::UnescapeField(str.substr(p1 + 1)); return info; }
        info.processName = ConfigUtils::UnescapeField(str.substr(p1 + 1, p2 - p1 - 1));
        size_t p3 = str.find(L'|', p2 + 1);
        if (p3 == std::wstring::npos) {
            info.pid = (DWORD)ConfigUtils::SafeParseInt(str.substr(p2 + 1));
            return info;
        }
        info.pid = (DWORD)ConfigUtils::SafeParseInt(str.substr(p2 + 1, p3 - p2 - 1));
        info.processPath = ConfigUtils::UnescapeField(str.substr(p3 + 1));
        return info;
    }

    // 判断指定窗口是否匹配此绑定信息
    bool Matches(HWND hwnd, bool requireVisible = true) const;

    // 获取绑定信息的显示名称
    std::wstring DisplayName() const {
        if (matchMode == MatchMode::Process) {
            return L"[P] " + processName;
        }
        if (!processName.empty() && processName != title)
            return title + L" [" + processName + L"]";
        return title;
    }
};

// ============================================================
// WindowEnum: 窗口枚举工具，用于列出未绑定的窗口
// ============================================================
namespace WindowEnum {
    // 枚举上下文，传递给 EnumWindows 回调
    struct EnumWindowCtx {
        std::vector<BoundWindowInfo>* cache;    // 缓存未绑定窗口信息
        const std::vector<BoundWindowInfo>* bound; // 已绑定窗口列表
        HWND lstAll;                            // 列表框控件句柄
    };

    // 枚举回调：32 位下 GCC 的 lambda 无法隐式转换为 stdcall 的 WNDENUMPROC，
    // 故用显式 CALLBACK 约定的命名函数
    inline BOOL CALLBACK EnumUnboundProc(HWND hwnd, LPARAM lParam) {
            if (!IsWindowVisible(hwnd)) return TRUE;
            wchar_t title[512] = {};
            GetWindowTextW(hwnd, title, 512);
            if (wcslen(title) == 0) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == 0 || pid == GetCurrentProcessId()) return TRUE;

            std::wstring processName = ProcessUtils::GetProcessName(pid);
            std::wstring processPath = ProcessUtils::GetProcessPath(pid);

            auto* ctx = reinterpret_cast<EnumWindowCtx*>(lParam);
            // 跳过已绑定的窗口
            bool alreadyBound = false;
            for (const auto& b : *ctx->bound) {
                if (b.pid == pid || (!b.processName.empty() && _wcsicmp(b.processName.c_str(), processName.c_str()) == 0)) {
                    alreadyBound = true;
                    break;
                }
            }
            if (!alreadyBound) {
                BoundWindowInfo info;
                info.title = title;
                info.processName = processName;
                info.pid = pid;
                info.processPath = processPath;
                info.matchMode = MatchMode::Window;
                ctx->cache->push_back(info);
                if (ctx->lstAll) {
                    SendMessageW(ctx->lstAll, LB_ADDSTRING, 0, (LPARAM)info.DisplayName().c_str());
                }
            }
            return TRUE;
    }

    // 枚举所有可见的未绑定顶层窗口，结果存入 ctx.cache 和列表框
    inline void EnumerateUnboundWindows(EnumWindowCtx& ctx) {
        ctx.cache->clear();
        if (ctx.lstAll) SendMessageW(ctx.lstAll, LB_RESETCONTENT, 0, 0);
        EnumWindows(EnumUnboundProc, reinterpret_cast<LPARAM>(&ctx));
    }
}

// ============================================================
// KeyMapping: 按键映射规则
// 支持组合键双向映射（如 WIN+6 → F6，或 F6 → WIN+6）
// ============================================================
struct KeyMapping {
    UINT srcMod = 0;   // 源修饰键（MOD_CONTROL/MOD_SHIFT/MOD_ALT/MOD_WIN 之组合，0=无）
    UINT srcVk = 0;    // 源主键虚拟键码
    UINT dstMod = 0;   // 目标修饰键（0=无）
    UINT dstVk = 0;    // 目标主键虚拟键码
};

// ============================================================
// AppConfig: 应用全局配置
// 包含所有功能模块的配置项，支持 INI 文件读写
// ============================================================
struct AppConfig {
    // --- 通用设置 ---
    bool autoStart = false;               // 开机自启动
    bool showLockKeyOverlay = true;       // 显示锁键状态浮层
    bool showMuteOverlay = false;         // 显示静音状态浮层
    bool overlayPrimaryOnly = false;      // 仅在主显示器显示浮层
    bool forceNumLockOn = false;          // 强制开启 NumLock
    bool disableStartupNotification = false; // 禁用启动通知
    // 资源管理器文件夹选项（0=不改变, 1=开启隐藏, 2=关闭隐藏即显示）
    int explorerHideHidden = 0;           // 隐藏文件和文件夹
    int explorerHideExt = 0;              // 隐藏已知文件类型的扩展名
    int explorerHideOS = 0;               // 隐藏受保护的操作系统文件
    bool disableSpecifiedKeysEnabled = false; // 启用禁用指定按键
    bool disableNumLock = false;          // 禁用 NumLock 键
    bool forceInsertMode = false;         // 禁用 Insert 键
    std::vector<int> disabledKeyCodes;    // 被禁用的按键虚拟键码列表

    // --- 按键重映射 ---
    bool keyRemapEnabled = false;         // 启用按键重映射
    std::vector<KeyMapping> keyRemappings; // 重映射规则（支持组合键）

    // --- 热键屏蔽与自定义热键 ---
    std::vector<std::pair<UINT, UINT>> disabledHotkeys; // 被屏蔽的系统热键：{修饰键, 虚拟键}
    bool customHotkeysEnabled = false;    // 启用自定义热键
    std::vector<CustomHotkey> customHotkeys; // 自定义热键列表

    // --- 老板键设置 ---
    bool bossKeyEnabled = false;          // 启用老板键
    UINT bossKeyMod = MOD_WIN;            // 老板键修饰键（默认 Win）
    UINT bossKeyVK = 0xC0;                // 老板键虚拟键（默认 ` 键）
    UINT bossKeyCloseMod = MOD_WIN;       // 一键关闭修饰键
    UINT bossKeyCloseVK = VK_ESCAPE;      // 一键关闭虚拟键
    bool bossKeyMute = false;             // 隐藏窗口时同时静音
    std::vector<BoundWindowInfo> bossKeyWindows; // 老板键绑定的窗口列表

    bool bossKeyHideCurrent = false;      // 隐藏当前活动窗口
    bool bossKeySendPause = false;        // 隐藏窗口前发送媒体暂停
    bool bossKeyMiddleButton = false;     // 鼠标中键触发老板键
    bool bossKeySideButton1 = false;      // 鼠标侧键1触发老板键
    bool bossKeySideButton2 = false;      // 鼠标侧键2触发老板键
    bool bossKeyCornerTL = false;         // 鼠标移至左上角触发
    bool bossKeyCornerTR = false;         // 鼠标移至右上角触发
    bool bossKeyCornerBL = false;         // 鼠标移至左下角触发
    bool bossKeyCornerBR = false;         // 鼠标移至右下角触发
    bool bossKeyAutoHide = false;         // 闲置自动隐藏
    int bossKeyAutoHideTime = 5;          // 自动隐藏的闲置时间（分钟）
    bool bossKeyCloseOnExit = false;      // 退出时关闭绑定的进程

    // --- WinDGuard 窗口保护 ---
    bool winDGuardEnabled = false;        // 启用 WinDGuard
    std::vector<BoundWindowInfo> winDGuardWindows; // 受保护窗口列表

    // --- UI 状态 ---
    int tabState = 0;                     // 设置对话框当前选中的标签页索引

    // 从 INI 文件加载配置
    void Load(const std::wstring& iniPath);
    // 保存配置到 INI 文件
    void Save(const std::wstring& iniPath) const;

    // 判断指定虚拟键码是否在禁用列表中
    bool IsKeyDisabled(int vkCode) const;

    // 虚拟键码转可读名称
    static std::wstring VKToName(int vk);
    // 可读名称转虚拟键码
    static int NameToVK(const std::wstring& name);
};
