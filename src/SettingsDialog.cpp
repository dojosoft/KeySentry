// ===== SettingsDialog.cpp - 设置对话框实现 =====
// 包含键客 KeySentry 设置对话框的所有界面创建、事件处理和配置管理逻辑。
// 对话框包含六个选项卡：常规、按键禁用、按键映射、热键管理、窗口隐藏、窗口置顶。

#include "SettingsDialog.h"
#include "Resource.h"
#include "Utils.h"
#include "WinDGuard.h"
#include <algorithm>
#include <TlHelp32.h>
#include <ShlObj.h>
#include <Commdlg.h>

// 外部函数声明
extern void ShowAboutDialog(HWND parent);   // 显示关于对话框
extern void SetSettingsWnd(HWND wnd);       // 设置设置窗口句柄到全局，供主窗口消息处理使用

// 读取控件文本到 std::wstring：不使用固定长度缓冲，避免长路径/长命令行被截断
static std::wstring ReadWindowText(HWND hwnd) {
    if (!hwnd) return L"";
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return L"";
    std::wstring text((size_t)len + 1, L'\0');
    GetWindowTextW(hwnd, &text[0], len + 1);
    text.resize(wcslen(text.c_str()));
    return text;
}

// ===== 静态回调指针初始化 =====
void (*SettingsDialog::s_applyCallback)() = nullptr;
void (*SettingsDialog::s_captureModeCallback)(bool) = nullptr;

// ===== 设置按键捕获模式回调 =====
// 在进入/退出按键捕获模式时通知外部模块（如钩子模块暂停/恢复）
void SettingsDialog::SetCaptureModeCallback(void (*callback)(bool)) {
    s_captureModeCallback = callback;
}

// ===== 设置应用回调 =====
// 在配置被应用后调用，通知外部模块重新加载配置
void SettingsDialog::SetApplyCallback(void (*callback)()) {
    s_applyCallback = callback;
}

// ===== 内联热键捕获的吞键钩子 =====
// 老板键/关闭键/自定义热键的内联捕获共用：捕获期间安装临时低级键盘钩子，
// 吞掉所有物理按键（含 Win+R/Win+D、开始菜单等系统快捷键），仅把按键信息
// 以 WM_INLINE_CAPTURE_KEY 投递给捕获窗口，避免捕获操作本身触发系统动作
static const UINT WM_INLINE_CAPTURE_KEY = WM_USER + 302;  // wParam=虚拟键码，lParam=修饰键组合(MOD_*)
static HHOOK s_inlineCapHook = nullptr;  // 内联捕获吞键钩子
static HWND s_inlineCapWnd = nullptr;    // 接收捕获按键的窗口
static UINT s_inlineCapMods = 0;         // 钩子内自行跟踪的修饰键状态（按键被吞后 GetAsyncKeyState 不更新）

// 按键按下/抬起时更新修饰键组合
static void InlineCapUpdateMods(UINT vk, bool down) {
    UINT bit = 0;
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) bit = MOD_SHIFT;
    else if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) bit = MOD_CONTROL;
    else if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) bit = MOD_ALT;
    else if (vk == VK_LWIN || vk == VK_RWIN) bit = MOD_WIN;
    if (bit) {
        if (down) s_inlineCapMods |= bit;
        else s_inlineCapMods &= ~bit;
    }
}

// 判断虚拟键码是否为纯修饰键（捕获时需继续等待主键）
static bool InlineCapIsModifier(int vk) {
    return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_LWIN || vk == VK_RWIN;
}

// 查询当前物理按下的修饰键组合，用于捕获开始时的初始状态。
// 捕获开始前按下的修饰键不会被吞键钩子拦截，其 keydown 不会进入 s_inlineCapMods，
// 因此必须显式查询，否则用户先按住 Ctrl 再点"捕获"时首个组合会被误判为裸键。
// 捕获过程中按下的修饰键由钩子自行跟踪（GetAsyncKeyState 对被吞按键不可靠）。
static UINT InlineCapQueryMods() {
    UINT mods = 0;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) mods |= MOD_WIN;
    return mods;
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
static bool BeginInlineCapture(HWND hwnd) {
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
            MessageBoxW(hwnd, L"无法安装键盘钩子，热键捕获已取消。\n请检查是否有安全软件拦截。",
                        L"提示", MB_OK | MB_ICONWARNING);
            return false;
        }
    }
    s_inlineCapWnd = hwnd;
    g_suppressHotkeys = true;  // 捕获期间抑制热键转发，避免捕获操作触发已注册热键
    if (SettingsDialog::s_captureModeCallback) SettingsDialog::s_captureModeCallback(true);
    return true;
}

// 前置声明：是否存在"只持有 s_captureKeyHook"的独立捕获对话框（定义见文件后部）
// EndInlineCapture 需要据此判断抑制标志是否由自己负责复位
static bool IsCaptureKeyDialogActive();

// 结束内联捕获：仅当钩子归属于 owner 时才卸载
// （捕获过程中若其他对话框接管了钩子，此处不得误拆，否则该对话框的捕获会失效）
static void EndInlineCapture(HWND owner) {
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
    // 抑制标志与钩子捕获模式必须归还给当前真正的持有者：
    // ShowCaptureKeyDialogEx 只持有 s_captureKeyHook（不持有 s_inlineCapWnd），
    // 若无条件复位会把它的按键捕获与抑制状态一并破坏
    if (ownedInline || !IsCaptureKeyDialogActive()) {
        g_suppressHotkeys = false;
        if (SettingsDialog::s_captureModeCallback) SettingsDialog::s_captureModeCallback(false);
    }
}

// ===== 对话框布局常量 =====
static const int CLIENT_W = 800;       // 客户区宽度
static const int CLIENT_H = 680;       // 客户区高度
static const int TAB_X = 10;           // 选项卡左边距
static const int TAB_Y = 10;           // 选项卡上边距
static const int TAB_W = CLIENT_W - 20;// 选项卡宽度
static const int TAB_H = CLIENT_H - 55;// 选项卡高度（留出底部按钮空间）
static const int BTN_Y = CLIENT_H - 42;// 底部按钮行 Y 坐标
static const int BTN_W = 90;           // 按钮宽度
static const int BTN_H = 32;           // 按钮高度

// ===== 对话框字体管理 =====
static HFONT s_dlgFont = nullptr;      // 对话框主字体（14号）
static HFONT s_smallFont = nullptr;    // 小号字体（11号，用于说明文字）

// 获取或创建对话框主字体
static HFONT GetDlgFont() {
    if (!s_dlgFont) {
        s_dlgFont = CreateUiFont(14, FW_NORMAL);
    }
    return s_dlgFont;
}

// 获取或创建小号字体
static HFONT GetSmallFont() {
    if (!s_smallFont) {
        s_smallFont = CreateUiFont(11, FW_NORMAL);
    }
    return s_smallFont;
}

// 清理字体资源，在对话框关闭时调用
static void CleanupDlgFont() {
    if (s_dlgFont) {
        DeleteObject(s_dlgFont);
        s_dlgFont = nullptr;
    }
    if (s_smallFont) {
        DeleteObject(s_smallFont);
        s_smallFont = nullptr;
    }
}

// 设置控件字体
static void SetControlFont(HWND hwnd, HFONT font) {
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
}

// ===== 清理选项卡页面控件 =====
// 销毁除选项卡控件和底部按钮之外的所有子控件
static void CleanupTabPageControls(HWND hwnd) {
    HWND child = GetWindow(hwnd, GW_CHILD);
    while (child) {
        HWND next = GetWindow(child, GW_HWNDNEXT);
        LONG_PTR id = GetWindowLongPtrW(child, GWLP_ID);
        // 保留选项卡、确定/取消/应用/关于按钮，销毁其余控件
        if (id != IDC_TAB_MAIN && id != IDOK && id != IDCANCEL && id != IDAPPLY && id != IDC_BTN_ABOUT) {
            DestroyWindow(child);
        }
        child = next;
    }
}

// ===== 获取选项卡页面内容区域 =====
// 计算选项卡内部可用于放置控件的矩形区域。
// 选项卡控件本身按 DPI 缩放为物理坐标，这里先还原为逻辑坐标，
// 使页面控件仍按逻辑尺寸创建，再由 ScalePageControls 统一放大。
static void GetPageArea(HWND tabCtrl, HWND parent, int* px, int* py, int* pw, int* ph) {
    RECT rc;
    GetClientRect(tabCtrl, &rc);
    TabCtrl_AdjustRect(tabCtrl, FALSE, &rc);   // 将选项卡外框转换为内部显示区域
    MapWindowPoints(tabCtrl, parent, (POINT*)&rc, 2); // 坐标转换到父窗口
    float s = GetUiScale();
    int left = (int)(rc.left / s + 0.5f);
    int top = (int)(rc.top / s + 0.5f);
    int right = (int)(rc.right / s + 0.5f);
    int bottom = (int)(rc.bottom / s + 0.5f);
    *px = left + 10;
    *py = top + 8;
    *pw = right - left - 20;
    *ph = bottom - top - 16;
}

// ===== 缩放当前选项卡页面控件 =====
// 页面控件仍按逻辑坐标创建，构建完成后一次性放大到物理坐标；
// 选项卡与底部按钮已按物理坐标创建，必须跳过，否则会被二次放大。
static void ScalePageControls(HWND hwnd) {
    static const LONG_PTR reserved[] = { IDC_TAB_MAIN, IDOK, IDCANCEL, IDAPPLY, IDC_BTN_ABOUT };
    ScaleChildWindows(hwnd, GetUiScale(), reserved, (int)(sizeof(reserved) / sizeof(reserved[0])));
}

// ===== 选项卡子类化过程 =====
// 自定义绘制选项卡，在选中标签下方填充一条与页面背景同色的矩形，
// 消除选项卡与页面之间的视觉间隙
static LRESULT CALLBACK TabSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                         UINT_PTR /*uid*/, DWORD_PTR /*data*/) {
    if (msg == WM_PAINT) {
        LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        int sel = TabCtrl_GetCurSel(hwnd);
        if (sel >= 0) {
            RECT itemRect;
            TabCtrl_GetItemRect(hwnd, sel, &itemRect); // 获取选中标签的矩形
            RECT displayRect;
            GetClientRect(hwnd, &displayRect);
            TabCtrl_AdjustRect(hwnd, FALSE, &displayRect); // 获取页面显示区域
            HDC hdc = GetDC(hwnd);
            RECT fillRect;
            fillRect.left = itemRect.left + 2;
            fillRect.right = itemRect.right - 2;
            fillRect.top = itemRect.bottom - 1;    // 从标签底部开始
            fillRect.bottom = displayRect.top + 2;  // 到页面区域顶部
            FillRect(hdc, &fillRect, (HBRUSH)(COLOR_BTNFACE + 1)); // 用按钮面色填充
            ReleaseDC(hwnd, hdc);
        }
        return result;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ===== 设置对话框主窗口过程（前向声明） =====
static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// ===== 窗口类名常量 =====
static const wchar_t* SETTINGS_CLASS = L"KeySentrySettingsDlg";

// ===== 虚拟键盘选择器相关定义 =====
// 用于按键映射功能中选取目标按键的可视化键盘界面

// 键盘按键定义：虚拟键码、标签文字、位置和尺寸
struct KbKey {
    int vk;                 // 虚拟键码
    const wchar_t* label;   // 按键上显示的标签
    int x, y, w, h;        // 位置和尺寸（相对坐标）
};

// 标准键盘按键布局数据
static const KbKey KB_KEYS[] = {
    {VK_ESCAPE, L"Esc", 0, 0, 36, 28},
    {VK_F1, L"F1", 52, 0, 32, 28}, {VK_F2, L"F2", 86, 0, 32, 28},
    {VK_F3, L"F3", 120, 0, 32, 28}, {VK_F4, L"F4", 154, 0, 32, 28},
    {VK_F5, L"F5", 196, 0, 32, 28}, {VK_F6, L"F6", 230, 0, 32, 28},
    {VK_F7, L"F7", 264, 0, 32, 28}, {VK_F8, L"F8", 298, 0, 32, 28},
    {VK_F9, L"F9", 340, 0, 32, 28}, {VK_F10, L"F10", 374, 0, 32, 28},
    {VK_F11, L"F11", 408, 0, 32, 28}, {VK_F12, L"F12", 442, 0, 32, 28},
    {VK_SNAPSHOT, L"PrtSc", 486, 0, 38, 28}, {VK_SCROLL, L"ScrLk", 526, 0, 38, 28},
    {VK_PAUSE, L"Pause", 566, 0, 38, 28},

    {VK_OEM_3, L"`", 0, 32, 28, 28}, {0x30, L"1", 30, 32, 28, 28}, {0x31, L"2", 60, 32, 28, 28},
    {0x32, L"3", 90, 32, 28, 28}, {0x33, L"4", 120, 32, 28, 28}, {0x34, L"5", 150, 32, 28, 28},
    {0x35, L"6", 180, 32, 28, 28}, {0x36, L"7", 210, 32, 28, 28}, {0x37, L"8", 240, 32, 28, 28},
    {0x38, L"9", 270, 32, 28, 28}, {0x39, L"0", 300, 32, 28, 28},
    {VK_OEM_MINUS, L"-", 330, 32, 28, 28}, {VK_OEM_PLUS, L"=", 360, 32, 28, 28},
    {VK_BACK, L"BkSp", 390, 32, 56, 28},
    {VK_INSERT, L"Ins", 456, 32, 36, 28}, {VK_HOME, L"Home", 494, 32, 36, 28}, {VK_PRIOR, L"PgUp", 532, 32, 36, 28},
    {VK_NUMLOCK, L"Num", 578, 32, 28, 28}, {VK_DIVIDE, L"/", 608, 32, 28, 28},
    {VK_MULTIPLY, L"*", 638, 32, 28, 28}, {VK_SUBTRACT, L"-", 668, 32, 28, 28},

    {VK_TAB, L"Tab", 0, 64, 44, 28}, {0x51, L"Q", 46, 64, 28, 28}, {0x57, L"W", 76, 64, 28, 28},
    {0x45, L"E", 106, 64, 28, 28}, {0x52, L"R", 136, 64, 28, 28}, {0x54, L"T", 166, 64, 28, 28},
    {0x59, L"Y", 196, 64, 28, 28}, {0x55, L"U", 226, 64, 28, 28}, {0x49, L"I", 256, 64, 28, 28},
    {0x4F, L"O", 286, 64, 28, 28}, {0x50, L"P", 316, 64, 28, 28},
    {VK_OEM_4, L"[", 346, 64, 28, 28}, {VK_OEM_6, L"]", 376, 64, 28, 28}, {VK_OEM_5, L"\\", 406, 64, 40, 28},
    {VK_DELETE, L"Del", 456, 64, 36, 28}, {VK_END, L"End", 494, 64, 36, 28}, {VK_NEXT, L"PgDn", 532, 64, 36, 28},
    {VK_NUMPAD7, L"7", 578, 64, 28, 28}, {VK_NUMPAD8, L"8", 608, 64, 28, 28},
    {VK_NUMPAD9, L"9", 638, 64, 28, 28}, {VK_ADD, L"+", 668, 64, 28, 56},

    {VK_CAPITAL, L"Caps", 0, 96, 52, 28}, {0x41, L"A", 54, 96, 28, 28}, {0x53, L"S", 84, 96, 28, 28},
    {0x44, L"D", 114, 96, 28, 28}, {0x46, L"F", 144, 96, 28, 28}, {0x47, L"G", 174, 96, 28, 28},
    {0x48, L"H", 204, 96, 28, 28}, {0x4A, L"J", 234, 96, 28, 28}, {0x4B, L"K", 264, 96, 28, 28},
    {0x4C, L"L", 294, 96, 28, 28},
    {VK_OEM_1, L";", 324, 96, 28, 28}, {VK_OEM_7, L"'", 354, 96, 28, 28},
    {VK_RETURN, L"Enter", 384, 96, 62, 28},
    {VK_NUMPAD4, L"4", 578, 96, 28, 28}, {VK_NUMPAD5, L"5", 608, 96, 28, 28},
    {VK_NUMPAD6, L"6", 638, 96, 28, 28},

    {VK_LSHIFT, L"Shift", 0, 128, 68, 28}, {0x5A, L"Z", 70, 128, 28, 28}, {0x58, L"X", 100, 128, 28, 28},
    {0x43, L"C", 130, 128, 28, 28}, {0x56, L"V", 160, 128, 28, 28}, {0x42, L"B", 190, 128, 28, 28},
    {0x4E, L"N", 220, 128, 28, 28}, {0x4D, L"M", 250, 128, 28, 28},
    {VK_OEM_COMMA, L",", 280, 128, 28, 28}, {VK_OEM_PERIOD, L".", 310, 128, 28, 28},
    {VK_OEM_2, L"/", 340, 128, 28, 28}, {VK_RSHIFT, L"Shift", 370, 128, 76, 28},
    {VK_UP, L"↑", 494, 128, 36, 28},
    {VK_NUMPAD1, L"1", 578, 128, 28, 28}, {VK_NUMPAD2, L"2", 608, 128, 28, 28},
    {VK_NUMPAD3, L"3", 638, 128, 28, 28}, {VK_RETURN, L"Ent", 668, 128, 28, 56},

    {VK_LCONTROL, L"Ctrl", 0, 160, 44, 28}, {VK_LWIN, L"Win", 48, 160, 36, 28},
    // 注意：右 Ctrl 必须排在方向键左侧且不与 ←（x=456）重叠，
    // 否则后创建的 ← 按钮会遮住右 Ctrl，导致鼠标无法选中它
    {VK_LMENU, L"Alt", 88, 160, 36, 28}, {VK_SPACE, L"Space", 128, 160, 158, 28},
    {VK_RMENU, L"Alt", 290, 160, 36, 28}, {VK_RWIN, L"Win", 330, 160, 36, 28},
    {VK_APPS, L"Menu", 370, 160, 36, 28}, {VK_RCONTROL, L"Ctrl", 410, 160, 42, 28},
    {VK_LEFT, L"←", 456, 160, 36, 28}, {VK_DOWN, L"↓", 494, 160, 36, 28}, {VK_RIGHT, L"→", 532, 160, 36, 28},
    {VK_NUMPAD0, L"0", 578, 160, 58, 28}, {VK_DECIMAL, L".", 638, 160, 28, 28},

    {VK_VOLUME_MUTE, L"Mute", 0, 196, 44, 28}, {VK_VOLUME_DOWN, L"Vol-", 48, 196, 44, 28},
    {VK_VOLUME_UP, L"Vol+", 96, 196, 44, 28}, {VK_MEDIA_NEXT_TRACK, L"Next", 148, 196, 44, 28},
    {VK_MEDIA_PREV_TRACK, L"Prev", 196, 196, 44, 28}, {VK_MEDIA_PLAY_PAUSE, L"Play", 244, 196, 44, 28},
    {VK_LAUNCH_MAIL, L"Mail", 292, 196, 44, 28}, {VK_LAUNCH_APP2, L"App", 340, 196, 44, 28},
};
static const int KB_KEY_COUNT = sizeof(KB_KEYS) / sizeof(KB_KEYS[0]); // 键盘按键总数

// 虚拟键盘选择器窗口类名
static const wchar_t* KB_PICKER_CLASS = L"KeySentryKeyboardPicker";

// 虚拟键盘选择器中的修饰键勾选框 ID（按键按钮占用 10000 起，这里从 11000 起避免冲突）
static const int IDC_KB_MOD_CTRL = 11000;
static const int IDC_KB_MOD_SHIFT = 11001;
static const int IDC_KB_MOD_ALT = 11002;
static const int IDC_KB_MOD_WIN = 11003;

// 虚拟键盘选择器数据
struct KbPickerData {
    int selectedVK;     // 用户选中的虚拟键码，0 表示取消
    UINT selectedMods;  // 勾选的修饰键组合（仅组合键录入模式有效）
    bool allowMods;     // 是否启用组合键录入（显示修饰键勾选框）
    bool done;          // 对话框完成标志
};

// 读取勾选框状态
static bool IsKbModChecked(HWND hwnd, int id) {
    HWND chk = GetDlgItem(hwnd, id);
    return chk && SendMessageW(chk, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

// ===== 虚拟键盘选择器窗口过程 =====
// 处理用户点击虚拟键盘上的按键
static LRESULT CALLBACK KbPickerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* pd = reinterpret_cast<KbPickerData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        // 按键按钮的 ID 范围为 10000 ~ 10000+KB_KEY_COUNT
        if (id >= 10000 && id < 10000 + KB_KEY_COUNT) {
            int vk = KB_KEYS[id - 10000].vk;
            if (pd) {
                // 组合键录入模式下，修饰键本身不能作为主键：否则得到的是无意义的
                // “按下某修饰键”映射，提示用户改为勾选修饰键再点主键
                if (pd->allowMods && ConfigUtils::IsModifierKey(vk)) {
                    MessageBoxW(hwnd, L"修饰键不能作为组合键的主键。\n请先勾选下方的 Ctrl/Shift/Alt/Win，再点击要使用的主键。",
                                L"提示", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                UINT mods = 0;
                if (pd->allowMods) {
                    if (IsKbModChecked(hwnd, IDC_KB_MOD_CTRL)) mods |= MOD_CONTROL;
                    if (IsKbModChecked(hwnd, IDC_KB_MOD_SHIFT)) mods |= MOD_SHIFT;
                    if (IsKbModChecked(hwnd, IDC_KB_MOD_ALT)) mods |= MOD_ALT;
                    if (IsKbModChecked(hwnd, IDC_KB_MOD_WIN)) mods |= MOD_WIN;
                }
                pd->selectedVK = vk;
                pd->selectedMods = mods;
                pd->done = true;
                DestroyWindow(hwnd);
            }
            return 0;
        }
        if (id == IDCANCEL) {
            if (pd) { pd->selectedVK = 0; pd->selectedMods = 0; pd->done = true; }
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    // Esc 取消（焦点在窗口自身时生效；焦点在按钮上时用下方"取消"按钮）
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (pd) { pd->selectedVK = 0; pd->selectedMods = 0; pd->done = true; }
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (pd) { pd->selectedVK = 0; pd->selectedMods = 0; pd->done = true; }
        DestroyWindow(hwnd);
        return 0;
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC: {
        // 设置按钮和静态控件的背景为透明
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ===== 显示虚拟键盘选择器 =====
// 创建一个模态对话框，显示标准键盘布局供用户点击选择目标按键
// allowMods=true 时额外显示修饰键勾选框，可录入组合键（如勾选 Ctrl 后点击 C）
// 返回选中的虚拟键码，0 表示用户取消
int SettingsDialog::ShowKeyboardPicker(HWND parent, UINT* outMods, bool allowMods) {
    if (outMods) *outMods = 0;
    static bool registered = false;
    if (!registered) {
        // 注册键盘选择器窗口类
        WNDCLASSW wc = {};
        wc.lpfnWndProc = KbPickerWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = KB_PICKER_CLASS;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // 注册失败时不得标记成功：否则窗口类始终未注册，
        // CreateWindowExW 会持续返回空且不再重试（ERROR_CLASS_ALREADY_EXISTS 视为成功）
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 0;
        registered = true;
    }

    // 计算窗口尺寸并居中显示（高度预留底部"取消"按钮，按系统 DPI 放大）
    RECT rc = { 0, 0, ScaleUi(710), ScaleUi(292) };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    KbPickerData pd = { 0, 0, allowMods, false };

    // WS_EX_CONTROLPARENT：非对话框窗口承载控件时，IsDialogMessage 需要它
    // 才能在子控件之间正确进行 Tab 导航
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, KB_PICKER_CLASS,
                                 allowMods ? L"选择目标按键或组合键" : L"选择映射按键",
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                 (screenW - winW) / 2, (screenH - winH) / 2,
                                 winW, winH,
                                 parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return 0;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&pd));

    GdiObjectGuard keyFont(CreateUiFont(12, FW_NORMAL));

    // 为每个键盘按键创建按钮控件
    // 必须带 WS_TABSTOP：本对话框正是为"鼠标不便"的场景准备的，
    // 缺少它会导致 Tab 无法在按键间移动焦点，只能用鼠标点选
    for (int i = 0; i < KB_KEY_COUNT; i++) {
        CreateWindowExW(0, L"BUTTON", KB_KEYS[i].label,
                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                         KB_KEYS[i].x + 8, KB_KEYS[i].y + 8, KB_KEYS[i].w, KB_KEYS[i].h,
                         hwnd, (HMENU)(LONG_PTR)(10000 + i), GetModuleHandleW(nullptr), nullptr);
        HWND btn = GetDlgItem(hwnd, 10000 + i);
        SendMessageW(btn, WM_SETFONT, (WPARAM)keyFont.get(), TRUE);
    }

    // 组合键录入模式：底部显示修饰键勾选框与操作提示
    if (allowMods) {
        struct { int id; const wchar_t* text; int x, w; } mods[] = {
            { IDC_KB_MOD_CTRL,  L"Ctrl",  8,   62 },
            { IDC_KB_MOD_SHIFT, L"Shift", 74,  62 },
            { IDC_KB_MOD_ALT,   L"Alt",   140, 52 },
            { IDC_KB_MOD_WIN,   L"Win",   196, 52 },
        };
        for (const auto& m : mods) {
            HWND chk = CreateWindowExW(0, L"BUTTON", m.text,
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                       m.x, 243, m.w, 20,
                                       hwnd, (HMENU)(LONG_PTR)m.id, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(chk, WM_SETFONT, (WPARAM)keyFont.get(), TRUE);
        }
        HWND hint = CreateWindowExW(0, L"STATIC", L"先勾选修饰键，再点击主键",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    410, 245, 290, 18,
                                    hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hint, WM_SETFONT, (WPARAM)keyFont.get(), TRUE);
    }

    // 底部"取消"按钮，提供明确的取消路径（点击后走 WM_COMMAND IDCANCEL）
    // 按 Tab 顺序排在所有按键之后，便于键盘操作
    HWND btnCancel = CreateWindowExW(0, L"BUTTON", L"取消",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                     310, 240, 90, 26,
                                     hwnd, (HMENU)(LONG_PTR)IDCANCEL, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(btnCancel, WM_SETFONT, (WPARAM)keyFont.get(), TRUE);

    // 键盘按钮与取消按钮均按逻辑坐标创建，统一放大到物理坐标
    ScaleChildWindows(hwnd, GetUiScale());

    // 模态消息循环
    {
        ModalGuard guard(parent);
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        SetFocus(hwnd);   // 让 Esc 能被窗口自身收到

        MSG msg;
        while (!pd.done && IsWindow(hwnd)) {
            BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
            if (ret <= 0) { if (ret == 0) PostQuitMessage((int)msg.wParam); break; }
            // 将热键消息转发给主窗口处理
            // 按键捕获期间丢弃 WM_HOTKEY，避免按下的组合同时触发已注册热键
            if (msg.message == WM_HOTKEY) {
                if (!g_suppressHotkeys && g_mainWnd && IsWindow(g_mainWnd))
                    SendMessageW(g_mainWnd, msg.message, msg.wParam, msg.lParam);
                continue;
            }
            // 交给对话框管理器：Tab 切换焦点、方向键移动、Esc 自动转成 IDCANCEL，
            // 这样焦点在按钮上时 Esc 也能取消
            if (IsDialogMessageW(hwnd, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (IsWindow(hwnd)) DestroyWindow(hwnd);

    if (outMods) *outMods = (pd.selectedVK != 0) ? pd.selectedMods : 0;
    return pd.selectedVK;
}

// ===== 显示设置对话框 =====
// 创建并显示模态设置对话框，管理对话框的生命周期和消息循环
INT_PTR SettingsDialog::Show(HWND parent, AppConfig& config) {
    // 初始化对话框数据
    DialogData data;
    data.config = &config;
    data.workingCopy = config;       // 创建工作副本，用户修改不影响原始配置
    data.originalConfig = config;    // 保存原始配置快照，取消时恢复
    data.currentTab = -1;            // 尚未选中任何选项卡
    data.capturingHotKey = false;
    data.captureMod = 0;
    data.captureVK = 0;
    data.capturingCloseHotKey = false;
    data.captureCloseMod = 0;
    data.captureCloseVK = 0;

    // 注册窗口类（仅注册一次）
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = SETTINGS_CLASS;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // 注册失败时不得标记成功：否则窗口类始终未注册，
        // CreateWindowExW 会持续返回空且不再重试（ERROR_CLASS_ALREADY_EXISTS 视为成功）
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 0;

        registered = true;
    }

    // 计算窗口尺寸并居中（按系统 DPI 放大，保证高分辨率下不偏小）
    RECT rc = { 0, 0, ScaleUi(CLIENT_W), ScaleUi(CLIENT_H) };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    RECT work = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int workW = work.right - work.left;
    int workH = work.bottom - work.top;
    int winX = work.left + (workW - winW) / 2;
    int winY = work.top + (workH - winH) / 2;
    // 夹紧到工作区，避免缩放后标题栏或底部按钮移出屏幕
    if (winX < work.left) winX = work.left;
    if (winY < work.top) winY = work.top;

    // 创建设置对话框主窗口
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, SETTINGS_CLASS,
                                 L"键客 设置",
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                 winX, winY,
                                 winW, winH,
                                 parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    // 窗口创建失败（极端资源耗尽）：直接返回，避免以空句柄创建子控件产生孤儿窗口
    if (!hwnd) return 0;

    // 将对话框数据存储到窗口的 USERDATA 中
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&data));

    // 设置全局设置窗口句柄，供主窗口消息处理使用
    SetSettingsWnd(hwnd);

    HFONT font = GetDlgFont();

    // 创建选项卡控件（按 DPI 缩放物理坐标；页面控件随后由 ScalePageControls 缩放）
    HWND tabCtrl = CreateWindowExW(0, WC_TABCONTROLW, L"",
                                     WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
                                     ScaleUi(TAB_X), ScaleUi(TAB_Y),
                                     ScaleUi(TAB_W), ScaleUi(TAB_H),
                                     hwnd, (HMENU)IDC_TAB_MAIN, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(tabCtrl, font);
    SetWindowSubclass(tabCtrl, TabSubclassProc, 0, 0); // 子类化以自定义绘制

    // 添加六个选项卡标签
    TCITEMW tie = {};
    tie.mask = TCIF_TEXT;
    tie.pszText = const_cast<LPWSTR>(L"常规");
    TabCtrl_InsertItem(tabCtrl, 0, &tie);
    tie.pszText = const_cast<LPWSTR>(L"按键禁用");
    TabCtrl_InsertItem(tabCtrl, 1, &tie);
    tie.pszText = const_cast<LPWSTR>(L"按键映射");
    TabCtrl_InsertItem(tabCtrl, 2, &tie);
    tie.pszText = const_cast<LPWSTR>(L"热键管理");
    TabCtrl_InsertItem(tabCtrl, 3, &tie);
    tie.pszText = const_cast<LPWSTR>(L"窗口隐藏");
    TabCtrl_InsertItem(tabCtrl, 4, &tie);
    tie.pszText = const_cast<LPWSTR>(L"窗口置顶");
    TabCtrl_InsertItem(tabCtrl, 5, &tie);

    // 创建底部按钮：关于、确定、取消、应用
    HWND btnAbout = CreateWindowExW(0, L"BUTTON", L"关于",
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      ScaleUi(10), ScaleUi(BTN_Y), ScaleUi(BTN_W), ScaleUi(BTN_H),
                                      hwnd, (HMENU)IDC_BTN_ABOUT, GetModuleHandleW(nullptr), nullptr);
    HWND btnOk = CreateWindowExW(0, L"BUTTON", L"确定",
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   ScaleUi(CLIENT_W - BTN_W * 3 - 30), ScaleUi(BTN_Y),
                                   ScaleUi(BTN_W), ScaleUi(BTN_H),
                                   hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
    HWND btnCancel = CreateWindowExW(0, L"BUTTON", L"取消",
                                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       ScaleUi(CLIENT_W - BTN_W * 2 - 20), ScaleUi(BTN_Y),
                                       ScaleUi(BTN_W), ScaleUi(BTN_H),
                                       hwnd, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);
    HWND btnApply = CreateWindowExW(0, L"BUTTON", L"应用",
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      ScaleUi(CLIENT_W - BTN_W - 10), ScaleUi(BTN_Y),
                                      ScaleUi(BTN_W), ScaleUi(BTN_H),
                                      hwnd, (HMENU)IDAPPLY, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(btnAbout, font);
    SetControlFont(btnOk, font);
    SetControlFont(btnCancel, font);
    SetControlFont(btnApply, font);

    // 切换到上次选中的选项卡
    SwitchTab(hwnd, data.workingCopy.tabState, data.workingCopy);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // 模态消息循环
    MSG msg;
    while (IsWindow(hwnd)) {
        BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
        if (ret <= 0) { if (ret == 0) PostQuitMessage((int)msg.wParam); break; }
        // 将热键消息转发给主窗口处理
        // 捕获热键期间丢弃 WM_HOTKEY：RegisterHotKey 不受键盘钩子影响，
        // 若此时按下的组合正好是已注册热键，会触发热键动作打断捕获
        if (msg.message == WM_HOTKEY) {
            if (!g_suppressHotkeys && !(data.capturingHotKey || data.capturingCloseHotKey)) {
                if (g_mainWnd && IsWindow(g_mainWnd)) SendMessageW(g_mainWnd, msg.message, msg.wParam, msg.lParam);
            }
            continue;
        }
        // 处理内联热键捕获（按键由吞键钩子以 WM_INLINE_CAPTURE_KEY 投递）
        if (msg.message == WM_INLINE_CAPTURE_KEY &&
            (data.capturingHotKey || data.capturingCloseHotKey)) {
            UINT mod = (UINT)msg.lParam;
            int vk = (int)msg.wParam;
            // 纯修饰键：继续等待主键
            if (InlineCapIsModifier(vk)) continue;

            HWND btn = GetDlgItem(hwnd, data.capturingHotKey ? IDC_BTN_SETHOTKEY : IDC_BTN_SETCLOSEHOTKEY);
            if (vk == VK_ESCAPE) {
                // Esc 取消捕获，恢复显示原热键
                if (data.capturingHotKey) {
                    data.capturingHotKey = false;
                    // 仅当另一项捕获也未进行时才卸载吞键钩子，避免相互干扰
                    if (!data.capturingCloseHotKey) EndInlineCapture(hwnd);
                    SetWindowTextW(btn,
                                   FormatHotKey(data.workingCopy.bossKeyMod, data.workingCopy.bossKeyVK).c_str());
                } else {
                    data.capturingCloseHotKey = false;
                    if (!data.capturingHotKey) EndInlineCapture(hwnd);
                    SetWindowTextW(btn,
                                   FormatHotKey(data.workingCopy.bossKeyCloseMod, data.workingCopy.bossKeyCloseVK).c_str());
                }
            } else if (mod == 0) {
                // 拒绝无修饰键的裸键：注册后会在全系统劫持该普通按键（如 A/回车）
                SetWindowTextW(btn, L"请使用 修饰键+按键 的组合...");
            } else if (data.capturingHotKey) {
                // 记录捕获到的窗口隐藏热键组合
                data.captureMod = mod;
                data.captureVK = (UINT)vk;
                data.capturingHotKey = false;
                if (!data.capturingCloseHotKey) EndInlineCapture(hwnd);
                data.workingCopy.bossKeyMod = mod;
                data.workingCopy.bossKeyVK = (UINT)vk;
                SetWindowTextW(btn, FormatHotKey(mod, (UINT)vk).c_str());
            } else {
                // 记录捕获到的一键关闭热键组合
                data.captureCloseMod = mod;
                data.captureCloseVK = (UINT)vk;
                data.capturingCloseHotKey = false;
                if (!data.capturingHotKey) EndInlineCapture(hwnd);
                data.workingCopy.bossKeyCloseMod = mod;
                data.workingCopy.bossKeyCloseVK = (UINT)vk;
                SetWindowTextW(btn, FormatHotKey(mod, (UINT)vk).c_str());
            }
            continue;
        }
        if (!IsWindow(hwnd)) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 兜底：捕获过程中若对话框被关闭，复位捕获状态并卸载吞键钩子，
    // 避免键盘钩子长期放行/吞掉按键；EndInlineCapture 内部按归属判断，
    // 若钩子已被其他捕获窗口接管则不会误拆
    if (data.capturingHotKey || data.capturingCloseHotKey || s_inlineCapWnd == hwnd) {
        data.capturingHotKey = false;
        data.capturingCloseHotKey = false;
        EndInlineCapture(hwnd);
    }

    // 兜底：异常退出路径（GetMessage 返回 0/-1）下窗口可能仍存活，
    // 此时 GWLP_USERDATA 指向本函数栈上的 data，必须销毁窗口避免悬垂
    if (IsWindow(hwnd)) DestroyWindow(hwnd);

    // 对话框关闭后清理字体资源
    CleanupDlgFont();
    return 0;
}

// ===== 格式化热键为可读字符串 =====
// 将修饰键标志和虚拟键码转换为 "Ctrl+Alt+F1" 这样的格式
std::wstring SettingsDialog::FormatHotKey(UINT mod, UINT vk) {
    std::wstring result;
    if (mod & MOD_CONTROL) result += L"Ctrl+";
    if (mod & MOD_ALT) result += L"Alt+";
    if (mod & MOD_SHIFT) result += L"Shift+";
    if (mod & MOD_WIN) result += L"Win+";
    result += AppConfig::VKToName((int)vk);
    return result;
}

// ===== 保存当前选项卡页面控件状态 =====
// 根据当前选项卡索引，读取对应页面上的复选框和输入框状态，写入配置对象
void SettingsDialog::SaveCurrentTabState(HWND hwnd, AppConfig& config) {
    // 辅助 lambda：获取复选框的选中状态
    auto chk = [&](int id) -> bool {
        HWND h = GetDlgItem(hwnd, id);
        if (!h) return false;
        return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
    };

    switch (config.tabState) {
    case 0: { // 常规页面
        config.autoStart = chk(IDC_CHK_AUTOSTART);
        config.forceNumLockOn = chk(IDC_CHK_FORCENUMLOCK);
        config.disableStartupNotification = chk(IDC_CHK_DISABLE_STARTUP_NOTIFY);
        config.showLockKeyOverlay = chk(IDC_CHK_OVERLAY);
        config.showMuteOverlay = chk(IDC_CHK_MUTE_OVERLAY);
        config.overlayPrimaryOnly = chk(IDC_CHK_PRIMARY_ONLY);
        // 保存资源管理器文件夹选项下拉框（0=不改变, 1=开启, 2=关闭）
        // CB_GETCURSEL 失败返回 CB_ERR(-1)，必须收敛到 0~2，避免把非法值写入配置
        auto combo = [&](int id) -> int {
            int sel = (int)SendMessageW(GetDlgItem(hwnd, id), CB_GETCURSEL, 0, 0);
            if (sel < 0) sel = 0;
            if (sel > 2) sel = 2;
            return sel;
        };
        config.explorerHideHidden = combo(IDC_CBO_EXPLORER_HIDDEN);
        config.explorerHideExt = combo(IDC_CBO_EXPLORER_EXT);
        config.explorerHideOS = combo(IDC_CBO_EXPLORER_OS);
        break;
    }
    case 1: // 按键禁用页面
        config.disableSpecifiedKeysEnabled = chk(IDC_CHK_DISABLEKEYS);
        config.disableNumLock = chk(IDC_CHK_DISABLENUM);
        config.forceInsertMode = chk(IDC_CHK_INSERT);
        break;
    case 2: // 按键映射页面
        config.keyRemapEnabled = chk(IDC_CHK_KEYREMAP);
        break;
    case 3: // 热键管理页面
        config.customHotkeysEnabled = chk(IDC_CHK_CUSTOM_HK_ENABLE);
        break;
    case 4: // 窗口隐藏页面
        config.bossKeyEnabled = chk(IDC_CHK_BOSSKEY);
        config.bossKeyMute = chk(IDC_CHK_BOSSMUTE);
        config.bossKeyHideCurrent = chk(IDC_CHK_BOSS_HIDE_CURRENT);
        config.bossKeySendPause = chk(IDC_CHK_BOSS_SEND_PAUSE);
        config.bossKeyMiddleButton = chk(IDC_CHK_BOSS_MIDDLE_BTN);
        config.bossKeySideButton1 = chk(IDC_CHK_BOSS_SIDE_BTN1);
        config.bossKeySideButton2 = chk(IDC_CHK_BOSS_SIDE_BTN2);
        config.bossKeyCornerTL = chk(IDC_CHK_BOSS_CORNER_TL);
        config.bossKeyCornerTR = chk(IDC_CHK_BOSS_CORNER_TR);
        config.bossKeyCornerBL = chk(IDC_CHK_BOSS_CORNER_BL);
        config.bossKeyCornerBR = chk(IDC_CHK_BOSS_CORNER_BR);
        config.bossKeyAutoHide = chk(IDC_CHK_BOSS_AUTO_HIDE);
        config.bossKeyCloseOnExit = chk(IDC_CHK_BOSS_CLOSE_ON_EXIT);
        {
            // 读取自动隐藏时间的数值（1~120 分钟）
            HWND spin = GetDlgItem(hwnd, IDC_SPIN_AUTO_HIDE_TIME);
            if (spin) {
                // UDM_GETPOS 高 16 位是错误标志，必须取 LOWORD
                config.bossKeyAutoHideTime = (int)LOWORD(SendMessageW(spin, UDM_GETPOS, 0, 0));
                if (config.bossKeyAutoHideTime < 1) config.bossKeyAutoHideTime = 1;
                if (config.bossKeyAutoHideTime > 120) config.bossKeyAutoHideTime = 120;
            }
        }
        break;
    case 5: // ===== 窗口置顶页面 =====
        config.winDGuardEnabled = chk(IDC_CHK_WINDGUARD);
        break;
    }
}

// ===== 更新禁用按键控件的启用/禁用状态 =====
void SettingsDialog::UpdateDisableKeysState(HWND hwnd, bool enabled) {
    HWND lst = GetDlgItem(hwnd, IDC_LST_DISABLED);
    HWND btnAdd = GetDlgItem(hwnd, IDC_BTN_ADDKEY);
    HWND btnDel = GetDlgItem(hwnd, IDC_BTN_DELKEY);
    if (lst) EnableWindow(lst, enabled);
    if (btnAdd) EnableWindow(btnAdd, enabled);
    if (btnDel) EnableWindow(btnDel, enabled);
}

// ===== 更新按键映射控件的启用/禁用状态 =====
void SettingsDialog::UpdateRemapState(HWND hwnd, bool enabled) {
    HWND lst = GetDlgItem(hwnd, IDC_LST_REMAP);
    HWND btnAdd = GetDlgItem(hwnd, IDC_BTN_ADDREMAP);
    HWND btnDel = GetDlgItem(hwnd, IDC_BTN_DELREMAP);
    HWND btnClearCombo = GetDlgItem(hwnd, IDC_BTN_CLEAR_COMBOREMAP);
    if (lst) EnableWindow(lst, enabled);
    if (btnAdd) EnableWindow(btnAdd, enabled);
    if (btnDel) EnableWindow(btnDel, enabled);
    if (btnClearCombo) EnableWindow(btnClearCombo, enabled);
}

// ===== 取消进行中的内联热键捕获 =====
// 捕获只用键盘钩子拦键盘，鼠标仍可操作，因此用户可能在捕获过程中切换选项卡、
// 点击"重置"或打开其他对话框。这些操作会脱离捕获上下文（捕获按钮可能被销毁），
// 若不取消，吞键钩子会继续吞掉全系统按键且界面无任何提示。
// 仅复位捕获标志、恢复按钮文本（按钮存在时）并卸载吞键钩子。
static void CancelInlineCapture(SettingsDialog::DialogData* data, HWND hwnd) {
    if (!data || (!data->capturingHotKey && !data->capturingCloseHotKey)) return;

    if (data->capturingHotKey) {
        data->capturingHotKey = false;
        HWND btn = GetDlgItem(hwnd, IDC_BTN_SETHOTKEY);
        if (btn) {
            SetWindowTextW(btn, SettingsDialog::FormatHotKey(
                data->workingCopy.bossKeyMod, data->workingCopy.bossKeyVK).c_str());
        }
    }
    if (data->capturingCloseHotKey) {
        data->capturingCloseHotKey = false;
        HWND btn = GetDlgItem(hwnd, IDC_BTN_SETCLOSEHOTKEY);
        if (btn) {
            SetWindowTextW(btn, SettingsDialog::FormatHotKey(
                data->workingCopy.bossKeyCloseMod, data->workingCopy.bossKeyCloseVK).c_str());
        }
    }
    EndInlineCapture(hwnd);
}

// ===== 切换选项卡页面 =====
// 保存当前页面状态，销毁旧控件，根据新索引创建对应的页面控件
void SettingsDialog::SwitchTab(HWND hwnd, int tabIndex, AppConfig& config) {
    auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    // 切页会销毁当前页面的捕获按钮，先取消进行中的捕获，
    // 避免捕获态残留导致键盘被持续吞掉、且下一次按键静默改写老板键
    CancelInlineCapture(data, hwnd);

    // 如果当前有已选中的选项卡且不是目标选项卡，先保存当前页面状态
    if (data->currentTab >= 0 && data->currentTab != tabIndex) {
        config.tabState = data->currentTab;
        SaveCurrentTabState(hwnd, config);
    }

    data->currentTab = tabIndex;
    CleanupTabPageControls(hwnd); // 销毁当前页面的所有控件

    HWND tabCtrl = GetDlgItem(hwnd, IDC_TAB_MAIN);
    TabCtrl_SetCurSel(tabCtrl, tabIndex); // 更新选项卡控件的选中状态

    HFONT font = GetDlgFont();
    int PAGE_X, PAGE_Y, PAGE_W, PAGE_H;
    GetPageArea(tabCtrl, hwnd, &PAGE_X, &PAGE_Y, &PAGE_W, &PAGE_H);

    // 页面布局参数
    int y = PAGE_Y;
    int lineH = 22;         // 行高
    int btnH = BTN_H;       // 按钮高度
    int smallBtnW = 80;     // 小按钮宽度

    // ===== 控件创建辅助 lambda 函数 =====

    // 创建复选框
    auto makeChk = [&](int id, const wchar_t* text, int px, int py, int w, bool checked, int h = 0) {
        HWND h2 = CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_VCENTER,
                                   px, py, w, h > 0 ? h : lineH,
                                   hwnd, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(h2, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
        SetControlFont(h2, font);
        return h2;
    };

    // 创建标签
    auto makeLabel = [&](const wchar_t* text, int px, int py, int w, int h = 0) {
        HWND h2 = CreateWindowExW(0, L"STATIC", text,
                                   WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                   px, py, w, h > 0 ? h : lineH,
                                   hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(h2, font);
        return h2;
    };

    // 创建编辑框
    auto makeEdit = [&](int id, int px, int py, int w) {
        HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                   px, py, w, lineH,
                                   hwnd, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(h, font);
        return h;
    };

    // 创建按钮
    auto makeButton = [&](int id, const wchar_t* text, int px, int py, int w, int h2) {
        HWND bh = CreateWindowExW(0, L"BUTTON", text,
                                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    px, py, w, h2,
                                    hwnd, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(bh, font);
        return bh;
    };

    // 创建列表框
    auto makeListBox = [&](int id, int px, int py, int w, int h2) {
        HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                   WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                                   px, py, w, h2,
                                   hwnd, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(h, font);
        return h;
    };

    // 创建列表视图（报表模式）
    auto makeListView = [&](int id, int px, int py, int w, int h2) {
        HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                   WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                   px, py, w, h2,
                                   hwnd, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(h, font);
        ListView_SetExtendedListViewStyle(h, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        ListView_SetBkColor(h, RGB(255, 255, 255));
        ListView_SetTextColor(h, GetSysColor(COLOR_WINDOWTEXT));
        ListView_SetTextBkColor(h, RGB(255, 255, 255));
        return h;
    };

    // 添加列表视图列
    auto addLvColumn = [](HWND lv, int idx, const wchar_t* text, int width) {
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.pszText = const_cast<LPWSTR>(text);
        col.cx = width;
        col.fmt = LVCFMT_LEFT;
        ListView_InsertColumn(lv, idx, &col);
    };

    // 创建分组框
    auto makeGroupBox = [&](const wchar_t* text, int px, int py, int w, int h2) {
        HWND h = CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                   px, py, w, h2,
                                   hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(h, font);
        return h;
    };

    switch (tabIndex) {
    case 0: { // ===== 常规页面 =====
        int gbPad = 12;
        int gbW = PAGE_W - 10;

        {   // 启动与退出分组
            int gbX = PAGE_X;
            int gbY = y;
            int innerY = y + 24;

            HWND gbStartup = CreateWindowExW(0, L"BUTTON", L"启动与退出",
                                               WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                               gbX, gbY, gbW, 100,
                                               hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SetControlFont(gbStartup, font);

            makeChk(IDC_CHK_AUTOSTART, L"开机自动启动", gbX + gbPad, innerY, gbW - gbPad * 2, config.autoStart);
            innerY += lineH + 4;
            makeChk(IDC_CHK_FORCENUMLOCK, L"启动后开启 NumLock", gbX + gbPad, innerY, gbW - gbPad * 2, config.forceNumLockOn);
            innerY += lineH + 4;
            makeChk(IDC_CHK_DISABLE_STARTUP_NOTIFY, L"启动时不显示通知", gbX + gbPad, innerY, gbW - gbPad * 2, config.disableStartupNotification);
            innerY += lineH + 4;

            MoveWindow(gbStartup, gbX, gbY, gbW, innerY - gbY + 4, TRUE);
            SetWindowPos(gbStartup, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
            y = innerY + 8;
        }

        {   // 状态提示分组
            int gbX = PAGE_X;
            int gbY = y;
            int innerY = y + 24;

            HWND gbOverlay = CreateWindowExW(0, L"BUTTON", L"状态提示",
                                               WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                               gbX, gbY, gbW, 80,
                                               hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SetControlFont(gbOverlay, font);

            makeChk(IDC_CHK_OVERLAY, L"显示锁定键状态提示 (Caps/Num/Scroll Lock)", gbX + gbPad, innerY, gbW - gbPad * 2, config.showLockKeyOverlay);
            innerY += lineH + 4;
            makeChk(IDC_CHK_MUTE_OVERLAY, L"显示静音状态提示", gbX + gbPad, innerY, gbW - gbPad * 2, config.showMuteOverlay);
            innerY += lineH + 4;
            makeChk(IDC_CHK_PRIMARY_ONLY, L"仅在主屏显示提示", gbX + gbPad, innerY, gbW - gbPad * 2, config.overlayPrimaryOnly);
            innerY += lineH + 4;

            MoveWindow(gbOverlay, gbX, gbY, gbW, innerY - gbY + 4, TRUE);
            SetWindowPos(gbOverlay, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
            y = innerY + 8;
        }

        {   // 软件启动时设置分组（重置资源管理器文件夹选项）
            int gbX = PAGE_X;
            int gbY = y;
            int innerY = y + 24;
            int lblW = 210;
            int cboW = 80;
            int cboH = 21;
            int cboX = gbX + gbPad + lblW + 8;

            HWND gbExplorer = CreateWindowExW(0, L"BUTTON", L"软件启动时设置",
                                               WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                               gbX, gbY, gbW, 80,
                                               hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SetControlFont(gbExplorer, font);

            // 使用全局懒加载字体（CleanupDlgFont 统一清理），避免每次切页泄漏 GDI 句柄
            HFONT smallFont = GetSmallFont();

            auto makeExplorerCombo = [&](int id, const wchar_t* labelText, int selVal) {
                makeLabel(labelText, gbX + gbPad, innerY, lblW, cboH);
                HWND cbo = CreateWindowExW(0, L"COMBOBOX", L"",
                                            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                            cboX, innerY, cboW, 200,
                                            hwnd, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
                SendMessageW(cbo, WM_SETFONT, (WPARAM)smallFont, TRUE);
                SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)L"保持原状");
                SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)L"隐藏");
                SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)L"显示");
                SendMessageW(cbo, CB_SETCURSEL, selVal, 0);
            };

            makeExplorerCombo(IDC_CBO_EXPLORER_HIDDEN, L"隐藏文件和文件夹", config.explorerHideHidden);
            innerY += cboH + 4;
            makeExplorerCombo(IDC_CBO_EXPLORER_EXT, L"隐藏已知文件类型扩展名", config.explorerHideExt);
            innerY += cboH + 4;
            makeExplorerCombo(IDC_CBO_EXPLORER_OS, L"隐藏受保护的操作系统文件", config.explorerHideOS);
            innerY += cboH + 4;

            MoveWindow(gbExplorer, gbX, gbY, gbW, innerY - gbY + 4, TRUE);
            SetWindowPos(gbExplorer, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
            y = innerY + 8;
        }

        makeLabel(L"一键关闭程序热键:", PAGE_X, y, 130, btnH);
        makeButton(IDC_BTN_SETCLOSEHOTKEY, FormatHotKey(config.bossKeyCloseMod, config.bossKeyCloseVK).c_str(),
                    PAGE_X + 135, y, 180, btnH);
        makeButton(IDC_BTN_RESETCLOSEHOTKEY, L"默认",
                    PAGE_X + 135 + 185, y, 55, btnH);
        y += btnH + 8;

        {
            int descY = PAGE_Y + PAGE_H - 70;
            HWND lblDesc = CreateWindowExW(0, L"STATIC",
                L"功能说明：\n「软件启动时设置」在软件启动或者应用设置时，自动重置资源管理器的文件夹选项，包括隐藏文件、扩展名和系统文件的显示状态。",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                PAGE_X, descY, PAGE_W - 10, 40,
                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(lblDesc, WM_SETFONT, (WPARAM)GetSmallFont(), TRUE);
        }
        break;
    }

    case 1: { // ===== 按键禁用页面 =====
        makeChk(IDC_CHK_DISABLENUM, L"保持 NumLock 开启状态", PAGE_X, y, PAGE_W - 20, config.disableNumLock);
        y += lineH + 6;

        makeChk(IDC_CHK_INSERT, L"禁用 Insert 插入键", PAGE_X, y, PAGE_W - 20, config.forceInsertMode);
        y += lineH + 6;

        makeChk(IDC_CHK_DISABLEKEYS, L"禁用自定义按键", PAGE_X, y, PAGE_W - 20, config.disableSpecifiedKeysEnabled);
        y += lineH + 8;

        makeButton(IDC_BTN_ADDKEY, L"添加禁用按键", PAGE_X, y, smallBtnW * 2 + 10, btnH);
        makeButton(IDC_BTN_DELKEY, L"删除选中项", PAGE_X + smallBtnW * 2 + 20, y, smallBtnW * 2 + 10, btnH);
        y += btnH + 6;

        int lstH1 = PAGE_Y + PAGE_H - y - 10 - 50;
        if (lstH1 < 60) lstH1 = 60;
        {
            HWND lv = makeListView(IDC_LST_DISABLED, PAGE_X, y, PAGE_W - 10, lstH1);
            int lvW = PAGE_W - 30;
            addLvColumn(lv, 0, L"按键名称", lvW * 2 / 3);
            addLvColumn(lv, 1, L"按键代码", lvW / 3);
            y += lstH1 + 6;
        }
        {
            HWND lblDesc = CreateWindowExW(0, L"STATIC",
                L"功能说明：\n按键禁用功能允许用户阻止特定按键的输入，实现按键的屏蔽，避免误操作。",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                PAGE_X, y, PAGE_W - 10, 40,
                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(lblDesc, WM_SETFONT, (WPARAM)GetSmallFont(), TRUE);
        }
        RefreshDisabledKeyList(hwnd, config);
        UpdateDisableKeysState(hwnd, config.disableSpecifiedKeysEnabled);
        break;
    }

    case 2: { // ===== 按键映射页面 =====
        makeChk(IDC_CHK_KEYREMAP, L"启用按键映射", PAGE_X, y, PAGE_W - 20, config.keyRemapEnabled);
        y += lineH + 8;

        makeButton(IDC_BTN_ADDREMAP, L"添加按键映射", PAGE_X, y, smallBtnW * 2 + 10, btnH);
        makeButton(IDC_BTN_DELREMAP, L"删除选中项", PAGE_X + smallBtnW * 2 + 20, y, smallBtnW * 2 + 10, btnH);
        makeButton(IDC_BTN_CLEAR_COMBOREMAP, L"撤销全部组合键", PAGE_X + (smallBtnW * 2 + 20) * 2, y, smallBtnW * 2 + 10, btnH);
        y += btnH + 6;

        int lstH2 = PAGE_Y + PAGE_H - y - 10 - 50;
        if (lstH2 < 60) lstH2 = 60;
        {
            HWND lv = makeListView(IDC_LST_REMAP, PAGE_X, y, PAGE_W - 10, lstH2);
            int lvW = PAGE_W - 30;
            addLvColumn(lv, 0, L"原始按键", lvW / 2);
            addLvColumn(lv, 1, L"映射为", lvW / 2);
            y += lstH2 + 6;
        }
        {
            HWND lblDesc = CreateWindowExW(0, L"STATIC",
                L"功能说明：\n按键映射支持单键或组合键（如 C → Ctrl+C）。映射为组合键后，单独按源键不再输出原字符；恢复请删除该映射或点击「撤销全部组合键」。",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                PAGE_X, y, PAGE_W - 10, 40,
                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(lblDesc, WM_SETFONT, (WPARAM)GetSmallFont(), TRUE);
        }
        RefreshRemapList(hwnd, config);
        UpdateRemapState(hwnd, config.keyRemapEnabled);
        break;
    }

    case 3: { // ===== 热键管理页面 =====
        makeChk(IDC_CHK_CUSTOM_HK_ENABLE, L"启用自定义热键", PAGE_X, y, PAGE_W - 20, config.customHotkeysEnabled);
        y += lineH + 8;

        // 按钮行放在列表框上方，排版与"按键映射"页保持一致
        makeButton(IDC_BTN_ADD_CUSTOMHK, L"添加自定义热键", PAGE_X, y, smallBtnW * 2 + 10, btnH);
        makeButton(IDC_BTN_EDIT_CUSTOMHK, L"编辑选中项", PAGE_X + smallBtnW * 2 + 20, y, smallBtnW * 2 + 10, btnH);
        makeButton(IDC_BTN_DEL_CUSTOMHK, L"删除选中项", PAGE_X + (smallBtnW * 2 + 20) * 2, y, smallBtnW * 2 + 10, btnH);
        y += btnH + 6;

        int customLvH = PAGE_Y + PAGE_H - y - 10;
        if (customLvH < 60) customLvH = 60;
        HWND customLv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                         WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER,
                                         PAGE_X, y, PAGE_W - 10, customLvH,
                                         hwnd, (HMENU)IDC_LV_CUSTOMHK, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(customLv, font);
        SendMessageW(customLv, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        ListView_SetBkColor(customLv, RGB(255, 255, 255));
        ListView_SetTextColor(customLv, GetSysColor(COLOR_WINDOWTEXT));
        ListView_SetTextBkColor(customLv, RGB(255, 255, 255));

        LVCOLUMNW ccol = {};
        ccol.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        ccol.fmt = LVCFMT_LEFT;
        ccol.cx = 160; ccol.pszText = const_cast<LPWSTR>(L"热键");
        SendMessageW(customLv, LVM_INSERTCOLUMNW, 0, (LPARAM)&ccol);
        ccol.cx = 240; ccol.pszText = const_cast<LPWSTR>(L"命令");
        SendMessageW(customLv, LVM_INSERTCOLUMNW, 1, (LPARAM)&ccol);
        ccol.cx = 200; ccol.pszText = const_cast<LPWSTR>(L"描述");
        SendMessageW(customLv, LVM_INSERTCOLUMNW, 2, (LPARAM)&ccol);

        if (!config.customHotkeysEnabled) {
            EnableWindow(GetDlgItem(hwnd, IDC_LV_CUSTOMHK), FALSE);
            EnableWindow(GetDlgItem(hwnd, IDC_BTN_ADD_CUSTOMHK), FALSE);
            EnableWindow(GetDlgItem(hwnd, IDC_BTN_EDIT_CUSTOMHK), FALSE);
            EnableWindow(GetDlgItem(hwnd, IDC_BTN_DEL_CUSTOMHK), FALSE);
        }

        RefreshCustomHotkeyList(hwnd, config);
        break;
    }

    case 4: { // ===== 窗口隐藏页面 =====
        int rowH = 24;
        int gap = 6;
        int gbPad = 12;
        int gbW = PAGE_W - 10;
        int col3W = (gbW - gbPad * 2 - 16) / 3;

        makeChk(IDC_CHK_BOSSKEY, L"启用窗口隐藏", PAGE_X, y, PAGE_W - 20, config.bossKeyEnabled);
        y += rowH + 6;

        {
            int gbX = PAGE_X;
            int gbY = y;
            int innerY = y + 24;

            HWND gbHotkey = CreateWindowExW(0, L"BUTTON", L"键盘热键设置",
                                             WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                             gbX, gbY, gbW, 60,
                                             hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SetControlFont(gbHotkey, font);

            makeLabel(L"隐藏/显示窗口热键:", gbX + gbPad, innerY, 130, btnH);
            makeButton(IDC_BTN_SETHOTKEY, FormatHotKey(config.bossKeyMod, config.bossKeyVK).c_str(),
                        gbX + gbPad + 135, innerY, 180, btnH);
            makeButton(IDC_BTN_RESETHOTKEY, L"默认",
                        gbX + gbPad + 135 + 185, innerY, 55, btnH);
            innerY += btnH + 8;

            MoveWindow(gbHotkey, gbX, gbY, gbW, innerY - gbY + 4, TRUE);
            SetWindowPos(gbHotkey, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
            y = innerY + 10;
        }

        {
            int gbX = PAGE_X;
            int gbY = y;
            int innerY = y + 24;

            HWND gbMouse = CreateWindowExW(0, L"BUTTON", L"鼠标隐藏",
                                            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                            gbX, gbY, gbW, 100,
                                            hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SetControlFont(gbMouse, font);

            makeChk(IDC_CHK_BOSS_MIDDLE_BTN, L"鼠标中键", gbX + gbPad, innerY, col3W, config.bossKeyMiddleButton);
            makeChk(IDC_CHK_BOSS_SIDE_BTN1, L"鼠标侧键1", gbX + gbPad + col3W + 8, innerY, col3W, config.bossKeySideButton1);
            makeChk(IDC_CHK_BOSS_SIDE_BTN2, L"鼠标侧键2", gbX + gbPad + (col3W + 8) * 2, innerY, col3W, config.bossKeySideButton2);
            innerY += rowH + gap;

            makeChk(IDC_CHK_BOSS_CORNER_TL, L"左上角隐藏", gbX + gbPad, innerY, col3W, config.bossKeyCornerTL);
            makeChk(IDC_CHK_BOSS_CORNER_TR, L"右上角隐藏", gbX + gbPad + col3W + 8, innerY, col3W, config.bossKeyCornerTR);
            innerY += rowH + gap;
            makeChk(IDC_CHK_BOSS_CORNER_BL, L"左下角隐藏", gbX + gbPad, innerY, col3W, config.bossKeyCornerBL);
            makeChk(IDC_CHK_BOSS_CORNER_BR, L"右下角隐藏", gbX + gbPad + col3W + 8, innerY, col3W, config.bossKeyCornerBR);
            innerY += rowH + 8;

            MoveWindow(gbMouse, gbX, gbY, gbW, innerY - gbY + 4, TRUE);
            SetWindowPos(gbMouse, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
            y = innerY + 10;
        }

        {
            int gbX = PAGE_X;
            int gbY = y;
            int innerY = y + 24;

            HWND gbOther = CreateWindowExW(0, L"BUTTON", L"其他选项",
                                            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                            gbX, gbY, gbW, 100,
                                            hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SetControlFont(gbOther, font);

            makeChk(IDC_CHK_BOSSMUTE, L"隐藏时静音", gbX + gbPad, innerY, col3W, config.bossKeyMute);
            makeChk(IDC_CHK_BOSS_HIDE_CURRENT, L"隐藏当前窗口", gbX + gbPad + col3W + 8, innerY, col3W, config.bossKeyHideCurrent);
            makeChk(IDC_CHK_BOSS_SEND_PAUSE, L"暂停媒体", gbX + gbPad + (col3W + 8) * 2, innerY, col3W, config.bossKeySendPause);
            innerY += rowH + gap + 4;
            makeChk(IDC_CHK_BOSS_AUTO_HIDE, L"闲置自动隐藏", gbX + gbPad, innerY, 120, config.bossKeyAutoHide);
            {
                int timeX = gbX + gbPad + 124;
                HWND edtTime = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                                                timeX, innerY, 38, rowH,
                                                hwnd, (HMENU)IDC_EDT_AUTO_HIDE_TIME, GetModuleHandleW(nullptr), nullptr);
                SetControlFont(edtTime, font);

                HWND spin = CreateWindowExW(0, UPDOWN_CLASSW, L"",
                                             WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ARROWKEYS,
                                             timeX + 38, innerY, 18, rowH,
                                             hwnd, (HMENU)IDC_SPIN_AUTO_HIDE_TIME, GetModuleHandleW(nullptr), nullptr);
                SendMessageW(spin, UDM_SETBUDDY, (WPARAM)edtTime, 0);
                SendMessageW(spin, UDM_SETRANGE, 0, MAKELPARAM(120, 1));
                SendMessageW(spin, UDM_SETPOS, 0, config.bossKeyAutoHideTime);

                HWND lblMin = CreateWindowExW(0, L"STATIC", L"分钟",
                                                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                                timeX + 58, innerY, 35, rowH,
                                                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
                SetControlFont(lblMin, font);
            }
            makeChk(IDC_CHK_BOSS_CLOSE_ON_EXIT, L"退出键客时关闭已隐藏程序", gbX + gbPad + col3W + 8, innerY, gbW - gbPad - col3W - 8, config.bossKeyCloseOnExit);
            innerY += rowH + 8;

            MoveWindow(gbOther, gbX, gbY, gbW, innerY - gbY + 4, TRUE);
            SetWindowPos(gbOther, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
            y = innerY + 10;
        }

        makeLabel(L"已绑定的窗口/进程", PAGE_X, y, PAGE_W - 10);
        y += rowH + 8;
        {
            int listH = PAGE_Y + PAGE_H - y - btnH - 12;
            if (listH < 60) listH = 60;
            HWND lstBound = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                             WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                             PAGE_X, y, PAGE_W - 10, listH,
                                             hwnd, (HMENU)IDC_LST_BOUND_WINDOWS, GetModuleHandleW(nullptr), nullptr);
            SetControlFont(lstBound, font);
            ListView_SetExtendedListViewStyle(lstBound, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            ListView_SetBkColor(lstBound, RGB(255, 255, 255));
            ListView_SetTextColor(lstBound, GetSysColor(COLOR_WINDOWTEXT));
            ListView_SetTextBkColor(lstBound, RGB(255, 255, 255));
            {
                int lvW = PAGE_W - 30;
                LVCOLUMNW col = {};
                col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
                col.fmt = LVCFMT_LEFT;
                col.pszText = const_cast<LPWSTR>(L"匹配模式"); col.cx = lvW / 4;
                ListView_InsertColumn(lstBound, 0, &col);
                col.pszText = const_cast<LPWSTR>(L"标题/进程名"); col.cx = lvW * 3 / 4;
                ListView_InsertColumn(lstBound, 1, &col);
            }
            y += listH + 6;
        }
        makeButton(IDC_BTN_ADD_BOSS_BINDING, L"添加绑定", PAGE_X, y, 100, btnH);
        makeButton(IDC_BTN_DELBOSS, L"删除选中项", PAGE_X + 110, y, 100, btnH);
        RefreshBossWindowList(hwnd, config);

        {
            bool enabled = config.bossKeyEnabled;
            static const int bossCtrlIds[] = {
                IDC_BTN_SETHOTKEY, IDC_BTN_RESETHOTKEY,
                IDC_CHK_BOSS_MIDDLE_BTN, IDC_CHK_BOSS_SIDE_BTN1, IDC_CHK_BOSS_SIDE_BTN2,
                IDC_CHK_BOSS_CORNER_TL, IDC_CHK_BOSS_CORNER_TR, IDC_CHK_BOSS_CORNER_BL, IDC_CHK_BOSS_CORNER_BR,
                IDC_CHK_BOSSMUTE, IDC_CHK_BOSS_HIDE_CURRENT, IDC_CHK_BOSS_SEND_PAUSE,
                IDC_CHK_BOSS_AUTO_HIDE, IDC_CHK_BOSS_CLOSE_ON_EXIT,
                IDC_EDT_AUTO_HIDE_TIME, IDC_SPIN_AUTO_HIDE_TIME,
                IDC_LST_BOUND_WINDOWS, IDC_BTN_ADD_BOSS_BINDING, IDC_BTN_DELBOSS
            };
            for (int id : bossCtrlIds) {
                HWND h = GetDlgItem(hwnd, id);
                if (h) EnableWindow(h, enabled);
            }
        }

        break;
    }

    case 5:
        makeChk(IDC_CHK_WINDGUARD, L"启用窗口置顶", PAGE_X, y, 200, config.winDGuardEnabled, btnH);
        y += btnH + 14;

        makeLabel(L"已绑定的窗口/进程", PAGE_X, y, PAGE_W - 10);
        y += lineH + 2;

        {
            int lstH5 = PAGE_Y + PAGE_H - y - btnH - 18 - 120;
            if (lstH5 < 60) lstH5 = 60;
            HWND lv = makeListView(IDC_LST_WINDWINS, PAGE_X, y, PAGE_W - 10, lstH5);
            int lvW = PAGE_W - 30;
            addLvColumn(lv, 0, L"匹配模式", lvW / 4);
            addLvColumn(lv, 1, L"标题/进程名", lvW * 3 / 4);
            y += lstH5 + 6;
        }
        makeButton(IDC_BTN_ADD_WIND_BINDING, L"添加绑定", PAGE_X, y, 100, btnH);
        makeButton(IDC_BTN_DELWIND, L"删除选中项", PAGE_X + 110, y, 100, btnH);
        makeButton(IDC_BTN_FIX_ZORDER, L"修复窗口堆叠", PAGE_X + PAGE_W - 10 - 120, y, 120, btnH);
        y += btnH + 10;

        {
            int descY = PAGE_Y + PAGE_H - 80;
            HWND lblDesc = CreateWindowExW(0, L"STATIC",
                L"功能说明：\n"
                L"1. 该功能主要用于对指定窗口或进程保持置顶激活状态，并禁止被最小化，实现对返回桌面等操作快捷键（如 Win+D）的免疫。\n"
                L"2. 若出现窗口异常置顶的情况，可点击\u201C修复窗口堆叠\u201D按钮，强制取消所有窗口的置顶状态，恢复正常窗口层级。",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                PAGE_X, descY, PAGE_W - 10, 50,
                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(lblDesc, WM_SETFONT, (WPARAM)GetSmallFont(), TRUE);
        }

        RefreshWindWindowList(hwnd, config);

        {
            bool enabled = config.winDGuardEnabled;
            static const int windCtrlIds[] = {
                IDC_LST_WINDWINS, IDC_BTN_ADD_WIND_BINDING, IDC_BTN_DELWIND
            };
            for (int id : windCtrlIds) {
                HWND h = GetDlgItem(hwnd, id);
                if (h) EnableWindow(h, enabled);
            }
        }
        break;
    }

    // 页面控件按逻辑坐标创建，构建完成后统一放大到当前 DPI 的物理尺寸
    ScalePageControls(hwnd);

    InvalidateRect(hwnd, nullptr, TRUE);
}

// ===== 应用设置 =====
// 保存当前页面状态，检查热键冲突，将工作副本写入实际配置
bool SettingsDialog::ApplySettings(HWND hwnd, AppConfig& config) {
    auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    config.tabState = data->currentTab;
    SaveCurrentTabState(hwnd, config);

    // 检查热键冲突
    std::wstring conflicts = PreCheckHotkeyConflicts(data->workingCopy);
    if (!conflicts.empty()) {
        std::wstring msg = L"以下热键存在冲突或被占用：\n\n" + conflicts + L"\n是否仍要应用设置？";
        if (MessageBoxW(hwnd, msg.c_str(), L"热键冲突", MB_YESNO | MB_ICONWARNING) != IDYES) {
            return false;
        }
    }

    // 将工作副本写入实际配置
    *data->config = data->workingCopy;
    data->originalConfig = data->workingCopy; // 更新原始配置快照
    data->applied = true;                     // 标记本次会话已应用过设置
    if (s_applyCallback) s_applyCallback();   // 通知外部模块重新加载配置
    return true;
}

// ===== 刷新禁用按键列表视图 =====
void SettingsDialog::RefreshDisabledKeyList(HWND hwnd, AppConfig& config) {
    HWND lv = GetDlgItem(hwnd, IDC_LST_DISABLED);
    if (!lv) return;
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < config.disabledKeyCodes.size(); i++) {
        std::wstring name = AppConfig::VKToName(config.disabledKeyCodes[i]);
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = (int)i;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(name.c_str());
        ListView_InsertItem(lv, &item);
        std::wstring code = L"0x" + std::to_wstring(config.disabledKeyCodes[i]);
        ListView_SetItemText(lv, (int)i, 1, const_cast<LPWSTR>(code.c_str()));
    }
}

// ===== 刷新窗口隐藏绑定列表视图 =====
void SettingsDialog::RefreshBossWindowList(HWND hwnd, AppConfig& config) {
    HWND lv = GetDlgItem(hwnd, IDC_LST_BOUND_WINDOWS);
    if (!lv) return;
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < config.bossKeyWindows.size(); i++) {
        const auto& w = config.bossKeyWindows[i];
        std::wstring mode = (w.matchMode == MatchMode::Process) ? L"进程" : L"窗口";
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = (int)i;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(mode.c_str());
        ListView_InsertItem(lv, &item);
        std::wstring display = w.DisplayName();
        if (display.length() > 2 && display.substr(0, 3) == L"[P]") {
            display = w.processName;
            if (!w.processPath.empty()) display += L" (" + w.processPath + L")";
        }
        ListView_SetItemText(lv, (int)i, 1, const_cast<LPWSTR>(display.c_str()));
    }
}

// ===== 刷新窗口置顶绑定列表视图 =====
void SettingsDialog::RefreshWindWindowList(HWND hwnd, AppConfig& config) {
    HWND lv = GetDlgItem(hwnd, IDC_LST_WINDWINS);
    if (!lv) return;
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < config.winDGuardWindows.size(); i++) {
        const auto& w = config.winDGuardWindows[i];
        std::wstring mode = (w.matchMode == MatchMode::Process) ? L"进程" : L"窗口";
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = (int)i;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(mode.c_str());
        ListView_InsertItem(lv, &item);
        std::wstring display = w.DisplayName();
        if (display.length() > 2 && display.substr(0, 3) == L"[P]") {
            display = w.processName;
            if (!w.processPath.empty()) display += L" (" + w.processPath + L")";
        }
        ListView_SetItemText(lv, (int)i, 1, const_cast<LPWSTR>(display.c_str()));
    }
}

// ===== 刷新按键映射列表视图 =====
void SettingsDialog::RefreshRemapList(HWND hwnd, AppConfig& config) {
    HWND lv = GetDlgItem(hwnd, IDC_LST_REMAP);
    if (!lv) return;
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < config.keyRemappings.size(); i++) {
        const auto& r = config.keyRemappings[i];
        std::wstring src = AppConfig::VKToName(r.sourceVK);
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = (int)i;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(src.c_str());
        ListView_InsertItem(lv, &item);
        // mods 为 0 时显示单键，否则显示 Ctrl+C 之类的组合键
        std::wstring dst = FormatHotKey(r.mods, (UINT)r.targetVK);
        ListView_SetItemText(lv, (int)i, 1, const_cast<LPWSTR>(dst.c_str()));
    }
}

// ===== 按键捕获对话框相关定义 =====
// 用于捕获用户按下的单个按键（用于禁用按键和按键映射功能）

static const wchar_t* CAPTURE_KEY_CLASS = L"KeySentryCaptureKeyDlg";
static const UINT WM_CAPTURE_KEY_MSG = WM_USER + 300;   // 自定义消息：捕获到按键
static const UINT WM_CAPTURE_CANCEL = WM_USER + 301;     // 自定义消息：取消捕获

// 按键捕获对话框数据
struct CaptureKeyData {
    int capturedVK;     // 捕获到的虚拟键码，0 表示取消
    UINT capturedMods;  // 捕获到的修饰键（仅组合键捕获时有意义）
    bool done;          // 对话框完成标志
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
static bool IsCaptureKeyDialogActive() {
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
    auto* data = reinterpret_cast<CaptureKeyData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CAPTURE_CANCEL:
        // 取消捕获：同时清零修饰键，避免沿用到下一次捕获
        if (data) { data->capturedVK = 0; data->capturedMods = 0; data->done = true; }
        DestroyWindow(hwnd);
        return 0;
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
        // Esc 取消捕获：返回 0（调用方据此视为取消），避免误绑定 Esc 且无退出路径
        if (vk == VK_ESCAPE) {
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
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ===== 显示按键捕获对话框（通用版） =====
// 创建一个置顶的模态对话框，安装低级键盘钩子捕获用户按键
// title: 对话框标题，line1/line2: 提示文字
// outMods/allowMods: 是否捕获组合键；outMods 返回捕获到的修饰键
// 返回捕获到的虚拟键码，0 表示取消
static int ShowCaptureKeyDialogEx(HWND parent, const wchar_t* title, const wchar_t* line1,
                                  const wchar_t* line2, UINT* outMods = nullptr,
                                  bool allowMods = false) {
    if (outMods) *outMods = 0;
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = CaptureKeyWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = CAPTURE_KEY_CLASS;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // 注册失败时不得标记成功：否则窗口类始终未注册，
        // CreateWindowExW 会持续返回空且不再重试（ERROR_CLASS_ALREADY_EXISTS 视为成功）
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 0;
        registered = true;
    }

    RECT rc = { 0, 0, ScaleUi(360), ScaleUi(120) };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

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
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                 (screenW - winW) / 2, (screenH - winH) / 2,
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

    s_captureKeyDlg = hwnd;

    // 通知进入捕获模式
    if (SettingsDialog::s_captureModeCallback) SettingsDialog::s_captureModeCallback(true);
    g_suppressHotkeys = true;  // 捕获期间抑制热键转发

    // 安装低级键盘钩子
    s_captureKeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, CaptureKeyHookProc,
                                          GetModuleHandleW(nullptr), 0);

    // 钩子安装失败时无法捕获按键，直接提示并退出，避免模态循环永久等待
    if (!s_captureKeyHook) {
        MessageBoxW(parent, L"键盘钩子安装失败，无法捕获按键。\n请稍后重试或检查安全软件拦截。",
                    L"提示", MB_OK | MB_ICONWARNING);
        s_captureKeyDlg = nullptr;
        if (SettingsDialog::s_captureModeCallback) SettingsDialog::s_captureModeCallback(false);
        g_suppressHotkeys = (s_inlineCapHook != nullptr);
        if (IsWindow(hwnd)) DestroyWindow(hwnd);
        return 0;
    }

    {
        ModalGuard guard(parent);
        ShowWindow(hwnd, SW_SHOW);
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
    g_suppressHotkeys = (s_inlineCapHook != nullptr);

    if (IsWindow(hwnd)) DestroyWindow(hwnd);

    // 组合键捕获：把累积的修饰键带回调用方（取消时为 0）
    if (outMods) *outMods = (data.capturedVK != 0) ? data.capturedMods : 0;
    return data.capturedVK;
}

// ===== 显示按键捕获对话框（禁用按键专用） =====
static int ShowCaptureKeyDialog(HWND parent) {
    return ShowCaptureKeyDialogEx(parent,
        L"添加禁用按键",
        L"请按下需要禁用的按键...",
        L"(Fn键无法捕获)");
}

// ===== 添加禁用按键 =====
// 弹出按键捕获对话框，捕获用户按下的按键并添加到禁用列表
void SettingsDialog::AddDisabledKey(HWND hwnd, AppConfig& config) {
    int vk = ShowCaptureKeyDialog(hwnd);
    if (vk == 0) return;

    if (std::find(config.disabledKeyCodes.begin(), config.disabledKeyCodes.end(), vk) != config.disabledKeyCodes.end()) {
        MessageBoxW(hwnd, L"该按键已在禁用列表中",
                     L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    config.disabledKeyCodes.push_back(vk);
    RefreshDisabledKeyList(hwnd, config);
}

// ===== 删除禁用按键 =====
// 从禁用按键列表中删除选中的项
void SettingsDialog::RemoveDisabledKey(HWND hwnd, AppConfig& config) {
    HWND lv = GetDlgItem(hwnd, IDC_LST_DISABLED);
    int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (sel < 0) return;
    if (sel < (int)config.disabledKeyCodes.size()) {
        config.disabledKeyCodes.erase(config.disabledKeyCodes.begin() + sel);
        RefreshDisabledKeyList(hwnd, config);
    }
}

// ===== 添加按键映射规则 =====
// 先捕获源按键，再捕获目标按键/组合键，检查循环映射与单键失效风险后添加规则
void SettingsDialog::AddRemapEntry(HWND hwnd, AppConfig& config) {
    int sourceVK = ShowCaptureKeyDialogEx(hwnd,
        L"按键映射 - 源按键",
        L"请按下要映射的源按键...",
        L"(Fn键无法捕获)");
    if (sourceVK == 0) return;

    // 修饰键不能作为源键：拦截后会破坏整个 Ctrl/Shift/Alt/Win
    if (ConfigUtils::IsModifierKey(sourceVK)) {
        MessageBoxW(hwnd, L"修饰键（Ctrl/Shift/Alt/Win）不能作为映射源键，\n否则会导致该修饰键整体失效。",
                     L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    bool exists = false;
    for (const auto& r : config.keyRemappings) {
        if (r.sourceVK == sourceVK) { exists = true; break; }
    }
    if (exists) {
        MessageBoxW(hwnd, L"该源按键已存在映射规则",
                     L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // 目标按键改由虚拟键盘选择：即使目标按键物理损坏也能完成映射；
    // 勾选修饰键后点击主键即可录入 Ctrl+C 之类的组合
    UINT targetMods = 0;
    int targetVK = ShowKeyboardPicker(hwnd, &targetMods, true);
    if (targetVK == 0) return;

    if (sourceVK == targetVK && targetMods == 0) {
        MessageBoxW(hwnd, L"源按键和目标按键不能相同",
                     L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    // 目标主键若本身是其他规则的源键，会形成循环映射
    for (const auto& r : config.keyRemappings) {
        if (r.sourceVK == targetVK) {
            MessageBoxW(hwnd, L"该目标按键已被其他按键映射为源按键，这会导致循环映射问题。\n请先删除相关映射后再试。",
                         L"提示", MB_OK | MB_ICONWARNING);
            return;
        }
    }

    // 组合键映射会让源键本身失效：添加前必须醒目提示，并说明恢复方式
    if (targetMods != 0) {
        std::wstring srcName = AppConfig::VKToName(sourceVK);
        std::wstring dstName = FormatHotKey(targetMods, (UINT)targetVK);
        std::wstring msg = L"单键「" + srcName + L"」将被映射为组合键「" + dstName + L"」。\r\n\r\n"
            L"注意：启用后单独按下「" + srcName + L"」将不再输出原字符，"
            L"而是等同于按下「" + dstName + L"」。\r\n"
            L"如需恢复原按键输入，请在本页删除该映射，或点击「撤销全部组合键」。\r\n\r\n"
            L"是否继续添加？";
        if (MessageBoxW(hwnd, msg.c_str(), L"单键失效提醒",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
            return;
        }
    }

    config.keyRemappings.push_back({ sourceVK, targetMods, targetVK });
    RefreshRemapList(hwnd, config);
}

// ===== 删除按键映射规则 =====
void SettingsDialog::RemoveRemapEntry(HWND hwnd, AppConfig& config) {
    HWND lv = GetDlgItem(hwnd, IDC_LST_REMAP);
    int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (sel < 0) return;
    if (sel < (int)config.keyRemappings.size()) {
        config.keyRemappings.erase(config.keyRemappings.begin() + sel);
        RefreshRemapList(hwnd, config);
    }
}

// ===== 添加绑定对话框相关定义 =====
static const wchar_t* ADD_BINDING_CLASS = L"KeySentryAddBindingDlg";

// 添加绑定对话框数据
struct AddBindingData {
    std::vector<BoundWindowInfo>* allWindowsCache;  // 所有可选窗口缓存
    std::vector<BoundWindowInfo>* boundWindows;      // 已绑定窗口列表
    AppConfig* workingCopy;                           // 工作副本配置
    bool changed;                                     // 绑定是否有变更
};

// ===== 添加绑定对话框窗口过程 =====
// 处理按进程绑定、按窗口绑定、手动添加等操作
static LRESULT CALLBACK AddBindingWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* abd = reinterpret_cast<AddBindingData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        case IDC_BTN_REFRESH_WINS: {
            // 刷新窗口列表
            HWND lst = GetDlgItem(hwnd, IDC_LST_ALL_WINDOWS);
            if (!lst) return 0;
            WindowEnum::EnumWindowCtx ctx{ abd->allWindowsCache, abd->boundWindows, lst };
            WindowEnum::EnumerateUnboundWindows(ctx);
            return 0;
        }
        case IDC_BTN_ADD_PROC_BINDING: {
            // 按进程名绑定：将选中窗口的进程添加到绑定列表
            HWND lst = GetDlgItem(hwnd, IDC_LST_ALL_WINDOWS);
            int sel = (int)SendMessageW(lst, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR || sel >= (int)abd->allWindowsCache->size()) {
                MessageBoxW(hwnd, L"请先从窗口列表中选择一个窗口", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            auto& info = (*abd->allWindowsCache)[sel];
            if (info.processName.empty()) {
                MessageBoxW(hwnd, L"无法获取该窗口的进程名称", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            for (const auto& b : *abd->boundWindows) {
                if (b.matchMode == MatchMode::Process && _wcsicmp(b.processName.c_str(), info.processName.c_str()) == 0) {
                    MessageBoxW(hwnd, L"该进程已在绑定列表中", L"提示", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
            }
            BoundWindowInfo binding;
            binding.matchMode = MatchMode::Process;
            binding.processName = info.processName;
            binding.processPath = info.processPath;
            abd->boundWindows->push_back(binding);
            abd->changed = true;
            MessageBoxW(hwnd, (L"已添加进程绑定: " + info.processName).c_str(), L"添加成功", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        case IDC_BTN_ADD_WIN_BINDING: {
            // 按窗口名绑定：将选中窗口的标题添加到绑定列表
            HWND lst = GetDlgItem(hwnd, IDC_LST_ALL_WINDOWS);
            int sel = (int)SendMessageW(lst, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR || sel >= (int)abd->allWindowsCache->size()) {
                MessageBoxW(hwnd, L"请先从窗口列表中选择一个窗口", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            auto& info = (*abd->allWindowsCache)[sel];
            if (info.title.empty()) {
                MessageBoxW(hwnd, L"该窗口没有标题，无法按窗口名绑定", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            for (const auto& b : *abd->boundWindows) {
                if (b.matchMode == MatchMode::Window && b.title == info.title) {
                    MessageBoxW(hwnd, L"该窗口已在绑定列表中", L"提示", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
            }
            BoundWindowInfo binding;
            binding.matchMode = MatchMode::Window;
            binding.title = info.title;
            binding.processName = info.processName;
            binding.processPath = info.processPath;
            binding.pid = info.pid;
            abd->boundWindows->push_back(binding);
            abd->changed = true;
            MessageBoxW(hwnd, (L"已添加窗口绑定: " + info.title).c_str(), L"添加成功", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        case IDC_BTN_ADDBOSS: {
            // 手动添加：读取编辑框中的窗口标题并添加到绑定列表
            HWND edt = GetDlgItem(hwnd, IDC_EDT_BOSSWIN);
            wchar_t text[512] = {};
            GetWindowTextW(edt, text, 512);
            std::wstring name(text);
            if (name.empty()) return 0;
            for (const auto& w : *abd->boundWindows) {
                if (w.title == name) {
                    MessageBoxW(hwnd, L"该窗口已在列表中", L"提示", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
            }
            BoundWindowInfo binding;
            binding.matchMode = MatchMode::Window;
            binding.title = name;
            abd->boundWindows->push_back(binding);
            abd->changed = true;
            SetWindowTextW(edt, L"");
            MessageBoxW(hwnd, (L"已添加窗口绑定: " + name).c_str(), L"添加成功", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ===== 显示添加绑定对话框 =====
// 创建模态对话框，支持按进程名、窗口名、手动输入三种方式添加绑定
void SettingsDialog::ShowBindingDialog(HWND hwnd, AppConfig& config, std::vector<BoundWindowInfo>& boundWindows, const wchar_t* className) {
    auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!data) return;

    static bool registered1 = false;
    static bool registered2 = false;
    bool* pRegistered = (className == ADD_BINDING_CLASS) ? &registered1 : &registered2;
    if (!*pRegistered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = AddBindingWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = className;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // 注册失败时不得标记成功：否则窗口类始终未注册，
        // CreateWindowExW 会持续返回空且不再重试（ERROR_CLASS_ALREADY_EXISTS 视为成功）
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        *pRegistered = true;
    }

    AddBindingData abd;
    abd.allWindowsCache = &data->allWindowsCache;
    abd.boundWindows = &boundWindows;
    abd.workingCopy = &config;
    abd.changed = false;

    RECT rc = { 0, 0, ScaleUi(520), ScaleUi(520) };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, className,
                                L"添加绑定",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                (screenW - winW) / 2, (screenH - winH) / 2,
                                winW, winH,
                                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    // 窗口创建失败时直接返回，避免以空句柄创建子控件产生孤儿窗口
    if (!dlg) return;
    SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&abd));

    HFONT font = GetDlgFont();
    int y = 12;
    int cx = 14;
    int cw = 480;

    HWND lblAll = CreateWindowExW(0, L"STATIC", L"现有窗口列表:",
                                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   cx, y, 200, 24, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(lblAll, font);

    HWND btnRefresh = CreateWindowExW(0, L"BUTTON", L"刷新",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                       cx + cw - 70, y - 2, 70, 26,
                                       dlg, (HMENU)IDC_BTN_REFRESH_WINS, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(btnRefresh, font);
    y += 28;

    HWND lstAll = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
                                   cx, y, cw, 220,
                                   dlg, (HMENU)IDC_LST_ALL_WINDOWS, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(lstAll, font);
    y += 228;

    HWND lblMode = CreateWindowExW(0, L"STATIC", L"添加绑定方式:",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    cx, y, cw, 24, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(lblMode, font);
    y += 28;

    HWND btnProc = CreateWindowExW(0, L"BUTTON", L"按进程名绑定",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                    cx, y, 160, 30,
                                    dlg, (HMENU)IDC_BTN_ADD_PROC_BINDING, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(btnProc, font);

    HWND btnWin = CreateWindowExW(0, L"BUTTON", L"按窗口名绑定",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   cx + 168, y, 160, 30,
                                   dlg, (HMENU)IDC_BTN_ADD_WIN_BINDING, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(btnWin, font);
    y += 34;

    HWND lblHint1 = CreateWindowExW(0, L"STATIC", L"按进程名绑定：匹配该进程的全部窗口（含后续新建窗口）",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     cx, y, cw, 20, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(lblHint1, font);
    y += 22;

    HWND lblHint2 = CreateWindowExW(0, L"STATIC", L"按窗口名绑定：仅匹配包含该标题的窗口",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     cx, y, cw, 20, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(lblHint2, font);
    y += 30;

    HWND lblManual = CreateWindowExW(0, L"STATIC", L"手动添加窗口标题 (标题包含该文字的所有窗口都会匹配):",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      cx, y, cw, 24, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(lblManual, font);
    y += 28;

    HWND edtManual = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                                      cx, y, cw - 90, 28,
                                      dlg, (HMENU)IDC_EDT_BOSSWIN, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(edtManual, font);

    HWND btnAddManual = CreateWindowExW(0, L"BUTTON", L"添加",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                         cx + cw - 80, y, 80, 28,
                                         dlg, (HMENU)IDC_BTN_ADDBOSS, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(btnAddManual, font);
    y += 38;

    HWND btnCancel = CreateWindowExW(0, L"BUTTON", L"关闭",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      cx + cw - 80, y, 80, 30,
                                      dlg, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(btnCancel, font);

    // 控件按逻辑坐标创建，统一放大到当前 DPI 的物理尺寸
    ScaleChildWindows(dlg, GetUiScale());

    WindowEnum::EnumWindowCtx ctx{ &data->allWindowsCache, &boundWindows, lstAll };
    WindowEnum::EnumerateUnboundWindows(ctx);

    {
        ModalGuard guard(hwnd);
        ShowWindow(dlg, SW_SHOW);
        UpdateWindow(dlg);

        MSG msg;
        while (IsWindow(dlg)) {
            BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
            if (ret <= 0) { if (ret == 0) PostQuitMessage((int)msg.wParam); break; }
            // 按键捕获期间丢弃 WM_HOTKEY，避免按下的组合同时触发已注册热键
            if (msg.message == WM_HOTKEY) {
                if (!g_suppressHotkeys && g_mainWnd && IsWindow(g_mainWnd))
                    SendMessageW(g_mainWnd, msg.message, msg.wParam, msg.lParam);
                continue;
            }
            // 交给对话框管理器：Tab 切换焦点、Esc 自动转成 IDCANCEL，便于纯键盘操作
            if (IsDialogMessageW(dlg, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // 兜底：异常退出路径下销毁窗口，避免 GWLP_USERDATA 指向已失效的栈对象 abd
        if (IsWindow(dlg)) DestroyWindow(dlg);
    }

    // 根据绑定列表类型刷新对应的列表视图
    if (abd.changed) {
        if (&boundWindows == &config.bossKeyWindows) {
            RefreshBossWindowList(hwnd, config);
        } else {
            RefreshWindWindowList(hwnd, config);
        }
    }
}

// ===== 删除窗口隐藏绑定 =====
void SettingsDialog::RemoveBossWindow(HWND hwnd, AppConfig& config) {
    HWND lv = GetDlgItem(hwnd, IDC_LST_BOUND_WINDOWS);
    int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (sel < 0) return;
    if (sel < (int)config.bossKeyWindows.size()) {
        config.bossKeyWindows.erase(config.bossKeyWindows.begin() + sel);
        RefreshBossWindowList(hwnd, config);
    }
}

// ===== 添加窗口置顶绑定 =====
// 弹出绑定对话框，使用窗口置顶专用的窗口类名
void SettingsDialog::AddWindWindow(HWND hwnd, AppConfig& config) {
    ShowBindingDialog(hwnd, config, config.winDGuardWindows, L"KeySentryWindBindingDlg");
}

// ===== 删除窗口置顶绑定 =====
void SettingsDialog::RemoveWindWindow(HWND hwnd, AppConfig& config) {
    HWND lv = GetDlgItem(hwnd, IDC_LST_WINDWINS);
    int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (sel < 0) return;
    if (sel < (int)config.winDGuardWindows.size()) {
        config.winDGuardWindows.erase(config.winDGuardWindows.begin() + sel);
        RefreshWindWindowList(hwnd, config);
    }
}

// ===== 自定义热键对话框相关定义 =====
static const wchar_t* ADD_HOTKEY_CLASS = L"KeySentryAddHotkeyDlg";

// 自定义热键对话框数据
struct AddHotkeyData {
    AppConfig* workingCopy;     // 工作副本配置
    bool capturing;             // 是否正在捕获热键
    UINT captureMod;            // 捕获到的修饰键
    UINT captureVK;             // 捕获到的虚拟键码
    bool confirmed;             // 用户是否确认
    bool isEdit;                // 是否为编辑模式
    int editIndex;              // 编辑模式下对应的配置索引
    bool simMode = false;       // 触发动作模式：true=模拟按键，false=运行命令
    UINT simVK = 0;             // 模拟按键模式下选中的被模拟键
    UINT simMods = 0;           // 模拟按键模式下目标组合的修饰键（0 表示单键）
};

// ===== 切换自定义热键对话框的触发动作模式 =====
// 运行命令模式：命令/程序行显示可用，其余命令相关选项启用
// 模拟按键模式：命令行位置切换为按键选择行，参数/工作目录/窗口状态等选项置灰
static void UpdateAddHotkeyMode(HWND hwnd, bool simMode) {
    auto* ahd = reinterpret_cast<AddHotkeyData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (ahd) ahd->simMode = simMode;

    // 动态标签："命令/程序:" <-> "模拟按键:"
    HWND lbl = GetDlgItem(hwnd, IDC_LBL_HK_CMDLABEL);
    if (lbl) SetWindowTextW(lbl, simMode ? L"模拟按键:" : L"命令/程序:");

    // 命令行控件与模拟行控件同位置互斥显示
    ShowWindow(GetDlgItem(hwnd, IDC_EDT_HK_CMD), simMode ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(hwnd, IDC_BTN_HK_BROWSE), simMode ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(hwnd, IDC_EDT_HK_SIMKEY), simMode ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, IDC_BTN_HK_PICKKEY), simMode ? SW_SHOW : SW_HIDE);

    // 仅命令模式有意义的选项：模拟模式下置灰
    const int cmdOnlyCtrls[] = {
        IDC_EDT_HK_PARAMS, IDC_EDT_HK_WORKDIR, IDC_BTN_HK_WORKDIR_BROWSE,
        IDC_CBO_HK_WINSTATE, IDC_CHK_HK_CONFIRM, IDC_CHK_HK_ADMIN
    };
    for (int id : cmdOnlyCtrls) {
        HWND h = GetDlgItem(hwnd, id);
        if (h) EnableWindow(h, !simMode);
    }
}

// ===== 结束自定义热键对话框的捕获态 =====
// 捕获期间打开"浏览程序/浏览目录/选择按键"等模态界面时必须先调用：
// 吞键钩子会吞掉全部物理按键，这些模态窗口将完全无法接收键盘输入，
// 且其模态循环不会处理 WM_INLINE_CAPTURE_KEY，捕获也无法自行结束
static void StopAddHotkeyCapture(HWND hwnd, AddHotkeyData* ahd) {
    if (!ahd->capturing) return;
    ahd->capturing = false;
    EndInlineCapture(hwnd);
    SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_HK_CAPTURE), L"捕获快捷键");
}

// ===== 自定义热键对话框窗口过程 =====
static LRESULT CALLBACK AddHotkeyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ahd = reinterpret_cast<AddHotkeyData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!ahd) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            // 捕获态下确定：先卸载吞键钩子，避免后续提示框无法用键盘关闭
            StopAddHotkeyCapture(hwnd, ahd);
            // 确定按钮：验证输入并保存自定义热键
            HWND edtKey = GetDlgItem(hwnd, IDC_EDT_HK_KEY);
            wchar_t keyText[64] = {};
            GetWindowTextW(edtKey, keyText, 64);
            if (wcslen(keyText) == 0) {
                MessageBoxW(hwnd, L"请设置热键", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }

            // 主键取值：优先采用文本框当前内容（支持手动修改）。
            // 仅当文本框内容与"捕获到的键名"完全一致时，才允许回退到捕获值——
            // VKToName 借助 GetKeyNameTextW 能产出 "Numpad 0"、"Divide" 这类
            // NameToVK 无法反解的本地化名称，若此时静默回退，会注册出与界面显示不符的键。
            // 因此解析失败且文本框已被改动时直接拒绝，而不是猜一个值
            UINT parsedVK = (UINT)AppConfig::NameToVK(keyText);
            if (parsedVK != 0) {
                ahd->captureVK = parsedVK;
            } else if (ahd->captureVK == 0 ||
                       keyText != AppConfig::VKToName((int)ahd->captureVK)) {
                MessageBoxW(hwnd, L"无法识别该按键名称。\n请点击\"捕获快捷键\"或\"选择按键\"按钮重新设置主键。",
                             L"提示", MB_OK | MB_ICONWARNING);
                return 0;
            }

            // 检查是否至少选择了一个修饰键
            UINT hotkeyMod = 0;
            if (SendMessageW(GetDlgItem(hwnd, IDC_CHK_HK_CTRL), BM_GETCHECK, 0, 0) == BST_CHECKED) hotkeyMod |= MOD_CONTROL;
            if (SendMessageW(GetDlgItem(hwnd, IDC_CHK_HK_SHIFT), BM_GETCHECK, 0, 0) == BST_CHECKED) hotkeyMod |= MOD_SHIFT;
            if (SendMessageW(GetDlgItem(hwnd, IDC_CHK_HK_ALT), BM_GETCHECK, 0, 0) == BST_CHECKED) hotkeyMod |= MOD_ALT;
            if (SendMessageW(GetDlgItem(hwnd, IDC_CHK_HK_WIN), BM_GETCHECK, 0, 0) == BST_CHECKED) hotkeyMod |= MOD_WIN;

            if (hotkeyMod == 0) {
                MessageBoxW(hwnd, L"自定义热键需要至少一个修饰键（Ctrl/Shift/Alt/Win），否则会与正常输入冲突。",
                             L"提示", MB_OK | MB_ICONWARNING);
                return 0;
            }

            HWND edtCmd = GetDlgItem(hwnd, IDC_EDT_HK_CMD);
            std::wstring cmdText = ReadWindowText(edtCmd);
            if (!ahd->simMode && cmdText.empty()) {
                MessageBoxW(hwnd, L"请输入命令或程序路径", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }

            // 模拟按键模式：必须已选择被模拟的按键
            if (ahd->simMode && ahd->simVK == 0) {
                MessageBoxW(hwnd, L"请点击\"选择按键\"按钮设置要模拟的按键", L"提示", MB_OK | MB_ICONWARNING);
                return 0;
            }

            // 构建自定义热键对象
            // 修饰键完全以复选框勾选状态为准（从 0 构建），
            // 用户取消勾选修饰键后保存才能生效（此前 |= 只加不减）
            CustomHotkey hk;
            hk.mod = hotkeyMod;
            hk.vk = ahd->captureVK;

            hk.simulateKey = ahd->simMode;
            hk.targetVK = ahd->simMode ? ahd->simVK : 0;
            hk.targetMod = ahd->simMode ? ahd->simMods : 0;

            hk.command = ahd->simMode ? L"" : cmdText;

            wchar_t paramsText[512] = {};
            GetWindowTextW(GetDlgItem(hwnd, IDC_EDT_HK_PARAMS), paramsText, 512);
            hk.parameters = ahd->simMode ? L"" : paramsText;

            std::wstring workDirText = ReadWindowText(GetDlgItem(hwnd, IDC_EDT_HK_WORKDIR));
            hk.workDir = ahd->simMode ? L"" : workDirText;

            wchar_t descText[256] = {};
            GetWindowTextW(GetDlgItem(hwnd, IDC_EDT_HK_DESC), descText, 256);
            hk.description = descText;

            HWND cboWinState = GetDlgItem(hwnd, IDC_CBO_HK_WINSTATE);
            int wsSel = (int)SendMessageW(cboWinState, CB_GETCURSEL, 0, 0);
            hk.windowState = ahd->simMode ? HotkeyWindowState::Normal : ConfigUtils::ClampWindowState(wsSel);

            hk.confirmBeforeRun = !ahd->simMode && SendMessageW(GetDlgItem(hwnd, IDC_CHK_HK_CONFIRM), BM_GETCHECK, 0, 0) == BST_CHECKED;
            hk.runAsAdmin = !ahd->simMode && SendMessageW(GetDlgItem(hwnd, IDC_CHK_HK_ADMIN), BM_GETCHECK, 0, 0) == BST_CHECKED;

            // 检查热键是否已存在（编辑模式下跳过自身）
            for (size_t i = 0; i < ahd->workingCopy->customHotkeys.size(); i++) {
                if (ahd->workingCopy->customHotkeys[i].mod == hk.mod &&
                    ahd->workingCopy->customHotkeys[i].vk == hk.vk) {
                    if (ahd->isEdit && (int)i == ahd->editIndex) continue;
                    MessageBoxW(hwnd, L"该热键已存在，请使用其他组合", L"提示", MB_OK | MB_ICONWARNING);
                    return 0;
                }
            }

            // 编辑模式：替换原有热键；新增模式：追加到列表
            if (ahd->isEdit && ahd->editIndex >= 0 && ahd->editIndex < (int)ahd->workingCopy->customHotkeys.size()) {
                ahd->workingCopy->customHotkeys[ahd->editIndex] = hk;
            } else {
                // 上限校验：超过注册上限的热键会被静默忽略，提前提示避免用户困惑
                if ((int)ahd->workingCopy->customHotkeys.size() >= MAX_CUSTOM_HOTKEYS) {
                    MessageBoxW(hwnd, L"自定义热键数量已达上限（100 个），无法继续添加。",
                                 L"提示", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                ahd->workingCopy->customHotkeys.push_back(hk);
            }
            ahd->confirmed = true;
            DestroyWindow(hwnd);
            return 0;
        }
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        case IDC_BTN_HK_CAPTURE:
            // 开始捕获热键：安装吞键钩子，避免按键被全局钩子吞掉或触发系统快捷键
            // 钩子安装失败时不进入捕获态，否则界面提示"请按下快捷键"但永远收不到按键
            if (BeginInlineCapture(hwnd)) {
                ahd->capturing = true;
                SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_HK_CAPTURE), L"请按下快捷键...");
            }
            SetFocus(hwnd);
            return 0;
        case IDC_RADIO_HK_CMD:
            UpdateAddHotkeyMode(hwnd, false);
            return 0;
        case IDC_RADIO_HK_SIMKEY:
            UpdateAddHotkeyMode(hwnd, true);
            // 首次切入模拟模式且尚未选键时，直接弹出键盘选择界面
            if (ahd->simVK == 0) {
                // 键盘选择界面需要接收键盘输入，先退出捕获态
                StopAddHotkeyCapture(hwnd, ahd);
                UINT pickedMods = 0;
                int picked = SettingsDialog::ShowKeyboardPicker(hwnd, &pickedMods, true);
                if (picked > 0) {
                    ahd->simVK = (UINT)picked;
                    ahd->simMods = pickedMods;
                    SetWindowTextW(GetDlgItem(hwnd, IDC_EDT_HK_SIMKEY),
                                   SettingsDialog::FormatHotKey(pickedMods, (UINT)picked).c_str());
                }
            }
            return 0;
        case IDC_BTN_HK_PICKKEY: {
            // 重新选择被模拟的按键或组合键（与按键映射一致的虚拟键盘界面）
            StopAddHotkeyCapture(hwnd, ahd);
            UINT pickedMods = 0;
            int picked = SettingsDialog::ShowKeyboardPicker(hwnd, &pickedMods, true);
            if (picked > 0) {
                ahd->simVK = (UINT)picked;
                ahd->simMods = pickedMods;
                SetWindowTextW(GetDlgItem(hwnd, IDC_EDT_HK_SIMKEY),
                               SettingsDialog::FormatHotKey(pickedMods, (UINT)picked).c_str());
            }
            return 0;
        }
        case IDC_BTN_HK_BROWSE: {
            // 浏览选择可执行文件：文件对话框需要键盘输入，先退出捕获态
            StopAddHotkeyCapture(hwnd, ahd);
            OPENFILENAMEW ofn = {};
            // 使用长路径上限缓冲（32768），避免路径超过 MAX_PATH 时被截断
            std::wstring szFile(32768, L'\0');
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFile = &szFile[0];
            ofn.nMaxFile = (DWORD)szFile.size();
            ofn.lpstrFilter = L"可执行文件\0*.exe;*.bat;*.cmd;*.ps1\0所有文件\0*.*\0";
            ofn.nFilterIndex = 1;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                SetWindowTextW(GetDlgItem(hwnd, IDC_EDT_HK_CMD), szFile.c_str());
            }
            return 0;
        }
        case IDC_BTN_HK_WORKDIR_BROWSE: {
            // 浏览选择工作目录：目录选择框需要键盘输入，先退出捕获态
            StopAddHotkeyCapture(hwnd, ahd);
            // 与上面的文件浏览一致使用长路径缓冲：MAX_PATH 下超过 260 字符的
            // 目录会让 SHGetPathFromIDListW 失败，工作目录被静默丢弃
            std::vector<wchar_t> dir(32768, L'\0');
            BROWSEINFOW bi = {};
            bi.hwndOwner = hwnd;
            bi.lpszTitle = L"选择工作目录";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                // 转换失败时不能清空已有工作目录，仅在成功取得路径时写回
                if (SHGetPathFromIDListW(pidl, dir.data())) {
                    SetWindowTextW(GetDlgItem(hwnd, IDC_EDT_HK_WORKDIR), dir.data());
                }
                CoTaskMemFree(pidl);
            }
            return 0;
        }
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, RGB(255, 255, 255));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ===== 显示自定义热键编辑对话框 =====
// editIndex=-1 为新增模式，否则为编辑模式
void SettingsDialog::ShowHotkeyDialog(HWND hwnd, AppConfig& config, int editIndex) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = AddHotkeyWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = ADD_HOTKEY_CLASS;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // 注册失败时不得标记成功：否则窗口类始终未注册，
        // CreateWindowExW 会持续返回空且不再重试（ERROR_CLASS_ALREADY_EXISTS 视为成功）
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        registered = true;
    }

    bool isEdit = editIndex >= 0;
    AddHotkeyData ahd;
    ahd.workingCopy = &config;
    ahd.capturing = false;
    ahd.captureMod = 0;
    ahd.captureVK = 0;
    ahd.confirmed = false;
    ahd.isEdit = isEdit;
    ahd.editIndex = editIndex;

    if (isEdit) {
        const auto& existing = config.customHotkeys[editIndex];
        ahd.captureMod = existing.mod;
        ahd.captureVK = existing.vk;
        ahd.simMode = existing.simulateKey;
        ahd.simVK = existing.targetVK;
        ahd.simMods = existing.targetMod;
    }

    RECT rc = { 0, 0, ScaleUi(480), ScaleUi(440) };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, ADD_HOTKEY_CLASS,
                                isEdit ? L"编辑自定义热键" : L"新增自定义热键",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                (screenW - winW) / 2, (screenH - winH) / 2,
                                winW, winH,
                                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    // 窗口创建失败时直接返回，避免以空句柄创建子控件产生孤儿窗口
    if (!dlg) return;
    SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&ahd));

    HFONT font = GetDlgFont();
    int y = 14;
    int cx = 14;
    int cw = 440;
    int lineH = 22;
    int btnH = 32;
    int labelW = 80;
    int editX = cx + labelW + 5;

    auto dlgLabel = [&](const wchar_t* text, int px, int py, int w, int h = 0) {
        HWND h2 = CreateWindowExW(0, L"STATIC", text,
                                   WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                   px, py, w, h > 0 ? h : lineH, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(h2, font);
    };

    auto dlgEdit = [&](int id, int px, int py, int w) {
        HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                                   px, py, w, lineH,
                                   dlg, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(h, font);
    };

    auto dlgChk = [&](int id, const wchar_t* text, int px, int py, int w) {
        HWND h = CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                   px, py, w, lineH,
                                   dlg, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(h, font);
    };

    auto dlgButton = [&](int id, const wchar_t* text, int px, int py, int w, int h2) {
        HWND h = CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   px, py, w, h2,
                                   dlg, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(h, font);
    };

    // 单选按钮（用于触发动作模式选择），group 参数标记单选组起始
    auto dlgRadio = [&](int id, const wchar_t* text, int px, int py, int w, bool group) {
        HWND h = CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | (group ? WS_GROUP : 0),
                                   px, py, w, lineH,
                                   dlg, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(h, font);
    };

    dlgLabel(L"修饰键:", cx, y, labelW, btnH);
    dlgChk(IDC_CHK_HK_CTRL, L"Ctrl", editX, y, 55);
    dlgChk(IDC_CHK_HK_SHIFT, L"Shift", editX + 60, y, 60);
    dlgChk(IDC_CHK_HK_ALT, L"Alt", editX + 125, y, 50);
    dlgChk(IDC_CHK_HK_WIN, L"Win", editX + 180, y, 50);

    if (isEdit) {
        const auto& existing = config.customHotkeys[editIndex];
        SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_CTRL), BM_SETCHECK, (existing.mod & MOD_CONTROL) ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_SHIFT), BM_SETCHECK, (existing.mod & MOD_SHIFT) ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_ALT), BM_SETCHECK, (existing.mod & MOD_ALT) ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_WIN), BM_SETCHECK, (existing.mod & MOD_WIN) ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    y += btnH + 6;

    dlgLabel(L"主键:", cx, y, labelW, btnH);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                     editX, y, 100, btnH,
                     dlg, (HMENU)(LONG_PTR)IDC_EDT_HK_KEY, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(GetDlgItem(dlg, IDC_EDT_HK_KEY), font);
    dlgButton(IDC_BTN_HK_CAPTURE, L"捕获快捷键", editX + 108, y, 115, btnH);

    if (isEdit) {
        SetWindowTextW(GetDlgItem(dlg, IDC_EDT_HK_KEY), AppConfig::VKToName(config.customHotkeys[editIndex].vk).c_str());
    }
    y += btnH + 6;

    // 触发动作：运行命令（默认）/ 模拟按键
    dlgLabel(L"触发动作:", cx, y, labelW, btnH);
    dlgRadio(IDC_RADIO_HK_CMD, L"运行命令", editX, y + 5, 90, true);
    dlgRadio(IDC_RADIO_HK_SIMKEY, L"模拟按键", editX + 100, y + 5, 90, false);
    if (isEdit) {
        const auto& existing = config.customHotkeys[editIndex];
        SendMessageW(GetDlgItem(dlg, IDC_RADIO_HK_CMD), BM_SETCHECK, existing.simulateKey ? BST_UNCHECKED : BST_CHECKED, 0);
        SendMessageW(GetDlgItem(dlg, IDC_RADIO_HK_SIMKEY), BM_SETCHECK, existing.simulateKey ? BST_CHECKED : BST_UNCHECKED, 0);
    } else {
        SendMessageW(GetDlgItem(dlg, IDC_RADIO_HK_CMD), BM_SETCHECK, BST_CHECKED, 0);
    }
    y += btnH + 6;

    // 命令/程序行 与 模拟按键选择行同位置，按触发动作模式互斥显示
    {
        HWND lblCmd = CreateWindowExW(0, L"STATIC", L"命令/程序:",
                                       WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                       cx, y, labelW, btnH,
                                       dlg, (HMENU)(LONG_PTR)IDC_LBL_HK_CMDLABEL, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(lblCmd, font);
    }
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                     editX, y, cw - labelW - 5 - 80, btnH,
                     dlg, (HMENU)(LONG_PTR)IDC_EDT_HK_CMD, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(GetDlgItem(dlg, IDC_EDT_HK_CMD), font);
    dlgButton(IDC_BTN_HK_BROWSE, L"浏览", cx + cw - 75, y, 75, btnH);

    // 模拟按键行：只读编辑框显示选中按键 + 选择按键按钮（初始不显示，由模式切换控制）
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                     WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | ES_READONLY,
                     editX, y, 150, btnH,
                     dlg, (HMENU)(LONG_PTR)IDC_EDT_HK_SIMKEY, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(GetDlgItem(dlg, IDC_EDT_HK_SIMKEY), font);
    dlgButton(IDC_BTN_HK_PICKKEY, L"选择按键", editX + 158, y, 115, btnH);

    if (isEdit) {
        const auto& existing = config.customHotkeys[editIndex];
        SetWindowTextW(GetDlgItem(dlg, IDC_EDT_HK_CMD), existing.command.c_str());
        if (existing.simulateKey && existing.targetVK != 0) {
            SetWindowTextW(GetDlgItem(dlg, IDC_EDT_HK_SIMKEY),
                           FormatHotKey(existing.targetMod, existing.targetVK).c_str());
        }
    }
    // 按当前模式（新增默认命令模式，编辑恢复原模式）应用控件显示/启用状态
    UpdateAddHotkeyMode(dlg, ahd.simMode);
    y += btnH + 6;

    dlgLabel(L"运行参数:", cx, y, labelW, btnH);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                     editX, y, cw - labelW - 5, btnH,
                     dlg, (HMENU)(LONG_PTR)IDC_EDT_HK_PARAMS, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(GetDlgItem(dlg, IDC_EDT_HK_PARAMS), font);

    if (isEdit) {
        SetWindowTextW(GetDlgItem(dlg, IDC_EDT_HK_PARAMS), config.customHotkeys[editIndex].parameters.c_str());
    }
    y += btnH + 6;

    dlgLabel(L"工作目录:", cx, y, labelW, btnH);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                     editX, y, cw - labelW - 5 - 80, btnH,
                     dlg, (HMENU)(LONG_PTR)IDC_EDT_HK_WORKDIR, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(GetDlgItem(dlg, IDC_EDT_HK_WORKDIR), font);
    dlgButton(IDC_BTN_HK_WORKDIR_BROWSE, L"浏览", cx + cw - 75, y, 75, btnH);

    if (isEdit) {
        SetWindowTextW(GetDlgItem(dlg, IDC_EDT_HK_WORKDIR), config.customHotkeys[editIndex].workDir.c_str());
    }
    y += btnH + 6;

    dlgLabel(L"描述:", cx, y, labelW, btnH);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                     editX, y, cw - labelW - 5, btnH,
                     dlg, (HMENU)(LONG_PTR)IDC_EDT_HK_DESC, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(GetDlgItem(dlg, IDC_EDT_HK_DESC), font);

    if (isEdit) {
        SetWindowTextW(GetDlgItem(dlg, IDC_EDT_HK_DESC), config.customHotkeys[editIndex].description.c_str());
    }
    y += btnH + 6;

    dlgLabel(L"窗口状态:", cx, y, labelW, btnH);
    HWND cboWinState = CreateWindowExW(0, L"COMBOBOX", L"",
                                        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                        editX, y, 100, 200,
                                        dlg, (HMENU)IDC_CBO_HK_WINSTATE, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(cboWinState, font);
    SendMessageW(cboWinState, CB_ADDSTRING, 0, (LPARAM)L"正常");
    SendMessageW(cboWinState, CB_ADDSTRING, 0, (LPARAM)L"最小化");
    SendMessageW(cboWinState, CB_ADDSTRING, 0, (LPARAM)L"最大化");
    SendMessageW(cboWinState, CB_ADDSTRING, 0, (LPARAM)L"隐藏");
    SendMessageW(cboWinState, CB_SETCURSEL, isEdit ? (int)config.customHotkeys[editIndex].windowState : 0, 0);

    dlgChk(IDC_CHK_HK_CONFIRM, L"确认后执行", editX + 108, y, 100);
    dlgChk(IDC_CHK_HK_ADMIN, L"以管理员运行", editX + 215, y, 130);

    if (isEdit) {
        SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_CONFIRM), BM_SETCHECK, config.customHotkeys[editIndex].confirmBeforeRun ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_ADMIN), BM_SETCHECK, config.customHotkeys[editIndex].runAsAdmin ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    y += lineH + 20;

    dlgButton(IDOK, L"确定", cx + cw - 180, y, 80, 30);
    dlgButton(IDCANCEL, L"取消", cx + cw - 90, y, 80, 30);

    // 控件按逻辑坐标创建，统一放大到当前 DPI 的物理尺寸
    ScaleChildWindows(dlg, GetUiScale());

    {
        ModalGuard guard(hwnd);
        ShowWindow(dlg, SW_SHOW);
        UpdateWindow(dlg);

        MSG msg;
        while (IsWindow(dlg)) {
            BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
            if (ret <= 0) { if (ret == 0) PostQuitMessage((int)msg.wParam); break; }
            // 捕获的按键由吞键钩子以 WM_INLINE_CAPTURE_KEY 投递（lParam=修饰键组合）
            if (msg.message == WM_INLINE_CAPTURE_KEY && ahd.capturing) {
                UINT mod = (UINT)msg.lParam;
                int vk = (int)msg.wParam;
                // 纯修饰键：继续等待主键
                if (InlineCapIsModifier(vk)) continue;
                if (vk == VK_ESCAPE) {
                    // Esc 取消捕获：复位捕获态并卸载吞键钩子，恢复按钮文本
                    ahd.capturing = false;
                    EndInlineCapture(dlg);
                    SetWindowTextW(GetDlgItem(dlg, IDC_BTN_HK_CAPTURE), L"捕获快捷键");
                    continue;
                }
                if (mod == 0) {
                    // 拒绝无修饰键的裸键：保存时本也会被拒，这里即时提示避免误以为已捕获
                    SetWindowTextW(GetDlgItem(dlg, IDC_BTN_HK_CAPTURE), L"请使用 修饰键+按键...");
                    continue;
                }
                ahd.captureMod = mod;
                ahd.captureVK = (UINT)vk;
                ahd.capturing = false;
                // 卸载吞键钩子：恢复键盘钩子对按键的拦截
                EndInlineCapture(dlg);
                SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_CTRL), BM_SETCHECK, (mod & MOD_CONTROL) ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_SHIFT), BM_SETCHECK, (mod & MOD_SHIFT) ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_ALT), BM_SETCHECK, (mod & MOD_ALT) ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_WIN), BM_SETCHECK, (mod & MOD_WIN) ? BST_CHECKED : BST_UNCHECKED, 0);
                SetWindowTextW(GetDlgItem(dlg, IDC_EDT_HK_KEY), AppConfig::VKToName(vk).c_str());
                SetWindowTextW(GetDlgItem(dlg, IDC_BTN_HK_CAPTURE), L"捕获快捷键");
                continue;
            }
            // 捕获热键期间丢弃 WM_HOTKEY：按下的组合若正好是已注册热键，
            // 转发会触发热键动作（如老板键隐藏窗口）打断捕获
            if (msg.message == WM_HOTKEY) {
                if (!ahd.capturing && g_mainWnd && IsWindow(g_mainWnd))
                    SendMessageW(g_mainWnd, msg.message, msg.wParam, msg.lParam);
                continue;
            }
            // 交给对话框管理器：Tab 切换焦点、Esc 自动转成 IDCANCEL，便于纯键盘操作
            if (IsDialogMessageW(dlg, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // 兜底：捕获过程中对话框被关闭时卸载吞键钩子，避免按键被长期吞掉；
        // EndInlineCapture 内部按归属判断，不会误拆其他对话框正在使用的钩子
        if (ahd.capturing || s_inlineCapWnd == dlg) {
            ahd.capturing = false;
            EndInlineCapture(dlg);
        }

        // 兜底：异常退出路径下销毁窗口，避免 GWLP_USERDATA 指向已失效的栈对象 ahd
        if (IsWindow(dlg)) DestroyWindow(dlg);
    }
}

// ===== 新增自定义热键 =====
void SettingsDialog::AddCustomHotkey(HWND hwnd, AppConfig& config) {
    ShowHotkeyDialog(hwnd, config);
    RefreshCustomHotkeyList(hwnd, config);
}

// ===== 编辑选中的自定义热键 =====
void SettingsDialog::EditCustomHotkey(HWND hwnd, AppConfig& config) {
    HWND lv = GetDlgItem(hwnd, IDC_LV_CUSTOMHK);
    if (!lv) return;
    int sel = (int)SendMessageW(lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (sel < 0) {
        MessageBoxW(hwnd, L"请先选择要编辑的热键", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    LVITEMW item = {};
    item.mask = LVIF_PARAM;
    item.iItem = sel;
    SendMessageW(lv, LVM_GETITEMW, 0, (LPARAM)&item);
    int configIdx = (int)item.lParam;
    if (configIdx < 0 || configIdx >= (int)config.customHotkeys.size()) {
        MessageBoxW(hwnd, L"该热键无法编辑", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    ShowHotkeyDialog(hwnd, config, configIdx);
    RefreshCustomHotkeyList(hwnd, config);
}

// ===== 删除选中的自定义热键 =====
void SettingsDialog::DeleteCustomHotkey(HWND hwnd, AppConfig& config) {
    HWND lv = GetDlgItem(hwnd, IDC_LV_CUSTOMHK);
    if (!lv) return;
    int sel = (int)SendMessageW(lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (sel < 0) {
        MessageBoxW(hwnd, L"请先选择要删除的热键", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    LVITEMW item = {};
    item.mask = LVIF_PARAM;
    item.iItem = sel;
    SendMessageW(lv, LVM_GETITEMW, 0, (LPARAM)&item);
    int configIdx = (int)item.lParam;
    if (configIdx < 0 || configIdx >= (int)config.customHotkeys.size()) {
        MessageBoxW(hwnd, L"该热键无法删除", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    config.customHotkeys.erase(config.customHotkeys.begin() + configIdx);
    RefreshCustomHotkeyList(hwnd, config);
}

// ===== 刷新自定义热键列表视图 =====
void SettingsDialog::RefreshCustomHotkeyList(HWND hwnd, AppConfig& config) {
    HWND lv = GetDlgItem(hwnd, IDC_LV_CUSTOMHK);
    if (!lv) return;
    SendMessageW(lv, LVM_DELETEALLITEMS, 0, 0);
    int idx = 0;
    for (size_t i = 0; i < config.customHotkeys.size(); i++) {
        const auto& hk = config.customHotkeys[i];
        if (hk.mod == 0 && hk.vk == 0) continue;
        // mod/vk 任一为 0 的条目不会被注册（ApplyConfig 按 || 跳过），
        // 明确标注为无效，避免列表看起来正常却始终不生效；仍保留行以便选中删除
        std::wstring keyName = (hk.mod == 0 || hk.vk == 0)
            ? L"（无效）" : FormatHotKey(hk.mod, hk.vk);
        // 模拟按键模式显示被模拟的按键/组合键，命令模式显示命令路径
        std::wstring cmd = hk.simulateKey
            ? (L"模拟按键: " + FormatHotKey(hk.targetMod, hk.targetVK))
            : (hk.command.empty() ? L"" : hk.command);
        std::wstring desc = hk.description.empty() ? L"" : hk.description;

        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = idx;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(keyName.c_str());
        item.lParam = (LPARAM)i;
        SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&item);

        item.mask = LVIF_TEXT;
        item.iSubItem = 1;
        item.pszText = const_cast<LPWSTR>(cmd.c_str());
        SendMessageW(lv, LVM_SETITEMW, 0, (LPARAM)&item);

        item.iSubItem = 2;
        item.pszText = const_cast<LPWSTR>(desc.c_str());
        SendMessageW(lv, LVM_SETITEMW, 0, (LPARAM)&item);

        idx++;
    }
}

// ===== 开始捕获窗口隐藏热键 =====
void SettingsDialog::StartHotKeyCapture(HWND hwnd) {
    auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    // 先安装吞键钩子：失败时不进入捕获态（失败提示已在内部弹出）
    if (!BeginInlineCapture(hwnd)) return;
    // 两项内联捕获互斥：开始本项前先取消另一项，
    // 避免两个标志同时为真时退出其一就错误地提前结束捕获模式
    if (data->capturingCloseHotKey) {
        data->capturingCloseHotKey = false;
        SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_SETCLOSEHOTKEY),
                       FormatHotKey(data->workingCopy.bossKeyCloseMod, data->workingCopy.bossKeyCloseVK).c_str());
    }
    data->capturingHotKey = true;
    SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_SETHOTKEY), L"请按下快捷键...");
    SetFocus(hwnd);
}

// ===== 开始捕获一键关闭程序热键 =====
void SettingsDialog::StartCloseHotKeyCapture(HWND hwnd) {
    auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    // 先安装吞键钩子：失败时不进入捕获态（失败提示已在内部弹出）
    if (!BeginInlineCapture(hwnd)) return;
    // 两项内联捕获互斥：开始本项前先取消另一项（对称处理，见 StartHotKeyCapture）
    if (data->capturingHotKey) {
        data->capturingHotKey = false;
        SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_SETHOTKEY),
                       FormatHotKey(data->workingCopy.bossKeyMod, data->workingCopy.bossKeyVK).c_str());
    }
    data->capturingCloseHotKey = true;
    SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_SETCLOSEHOTKEY), L"请按下快捷键...");
    SetFocus(hwnd);
}

// ===== 重置窗口隐藏热键为默认值（Win+`） =====
void SettingsDialog::ResetHotKey(HWND hwnd) {
    auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    // 若该项正处于捕获中，先取消捕获：否则标志与吞键钩子残留，
    // 按钮文本已被改成默认值却仍提示按键，下一次按键会静默覆盖重置结果
    CancelInlineCapture(data, hwnd);
    data->workingCopy.bossKeyMod = MOD_WIN;
    data->workingCopy.bossKeyVK = 0xC0;
    SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_SETHOTKEY),
                   FormatHotKey(MOD_WIN, 0xC0).c_str());
}

// ===== 重置一键关闭程序热键为默认值（Win+Esc） =====
void SettingsDialog::ResetCloseHotKey(HWND hwnd) {
    auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    // 与 ResetHotKey 对称处理，避免捕获态残留
    CancelInlineCapture(data, hwnd);
    data->workingCopy.bossKeyCloseMod = MOD_WIN;
    data->workingCopy.bossKeyCloseVK = VK_ESCAPE;
    SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_SETCLOSEHOTKEY),
                   FormatHotKey(MOD_WIN, VK_ESCAPE).c_str());
}

// ===== 设置对话框主窗口过程 =====
// 处理所有控件事件、选项卡切换、右键菜单等消息
static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<SettingsDialog::DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!data) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            // 确定：应用设置并关闭对话框（冲突被拒绝时不关闭，允许用户修改）
            // 应用过程可能弹出热键冲突询问框，必须先取消内联捕获，否则提示框键盘失灵
            CancelInlineCapture(data, hwnd);
            if (SettingsDialog::ApplySettings(hwnd, data->workingCopy))
                DestroyWindow(hwnd);
            return 0;
        case IDCANCEL:
            // 取消：恢复原始配置，不保存、不生效，直接关闭对话框
            *data->config = data->originalConfig;
            DestroyWindow(hwnd);
            return 0;
        case IDAPPLY:
            // 应用：仅应用设置，不关闭对话框（同样可能弹出冲突提示框）
            CancelInlineCapture(data, hwnd);
            SettingsDialog::ApplySettings(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_ABOUT:
            // 关于按钮：打开模态子对话框前先取消进行中的内联捕获，
            // 否则子对话框模态循环会吞掉 WM_INLINE_CAPTURE_KEY，捕获永不完成且吞键钩子持续全系统吞键
            CancelInlineCapture(data, hwnd);
            ShowAboutDialog(hwnd);
            return 0;

        case IDC_CHK_DISABLEKEYS: {
            // 禁用自定义按键复选框
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.disableSpecifiedKeysEnabled = checked;
                SettingsDialog::UpdateDisableKeysState(hwnd, checked);
            }
            return 0;
        }

        case IDC_CHK_BOSSKEY: {
            // 启用窗口隐藏复选框：切换所有相关控件的启用状态
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.bossKeyEnabled = checked;
                static const int bossCtrlIds[] = {
                    IDC_BTN_SETHOTKEY, IDC_BTN_RESETHOTKEY,
                    IDC_CHK_BOSS_MIDDLE_BTN, IDC_CHK_BOSS_SIDE_BTN1, IDC_CHK_BOSS_SIDE_BTN2,
                    IDC_CHK_BOSS_CORNER_TL, IDC_CHK_BOSS_CORNER_TR, IDC_CHK_BOSS_CORNER_BL, IDC_CHK_BOSS_CORNER_BR,
                    IDC_CHK_BOSSMUTE, IDC_CHK_BOSS_HIDE_CURRENT, IDC_CHK_BOSS_SEND_PAUSE,
                    IDC_CHK_BOSS_AUTO_HIDE, IDC_CHK_BOSS_CLOSE_ON_EXIT,
                    IDC_EDT_AUTO_HIDE_TIME, IDC_SPIN_AUTO_HIDE_TIME,
                    IDC_LST_BOUND_WINDOWS, IDC_BTN_ADD_BOSS_BINDING, IDC_BTN_DELBOSS
                };
                for (int id : bossCtrlIds) {
                    HWND h = GetDlgItem(hwnd, id);
                    if (h) EnableWindow(h, checked);
                }
            }
            return 0;
        }

        case IDC_CHK_KEYREMAP: {
            // 启用按键映射复选框
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.keyRemapEnabled = checked;
                SettingsDialog::UpdateRemapState(hwnd, checked);
            }
            return 0;
        }

        case IDC_CHK_DISABLENUM: {
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.disableNumLock = checked;
            }
            return 0;
        }

        case IDC_CHK_INSERT: {
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.forceInsertMode = checked;
            }
            return 0;
        }

        case IDC_CHK_FORCENUMLOCK: {
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.forceNumLockOn = checked;
            }
            return 0;
        }

        case IDC_CHK_DISABLE_STARTUP_NOTIFY: {
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.disableStartupNotification = checked;
            }
            return 0;
        }

        case IDC_CHK_OVERLAY: {
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.showLockKeyOverlay = checked;
            }
            return 0;
        }

        case IDC_CHK_MUTE_OVERLAY: {
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.showMuteOverlay = checked;
            }
            return 0;
        }

        case IDC_CHK_PRIMARY_ONLY: {
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.overlayPrimaryOnly = checked;
            }
            return 0;
        }

        case IDC_CHK_AUTOSTART: {
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.autoStart = checked;
            }
            return 0;
        }

        case IDC_CHK_WINDGUARD: {
            // 启用窗口置顶复选框：切换相关控件的启用状态
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.winDGuardEnabled = checked;
                static const int windCtrlIds[] = {
                    IDC_LST_WINDWINS, IDC_BTN_ADD_WIND_BINDING, IDC_BTN_DELWIND
                };
                for (int id : windCtrlIds) {
                    HWND h = GetDlgItem(hwnd, id);
                    if (h) EnableWindow(h, checked);
                }
            }
            return 0;
        }

        case IDC_BTN_FIX_ZORDER: {
            // 修复窗口堆叠：只恢复键客自己记录过的窗口
            // 确认框/结果框需要键盘操作，先取消内联捕获避免吞键钩子让弹窗键盘失灵
            CancelInlineCapture(data, hwnd);
            int confirm = MessageBoxW(hwnd,
                L"此功能用于修复 Win+D 桌面保护引发的窗口 Z 轴错乱及异常置顶问题。\n请确认是否运行修复？",
                L"修复窗口堆叠",
                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
            if (confirm != IDYES) return 0;

            // 仅依据键客的窗口样式记录（KeySentry.wguard）恢复窗口，不再枚举
            // 全系统置顶窗口统一取消置顶：否则会误伤其他程序主动置顶的窗口
            //（如播放器小窗、聊天工具置顶），那些置顶并非键客造成
            // 仅在窗口置顶守护当前未启用时执行：启用状态下记录文件属于正在运行的
            // 本实例，恢复会临时撤销正在生效的保护（置顶 / 禁止最小化）。
            // removeFile=false：保留记录文件，不影响运行中实例的崩溃恢复
            // 判断依据必须是"当前已生效的配置"（data->config）而非未保存的 workingCopy：
            // 若用户勾选了正在编辑但尚未应用的 winDGuardEnabled，此时守护尚未运行，
            // 却会因 workingCopy 为 true 而跳过恢复，导致残留样式无法修复
            int repaired = 0;
            if (!data->config->winDGuardEnabled) {
                repaired = WinDGuard::RecoverOrphanedStyles(false);
            }

            wchar_t buf[160] = {};
            if (repaired > 0) {
                swprintf_s(buf, L"已修复 %d 个窗口的置顶/最小化按钮状态", repaired);
            } else {
                swprintf_s(buf, L"未发现由键客造成异常的窗口");
            }
            MessageBoxW(hwnd, buf, L"修复完成", MB_OK | MB_ICONINFORMATION);
            return 0;
        }

        case IDC_CHK_BOSSMUTE: {
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.bossKeyMute = checked;
            }
            return 0;
        }

        case IDC_CHK_BOSS_HIDE_CURRENT: {
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.bossKeyHideCurrent = checked;
            }
            return 0;
        }

        case IDC_CHK_BOSS_SEND_PAUSE: {
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.bossKeySendPause = checked;
            }
            return 0;
        }

        case IDC_CHK_BOSS_MIDDLE_BTN: {
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.bossKeyMiddleButton = checked;
            }
            return 0;
        }

        case IDC_CHK_BOSS_SIDE_BTN1: {
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.bossKeySideButton1 = checked;
            }
            return 0;
        }

        case IDC_CHK_BOSS_SIDE_BTN2: {
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.bossKeySideButton2 = checked;
            }
            return 0;
        }

        case IDC_CHK_BOSS_CORNER_TL: {
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.bossKeyCornerTL = checked;
            }
            return 0;
        }

        case IDC_CHK_BOSS_CORNER_TR: {
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.bossKeyCornerTR = checked;
            }
            return 0;
        }

        case IDC_CHK_BOSS_CORNER_BL: {
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.bossKeyCornerBL = checked;
            }
            return 0;
        }

        case IDC_CHK_BOSS_CORNER_BR: {
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.bossKeyCornerBR = checked;
            }
            return 0;
        }

        case IDC_CHK_BOSS_AUTO_HIDE: {
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.bossKeyAutoHide = checked;
            }
            return 0;
        }

        case IDC_CHK_BOSS_CLOSE_ON_EXIT: {
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.bossKeyCloseOnExit = checked;
            }
            return 0;
        }

        case IDC_BTN_ADDKEY:
            // 打开按键捕获框（自带吞键钩子 + 后续提示框）前先退出内联捕获
            CancelInlineCapture(data, hwnd);
            SettingsDialog::AddDisabledKey(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_DELKEY:
            CancelInlineCapture(data, hwnd);
            SettingsDialog::RemoveDisabledKey(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_ADDREMAP:
            // 映射项对话框内含按键捕获与模态提示框，必须先取消内联捕获
            CancelInlineCapture(data, hwnd);
            SettingsDialog::AddRemapEntry(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_DELREMAP:
            CancelInlineCapture(data, hwnd);
            SettingsDialog::RemoveRemapEntry(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_CLEAR_COMBOREMAP: {
            // 一键恢复方案：删除所有组合键映射，保留单键映射
            CancelInlineCapture(data, hwnd);
            auto& remaps = data->workingCopy.keyRemappings;
            int comboCount = 0;
            for (const auto& r : remaps) {
                if (r.mods != 0) comboCount++;
            }
            if (comboCount == 0) {
                MessageBoxW(hwnd, L"当前没有组合键映射。", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            wchar_t confirmMsg[256] = {};
            swprintf_s(confirmMsg, L"确定要撤销全部 %d 条组合键映射吗？\r\n\r\n"
                             L"撤销后这些源按键将恢复正常输入；单键映射不受影响。",
                       comboCount);
            if (MessageBoxW(hwnd, confirmMsg, L"撤销全部组合键",
                            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
                return 0;
            }
            std::vector<KeyRemapEntry> kept;
            kept.reserve(remaps.size());
            for (const auto& r : remaps) {
                if (r.mods == 0) kept.push_back(r);
            }
            remaps.swap(kept);
            SettingsDialog::RefreshRemapList(hwnd, data->workingCopy);
            return 0;
        }
        case IDC_BTN_DELBOSS:
            SettingsDialog::RemoveBossWindow(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_ADD_BOSS_BINDING:
            // 打开模态子对话框前取消内联捕获，避免子循环吞掉 WM_INLINE_CAPTURE_KEY 导致捕获卡死
            CancelInlineCapture(data, hwnd);
            SettingsDialog::ShowBindingDialog(hwnd, data->workingCopy, data->workingCopy.bossKeyWindows, ADD_BINDING_CLASS);
            return 0;
        case IDC_BTN_ADD_WIND_BINDING:
            // 与"添加绑定"保持一致：打开模态子对话框前取消内联捕获
            CancelInlineCapture(data, hwnd);
            SettingsDialog::AddWindWindow(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_DELWIND:
            SettingsDialog::RemoveWindWindow(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_SETHOTKEY:
            SettingsDialog::StartHotKeyCapture(hwnd);
            return 0;
        case IDC_BTN_RESETHOTKEY:
            SettingsDialog::ResetHotKey(hwnd);
            return 0;
        case IDC_BTN_SETCLOSEHOTKEY:
            SettingsDialog::StartCloseHotKeyCapture(hwnd);
            return 0;
        case IDC_BTN_RESETCLOSEHOTKEY:
            SettingsDialog::ResetCloseHotKey(hwnd);
            return 0;
        case IDC_CHK_CUSTOM_HK_ENABLE: {
            // 启用自定义热键复选框：切换相关控件的启用状态
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.customHotkeysEnabled = checked;
                EnableWindow(GetDlgItem(hwnd, IDC_LV_CUSTOMHK), checked);
                EnableWindow(GetDlgItem(hwnd, IDC_BTN_ADD_CUSTOMHK), checked);
                EnableWindow(GetDlgItem(hwnd, IDC_BTN_EDIT_CUSTOMHK), checked);
                EnableWindow(GetDlgItem(hwnd, IDC_BTN_DEL_CUSTOMHK), checked);
            }
            return 0;
        }

        case IDC_BTN_ADD_CUSTOMHK:
            // 子对话框自带捕获钩子：不先取消会让主窗口捕获标志与钩子归属不一致而卡死
            CancelInlineCapture(data, hwnd);
            SettingsDialog::AddCustomHotkey(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_EDIT_CUSTOMHK:
            CancelInlineCapture(data, hwnd);
            SettingsDialog::EditCustomHotkey(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_DEL_CUSTOMHK:
            CancelInlineCapture(data, hwnd);
            SettingsDialog::DeleteCustomHotkey(hwnd, data->workingCopy);
            return 0;
        }
        break;

    case WM_NOTIFY: {
        auto* nmhdr = reinterpret_cast<NMHDR*>(lp);
        // 选项卡切换通知
        if (nmhdr->idFrom == IDC_TAB_MAIN && nmhdr->code == TCN_SELCHANGE) {
            int sel = TabCtrl_GetCurSel(GetDlgItem(hwnd, IDC_TAB_MAIN));
            SettingsDialog::SwitchTab(hwnd, sel, data->workingCopy);
        }
        break;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        // 设置控件背景为透明，使用系统按钮面色
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }

    case WM_CLOSE:
        // 关闭窗口时恢复原始配置；仅在本次确实应用过设置时才回滚并通知外部，
        // 避免用户什么都没改也重写 INI 并重新广播资源管理器设置
        if (data->applied) {
            *data->config = data->originalConfig;
            if (SettingsDialog::s_applyCallback) SettingsDialog::s_applyCallback();
        }
        DestroyWindow(hwnd);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

