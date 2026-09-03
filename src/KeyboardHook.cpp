#include "KeyboardHook.h"
#include "Resource.h"
#include "Utils.h"
#include <algorithm>

KeyboardHook* KeyboardHook::s_instance = nullptr;

// ============================================================
// 辅助函数
// ============================================================

// 判断虚拟键是否为修饰键
static bool IsModifierVk(UINT vk) {
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_LWIN || vk == VK_RWIN;
}

// 获取当前按下的修饰键组合（用于组合键匹配与热键屏蔽）
static UINT GetCurrentModifiers(const KBDLLHOOKSTRUCT* kb) {
    UINT mod = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mod |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mod |= MOD_SHIFT;
    if (kb && (kb->flags & LLKHF_ALTDOWN)) mod |= MOD_ALT;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) mod |= MOD_WIN;
    return mod;
}

// 构建一个键盘 INPUT 项
static void MakeKeyInput(INPUT& in, WORD vk, bool up) {
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    if (NeedsExtendedKeyFlag(vk)) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
}

// 向注入序列追加"抬起源修饰键"：精确检测左/右修饰键的按住状态
// （WIN/Ctrl/Shift/Alt 均区分左右，避免注入通用键导致抬起不完全）
static void PushSourceModUps(UINT mod, INPUT* inputs, int& n) {
    auto push = [&](WORD vk) { MakeKeyInput(inputs[n], vk, true); n++; };
    if (mod & MOD_CONTROL) {
        if (GetAsyncKeyState(VK_LCONTROL) & 0x8000) push(VK_LCONTROL);
        if (GetAsyncKeyState(VK_RCONTROL) & 0x8000) push(VK_RCONTROL);
    }
    if (mod & MOD_SHIFT) {
        if (GetAsyncKeyState(VK_LSHIFT) & 0x8000) push(VK_LSHIFT);
        if (GetAsyncKeyState(VK_RSHIFT) & 0x8000) push(VK_RSHIFT);
    }
    if (mod & MOD_ALT) {
        if (GetAsyncKeyState(VK_LMENU) & 0x8000) push(VK_LMENU);
        if (GetAsyncKeyState(VK_RMENU) & 0x8000) push(VK_RMENU);
    }
    if (mod & MOD_WIN) {
        if (GetAsyncKeyState(VK_LWIN) & 0x8000) push(VK_LWIN);
        if (GetAsyncKeyState(VK_RWIN) & 0x8000) push(VK_RWIN);
    }
}

// 向注入序列追加"按下目标修饰键"（用通用键码，GetAsyncKeyState 均可检测到）
static void PushTargetModDowns(UINT mod, INPUT* inputs, int& n) {
    if (mod & MOD_CONTROL) { MakeKeyInput(inputs[n], VK_CONTROL, false); n++; }
    if (mod & MOD_SHIFT) { MakeKeyInput(inputs[n], VK_SHIFT, false); n++; }
    if (mod & MOD_ALT) { MakeKeyInput(inputs[n], VK_MENU, false); n++; }
    if (mod & MOD_WIN) { MakeKeyInput(inputs[n], VK_LWIN, false); n++; }
}

// 向注入序列追加"抬起目标修饰键"
static void PushTargetModUps(UINT mod, INPUT* inputs, int& n) {
    if (mod & MOD_CONTROL) { MakeKeyInput(inputs[n], VK_CONTROL, true); n++; }
    if (mod & MOD_SHIFT) { MakeKeyInput(inputs[n], VK_SHIFT, true); n++; }
    if (mod & MOD_ALT) { MakeKeyInput(inputs[n], VK_MENU, true); n++; }
    if (mod & MOD_WIN) { MakeKeyInput(inputs[n], VK_LWIN, true); n++; }
}

// ============================================================
// KeyboardHook 公共接口
// ============================================================

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

// 卸载低级键盘钩子
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

void KeyboardHook::SetKeyRemappings(const std::vector<KeyMapping>& remappings) {
    m_remappings = remappings;
    // 映射表变化后，进行中的会话已无意义，清空避免抬起时注入错误序列
    m_activeMappings.clear();
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

void KeyboardHook::SetListenMode(bool enabled) {
    m_listenMode = enabled;
    if (!enabled) {
        // 退出侦听时无需清理（侦听不拦截按键，无会话状态）
    }
}

// ============================================================
// KeyboardHook::PerformMapping - 组合键映射执行
// 按下：临时抬起源修饰键 -> 按下目标修饰键 -> 按下目标主键 -> 恢复源修饰键
// 抬起：临时抬起源修饰键 -> 抬起目标主键 -> 抬起目标修饰键 -> 恢复源修饰键
// 返回 true 表示事件已处理（调用方吞掉原事件）
// ============================================================
bool KeyboardHook::PerformMapping(UINT vk, UINT curMod, bool isDown) {
    KeyMapping m;
    if (!isDown) {
        // 抬起事件：从会话表取出按下时匹配的映射，保证 up 序列与 down 对称
        auto it = m_activeMappings.find(vk);
        if (it == m_activeMappings.end()) return false;
        m = it->second;
        m_activeMappings.erase(it);
    } else {
        bool found = false;
        for (const auto& r : m_remappings) {
            if (r.srcVk == vk && r.srcMod == curMod) { m = r; found = true; break; }
        }
        if (!found) return false;
        // 记录会话（自动重复按下时覆盖，无碍）
        m_activeMappings[vk] = m;
    }

    INPUT inputs[12] = {};
    int n = 0;
    // 1. 临时抬起源修饰键（防止系统把注入的目标键识别为 源修饰键+目标键 组合）
    PushSourceModUps(m.srcMod, inputs, n);
    if (isDown) {
        // 2. 按下目标修饰键
        PushTargetModDowns(m.dstMod, inputs, n);
        // 3. 按下目标主键
        MakeKeyInput(inputs[n], (WORD)m.dstVk, false); n++;
    } else {
        // 2. 抬起目标主键
        MakeKeyInput(inputs[n], (WORD)m.dstVk, true); n++;
        // 3. 抬起目标修饰键
        PushTargetModUps(m.dstMod, inputs, n);
    }
    // 4. 恢复源修饰键按住状态（用户物理按住的修饰键保持按住，不干扰后续组合）
    PushTargetModDowns(m.srcMod, inputs, n);

    if (n > 0) SendInput(n, inputs, sizeof(INPUT));
    return true;
}

// ============================================================
// KeyboardHook::LowLevelProc - 低级键盘钩子回调
// 处理优先级：
//   1. 模拟输入/捕获模式下直接放行
//   2. 热键侦听模式（仅记录，不拦截）
//   3. 热键屏蔽检查
//   4. 按键重映射（支持组合键）
//   5. NumLock 禁用处理
//   6. Insert 键禁用
//   7. 指定按键屏蔽
//   8. 锁键/静音状态浮层通知
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

            // --- 热键侦听模式 ---
            // 不拦截任何按键（用户需要真实触发目标软件的功能），
            // 仅记录非注入、非修饰键的按下组合，上报主窗口识别归属进程
            if (s_instance->m_listenMode) {
                if ((wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) &&
                    !(kb->flags & LLKHF_INJECTED) && !IsModifierVk(kb->vkCode)) {
                    UINT mod = GetCurrentModifiers(kb);
                    if (s_instance->m_notifyWnd && IsWindow(s_instance->m_notifyWnd)) {
                        PostMessageW(s_instance->m_notifyWnd, WM_LISTENED_KEY,
                                     (WPARAM)mod, (LPARAM)kb->vkCode);
                    }
                }
                return CallNextHookEx(s_instance->m_hook, nCode, wParam, lParam);
            }

            // --- 热键屏蔽（延迟处理，避免钩子回调阻塞）---
            // 仅在非注入按键按下时检查，避免拦截自己模拟的按键
            if (!s_instance->m_disabledHotkeys.empty() &&
                (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) &&
                !(kb->flags & LLKHF_INJECTED)) {
                int vk = (int)kb->vkCode;
                // 跳过修饰键本身，只检查非修饰键
                if (!IsModifierVk((UINT)vk)) {
                    UINT mod = GetCurrentModifiers(kb);
                    // 检查是否在屏蔽列表中
                    for (const auto& hk : s_instance->m_disabledHotkeys) {
                        if (hk.first == mod && hk.second == (UINT)vk) {
                            // 延迟到主窗口处理，不直接 return 1（避免钩子超时）
                            s_instance->ProcessBlockedHotkey(mod, (UINT)vk);
                            return 1; // 吞掉原始按键，但处理延迟执行
                        }
                    }
                }
            }

            // --- 按键重映射（支持组合键）---
            // 跳过注入按键，防止重映射的注入按键触发二次重映射（无限递归）
            if (s_instance->m_keyRemapEnabled && !(kb->flags & LLKHF_INJECTED)) {
                UINT vk = kb->vkCode;
                bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
                if (!IsModifierVk(vk)) {
                    UINT curMod = isDown ? GetCurrentModifiers(kb) : 0;
                    if (s_instance->PerformMapping(vk, curMod, isDown)) {
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

// ============================================================
// ProcessBlockedHotkey - 延迟处理被屏蔽的热键
// 从钩子回调 PostMessage 到主窗口执行，避免钩子回调阻塞
// ============================================================
void KeyboardHook::ProcessBlockedHotkey(UINT mod, UINT vk) {
    // 向主窗口发送自定义消息，主窗口收到后记录日志或显示提示
    // 实际屏蔽已在钩子中 return 1 完成，这里仅做通知
    if (m_notifyWnd && IsWindow(m_notifyWnd)) {
        PostMessageW(m_notifyWnd, WM_HOTKEY_BLOCKED, (WPARAM)mod, (LPARAM)vk);
    }
}
