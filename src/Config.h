#pragma once
#include <string>
#include <vector>
#include <Windows.h>
#include "Utils.h"

// 自定义热键最大数量
static constexpr int MAX_CUSTOM_HOTKEYS = 100;
// 禁用按键/按键映射条目上限：配置可手改，钩子回调按条查询，
// 无上限的列表会让每次按键产生线性扫描开销
static constexpr int MAX_DISABLED_KEYS = 256;
static constexpr int MAX_KEY_REMAPS = 256;
// 窗口绑定上限、标题长度上限、自动隐藏分钟数上限
static constexpr int MAX_BOUND_WINDOWS = 64;
static constexpr int MAX_BINDING_TITLE = 256;
static constexpr int MAX_BINDING_NAME = 256;
static constexpr int MAX_BINDING_PATH = 1024;
// BossKey/WinDGuard 绑定列表的单值总长度上限（与自定义热键相同的 INI 限制）
static constexpr size_t MAX_BOUND_WINDOWS_CHARS = 30000;
static constexpr int MAX_AUTO_HIDE_MINUTES = 120;
// 自定义热键序列化后的总长度上限：profile API 对单个 INI 值的长行支持有限
// （实测超过约 3 万字符后读取会异常），保存前必须拦截并提示用户
static constexpr size_t MAX_CUSTOM_ENTRIES_CHARS = 30000;
// 自定义热键单个文本字段上限（与设置对话框 Edit 控件的输入限制一致）。
// 手改 INI 可能写入超长字段，加载时统一收敛，避免提示消息/列表被撑爆（R10-M3）
static constexpr size_t MAX_CUSTOM_COMMAND_CHARS = 32768;
static constexpr size_t MAX_CUSTOM_PARAMS_CHARS = 8192;
static constexpr size_t MAX_CUSTOM_WORKDIR_CHARS = 1024;
static constexpr size_t MAX_CUSTOM_DESC_CHARS = 512;
// 配置文件版本号：5 = 一键关闭热键默认恢复为 Win+Esc；4 = 旧版清理标记；
// 3 起按键映射使用四元组（MappingsV3，含互换组）
static constexpr int CONFIG_VERSION = 5;

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
    // 转义字段中的特殊字符（| -> \p, ; -> \s, \ -> \\, 换行 -> \r/\n, = -> \e），用于序列化
    // 换行与 = 会破坏 INI 的单行键值结构，必须转义；
    // 首尾空白与引号会被 profile API 读写时裁剪，也需转义才能原样保存
    static inline std::wstring EscapeField(const std::wstring& s) {
        std::wstring r;
        r.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            const wchar_t c = s[i];
            const bool boundary = (i == 0 || i + 1 == s.size());
            if (c == L'|') r += L"\\p";
            else if (c == L';') r += L"\\s";
            else if (c == L'\\') r += L"\\\\";
            else if (c == L'\r') r += L"\\r";
            else if (c == L'\n') r += L"\\n";
            else if (c == L'=') r += L"\\e";
            else if (boundary && c == L' ') r += L"\\b";
            else if (boundary && c == L'\t') r += L"\\t";
            else if (boundary && c == L'"') r += L"\\q";
            else r += c;
        }
        return r;
    }

    // 反转义字段，还原特殊字符
    static inline std::wstring UnescapeField(const std::wstring& s) {
        std::wstring r;
        r.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == L'\\' && i + 1 < s.size()) {
                if (s[i + 1] == L'p') { r += L'|'; ++i; }
                else if (s[i + 1] == L's') { r += L';'; ++i; }
                else if (s[i + 1] == L'\\') { r += L'\\'; ++i; }
                else if (s[i + 1] == L'r') { r += L'\r'; ++i; }
                else if (s[i + 1] == L'n') { r += L'\n'; ++i; }
                else if (s[i + 1] == L'e') { r += L'='; ++i; }
                else if (s[i + 1] == L'b') { r += L' '; ++i; }
                else if (s[i + 1] == L't') { r += L'\t'; ++i; }
                else if (s[i + 1] == L'q') { r += L'"'; ++i; }
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

    // 只保留合法的热键修饰键位：手改 INI 写入非法位会让 RegisterHotKey 永远失败
    static inline UINT SanitizeMods(int v) {
        return (UINT)(v & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN));
    }

    // 虚拟键码合法范围：1~254（0 与更大的值无法被 RegisterHotKey/SendInput 识别）
    static inline bool IsValidVK(int vk) {
        return vk > 0 && vk < 255;
    }

    // 是否为修饰键（Shift/Ctrl/Alt/Win 及其左右版本）。
    // 修饰键不能作为按键映射的源键：拦截后会导致整个修饰键失效。
    static inline bool IsModifierKey(int vk) {
        return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
               vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
               vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
               vk == VK_LWIN || vk == VK_RWIN;
    }

    // 禁止加入「禁用按键」列表的修饰键：Ctrl/Shift/Alt 一旦被屏蔽，
    // 所有依赖它们的组合键都会失效（如 Ctrl+C/V），属于不可接受的误配置。
    // Win 键允许禁用（游戏场景常见），故不在此列。
    static inline bool IsDangerousDisabledKey(int vk) {
        return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
               vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
               vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU;
    }

    // 安全地将宽字符串解析为整数，解析失败返回默认值。
    // 必须完整消费整段文本（允许首尾空白）：std::stoi 接受 "12abc" 这类前缀匹配，
    // 手改 INI 写入的错误值会被静默当作合法值
    static inline int SafeParseInt(const std::wstring& s, int defVal = 0) {
        const size_t first = s.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos) return defVal;
        const size_t last = s.find_last_not_of(L" \t\r\n");
        const std::wstring token = s.substr(first, last - first + 1);
        try {
            size_t pos = 0;
            int val = std::stoi(token, &pos);
            return (pos == token.size()) ? val : defVal;
        } catch (...) {
            return defVal;
        }
    }

    // 无符号 32 位解析：PID 等字段可能超过 INT_MAX，
    // 使用 stoi 会把大 PID 抛成 0，导致绑定快照丢失。
    static inline DWORD SafeParseDWord(const std::wstring& s, DWORD defVal = 0) {
        const size_t first = s.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos) return defVal;
        const size_t last = s.find_last_not_of(L" \t\r\n");
        const std::wstring token = s.substr(first, last - first + 1);
        try {
            size_t pos = 0;
            const unsigned long long val = std::stoull(token, &pos);
            return (pos == token.size() && val <= 0xFFFFFFFFull) ? (DWORD)val : defVal;
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
    std::wstring description;           // 热键描述/显示名称
    HotkeyWindowState windowState = HotkeyWindowState::Normal; // 启动后窗口状态
    bool confirmBeforeRun = false;       // 执行前是否需要确认
    bool runAsAdmin = false;             // 是否以管理员权限运行
    bool simulateKey = false;            // 触发动作类型：true=模拟按键（targetVK 为被模拟键），false=运行命令
    UINT targetVK = 0;                   // 模拟按键模式下被模拟的按键虚拟键码
    UINT targetMod = 0;                  // 模拟按键模式下目标组合的修饰键（0 表示单键）

    // 将自定义热键序列化为管道分隔的字符串（当前规范共 11 个字段）
    // 说明：早期版本中的 sound/opacity/autoStart 字段已废弃，不再写入
    // 模拟组合键（targetMod 非 0）时追加第 12 个字段，保持旧配置格式不变
    std::wstring Serialize() const {
        std::wstring result =
               std::to_wstring(mod) + L"|" + std::to_wstring(vk) + L"|" +
               ConfigUtils::EscapeField(command) + L"|" + ConfigUtils::EscapeField(parameters) + L"|" +
               ConfigUtils::EscapeField(workDir) + L"|" +
               ConfigUtils::EscapeField(description) + L"|" +
               std::to_wstring((int)windowState) + L"|" +
               IniUtils::BoolToStr(confirmBeforeRun) + L"|" +
               IniUtils::BoolToStr(runAsAdmin) + L"|" +
               IniUtils::BoolToStr(simulateKey) + L"|" +
               std::to_wstring(targetVK);
        // 仅模拟组合键才追加第 12 字段。命令型热键即使残留 targetMod，
        // 也不能写出 12 字段，否则会与旧 sound/opacity/autoStart 格式产生歧义。
        if (simulateKey && targetMod != 0) result += L"|" + std::to_wstring(targetMod);
        return result;
    }

    // 从管道分隔的字符串反序列化自定义热键
    // 兼容三种历史格式（按管道 | 的数量区分）：
    //   新格式  11 字段(10 个 |)：已移除废弃字段 sound/opacity/autoStart
    //   新格式  12 字段(11 个 |)：在第 11 字段后追加模拟组合键修饰键 targetMod
    //   旧格式  12/13 字段(11/12 个 |)：含 sound/opacity/autoStart
    //   更旧    14 字段(13 个 |)：额外含 simulateKey/targetVK
    static CustomHotkey Deserialize(const std::wstring& str) {
        CustomHotkey hk;
        std::vector<size_t> pipes;
        for (size_t i = 0; i < str.size(); i++) {
            if (str[i] == L'|') pipes.push_back(i);
        }

        // --- 新格式：12 字段，末尾追加了 targetMod ---
        // 与旧 12 字段格式区分：新格式第 11 字段为目标键码（>=2），
        // 第 12 字段为修饰键位（1..15）；旧格式这两个位置是布尔值 confirm/runAsAdmin
        if (pipes.size() == 11) {
            int vkVal = ConfigUtils::SafeParseInt(str.substr(pipes[9] + 1, pipes[10] - pipes[9] - 1));
            int modVal = ConfigUtils::SafeParseInt(str.substr(pipes[10] + 1));
            if (vkVal >= 2 && modVal >= 1 && modVal <= 15) {
                hk.mod = (UINT)ConfigUtils::SafeParseInt(str.substr(0, pipes[0]));
                hk.vk = (UINT)ConfigUtils::SafeParseInt(str.substr(pipes[0] + 1, pipes[1] - pipes[0] - 1));
                hk.command = ConfigUtils::UnescapeField(str.substr(pipes[1] + 1, pipes[2] - pipes[1] - 1));
                hk.parameters = ConfigUtils::UnescapeField(str.substr(pipes[2] + 1, pipes[3] - pipes[2] - 1));
                hk.workDir = ConfigUtils::UnescapeField(str.substr(pipes[3] + 1, pipes[4] - pipes[3] - 1));
                hk.description = ConfigUtils::UnescapeField(str.substr(pipes[4] + 1, pipes[5] - pipes[4] - 1));
                hk.windowState = ConfigUtils::ClampWindowState(ConfigUtils::SafeParseInt(str.substr(pipes[5] + 1, pipes[6] - pipes[5] - 1)));
                hk.confirmBeforeRun = ConfigUtils::SafeParseInt(str.substr(pipes[6] + 1, pipes[7] - pipes[6] - 1)) != 0;
                hk.runAsAdmin = ConfigUtils::SafeParseInt(str.substr(pipes[7] + 1, pipes[8] - pipes[7] - 1)) != 0;
                hk.simulateKey = ConfigUtils::SafeParseInt(str.substr(pipes[8] + 1, pipes[9] - pipes[8] - 1)) != 0;
                hk.targetVK = (UINT)vkVal;
                hk.targetMod = (UINT)modVal;
                // 命令型热键不应携带模拟目标；历史数据可能残留，加载时归一化，
                // 避免保存后继续写出 12 字段并放大格式歧义。
                if (!hk.simulateKey) {
                    hk.targetVK = 0;
                    hk.targetMod = 0;
                }
                return hk;
            }
        }

        // --- 旧格式：含已废弃的 sound / opacity / autoStart 字段 ---
        if (pipes.size() >= 11) {
            hk.mod = (UINT)ConfigUtils::SafeParseInt(str.substr(0, pipes[0]));
            hk.vk = (UINT)ConfigUtils::SafeParseInt(str.substr(pipes[0] + 1, pipes[1] - pipes[0] - 1));
            hk.command = ConfigUtils::UnescapeField(str.substr(pipes[1] + 1, pipes[2] - pipes[1] - 1));
            hk.parameters = ConfigUtils::UnescapeField(str.substr(pipes[2] + 1, pipes[3] - pipes[2] - 1));
            hk.workDir = ConfigUtils::UnescapeField(str.substr(pipes[3] + 1, pipes[4] - pipes[3] - 1));
            // pipes[4]~pipes[5] 为废弃的 sound 字段，直接丢弃
            hk.description = ConfigUtils::UnescapeField(str.substr(pipes[5] + 1, pipes[6] - pipes[5] - 1));
            hk.windowState = ConfigUtils::ClampWindowState(ConfigUtils::SafeParseInt(str.substr(pipes[6] + 1, pipes[7] - pipes[6] - 1)));
            // pipes[7]~pipes[8] 为废弃的 opacity、pipes[8]~pipes[9] 为废弃的 autoStart，直接丢弃
            hk.confirmBeforeRun = ConfigUtils::SafeParseInt(str.substr(pipes[9] + 1, pipes[10] - pipes[9] - 1)) != 0;
            // runAsAdmin：13/14 字段格式需截断到第 12 个管道，12 字段格式取到结尾
            hk.runAsAdmin = ConfigUtils::SafeParseInt(str.substr(pipes[10] + 1,
                (pipes.size() >= 12 ? pipes[11] : str.size()) - pipes[10] - 1)) != 0;
            // 模拟按键字段：仅当存在第 13/14 字段时解析
            if (pipes.size() >= 13) {
                hk.simulateKey = ConfigUtils::SafeParseInt(str.substr(pipes[11] + 1, pipes[12] - pipes[11] - 1)) != 0;
                hk.targetVK = (UINT)ConfigUtils::SafeParseInt(str.substr(pipes[12] + 1));
            }
            // 命令型热键不保留模拟目标，与 11/12 字段格式保持一致
            if (!hk.simulateKey) {
                hk.targetVK = 0;
                hk.targetMod = 0;
            }
            return hk;
        }

        // --- 新格式：11 个字段 ---
        if (pipes.size() == 10) {
            hk.mod = (UINT)ConfigUtils::SafeParseInt(str.substr(0, pipes[0]));
            hk.vk = (UINT)ConfigUtils::SafeParseInt(str.substr(pipes[0] + 1, pipes[1] - pipes[0] - 1));
            hk.command = ConfigUtils::UnescapeField(str.substr(pipes[1] + 1, pipes[2] - pipes[1] - 1));
            hk.parameters = ConfigUtils::UnescapeField(str.substr(pipes[2] + 1, pipes[3] - pipes[2] - 1));
            hk.workDir = ConfigUtils::UnescapeField(str.substr(pipes[3] + 1, pipes[4] - pipes[3] - 1));
            hk.description = ConfigUtils::UnescapeField(str.substr(pipes[4] + 1, pipes[5] - pipes[4] - 1));
            hk.windowState = ConfigUtils::ClampWindowState(ConfigUtils::SafeParseInt(str.substr(pipes[5] + 1, pipes[6] - pipes[5] - 1)));
            hk.confirmBeforeRun = ConfigUtils::SafeParseInt(str.substr(pipes[6] + 1, pipes[7] - pipes[6] - 1)) != 0;
            hk.runAsAdmin = ConfigUtils::SafeParseInt(str.substr(pipes[7] + 1, pipes[8] - pipes[7] - 1)) != 0;
            hk.simulateKey = ConfigUtils::SafeParseInt(str.substr(pipes[8] + 1, pipes[9] - pipes[8] - 1)) != 0;
            hk.targetVK = (UINT)ConfigUtils::SafeParseInt(str.substr(pipes[9] + 1));
            // 命令型热键不保留模拟目标，避免保存后反复写出 targetVK/targetMod
            if (!hk.simulateKey) {
                hk.targetVK = 0;
                hk.targetMod = 0;
            }
        }
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

    // 序列化为管道分隔的字符串。序列化前统一截断字段，
    // 保证长度预算检查与实际写出的内容一致（否则保存成功但下次加载被截断）。
    std::wstring Serialize() const {
        BoundWindowInfo copy = *this;
        copy.ClampFields();
        return std::to_wstring((int)copy.matchMode) + L"|" + ConfigUtils::EscapeField(copy.title) + L"|" +
               ConfigUtils::EscapeField(copy.processName) + L"|" + std::to_wstring(copy.pid) + L"|" +
               ConfigUtils::EscapeField(copy.processPath);
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
            info.pid = ConfigUtils::SafeParseDWord(str.substr(p2 + 1));
            return info;
        }
        info.pid = ConfigUtils::SafeParseDWord(str.substr(p2 + 1, p3 - p2 - 1));
        info.processPath = ConfigUtils::UnescapeField(str.substr(p3 + 1));
        return info;
    }

    // 判断指定窗口是否匹配此绑定信息
    bool Matches(HWND hwnd, bool requireVisible = true) const;

    // 判断进程可执行名/完整路径是否匹配此绑定信息（供带缓存的匹配复用）
    bool MatchesProcessInfo(const std::wstring& exeName, const std::wstring& fullPath) const;
    // 判断窗口是否匹配，使用进程路径缓存避免重复 OpenProcess（仅 Process 模式受益）。
    // cachedTitle 非空时复用调用方已读取的窗口标题，避免同一次枚举内对每个绑定
    // 重复执行跨进程 WM_GETTEXT（存在无响应窗口时会累加成秒级阻塞）
    bool Matches(HWND hwnd, ProcessUtils::PathCache& cache, bool requireVisible = true,
                 const std::wstring* cachedTitle = nullptr) const;

    // 截断超长字段：手改 INI 或超长进程路径进入序列化前统一收敛，
    // 避免单个绑定把整个 INI 值撑过 profile API 的可靠读写上限
    void ClampFields() {
        if (title.size() > MAX_BINDING_TITLE) title.resize(MAX_BINDING_TITLE);
        if (processName.size() > MAX_BINDING_NAME) processName.resize(MAX_BINDING_NAME);
        if (processPath.size() > MAX_BINDING_PATH) processPath.resize(MAX_BINDING_PATH);
    }

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
    // 枚举窗口列表时读取标题的总预算（毫秒）
    inline constexpr ULONGLONG WINDOW_ENUM_TITLE_BUDGET_MS = 300;

    // 枚举上下文，传递给 EnumWindows 回调
    struct EnumWindowCtx {
        std::vector<BoundWindowInfo>* cache;    // 缓存未绑定窗口信息
        const std::vector<BoundWindowInfo>* bound; // 已绑定窗口列表
        HWND lstAll;                            // 列表框控件句柄
        // 单次枚举内复用进程路径，避免同一进程的多个窗口重复 OpenProcess
        ProcessUtils::PathCache pathCache;
        // 标题读取的整轮预算：多个无响应窗口各 50ms 会让设置界面卡住数秒。
        // 默认值保证未来新增调用方忘记赋值时也不会退化为无预算读取。
        ULONGLONG deadline = GetTickCount64() + WINDOW_ENUM_TITLE_BUDGET_MS;
        bool budgetExceeded = false;
    };

    // 枚举回调：32 位下 GCC 的 lambda 无法隐式转换为 stdcall 的 WNDENUMPROC，
    // 故用显式 CALLBACK 约定的命名函数
    inline BOOL CALLBACK EnumUnboundProc(HWND hwnd, LPARAM lParam) {
            if (!IsWindowVisible(hwnd)) return TRUE;
            // 跳过桌面/任务栏等外壳窗口：这些窗口不允许被隐藏，绑定后也不会生效
            if (WindowUtils::IsShellWindow(hwnd)) return TRUE;
            auto* ctx = reinterpret_cast<EnumWindowCtx*>(lParam);
            // 安全读取标题：此枚举在设置窗口的 UI 线程上执行，
            // 不能因某个无响应窗口的 WM_GETTEXT 把设置界面卡死。
            // 预算耗尽后停止读标题，但仍通过进程名收录窗口。
            std::wstring title;
            if (GetTickCount64() < ctx->deadline) {
                title = WindowUtils::GetTextSafe(hwnd);
            } else {
                ctx->budgetExceeded = true;
            }
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == 0 || pid == GetCurrentProcessId()) return TRUE;

            // 复用缓存查询进程路径：同一进程的多个窗口只打开一次进程
            const ProcessUtils::ProcPathInfo& proc = ctx->pathCache.Get(pid);
            const std::wstring& processPath = proc.path;
            const std::wstring& processName = proc.name;
            // 标题预算耗尽后无法区分“真正无标题的窗口”和“标题未读取的窗口”：
            // 不能把后者当作无标题窗口降级为进程绑定，否则用户会误绑整个进程（R13-A6）
            if (title.empty() && ctx->budgetExceeded) return TRUE;
            // 无标题窗口仍可能按进程名绑定；只有标题和进程名都拿不到时才跳过
            if (title.empty() && processName.empty()) return TRUE;

            // 跳过已绑定的窗口。
            // 不能用 b.pid 判断：pid 是绑定当时的快照（已持久化），重启后会被系统复用，
            // 会把无关进程的窗口误判为已绑定而从列表中消失。
            // 必须按匹配模式分别比较：标题模式只比较标题，进程模式才比较进程名/路径。
            // 否则按窗口名绑定时顺带保存的进程名会把同进程的其他窗口全部隐藏掉（R9-M2）
            bool alreadyBound = false;
            for (const auto& b : *ctx->bound) {
                if (b.matchMode == MatchMode::Process) {
                    if ((!b.processName.empty() && _wcsicmp(b.processName.c_str(), processName.c_str()) == 0) ||
                        (!b.processPath.empty() && _wcsicmp(b.processPath.c_str(), processPath.c_str()) == 0)) {
                        alreadyBound = true;
                        break;
                    }
                } else if (!b.title.empty() && b.title == title) {
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
                // 无标题窗口只能按进程绑定，列表中也以进程模式显示
                info.matchMode = title.empty() ? MatchMode::Process : MatchMode::Window;
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
// KeyRemapEntry: 按键映射项
// 源键 -> 目标单键或组合键；mods 为 0 表示映射为单键。
// ============================================================
struct KeyRemapEntry {
    int sourceVK = 0;   // 源键虚拟键码
    UINT mods = 0;      // 目标组合修饰键（MOD_CONTROL/MOD_SHIFT/MOD_ALT/MOD_WIN）
    int targetVK = 0;   // 目标主键虚拟键码
    int swapGroup = 0;  // >0：由「按键互换」生成的联动组（A→B 与 B→A 同组）
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
    int overlayPosition = 0;              // 浮窗位置：0=底部 1=顶部 2=居中 3=左侧 4=右侧
    int overlayTheme = 0;                 // 浮窗颜色：0=深色 1=浅色
    int overlayCorner = 1;                // 浮窗边框：0=直角 1=圆角（默认）
    int overlayOpacity = 2;               // 浮窗透明度：0=高透明 1=适中 2=低透明(默认) 3=不透明
    int overlayDelay = 1;                 // 浮窗延时：0=快速(0.5s) 1=适中(1s) 2=缓慢(2s) 3=保持显示
    bool forceNumLockOn = false;          // 强制开启 NumLock
    bool disableStartupNotification = false; // 禁用启动通知
    int appTheme = 0;                     // 外观：0=跟随系统 1=浅色 2=深色
    int appLanguage = 0;                  // 语言：0=中文 1=English
    // 资源管理器文件夹选项（0=不改变, 1=开启隐藏, 2=关闭隐藏即显示）
    int explorerHideHidden = 0;           // 隐藏文件和文件夹
    int explorerHideExt = 0;              // 隐藏已知文件类型的扩展名
    int explorerHideOS = 0;               // 隐藏受保护的操作系统文件
    bool disableSpecifiedKeysEnabled = false; // 启用禁用指定按键
    bool disableNumLock = false;          // 禁用 NumLock 键
    std::vector<int> disabledKeyCodes;    // 被禁用的按键虚拟键码列表

    // --- 按键重映射 ---
    bool keyRemapEnabled = false;         // 启用按键重映射
    std::vector<KeyRemapEntry> keyRemappings; // 重映射项：源键 -> 单键或组合键

    // --- 自定义热键 ---
    bool customHotkeysEnabled = false;    // 启用自定义热键
    std::vector<CustomHotkey> customHotkeys; // 自定义热键列表

    // --- 老板键设置 ---
    bool bossKeyEnabled = false;          // 启用老板键
    UINT bossKeyMod = MOD_WIN;            // 老板键修饰键（默认 Win）
    UINT bossKeyVK = 0xC0;                // 老板键虚拟键（默认 ` 键）
    UINT bossKeyCloseMod = MOD_WIN;       // 一键关闭修饰键（默认 Win）
    UINT bossKeyCloseVK = VK_ESCAPE;      // 一键关闭虚拟键（默认 Esc，即 Win+Esc）
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
    // 配置文件存在但无法读取时置位：本次以默认值运行，并拒绝用默认值覆盖原文件，
    // 避免权限/占用导致的瞬时读取失败在退出时把用户配置整体冲掉
    bool loadFailed = false;
    // 读取失败是否由编码不符引起（非 UTF-16LE，常见于记事本另存为 UTF-8/ANSI）。
    // 单独记录以便启动提示给出可操作的修复方式，而不是笼统的"读取失败"（R11-M3）
    bool loadFailedEncoding = false;
    bool LoadFailed() const { return loadFailed; }
    bool LoadFailedEncoding() const { return loadFailedEncoding; }

    void Load(const std::wstring& iniPath);
    // 保存配置到 INI（原子写入）；返回 false 表示落盘失败（目录只读/磁盘满/权限不足）
    bool Save(const std::wstring& iniPath) const;

    // 虚拟键码转可读名称
    static std::wstring VKToName(int vk);
    // 可读名称转虚拟键码
    static int NameToVK(const std::wstring& name);
};
