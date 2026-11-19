#include "WinDGuard.h"
#include "Utils.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <ShlObj.h>

WinDGuard::WinDGuard() = default;

// 同一窗口两次"重建框架"的最小间隔：目标程序周期性重建样式时，
// 每轮都调用 SWP_FRAMECHANGED 会让标题栏反复闪烁
static const ULONGLONG STYLE_FIX_INTERVAL_MS = 15000;

// 一次"应用/禁用守护"中，跨进程样式探测允许消耗的总时间上限。
// 逐个窗口各等 100ms 会累加成 N × 100ms，匹配窗口较多时足以让
// 承载低级键鼠钩子的 UI 线程出现可感知的迟滞，因此整轮共享一个预算
static const ULONGLONG TOUCH_BUDGET_MS = 200;
// 单轮窗口枚举读取标题的预算：无响应窗口每次 WM_GETTEXT 需要 50ms，
// 逐窗口累加会让 ApplyLock/定时检查长时间占用 UI 线程
static const ULONGLONG TITLE_BUDGET_MS = 300;

// 锁文件大小上限：正常只有少量记录行，异常膨胀时放弃读取，
// 避免整文件读入导致内存暴涨
static constexpr LONGLONG MAX_LOCK_FILE_BYTES = 1024 * 1024;
// 单次恢复最多处理的行数：防止被写坏/伪造的文件塞入海量记录，
// 让启动阶段反复枚举窗口并等待无响应目标
static constexpr int MAX_LOCK_FILE_LINES = 512;
// 恢复阶段跨进程探测的总时间预算：超时后剩余记录原样保留，下次启动继续恢复
static constexpr ULONGLONG RECOVER_BUDGET_MS = 300;

// 上次运行遗留、本轮未能恢复的样式记录（见 WinDGuard.h 声明说明）。
// 定义为静态函数：RecoverOrphanedStyles 是静态函数，读文件时还没有可用的实例状态。
// 内部数据放在堆上且从不释放——进程退出时 ~WinDGuard 会经 RemoveLock 落盘，
// 而静态对象在不同翻译单元之间的析构顺序并无保证，用普通静态 vector
// 可能在落盘时已被销毁
std::vector<WinDGuard::LockedWindow>& WinDGuard::OrphanRecords() {
    static auto* records = new std::vector<LockedWindow>();
    return *records;
}

WinDGuard::~WinDGuard() {
    RemoveLock();
}

// 判断两组保护规则是否完全一致（用于跳过配置未变化时的重复重锁）
static bool SameBoundWindows(const std::vector<BoundWindowInfo>& a,
                             const std::vector<BoundWindowInfo>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const BoundWindowInfo& x = a[i];
        const BoundWindowInfo& y = b[i];
        if (x.matchMode != y.matchMode || x.pid != y.pid || x.title != y.title ||
            x.processName != y.processName || x.processPath != y.processPath) {
            return false;
        }
    }
    return true;
}

// 提示一次"权限不足导致保护未生效"（提权窗口受 UIPI 限制，静默失败容易让用户误以为已生效）
// 保护成功生效后复位：否则同一会话内后续新的失败（例如换了另一个提权窗口）
// 不会再提示，用户会错过真正需要处理的异常
static bool s_lockFailedWarned = false;

static void WarnLockFailedOnce() {
    if (s_lockFailedWarned) return;
    s_lockFailedWarned = true;
    ShowTrayNotification(L"部分目标窗口以管理员权限运行，置顶/禁止最小化保护无法生效。", true);
}

static void ResetLockFailedWarn() { s_lockFailedWarned = false; }

// 标题读取预算耗尽提示：仅在存在窗口标题绑定时提示；
// 后续某轮不再超预算时复位，确保新的超预算情况仍能提示。
static bool s_titleBudgetWarned = false;

static void WarnTitleBudgetOnce() {
    if (s_titleBudgetWarned) return;
    s_titleBudgetWarned = true;
    ShowTrayNotification(L"部分窗口因目标无响应未能读取标题，窗口守护可能未覆盖全部窗口。", true);
}

static void ResetTitleBudgetWarn() { s_titleBudgetWarned = false; }

// 锁记录写盘失败提示：失败意味着崩溃后无法恢复窗口样式，必须让用户知道；
// 后续任一次写盘成功后复位，避免永久静默。
static bool s_lockSaveWarned = false;

static void WarnLockSaveFailedOnce() {
    if (s_lockSaveWarned) return;
    s_lockSaveWarned = true;
    ShowTrayNotification(L"窗口守护记录无法写入（目录只读或磁盘已满），"
                         L"异常退出后窗口样式可能无法自动恢复。", true);
}

static void ResetLockSaveWarn() { s_lockSaveWarned = false; }

// ============================================================
// 跨进程窗口操作的非阻塞辅助
// 背景：WinDGuard 的定时检查与"应用"操作都运行在承载低级键鼠钩子的
//       UI 线程上。ShowWindow / SetWindowPos 会向目标窗口线程同步发送
//       消息，目标进程无响应时会卡住该线程，导致全系统鼠标/键盘输入停滞。
// ============================================================
namespace {
    // 严格解析十进制无符号整数：必须非空、全为数字、无溢出。
    // strtoull 会接受空串/部分数字（"12abc"）并返回 0 或前缀值，
    // 被写坏的记录文件会因此产生看似有效的错误样式记录
    bool ParseUnsignedDecimal(const std::string& s, unsigned long long* out) {
        if (s.empty() || !out) return false;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
        }
        errno = 0;
        char* end = nullptr;
        const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
        if (errno == ERANGE || end != s.c_str() + s.size()) return false;
        *out = v;
        return true;
    }

    // 严格解析十进制有符号整数（允许一个前导 '-'）
    bool ParseSignedDecimal(const std::string& s, long long* out) {
        if (s.empty() || !out) return false;
        size_t i = (s[0] == '-') ? 1 : 0;
        if (i >= s.size()) return false;
        for (size_t j = i; j < s.size(); ++j) {
            if (s[j] < '0' || s[j] > '9') return false;
        }
        errno = 0;
        char* end = nullptr;
        const long long v = std::strtoll(s.c_str(), &end, 10);
        if (errno == ERANGE || end != s.c_str() + s.size()) return false;
        *out = v;
        return true;
    }

    // 锁文件一行的解析结果（格式与数值范围已校验，窗口有效性由调用方校验）
    struct ParsedLockRecord {
        DWORD processId = 0;
        HWND hwnd = nullptr;
        LONG_PTR originalStyle = 0;
        LONG_PTR originalExStyle = 0;
    };

    // 解析并校验一行锁记录：4 个制表符分隔的十进制字段 + 合法范围。
    // 恢复主循环与“预算耗尽后原样保留”共用同一实现，避免两份规则漂移（R14-A10）
    bool ParseLockRecordLine(const std::string& line, ParsedLockRecord* out) {
        if (!out) return false;
        const size_t t1 = line.find('\t');
        if (t1 == std::string::npos) return false;
        const size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) return false;
        const size_t t3 = line.find('\t', t2 + 1);
        if (t3 == std::string::npos) return false;

        unsigned long long pidVal = 0, hwndVal = 0;
        long long styleVal = 0, exStyleVal = 0;
        if (!ParseUnsignedDecimal(line.substr(0, t1), &pidVal) ||
            !ParseUnsignedDecimal(line.substr(t1 + 1, t2 - t1 - 1), &hwndVal) ||
            !ParseSignedDecimal(line.substr(t2 + 1, t3 - t2 - 1), &styleVal) ||
            !ParseSignedDecimal(line.substr(t3 + 1), &exStyleVal)) return false;
        // 合法范围校验：句柄样式只使用 32 位；全 0 记录视为损坏/伪造
        if (pidVal == 0 || hwndVal == 0 || hwndVal > 0xFFFFFFFFull) return false;
        if (styleVal == 0 && exStyleVal == 0) return false;
        if (styleVal < INT32_MIN || styleVal > UINT32_MAX) return false;
        if (exStyleVal < INT32_MIN || exStyleVal > UINT32_MAX) return false;

        out->processId = (DWORD)pidVal;
        out->hwnd = (HWND)(uintptr_t)hwndVal;
        out->originalStyle = (LONG_PTR)styleVal;
        out->originalExStyle = (LONG_PTR)exStyleVal;
        return true;
    }

    bool IsWindowHung(HWND hwnd) {
        return IsHungAppWindow(hwnd) != FALSE;
    }

    bool IsForeignWindow(HWND hwnd) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        return pid != GetCurrentProcessId();
    }

    // 跨进程窗口操作一律异步投递：SetWindowPos 对响应中的窗口同样是同步等待，
    // 目标短暂繁忙（1~2 秒）就足以卡住承载键鼠钩子的 UI 线程；
    // IsHungAppWindow 只识别无响应(≥5s)窗口，不能作为唯一判据
    void SetWindowPosSafe(HWND hwnd, HWND insertAfter, UINT flags) {
        if (IsWindowHung(hwnd) || IsForeignWindow(hwnd)) flags |= SWP_ASYNCWINDOWPOS;
        SetWindowPos(hwnd, insertAfter, 0, 0, 0, 0, flags);
    }

    // 安全显示/恢复窗口：跨进程或目标无响应时异步
    void ShowWindowSafe(HWND hwnd, int showCmd) {
        if (IsWindowHung(hwnd) || IsForeignWindow(hwnd)) {
            ShowWindowAsync(hwnd, showCmd);
        } else {
            ShowWindow(hwnd, showCmd);
        }
    }

    // 能否安全地修改该窗口的样式（SetWindowLongPtrW 的 GWL_STYLE）。
    // 说明：SetWindowLongPtrW 修改样式时窗口管理器会向目标窗口发送
    //       WM_STYLECHANGING / WM_STYLECHANGED，对别的进程来说就是一次
    //       同步的跨线程等待。目标进程繁忙（未响应的浏览器、游戏、脚本窗口）
    //       时，承载低级键鼠钩子的 UI 线程会一起被挂住，表现为全系统输入迟滞。
    //       因此改动前先做一次有界探测：100ms 内无响应就跳过本次修改，
    //       交给下一轮定时检查重试，而不是冒险阻塞。
    // accessDenied：回填是否因权限不足（提权窗口受 UIPI 限制）被拒绝——
    //       这属于永久失败而非暂时繁忙，调用方据此保留"保护未生效"的提示
    // deadline：整轮探测的总时间上限（0 表示不限）。预算用尽时直接放弃本次探测，
    // 由调用方在下一轮重试，避免多个窗口的超时累加成秒级阻塞
    bool CanTouchWindow(HWND hwnd, bool* accessDenied = nullptr, UINT timeoutMs = 100,
                        ULONGLONG deadline = 0) {
        if (accessDenied) *accessDenied = false;
        if (!IsWindow(hwnd)) return false;
        if (!IsForeignWindow(hwnd)) return true;  // 同进程窗口：直接修改，不会跨线程等待
        if (IsWindowHung(hwnd)) return false;
        UINT timeout = timeoutMs;
        if (deadline) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) return false;
            ULONGLONG remain = deadline - now;
            if (remain < (ULONGLONG)timeout) timeout = (UINT)remain;
        }
        SetLastError(0);
        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, timeout, &result) != 0) {
            return true;
        }
        if (accessDenied && GetLastError() == ERROR_ACCESS_DENIED) *accessDenied = true;
        return false;
    }
}

// ============================================================
// 窗口样式落盘：防止程序异常退出后，被移除的 WS_MINIMIZEBOX 永久残留
// 在其他进程的窗口上（KeySentry 崩溃/被强杀时析构不会执行）
//   - 文件路径策略与 INI/恢复文件一致：exe 同目录优先，只读时回退 %APPDATA%
//   - 每行格式：pid \t hwnd \t originalStyle \t originalExStyle（均为十进制）
// ============================================================
// 锁记录的两个候选位置：exe 同目录优先，%APPDATA%\KeySentry 兜底。
// 读取阶段必须同时检查两个位置，不能只选一个，否则较旧文件中的
// 未恢复窗口会永久残留（两份记录可能包含不同的窗口）。
// 候选枚举与 BossKey 共用同一实现（R15-A02）
static std::vector<std::wstring> LockFileCandidates() {
    return ProcessUtils::RecoveryFileCandidates(L"KeySentry.wguard");
}

std::wstring& WinDGuard::LockPathCache() {
    // 路径在进程生命周期内通常固定，缓存探测结果，避免每次保存/删除记录文件
    // 都创建一个临时探测文件；写失败时可切换到另一候选位置（R15-A02）。
    // 缓存必须放在堆上且不释放：GetLockFilePath 会在 ~WinDGuard 中经
    // RemoveLock → SaveLockFile 调用，而函数内静态对象（在 wWinMain 期间构造）
    // 会先于文件级静态 g_winDGuard 析构，直接返回函数内静态 std::wstring
    // 会造成 use-after-destroy，导致退出时锁记录无法落盘。
    static std::wstring* const cachedPath = new std::wstring([]() -> std::wstring {
        const std::vector<std::wstring> paths = LockFileCandidates();
        if (paths.empty()) return std::wstring();
        if (paths.size() == 1) return paths[0];

        const std::wstring& exeLock = paths[0];
        const std::wstring& appDataLock = paths[1];
        // 用唯一临时文件探测 exe 目录可写性（与 INI 回退策略一致）。
        // 固定名可能被预置重解析点，导致探测结果与实际写入行为不一致（R8-M12）
        const std::wstring dir = ProcessUtils::GetModuleDir();
        std::wstring testPath;
        wchar_t testName[MAX_PATH] = {};
        if (!dir.empty() && GetTempFileNameW(dir.c_str(), L"ksg", 0, testName) != 0) {
            testPath = testName;
        } else {
            testPath = dir + L".~wgtest.tmp";
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
            return ProcessUtils::PreferNewerFile(exeLock, appDataLock);
        }
        return ProcessUtils::PreferNewerFile(appDataLock, exeLock);
    }());
    return *cachedPath;
}

std::wstring WinDGuard::GetLockFilePath() {
    return LockPathCache();
}

void WinDGuard::SaveLockFile() {
    // 先清掉已失效的孤儿记录（窗口已销毁或句柄被复用），避免记录文件无限增长
    std::vector<LockedWindow>& orphans = OrphanRecords();
    orphans.erase(
        std::remove_if(orphans.begin(), orphans.end(),
            [](const LockedWindow& lw) {
                if (!IsWindow(lw.hwnd)) return true;
                DWORD wpid = 0;
                GetWindowThreadProcessId(lw.hwnd, &wpid);
                return wpid != lw.processId;
            }),
        orphans.end());

    // 合并"本次运行锁定的窗口"与"上次运行遗留、本轮未能恢复的窗口"。
    // 后者若不同时落盘，一次 SaveLockFile 就会把它覆盖掉，
    // 这些窗口的原始样式（最小化按钮）随之永久丢失，下次启动再也无法恢复
    std::vector<LockedWindow> merged = orphans;
    merged.reserve(merged.size() + m_lockedWindows.size());
    for (const auto& lw : m_lockedWindows) {
        auto dup = std::find_if(merged.begin(), merged.end(),
                                [&lw](const LockedWindow& o) { return o.hwnd == lw.hwnd; });
        if (dup != merged.end()) {
            *dup = lw;      // 同一窗口已被本轮锁定：以本轮（含沿用过来的原始样式）记录为准
        } else {
            merged.push_back(lw);
        }
    }

    if (merged.empty()) {
        ClearLockFile();
        return;
    }

    std::string content;
    for (const auto& lw : merged) {
        char line[128] = {};
        snprintf(line, sizeof(line), "%lu\t%llu\t%lld\t%lld\n",
                 (unsigned long)lw.processId,
                 (unsigned long long)(uintptr_t)lw.hwnd,
                 (long long)lw.originalStyle,
                 (long long)lw.originalExStyle);
        content += line;
    }

    // 内容未变化且目标文件仍存在时跳过写盘：长期无响应窗口会让
    // RemoveLock 每 3 秒重试一次，重复 Flush 没有必要。
    if (content == m_lastLockContent &&
        ProcessUtils::FileExistsNonEmpty(GetLockFilePath())) {
        return;
    }

    // 原子写入：短写/磁盘满时不会用截断内容覆盖可用的旧记录。
    // 规范位置失败时尝试另一个候选位置，确保至少一份崩溃恢复记录落盘。
    // 锁记录只需抵御进程崩溃，跳过同步刷盘，避免每 3 秒检查时阻塞 UI 线程（R9-A9）
    const std::wstring preferredPath = GetLockFilePath();
    bool saved = ProcessUtils::AtomicWriteFile(preferredPath, content.data(), content.size(), false);
    if (!saved) {
        for (const auto& path : LockFileCandidates()) {
            if (_wcsicmp(path.c_str(), preferredPath.c_str()) == 0) continue;
            if (ProcessUtils::AtomicWriteFile(path, content.data(), content.size(), false)) {
                // 首选目录运行期变为只读：切换缓存到可写位置，
                // 并尽量删除旧位置的过期记录，避免下次启动恢复旧样式（R15-A02）
                LockPathCache() = path;
                if (!preferredPath.empty()) DeleteFileW(preferredPath.c_str());
                saved = true;
                break;
            }
        }
    }
    if (saved) {
        m_lastLockContent = content;
        ResetLockSaveWarn();
    } else {
        WarnLockSaveFailedOnce();
    }
}

void WinDGuard::ClearLockFile() {
    // 两个候选位置都要清理：否则旧位置的残留记录会在下次启动
    // 被重新读取并再次尝试恢复。
    for (const auto& path : LockFileCandidates()) {
        DeleteFileW(path.c_str());
    }
    m_lastLockContent.clear();
}

// 恢复上一次运行遗留的窗口样式（崩溃兜底）。
// 仅在 hwnd 仍有效且进程 ID 一致时恢复，避免句柄被复用后误改无关窗口。
int WinDGuard::RecoverOrphanedStyles(bool removeFile) {
    // 两个位置可能同时存在且包含不同窗口：全部读取、合并解析。
    // seenWindows 会在后续去重，重复记录不会重复恢复。
    std::vector<std::wstring> paths = LockFileCandidates();
    // 首选位置优先解析：同一窗口在两处记录不一致时以首选位置为准，
    // 与运行期写入位置的切换保持一致（R15-A02）
    const std::wstring preferredPath = GetLockFilePath();
    std::stable_sort(paths.begin(), paths.end(),
                     [&preferredPath](const std::wstring& a, const std::wstring& b) {
                         const bool aPreferred =
                             _wcsicmp(a.c_str(), preferredPath.c_str()) == 0;
                         const bool bPreferred =
                             _wcsicmp(b.c_str(), preferredPath.c_str()) == 0;
                         return aPreferred && !bPreferred;
                     });
    std::string data;
    bool skippedFile = false;  // 有文件因异常膨胀被跳过：保留文件供人工检查，不删除
    for (const auto& path : paths) {
        HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            // 文件存在但无法读取（占用/权限）：保留，不能在恢复结束后误删
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                skippedFile = true;
            }
            continue;
        }

        // 异常膨胀的锁文件直接跳过（保留文件供人工检查），
        // 避免整文件读入造成内存暴涨或长度溢出
        LARGE_INTEGER fileSize = {};
        if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart > MAX_LOCK_FILE_BYTES) {
            CloseHandle(hFile);
            skippedFile = true;
            continue;
        }

        char buf[2048];
        DWORD read = 0;
        while (ReadFile(hFile, buf, sizeof(buf), &read, nullptr) && read > 0) {
            data.append(buf, (size_t)read);
        }
        CloseHandle(hFile);
        // 防止前一个文件的末行与下一个文件的首行粘连
        if (!data.empty() && data.back() != '\n') data.push_back('\n');
    }

    size_t pos = 0;
    int repaired = 0;
    int lineNo = 0;
    // 本轮无法恢复的记录（目标无响应/超出预算）先收集到类级存储，随后与普通记录
    // 一起落盘，保证下次启动还能继续恢复
    OrphanRecords().clear();
    // 分为“预算未处理”和“已处理但失败”两类：写回时未处理项排在前面，
    // 避免文件前部每次都超时的记录永久垄断恢复预算（R15-M06）
    std::vector<LockedWindow> deferredRecords;
    std::vector<LockedWindow> failedRecords;
    // 有窗口未响应而被跳过恢复：此时必须保留落盘记录，下次启动继续恢复，
    // 否则样式会永久残留在这批窗口上
    bool skipped = false;
    const ULONGLONG deadline = GetTickCount64() + RECOVER_BUDGET_MS;
    std::unordered_set<HWND> seenWindows;

    // 解析一行并原样保留为"待恢复"记录（不做任何跨进程修改）。
    // 用于行数/时间预算用尽后把剩余记录留给下次启动
    auto preserveLine = [&](const std::string& line) {
        ParsedLockRecord rec;
        if (!ParseLockRecordLine(line, &rec)) return;
        if (!IsWindow(rec.hwnd)) return;
        DWORD wpid = 0;
        GetWindowThreadProcessId(rec.hwnd, &wpid);
        if (wpid != rec.processId) return;
        // 这里只收集待恢复记录、不修改窗口，因此不需要按完整性过滤：
        // 更高完整性窗口的记录保留下来，交给后续提权运行处理（R13-A5）
        if (!seenWindows.insert(rec.hwnd).second) return;  // 同一窗口只保留一条
        LockedWindow orphan{};
        orphan.hwnd = rec.hwnd;
        orphan.originalStyle = rec.originalStyle;
        orphan.originalExStyle = rec.originalExStyle;
        orphan.processId = rec.processId;
        deferredRecords.push_back(orphan);
        skipped = true;
    };

    while (pos < data.size()) {
        size_t eol = data.find('\n', pos);
        if (eol == std::string::npos) eol = data.size();
        std::string line = data.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty()) continue;

        // 行数与时间预算：超限后剩余记录原样保留，避免伪造/损坏文件导致启动长时间卡顿
        if (++lineNo > MAX_LOCK_FILE_LINES || GetTickCount64() >= deadline) {
            preserveLine(line);
            continue;
        }

        ParsedLockRecord rec;
        if (!ParseLockRecordLine(line, &rec)) continue;
        DWORD pid = rec.processId;
        HWND hwnd = rec.hwnd;
        LONG_PTR origStyle = rec.originalStyle;
        LONG_PTR origExStyle = rec.originalExStyle;

        if (!IsWindow(hwnd)) continue;
        DWORD wpid = 0;
        GetWindowThreadProcessId(hwnd, &wpid);
        if (wpid != pid) continue;  // 句柄已被复用，跳过以免误改
        if (!seenWindows.insert(hwnd).second) continue;  // 同一窗口只恢复一次
        // 锁文件可被同用户进程伪造：本次不修改更高完整性窗口（R8-H3），
        // 但必须保留记录而不是丢弃，否则后续管理员运行也无法再恢复（R13-A5）
        if (!ProcessUtils::IsProcessIntegrityAllowed(pid)) {
            LockedWindow orphan;
            orphan.hwnd = hwnd;
            orphan.originalStyle = origStyle;
            orphan.originalExStyle = origExStyle;
            orphan.processId = pid;
            failedRecords.push_back(orphan);
            skipped = true;
            continue;
        }

        // 恢复 WS_MINIMIZEBOX（仅当原本有、当前没有时）
        LONG_PTR curStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
        bool needMinimizeBox = ((origStyle & WS_MINIMIZEBOX) != 0) && ((curStyle & WS_MINIMIZEBOX) == 0);
        // 跨进程改样式会同步等待目标窗口响应：目标繁忙时跳过改样式，
        // 由下次启动继续恢复（记录文件不删除）。但"取消置顶"是异步投递，
        // 不依赖目标响应，所以不能跟着一起跳过，否则该窗口在本次运行期间
        // 会一直保持上次残留的置顶状态
        bool styleSkipped = false;
        if (needMinimizeBox) {
            bool accessDenied = false;
            if (!CanTouchWindow(hwnd, &accessDenied, 100, deadline)) {
                styleSkipped = true;
                // 权限不足（提权窗口受 UIPI 限制）是永久失败，重试也不会成功：
                // 不保留记录，否则孤儿条目会一直留在 .wguard 里、每次启动都白做一次探测
                if (!accessDenied) {
                    skipped = true;
                    // 保留真正的原始样式：窗口当前已没有 WS_MINIMIZEBOX（上次运行残留），
                    // 若只把"现状"记进新列表，后续 RemoveLock 不会恢复最小化按钮
                    LockedWindow orphan;
                    orphan.hwnd = hwnd;
                    orphan.originalStyle = origStyle;
                    orphan.originalExStyle = origExStyle;
                    orphan.processId = pid;
                    failedRecords.push_back(orphan);
                }
            }
        }
        if (needMinimizeBox && !styleSkipped) {
            // SetWindowLongPtr 返回 0 且 GetLastError 非 0 才算失败。
            // 失败时不能计入"已修复"（否则提示数量虚高），也不能丢弃恢复记录：
            // 探测通过说明目标当时可响应，写回失败可能只是瞬时失败；
            // 若直接删掉 .wguard，最小化按钮将永久缺失（R11-M2）。
            // ERROR_ACCESS_DENIED 属于永久权限失败，不保留记录以避免每次启动重试
            SetLastError(0);
            LONG_PTR prev = SetWindowLongPtrW(hwnd, GWL_STYLE, curStyle | WS_MINIMIZEBOX);
            if (prev == 0 && GetLastError() != 0) {
                const DWORD styleErr = GetLastError();
                needMinimizeBox = false;
                if (styleErr != ERROR_ACCESS_DENIED) {
                    styleSkipped = true;
                    skipped = true;
                    // 保存真正的原始样式：当前样式已缺少 WS_MINIMIZEBOX，
                    // 只记现状会让后续恢复无法还原最小化按钮
                    LockedWindow orphan;
                    orphan.hwnd = hwnd;
                    orphan.originalStyle = origStyle;
                    orphan.originalExStyle = origExStyle;
                    orphan.processId = pid;
                    failedRecords.push_back(orphan);
                }
            }
        }

        // 恢复非置顶状态。不能依据“当前 exStyle 是否有 TOPMOST”决定是否撤销：
        // 上次运行可能留下尚未被目标线程处理的异步 TOPMOST，当前读到的状态还没更新。
        // 只要原始状态不是 TOPMOST，就无条件投递一次 NOTOPMOST；两次操作同队列
        // 先进先出，最终状态一定为非置顶，且重复投递是幂等的。
        LONG_PTR curExStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        bool needUntopmost = (!(origExStyle & WS_EX_TOPMOST)) && ((curExStyle & WS_EX_TOPMOST) != 0);

        if (!(origExStyle & WS_EX_TOPMOST)) {
            SetWindowPosSafe(hwnd, HWND_NOTOPMOST,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        } else if (needMinimizeBox && !styleSkipped) {
            SetWindowPosSafe(hwnd, nullptr,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        // styleSkipped 的窗口本轮并未完整恢复，不计入"已修复"数量
        if (!styleSkipped && (needUntopmost || needMinimizeBox)) repaired++;
    }

    // 未处理的记录排在失败记录之前写回：下一次启动从上次的预算边界继续，
    // 避免文件前部的永久失败项每轮都耗尽预算（R15-M06）
    OrphanRecords().reserve(deferredRecords.size() + failedRecords.size());
    OrphanRecords().insert(OrphanRecords().end(),
                           deferredRecords.begin(), deferredRecords.end());
    OrphanRecords().insert(OrphanRecords().end(),
                           failedRecords.begin(), failedRecords.end());

    // 恢复动作全部完成后再删除记录文件：若恢复到一半再次异常退出，
    // 文件仍保留，下次启动可继续恢复剩余窗口，避免样式永久残留；
    // 若本轮有窗口因无响应被跳过，同样保留记录
    if (removeFile && !skipped && !skippedFile) {
        for (const auto& path : paths) DeleteFileW(path.c_str());
    }
    return repaired;
}

// 一次性配置启用状态和窗口列表
void WinDGuard::Configure(bool enabled, const std::vector<BoundWindowInfo>& windows) {
    // 配置与当前完全一致时直接返回：避免每次点击"应用/确定"都移除并按全量枚举重锁所有窗口
    if (enabled == m_enabled && SameBoundWindows(windows, m_protectedWindows)) return;

    if (m_enabled) RemoveLock();       // 先移除旧保护（无论新状态是否启用）
    m_enabled = enabled;
    m_protectedWindows = windows;
    m_enumCounter = 0;
    if (m_enabled && !m_protectedWindows.empty()) {
        ApplyLock();                    // 仅调用一次 ApplyLock
    }
}

// 检查指定窗口是否匹配任一保护规则（复用进程路径缓存，避免重复 OpenProcess）
bool WinDGuard::IsWindowMatched(HWND hwnd, ProcessUtils::PathCache& cache,
                                const std::wstring* cachedTitle) const {
    for (const auto& info : m_protectedWindows) {
        if (info.Matches(hwnd, cache, true, cachedTitle)) return true;
    }
    return false;
}

// 单次枚举捕获的窗口：标题只读一次，供所有绑定复用
struct TopLevelWindowInfo {
    HWND hwnd;
    std::wstring title;
};

struct TopLevelEnumCtx {
    std::vector<TopLevelWindowInfo> windows;
    ULONGLONG deadline;
    bool hasProcessBindings;  // 无标题窗口也可能按进程匹配
    bool needTitle;           // 是否存在按标题匹配的绑定
    bool titleBudgetExceeded = false;  // 标题预算已耗尽（仅提示，不终止枚举）
};

// EnumWindows 回调：收集所有可见的顶层窗口（带标题与总预算）
static BOOL CALLBACK EnumAllTopLevelWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<TopLevelEnumCtx*>(lParam);

    if (IsWindowVisible(hwnd) == FALSE) return TRUE;
    if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) return TRUE;
    // 跳过任务栏/桌面等外壳窗口：与老板键保持一致，避免按进程绑定
    // explorer.exe 时把外壳改成置顶/去掉最小化按钮
    if (WindowUtils::IsShellWindow(hwnd)) return TRUE;
    // 跳过自身进程窗口：标题子串规则（如"设置"）不应命中自己的设置对话框
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;

    // 只有存在标题匹配绑定时才读取标题；纯进程模式不读标题，
    // 因而不会因标题预算耗尽而漏掉按进程绑定的窗口。
    std::wstring title;
    if (ctx->needTitle) {
        if (GetTickCount64() < ctx->deadline) {
            // 安全读取标题：目标进程无响应时不会阻塞承载钩子的 UI 线程
            title = WindowUtils::GetTextSafe(hwnd);
        } else {
            // 预算耗尽后继续枚举，仅停止读标题：进程模式仍可继续匹配，
            // 窗口模式因空标题自然不匹配，由调用方提示部分窗口未处理。
            ctx->titleBudgetExceeded = true;
        }
    }
    // 无标题窗口：存在按进程匹配的绑定仍需保留（Window 模式会因空标题自然不匹配）
    if (title.empty() && !ctx->hasProcessBindings) return TRUE;

    ctx->windows.push_back({ hwnd, std::move(title) });
    return TRUE;
}

// 是否存在按进程匹配的保护规则
static bool HasProcessBinding(const std::vector<BoundWindowInfo>& windows) {
    for (const auto& w : windows) {
        if (w.matchMode == MatchMode::Process) return true;
    }
    return false;
}

// 是否存在按窗口标题匹配的保护规则（只有这类绑定需要读取标题）
static bool HasWindowBinding(const std::vector<BoundWindowInfo>& windows) {
    for (const auto& w : windows) {
        if (w.matchMode == MatchMode::Window && !w.title.empty()) return true;
    }
    return false;
}

// ============================================================
// WinDGuard::LockWindow - 对单个窗口应用保护
//   1. 记录原始窗口样式（用于恢复）
//   2. 将窗口设为置顶
//   3. 移除 WS_MINIMIZEBOX 样式（禁止最小化）
// ============================================================
void WinDGuard::LockWindow(HWND hwnd, DWORD pid, ULONGLONG deadline) {
    if (!IsWindow(hwnd)) return;
    // 枚举与加锁之间窗口可能销毁并被复用：以当前实际 PID 为准，
    // 避免把样式改到无关窗口上。
    DWORD actualPid = 0;
    GetWindowThreadProcessId(hwnd, &actualPid);
    if (actualPid == 0 || (pid != 0 && actualPid != pid)) return;
    pid = actualPid;

    // 保护规则来自用户可写配置：不能被配置驱动去修改更高完整性级别的窗口
    // （置顶/移除最小化按钮），否则提权实例会变成跨完整性级别的操作代理（R8-H3）
    if (!ProcessUtils::IsProcessIntegrityAllowed(pid)) return;

    // 避免重复锁定（句柄被复用时进程 ID 会不同，需继续按新窗口处理）
    for (const auto& lw : m_lockedWindows) {
        if (lw.hwnd == hwnd && lw.processId == pid) return;
    }

    LONG_PTR curStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR curExStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    LONG_PTR origStyle = curStyle;
    LONG_PTR origExStyle = curExStyle;

    // 该窗口可能是上次运行锁定、本轮又因无响应未能恢复的：必须沿用记录中的
    // 真正原始样式，否则会把"最小化按钮已被移除"的现状当成原始状态，
    // 之后 RemoveLock 不再恢复最小化按钮，样式永久残留
    std::vector<LockedWindow>& orphans = OrphanRecords();
    auto orphan = std::find_if(orphans.begin(), orphans.end(),
                               [hwnd, pid](const LockedWindow& o) {
                                   return o.hwnd == hwnd && o.processId == pid;
                               });
    if (orphan != orphans.end()) {
        origStyle = orphan->originalStyle;
        origExStyle = orphan->originalExStyle;
        orphans.erase(orphan);   // 已转入本轮正式记录，避免重复落盘
    }

    LockedWindow lw;
    lw.hwnd = hwnd;
    lw.originalStyle = origStyle;
    lw.originalExStyle = origExStyle;
    lw.processId = pid;
    lw.lockTick = GetTickCount64();
    m_lockedWindows.push_back(lw);

    // 目标无响应时跳过窗口操作，留给下一轮定时检查重试。
    // 注意：跨进程 SetWindowLongPtrW 会向目标窗口发送 WM_STYLECHANGING/WM_STYLECHANGED，
    // 属于同步等待，目标繁忙时必须一起跳过；原始样式先记录（只读内核操作，不会阻塞）
    bool hung = IsWindowHung(hwnd);

    // 设为置顶窗口（跨进程异步投递，避免拖住钩子线程；
    // 目标窗口若以管理员权限运行，会因 UIPI 限制失败）
    if (!hung) {
        SetWindowPosSafe(hwnd, HWND_TOPMOST, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // 移除最小化按钮并刷新窗口框架
    bool minimizeOk = true;
    if (curStyle & WS_MINIMIZEBOX) {
        // 有界探测只在真正要改样式时做，避免每次锁定都白等一轮超时；
        // accessDenied 用于区分"权限不足（永久失败）"与"暂时繁忙（下一轮重试）"
        bool accessDenied = false;
        bool canTouch = CanTouchWindow(hwnd, &accessDenied, 100, deadline);
        if (canTouch) {
            // 有界探测期间目标进程可能修改了其他样式位：写入前重读最新值，
            // 只移除最小化位，不能用等待前的旧快照整体覆盖（R15-M08）
            const LONG_PTR latestStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
            SetLastError(0);
            LONG_PTR prev = SetWindowLongPtrW(hwnd, GWL_STYLE, latestStyle & ~WS_MINIMIZEBOX);
            // SetWindowLongPtrW 失败返回 0，需用 GetLastError 区分"返回值恰为 0"与"调用失败"
            minimizeOk = (prev != 0) || (GetLastError() == 0);
            // 仅刷新框架，不改变 Z-order（上一步已设为 TOPMOST）
            if (!hung) {
                SetWindowPosSafe(hwnd, nullptr,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            }
        }

        // 保护未真正生效时提示一次，避免用户误以为已置顶/已禁止最小化。
        // 置顶走异步投递无法拿到结果，故只以样式修改结果作为判据；
        // 提权窗口（权限不足）属于永久失败要提示，因目标暂时繁忙而跳过的窗口不提示
        if (!hung && (accessDenied || (canTouch && !minimizeOk))) {
            WarnLockFailedOnce();
        } else if (canTouch && minimizeOk) {
            // 本次保护成功生效：复位一次性告警，允许后续新的失败再次提示
            ResetLockFailedWarn();
        }
    } else {
        // 窗口本就没有最小化按钮（保护已满足）：同样视为成功
        ResetLockFailedWarn();
    }
}

// ============================================================
// WinDGuard::ReLockWindow - 重新应用保护
// 仅在窗口状态实际变化时才调用 SetWindowPos，避免不必要的开销
// ============================================================
void WinDGuard::ReLockWindow(LockedWindow& lw, ULONGLONG deadline) {
    if (!IsWindow(lw.hwnd)) return;
    // 已解绑但尚未成功恢复样式：等待 RemoveLock 重试，期间不得重新施加保护
    if (m_pendingRelease.count(lw.hwnd)) return;

    // 校验进程 ID：句柄可能已被回收复用并指向无关窗口，避免误改其样式
    DWORD wpid = 0;
    GetWindowThreadProcessId(lw.hwnd, &wpid);
    if (wpid != lw.processId) return;
    // 无响应窗口跳过本轮（IsHungAppWindow 是廉价内核查询，不发消息）。
    // 恢复最小化/重新置顶走的是 SWP_ASYNCWINDOWPOS 异步投递，本就不会阻塞；
    // 唯一可能同步等待目标的只有下面那次跨进程改样式，因此把有界探测放在那里
    if (IsWindowHung(lw.hwnd)) return;

    // 窗口已被隐藏（如老板键隐藏）：IsIconic 仍可能为真（最小化后被隐藏），
    // 此时恢复显示会把本应隐藏的窗口重新暴露出来。隐藏期间跳过守护，
    // 等窗口重新可见后由下一轮定时检查继续处理
    if (!IsWindowVisible(lw.hwnd)) return;

    bool wasMinimized = IsIconic(lw.hwnd);
    if (wasMinimized) {
        // 使用 SHOWNOACTIVATE 恢复窗口但不抢占焦点，避免干扰用户操作
        ShowWindowSafe(lw.hwnd, SW_SHOWNOACTIVATE);
    }

    // 检查窗口是否失去了 TOPMOST 状态
    LONG_PTR exStyle = GetWindowLongPtrW(lw.hwnd, GWL_EXSTYLE);
    bool lostTopmost = !(exStyle & WS_EX_TOPMOST);

    // 检查 WS_MINIMIZEBOX 是否被恢复（某些窗口会重置样式）
    LONG_PTR currentStyle = GetWindowLongPtrW(lw.hwnd, GWL_STYLE);
    bool minimizeBoxRestored = (currentStyle & WS_MINIMIZEBOX) != 0;

    if (minimizeBoxRestored) {
        // 重新移除最小化按钮：样式位修改本身不会引起重绘（只有
        // SWP_FRAMECHANGED 才会让标题栏闪烁），必须每次都执行；
        // 否则退避期间保护会失效，最小化按钮重新出现。
        // 跨进程改样式是同步等待：目标繁忙时跳过本轮，交给下一轮定时检查
        if (!CanTouchWindow(lw.hwnd, nullptr, 100, deadline)) return;
        // 探测等待期间目标可能修改了其他样式位：重读后只改最小化位（R15-M08）
        const LONG_PTR latestStyle = GetWindowLongPtrW(lw.hwnd, GWL_STYLE);
        if (latestStyle & WS_MINIMIZEBOX) {
            SetWindowLongPtrW(lw.hwnd, GWL_STYLE, latestStyle & ~WS_MINIMIZEBOX);
        }

        // 只有重绘框架（SWP_FRAMECHANGED）才会让标题栏闪烁，因此仅对它做退避：
        // 浏览器导航、Electron/游戏窗口会周期性重建样式，每 3 秒刷一次会持续闪烁
        ULONGLONG now = GetTickCount64();
        if (now - lw.lastFrameFixTick >= STYLE_FIX_INTERVAL_MS) {
            lw.lastFrameFixTick = now;
            SetWindowPosSafe(lw.hwnd, HWND_TOPMOST,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        } else if (lostTopmost || wasMinimized) {
            // 退避期间仍要保证置顶与未最小化，只是不再刷框架
            SetWindowPosSafe(lw.hwnd, HWND_TOPMOST,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    } else if (lostTopmost || wasMinimized) {
        // 窗口失去了置顶状态或刚从最小化恢复，重新设为置顶
        SetWindowPosSafe(lw.hwnd, HWND_TOPMOST,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    // 若无变化则跳过 SetWindowPos，减少不必要的窗口消息
}

// ============================================================
// WinDGuard::ApplyLock - 对所有匹配窗口应用保护
// 枚举所有顶层窗口，对匹配保护规则的窗口调用 LockWindow
// ============================================================
void WinDGuard::ApplyLock() {
    if (m_protectedWindows.empty()) return;

    TopLevelEnumCtx enumCtx{ {}, GetTickCount64() + TITLE_BUDGET_MS,
                             HasProcessBinding(m_protectedWindows),
                             HasWindowBinding(m_protectedWindows) };
    EnumWindows(EnumAllTopLevelWindowsProc, reinterpret_cast<LPARAM>(&enumCtx));
    if (enumCtx.titleBudgetExceeded) WarnTitleBudgetOnce();
    else ResetTitleBudgetWarn();

    ProcessUtils::PathCache cache;  // 单次枚举内复用进程路径，避免相同 pid 重复查询
    // 整轮共享一次探测预算：逐个窗口各等 100ms 会累加，窗口较多时阻塞 UI 线程
    ULONGLONG deadline = GetTickCount64() + TOUCH_BUDGET_MS;
    for (const auto& w : enumCtx.windows) {
        if (IsWindowMatched(w.hwnd, cache, &w.title)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w.hwnd, &pid);
            LockWindow(w.hwnd, pid, deadline);
        }
    }

    // 锁定结果落盘，供程序异常退出后恢复窗口样式；
    // 即使本轮没有窗口被锁定，也要在有遗留记录时重写，避免失效条目长期堆积
    if (!m_lockedWindows.empty() || !OrphanRecords().empty()) SaveLockFile();
}

// 清理：移除所有保护并禁用功能
void WinDGuard::Cleanup() {
    RemoveLock();
    m_enabled = false;
}

// ============================================================
// WinDGuard::RemoveLock - 移除所有窗口保护
// 恢复窗口的原始样式，取消置顶
// ============================================================
void WinDGuard::RemoveLock(const std::unordered_set<HWND>* only) {
    // 逐个处理：能恢复的立即恢复并从记录中移除；目标仍无响应的保留在列表里，
    // 由下一次 RemoveLock（定时重试/退出时析构还会再调一次）或下次启动的
    // RecoverOrphanedStyles 继续恢复，避免样式永久残留
    // 跨进程探测同样共享一次预算，避免"窗口多 + 目标繁忙"时累加阻塞 UI 线程
    ULONGLONG deadline = GetTickCount64() + TOUCH_BUDGET_MS;
    // 本轮仍未能释放的窗口：重试期间不得被 ReLockWindow 重新施加保护
    std::unordered_set<HWND> stillPending;
    for (auto it = m_lockedWindows.begin(); it != m_lockedWindows.end(); ) {
        LockedWindow& lw = *it;

        // 只处理待释放集合中的窗口：其余窗口必须继续保持保护，
        // 否则一个无响应窗口就会让所有受保护窗口周期性失去保护
        if (only && only->find(lw.hwnd) == only->end()) {
            ++it;
            continue;
        }

        // 窗口已销毁或句柄被回收复用（进程 ID 不同）：无需也无法恢复，丢弃记录
        bool stale = !IsWindow(lw.hwnd);
        if (!stale) {
            DWORD wpid = 0;
            GetWindowThreadProcessId(lw.hwnd, &wpid);
            stale = (wpid != lw.processId);
        }
        if (stale) {
            it = m_lockedWindows.erase(it);
            continue;
        }

        // 仅恢复 WS_MINIMIZEBOX 位，保留窗口运行期间的其他样式变化；
        // 仅当样式确有变化时才写回并刷新框架，避免无谓的跨进程窗口操作
        LONG_PTR currentStyle = GetWindowLongPtrW(lw.hwnd, GWL_STYLE);
        LONG_PTR restoredStyle = (currentStyle & ~WS_MINIMIZEBOX) | (lw.originalStyle & WS_MINIMIZEBOX);
        bool styleChanged = (restoredStyle != currentStyle);

        // 跨进程改样式会同步等待目标响应：目标繁忙时本轮跳过，记录保留待重试。
        // 权限永久不足（提权窗口受 UIPI 限制）重试永远不会成功，直接丢弃记录，
        // 避免 CheckNewWindows 每 3 秒无限探测且 .wguard 记录永远残留（R10-M4）
        bool accessDenied = false;
        if (styleChanged && !CanTouchWindow(lw.hwnd, &accessDenied, 100, deadline)) {
            if (accessDenied) {
                it = m_lockedWindows.erase(it);
                continue;
            }
            stillPending.insert(lw.hwnd);
            ++it;
            continue;
        }
        if (styleChanged) {
            // 探测等待期间目标可能修改了其他样式位：重读后只恢复最小化位，
            // 避免用等待前的快照覆盖目标进程的并发变化（R15-M08）
            const LONG_PTR latestStyle = GetWindowLongPtrW(lw.hwnd, GWL_STYLE);
            const LONG_PTR latestRestored =
                (latestStyle & ~WS_MINIMIZEBOX) | (lw.originalStyle & WS_MINIMIZEBOX);
            if (latestRestored != latestStyle) {
                // 写回失败：权限不足视为永久失败并丢弃记录；其他失败保留待重试，
                // 否则窗口的最小化按钮会永久缺失。
                // SetWindowLongPtrW 返回 0 也可能是"原样式恰为 0"，需用 GetLastError 区分
                SetLastError(0);
                LONG_PTR prev = SetWindowLongPtrW(lw.hwnd, GWL_STYLE, latestRestored);
                if (prev == 0 && GetLastError() != 0) {
                    const DWORD err = GetLastError();
                    if (err == ERROR_ACCESS_DENIED) {
                        it = m_lockedWindows.erase(it);
                    } else {
                        stillPending.insert(lw.hwnd);
                        ++it;
                    }
                    continue;
                }
            }
        }

        // 恢复置顶状态。目标可能尚未处理先前异步投递的 TOPMOST，当前
        // exStyle 不能作为“无需撤销”的依据；但只要原始状态不是 TOPMOST，
        // 在刚加锁（异步请求可能仍在队列）或目标无响应时仍必须投递
        // NOTOPMOST（同队列 FIFO、幂等）。超过宽限期且窗口已能正常响应时，
        // 当前 exStyle 可信，不再无谓撤销用户后来手动设置的置顶。
        if (!(lw.originalExStyle & WS_EX_TOPMOST)) {
            const LONG_PTR curExStyle = GetWindowLongPtrW(lw.hwnd, GWL_EXSTYLE);
            const bool pendingAsyncTopmost =
                (GetTickCount64() - lw.lockTick) < 5000 || IsWindowHung(lw.hwnd);
            if ((curExStyle & WS_EX_TOPMOST) || pendingAsyncTopmost) {
                SetWindowPosSafe(lw.hwnd, HWND_NOTOPMOST,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            }
        } else if (styleChanged) {
            // 原本就是 TOPMOST，仅在样式变化时刷新框架（最小化按钮可能已恢复）
            SetWindowPosSafe(lw.hwnd, nullptr,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        it = m_lockedWindows.erase(it);  // 该窗口已恢复完毕
    }

    // 落盘记录与内存保持一致：全部恢复时 SaveLockFile 会删除记录文件；
    // 仍有未恢复条目时写入剩余条目，供下次启动继续恢复
    m_pendingRelease.swap(stillPending);
    SaveLockFile();
}

// ============================================================
// WinDGuard::CheckNewWindows - 定时检查新窗口并重新应用保护
// 由主窗口的 WM_TIMER 消息调用
// 优化：EnumWindows 较重，每3次检查才执行一次全量枚举
//       已锁定窗口的 ReLock 每次都执行（轻量，仅检查状态变化）
// ============================================================
void WinDGuard::CheckNewWindows() {
    // 已解绑但当时目标繁忙、尚未恢复样式的窗口：每轮继续尝试释放，
    // 只处理 m_pendingRelease 中的条目，不能影响其他受保护窗口
    if (!m_pendingRelease.empty()) RemoveLock(&m_pendingRelease);

    if (!m_enabled || m_protectedWindows.empty()) return;

    // 移除已销毁或句柄已被复用（进程 ID 不同）的窗口记录，
    // 并同步收敛落盘记录（否则失效条目会不断累积）
    size_t lockedBefore = m_lockedWindows.size();
    m_lockedWindows.erase(
        std::remove_if(m_lockedWindows.begin(), m_lockedWindows.end(),
            [](const LockedWindow& lw) {
                if (!IsWindow(lw.hwnd)) return true;
                DWORD wpid = 0;
                GetWindowThreadProcessId(lw.hwnd, &wpid);
                return wpid != lw.processId;
            }),
        m_lockedWindows.end());
    if (m_lockedWindows.size() != lockedBefore) SaveLockFile();

    // 对已锁定的窗口重新应用保护（轻量操作，仅状态变化时才调用 SetWindowPos）。
    // 整轮共享一次跨进程探测预算，避免窗口多 + 目标繁忙时逐个 100ms 累加成秒级阻塞
    ULONGLONG deadline = GetTickCount64() + TOUCH_BUDGET_MS;
    for (auto& lw : m_lockedWindows) {
        ReLockWindow(lw, deadline);
    }

    // 每3次检查（约9秒）执行一次 EnumWindows 全量枚举
    // 新窗口出现频率低，无需每次都枚举所有窗口
    ++m_enumCounter;
    if (m_enumCounter < 3) return;
    m_enumCounter = 0;

    // 枚举所有窗口，检查新匹配的窗口（标题只读一次并复用）
    TopLevelEnumCtx enumCtx{ {}, GetTickCount64() + TITLE_BUDGET_MS,
                             HasProcessBinding(m_protectedWindows),
                             HasWindowBinding(m_protectedWindows) };
    EnumWindows(EnumAllTopLevelWindowsProc, reinterpret_cast<LPARAM>(&enumCtx));
    if (enumCtx.titleBudgetExceeded) WarnTitleBudgetOnce();
    else ResetTitleBudgetWarn();

    // 构建已锁定窗口的快速查找集合
    std::unordered_set<HWND> lockedSet;
    lockedSet.reserve(m_lockedWindows.size());
    for (const auto& lw : m_lockedWindows) {
        lockedSet.insert(lw.hwnd);
    }

    ProcessUtils::PathCache cache;  // 单次枚举内复用进程路径，避免相同 pid 重复查询
    lockedBefore = m_lockedWindows.size();
    for (const auto& w : enumCtx.windows) {
        if (lockedSet.find(w.hwnd) == lockedSet.end() &&
            IsWindowMatched(w.hwnd, cache, &w.title)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w.hwnd, &pid);
            LockWindow(w.hwnd, pid, deadline);
        }
    }

    // 新增锁定窗口时更新落盘记录，保证崩溃后能恢复这些窗口的样式
    if (m_lockedWindows.size() != lockedBefore) SaveLockFile();
}
