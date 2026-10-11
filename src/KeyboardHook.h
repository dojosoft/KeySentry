#pragma once
#include <Windows.h>
#include <vector>
#include <utility>

// ============================================================
// KeyboardHook: 低级键盘钩子管理器
// 通过 WH_KEYBOARD_LL 钩子拦截全局键盘事件，实现以下功能：
//   - 屏蔽指定按键
//   - 禁用 NumLock 键
//   - 强制 Insert 模式（屏蔽 Insert 键）
//   - 按键重映射
//   - 锁键状态浮层通知
//   - 静音状态浮层通知
// ============================================================
class KeyboardHook {
public:
    KeyboardHook();
    ~KeyboardHook();

    // 安装低级键盘钩子，notifyWnd 为接收通知消息的窗口
    bool Install(HWND notifyWnd);
    // 卸载键盘钩子
    void Uninstall();

    // 设置被禁用的按键列表
    void SetDisabledKeys(const std::vector<int>& codes);
    // 启用/禁用指定按键屏蔽功能
    void SetDisableSpecifiedKeysEnabled(bool enabled);
    // 启用/禁用 NumLock 键屏蔽
    void SetDisableNumLock(bool disable);
    // 启用/禁用 Insert 键屏蔽（强制 Insert 模式）
    void SetForceInsertMode(bool force);
    // 启用/禁用锁键状态浮层通知
    void SetShowOverlay(bool show);
    // 启用/禁用静音状态浮层通知（与锁键浮层相互独立）
    void SetShowMuteOverlay(bool show);
    // 启用/禁用按键重映射
    void SetKeyRemapEnabled(bool enabled);
    // 设置按键重映射表：{源键虚拟码, 目标键虚拟码}
    void SetKeyRemappings(const std::vector<std::pair<int, int>>& remappings);
    // 标记当前正在模拟输入（避免钩子递归拦截自己发送的按键）
    void SetSimulatingInput(bool sim);
    // 启用/禁用热键捕获模式（捕获模式下不拦截任何按键）
    void SetCaptureMode(bool enabled);

private:
    // 低级键盘钩子回调函数
    static LRESULT CALLBACK LowLevelProc(int nCode, WPARAM wParam, LPARAM lParam);

    HHOOK m_hook = nullptr;            // 钩子句柄
    HWND m_notifyWnd = nullptr;        // 通知窗口句柄
    std::vector<int> m_disabledKeys;   // 被禁用的按键虚拟码列表
    bool m_disableSpecifiedKeysEnabled = false; // 是否启用指定按键屏蔽
    bool m_disableNumLock = false;     // 是否禁用 NumLock 键
    bool m_forceInsert = false;        // 是否强制 Insert 模式
    bool m_showOverlay = true;         // 是否显示锁键状态浮层
    bool m_showMuteOverlay = false;    // 是否显示静音状态浮层
    bool m_keyRemapEnabled = false;    // 是否启用按键重映射
    std::vector<std::pair<int, int>> m_remappings; // 按键重映射表
    bool m_simulatingInput = false;    // 正在模拟输入（用于防止递归拦截）
    bool m_captureMode = false;        // 热键捕获模式（不拦截任何按键）

    // 锁键连发抑制：只在物理按下的第一次 keydown 通知，keyup 时清除标志。
    // 锁键被按住时系统会持续产生 keydown 连发消息，若不抑制会高频触发浮层
    // 置顶重排与重绘，导致浮层闪烁并使任务栏/DWM 卡顿。
    bool m_lockKeyHeld[3] = { false, false, false }; // Caps/Num/Scroll 是否已按下
    ULONGLONG m_lockKeyHeldTick[3] = { 0, 0, 0 };    // 按下时刻（超时兜底，防止标志卡住）
    // 静音键连发抑制：与锁键同理，只在首次物理按下时通知（避免连发时反复枚举音频设备）
    bool m_muteKeyHeld = false;
    ULONGLONG m_muteKeyHeldTick = 0;
    // 锁键虚拟码 -> 下标：CAPITAL->0, NUMLOCK->1, SCROLL->2，其他返回 -1
    static int LockKeyIndex(UINT vk);
    // 复位所有「按键已按下」连发抑制标志（捕获模式开始、钩子卸载时调用）
    void ResetHeldInputState();

    static KeyboardHook* s_instance;   // 单例指针，供静态回调使用
};
