#include "WinDGuard.h"
#include "Utils.h"
#include <algorithm>
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
static void WarnLockFailedOnce() {
    static bool warned = false;
    if (warned) return;
    warned = true;
    ShowTrayNotification(L"部分目标窗口以管理员权限运行，置顶/禁止最小化保护无法生效。", true);
}

// ============================================================
// 跨进程窗口操作的非阻塞辅助
// 背景：WinDGuard 的定时检查与"应用"操作都运行在承载低级键鼠钩子的
//       UI 线程上。ShowWindow / SetWindowPos 会向目标窗口线程同步发送
//       消息，目标进程无响应时会卡住该线程，导致全系统鼠标/键盘输入停滞。
// ============================================================
namespace {
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
std::wstring WinDGuard::GetLockFilePath() {
    // 路径在进程生命周期内固定（exe 目录与可写性不变），缓存探测结果，
    // 避免每次保存/删除记录文件都创建一个临时探测文件
    static const std::wstring cachedPath = []() -> std::wstring {
        // 动态缓冲取 exe 路径，避免超过 MAX_PATH 的安装目录被截断
        std::wstring dir = ProcessUtils::GetModuleDir();

        // 用自动删除的临时文件探测目录可写性（与 INI 回退策略一致）
        std::wstring testPath = dir + L".~wgtest.tmp";
        HANDLE hTest = CreateFileW(testPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (hTest != INVALID_HANDLE_VALUE) {
            CloseHandle(hTest);  // 关闭即自动删除测试文件
            return dir + L"KeySentry.wguard";
        }

        std::wstring appData = ProcessUtils::GetAppDataDir();
        if (!appData.empty()) {
            std::wstring ad = appData + L"\\KeySentry";
            CreateDirectoryW(ad.c_str(), nullptr);
            return ad + L"\\KeySentry.wguard";
        }
        return dir + L"KeySentry.wguard";
    }();
    return cachedPath;
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

    // 原子写入：先写临时文件再重命名替换，避免崩溃时记录文件被截断损坏
    std::wstring path = GetLockFilePath();
    std::wstring tmpPath = path + L".tmp";
    HANDLE hFile = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(hFile, content.data(), (DWORD)content.size(), &written, nullptr);
    CloseHandle(hFile);
    // 重命名失败时清理临时文件，避免 .tmp 残留
    if (!MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmpPath.c_str());
    }
}

void WinDGuard::ClearLockFile() {
    DeleteFileW(GetLockFilePath().c_str());
}

// 恢复上一次运行遗留的窗口样式（崩溃兜底）。
// 仅在 hwnd 仍有效且进程 ID 一致时恢复，避免句柄被复用后误改无关窗口。
int WinDGuard::RecoverOrphanedStyles(bool removeFile) {
    std::wstring path = GetLockFilePath();
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return 0;

    std::string data;
    char buf[2048];
    DWORD read = 0;
    while (ReadFile(hFile, buf, sizeof(buf), &read, nullptr) && read > 0) {
        data.append(buf, (size_t)read);
    }
    CloseHandle(hFile);

    size_t pos = 0;
    int repaired = 0;
    // 本轮无法恢复的记录（目标无响应）先收集到类级存储，随后与普通记录一起
    // 落盘，保证下次启动还能继续恢复
    OrphanRecords().clear();
    // 有窗口未响应而被跳过恢复：此时必须保留落盘记录，下次启动继续恢复，
    // 否则样式会永久残留在这批窗口上
    bool skipped = false;
    while (pos < data.size()) {
        size_t eol = data.find('\n', pos);
        if (eol == std::string::npos) eol = data.size();
        std::string line = data.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty()) continue;

        size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        size_t t3 = line.find('\t', t2 + 1);
        if (t3 == std::string::npos) continue;

        DWORD pid = (DWORD)std::strtoul(line.substr(0, t1).c_str(), nullptr, 10);
        HWND hwnd = (HWND)(uintptr_t)std::strtoull(line.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 10);
        LONG_PTR origStyle = (LONG_PTR)std::strtoll(line.substr(t2 + 1, t3 - t2 - 1).c_str(), nullptr, 10);
        LONG_PTR origExStyle = (LONG_PTR)std::strtoll(line.substr(t3 + 1).c_str(), nullptr, 10);

        if (!hwnd || !IsWindow(hwnd)) continue;
        DWORD wpid = 0;
        GetWindowThreadProcessId(hwnd, &wpid);
        if (wpid != pid) continue;  // 句柄已被复用，跳过以免误改

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
            if (!CanTouchWindow(hwnd, &accessDenied)) {
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
                    OrphanRecords().push_back(orphan);
                }
            }
        }
        if (needMinimizeBox && !styleSkipped) {
            // SetWindowLongPtr 返回 0 且 GetLastError 非 0 才算失败（UIPI 拦截提权窗口等），
            // 失败时不能计入"已修复"，否则提示数量会虚高
            SetLastError(0);
            LONG_PTR prev = SetWindowLongPtrW(hwnd, GWL_STYLE, curStyle | WS_MINIMIZEBOX);
            if (prev == 0 && GetLastError() != 0) needMinimizeBox = false;
        }

        // 恢复非置顶状态（仅当原本不置顶、当前却置顶时）
        LONG_PTR curExStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        bool needUntopmost = (!(origExStyle & WS_EX_TOPMOST)) && ((curExStyle & WS_EX_TOPMOST) != 0);

        if (needUntopmost) {
            SetWindowPosSafe(hwnd, HWND_NOTOPMOST,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        } else if (needMinimizeBox && !styleSkipped) {
            SetWindowPosSafe(hwnd, nullptr,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        // styleSkipped 的窗口本轮并未真正恢复，不计入"已修复"数量
        if (needUntopmost || (needMinimizeBox && !styleSkipped)) repaired++;
    }

    // 恢复动作全部完成后再删除记录文件：若恢复到一半再次异常退出，
    // 文件仍保留，下次启动可继续恢复剩余窗口，避免样式永久残留；
    // 若本轮有窗口因无响应被跳过，同样保留记录
    if (removeFile && !skipped) DeleteFileW(path.c_str());
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
bool WinDGuard::IsWindowMatched(HWND hwnd, ProcessUtils::PathCache& cache) const {
    for (const auto& info : m_protectedWindows) {
        if (info.Matches(hwnd, cache)) return true;
    }
    return false;
}

// EnumWindows 回调：收集所有可见的顶层窗口
static BOOL CALLBACK EnumAllTopLevelWindowsProc(HWND hwnd, LPARAM lParam) {
    if (IsWindowVisible(hwnd) == FALSE) return TRUE;
    if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) return TRUE;
    // 跳过任务栏/桌面等外壳窗口：与老板键保持一致，避免按进程绑定
    // explorer.exe 时把外壳改成置顶/去掉最小化按钮
    if (WindowUtils::IsShellWindow(hwnd)) return TRUE;

    // 安全读取标题：目标进程无响应时不会阻塞承载钩子的 UI 线程
    std::wstring title = WindowUtils::GetTextSafe(hwnd);
    if (title.empty()) return TRUE;

    auto* data = reinterpret_cast<std::vector<HWND>*>(lParam);
    data->push_back(hwnd);
    return TRUE;
}

// ============================================================
// WinDGuard::LockWindow - 对单个窗口应用保护
//   1. 记录原始窗口样式（用于恢复）
//   2. 将窗口设为置顶
//   3. 移除 WS_MINIMIZEBOX 样式（禁止最小化）
// ============================================================
void WinDGuard::LockWindow(HWND hwnd, DWORD pid, ULONGLONG deadline) {
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
            SetLastError(0);
            LONG_PTR prev = SetWindowLongPtrW(hwnd, GWL_STYLE, curStyle & ~WS_MINIMIZEBOX);
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
        if (!hung && (accessDenied || (canTouch && !minimizeOk))) WarnLockFailedOnce();
    }
}

// ============================================================
// WinDGuard::ReLockWindow - 重新应用保护
// 仅在窗口状态实际变化时才调用 SetWindowPos，避免不必要的开销
// ============================================================
void WinDGuard::ReLockWindow(LockedWindow& lw) {
    if (!IsWindow(lw.hwnd)) return;
    // 校验进程 ID：句柄可能已被回收复用并指向无关窗口，避免误改其样式
    DWORD wpid = 0;
    GetWindowThreadProcessId(lw.hwnd, &wpid);
    if (wpid != lw.processId) return;
    // 无响应窗口跳过本轮（IsHungAppWindow 是廉价内核查询，不发消息）。
    // 恢复最小化/重新置顶走的是 SWP_ASYNCWINDOWPOS 异步投递，本就不会阻塞；
    // 唯一可能同步等待目标的只有下面那次跨进程改样式，因此把有界探测放在那里
    if (IsWindowHung(lw.hwnd)) return;

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
        if (!CanTouchWindow(lw.hwnd)) return;
        SetWindowLongPtrW(lw.hwnd, GWL_STYLE, currentStyle & ~WS_MINIMIZEBOX);

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

    std::vector<HWND> allWindows;
    EnumWindows(EnumAllTopLevelWindowsProc, reinterpret_cast<LPARAM>(&allWindows));

    ProcessUtils::PathCache cache;  // 单次枚举内复用进程路径，避免相同 pid 重复查询
    // 整轮共享一次探测预算：逐个窗口各等 100ms 会累加，窗口较多时阻塞 UI 线程
    ULONGLONG deadline = GetTickCount64() + TOUCH_BUDGET_MS;
    for (HWND hwnd : allWindows) {
        if (IsWindowMatched(hwnd, cache)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            LockWindow(hwnd, pid, deadline);
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
void WinDGuard::RemoveLock() {
    // 逐个处理：能恢复的立即恢复并从记录中移除；目标仍无响应的保留在列表里，
    // 由下一次 RemoveLock（退出时析构还会再调一次）或下次启动的
    // RecoverOrphanedStyles 继续恢复，避免样式永久残留
    // 跨进程探测同样共享一次预算，避免"窗口多 + 目标繁忙"时累加阻塞 UI 线程
    ULONGLONG deadline = GetTickCount64() + TOUCH_BUDGET_MS;
    for (auto it = m_lockedWindows.begin(); it != m_lockedWindows.end(); ) {
        LockedWindow& lw = *it;

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

        // 跨进程改样式会同步等待目标响应：目标繁忙时本轮跳过，记录保留待重试
        if (styleChanged && !CanTouchWindow(lw.hwnd, nullptr, 100, deadline)) {
            ++it;
            continue;
        }
        if (styleChanged) {
            SetWindowLongPtrW(lw.hwnd, GWL_STYLE, restoredStyle);
        }

        // 恢复置顶状态：仅当窗口原本不是 TOPMOST 时才取消置顶
        // 避免破坏用户手动设置的"始终置顶"窗口（如任务管理器）
        if (!(lw.originalExStyle & WS_EX_TOPMOST)) {
            LONG_PTR curExStyle = GetWindowLongPtrW(lw.hwnd, GWL_EXSTYLE);
            if (curExStyle & WS_EX_TOPMOST) {
                SetWindowPosSafe(lw.hwnd, HWND_NOTOPMOST,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            } else if (styleChanged) {
                SetWindowPosSafe(lw.hwnd, nullptr,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
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
    SaveLockFile();
}

// ============================================================
// WinDGuard::CheckNewWindows - 定时检查新窗口并重新应用保护
// 由主窗口的 WM_TIMER 消息调用
// 优化：EnumWindows 较重，每3次检查才执行一次全量枚举
//       已锁定窗口的 ReLock 每次都执行（轻量，仅检查状态变化）
// ============================================================
void WinDGuard::CheckNewWindows() {
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

    // 对已锁定的窗口重新应用保护（轻量操作，仅状态变化时才调用 SetWindowPos）
    for (auto& lw : m_lockedWindows) {
        ReLockWindow(lw);
    }

    // 每3次检查（约9秒）执行一次 EnumWindows 全量枚举
    // 新窗口出现频率低，无需每次都枚举所有窗口
    ++m_enumCounter;
    if (m_enumCounter < 3) return;
    m_enumCounter = 0;

    // 枚举所有窗口，检查新匹配的窗口
    std::vector<HWND> allWindows;
    EnumWindows(EnumAllTopLevelWindowsProc, reinterpret_cast<LPARAM>(&allWindows));

    // 构建已锁定窗口的快速查找集合
    std::unordered_set<HWND> lockedSet;
    lockedSet.reserve(m_lockedWindows.size());
    for (const auto& lw : m_lockedWindows) {
        lockedSet.insert(lw.hwnd);
    }

    ProcessUtils::PathCache cache;  // 单次枚举内复用进程路径，避免相同 pid 重复查询
    lockedBefore = m_lockedWindows.size();
    for (HWND hwnd : allWindows) {
        if (IsWindowMatched(hwnd, cache) && lockedSet.find(hwnd) == lockedSet.end()) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            LockWindow(hwnd, pid);
        }
    }

    // 新增锁定窗口时更新落盘记录，保证崩溃后能恢复这些窗口的样式
    if (m_lockedWindows.size() != lockedBefore) SaveLockFile();
}
