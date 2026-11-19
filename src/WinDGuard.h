#pragma once
#include <Windows.h>
#include <vector>
#include <string>
#include <unordered_set>
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
    // 是否仍有“已解绑但因目标繁忙未恢复样式”的窗口。
    // 守护关闭后调用方仍需保留定时器，直到这些窗口全部恢复
    bool HasPendingRelease() const { return !m_pendingRelease.empty(); }
    // 清理：移除所有保护并禁用功能
    void Cleanup();

    // 读取上一次运行遗留的窗口样式记录并恢复（程序异常退出时的兜底）。
    // removeFile=true 表示恢复后删除记录文件（启动路径使用）；
    // removeFile=false 用于“修复窗口堆叠”，保留记录文件以免影响运行中实例的崩溃恢复。
    // 返回实际恢复的窗口数量（仅统计键客记录过的窗口）。
    static int RecoverOrphanedStyles(bool removeFile = true);

private:
    // 被锁定保护的窗口信息
    struct LockedWindow {
        HWND hwnd;                  // 窗口句柄
        LONG_PTR originalStyle;     // 原始窗口样式（用于恢复）
        LONG_PTR originalExStyle;   // 原始扩展样式（用于恢复）
        DWORD processId;            // 进程ID
        // 上次重建窗口框架（SWP_FRAMECHANGED）的时刻：部分程序会周期性
        // 重建样式，每轮都重建会让标题栏反复闪烁，据此做重试退避
        ULONGLONG lastFrameFixTick = 0;
        // 本次加锁时刻：释放时用于判断异步 TOPMOST 是否仍在队列中
        ULONGLONG lockTick = 0;
    };

    // 对所有匹配窗口应用保护
    void ApplyLock();
    // 移除窗口保护并恢复原始样式。
    // only 为空：处理全部已锁定窗口；
    // only 非空：只处理该集合中的窗口，其余窗口继续保持保护（用于重试待释放项）
    void RemoveLock(const std::unordered_set<HWND>* only = nullptr);
    // 对单个窗口应用保护（置顶 + 移除最小化按钮）
    // deadline 非 0 时为本轮跨进程探测的总时间上限（GetTickCount64 基准），
    // 预算耗尽即跳过该窗口的样式修改，交给下一轮重试
    void LockWindow(HWND hwnd, DWORD pid, ULONGLONG deadline = 0);
    // 重新应用保护（恢复最小化的窗口、重新移除最小化按钮）
    // deadline 非 0 时为本轮跨进程探测的总时间上限，避免逐个窗口各等 100ms 累加
    void ReLockWindow(LockedWindow& lw, ULONGLONG deadline = 0);
    // 检查指定窗口是否匹配保护规则（cache 用于复用进程路径查询结果）
    bool IsWindowMatched(HWND hwnd, ProcessUtils::PathCache& cache,
                         const std::wstring* cachedTitle = nullptr) const;

    // 将当前已锁定窗口的原始样式落盘（用于崩溃后恢复，防止永久破坏其他进程窗口样式）
    void SaveLockFile();
    // 删除落盘的窗口样式记录
    void ClearLockFile();
    // 获取窗口样式记录文件路径（优先 exe 同目录，目录只读时回退 %APPDATA%\KeySentry）
    static std::wstring GetLockFilePath();
    // 首选写入路径的缓存：写失败后可切换到另一候选位置（R15-A02）
    static std::wstring& LockPathCache();

    bool m_enabled = false;                             // 是否启用
    std::vector<BoundWindowInfo> m_protectedWindows;    // 受保护窗口匹配规则
    std::vector<LockedWindow> m_lockedWindows;          // 已锁定保护的窗口列表
    // 已解绑但当时目标繁忙、尚未成功恢复样式的窗口：
    // 保留在 m_lockedWindows 中继续重试，但在释放成功前不得重新施加保护，
    // 否则用户删除绑定后窗口仍会被持续置顶、最小化按钮仍被移除
    std::unordered_set<HWND> m_pendingRelease;
    // 上次运行遗留、本轮因目标无响应而未能恢复的样式记录。
    // 必须与 m_lockedWindows 一起落盘：单独存放期间若被一次 SaveLockFile
    // 覆盖丢失，这些窗口的原始样式就再也无法恢复（最小化按钮永久消失）
    static std::vector<LockedWindow>& OrphanRecords();
    int m_enumCounter = 0;                              // EnumWindows 调用计数器，降低枚举频率
    // 上次成功写入锁文件的完整内容：内容未变化时跳过原子写与 Flush，
    // 避免目标长期无响应时每 3 秒重复写盘。
    std::string m_lastLockContent;
};
