// ===== 按键捕获对话框 =====
// 用于禁用按键/按键映射等功能的单键与组合键捕获。
// 从 SettingsDialog.cpp 拆出，行为保持不变。

#include "SettingsDialog.h"
#include "SettingsDialogSubDialogs.h"
#include "SettingsDialogInlineCapture.h"
#include "Utils.h"
#include "FluentTheme.h"
#include "Lang.h"

static const wchar_t* CAPTURE_KEY_CLASS = L"KeySentryCaptureKeyDlg";
static const UINT WM_CAPTURE_KEY_MSG = WM_USER + 300;   // 自定义消息：捕获到按键

// 按键捕获对话框数据
struct CaptureKeyData {
    int capturedVK;     // 捕获到的虚拟键码，0 表示取消
    UINT capturedMods;  // 捕获到的修饰键（仅组合键捕获时有意义）
    bool done;          // 对话框完成标志
    bool activated = false; // 是否已真正激活过，避免创建初期的 WA_INACTIVE 误取消
};

static HHOOK s_captureKeyHook = nullptr; // 全局键盘钩子句柄
static HWND s_captureKeyDlg = nullptr;   // 捕获对话框窗口句柄
static UINT s_captureKeyMods = 0;        // 捕获过程中累积的修饰键状态
static bool s_captureKeyAllowMods = false; // 是否允许捕获组合键

// 虚拟键码 -> 修饰键位（MOD_*），非修饰键返回 0
static UINT CaptureModifierBit(int vk) {
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return MOD_SHIFT;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return MOD_CONTROL;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return MOD_ALT;
    case VK_LWIN: case VK_RWIN: return MOD_WIN;
    default: return 0;
    }
}

// 是否存在"只持有 s_captureKeyHook"的独立捕获对话框（禁用键/按键映射的取值对话框）
bool IsCaptureKeyDialogActive() {
    return s_captureKeyHook != nullptr || s_captureKeyDlg != nullptr;
}

// ===== 按键捕获低级键盘钩子过程 =====
// 拦截键盘输入，将非注入的按键事件转发给捕获对话框
static LRESULT CALLBACK CaptureKeyHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && s_captureKeyDlg) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        // 忽略注入的按键（避免模拟按键被捕获）
        if (kb->flags & LLKHF_INJECTED) {
            return CallNextHookEx(s_captureKeyHook, nCode, wParam, lParam);
        }
        bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
        // 跟踪修饰键状态，供组合键捕获使用
        UINT modBit = CaptureModifierBit((int)kb->vkCode);
        if (modBit) {
            if (isDown) s_captureKeyMods |= modBit;
            else if (isUp) s_captureKeyMods &= ~modBit;
            // 组合键捕获模式：修饰键只更新状态并吞掉，等待主键
            if (s_captureKeyAllowMods) return 1;
        }
        // Win+L 是系统锁定快捷键。Win 的 keydown 已被本钩子吞掉，
        // 只放行 L 只会产生一个普通字符，不会触发系统锁定；
        // 因此检测到该组合时直接调用 LockWorkStation() 并吞掉按键（R9-M3）
        if ((int)kb->vkCode == 'L' && (s_captureKeyMods & MOD_WIN)) {
            if (isDown) LockWorkStation();
            return 1;
        }
        if (isDown) {
            PostMessageW(s_captureKeyDlg, WM_CAPTURE_KEY_MSG,
                         (WPARAM)kb->vkCode, (LPARAM)s_captureKeyMods);
        }
        return 1; // 拦截按键，不传递给其他应用
    }
    return CallNextHookEx(s_captureKeyHook, nCode, wParam, lParam);
}

// ===== 按键捕获对话框窗口过程 =====
static LRESULT CALLBACK CaptureKeyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) { EraseWindowBackground(hwnd, wp); return 1; }
    // DPI 处理不依赖 USERDATA，必须在取 data 之前处理
    if (msg == WM_NCDESTROY) {
        CleanupSubDialogDpiState(hwnd);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (msg == WM_DPICHANGED) {
        HandleSubDialogDpiChanged(hwnd, wp, lp, 14, FW_NORMAL);
        return 0;
    }
    auto* data = reinterpret_cast<CaptureKeyData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CAPTURE_KEY_MSG: {
        // 捕获到按键：区分左右修饰键
        if (!data) return DefWindowProcW(hwnd, msg, wp, lp);
        int vk = (int)wp;
        // 区分左右 Ctrl/Shift/Alt 键
        if (vk == VK_CONTROL) {
            vk = (GetKeyState(VK_LCONTROL) & 0x8000) ? VK_LCONTROL : VK_RCONTROL;
        }
        if (vk == VK_SHIFT) {
            vk = (GetKeyState(VK_LSHIFT) & 0x8000) ? VK_LSHIFT : VK_RSHIFT;
        }
        if (vk == VK_MENU) {
            vk = (GetKeyState(VK_LMENU) & 0x8000) ? VK_LMENU : VK_RMENU;
        }
        // 仅无修饰键的 Esc 取消捕获；带修饰键的 Esc 组合应作为按键录入，
        // 否则 Ctrl+Esc/Alt+Esc/Win+Esc 永远无法被捕获（R8-M1）
        if (vk == VK_ESCAPE && s_captureKeyMods == 0) {
            data->capturedVK = 0;
            data->capturedMods = 0;
            data->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        data->capturedVK = vk;
        data->capturedMods = (UINT)lp;
        data->done = true;
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_CLOSE:
        if (data) { data->capturedVK = 0; data->capturedMods = 0; data->done = true; }
        DestroyWindow(hwnd);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            // 捕获窗口失焦（切到其他程序/被其他置顶窗口遮挡）时立即取消：
            // 吞键钩子在所有应用中生效，不取消会让键盘在整个系统中失灵（R8-H4）。
            // 但创建初期的 WA_INACTIVE 不能当作"用户切走"：窗口尚未激活就销毁
            // 会表现为点击"添加按键"后没有任何反应，必须先真正激活过一次。
            // 已捕获完成的窗口在 DestroyWindow 过程中也会收到 WA_INACTIVE，
            // 此时不能把刚捕获到的键码清零。
            if (data && data->activated && !data->done) {
                data->capturedVK = 0;
                data->capturedMods = 0;
                data->done = true;
                DestroyWindow(hwnd);
            }
            return 0;
        }
        if (data) data->activated = true;
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, IsWindowEnabled((HWND)lp) ? Fluent::TextColor() : Fluent::TextDisabledColor());
        return (LRESULT)Fluent::SurfaceBrush();
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ===== 显示按键捕获对话框（通用版） =====
// 创建一个置顶的模态对话框，安装低级键盘钩子捕获用户按键
// title: 对话框标题，line1/line2: 提示文字
// outMods/allowMods: 是否捕获组合键；outMods 返回捕获到的修饰键
// 返回捕获到的虚拟键码，0 表示取消
int ShowCaptureKeyDialogEx(HWND parent, const wchar_t* title, const wchar_t* line1,
                           const wchar_t* line2, UINT* outMods,
                           bool allowMods) {
    if (outMods) *outMods = 0;
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = CaptureKeyWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = CAPTURE_KEY_CLASS;
        wc.hbrBackground = nullptr;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // 注册失败时不得标记成功：否则窗口类始终未注册，
        // CreateWindowExW 会持续返回空且不再重试（ERROR_CLASS_ALREADY_EXISTS 视为成功）
        if (!WindowUtils::RegisterClassOnce(wc)) return 0;
        registered = true;
    }

    RECT rc = { 0, 0, ScaleUi(360), ScaleUi(120) };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int winX = 0, winY = 0;
    WindowUtils::ClampWindowToWorkArea(&winX, &winY, &winW, &winH);

    CaptureKeyData data = {};

    // 初始化修饰键状态：捕获开始时用户可能已经按住 Ctrl/Shift 等
    s_captureKeyMods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) s_captureKeyMods |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) s_captureKeyMods |= MOD_SHIFT;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) s_captureKeyMods |= MOD_ALT;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) s_captureKeyMods |= MOD_WIN;
    s_captureKeyAllowMods = allowMods;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, CAPTURE_KEY_CLASS,
                                 title,
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                                 winX, winY,
                                 winW, winH,
                                 parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return 0;  // 创建失败时直接返回取消，避免对空句柄调用 API
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&data));

    GdiObjectGuard font(CreateUiFont(14, FW_NORMAL));

    HWND lbl1 = CreateWindowExW(0, L"STATIC",
                                  line1,
                                  WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                  10, 18, 320, 30,
                                  hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(lbl1, WM_SETFONT, (WPARAM)font.get(), TRUE);

    HWND lbl2 = CreateWindowExW(0, L"STATIC",
                                  line2,
                                  WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                  10, 48, 320, 30,
                                  hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(lbl2, WM_SETFONT, (WPARAM)font.get(), TRUE);

    // 提示文字按逻辑坐标创建，统一放大到物理坐标
    ScaleChildWindows(hwnd, GetUiScale());
    Fluent::ApplyWindowChrome(hwnd);
    Fluent::DecorateDialog(hwnd);

    s_captureKeyDlg = hwnd;

    // 通知进入捕获模式
    if (SettingsDialog::s_captureModeCallback) SettingsDialog::s_captureModeCallback(true);
    g_suppressHotkeys = true;  // 捕获期间抑制热键转发

    // 安装低级键盘钩子
    s_captureKeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, CaptureKeyHookProc,
                                          GetModuleHandleW(nullptr), 0);

    // 钩子安装失败时无法捕获按键，直接提示并退出，避免模态循环永久等待
    if (!s_captureKeyHook) {
        Fluent::ShowMessageBox(parent, L"键盘钩子安装失败，无法捕获按键。\n请稍后重试或检查安全软件拦截。",
                    L"提示", MB_OK | MB_ICONWARNING);
        s_captureKeyDlg = nullptr;
        if (SettingsDialog::s_captureModeCallback) SettingsDialog::s_captureModeCallback(false);
        g_suppressHotkeys = IsInlineCaptureActive();
        if (IsWindow(hwnd)) DestroyWindow(hwnd);
        return 0;
    }

    ApplyLanguageToWindowTree(hwnd);

    {
        ModalGuard guard(parent);
        ShowWindow(hwnd, SW_SHOW);
        // 必须显式置前并激活：ShowWindow 不保证成为活动窗口，
        // 未激活时键盘钩子虽能收键，但用户看不到明确的输入焦点。
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
        SetFocus(hwnd);
        UpdateWindow(hwnd);

        MSG msg;
        while (!data.done && IsWindow(hwnd)) {
            BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
            if (ret <= 0) { if (ret == 0) PostQuitMessage((int)msg.wParam); break; }
            // 按键捕获期间丢弃 WM_HOTKEY，避免按下的组合同时触发已注册热键
            if (msg.message == WM_HOTKEY) {
                if (!g_suppressHotkeys && g_mainWnd && IsWindow(g_mainWnd))
                    SendMessageW(g_mainWnd, msg.message, msg.wParam, msg.lParam);
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // 卸载键盘钩子
    if (s_captureKeyHook) {
        UnhookWindowsHookEx(s_captureKeyHook);
        s_captureKeyHook = nullptr;
    }
    s_captureKeyDlg = nullptr;

    // 通知退出捕获模式
    if (SettingsDialog::s_captureModeCallback) SettingsDialog::s_captureModeCallback(false);
    // 仅在本对话框之外已无内联捕获时解除抑制（内联捕获可能嵌套于本对话框之下）
    g_suppressHotkeys = IsInlineCaptureActive();

    if (IsWindow(hwnd)) DestroyWindow(hwnd);

    // 组合键捕获：把累积的修饰键带回调用方（取消时为 0）
    if (outMods) *outMods = (data.capturedVK != 0) ? data.capturedMods : 0;
    return data.capturedVK;
}

// ===== 显示按键捕获对话框（按键屏蔽专用） =====
int ShowCaptureKeyDialog(HWND parent) {
    return ShowCaptureKeyDialogEx(parent,
        L"添加屏蔽按键",
        L"请按下需要屏蔽的按键...",
        L"(Fn键无法捕获)");
}
