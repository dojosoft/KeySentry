#include "WinDGuard.h"
#include "Utils.h"
#include <algorithm>
#include <unordered_set>

WinDGuard::WinDGuard() = default;

WinDGuard::~WinDGuard() {
    RemoveLock();
}

// 一次性配置启用状态和窗口列表
void WinDGuard::Configure(bool enabled, const std::vector<BoundWindowInfo>& windows) {
    if (m_enabled) RemoveLock();       // 先移除旧保护（无论新状态是否启用）
    m_enabled = enabled;
    m_protectedWindows = windows;
    m_enumCounter = 0;
    if (m_enabled && !m_protectedWindows.empty()) {
        ApplyLock();                    // 仅调用一次 ApplyLock
    }
}

// 检查指定窗口是否匹配任一保护规则
bool WinDGuard::IsWindowMatched(HWND hwnd) const {
    for (const auto& info : m_protectedWindows) {
        if (info.Matches(hwnd)) return true;
    }
    return false;
}

// EnumWindows 回调：收集所有可见的顶层窗口
static BOOL CALLBACK EnumAllTopLevelWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) return TRUE;

    wchar_t title[512] = {};
    if (GetWindowTextW(hwnd, title, 512) == 0) return TRUE;
    if (wcslen(title) == 0) return TRUE;

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
    // 避免重复锁定
    for (const auto& lw : m_lockedWindows) {
        if (lw.hwnd == hwnd) return;
    }

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    LockedWindow lw;
    lw.hwnd = hwnd;
    lw.originalStyle = style;
    lw.originalExStyle = exStyle;
    lw.processId = pid;
    m_lockedWindows.push_back(lw);

    // 设为置顶窗口
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // 移除最小化按钮并刷新窗口框架
    if (style & WS_MINIMIZEBOX) {
        SetWindowLongPtrW(hwnd, GWL_STYLE, style & ~WS_MINIMIZEBOX);
        // 仅刷新框架，不改变 Z-order（上一步已设为 TOPMOST）
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

// ============================================================
// WinDGuard::ReLockWindow - 重新应用保护
// 仅在窗口状态实际变化时才调用 SetWindowPos，避免不必要的开销
// ============================================================
void WinDGuard::ReLockWindow(LockedWindow& lw) {
    if (!IsWindow(lw.hwnd)) return;

    bool wasMinimized = IsIconic(lw.hwnd);
    if (wasMinimized) {
        // 使用 SHOWNOACTIVATE 恢复窗口但不抢占焦点，避免干扰用户操作
        ShowWindow(lw.hwnd, SW_SHOWNOACTIVATE);
    }

    // 检查窗口是否失去了 TOPMOST 状态
    LONG_PTR exStyle = GetWindowLongPtrW(lw.hwnd, GWL_EXSTYLE);
    bool lostTopmost = !(exStyle & WS_EX_TOPMOST);

    // 检查 WS_MINIMIZEBOX 是否被恢复（某些窗口会重置样式）
    LONG_PTR currentStyle = GetWindowLongPtrW(lw.hwnd, GWL_STYLE);
    bool minimizeBoxRestored = (currentStyle & WS_MINIMIZEBOX) != 0;

    if (minimizeBoxRestored) {
        // 需要重新移除最小化按钮并刷新框架
        SetWindowLongPtrW(lw.hwnd, GWL_STYLE, currentStyle & ~WS_MINIMIZEBOX);
        SetWindowPos(lw.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    } else if (lostTopmost || wasMinimized) {
        // 窗口失去了置顶状态或刚从最小化恢复，重新设为置顶
        SetWindowPos(lw.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
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

    for (HWND hwnd : allWindows) {
        if (IsWindowMatched(hwnd)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            LockWindow(hwnd, pid);
        }
    }
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

        // 仅恢复 WS_MINIMIZEBOX 位，保留窗口运行期间的其他样式变化
        LONG_PTR currentStyle = GetWindowLongPtrW(lw.hwnd, GWL_STYLE);
        LONG_PTR restoredStyle = (currentStyle & ~WS_MINIMIZEBOX) | (lw.originalStyle & WS_MINIMIZEBOX);
        SetWindowLongPtrW(lw.hwnd, GWL_STYLE, restoredStyle);

        // 恢复置顶状态：仅当窗口原本不是 TOPMOST 时才取消置顶
        // 避免破坏用户手动设置的"始终置顶"窗口（如任务管理器）
        if (!(lw.originalExStyle & WS_EX_TOPMOST)) {
            SetWindowPos(lw.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        } else {
            // 原本就是 TOPMOST，只需刷新框架（最小化按钮可能已恢复）
            SetWindowPos(lw.hwnd, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }
    m_lockedWindows.clear();
}

// ============================================================
// WinDGuard::CheckNewWindows - 定时检查新窗口并重新应用保护
// 由主窗口的 WM_TIMER 消息调用
// 优化：EnumWindows 较重，每3次检查才执行一次全量枚举
//       已锁定窗口的 ReLock 每次都执行（轻量，仅检查状态变化）
// ============================================================
void WinDGuard::CheckNewWindows() {
    if (!m_enabled || m_protectedWindows.empty()) return;

    // 移除已销毁的窗口记录
    m_lockedWindows.erase(
        std::remove_if(m_lockedWindows.begin(), m_lockedWindows.end(),
            [](const LockedWindow& lw) { return !IsWindow(lw.hwnd); }),
        m_lockedWindows.end());

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

    for (HWND hwnd : allWindows) {
        if (IsWindowMatched(hwnd) && lockedSet.find(hwnd) == lockedSet.end()) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            LockWindow(hwnd, pid);
        }
    }
}
