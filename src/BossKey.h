#pragma once
#include <Windows.h>
#include <vector>
#include <string>
#include "Config.h"

// ============================================================
// BossKey: 老板键管理器
// 核心功能：一键隐藏/恢复指定窗口，支持以下触发方式：
//   - 键盘热键
//   - 鼠标中键/侧键
//   - 鼠标移至屏幕角落
//   - 闲置自动隐藏
// 附加功能：
//   - 隐藏窗口时自动静音
//   - 隐藏窗口前发送媒体暂停
//   - 一键关闭绑定进程
// ============================================================
class BossKey {
public:
    BossKey();
    ~BossKey();

    // 注册老板键热键
    bool Register(HWND hwnd, int hotkeyId, UINT modifiers, UINT vk);
    // 注销老板键热键
    void Unregister();

    // 设置绑定的窗口列表
    void SetWindows(const std::vector<BoundWindowInfo>& windows);
    // 启用/禁用隐藏时静音
    void SetMuteEnabled(bool enabled);
    // 启用/禁用隐藏当前活动窗口
    void SetHideCurrentWindow(bool enabled);
    // 启用/禁用隐藏前发送媒体暂停
    void SetSendPauseBeforeHide(bool enabled);

    // 激活老板键（隐藏窗口）
    void Activate();
    // 停用老板键（恢复窗口）
    void Deactivate();
    // 当前是否处于激活状态
    bool IsActive() const { return m_active; }
    // 获取上次停用的时间戳（用于自动隐藏的冷却判断）
    ULONGLONG GetLastDeactivateTick() const { return m_lastDeactivateTick; }
    // 切换激活/停用状态
    void Toggle();
    // 关闭所有绑定的进程（优雅关闭 -> 超时强制终止）
    // timeoutMs：等待进程退出的超时时间，关机路径可传较小值避免阻塞
    void CloseBoundProcesses(DWORD timeoutMs = PROCESS_CLOSE_TIMEOUT_MS);
    // 恢复上次异常退出时被隐藏的窗口（启动时调用），返回恢复的窗口数
    int RecoverOrphanedWindows();
    // 将当前被隐藏窗口的信息持久化到恢复文件（关机前调用）
    void SaveRecoverFile();

    // 安装鼠标低级钩子（用于中键/侧键触发）
    bool InstallMouseHook(HWND notifyWnd);
    // 卸载鼠标钩子
    void UninstallMouseHook();
    // 更新鼠标按钮触发设置
    void UpdateMouseHookSettings(bool middleBtn, bool sideBtn1, bool sideBtn2);

private:
    // 隐藏所有绑定窗口
    void HideWindows();
    // 恢复所有隐藏窗口
    void ShowWindows();
    // 设置系统静音/取消静音（通过 Windows Core Audio API）
    void SetMute(bool mute, bool forceUnmute = false);
    // 发送媒体暂停/播放按键
    void SendMediaPause();
    // 获取进程完整路径（委托给 ProcessUtils）
    static std::wstring GetProcessPath(DWORD pid) { return ProcessUtils::GetProcessPath(pid); }

    // 鼠标低级钩子回调
    static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam);

    // 获取恢复文件路径（exe 同目录下 KeySentry.recover）
    static std::wstring GetRecoverFilePath();
    // 删除恢复文件（窗口恢复/进程关闭后调用）
    void ClearRecoverFile();

    bool m_active = false;          // 当前是否处于激活（隐藏）状态
    HWND m_hwnd = nullptr;          // 热键所属窗口句柄
    int m_hotkeyId = 0;             // 热键ID
    UINT m_modifiers = 0;           // 热键修饰键
    UINT m_vk = 0;                  // 热键虚拟键码
    bool m_registered = false;      // 热键是否已注册
    bool m_muteEnabled = true;      // 隐藏时是否静音
    bool m_hideCurrentWindow = false; // 是否隐藏当前活动窗口
    bool m_sendPauseBeforeHide = false; // 隐藏前是否发送媒体暂停

    // 被隐藏的窗口信息，用于恢复时还原窗口状态
    struct HiddenWindow {
        HWND hwnd;                  // 窗口句柄
        int showCmd;                // 隐藏前的窗口显示状态（正常/最小化/最大化）
        DWORD pid;                  // 进程ID
        std::wstring processPath;   // 进程路径
    };
    std::vector<HiddenWindow> m_hiddenWindows; // 被隐藏的窗口列表
    std::vector<BoundWindowInfo> m_boundWindows; // 绑定的窗口匹配规则
    bool m_wasMuted = false;        // 隐藏前系统是否已静音
    ULONGLONG m_lastDeactivateTick = 0; // 上次停用的时间戳

    HHOOK m_mouseHook = nullptr;    // 鼠标低级钩子句柄
    HWND m_mouseNotifyWnd = nullptr; // 鼠标事件通知窗口
    bool m_middleBtnEnabled = false; // 中键触发老板键
    bool m_sideBtn1Enabled = false;  // 侧键1触发老板键
    bool m_sideBtn2Enabled = false;  // 侧键2触发老板键
    static BossKey* s_instance;     // 单例指针，供静态回调使用
};
