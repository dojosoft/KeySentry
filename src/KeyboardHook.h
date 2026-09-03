#pragma once
#include <Windows.h>
#include <vector>
#include <utility>
#include <unordered_map>
#include "Config.h"

// ============================================================
// KeyboardHook: 低级键盘钩子管理器
// 通过 WH_KEYBOARD_LL 钩子拦截全局键盘事件，实现以下功能：
//   - 屏蔽指定按键
//   - 禁用 NumLock 键
//   - 强制 Insert 模式（屏蔽 Insert 键）
//   - 按键重映射（含组合键，如 WIN+6=F6 / F6=WIN+6）
//   - 屏蔽系统热键
//   - 锁键状态浮层通知
//   - 静音状态浮层通知
//   - 热键侦听模式（记录实际按下的组合键，用于发现应用内热键）
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
    // 设置按键重映射表（支持组合键映射）
    void SetKeyRemappings(const std::vector<KeyMapping>& remappings);
    // 设置被屏蔽的系统热键列表：{修饰键, 虚拟键}
    void SetDisabledHotkeys(const std::vector<std::pair<UINT, UINT>>& hotkeys);
    // 标记当前正在模拟输入（避免钩子递归拦截自己发送的按键）
    void SetSimulatingInput(bool sim);
    // 启用/禁用热键捕获模式（捕获模式下不拦截任何按键）
    void SetCaptureMode(bool enabled);

    // 启用/禁用热键侦听模式（侦听期间不拦截任何按键，仅记录组合键并上报主窗口）
    void SetListenMode(bool enabled);
    bool IsListenMode() const { return m_listenMode; }

private:
    // 低级键盘钩子回调函数
    static LRESULT CALLBACK LowLevelProc(int nCode, WPARAM wParam, LPARAM lParam);

    // 延迟处理热键屏蔽（从钩子回调 PostMessage 到主窗口执行）
    void ProcessBlockedHotkey(UINT mod, UINT vk);

    // 执行组合键映射：按下时注入目标序列并记录会话，抬起时注入对应释放序列
    // 返回 true 表示该事件已被映射处理（调用方应吞掉原事件）
    bool PerformMapping(UINT vk, UINT curMod, bool isDown);

    HHOOK m_hook = nullptr;            // 钩子句柄
    HWND m_notifyWnd = nullptr;        // 通知窗口句柄
    std::vector<int> m_disabledKeys;   // 被禁用的按键虚拟键列表
    bool m_disableSpecifiedKeysEnabled = false; // 是否启用指定按键屏蔽
    bool m_disableNumLock = false;     // 是否禁用 NumLock 键
    bool m_forceInsert = false;        // 是否强制 Insert 模式
    bool m_showOverlay = true;         // 是否显示锁键状态浮层
    bool m_keyRemapEnabled = false;    // 是否启用按键重映射
    std::vector<KeyMapping> m_remappings; // 按键重映射表（支持组合键）
    // 进行中的映射会话：物理键按下且已被映射 -> 该映射（抬起时需注入对应释放序列）
    std::unordered_map<UINT, KeyMapping> m_activeMappings;
    std::vector<std::pair<UINT, UINT>> m_disabledHotkeys; // 被屏蔽的系统热键
    bool m_simulatingInput = false;    // 正在模拟输入（用于防止递归拦截）
    bool m_captureMode = false;        // 热键捕获模式（不拦截任何按键）
    bool m_listenMode = false;         // 热键侦听模式（仅记录不拦截）

    static KeyboardHook* s_instance;   // 单例指针，供静态回调使用
};
