#pragma once
#include <Windows.h>
#include <vector>
#include <string>
#include "Config.h"

// ============================================================
// WinDGuard: 窗口保护管理器
// 防止指定窗口被最小化（如 Win+D 快捷键导致的桌面显示）
// 实现方式：
//   - 将匹配窗口设为置顶（HWND_TOPMOST）
//   - 移除窗口的 WS_MINIMIZEBOX 样式，禁止最小化
//   - 定时检查并重新应用保护（应对新窗口和状态变化）
// ============================================================
class WinDGuard {
public:
    WinDGuard();
    ~WinDGuard();

    // 一次性配置启用状态和窗口列表
    void Configure(bool enabled, const std::vector<BoundWindowInfo>& windows);
    // 检查新窗口并重新应用保护（由定时器调用）
    void CheckNewWindows();
    // 清理：移除所有保护并禁用功能
    void Cleanup();

private:
    // 被锁定保护的窗口信息
    struct LockedWindow {
        HWND hwnd;                  // 窗口句柄
        LONG_PTR originalStyle;     // 原始窗口样式（用于恢复）
        LONG_PTR originalExStyle;   // 原始扩展样式（用于恢复）
        DWORD processId;            // 进程ID
    };

    // 对所有匹配窗口应用保护
    void ApplyLock();
    // 移除所有窗口保护，恢复原始样式
    void RemoveLock();
    // 对单个窗口应用保护（置顶 + 移除最小化按钮）
    void LockWindow(HWND hwnd, DWORD pid);
    // 重新应用保护（恢复最小化的窗口、重新移除最小化按钮）
    void ReLockWindow(LockedWindow& lw);
    // 检查指定窗口是否匹配保护规则
    bool IsWindowMatched(HWND hwnd) const;

    bool m_enabled = false;                             // 是否启用
    std::vector<BoundWindowInfo> m_protectedWindows;    // 受保护窗口匹配规则
    std::vector<LockedWindow> m_lockedWindows;          // 已锁定保护的窗口列表
    int m_enumCounter = 0;                              // EnumWindows 调用计数器，降低枚举频率
};
