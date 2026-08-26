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
//   - 屏蔽系统热键
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
    // 启用/禁用按键重映射
    void SetKeyRemapEnabled(bool enabled);
    // 设置按键重映射表：{源键虚拟码, 目标键虚拟码}
    void SetKeyRemappings(const std::vector<std::pair<int, int>>& remappings);
    // 设置被屏蔽的系统热键列表：{修饰键, 虚拟键}
    void SetDisabledHotkeys(const std::vector<std::pair<UINT, UINT>>& hotkeys);
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
    bool m_keyRemapEnabled = false;    // 是否启用按键重映射
    std::vector<std::pair<int, int>> m_remappings; // 按键重映射表
    std::vector<std::pair<UINT, UINT>> m_disabledHotkeys; // 被屏蔽的系统热键
    bool m_simulatingInput = false;    // 正在模拟输入（用于防止递归拦截）
    bool m_captureMode = false;        // 热键捕获模式（不拦截任何按键）

    static KeyboardHook* s_instance;   // 单例指针，供静态回调使用
};
