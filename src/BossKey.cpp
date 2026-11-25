#include "BossKey.h"
#include "Resource.h"
#include "Utils.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <string.h>
#include <unordered_set>
#include <ShlObj.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

// 枚举预算耗尽的提示：同一会话内只提示一次，下一次枚举完整完成后复位，
// 使后续新的预算耗尽仍能提醒用户
static bool s_hideBudgetWarned = false;

std::atomic<BossKey*> BossKey::s_instance{ nullptr };

namespace {
    // 鼠标钩子回调的入口闸门与在途计数（R14-M2）。
    // 析构时先关闸、置空 s_instance，再等待在途回调归零，之后才析构宿主成员；
    // 钩子句柄放在对象外，回调在宿主析构后调用 CallNextHookEx 也不再解引用 this。
    std::atomic<bool> s_mouseCallbackGate{ true };
    std::atomic<int> s_mouseCallbackCount{ 0 };
    std::atomic<HHOOK> s_mouseHookHandle{ nullptr };
}

// 异步隐藏的延迟确认时长：给"短暂无响应"的目标进程留出恢复并处理隐藏请求的时间。
// 过短会漏判，过长会让老板键在这段时间内表现为"未激活"
static const UINT BOSSKEY_VERIFY_DELAY_MS = 800;
// 隐藏确认的最大重试次数：目标繁忙时 ShowWindowAsync 可能 800ms 后才生效，
// 首次确认仍看到窗口可见时不能立即清账，否则迟到生效的隐藏将无法恢复
static const int BOSSKEY_VERIFY_MAX_RETRIES = 5;
// 单轮窗口枚举的标题读取预算：存在无响应窗口时，逐窗口 50ms 的超时会累加，
// 预算用尽即停止枚举，剩余窗口留给下一次触发/定时检查
static constexpr ULONGLONG ENUM_TITLE_BUDGET_MS = 300;

// 恢复文件大小上限：正常只有少量记录行，异常膨胀（磁盘损坏/被写坏）时
// 放弃读取，避免整文件读入导致内存暴涨或 32 位长度溢出
static constexpr LONGLONG MAX_RECOVER_FILE_BYTES = 1024 * 1024;
// 写入端字符上限：UTF-8 编码后仍低于读取端的 1MB 限制，
// 避免本程序写出的恢复文件超出自身可读取范围（R9-A3）
static constexpr size_t MAX_RECOVER_WRITE_CHARS = 240 * 1024;
static bool s_recoverTruncatedWarned = false;
// 单次恢复最多处理的行数：每条失败记录都要 EnumWindows 并等待显示生效，
// 上万行会让启动长时间卡顿
static constexpr int MAX_RECOVER_LINES = 256;
// 恢复阶段总时间预算：用尽后剩余记录原样保留，下次启动继续恢复
static constexpr ULONGLONG RECOVER_TIME_BUDGET_MS = 1500;

// 恢复文件中标题字段的转义：标题可能包含换行/制表符（部分程序允许），
// 直接写入会破坏按行/按字段解析，导致该窗口永远无法恢复
static std::wstring EscapeRecoverTitle(const std::wstring& s) {
    std::wstring r;
    r.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L'\\') r += L"\\\\";
        else if (c == L'\t') r += L"\\t";
        else if (c == L'\r') r += L"\\r";
        else if (c == L'\n') r += L"\\n";
        else r += c;
    }
    return r;
}

// 严格解析恢复文件中的十进制字段：必须整段为数字（允许前导 '-'），
// 与 WinDGuard 的锁文件解析保持一致，避免 "123abc" 被当作 123
static bool ParseRecoverUnsigned(const std::wstring& s, unsigned long long* out) {
    if (s.empty() || !out) return false;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') return false;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long long v = wcstoull(s.c_str(), &end, 10);
    if (errno == ERANGE || end != s.c_str() + s.size()) return false;
    *out = v;
    return true;
}

static bool ParseRecoverSigned(const std::wstring& s, long long* out) {
    if (s.empty() || !out) return false;
    size_t i = (s[0] == L'-') ? 1 : 0;
    if (i >= s.size()) return false;
    for (size_t j = i; j < s.size(); ++j) {
        if (s[j] < L'0' || s[j] > L'9') return false;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const long long v = wcstoll(s.c_str(), &end, 10);
    if (errno == ERANGE || end != s.c_str() + s.size()) return false;
    *out = v;
    return true;
}

static bool s_recoverSaveWarned = false;

static void WarnRecoverSaveFailedOnce() {
    if (s_recoverSaveWarned) return;
    s_recoverSaveWarned = true;
    ShowTrayNotification(L"崩溃恢复记录写入失败：若程序异常退出，被隐藏的窗口可能无法自动恢复。", true);
}

static void ResetRecoverSaveWarn() { s_recoverSaveWarned = false; }

static std::wstring UnescapeRecoverTitle(const std::wstring& s) {
    std::wstring r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            const wchar_t n = s[i + 1];
            if (n == L'\\') { r += L'\\'; ++i; }
            else if (n == L't') { r += L'\t'; ++i; }
            else if (n == L'r') { r += L'\r'; ++i; }
            else if (n == L'n') { r += L'\n'; ++i; }
            else r += s[i];
        } else {
            r += s[i];
        }
    }
    return r;
}

// 静音端点恢复记录（m5）：保存崩溃前被本程序静音的端点身份与原始状态。
// 恢复时按端点 ID 匹配，避免崩溃期间默认设备切换后被误恢复到新设备（R15-M07）
static std::wstring MakeMuteRecoverLine(const std::wstring& deviceId,
                                        bool wasMuted, bool valid) {
    std::wstring line = L"m5\t";
    line += EscapeRecoverTitle(deviceId);
    line += L'\t';
    line += wasMuted ? L'1' : L'0';
    line += L'\t';
    line += valid ? L'1' : L'0';
    line += L'\n';
    return line;
}

// ============================================================
// 跨进程窗口操作的非阻塞辅助
// 背景：这些操作运行在承载 WH_MOUSE_LL 鼠标钩子的 UI 线程上。
//       GetWindowTextW / ShowWindow 会向目标窗口线程同步发送消息，
//       目标进程（如浏览器）一时无响应时会把该线程卡住，导致低级
//       鼠标钩子无法派发，表现为"全系统鼠标点击无响应、键盘正常"。
// ============================================================
namespace {
    // 系统当前的输入模式：鼠标捕获窗口、菜单循环宿主、移动/缩放循环宿主。
    // 这三者任意一个残留在"已隐藏的窗口"上，都会让全系统鼠标点击只送给
    // 该窗口：目标窗口的最小化/最大化/关闭按钮、任务栏点击全部无响应，
    // 而键盘输入正常；只有切到 Ctrl+Alt+Del 安全桌面再返回才会被动解除
    struct InputModeState {
        HWND capture = nullptr;
        HWND menuOwner = nullptr;
        HWND moveSize = nullptr;
    };

    // 查询某个输入队列上的残留输入模式；threadId 为 0 表示前台输入队列。
    // 鼠标捕获只在按键按下期间对后台线程生效，隐藏后前台往往已切到别的
    // 线程，因此只查前台队列会漏掉仍滞留在原线程上的捕获
    InputModeState QueryInputModeState(DWORD threadId = 0) {
        InputModeState st;
        GUITHREADINFO gti = { sizeof(gti) };
        if (GetGUIThreadInfo(threadId, &gti)) {
            st.capture = gti.hwndCapture;
            st.menuOwner = gti.hwndMenuOwner;
            st.moveSize = gti.hwndMoveSize;
        }
        return st;
    }

    // 两个窗口是否属于同一棵窗口树（同一顶层窗口）
    bool IsSameWindowTree(HWND a, HWND b) {
        if (!a || !b) return false;
        HWND ra = GetAncestor(a, GA_ROOT);
        HWND rb = GetAncestor(b, GA_ROOT);
        return ra != nullptr && ra == rb;
    }

    // 单次跨进程发送超时：不使用 SMTO_BLOCK（它要求等待期间不处理
    // 任何其它消息，会让承载鼠标钩子的线程停止派发），目标无响应时 50ms 放弃。
    // deadline 是"整个窗口/整轮处理"共享的时间预算：目标只是"响应但缓慢"时，
    // 每次最大 50ms 的等待会累加（多个模式对象 × 多个绑定窗口），用户按一次
    // 热键就能感到输入停顿，因此预算用尽即停止发送，交给下一轮重试
    constexpr UINT CANCEL_MODE_TIMEOUT_MS = 50;
    constexpr ULONGLONG CANCEL_MODE_BUDGET_MS = 80;

    void SendCancelModeBounded(HWND hwnd, ULONGLONG deadline) {
        if (!hwnd || !IsWindow(hwnd)) return;
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) return;  // 预算已用尽：本轮跳过，下次重试
        UINT timeout = (UINT)std::min<ULONGLONG>(CANCEL_MODE_TIMEOUT_MS, deadline - now);
        DWORD_PTR result = 0;
        SendMessageTimeoutW(hwnd, WM_CANCELMODE, 0, 0, SMTO_ABORTIFHUNG, timeout, &result);
    }

    // 强制解除系统鼠标捕获：捕获是系统级独占，本线程临时抢占再释放，
    // 原持有窗口会收到 WM_CAPTURECHANGED 并失去捕获（跨进程同样有效）。
    // 仅在目标不处理 WM_CANCELMODE 时使用（Chromium 等自绘窗口常见）
    void ForceReleaseMouseCapture(HWND hostWnd) {
        if (!hostWnd || !IsWindow(hostWnd)) return;
        SetCapture(hostWnd);
        ReleaseCapture();
    }

    // 取消目标窗口的内部模式（鼠标捕获/菜单/移动缩放循环）。
    // 必须针对"真正持有捕获的那个窗口"发送：Chromium 等程序捕获的是子窗口，
    // 只给顶层窗口发 WM_CANCELMODE 解除不了，隐藏后该窗口仍独占鼠标输入
    void CancelWindowModes(HWND target, HWND captureHost) {
        if (!IsWindow(target)) return;

        // 整窗共享一份预算：多个模式对象/两个消息队列叠加时不会超出上限
        ULONGLONG deadline = GetTickCount64() + CANCEL_MODE_BUDGET_MS;

        // 目标线程与前台线程可能不同：两个队列上的残留模式都要处理
        DWORD tid = GetWindowThreadProcessId(target, nullptr);
        InputModeState states[2] = {
            QueryInputModeState(0),
            (tid && tid != GetCurrentThreadId()) ? QueryInputModeState(tid) : InputModeState()
        };
        for (const InputModeState& st : states) {
            if (st.capture && IsSameWindowTree(st.capture, target)) {
                SendCancelModeBounded(st.capture, deadline);
                // 自定义 WndProc 可能不把 WM_CANCELMODE 交给 DefWindowProc，
                // 此时只能由本线程强行夺取捕获再释放（重复夺取无害）
                ForceReleaseMouseCapture(captureHost);
            }
            if (st.moveSize && IsSameWindowTree(st.moveSize, target)) {
                SendCancelModeBounded(st.moveSize, deadline);
            }
            if (st.menuOwner && IsSameWindowTree(st.menuOwner, target)) {
                SendCancelModeBounded(st.menuOwner, deadline);
            }
        }
    }

    // 有界探测窗口是否可响应：用于"抢焦点"这类会同步等待目标线程的操作之前，
    // 目标繁忙时宁可跳过，也不要让承载鼠标钩子的 UI 线程一起被挂住
    bool WindowResponds(HWND hwnd, UINT timeoutMs = 120) {
        if (!IsWindow(hwnd) || IsHungAppWindow(hwnd)) return false;
        DWORD_PTR result = 0;
        return SendMessageTimeoutW(hwnd, WM_NULL, 0, 0,
                                   SMTO_ABORTIFHUNG, timeoutMs, &result) != 0;
    }

    // 安全地显示/隐藏窗口，返回 true 表示走了异步投递。
    // 对其他进程的窗口一律异步：ShowWindow 会同步等待目标线程处理消息，
    // 目标只需短暂繁忙（脚本、同步 IO、弹模态）就足以把承载低级键鼠钩子的
    // UI 线程一起挂住；IsHungAppWindow 只识别已无响应(≥5s)的窗口，不能依赖。
    bool SetWindowShowState(HWND hwnd, int showCmd) {
        if (!IsWindow(hwnd)) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId() || IsHungAppWindow(hwnd)) {
            return ShowWindowAsync(hwnd, showCmd) != FALSE;
        }
        ShowWindow(hwnd, showCmd);
        return false;
    }

    // 安全获取"恢复时要用哪个显示状态"：不调用 GetWindowPlacement，
    // 它是跨进程查询、目标繁忙时会阻塞承载鼠标钩子的 UI 线程；
    // IsIconic/IsZoomed 由内核按窗口样式位直接回答，不跨线程、不会阻塞。
    // 附带好处：窗口自身处于隐藏状态时 WINDOWPLACEMENT.showCmd 可能返回 SW_HIDE，
    // 一旦按它恢复就会"越恢复越隐藏"，改用样式位可避免这种记录
    int GetShowCmdSafe(HWND hwnd) {
        if (IsIconic(hwnd)) return SW_SHOWMINIMIZED;
        if (IsZoomed(hwnd)) return SW_SHOWMAXIMIZED;
        return SW_SHOWNORMAL;
    }

    // 有界等待异步窗口操作生效：最多 100ms 且与窗口数量无关，
    // 既不长时间阻塞钩子线程，又能识别真正失败（如提权窗口无法隐藏）
    void WaitForVisibility(const std::vector<HWND>& hwnds, bool expectVisible) {
        for (int i = 0; i < 10; ++i) {
            bool settled = true;
            for (HWND h : hwnds) {
                if (!IsWindow(h)) continue;
                if ((IsWindowVisible(h) != FALSE) != expectVisible && !IsHungAppWindow(h)) {
                    settled = false;
                    break;
                }
            }
            if (settled) return;
            Sleep(10);
        }
    }

    // 关键系统进程不参与强制关闭，避免误绑 explorer.exe 等导致桌面/登录会话异常
    // cache：复用进程路径查询结果，避免同一 pid 在一次关闭流程中被反复 OpenProcess
    bool IsCriticalProcess(DWORD pid, ProcessUtils::PathCache& cache) {
        const std::wstring& name = cache.Get(pid).name;
        static const wchar_t* critical[] = {
            L"explorer.exe", L"winlogon.exe", L"csrss.exe", L"wininit.exe",
            L"services.exe", L"smss.exe", L"lsass.exe", L"dwm.exe",
            L"svchost.exe", L"taskhostw.exe", L"audiodg.exe", L"LogonUI.exe",
            L"sihost.exe", L"fontdrvhost.exe", L"MemCompression",
            L"System", L"Registry"
        };
        for (auto n : critical) {
            if (_wcsicmp(name.c_str(), n) == 0) return true;
        }
        return false;
    }
}

BossKey::BossKey() {
    s_mouseCallbackGate.store(true, std::memory_order_release);
    s_mouseCallbackCount.store(0, std::memory_order_release);
    s_mouseHookHandle.store(nullptr, std::memory_order_release);
    s_instance.store(this, std::memory_order_release);
}

BossKey::~BossKey() {
    // 先关闭回调入口并置空单例，等待在途回调离开，再停止钩子线程（R14-M2）
    s_mouseCallbackGate.store(false, std::memory_order_release);
    s_instance.store(nullptr, std::memory_order_release);
    for (int i = 0; i < 2000 && s_mouseCallbackCount.load(std::memory_order_acquire) > 0; ++i) {
        Sleep(1);
    }
    UninstallMouseHook();
    Unregister();
    // 析构时恢复所有隐藏的窗口并取消静音（异常退出路径的兜底）；
    // 正常退出/关机路径已由 WM_DESTROY / WM_ENDSESSION 调用 Deactivate 覆盖
    if (m_active) Deactivate();
    // 释放缓存的音频设备枚举器（须在 Deactivate 内的 SetMute 之后）
    ReleaseAudio();
}

// 释放缓存的音频 COM 接口：正常退出时应在 CoUninitialize 之前调用
// （g_bossKey 是全局对象，其析构发生在 wWinMain 中的 CoInitGuard 之后，
//  届时再 Release 属于在已反初始化的线程上访问 COM 对象）
void BossKey::ReleaseAudio() {
    if (m_pVolume) { m_pVolume->Release(); m_pVolume = nullptr; }
    if (m_pEnumerator) { m_pEnumerator->Release(); m_pEnumerator = nullptr; }
    m_volumeDeviceId.clear();
    // m_mutedEndpoints 保留到进程结束：退出路径若恢复失败，m_mutedByUs
    // 仍为 true，供恢复文件/下次启动继续兜底解除静音。
}

// 正常退出时解除本程序造成的系统静音。
// CloseBoundProcesses 在“仍有窗口未能关闭”时会保留激活态与静音状态，
// 若等到全局析构的 Deactivate 才解除，COM 已反初始化，SetMute 会失败；
// 因此退出流程必须在 ReleaseAudio/CoUninitialize 之前显式调用一次。
void BossKey::RestoreSystemMute() {
    if (!m_mutedByUs) return;
    // 有逐端点记录时按原始状态恢复：用户原本就静音的端点保持静音，
    // 不能被退出兜底强制解除。只有崩溃恢复（无记录、原状态未知）
    // 才强制解除当前默认端点，避免无声状态持续。
    // 退出阶段静默处理：失败记录已写回恢复文件，无需在退出瞬间弹气泡
    SetMute(false, m_mutedEndpoints.empty(), true);
}

// 注册老板键热键，先注销旧热键再注册新热键
bool BossKey::Register(HWND hwnd, int hotkeyId, UINT modifiers, UINT vk) {
    Unregister();
    m_hwnd = hwnd;
    m_hotkeyId = hotkeyId;
    m_registered = RegisterHotKey(hwnd, hotkeyId, modifiers, vk) != 0;
    return m_registered;
}

// 注销老板键热键
void BossKey::Unregister() {
    if (m_registered && m_hwnd) {
        UnregisterHotKey(m_hwnd, m_hotkeyId);
        m_registered = false;
    }
}

void BossKey::SetWindows(const std::vector<BoundWindowInfo>& windows) { m_boundWindows = windows; }
void BossKey::SetMuteEnabled(bool enabled) { m_muteEnabled = enabled; }
void BossKey::SetHideCurrentWindow(bool enabled) { m_hideCurrentWindow = enabled; }
void BossKey::SetSendPauseBeforeHide(bool enabled) { m_sendPauseBeforeHide = enabled; }

// 枚举阶段捕获的匹配窗口：标题与进程名随结果一起缓存，
// 供隐藏记录写盘复用，避免再次跨进程读取。
struct BossMatchedWindow {
    HWND hwnd;
    std::wstring title;
    std::wstring processName;
};

// EnumWindows 回调的上下文数据
struct EnumBossData {
    const std::vector<BoundWindowInfo>* boundWindows;
    std::vector<BossMatchedWindow>* results;
    ProcessUtils::PathCache* cache;  // 单次枚举内复用进程路径，避免同一 pid 重复 OpenProcess
    bool needTitle;                  // 是否存在按标题匹配的绑定
    ULONGLONG deadline;              // 整轮枚举的标题读取预算
    bool budgetExceeded;             // 是否因预算用尽提前结束枚举
};

// 是否有任何绑定需要读取窗口标题
static bool NeedsWindowTitle(const std::vector<BoundWindowInfo>& windows) {
    for (const auto& w : windows) {
        if (w.matchMode == MatchMode::Window && !w.title.empty()) return true;
    }
    return false;
}

// EnumWindows 回调：收集匹配绑定规则的可见窗口
static BOOL CALLBACK EnumBossWindowsProc(HWND hwnd, LPARAM lParam) {
    if (IsWindowVisible(hwnd) == FALSE) return TRUE;
    // 跳过任务栏/桌面等外壳窗口，避免误绑 explorer.exe 时把外壳一起隐藏
    if (WindowUtils::IsShellWindow(hwnd)) return TRUE;
    // 跳过自身进程的窗口：绑定标题或进程名时不能把设置窗口/关于框一起藏掉
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return TRUE;
    // 提权会话不能被用户可写配置驱动去隐藏更高完整性级别的窗口（R9-A1）。
    // 非提权会话跳过查询，避免枚举期间为每个窗口增加 OpenProcess 开销
    if (ProcessUtils::IsCurrentProcessElevated() &&
        !ProcessUtils::IsProcessIntegrityAllowed(pid)) return TRUE;
    auto* data = reinterpret_cast<EnumBossData*>(lParam);
    // 仅标题读取需要预算：Process 模式不读标题，不应因预算被提前截断
    if (data->needTitle && GetTickCount64() >= data->deadline) {
        data->budgetExceeded = true;
        return FALSE;
    }

    // 每个窗口只读取一次标题，供所有 Window 模式绑定复用
    std::wstring title;
    if (data->needTitle) title = WindowUtils::GetTextSafe(hwnd);

    for (const auto& info : *data->boundWindows) {
        // 使用带缓存的匹配：进程模式复用路径查询结果，标题模式复用本次读取结果
        if (info.Matches(hwnd, *data->cache, true, data->needTitle ? &title : nullptr)) {
            // 仅对真正匹配的窗口补充读取标题/进程名，供恢复文件写盘复用；
            // 进程模式不参与匹配的窗口不会产生额外跨进程调用。
            if (title.empty()) title = WindowUtils::GetTextSafe(hwnd);
            const ProcessUtils::ProcPathInfo& proc = data->cache->Get(pid);
            data->results->push_back({ hwnd, title, proc.name });
            return TRUE;
        }
    }
    return TRUE;
}

// ============================================================
// BossKey::HideWindows - 隐藏所有绑定窗口
// 流程：
//   1. 枚举所有匹配绑定规则的可见窗口
//   2. 可选：隐藏当前活动窗口
//   3. 可选：发送媒体暂停
//   4. 隐藏所有窗口（SW_HIDE）
//   5. 可选：静音系统音频
// ============================================================
void BossKey::HideWindows() {
    // 新一轮隐藏：作废上一次"异步隐藏延迟确认"，避免旧记录干扰本次判定
    if (m_pendingVerify) {
        m_pendingVerify = false;
        if (m_hwnd) KillTimer(m_hwnd, TIMER_BOSSKEY_VERIFY);
    }
    m_hiddenWindows.clear();

    // 枚举匹配绑定规则的窗口（枚举期间复用进程路径缓存）
    std::vector<BossMatchedWindow> matching;
    ProcessUtils::PathCache cache;
    EnumBossData data{ &m_boundWindows, &matching, &cache,
                       NeedsWindowTitle(m_boundWindows),
                       GetTickCount64() + ENUM_TITLE_BUDGET_MS, false };
    EnumWindows(EnumBossWindowsProc, reinterpret_cast<LPARAM>(&data));
    if (data.budgetExceeded) {
        if (!s_hideBudgetWarned) {
            s_hideBudgetWarned = true;
            ShowTrayNotification(L"部分窗口因目标无响应未被处理，请再次触发老板键或稍后重试。", true);
        }
    } else {
        s_hideBudgetWarned = false;
    }

    // 保存窗口信息（显示状态、进程ID、标题、进程名），用于恢复与写盘
    for (const auto& item : matching) {
        DWORD pid = 0;
        GetWindowThreadProcessId(item.hwnd, &pid);
        m_hiddenWindows.push_back({ item.hwnd, GetShowCmdSafe(item.hwnd), pid,
                                    item.title, item.processName });
    }

    // 可选：隐藏当前前台窗口（排除自身进程与桌面/任务栏等外壳窗口）
    if (m_hideCurrentWindow) {
        HWND fg = GetForegroundWindow();
        if (fg) {
            DWORD fgPid = 0;
            GetWindowThreadProcessId(fg, &fgPid);
            if (fgPid == GetCurrentProcessId()) fg = nullptr;
            // 提权会话不隐藏更高完整性级别的窗口（与绑定规则一致，R9-A1）
            if (fg && ProcessUtils::IsCurrentProcessElevated() &&
                !ProcessUtils::IsProcessIntegrityAllowed(fgPid)) fg = nullptr;
            // 焦点在桌面(Progman)/任务栏(Shell_TrayWnd)时按老板键会隐藏外壳，
            // 导致桌面图标/任务栏消失，必须排除
            if (fg && WindowUtils::IsShellWindow(fg)) fg = nullptr;
            if (fg) {
                // 避免重复添加
                bool alreadyInList = false;
                for (const auto& hw : m_hiddenWindows) {
                    if (hw.hwnd == fg) { alreadyInList = true; break; }
                }
                if (!alreadyInList) {
                    m_hiddenWindows.push_back({ fg, GetShowCmdSafe(fg), fgPid,
                                                WindowUtils::GetTextSafe(fg),
                                                ProcessUtils::GetProcessName(fgPid) });
                }
            }
        }
    }

    // 隐藏所有窗口；跨进程窗口改用异步投递，避免目标短暂繁忙时拖住钩子线程
    std::vector<HWND> targets;
    targets.reserve(m_hiddenWindows.size());
    // 强制解除捕获需要一个属于本线程的窗口：优先用鼠标钩子的通知窗口，
    // 未安装鼠标钩子（仅用键盘热键触发）时退回主窗口
    HWND captureHost = m_mouseNotifyWnd ? m_mouseNotifyWnd : m_hwnd;
    bool anyAsync = false;
    for (auto& hw : m_hiddenWindows) {
        if (!IsWindow(hw.hwnd)) continue;
        // 先释放目标窗口可能持有的鼠标捕获，避免隐藏后鼠标输入仍被
        // 无响应窗口独占
        CancelWindowModes(hw.hwnd, captureHost);
        // 逐窗口记录是否走了异步隐藏：可见但请求尚未落地时，
        // 恢复和清理阶段都不能把它当作“已恢复/从未隐藏”（R15-M03）
        const bool async = SetWindowShowState(hw.hwnd, SW_HIDE);
        hw.hidePending = async;
        if (async) anyAsync = true;
        targets.push_back(hw.hwnd);
    }
    if (anyAsync) WaitForVisibility(targets, false);

    // 记录所有已投递隐藏请求的窗口（含异步尚未生效的），避免漏恢复导致
    // 窗口被隐藏后既不在列表里、也无法通过恢复文件找回
    std::vector<HiddenWindow> recorded;
    recorded.reserve(m_hiddenWindows.size());
    bool anyConfirmed = false;
    bool anyPending = false;
    for (auto& hw : m_hiddenWindows) {
        if (!IsWindow(hw.hwnd)) continue;
        // 仅以"确实不可见"作为隐藏成功的判据：IsHungAppWindow 只说明目标无响应，
        // 挂起的窗口往往仍然可见，误判为成功会导致窗口没藏起来却静音/暂停媒体
        if (IsWindowVisible(hw.hwnd) == FALSE) {
            // 隐藏已确认：清掉异步标记，恢复阶段不再把它当作待结算请求
            hw.hidePending = false;
            anyConfirmed = true;
        } else if (hw.hidePending) {
            anyPending = true;
        }
        recorded.push_back(hw);
    }
    m_hiddenWindows.swap(recorded);

    // 没有任何窗口可隐藏（如绑定程序未运行）：退出激活状态
    if (m_hiddenWindows.empty()) {
        HealStuckInputMode();
        m_active = false;
        m_pendingVerify = false;
        m_pendingVerifyRetries = 0;
        ClearRecoverFile();
        return;
    }

    // 异步隐藏可能未在有界等待内生效：仍写入恢复记录，防止随后程序崩溃时
    // 窗口已隐藏却无法找回；但不暂停媒体、不静音，避免隐藏实际失败
    // （如提权窗口）时误操作系统状态
    if (!anyConfirmed) {
        // 没有任何窗口确认隐藏成功（目标挂起或受 UIPI 限制）：先复位激活状态。
        // 否则角落触发与闲置自动隐藏会被 IsActive() 长期抑制，
        // 托盘菜单还会显示"恢复隐藏窗口"，与实际状态不符
        m_active = false;
        if (anyAsync) {
            // 异步隐藏仍可能稍后生效：保留记录并安排延迟确认，
            // 生效时重新进入激活态，保证这些窗口仍可用老板键恢复
            SaveRecoverFile();
            m_pendingVerify = true;
            m_pendingVerifyRetries = 0;
            if (m_hwnd) SetTimer(m_hwnd, TIMER_BOSSKEY_VERIFY, BOSSKEY_VERIFY_DELAY_MS, nullptr);
        } else {
            m_hiddenWindows.clear();
            m_pendingVerify = false;
            m_pendingVerifyRetries = 0;
            ClearRecoverFile();
        }
        HealStuckInputMode();
        return;
    }

    // 媒体暂停放在确认隐藏之后：没有可隐藏窗口时不再切换播放状态
    if (m_sendPauseBeforeHide) SendMediaPause();

    // 可选：静音系统音频（仅当确有窗口被隐藏时才静音，避免无恢复契机地静音全系统）
    if (m_muteEnabled) SetMute(true);

    // 持久化被隐藏窗口的信息（含"是否由本程序静音"），用于程序异常退出后恢复。
    // 必须放在 SetMute 之后：恢复文件里的静音标志要让崩溃重启时能解除系统静音
    SaveRecoverFile();

    // 已有窗口确认隐藏，但仍有异步隐藏尚未落地：不能因为“任意一个成功”
    // 就放弃对剩余窗口的确认，否则迟到 SW_HIDE 生效后会失去恢复记录（R15-M03）
    if (anyPending) {
        m_pendingVerify = true;
        m_pendingVerifyRetries = 0;
        if (m_hwnd) SetTimer(m_hwnd, TIMER_BOSSKEY_VERIFY, BOSSKEY_VERIFY_DELAY_MS, nullptr);
    }

    // 收尾自愈：异步隐藏生效后，被隐藏的窗口可能仍独占鼠标捕获
    HealStuckInputMode();
}

// ============================================================
// BossKey::ShowWindows - 恢复所有隐藏窗口
// 按隐藏前的显示状态恢复，最大化的窗口同时置前
// ============================================================
void BossKey::ShowWindows() {
    // 用户已显式恢复：作废延迟确认，避免其随后把状态又改回"激活"
    if (m_pendingVerify) {
        m_pendingVerify = false;
        if (m_hwnd) KillTimer(m_hwnd, TIMER_BOSSKEY_VERIFY);
    }
    HWND lastForeground = nullptr;
    bool anyAsync = false;
    std::vector<HWND> targets;
    targets.reserve(m_hiddenWindows.size());
    // 恢复前先自愈：上次隐藏时若把鼠标捕获留在了被隐藏的窗口上，
    // 直接恢复会让这个仍独占输入的窗口重新可见，点击依旧全部送给它
    HealStuckInputMode();
    for (auto& hw : m_hiddenWindows) {
        if (!IsWindow(hw.hwnd)) continue;
        // 校验进程 ID：句柄可能已被回收复用，避免恢复无关窗口
        DWORD wpid = 0;
        GetWindowThreadProcessId(hw.hwnd, &wpid);
        if (hw.pid != 0 && wpid != hw.pid) continue;
        // 窗口已可见且没有待结算的异步隐藏：保持现状，
        // 避免改动用户当前状态或抢走焦点。
        // hidePending 的窗口即使可见也必须投递恢复，抵消队列中尚未落地的
        // SW_HIDE；两者同队列先进先出，后投递的显示会最终生效（R15-M03）
        const bool visible = IsWindowVisible(hw.hwnd) != FALSE;
        if (visible && !hw.hidePending) continue;
        const bool async = SetWindowShowState(hw.hwnd, hw.showCmd);
        if (async) {
            anyAsync = true;
            targets.push_back(hw.hwnd);
        }
        // 恢复请求已投递：清除 pending。若随后窗口仍未显示，
        // 下面的复核会把记录保留下来，不会丢失恢复线索
        hw.hidePending = false;
        // 记录最后一个需要前置的窗口，恢复完成后只前置它，避免依次抢焦点
        if (hw.showCmd == SW_SHOWMAXIMIZED || hw.showCmd == SW_SHOWNORMAL) {
            lastForeground = hw.hwnd;
        }
    }
    // 异步恢复时先有界等待窗口显现，否则 SetForegroundWindow 对隐藏窗口无效
    if (anyAsync) WaitForVisibility(targets, true);
    // 抢焦点前先做有界探测：SetForegroundWindow 会同步等待目标线程处理激活消息，
    // 目标正忙（如浏览器加载页面）时会连带阻塞承载鼠标钩子的 UI 线程。
    // 窗口此时已经恢复，抢焦点只是附属效果，目标忙时跳过比卡住系统输入更划算
    if (lastForeground && IsWindowVisible(lastForeground) &&
        WindowResponds(lastForeground)) {
        SetForegroundWindow(lastForeground);
    }

    // 逐项复核恢复结果：仍未显示出来的窗口（挂起、受 UIPI 限制、异步投递尚未完成）
    // 必须保留记录与恢复文件，否则这些窗口既无法再用老板键恢复，重启程序也找不回来
    std::vector<HiddenWindow> remaining;
    remaining.reserve(m_hiddenWindows.size());
    for (const auto& hw : m_hiddenWindows) {
        if (!IsWindow(hw.hwnd)) continue;               // 窗口已销毁，无需保留
        DWORD wpid = 0;
        GetWindowThreadProcessId(hw.hwnd, &wpid);
        if (hw.pid != 0 && wpid != hw.pid) continue;    // 句柄已被复用，记录作废
        if (IsWindowVisible(hw.hwnd)) continue;         // 已恢复成功
        remaining.push_back(hw);
    }
    m_hiddenWindows.swap(remaining);

    if (m_hiddenWindows.empty()) {
        // 全部恢复成功（或窗口已销毁）：退出激活状态并清除恢复文件
        HealStuckInputMode();
        m_active = false;
        ClearRecoverFile();
        return;
    }

    // 仍有窗口没显示出来：保持激活态，保留恢复记录，并安排延迟确认。
    // 稍后若确实显示成功，VerifyPendingHide 会清理记录与恢复文件
    m_active = true;
    SaveRecoverFile();
    m_pendingVerify = true;
    m_pendingVerifyRetries = 0;
    if (m_hwnd) SetTimer(m_hwnd, TIMER_BOSSKEY_VERIFY, BOSSKEY_VERIFY_DELAY_MS, nullptr);
    HealStuckInputMode();
}

// ============================================================
// BossKey::VerifyPendingHide - 延迟确认异步隐藏是否最终生效
// 场景：隐藏时所有目标都走异步投递且在有界等待内仍未隐藏，
//       此时先复位激活状态以免抑制角落触发/自动隐藏；
//       若稍后隐藏真正生效，则收回激活状态，保证仍可用老板键恢复。
// ============================================================
void BossKey::VerifyPendingHide() {
    if (!m_pendingVerify) return;
    m_pendingVerify = false;
    // 延迟确认可能来自两条路径：异步隐藏迟到生效，或恢复未能立即完成。
    // 前者 m_active 已为 false，后者仍为 true —— 收尾时据此决定是否补做解除静音
    const bool wasActive = m_active;

    // 保留仍然隐藏的窗口记录：它们既可能来自"迟到的隐藏已生效"，
    // 也可能来自上一轮恢复失败。失效句柄与已恢复的窗口一并剔除
    std::vector<HiddenWindow> remaining;
    remaining.reserve(m_hiddenWindows.size());
    bool visiblePending = false;
    bool anyHidden = false;
    for (auto& hw : m_hiddenWindows) {
        // 句柄与进程仍需有效：窗口已销毁或句柄被复用时不能据此判定"已隐藏"
        if (!IsWindow(hw.hwnd)) continue;
        DWORD wpid = 0;
        GetWindowThreadProcessId(hw.hwnd, &wpid);
        if (hw.pid != 0 && wpid != hw.pid) continue;
        if (!IsWindowVisible(hw.hwnd)) {
            // 隐藏已确认：清除异步标记，后续只按“仍隐藏”保留
            hw.hidePending = false;
            remaining.push_back(hw);
            anyHidden = true;
        } else if (hw.hidePending) {
            // 窗口当前可见，但隐藏请求尚未结算：不能丢弃记录，
            // 否则迟到的 SW_HIDE 生效后会永久失去恢复线索（R15-M03）
            remaining.push_back(hw);
            visiblePending = true;
        }
    }

    if (!remaining.empty()) {
        // 迟到的隐藏已生效（或恢复仍未完成）：重新进入激活态并刷新恢复文件，
        // 保证这些窗口仍能用老板键恢复
        m_hiddenWindows.swap(remaining);
        if (anyHidden || wasActive) {
            m_active = true;
            // 迟到生效的隐藏：HideWindows 当时因"尚未确认隐藏成功"跳过了静音与媒体暂停，
            // 此刻窗口确认已不可见，必须补齐，否则该路径下"隐藏时静音/暂停媒体"会静默失效。
            // wasActive 为 true 表示本次是"恢复未完成"，静音早已生效，不能重复执行
            if (!wasActive) {
                if (m_sendPauseBeforeHide) SendMediaPause();
                if (m_muteEnabled) SetMute(true);
            }
        } else {
            // 仅剩尚未结算的异步隐藏、窗口实际仍可见：不能标记为激活，
            // 否则会错误抑制角落触发与自动隐藏（R15-M03）
            m_active = false;
        }
        // 仍有可见的待结算隐藏：继续有界确认；预算耗尽后保留记录与恢复文件，
        // 由用户下次恢复或下一次启动继续处理
        if (visiblePending) {
            if (m_pendingVerifyRetries < BOSSKEY_VERIFY_MAX_RETRIES) {
                ++m_pendingVerifyRetries;
                m_pendingVerify = true;
                if (m_hwnd) SetTimer(m_hwnd, TIMER_BOSSKEY_VERIFY, BOSSKEY_VERIFY_DELAY_MS, nullptr);
            }
        } else {
            m_pendingVerifyRetries = 0;
        }
        SaveRecoverFile();
        HealStuckInputMode();
        return;
    }

    // 没有任何窗口处于隐藏状态。wasActive 为 false 表示本次确认来自"异步隐藏"：
    // 此时窗口仍可见可能只是隐藏请求还没落地（目标线程繁忙），
    // 过早清账会让随后真正生效的隐藏失去恢复记录，需重试若干轮后再放弃
    if (!wasActive && m_pendingVerifyRetries < BOSSKEY_VERIFY_MAX_RETRIES) {
        ++m_pendingVerifyRetries;
        m_pendingVerify = true;
        if (m_hwnd) SetTimer(m_hwnd, TIMER_BOSSKEY_VERIFY, BOSSKEY_VERIFY_DELAY_MS, nullptr);
        return;
    }

    // 异步隐藏确认预算耗尽但仍有窗口记录：目标线程可能只是极晚才处理 SW_HIDE。
    // 不能直接删除恢复文件，否则随后真正落地的隐藏将失去恢复线索（R14-A7）。
    // 这里把记录写回文件后按未激活收尾；下次启动若隐藏已生效仍可恢复。
    const bool keepRecoverHint = !wasActive && !m_hiddenWindows.empty();
    if (keepRecoverHint) {
        SaveRecoverFile();
    }
    m_hiddenWindows.clear();
    if (!keepRecoverHint) {
        ClearRecoverFile();
    }
    m_active = false;
    m_pendingVerifyRetries = 0;
    // 恢复路径的收尾：Deactivate 当时因"仍有窗口未恢复"跳过了取消静音与冷却记录，
    // 此刻窗口已全部回来，必须补做，否则系统会一直停留在静音状态
    if (wasActive) {
        if (m_mutedByUs) SetMute(false);
        m_lastDeactivateTick = GetTickCount64();
    }
    HealStuckInputMode();
}

// ============================================================
// BossKey::HealStuckInputMode - 修复隐藏/恢复后残留的输入模式
// 症状：目标窗口的最小化/最大化/关闭按钮与任务栏点击全部无响应，键盘正常，
//       只有切到 Ctrl+Alt+Del 安全桌面再返回才恢复。
// 原因：某个已不可见的窗口仍独占鼠标捕获（用鼠标侧键/中键触发时，按下
//       那一刻的捕获会留在被隐藏的窗口上），捕获是全局独占的，所有点击
//       都被送给这个看不见的窗口；菜单循环、移动/缩放循环残留同理。
// 只在窗口"已不可见"或"属于本次隐藏清单"时处理：可见窗口持有捕获
// 可能是用户正在拖拽/框选，不能打断。
// ============================================================
void BossKey::HealStuckInputMode() {
    HWND captureHost = m_mouseNotifyWnd ? m_mouseNotifyWnd : m_hwnd;
    if (!captureHost || !IsWindow(captureHost)) return;

    auto isStuck = [this](HWND h) {
        if (!h || !IsWindow(h)) return false;
        if (!IsWindowVisible(h)) return true;
        for (const auto& hw : m_hiddenWindows) {
            if (hw.hwnd == h || IsSameWindowTree(hw.hwnd, h)) return true;
        }
        return false;
    };

    // 检查前台队列 + 每个隐藏窗口所属线程的队列（去重）：隐藏后前台常常
    // 已切换到别的线程，只查前台队列会漏掉滞留在原线程上的残留捕获
    std::vector<DWORD> queues;
    queues.push_back(0);
    for (const auto& hw : m_hiddenWindows) {
        if (!hw.hwnd || !IsWindow(hw.hwnd)) continue;
        DWORD tid = GetWindowThreadProcessId(hw.hwnd, nullptr);
        if (tid && tid != GetCurrentThreadId() &&
            std::find(queues.begin(), queues.end(), tid) == queues.end()) {
            queues.push_back(tid);
        }
    }

    // 整轮修复共享一份预算：多个隐藏窗口各自的线程队列叠加时不会累加成
    // N×300ms；被跳过的窗口在本次激活流程的后续调用里还会再试
    ULONGLONG deadline = GetTickCount64() + CANCEL_MODE_BUDGET_MS;
    for (DWORD tid : queues) {
        InputModeState st = QueryInputModeState(tid);
        if (st.capture && isStuck(st.capture)) {
            SendCancelModeBounded(st.capture, deadline);
            // 目标忽略 WM_CANCELMODE 时由本线程强行夺走捕获再释放
            ForceReleaseMouseCapture(captureHost);
        }
        if (st.moveSize && isStuck(st.moveSize)) {
            SendCancelModeBounded(st.moveSize, deadline);
        }
        if (st.menuOwner && isStuck(st.menuOwner)) {
            SendCancelModeBounded(st.menuOwner, deadline);
        }
    }
}

// ============================================================
// BossKey::SetMute - 通过 Windows Core Audio API 设置系统静音
//   mute=true: 记录当前静音状态后静音
//   mute=false: 恢复到隐藏前的静音状态（除非 forceUnmute）
// ============================================================
// 静音恢复失败提示：只在首次失败时提示，避免每次切换老板键都弹气泡
static bool s_muteRestoreWarned = false;

static void WarnMuteRestoreFailedOnce() {
    if (s_muteRestoreWarned) return;
    s_muteRestoreWarned = true;
    ShowTrayNotification(L"系统静音状态未能恢复（音频设备暂不可用），将在下次启动时重试解除。", true);
}

static void ResetMuteRestoreWarn() { s_muteRestoreWarned = false; }

void BossKey::SetMute(bool mute, bool forceUnmute, bool quiet) {
    // COM 已在主线程 wWinMain 中初始化，无需重复初始化

    // 复用缓存的设备枚举器：老板键可由鼠标中键/侧键频繁切换，
    // 若每次都 CoCreateInstance 会产生大量 COM 实例创建开销
    if (!m_pEnumerator) {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                               __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&m_pEnumerator));
        if (FAILED(hr)) {
            m_pEnumerator = nullptr;
            if (!mute && !quiet) WarnMuteRestoreFailedOnce();
            return;
        }
    }

    IMMDevice* pDevice = nullptr;
    HRESULT hr = m_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    if (FAILED(hr)) {
        // 音频服务可能已重启，缓存的枚举器会持续失效：释放后下次调用重建
        m_pEnumerator->Release();
        m_pEnumerator = nullptr;
        if (!mute) WarnMuteRestoreFailedOnce();
        return;
    }

    // 读取默认输出设备 ID：设备变化（插拔耳机/切换 HDMI）后旧接口仍指向失效设备，
    // 写入会被静默忽略，必须按设备 ID 重建
    LPWSTR devId = nullptr;
    const bool gotId = SUCCEEDED(pDevice->GetId(&devId)) && devId != nullptr;
    std::wstring deviceId;
    if (gotId) {
        deviceId = devId;
        CoTaskMemFree(devId);
    } else if (m_pVolume) {
        // 无法读取设备 ID 时不能复用缓存的音量接口：默认设备可能已切换，
        // 复用会把旧设备的状态当成当前状态。释放后按当前设备重新 Activate
        m_pVolume->Release();
        m_pVolume = nullptr;
        m_volumeDeviceId.clear();
    }

    if (!m_pVolume || !gotId || deviceId != m_volumeDeviceId) {
        if (m_pVolume) { m_pVolume->Release(); m_pVolume = nullptr; }
        m_volumeDeviceId = deviceId;
        hr = pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                               nullptr, reinterpret_cast<void**>(&m_pVolume));
        if (FAILED(hr)) m_pVolume = nullptr;
    }
    pDevice->Release();
    if (!m_pVolume) {
        if (!mute) WarnMuteRestoreFailedOnce();
        return;
    }

    // 按设备 ID 打开历史端点：隐藏期间切换默认输出后仍需恢复旧设备，
    // 否则旧设备会一直保持静音（用户切回该设备时无声）。
    // hrOut 返回打开结果：E_NOTFOUND/E_INVALIDARG 表示端点已不存在，
    // 与音频服务暂不可用（其他错误）区分，前者可以直接丢弃记录
    auto openEndpointVolume = [this](const std::wstring& id,
                                     HRESULT* hrOut) -> IAudioEndpointVolume* {
        if (hrOut) *hrOut = E_FAIL;
        if (id.empty() || !m_pEnumerator) return nullptr;
        IMMDevice* device = nullptr;
        HRESULT hr = m_pEnumerator->GetDevice(id.c_str(), &device);
        if (hrOut) *hrOut = hr;
        if (FAILED(hr) || !device) return nullptr;
        IAudioEndpointVolume* volume = nullptr;
        hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                              nullptr, reinterpret_cast<void**>(&volume));
        if (hrOut) *hrOut = hr;
        device->Release();
        return volume;
    };

    if (mute) {
        // 以设备 ID 去重：同一端点重复静音（上次恢复失败后再次隐藏）时
        // 保留最早记录的“静音前状态”，不能把本程序造成的静音误当成用户原始状态。
        const std::wstring endpointId = gotId ? deviceId : std::wstring();
        auto it = std::find_if(m_mutedEndpoints.begin(), m_mutedEndpoints.end(),
                               [&endpointId](const MutedEndpoint& e) {
                                   return e.deviceId == endpointId;
                               });
        if (it == m_mutedEndpoints.end()) {
            BOOL wasMuted = FALSE;
            const HRESULT getHr = m_pVolume->GetMute(&wasMuted);
            if (FAILED(m_pVolume->SetMute(TRUE, nullptr))) return;
            MutedEndpoint endpoint;
            endpoint.deviceId = endpointId;
            endpoint.valid = SUCCEEDED(getHr);
            // GetMute 失败时按“取消静音”恢复，宁可漏静音也不能永久静音
            endpoint.wasMuted = endpoint.valid && wasMuted != FALSE;
            m_mutedEndpoints.push_back(std::move(endpoint));
        } else {
            // 已记录过该端点：只确保当前仍静音，快照保持首次值
            if (FAILED(m_pVolume->SetMute(TRUE, nullptr))) return;
        }
        m_mutedByUs = true;
        return;
    }

    if (m_mutedEndpoints.empty()) {
        // 没有逐端点记录（崩溃恢复或旧版本状态）：只处理当前默认端点
        if (m_mutedByUs || forceUnmute) {
            if (FAILED(m_pVolume->SetMute(FALSE, nullptr))) {
                if (!forceUnmute && !quiet) WarnMuteRestoreFailedOnce();
                return;
            }
            m_mutedByUs = false;
            m_muteRestorePending = false;
            ResetMuteRestoreWarn();
            DropResolvedMuteRecordsFromLegacyContent();
        }
        return;
    }

    // 逐个端点恢复。端点已被系统移除（E_NOTFOUND/E_INVALIDARG）时记录永久失效，
    // 直接丢弃，避免每次启动都重试并提示“设备暂不可用”；
    // 其他错误（音频服务暂不可用等）保留记录，等本次重试或下次启动继续。
    std::vector<MutedEndpoint> stillMuted;
    bool restoreFailed = false;
    for (const auto& endpoint : m_mutedEndpoints) {
        const bool useCurrent = endpoint.deviceId.empty() || endpoint.deviceId == m_volumeDeviceId;
        HRESULT openHr = S_OK;
        IAudioEndpointVolume* volume = useCurrent ? m_pVolume
                                                  : openEndpointVolume(endpoint.deviceId, &openHr);
        if (!volume) {
            // 只有按设备 ID 打开时才可能确认“设备已不存在”；
            // 当前默认端点失败属于服务/驱动暂不可用，仍需保留记录
            if (!useCurrent && (openHr == E_NOTFOUND || openHr == E_INVALIDARG)) {
                continue;
            }
            restoreFailed = true;
            if (!forceUnmute) stillMuted.push_back(endpoint);
            continue;
        }
        // 原始状态可信时恢复原值；不可信时一律取消静音，避免永久无声
        const BOOL restoreMute = (forceUnmute || !endpoint.valid)
                               ? FALSE
                               : (endpoint.wasMuted ? TRUE : FALSE);
        if (FAILED(volume->SetMute(restoreMute, nullptr))) {
            restoreFailed = true;
            if (!forceUnmute) stillMuted.push_back(endpoint);
        }
        if (!useCurrent) volume->Release();
    }

    m_mutedEndpoints.swap(stillMuted);
    if (!restoreFailed) {
        m_mutedByUs = false;
        m_muteRestorePending = false;
        ResetMuteRestoreWarn();
        DropResolvedMuteRecordsFromLegacyContent();
    } else {
        // 非强制恢复：保留失败项继续重试；强制恢复：清空记录但保留标志，
        // 供恢复文件/下次启动兜底解除当前默认设备
        m_mutedByUs = true;
        if (!forceUnmute && !quiet) WarnMuteRestoreFailedOnce();
    }
}

// 静默重试启动时未完成的静音恢复。
// 开机自启动时音频服务/默认端点可能尚未就绪，第一次恢复会失败；
// 由定时器稍后重试，失败不弹气泡，成功则清理恢复文件中的静音记录。
bool BossKey::RetryPendingMuteRestore() {
    if (!m_muteRestorePending) return true;
    // 老板键再次激活（窗口被隐藏并重新静音）时不能解除静音，
    // 等恢复窗口后的 SetMute(false) 自行处理
    if (m_active) return false;

    SetMute(false, false, true);
    if (!m_mutedByUs) {
        m_muteRestorePending = false;
        DropResolvedMuteRecordsFromLegacyContent();
        return true;
    }
    return false;
}

// 静音记录已恢复：从 m_legacyRecoverContent 中删除 m5 行并落盘。
// 不删除其他遗留窗口记录；全部清空时才删除恢复文件。
void BossKey::DropResolvedMuteRecordsFromLegacyContent() {
    if (m_legacyRecoverContent.empty()) return;
    std::wstring filtered;
    filtered.reserve(m_legacyRecoverContent.size());
    bool changed = false;
    size_t start = 0;
    const std::wstring& src = m_legacyRecoverContent;
    while (start < src.size()) {
        size_t end = src.find(L'\n', start);
        if (end == std::wstring::npos) end = src.size();
        const size_t len = end - start;
        if (len >= 3 && src.compare(start, 3, L"m5\t") == 0) {
            changed = true;
        } else {
            filtered.append(src, start, len);
            if (end < src.size()) filtered += L'\n';
        }
        start = end + 1;
    }
    if (!changed) return;
    m_legacyRecoverContent.swap(filtered);
    if (m_legacyRecoverContent.empty()) {
        ClearRecoverFile();
    } else {
        SaveRecoverContent(m_legacyRecoverContent, !m_recoverHasUnreadableFile);
    }
}

// 发送媒体暂停/播放按键
void BossKey::SendMediaPause() {
    INPUT inp = {};
    inp.type = INPUT_KEYBOARD;
    inp.ki.wVk = VK_MEDIA_PLAY_PAUSE;
    inp.ki.wScan = (WORD)MapVirtualKeyW(VK_MEDIA_PLAY_PAUSE, MAPVK_VK_TO_VSC);
    // 按下失败（如被 UIPI 拦截）时不再补发抬起，避免留下卡键状态
    if (SendInput(1, &inp, sizeof(INPUT)) == 0) return;
    inp.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &inp, sizeof(INPUT));
}

void BossKey::Activate() {
    // 先结算未决的延迟确认：若上次的异步隐藏已迟到生效，本次按键应按"恢复"处理，
    // 否则那个窗口会一直留在隐藏状态且无法用老板键找回
    if (m_pendingVerify) {
        VerifyPendingHide();
        if (m_active) { Deactivate(); return; }
    }
    if (!m_active) { m_active = true; HideWindows(); }
}

// 停用老板键：恢复窗口、取消静音、记录停用时间
void BossKey::Deactivate() {
    if (!m_active) return;
    m_active = false;
    ShowWindows();
    // ShowWindows 若有窗口未能恢复，会把 m_active 重新置为 true 并保留恢复记录：
    // 此时窗口实际仍处于隐藏状态，不能解除静音、也不应消耗自动隐藏冷却，
    // 否则会出现"窗口还藏着、声音却已恢复"
    if (m_active) return;
    // 依据"是否由本程序静音"而非当前静音开关状态解除静音：
    // 隐藏期间用户若关闭"隐藏时静音"，此处仍应恢复，避免系统被永久静音
    if (m_mutedByUs) SetMute(false);
    m_lastDeactivateTick = GetTickCount64();
}

void BossKey::Toggle() {
    // 异步隐藏尚未确认时先结算一次：若隐藏已真正生效，
    // 此时应按激活处理并走恢复；否则会再次隐藏并清掉仍有效的恢复记录
    if (m_pendingVerify) VerifyPendingHide();
    if (m_active) Deactivate(); else Activate();
}

// ============================================================
// BossKey::CloseBoundProcesses - 关闭“当前仍处于隐藏状态”的绑定进程
// 流程：
//   1. 从隐藏记录中收集仍不可见窗口的进程ID（跳过已手动恢复的窗口）
//   2. 打开进程句柄
//   3. 发送 WM_CLOSE 优雅关闭
//   4. 等待进程退出（超时 PROCESS_CLOSE_TIMEOUT_MS）
//   5. 超时后强制终止（TerminateProcess）
// 注意：从未隐藏的绑定程序不在处理范围内（R9-H1）
// ============================================================
void BossKey::CloseBoundProcesses(DWORD timeoutMs) {
    std::unordered_set<DWORD> closedPids;
    // 进程路径缓存：本函数会多次判断同一 PID 是否为关键进程，缓存可避免重复 OpenProcess
    ProcessUtils::PathCache cache;

    // 收集隐藏窗口的进程ID。
    // 仅处理"当前仍处于隐藏状态"的窗口：本功能名称为“退出时关闭已隐藏程序”，
    // 不能把从未隐藏或用户已手动恢复的绑定窗口一并关闭/强杀（R9-H1）
    for (auto& hw : m_hiddenWindows) {
        if (!IsWindow(hw.hwnd)) continue;
        if (IsWindowVisible(hw.hwnd)) continue;   // 已手动恢复：不再视为隐藏程序
        DWORD pid = 0;
        GetWindowThreadProcessId(hw.hwnd, &pid);
        if (pid == 0 || pid == GetCurrentProcessId()) continue;
        // 句柄可能已被复用：与隐藏时记录的 PID 不一致时不得关闭/终止
        if (hw.pid != 0 && pid != hw.pid) continue;
        // 关键系统进程（资源管理器/登录/音频等）不允许被强制关闭
        if (IsCriticalProcess(pid, cache)) continue;
        // 提权运行时只处理完整性级别不高于自身的进程：
        // 绑定规则来自用户可写配置，不能借此向高完整性进程发送关闭/强杀（R8-H3）
        if (!ProcessUtils::IsProcessIntegrityAllowed(pid)) continue;
        closedPids.insert(pid);
    }

    // 打开进程句柄，用于后续等待和强制终止
    struct ProcHandle { DWORD pid; HANDLE handle; std::wstring imagePath; };
    std::vector<ProcHandle> procHandles;
    for (DWORD pid : closedPids) {
        HANDLE hProc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
        if (hProc) {
            // 记录打开时的镜像路径：强杀前复核，避免 PID 复用误杀无关进程
            procHandles.push_back({ pid, hProc, ProcessUtils::GetProcessPath(pid) });
        }
    }

    // 先尝试优雅关闭（发送 WM_CLOSE）。与收集条件保持一致：
    // 只向仍处于隐藏状态的窗口发送；已手动恢复的窗口不再关闭。
    // 完整性检查必须与收集阶段相同，避免两处过滤条件漂移（R13-A4）
    for (auto& hw : m_hiddenWindows) {
        if (!IsWindow(hw.hwnd)) continue;
        if (IsWindowVisible(hw.hwnd)) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(hw.hwnd, &pid);
        if (pid == 0 || pid == GetCurrentProcessId()) continue;
        if (hw.pid != 0 && pid != hw.pid) continue;   // 句柄已复用，跳过
        if (IsCriticalProcess(pid, cache)) continue;
        if (!ProcessUtils::IsProcessIntegrityAllowed(pid)) continue;
        PostMessageW(hw.hwnd, WM_CLOSE, 0, 0);
    }

    // 等待进程退出（使用 WaitForMultipleObjects 替代轮询，减少 CPU 唤醒）
    // WaitForMultipleObjects 最多等待 MAXIMUM_WAIT_OBJECTS(64) 个对象，需分批处理
    // 关机路径（WM_ENDSESSION）传入较短超时，避免阻塞系统关机
    ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < timeoutMs) {
        // 收集仍存活的进程句柄
        std::vector<HANDLE> alive;
        for (auto& ph : procHandles) {
            if (WaitForSingleObject(ph.handle, 0) != WAIT_OBJECT_0) {
                alive.push_back(ph.handle);
            }
        }
        if (alive.empty()) break;
        // 分批等待，每批最多 MAXIMUM_WAIT_OBJECTS 个；
        // 每批按剩余预算重算超时，避免多批串行等待导致总时长超出 timeoutMs
        for (size_t batchStart = 0; batchStart < alive.size(); batchStart += MAXIMUM_WAIT_OBJECTS) {
            // 批间重检总预算：已超时后直接结束，避免无符号回绕把剩余批次
            // 各延长 500ms、拖慢关机（R14-M7）
            const ULONGLONG elapsed = GetTickCount64() - start;
            if (elapsed >= timeoutMs) break;
            const ULONGLONG remain = timeoutMs - elapsed;
            DWORD batchWait = (DWORD)std::min((ULONGLONG)500, remain);
            size_t batchSize = std::min(alive.size() - batchStart, (size_t)MAXIMUM_WAIT_OBJECTS);
            WaitForMultipleObjects((DWORD)batchSize, &alive[batchStart], FALSE, batchWait);
        }
    }

    // 超时后强制终止未退出的进程
    for (auto& ph : procHandles) {
        if (WaitForSingleObject(ph.handle, 0) != WAIT_OBJECT_0) {
            // PID 可能在收集与强杀之间被复用：镜像路径变化或无法确认时不终止，
            // 宁可保留进程也不能误杀无关程序
            const std::wstring nowPath = ProcessUtils::GetProcessPath(ph.pid);
            if (!ph.imagePath.empty() && !nowPath.empty() &&
                _wcsicmp(ph.imagePath.c_str(), nowPath.c_str()) == 0) {
                TerminateProcess(ph.handle, 0);
            }
        }
        CloseHandle(ph.handle);
    }

    // 逐项复核：仍有窗口处于隐藏状态（关键系统进程不会被关闭、提权进程无法终止、
    // 目标进程未响应等）时必须保留记录与恢复文件，
    // 否则这些窗口既无法再用老板键恢复，重启后也找不回来
    std::vector<HiddenWindow> remaining;
    remaining.reserve(m_hiddenWindows.size());
    for (auto& hw : m_hiddenWindows) {
        if (!IsWindow(hw.hwnd)) continue;               // 窗口已销毁，无需保留
        DWORD wpid = 0;
        GetWindowThreadProcessId(hw.hwnd, &wpid);
        if (hw.pid != 0 && wpid != hw.pid) continue;    // 句柄已被复用，记录作废
        // 可见且没有待结算的异步隐藏：已恢复/关闭成功；
        // 可见但 hidePending 的窗口仍可能被迟到的 SW_HIDE 隐藏，必须保留记录（R15-M03）
        if (IsWindowVisible(hw.hwnd) && !hw.hidePending) continue;
        remaining.push_back(hw);
    }
    m_hiddenWindows.swap(remaining);

    if (m_hiddenWindows.empty()) {
        if (m_active) {
            m_active = false;
            if (m_mutedByUs) SetMute(false);  // 按逐端点快照恢复，不能强制解除用户原有静音（R14-M3）
            // 记录停用时间，使自动隐藏冷却生效（与 Deactivate 保持一致）
            m_lastDeactivateTick = GetTickCount64();
        }
        // 状态已结算：取消可能残留的延迟确认，避免定时器再次改写激活态（R13-A4）
        m_pendingVerify = false;
        m_pendingVerifyRetries = 0;
        if (m_hwnd) KillTimer(m_hwnd, TIMER_BOSSKEY_VERIFY);
        // 绑定进程已全部关闭，清除恢复文件
        ClearRecoverFile();
    } else {
        // 仍有窗口未关闭：保持激活态与静音，写回恢复文件供下次启动继续恢复
        m_active = true;
        SaveRecoverFile();
    }
    // 部分窗口未能关闭时可能仍残留鼠标捕获/菜单模式，统一自愈（R13-A4）
    HealStuckInputMode();
}

// ============================================================
// 恢复文件机制：防止程序异常退出后被隐藏窗口永久失联
//   - HideWindows 后将被隐藏窗口信息（pid/显示状态/句柄/标题）写入
//     exe 同目录的 KeySentry.recover
//   - ShowWindows / CloseBoundProcesses 正常恢复后删除
//   - 下次启动时 RecoverOrphanedWindows 读取并恢复
// ============================================================
std::vector<std::wstring> BossKey::GetRecoverFileCandidates() {
    // 候选枚举与 WinDGuard 共用同一实现（R15-A02）
    return ProcessUtils::RecoveryFileCandidates(L"KeySentry.recover");
}

std::wstring& BossKey::RecoverPathCache() {
    // 路径在进程生命周期内通常固定，缓存探测结果，避免每次读写都创建探测文件。
    // 写失败时 SaveRecoverContent 会把缓存切换到另一候选位置（R15-M04）。
    // 注意：必须使用堆上永不释放的对象。函数内静态对象的析构早于文件级
    // 全局对象（g_bossKey）的析构，而 ~BossKey() 在老板键仍激活时会调用
    // Deactivate() → ClearRecoverFile() → 本函数；若此处是普通静态对象，
    // 退出阶段会读取已析构的 std::wstring（use-after-destroy）
    static std::wstring* const cachedPath = new std::wstring([]() -> std::wstring {
        const std::vector<std::wstring> paths = GetRecoverFileCandidates();
        if (paths.empty()) return std::wstring();
        if (paths.size() == 1) return paths[0];
        const std::wstring& exeRecover = paths[0];
        const std::wstring& appDataRecover = paths[1];
        const std::wstring dir = ProcessUtils::GetModuleDir();

        // 用唯一临时文件探测 exe 目录可写性：固定名可能被预置重解析点，
        // 导致探测结果与实际写入行为不一致（R8-M12）
        std::wstring testPath;
        wchar_t testName[MAX_PATH] = {};
        if (!dir.empty() && GetTempFileNameW(dir.c_str(), L"ksr", 0, testName) != 0) {
            testPath = testName;
        } else {
            testPath = dir + L".~rtest.tmp";
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
            CloseHandle(hTest);  // 关闭即自动删除测试文件
            // 优先使用已存在的恢复记录：程序移动/权限变化后仍能找到旧的隐藏窗口记录
            return ProcessUtils::PreferNewerFile(exeRecover, appDataRecover);
        }
        return ProcessUtils::PreferNewerFile(appDataRecover, exeRecover);
    }());
    return *cachedPath;
}

std::wstring BossKey::GetRecoverFilePath() {
    return RecoverPathCache();
}

// 取进程可执行文件名（不含目录）：用于恢复记录的二次校验，
// 避免重启后 PID 被复用而误显示同标题的无关窗口
static std::wstring GetProcessBaseName(DWORD pid) {
    return ProcessUtils::GetProcessName(pid);
}

void BossKey::SaveRecoverFile() {
    if (m_hiddenWindows.empty()) {
        ClearRecoverFile();
        return;
    }

    // 每行格式：v4 \t pid \t showCmd \t hwnd \t 由本程序静音 \t 进程名 \t 标题（已转义）
    // v4 在 v3（静音标志）基础上对标题做 \\r/\\n/\\t 转义；
    // v3/v2（无静音字段）与更旧格式仍按原规则解析
    // 先保留上一轮未能恢复的遗留记录，避免本次写盘把它们覆盖掉
    std::wstring content = m_legacyRecoverContent;
    bool truncated = false;
    for (const auto& hw : m_hiddenWindows) {
        // 标题/进程名在枚举时已缓存，写盘不再重复跨进程读取；
        // 仅在旧记录或异常路径字段为空时回退读取一次
        std::wstring title = hw.title.empty() ? WindowUtils::GetTextSafe(hw.hwnd) : hw.title;
        // v4 起标题按 \\r/\\n/\\t 转义后写入：既不会破坏按行解析，
        // 也能在恢复时还原含换行/制表符的原始标题做精确匹配
        title = EscapeRecoverTitle(title);
        std::wstring procName = hw.processName.empty()
                              ? GetProcessBaseName(hw.pid)
                              : hw.processName;
        // 用拼接而非固定缓冲区格式化：标题过长时 swprintf_s 会失败并丢弃整行记录
        wchar_t prefix[64] = {};
        swprintf_s(prefix, L"v4\t%lu\t%d\t%llu\t%d\t",
                   (unsigned long)hw.pid, hw.showCmd,
                   (unsigned long long)(uintptr_t)hw.hwnd,
                   m_mutedByUs ? 1 : 0);
        std::wstring line = prefix;
        line += procName;
        line += L'\t';
        line += title;
        line += L'\n';
        // 写入端上限：UTF-8 编码后仍低于读取端的 1MB 限制，避免本程序写出的
        // 文件超出自身可读取范围、导致全部记录被忽略（R9-A3）
        if (content.size() + line.size() > MAX_RECOVER_WRITE_CHARS) {
            truncated = true;
            break;
        }
        content += line;
    }
    // 静音端点记录：窗口行里的静音标志只能表示“本程序静音过”，
    // 无法在设备切换后定位原端点；额外写入 m5 行记录端点身份（R15-M07）
    if (m_mutedByUs) {
        for (const auto& endpoint : m_mutedEndpoints) {
            const std::wstring muteLine = MakeMuteRecoverLine(
                endpoint.deviceId, endpoint.wasMuted, endpoint.valid);
            if (content.size() + muteLine.size() > MAX_RECOVER_WRITE_CHARS) {
                truncated = true;
                break;
            }
            content += muteLine;
        }
    }
    if (!SaveRecoverContent(content, !m_recoverHasUnreadableFile)) {
        WarnRecoverSaveFailedOnce();
    } else {
        ResetRecoverSaveWarn();
        if (truncated) {
            // 告知用户部分窗口失去崩溃恢复线索；同一会话只提示一次
            if (!s_recoverTruncatedWarned) {
                s_recoverTruncatedWarned = true;
                ShowTrayNotification(
                    L"被隐藏窗口过多，恢复记录已截断；部分窗口在程序异常退出后可能无法自动恢复。",
                    true);
            }
        } else {
            s_recoverTruncatedWarned = false;
        }
    }
}

// 将恢复记录以 UTF-8 原子写入恢复文件：
// 先写临时文件并校验实际写入量，成功后才替换目标文件，
// 避免磁盘满/短写时用截断内容覆盖掉可用的旧记录。
// 首选位置写失败（目录变只读、磁盘错误）时自动尝试另一候选位置，
// 避免恢复文件在运行期间静默失去持久化能力（R15-M04）
bool BossKey::SaveRecoverContent(const std::wstring& content, bool deleteOtherCandidates) {
    int bytes = WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return false;
    std::string utf8((size_t)bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, &utf8[0], bytes, nullptr, nullptr);

    const std::vector<std::wstring> candidates = GetRecoverFileCandidates();
    // 先试当前首选路径，再试其他候选；切换后后续读写都使用新位置
    std::vector<std::wstring> order;
    order.reserve(candidates.size() + 1);
    const std::wstring preferred = GetRecoverFilePath();
    if (!preferred.empty()) order.push_back(preferred);
    for (const auto& path : candidates) {
        if (path.empty()) continue;
        bool duplicate = false;
        for (const auto& existing : order) {
            if (_wcsicmp(existing.c_str(), path.c_str()) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) order.push_back(path);
    }

    // 恢复记录只需抵御进程崩溃，不必同步刷盘：跳过 FlushFileBuffers 避免
    // 老板键隐藏时在 UI 线程（承载低级钩子）出现磁盘停顿（R9-A9）
    for (const auto& path : order) {
        if (!ProcessUtils::AtomicWriteFile(path, utf8.data(), (size_t)bytes - 1, false)) {
            continue;  // 不写结尾 NUL
        }
        RecoverPathCache() = path;
        if (deleteOtherCandidates) {
            // 已把完整内容写到新位置：删除旧位置的记录，
            // 避免下次启动合并到过期内容（R15-M05）
            for (const auto& other : candidates) {
                if (_wcsicmp(other.c_str(), path.c_str()) != 0) {
                    DeleteFileW(other.c_str());
                }
            }
        }
        return true;
    }
    return false;
}

void BossKey::ClearRecoverFile() {
    // 上一轮未恢复的遗留记录必须保留：本会话只清理自己写入的记录，
    // 否则一次“没有可隐藏窗口”的切换就会把遗留窗口的恢复线索删掉
    if (!m_legacyRecoverContent.empty()) {
        SaveRecoverContent(m_legacyRecoverContent, !m_recoverHasUnreadableFile);
        return;
    }
    // 存在不可读的旧文件时保留它：可能仍包含只有它才有的恢复线索
    if (m_recoverHasUnreadableFile) return;
    // 两个候选位置都可能存在旧记录：全部删除，
    // 避免下次启动读取到已被清理的窗口记录（R15-M05）
    for (const auto& path : GetRecoverFileCandidates()) {
        DeleteFileW(path.c_str());
    }
}

// 枚举回调上下文：按进程 ID + 进程名 + 标题精确匹配查找窗口
struct RecoverFindCtx {
    DWORD pid;
    const std::wstring* title;
    const std::wstring* procName;  // 旧格式记录为空，跳过进程名校验
    int showCmd;                   // 记录的原显示状态，恢复时按原样显示
    bool found;
};

// 枚举回调：32 位下 GCC 的 lambda 无法隐式转换为 stdcall 的 WNDENUMPROC，
// 故用显式 CALLBACK 约定的命名函数
static BOOL CALLBACK RecoverFindWindowProc(HWND h, LPARAM lp) {
    auto* ctx = reinterpret_cast<RecoverFindCtx*>(lp);
    DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    if (p != ctx->pid) return TRUE;
    // 进程名校验：重启后 PID 可能被复用，仅凭 PID + 标题仍可能命中无关窗口
    if (!ctx->procName->empty()) {
        std::wstring name = GetProcessBaseName(p);
        if (_wcsicmp(name.c_str(), ctx->procName->c_str()) != 0) return TRUE;
    }
    // 安全读取标题：枚举可能遇到无响应进程，避免阻塞启动流程
    std::wstring t = WindowUtils::GetTextSafe(h);
    // 有标题时精确匹配（子串匹配可能命中标题更长的无关窗口）；
    // 记录本身无标题时，按 PID + 进程名匹配第一个无标题窗口，
    // 否则按进程绑定隐藏的无标题窗口将永远无法恢复。
    const bool titleMatch = !ctx->title->empty() && t == *ctx->title;
    const bool titlelessMatch = ctx->title->empty() && t.empty() &&
                                !ctx->procName->empty();
    if (titleMatch || titlelessMatch) {
        // 按记录的显示状态恢复（最大化/最小化），而不是一律普通显示
        if (!IsWindowVisible(h)) SetWindowShowState(h, ctx->showCmd);
        // 异步显示需要一点时间生效：短暂等待后复核，只有确实显示出来才算恢复成功，
        // 否则继续枚举（可能还有同名窗口），避免"假成功"导致记录被删
        for (int i = 0; i < 10 && !IsWindowVisible(h); ++i) Sleep(10);
        if (!IsWindowVisible(h)) return TRUE;
        ctx->found = true;
        return FALSE;  // 找到即停止枚举
    }
    return TRUE;
}

// 恢复上次异常退出时被隐藏的窗口，返回成功恢复的窗口数
// 优先用记录的窗口句柄（同一次会话内仍有效），失效则按进程 ID + 标题匹配
int BossKey::RecoverOrphanedWindows() {
    // 两个候选位置可能同时存在且包含不同窗口：全部读取、合并解析。
    // 只取“较新一份”会漏掉另一处的有效记录（R15-M05）
    std::vector<std::wstring> paths = GetRecoverFileCandidates();
    // 首选路径优先解析：同一记录在两处内容不一致时以首选位置为准，
    // 保持与旧版“优先较新文件”一致的覆盖顺序
    const std::wstring preferred = GetRecoverFilePath();
    std::stable_sort(paths.begin(), paths.end(),
                     [&preferred](const std::wstring& a, const std::wstring& b) {
                         const bool aPreferred = _wcsicmp(a.c_str(), preferred.c_str()) == 0;
                         const bool bPreferred = _wcsicmp(b.c_str(), preferred.c_str()) == 0;
                         return aPreferred && !bPreferred;
                     });
    std::wstring content;
    bool anyReadable = false;
    // 存在但无法读取/异常膨胀的文件：保留供人工检查或后续提权运行处理
    bool anyUnreadable = false;
    for (const auto& path : paths) {
        HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                anyUnreadable = true;
            }
            continue;
        }

        // 异常膨胀的恢复文件直接放弃读取（保留文件供人工检查）
        LARGE_INTEGER fileSize = {};
        if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart > MAX_RECOVER_FILE_BYTES) {
            CloseHandle(hFile);
            anyUnreadable = true;
            continue;
        }

        std::string utf8;
        char buf[4096];
        DWORD read = 0;
        while (ReadFile(hFile, buf, sizeof(buf), &read, nullptr) && read > 0) {
            utf8.append(buf, (size_t)read);
        }
        CloseHandle(hFile);
        anyReadable = true;
        if (utf8.empty()) continue;

        // UTF-8 转宽字符；单份文件损坏不应阻止另一份有效文件恢复
        int wchars = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
        if (wchars <= 0) {
            anyUnreadable = true;
            continue;
        }
        std::wstring part((size_t)wchars, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &part[0], wchars);
        // 某些外部工具写出的 UTF-8 文件带 BOM：不剥离会让首行前缀变成 "\uFEFFv4"，
        // 合并时该记录会被解析器跳过（compare 对空串安全）
        if (part.compare(0, 1, L"\uFEFF") == 0) part.erase(0, 1);
        // 防止前一份文件的末行与后一份文件的首行粘连
        if (!content.empty() && content.back() != L'\n') content += L'\n';
        content += part;
    }

    // 没有可读记录：文件全部为空才清理；存在不可读文件时保留，
    // 避免丢失只有它包含的恢复线索
    m_recoverHasUnreadableFile = anyUnreadable;
    if (content.empty()) {
        if (!anyUnreadable && anyReadable) ClearRecoverFile();
        return 0;
    }

    int recovered = 0;
    // 崩溃前是否由本程序静音：记录在 v3/v4 行中，用于恢复系统声音
    bool wasMutedByUs = false;
    // 两处恢复文件可能包含同一条记录：合并后按窗口身份去重，
    // 避免同一窗口被重复恢复或重复写回（R15-M05）
    std::unordered_set<std::wstring> seenRecords;
    // m5 静音端点记录：按端点身份恢复，避免设备切换后误操作新的默认端点（R15-M07）
    std::vector<MutedEndpoint> recoveredMuteEndpoints;
    std::unordered_set<std::wstring> seenMuteEndpoints;
    // 恢复失败的记录（原样保留，写回文件供下次启动继续尝试）。
    // 分为两类并让“尚未处理”的记录排在前面：预算耗尽时若仍按原顺序写回，
    // 文件前部每次都会失败的记录会永远占用预算，后部记录永远得不到处理（R15-M06）
    std::wstring deferredLines;   // 因行数/时间预算未解析
    std::wstring failedLines;     // 已解析但本轮恢复失败
    // 把一条本轮无法处理的记录按当前 v4 格式写回：标题做转义，
    // 供下次启动或更高权限运行继续恢复（R14-M1）
    auto appendRemainingRecord = [&](DWORD pid, int showCmd, HWND hwnd,
                                     const std::wstring& procName, const std::wstring& title) {
        wchar_t prefix[64] = {};
        swprintf_s(prefix, L"v4\t%lu\t%d\t%llu\t0\t",
                   (unsigned long)pid, showCmd, (unsigned long long)(uintptr_t)hwnd);
        failedLines += prefix;
        failedLines += procName;
        failedLines += L'\t';
        failedLines += EscapeRecoverTitle(title);
        failedLines += L'\n';
    };
    size_t lineStart = 0;
    int lineNo = 0;
    const ULONGLONG deadline = GetTickCount64() + RECOVER_TIME_BUDGET_MS;
    while (lineStart < content.size()) {
        size_t lineEnd = content.find(L'\n', lineStart);
        if (lineEnd == std::wstring::npos) lineEnd = content.size();
        std::wstring line = content.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;

        // 行数/时间预算：超限后剩余记录原样保留（不解析、不枚举窗口），
        // 避免损坏/伪造文件里的海量记录让启动阶段长时间卡顿。
        // 这些“从未处理”的记录写回时排在最前，保证下次启动优先进度（R15-M06）
        if (++lineNo > MAX_RECOVER_LINES || GetTickCount64() >= deadline) {
            deferredLines += line;
            deferredLines += L'\n';
            continue;
        }

        // m5：静音端点记录（不属于窗口记录）。端点 ID 与窗口标题一样做转义
        if (line.compare(0, 3, L"m5\t") == 0) {
            const size_t m1 = line.find(L'\t', 3);
            if (m1 == std::wstring::npos) continue;
            const size_t m2 = line.find(L'\t', m1 + 1);
            if (m2 == std::wstring::npos) continue;
            long long wasMutedVal = 0;
            long long validVal = 0;
            if (!ParseRecoverSigned(line.substr(m1 + 1, m2 - m1 - 1), &wasMutedVal)) continue;
            if (!ParseRecoverSigned(line.substr(m2 + 1), &validVal)) continue;
            wasMutedByUs = true;
            MutedEndpoint endpoint;
            endpoint.deviceId = UnescapeRecoverTitle(line.substr(3, m1 - 3));
            endpoint.wasMuted = wasMutedVal != 0;
            endpoint.valid = validVal != 0;
            // 两处文件可能包含同一端点：只恢复一次
            if (seenMuteEndpoints.insert(endpoint.deviceId).second) {
                recoveredMuteEndpoints.push_back(std::move(endpoint));
            }
            continue;
        }

        // 解析：v4 为当前格式（标题字段已转义）；v3 为上一版（pid \t showCmd \t hwnd \t
        // 静音标志 \t 进程名 \t 原始标题）；v2（无静音标志）与更旧格式继续兼容
        bool v4 = line.compare(0, 3, L"v4\t") == 0;
        bool v3 = !v4 && line.compare(0, 3, L"v3\t") == 0;
        bool v2 = !v4 && !v3 && line.compare(0, 3, L"v2\t") == 0;
        size_t pos = (v2 || v3 || v4) ? 3 : 0;
        size_t t1 = line.find(L'\t', pos);
        if (t1 == std::wstring::npos) continue;
        size_t t2 = line.find(L'\t', t1 + 1);
        if (t2 == std::wstring::npos) continue;
        size_t t3 = line.find(L'\t', t2 + 1);
        if (t3 == std::wstring::npos) continue;

        std::wstring procName;
        size_t titleStart = t3 + 1;
        if (v4 || v3) {
            size_t t4 = line.find(L'\t', t3 + 1);   // 静音标志结束
            if (t4 == std::wstring::npos) continue;
            size_t t5 = line.find(L'\t', t4 + 1);   // 进程名结束
            if (t5 == std::wstring::npos) continue;
            long long mutedFlag = 0;
            if (!ParseRecoverSigned(line.substr(t3 + 1, t4 - t3 - 1), &mutedFlag)) continue;
            if (mutedFlag != 0) wasMutedByUs = true;
            procName = line.substr(t4 + 1, t5 - t4 - 1);
            titleStart = t5 + 1;
        } else if (v2) {
            size_t t4 = line.find(L'\t', t3 + 1);
            if (t4 == std::wstring::npos) continue;
            procName = line.substr(t3 + 1, t4 - t3 - 1);
            titleStart = t4 + 1;
        }

        unsigned long long pidVal = 0, hwndVal = 0;
        long long showCmdVal = 0;
        if (!ParseRecoverUnsigned(line.substr(pos, t1 - pos), &pidVal) ||
            !ParseRecoverSigned(line.substr(t1 + 1, t2 - t1 - 1), &showCmdVal) ||
            !ParseRecoverUnsigned(line.substr(t2 + 1, t3 - t2 - 1), &hwndVal)) continue;
        // 旧格式（v2/v3）允许 hwnd=0：表示写入时没有可用句柄，
        // 恢复时仍可按 PID + 进程名 + 标题精确匹配，不能因此丢弃记录
        if (pidVal == 0 || pidVal > 0xFFFFFFFFull || hwndVal > 0xFFFFFFFFull) continue;
        if (showCmdVal < INT_MIN || showCmdVal > INT_MAX) continue;
        DWORD pid = (DWORD)pidVal;
        int showCmd = (int)showCmdVal;
        HWND hwnd = (HWND)(uintptr_t)hwndVal;
        std::wstring title = line.substr(titleStart);
        // v4 起标题经过转义，恢复原始文本后再做精确匹配
        if (v4) title = UnescapeRecoverTitle(title);
        if (showCmd <= 0) showCmd = SW_SHOWNORMAL;
        // 合并两处文件后去重：同一 PID/句柄/进程名/标题只处理一次
        std::wstring recordKey = std::to_wstring(pidVal);
        recordKey += L'\t';
        recordKey += std::to_wstring(hwndVal);
        recordKey += L'\t';
        recordKey += procName;
        recordKey += L'\t';
        recordKey += title;
        if (!seenRecords.insert(recordKey).second) continue;
        // 恢复文件位于用户可写目录，可能被伪造：只允许恢复完整性级别不高于
        // 自身的进程窗口，避免提权实例被记录驱动去显示/操作高完整性窗口（R8-H3）。
        // 但记录不能丢弃：保留到文件，交给后续更高权限运行处理（R14-M1）
        if (!ProcessUtils::IsProcessIntegrityAllowed(pid)) {
            appendRemainingRecord(pid, showCmd, hwnd, procName, title);
            continue;
        }

        // 路径 1：原窗口句柄仍有效（同一会话内程序崩溃后重启）
        bool done = false;
        if (hwnd && IsWindow(hwnd)) {
            DWORD wpid = 0;
            GetWindowThreadProcessId(hwnd, &wpid);
            if (wpid == pid) {
                // 跨会话 PID 也可能被复用：记录里有进程名时再校验一次，
                // 避免把无关进程的窗口显示出来
                bool nameOk = procName.empty();
                if (!nameOk) {
                    std::wstring curName = ProcessUtils::GetProcessName(wpid);
                    nameOk = !curName.empty() && _wcsicmp(curName.c_str(), procName.c_str()) == 0;
                }
                if (nameOk) {
                    if (!IsWindowVisible(hwnd)) {
                        SetWindowShowState(hwnd, showCmd);
                        // 异步显示需要一点时间生效：短暂等待后以"确实可见"作为成功判据，
                        // 否则恢复失败的窗口会因记录被删而永久隐藏
                        for (int i = 0; i < 10 && !IsWindowVisible(hwnd); ++i) Sleep(10);
                    }
                    done = IsWindowVisible(hwnd) != FALSE;
                }
            }
        }

        // 路径 2：句柄失效（跨会话/句柄被复用），按进程 ID + 进程名 + 标题精确匹配。
        // 无标题记录在进程名可用时也尝试匹配（无标题窗口按进程绑定）。
        if (!done && (!title.empty() || !procName.empty())) {
            RecoverFindCtx fctx{ pid, &title, &procName, showCmd, false };
            EnumWindows(RecoverFindWindowProc, reinterpret_cast<LPARAM>(&fctx));
            done = fctx.found;
        }

        if (done) {
            recovered++;
        } else {
            // 目标进程已不存在（PID 未被复用）：记录永久失效，
            // 直接丢弃，避免每次启动都重试并反复写回文件。
            HANDLE hProc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                       FALSE, pid);
            if (!hProc && GetLastError() == ERROR_INVALID_PARAMETER) continue;
            if (hProc) CloseHandle(hProc);

            // 恢复失败：保留记录供下次启动重试；静音标志写 0（本次已按下方逻辑解除）。
            // 统一改写为 v4（标题转义），避免含换行/制表符的标题再次破坏文件格式
            appendRemainingRecord(pid, showCmd, hwnd, procName, title);
        }
    }

    // 恢复系统声音：
    // - 有 m5 端点记录时按端点 ID 精确恢复，默认设备已切换也不会误操作新设备；
    // - 旧格式没有端点身份时保留原有“强制解除当前默认端点”兜底（R15-M07）
    if (!recoveredMuteEndpoints.empty()) {
        m_mutedEndpoints = std::move(recoveredMuteEndpoints);
        m_mutedByUs = true;
        // 启动阶段音频服务/端点可能尚未就绪：静默恢复，失败交给定时器稍后重试，
        // 不再开机就弹“音频设备暂不可用”的告警
        SetMute(false, false, true);
        // SetMute 会把失败/设备暂不可用的端点保留在 m_mutedEndpoints：
        // 写回文件供本次重试失败后下次启动继续尝试，不能删除恢复线索
        for (const auto& endpoint : m_mutedEndpoints) {
            failedLines += MakeMuteRecoverLine(endpoint.deviceId,
                                               endpoint.wasMuted, endpoint.valid);
        }
        m_muteRestorePending = !m_mutedEndpoints.empty();
    } else if (wasMutedByUs) {
        // 崩溃后内存中的原静音状态已丢失，强制解除当前默认端点，避免无声状态持续
        SetMute(false, true, true);
        m_muteRestorePending = m_mutedByUs;
    }

    // 未处理的记录排在失败记录之前写回：下一次启动从上次的预算边界继续，
    // 避免文件前部的永久失败项每轮都耗尽预算（R15-M06）
    const std::wstring remainingLines = deferredLines + failedLines;

    // 全部恢复才删除记录文件；有失败项时写回剩余记录，下次启动继续尝试。
    // 存在不可读文件时不得清理：它可能仍包含只有它才有的恢复线索（R15-M05）
    if (remainingLines.empty()) {
        m_legacyRecoverContent.clear();
        if (!anyUnreadable) ClearRecoverFile();
    } else {
        // 本会话未能恢复的遗留记录：保存到成员，后续任何 ClearRecoverFile
        // 都必须保留它们，不能再直接删除整个文件
        m_legacyRecoverContent = remainingLines;
        // 不可读文件保留在原位，避免删除尚未合并的记录
        SaveRecoverContent(remainingLines, !anyUnreadable);
    }
    return recovered;
}

// 安装 WH_MOUSE_LL 低级鼠标钩子
// 与键盘钩子一致：安装到专用线程，回调不受 UI 线程的跨进程等待影响
bool BossKey::InstallMouseHook(HWND notifyWnd) {
    if (m_mouseHookThread.Installed()) return true;
    m_mouseNotifyWnd = notifyWnd;
    return m_mouseHookThread.Start(
        []() -> HHOOK {
            // 不捕获 this：启动超时被放弃后，线程仍可能执行到这里（R14-M2）
            HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
            s_mouseHookHandle.store(hook, std::memory_order_release);
            return hook;
        },
        [](HHOOK hook) {
            UnhookWindowsHookEx(hook);
            s_mouseHookHandle.store(nullptr, std::memory_order_release);
        });
}

// 卸载鼠标钩子
void BossKey::UninstallMouseHook() {
    m_mouseHookThread.Stop();
}

// 更新鼠标按钮触发设置
void BossKey::UpdateMouseHookSettings(bool middleBtn, bool sideBtn1, bool sideBtn2) {
    std::lock_guard<std::recursive_mutex> lk(m_mouseStateMutex);
    m_middleBtnEnabled = middleBtn;
    m_sideBtn1Enabled = sideBtn1;
    m_sideBtn2Enabled = sideBtn2;
}

// ============================================================
// BossKey::MouseProc - 鼠标低级钩子回调
// 拦截中键和侧键点击，触发老板键切换
// 同时拦截对应的按键释放事件，避免事件传递到其他应用
// ============================================================
LRESULT CALLBACK BossKey::MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    // 析构已开始：直接放行，不再接触宿主成员（R14-M2）
    if (!s_mouseCallbackGate.load(std::memory_order_acquire)) {
        return CallNextHookEx(s_mouseHookHandle.load(std::memory_order_acquire), nCode, wParam, lParam);
    }
    struct CallbackScope {
        CallbackScope() { s_mouseCallbackCount.fetch_add(1, std::memory_order_acq_rel); }
        ~CallbackScope() { s_mouseCallbackCount.fetch_sub(1, std::memory_order_acq_rel); }
    } callbackScope;
    // 原子读取单例：UI 线程析构/停止超时放弃线程时不会与回调形成数据竞争（R9-A8）
    BossKey* const inst = s_instance.load(std::memory_order_acquire);
    // 忽略注入事件（远程桌面/自动化工具）：只有物理鼠标按键才应触发老板键，
    // 与键盘钩子对 LLKHF_INJECTED 的处理保持一致
    auto* msIn = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    // 设置对话框/热键捕获取键期间不响应老板键鼠标触发，避免用户在拾取按键
    // 或修改配置时被意外隐藏窗口
    if (nCode == HC_ACTION && inst && !g_suppressHotkeys &&
        msIn && !(msIn->flags & LLMHF_INJECTED)) {
        // 钩子在专用线程运行：设置项由 UI 线程更新，读取时短暂加锁保证一致
        std::lock_guard<std::recursive_mutex> lk(inst->m_mouseStateMutex);
        // 中键按下：触发老板键并吞掉事件
        if (wParam == WM_MBUTTONDOWN && inst->m_middleBtnEnabled && inst->m_mouseNotifyWnd) {
            PostMessageW(inst->m_mouseNotifyWnd, WM_BOSS_MOUSE_TOGGLE, 0, 0);
            return 1;
        }
        // 中键释放：吞掉事件（判空条件与按下分支保持一致，避免钩子已解绑时仍吞消息）
        if (wParam == WM_MBUTTONUP && inst->m_middleBtnEnabled && inst->m_mouseNotifyWnd) {
            return 1;
        }
        // 中键双击消息同样吞掉：否则双击时第二次按下会泄漏给应用（不重复触发切换）
        if (wParam == WM_MBUTTONDBLCLK && inst->m_middleBtnEnabled && inst->m_mouseNotifyWnd) {
            return 1;
        }
        // 侧键按下：检查是否为启用的侧键
        if (wParam == WM_XBUTTONDOWN && inst->m_mouseNotifyWnd) {
            auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            int button = HIWORD(ms->mouseData);
            if ((button == XBUTTON1 && inst->m_sideBtn1Enabled) ||
                (button == XBUTTON2 && inst->m_sideBtn2Enabled)) {
                PostMessageW(inst->m_mouseNotifyWnd, WM_BOSS_MOUSE_TOGGLE, 0, 0);
                return 1;
            }
        }
        // 侧键释放：吞掉事件
        if (wParam == WM_XBUTTONUP && inst->m_mouseNotifyWnd) {
            auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            int button = HIWORD(ms->mouseData);
            if ((button == XBUTTON1 && inst->m_sideBtn1Enabled) ||
                (button == XBUTTON2 && inst->m_sideBtn2Enabled)) {
                return 1;
            }
        }
        // 侧键双击消息：同样吞掉，避免事件泄漏到其他应用（不重复触发切换）
        if (wParam == WM_XBUTTONDBLCLK && inst->m_mouseNotifyWnd) {
            auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            int button = HIWORD(ms->mouseData);
            if ((button == XBUTTON1 && inst->m_sideBtn1Enabled) ||
                (button == XBUTTON2 && inst->m_sideBtn2Enabled)) {
                return 1;
            }
        }
    }
    return CallNextHookEx(s_mouseHookHandle.load(std::memory_order_acquire), nCode, wParam, lParam);
}
