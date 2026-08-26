#include "KeyboardHook.h"
#include "Resource.h"
#include "Utils.h"
#include <algorithm>

KeyboardHook* KeyboardHook::s_instance = nullptr;

KeyboardHook::KeyboardHook() {
    s_instance = this;
}

KeyboardHook::~KeyboardHook() {
    Uninstall();
    s_instance = nullptr;
}

// 安装 WH_KEYBOARD_LL 低级键盘钩子
bool KeyboardHook::Install(HWND notifyWnd) {
    if (m_hook) return true;
    m_notifyWnd = notifyWnd;
    m_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelProc,
                                GetModuleHandleW(nullptr), 0);
    return m_hook != nullptr;
}

// 卸载键盘钩子
void KeyboardHook::Uninstall() {
    if (m_hook) {
        UnhookWindowsHookEx(m_hook);
        m_hook = nullptr;
    }
}

void KeyboardHook::SetDisabledKeys(const std::vector<int>& codes) {
    m_disabledKeys = codes;
}

void KeyboardHook::SetDisableSpecifiedKeysEnabled(bool enabled) {
    m_disableSpecifiedKeysEnabled = enabled;
}

void KeyboardHook::SetDisableNumLock(bool disable) {
    m_disableNumLock = disable;
}

void KeyboardHook::SetForceInsertMode(bool force) {
    m_forceInsert = force;
}

void KeyboardHook::SetShowOverlay(bool show) {
    m_showOverlay = show;
}

void KeyboardHook::SetKeyRemapEnabled(bool enabled) {
    m_keyRemapEnabled = enabled;
}

void KeyboardHook::SetKeyRemappings(const std::vector<std::pair<int, int>>& remappings) {
    m_remappings = remappings;
}

void KeyboardHook::SetDisabledHotkeys(const std::vector<std::pair<UINT, UINT>>& hotkeys) {
    m_disabledHotkeys = hotkeys;
}

void KeyboardHook::SetSimulatingInput(bool sim) {
    m_simulatingInput = sim;
}

void KeyboardHook::SetCaptureMode(bool enabled) {
    m_captureMode = enabled;
}

// ============================================================
// KeyboardHook::LowLevelProc - 低级键盘钩子回调
// 处理优先级：
//   1. 模拟输入/重映射/捕获模式下直接放行
//   2. 热键屏蔽检查
//   3. 按键重映射
//   4. NumLock 禁用处理
//   5. Insert 键禁用
//   6. 指定按键屏蔽
//   7. 锁键/静音状态浮层通知
// ============================================================
LRESULT CALLBACK KeyboardHook::LowLevelProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && s_instance) {
        // 正在模拟输入（如 BossKey 发送媒体暂停），放行避免递归拦截
        if (s_instance->m_simulatingInput) {
            return CallNextHookEx(s_instance->m_hook, nCode, wParam, lParam);
        }

        // 捕获模式下放行所有按键（用于设置对话框中的热键捕获）
        if (s_instance->m_captureMode) {
            return CallNextHookEx(s_instance->m_hook, nCode, wParam, lParam);
        }

        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN ||
            wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {

            // --- 热键屏蔽 ---
            // 仅在非注入按键按下时检查，避免拦截自己模拟的按键
            if (!s_instance->m_disabledHotkeys.empty() &&
                (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) &&
                !(kb->flags & LLKHF_INJECTED)) {
                int vk = (int)kb->vkCode;
                // 跳过修饰键本身，只检查非修饰键
                if (vk != VK_CONTROL && vk != VK_LCONTROL && vk != VK_RCONTROL &&
                    vk != VK_SHIFT && vk != VK_LSHIFT && vk != VK_RSHIFT &&
                    vk != VK_MENU && vk != VK_LMENU && vk != VK_RMENU &&
                    vk != VK_LWIN && vk != VK_RWIN) {
                    // 获取当前按下的修饰键组合
                    UINT mod = 0;
                    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mod |= MOD_CONTROL;
                    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mod |= MOD_SHIFT;
                    if (kb->flags & LLKHF_ALTDOWN) mod |= MOD_ALT;
                    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) ||
                        (GetAsyncKeyState(VK_RWIN) & 0x8000)) mod |= MOD_WIN;
                    // 检查是否在屏蔽列表中
                    for (const auto& hk : s_instance->m_disabledHotkeys) {
                        if (hk.first == mod && hk.second == (UINT)vk) {
                            return 1; // 屏蔽此热键
                        }
                    }
                }
            }

            // --- 按键重映射 ---
            // 跳过注入按键，防止重映射的注入按键触发二次重映射（无限递归）
            if (s_instance->m_keyRemapEnabled && !(kb->flags & LLKHF_INJECTED)) {
                for (const auto& remap : s_instance->m_remappings) {
                    if ((int)kb->vkCode == remap.first) {
                        // 模拟目标按键
                        INPUT input = {};
                        input.type = INPUT_KEYBOARD;
                        input.ki.wVk = (WORD)remap.second;
                        input.ki.wScan = (WORD)MapVirtualKeyW(remap.second, MAPVK_VK_TO_VSC);
                        input.ki.dwFlags = 0;
                        if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
                            input.ki.dwFlags = KEYEVENTF_KEYUP;
                        if (NeedsExtendedKeyFlag(remap.second))
                            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
                        // 发送重映射按键（注入的，带 LLKHF_INJECTED 标志）
                        // 上方的 LLKHF_INJECTED 检查确保不会对注入按键再次重映射
                        SendInput(1, &input, sizeof(INPUT));
                        return 1; // 吞掉原始按键
                    }
                }
            }

            // --- NumLock 禁用 ---
            // 屏蔽物理 NumLock 按键（return 1），注入按键放行
            // 关键：物理按键被屏蔽后，Windows 状态不变（仍为 ON），
            // 但键盘固件已在物理按键时切换为 OFF，导致小键盘发出方向键扫描码。
            // 解决：发送两次 keybd_event 切换（ON→OFF→ON），使 Windows 重新
            // 发送键盘指示器状态给键盘固件，将固件恢复为数字输入模式。
            if (s_instance->m_disableNumLock && kb->vkCode == VK_NUMLOCK) {
                if (kb->flags & LLKHF_INJECTED) {
                    return CallNextHookEx(s_instance->m_hook, nCode, wParam, lParam);
                }
                if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                    // 发送两次切换：Windows ON→OFF→ON，键盘固件收到 ON 指示
                    BYTE scanCode = (BYTE)MapVirtualKeyW(VK_NUMLOCK, MAPVK_VK_TO_VSC);
                    keybd_event(VK_NUMLOCK, scanCode, KEYEVENTF_EXTENDEDKEY, 0);
                    keybd_event(VK_NUMLOCK, scanCode, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
                    keybd_event(VK_NUMLOCK, scanCode, KEYEVENTF_EXTENDEDKEY, 0);
                    keybd_event(VK_NUMLOCK, scanCode, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
                    // 显示 NumLock 已锁定的浮层提示（lp=3 表示已锁定且开启）
                    if (s_instance->m_showOverlay) {
                        PostMessageW(s_instance->m_notifyWnd, WM_LOCKKEY_NOTIFY,
                                     (WPARAM)VK_NUMLOCK, 3);
                    }
                }
                return 1; // 屏蔽物理按键
            }

            // --- Insert 键禁用（强制 Insert 模式）---
            // 跳过注入按键，防止重映射到 Insert 的按键被错误屏蔽
            if (s_instance->m_forceInsert && kb->vkCode == VK_INSERT && !(kb->flags & LLKHF_INJECTED)) {
                return 1;
            }

            // --- 指定按键屏蔽 ---
            // 跳过注入按键，防止重映射到被禁用按键的注入按键被错误屏蔽
            if (s_instance->m_disableSpecifiedKeysEnabled && !(kb->flags & LLKHF_INJECTED)) {
                for (int code : s_instance->m_disabledKeys) {
                    if ((int)kb->vkCode == code) return 1;
                }
            }

            // --- 锁键/静音状态浮层通知 ---
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                if (s_instance->m_showOverlay) {
                    // CapsLock / NumLock / ScrollLock 状态变化通知
                    if (kb->vkCode == VK_CAPITAL || kb->vkCode == VK_NUMLOCK || kb->vkCode == VK_SCROLL) {
                        // NumLock 禁用时，已在上面的专用段发送了通知，此处跳过避免双重发送
                        if (kb->vkCode == VK_NUMLOCK && s_instance->m_disableNumLock) {
                            // 已由 NumLock 专用段处理
                        } else {
                            bool oldState = (GetKeyState((int)kb->vkCode) & 0x0001) != 0;
                            bool newState = !oldState;
                            PostMessageW(s_instance->m_notifyWnd, WM_LOCKKEY_NOTIFY,
                                         (WPARAM)kb->vkCode, (LPARAM)(newState ? 1 : 0));
                        }
                    }
                    // 静音键状态变化通知
                    else if (kb->vkCode == VK_VOLUME_MUTE) {
                        PostMessageW(s_instance->m_notifyWnd, WM_MUTE_NOTIFY, 0, 0);
                    }
                }
            }
        }
    }
    return CallNextHookEx(s_instance ? s_instance->m_hook : nullptr, nCode, wParam, lParam);
}
