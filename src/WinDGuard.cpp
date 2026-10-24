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
    if (m_lockedWindows.empty()) {
        ClearLockFile();
        return;
    }

    std::string content;
    for (const auto& lw : m_lockedWindows) {
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
        if (needMinimizeBox) {
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
        } else if (needMinimizeBox) {
            SetWindowPosSafe(hwnd, nullptr,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        if (needUntopmost || needMinimizeBox) repaired++;
    }

    // 恢复动作全部完成后再删除记录文件：若恢复到一半再次异常退出，
    // 文件仍保留，下次启动可继续恢复剩余窗口，避免样式永久残留
    if (removeFile) DeleteFileW(path.c_str());
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
void WinDGuard::LockWindow(HWND hwnd, DWORD pid) {
    // 避免重复锁定（句柄被复用时进程 ID 会不同，需继续按新窗口处理）
    for (const auto& lw : m_lockedWindows) {
        if (lw.hwnd == hwnd && lw.processId == pid) return;
    }

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    LockedWindow lw;
    lw.hwnd = hwnd;
    lw.originalStyle = style;
    lw.originalExStyle = exStyle;
    lw.processId = pid;
    m_lockedWindows.push_back(lw);

    // 目标无响应时跳过窗口操作，留给下一轮定时检查重试；
    // 样式修改本身是内核操作，不会阻塞，仍先记录原始样式
    bool hung = IsWindowHung(hwnd);

    // 设为置顶窗口（跨进程异步投递，避免拖住钩子线程；
    // 目标窗口若以管理员权限运行，会因 UIPI 限制失败）
    if (!hung) {
        SetWindowPosSafe(hwnd, HWND_TOPMOST, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // 移除最小化按钮并刷新窗口框架
    bool minimizeOk = true;
    if (style & WS_MINIMIZEBOX) {
        SetLastError(0);
        LONG_PTR prev = SetWindowLongPtrW(hwnd, GWL_STYLE, style & ~WS_MINIMIZEBOX);
        // SetWindowLongPtrW 失败返回 0，需用 GetLastError 区分"返回值恰为 0"与"调用失败"
        minimizeOk = (prev != 0) || (GetLastError() == 0);
        // 仅刷新框架，不改变 Z-order（上一步已设为 TOPMOST）
        if (!hung) {
            SetWindowPosSafe(hwnd, nullptr,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }

    // 保护未真正生效时提示一次，避免用户误以为已置顶/已禁止最小化。
    // 置顶走异步投递无法拿到结果，故只以样式修改结果作为判据
    if (!hung && !minimizeOk) WarnLockFailedOnce();
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
    // 目标无响应时跳过本轮，下一轮定时检查再试：
    // ShowWindow / SetWindowPos 会同步等待目标线程，可能卡死承载钩子的 UI 线程
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
        // 重新移除最小化按钮：SetWindowLongPtrW 不发送窗口消息、不会引起闪烁，
        // 必须每次都执行；否则退避期间保护会失效，最小化按钮重新出现
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
    for (HWND hwnd : allWindows) {
        if (IsWindowMatched(hwnd, cache)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            LockWindow(hwnd, pid);
        }
    }

    // 锁定结果落盘，供程序异常退出后恢复窗口样式
    if (!m_lockedWindows.empty()) SaveLockFile();
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
    for (auto& lw : m_lockedWindows) {
        if (!IsWindow(lw.hwnd)) continue;

        // 校验进程 ID：句柄可能已被回收复用，指向无关窗口，此时跳过以免误改
        DWORD wpid = 0;
        GetWindowThreadProcessId(lw.hwnd, &wpid);
        if (wpid != lw.processId) continue;

        // 仅恢复 WS_MINIMIZEBOX 位，保留窗口运行期间的其他样式变化；
        // 仅当样式确有变化时才写回并刷新框架，避免无谓的跨进程窗口操作
        LONG_PTR currentStyle = GetWindowLongPtrW(lw.hwnd, GWL_STYLE);
        LONG_PTR restoredStyle = (currentStyle & ~WS_MINIMIZEBOX) | (lw.originalStyle & WS_MINIMIZEBOX);
        bool styleChanged = (restoredStyle != currentStyle);
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
    }
    m_lockedWindows.clear();
    // 已全部恢复原始样式，删除落盘记录
    ClearLockFile();
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
