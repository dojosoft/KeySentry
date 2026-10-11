#include "KeyboardHook.h"
#include "Resource.h"
#include "Utils.h"
#include <algorithm>

KeyboardHook* KeyboardHook::s_instance = nullptr;

// 锁键「已按下」标志的超时兜底时长：若超过该时长仍未收到 keyup（例如 keyup 丢失），
// 视为标志失效并允许重新通知，避免按一次之后再也收不到浮层。
static const ULONGLONG LOCK_KEY_HOLD_TIMEOUT_MS = 3000;

KeyboardHook::KeyboardHook() {
    s_instance = this;
}

KeyboardHook::~KeyboardHook() {
    Uninstall();
    s_instance = nullptr;
}

// 锁键虚拟码 -> 下标（配合 m_lockKeyHeld / m_lockKeyHeldTick 使用）
int KeyboardHook::LockKeyIndex(UINT vk) {
    switch (vk) {
    case VK_CAPITAL: return 0;
    case VK_NUMLOCK: return 1;
    case VK_SCROLL:  return 2;
    default:         return -1;
    }
}

// 复位所有连发抑制标志：捕获模式开始时用户可能正按住某键，
// 钩子卸载后重新安装也应视为「全新状态」，避免沿用陈旧标志而漏发通知
void KeyboardHook::ResetHeldInputState() {
    for (int i = 0; i < 3; i++) {
        m_lockKeyHeld[i] = false;
        m_lockKeyHeldTick[i] = 0;
    }
    m_muteKeyHeld = false;
    m_muteKeyHeldTick = 0;
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
    ResetHeldInputState();
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

void KeyboardHook::SetShowMuteOverlay(bool show) {
    m_showMuteOverlay = show;
}

void KeyboardHook::SetKeyRemapEnabled(bool enabled) {
    m_keyRemapEnabled = enabled;
}

void KeyboardHook::SetKeyRemappings(const std::vector<std::pair<int, int>>& remappings) {
    m_remappings = remappings;
}

void KeyboardHook::SetSimulatingInput(bool sim) {
    m_simulatingInput = sim;
}

void KeyboardHook::SetCaptureMode(bool enabled) {
    m_captureMode = enabled;
    // 进入捕获模式时复位连发标志：捕获期间钩子放行所有按键（不更新标志），
    // 若不复位，退出捕获后可能沿用捕获前的旧标志而漏发一次通知
    if (enabled) ResetHeldInputState();
}

// ============================================================
// KeyboardHook::LowLevelProc - 低级键盘钩子回调
// 处理优先级：
//   1. 模拟输入/重映射/捕获模式下直接放行
//   2. 按键重映射
//   3. NumLock 禁用处理
//   4. Insert 键禁用
//   5. 指定按键屏蔽
//   6. 锁键/静音状态浮层通知
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
                    // 仅在物理按下的第一次 keydown 处理：连发时重复注入会造成
                    // 大量 keybd_event 与浮层通知，拖垮系统
                    int idx = LockKeyIndex(VK_NUMLOCK);
                    ULONGLONG now = GetTickCount64();
                    bool firstPress = !s_instance->m_lockKeyHeld[idx] ||
                                      (now - s_instance->m_lockKeyHeldTick[idx]) > LOCK_KEY_HOLD_TIMEOUT_MS;
                    if (firstPress) {
                        s_instance->m_lockKeyHeld[idx] = true;
                        s_instance->m_lockKeyHeldTick[idx] = now;
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
                } else {
                    // keyup：清除按下标志，下次按下才会再次通知
                    s_instance->m_lockKeyHeld[LockKeyIndex(VK_NUMLOCK)] = false;
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
                // CapsLock / NumLock / ScrollLock 状态变化通知（受锁键浮层开关控制）
                if (s_instance->m_showOverlay) {
                    int idx = LockKeyIndex(kb->vkCode);
                    // NumLock 禁用时已在上面的专用段发送通知，此处跳过避免双重发送
                    bool handledByNumLockBranch =
                        (kb->vkCode == VK_NUMLOCK && s_instance->m_disableNumLock);
                    if (idx >= 0 && !handledByNumLockBranch) {
                        // 仅在物理按下的第一次 keydown 通知：连发时状态取反推算会失真，
                        // 且高频重绘会使浮层闪烁并拖垮任务栏/DWM
                        ULONGLONG now = GetTickCount64();
                        bool firstPress = !s_instance->m_lockKeyHeld[idx] ||
                                          (now - s_instance->m_lockKeyHeldTick[idx]) > LOCK_KEY_HOLD_TIMEOUT_MS;
                        if (firstPress) {
                            s_instance->m_lockKeyHeld[idx] = true;
                            s_instance->m_lockKeyHeldTick[idx] = now;
                            bool oldState = (GetKeyState((int)kb->vkCode) & 0x0001) != 0;
                            bool newState = !oldState;
                            PostMessageW(s_instance->m_notifyWnd, WM_LOCKKEY_NOTIFY,
                                         (WPARAM)kb->vkCode, (LPARAM)(newState ? 1 : 0));
                        }
                    }
                }
                // 静音键状态变化通知（受静音浮层开关控制，与锁键浮层解耦）
                // 仅在物理按下的第一次 keydown 通知：连发时 ShowMute 会反复
                // CoCreateInstance 枚举音频设备并重绘浮层，造成资源浪费与闪烁
                if (s_instance->m_showMuteOverlay && kb->vkCode == VK_VOLUME_MUTE) {
                    ULONGLONG now = GetTickCount64();
                    bool firstPress = !s_instance->m_muteKeyHeld ||
                                      (now - s_instance->m_muteKeyHeldTick) > LOCK_KEY_HOLD_TIMEOUT_MS;
                    if (firstPress) {
                        s_instance->m_muteKeyHeld = true;
                        s_instance->m_muteKeyHeldTick = now;
                        PostMessageW(s_instance->m_notifyWnd, WM_MUTE_NOTIFY, 0, 0);
                    }
                }
            } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                // 锁键抬起：清除按下标志，使下一次按下能再次通知
                int idx = LockKeyIndex(kb->vkCode);
                if (idx >= 0) s_instance->m_lockKeyHeld[idx] = false;
                // 静音键抬起：同样清除标志
                if (kb->vkCode == VK_VOLUME_MUTE) s_instance->m_muteKeyHeld = false;
            }
        }
    }
    return CallNextHookEx(s_instance ? s_instance->m_hook : nullptr, nCode, wParam, lParam);
}
