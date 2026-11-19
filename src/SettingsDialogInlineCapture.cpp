// ===== 内联热键捕获的吞键钩子 =====
// 老板键/关闭键/自定义热键的内联捕获共用：捕获期间安装临时低级键盘钩子，
// 吞掉所有物理按键（含 Win+R/Win+D、开始菜单等系统快捷键），仅把按键信息
// 以 WM_INLINE_CAPTURE_KEY 投递给捕获窗口，避免捕获操作本身触发系统动作。

#include "SettingsDialogInlineCapture.h"
#include "SettingsDialogSubDialogs.h"
#include "SettingsDialog.h"
#include "FluentTheme.h"
#include "Utils.h"
#include <cstring>

static HHOOK s_inlineCapHook = nullptr;  // 内联捕获吞键钩子
static HWND s_inlineCapWnd = nullptr;    // 接收捕获按键的窗口
static UINT s_inlineCapMods = 0;         // 钩子内自行跟踪的修饰键状态（按键被吞后 GetAsyncKeyState 不更新）
// 逐个物理键记录按下状态：左右 Shift/Ctrl/Alt 共用一个 MOD 位时，
// 松开其中一侧不能把另一侧仍按住的位清掉（R14-A9）
static bool s_inlineCapKeyDown[256] = {};

// 由逐键状态合成当前修饰键组合
static UINT InlineCapRecomputeMods() {
    auto down = [](int vk) { return vk > 0 && vk < 256 && s_inlineCapKeyDown[vk]; };
    UINT mods = 0;
    if (down(VK_SHIFT) || down(VK_LSHIFT) || down(VK_RSHIFT)) mods |= MOD_SHIFT;
    if (down(VK_CONTROL) || down(VK_LCONTROL) || down(VK_RCONTROL)) mods |= MOD_CONTROL;
    if (down(VK_MENU) || down(VK_LMENU) || down(VK_RMENU)) mods |= MOD_ALT;
    if (down(VK_LWIN) || down(VK_RWIN)) mods |= MOD_WIN;
    return mods;
}

// 按键按下/抬起时更新修饰键组合
static void InlineCapUpdateMods(UINT vk, bool down) {
    if (!InlineCapIsModifier((int)vk) || vk >= 256) return;
    s_inlineCapKeyDown[vk] = down;
    s_inlineCapMods = InlineCapRecomputeMods();
}

bool InlineCapIsModifier(int vk) {
    return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_LWIN || vk == VK_RWIN;
}

// 查询当前物理按下的修饰键组合，用于捕获开始时的初始状态。
// 捕获开始前按下的修饰键不会被吞键钩子拦截，其 keydown 不会进入 s_inlineCapMods，
// 因此必须显式查询，否则用户先按住 Ctrl 再点"捕获"时首个组合会被误判为裸键。
static UINT InlineCapQueryMods() {
    // 按具体左右键位记录初始状态；若只记聚合键，松开一侧后无法区分另一侧
    memset(s_inlineCapKeyDown, 0, sizeof(s_inlineCapKeyDown));
    static const int keys[] = {
        VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL,
        VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN
    };
    for (int vk : keys) {
        if (GetAsyncKeyState(vk) & 0x8000) s_inlineCapKeyDown[vk] = true;
    }
    return InlineCapRecomputeMods();
}

static LRESULT CALLBACK InlineCaptureHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && s_inlineCapWnd) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        // 注入按键放行（程序自身的模拟输入不参与捕获）
        if (!(kb->flags & LLKHF_INJECTED) &&
            (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN ||
             wParam == WM_KEYUP || wParam == WM_SYSKEYUP)) {
            bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            InlineCapUpdateMods(kb->vkCode, down);
            // Win+L 是系统锁定快捷键。Win 的 keydown 已被本钩子吞掉，
            // 只放行 L 只会产生一个普通字符，不会触发系统锁定；
            // 因此检测到该组合时直接调用 LockWorkStation() 并吞掉按键（R9-M3）
            if (kb->vkCode == L'L' && (s_inlineCapMods & MOD_WIN)) {
                if (down) LockWorkStation();
                return 1;
            }
            if (down) {
                PostMessageW(s_inlineCapWnd, WM_INLINE_CAPTURE_KEY,
                             (WPARAM)kb->vkCode, (LPARAM)s_inlineCapMods);
            }
            return 1;  // 吞掉物理按键：普通输入与系统快捷键均不会触发
        }
    }
    return CallNextHookEx(s_inlineCapHook, nCode, wParam, lParam);
}

// 开始内联捕获：安装吞键钩子并通知全局键盘钩子放行（幂等，可在两项捕获间切换）
// 返回 false 表示钩子安装失败，调用方不得进入捕获态
bool BeginInlineCapture(HWND hwnd) {
    // 已有其他仍然有效的窗口持有内联捕获：拒绝接管。
    // 直接覆盖 owner 会让原窗口的按键被持续吞掉，却无法通过它自己的
    // "取消/关闭"结束捕获（EndInlineCapture 会因归属不符提前返回）
    if (s_inlineCapWnd && s_inlineCapWnd != hwnd && IsWindow(s_inlineCapWnd)) {
        Fluent::ShowMessageBox(hwnd, L"其他窗口正在捕获热键，请先完成或取消该捕获。", L"提示",
                    MB_OK | MB_ICONWARNING);
        return false;
    }
    const bool alreadyActive = (s_inlineCapHook != nullptr && s_inlineCapWnd == hwnd);
    if (!s_inlineCapHook) {
        // 全新开始捕获：以物理键状态初始化修饰键，避免用户在点击"捕获"前就已按住的
        // 修饰键丢失（其 keydown 未被钩子吞掉，不会进入 s_inlineCapMods）。
        // 两项捕获间切换时钩子仍在，s_inlineCapMods 中的跟踪状态更准确，不再重置
        s_inlineCapMods = InlineCapQueryMods();
        s_inlineCapHook = SetWindowsHookExW(WH_KEYBOARD_LL, InlineCaptureHookProc,
                                            GetModuleHandleW(nullptr), 0);
        if (!s_inlineCapHook) {
            // 安装失败：不进入捕获态，否则界面提示"请按下快捷键"但永远收不到按键
            s_inlineCapMods = 0;
            Fluent::ShowMessageBox(hwnd, L"无法安装键盘钩子，热键捕获已取消。\n请检查是否有安全软件拦截。",
                        L"提示", MB_OK | MB_ICONWARNING);
            return false;
        }
    }
    s_inlineCapWnd = hwnd;
    // 两项内联捕获间切换时钩子仍在：不要重复通知外部进入捕获模式，
    // 否则外部引用计数/抑制状态会叠加，退出捕获一次无法完全恢复
    if (!alreadyActive) {
        g_suppressHotkeys = true;  // 捕获期间抑制热键转发，避免捕获操作触发已注册热键
        if (SettingsDialog::s_captureModeCallback) SettingsDialog::s_captureModeCallback(true);
    }
    return true;
}

bool IsInlineCaptureActive() { return s_inlineCapHook != nullptr; }
HWND InlineCaptureOwner() { return s_inlineCapWnd; }

// 结束内联捕获：仅当钩子归属于 owner 时才卸载
// （捕获过程中若其他对话框接管了钩子，此处不得误拆，否则该对话框的捕获会失效）
void EndInlineCapture(HWND owner) {
    // 钩子已归属其他捕获窗口：交由该窗口收尾
    if (s_inlineCapWnd && s_inlineCapWnd != owner) return;
    // 走到这里 s_inlineCapWnd 只可能是 owner 或 nullptr；
    // 前者说明本次调用确实释放了自己持有的内联捕获
    const bool ownedInline = (s_inlineCapWnd != nullptr);
    if (s_inlineCapHook) {
        UnhookWindowsHookEx(s_inlineCapHook);
        s_inlineCapHook = nullptr;
    }
    s_inlineCapWnd = nullptr;
    s_inlineCapMods = 0;
    memset(s_inlineCapKeyDown, 0, sizeof(s_inlineCapKeyDown));
    // 抑制标志与钩子捕获模式必须归还给当前真正的持有者：
    // ShowCaptureKeyDialogEx 只持有 s_captureKeyHook（不持有 s_inlineCapWnd），
    // 若无条件复位会把它的按键捕获与抑制状态一并破坏
    if (ownedInline || !IsCaptureKeyDialogActive()) {
        g_suppressHotkeys = false;
        if (SettingsDialog::s_captureModeCallback) SettingsDialog::s_captureModeCallback(false);
    }
}
