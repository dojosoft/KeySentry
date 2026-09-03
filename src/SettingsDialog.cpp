// ===== SettingsDialog.cpp - 设置对话框实现 =====
// 包含键客 KeySentry 设置对话框的所有界面创建、事件处理和配置管理逻辑。
// 对话框包含六个选项卡：常规、按键禁用、按键映射、热键管理、窗口隐藏、窗口置顶。

#include "SettingsDialog.h"
#include "Resource.h"
#include "Utils.h"
#include <algorithm>
#include <TlHelp32.h>
#include <ShlObj.h>
#include <Commdlg.h>

// 外部函数声明
extern void ShowAboutDialog(HWND parent);   // 显示关于对话框
extern void SetHookSimulating(bool sim);    // 设置钩子模拟标志，防止模拟按键被自身钩子拦截
extern void SetSettingsWnd(HWND wnd);       // 设置设置窗口句柄到全局，供主窗口消息处理使用

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
// 计算选项卡内部可用于放置控件的矩形区域
static void GetPageArea(HWND tabCtrl, HWND parent, int* px, int* py, int* pw, int* ph) {
    RECT rc;
    GetClientRect(tabCtrl, &rc);
    TabCtrl_AdjustRect(tabCtrl, FALSE, &rc);   // 将选项卡外框转换为内部显示区域
    MapWindowPoints(tabCtrl, parent, (POINT*)&rc, 2); // 坐标转换到父窗口
    *px = rc.left + 10;
    *py = rc.top + 8;
    *pw = rc.right - rc.left - 20;
    *ph = rc.bottom - rc.top - 16;
}

// ===== 选项卡子类化过程 =====
// 自定义绘制选项卡，在选中标签下方填充一条与页面背景同色的矩形，
// 消除选项卡与页面之间的视觉间隙
static LRESULT CALLBACK TabSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                         UINT_PTR uid, DWORD_PTR data) {
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

// ===== 窗口选择对话框数据 =====
struct SelectDlgData {
    std::wstring* result;   // 选中窗口的标题
    bool* done;             // 对话框完成标志
};

// ===== 窗口枚举回调数据 =====
struct EnumSelData {
    HWND listHwnd;          // 列表框句柄，用于添加枚举到的窗口
};

// 窗口枚举回调（前向声明）
static BOOL CALLBACK EnumSelWindowsProc(HWND hwnd, LPARAM lParam);

// ===== 窗口选择对话框窗口过程 =====
// 显示当前所有可见窗口的列表，供用户选择
static LRESULT CALLBACK SelectWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* selData = reinterpret_cast<SelectDlgData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            // 确定按钮：获取选中项文本并写入结果
            if (selData && selData->result) {
                HWND lst = GetDlgItem(hwnd, 9999);
                int sel = (int)SendMessageW(lst, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR) {
                    wchar_t buf[512] = {};
                    SendMessageW(lst, LB_GETTEXT, sel, (LPARAM)buf);
                    *selData->result = buf;
                }
            }
            if (selData && selData->done) *selData->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        case IDCANCEL:
            // 取消按钮：不设置结果直接关闭
            if (selData && selData->done) *selData->done = true;
            DestroyWindow(hwnd);
            return 0;
        case 9998: {
            // 刷新按钮：重新枚举所有窗口
            HWND lst = GetDlgItem(hwnd, 9999);
            SendMessageW(lst, LB_RESETCONTENT, 0, 0);
            EnumSelData enumData{ lst };
            EnumWindows(EnumSelWindowsProc, (LPARAM)&enumData);
            return 0;
        }
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 窗口选择对话框类名
static const wchar_t* SELECT_CLASS = L"KeySentrySelectDlg";

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
    {VK_LMENU, L"Alt", 88, 160, 36, 28}, {VK_SPACE, L"Space", 128, 160, 198, 28},
    {VK_RMENU, L"Alt", 330, 160, 36, 28}, {VK_RWIN, L"Win", 370, 160, 36, 28},
    {VK_APPS, L"Menu", 410, 160, 36, 28}, {VK_RCONTROL, L"Ctrl", 450, 160, 42, 28},
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

// 虚拟键盘选择器数据
struct KbPickerData {
    int selectedVK;     // 用户选中的虚拟键码，0 表示取消
    bool done;          // 对话框完成标志
};

// ===== 虚拟键盘选择器窗口过程 =====
// 处理用户点击虚拟键盘上的按键
static LRESULT CALLBACK KbPickerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* pd = reinterpret_cast<KbPickerData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        // 按键按钮的 ID 范围为 10000 ~ 10000+KB_KEY_COUNT
        if (id >= 10000 && id < 10000 + KB_KEY_COUNT) {
            if (pd) pd->selectedVK = KB_KEYS[id - 10000].vk;
            if (pd) pd->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == IDCANCEL) {
            if (pd) pd->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (pd) pd->done = true;
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
// 返回选中的虚拟键码，0 表示用户取消
int SettingsDialog::ShowKeyboardPicker(HWND parent) {
    static bool registered = false;
    if (!registered) {
        // 注册键盘选择器窗口类
        WNDCLASSW wc = {};
        wc.lpfnWndProc = KbPickerWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = KB_PICKER_CLASS;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    // 计算窗口尺寸并居中显示
    RECT rc = { 0, 0, 710, 260 };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    KbPickerData pd = { 0, false };

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, KB_PICKER_CLASS,
                                 L"选择映射按键",
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                 (screenW - winW) / 2, (screenH - winH) / 2,
                                 winW, winH,
                                 parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&pd));

    GdiObjectGuard keyFont(CreateUiFont(12, FW_NORMAL));

    // 为每个键盘按键创建按钮控件
    for (int i = 0; i < KB_KEY_COUNT; i++) {
        CreateWindowExW(0, L"BUTTON", KB_KEYS[i].label,
                         WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                         KB_KEYS[i].x + 8, KB_KEYS[i].y + 8, KB_KEYS[i].w, KB_KEYS[i].h,
                         hwnd, (HMENU)(LONG_PTR)(10000 + i), GetModuleHandleW(nullptr), nullptr);
        HWND btn = GetDlgItem(hwnd, 10000 + i);
        SendMessageW(btn, WM_SETFONT, (WPARAM)keyFont.get(), TRUE);
    }

    // 模态消息循环
    {
        ModalGuard guard(parent);
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        MSG msg;
        while (!pd.done && IsWindow(hwnd)) {
            BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
            if (ret <= 0) { if (ret == 0) PostQuitMessage((int)msg.wParam); break; }
            // 将热键消息转发给主窗口处理
            if (msg.message == WM_HOTKEY) { if (g_mainWnd && IsWindow(g_mainWnd)) SendMessageW(g_mainWnd, msg.message, msg.wParam, msg.lParam); continue; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (IsWindow(hwnd)) DestroyWindow(hwnd);

    return pd.selectedVK;
}

// ===== 热键扫描：修饰键组合列表 =====
// 包含所有需要扫描的双修饰键及三修饰键组合（不含单修饰键，因为单修饰键+字母容易误判）
static const UINT SCAN_MODS[] = {
    MOD_ALT | MOD_CONTROL,
    MOD_ALT | MOD_SHIFT,
    MOD_CONTROL | MOD_SHIFT,
    MOD_ALT | MOD_CONTROL | MOD_SHIFT,
    MOD_WIN,
    MOD_WIN | MOD_ALT,
    MOD_WIN | MOD_CONTROL,
    MOD_WIN | MOD_SHIFT,
    MOD_WIN | MOD_ALT | MOD_CONTROL,
    MOD_WIN | MOD_ALT | MOD_SHIFT,
    MOD_WIN | MOD_CONTROL | MOD_SHIFT,
    MOD_WIN | MOD_ALT | MOD_CONTROL | MOD_SHIFT,
};
static const int SCAN_MOD_COUNT = sizeof(SCAN_MODS) / sizeof(SCAN_MODS[0]);

// ===== 热键扫描：虚拟键码列表 =====
// 包含字母键 A-Z、功能键 F1-F12、数字键 0-9
static const int SCAN_VKS[] = {
    0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A,
    0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54,
    0x55, 0x56, 0x57, 0x58, 0x59, 0x5A,
    VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6,
    VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
};
static const int SCAN_VK_COUNT = sizeof(SCAN_VKS) / sizeof(SCAN_VKS[0]);

// ===== 根据进程 ID 获取进程名称 =====
static std::wstring GetProcessNameFromPid(DWORD pid) {
    return ProcessUtils::GetProcessName(pid);
}

// ===== 判断热键是否安全可模拟 =====
// 某些系统热键（如 Ctrl+Alt+Del、Win+L 等）模拟后会导致严重后果，
// 此函数用于在探测热键归属程序前检查安全性
static bool IsSafeHotkeyToSimulate(UINT mod, UINT vk) {
    if ((mod & MOD_CONTROL) && (mod & MOD_ALT) && vk == VK_DELETE) return false;  // Ctrl+Alt+Del
    if ((mod & MOD_WIN) && (vk == 'L')) return false;                              // Win+L 锁屏
    if ((mod & MOD_ALT) && vk == VK_F4) return false;                              // Alt+F4 关闭
    if ((mod & MOD_WIN) && (vk == 'D')) return false;                              // Win+D 桌面
    if ((mod & MOD_CONTROL) && (mod & MOD_SHIFT) && vk == VK_ESCAPE) return false; // Ctrl+Shift+Esc 任务管理器
    if ((mod & MOD_ALT) && vk == VK_TAB) return false;                             // Alt+Tab 切换
    if ((mod & MOD_WIN) && (vk == 'R')) return false;                              // Win+R 运行
    if ((mod & MOD_WIN) && (vk == 'E')) return false;                              // Win+E 资源管理器
    if ((mod & MOD_WIN) && (vk == 'I')) return false;                              // Win+I 设置
    if ((mod & MOD_WIN) && (vk == 'M')) return false;                              // Win+M 最小化
    return true;
}

// ===== 识别热键归属程序 =====
// 通过模拟发送热键组合，观察前台窗口是否变化来判断热键被哪个程序注册
// 原理：模拟按键后等待 300ms，如果前台窗口发生变化，则新前台窗口即为热键注册者
static std::wstring IdentifyHotkeyOwner(UINT mod, UINT vk) {
    if (!IsSafeHotkeyToSimulate(mod, vk)) return L"";

    // 检查是否为可能触发数据丢失的热键（如 Alt+F4、Ctrl+W 等关闭类热键）
    // 同时检查是否为系统保护热键（如 Ctrl+Alt+Del、Win+L 等）
    bool isDangerousClose = (mod & MOD_ALT) && vk == VK_F4;
    bool isDangerousTabClose = (mod & MOD_CONTROL) && (vk == 'W' || vk == VK_F4);
    bool isSystemProtected = !IsSafeHotkeyToSimulate(mod, vk);
    if (isDangerousClose || isDangerousTabClose || isSystemProtected) {
        int result = MessageBoxW(g_mainWnd,
            L"该热键可能触发程序关闭、标签页关闭或系统功能，导致未保存数据丢失或系统不稳定。\n\n是否继续探测？",
            L"安全确认", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (result != IDYES) return L"";
    }

    // 记录当前前台窗口信息
    HWND prevFG = GetForegroundWindow();
    DWORD prevPid = 0;
    if (prevFG) GetWindowThreadProcessId(prevFG, &prevPid);

    // 构建模拟按键输入序列：按下修饰键 -> 按下主键 -> 释放主键 -> 释放修饰键
    INPUT inputs[16] = {};
    int idx = 0;
    if (mod & MOD_CONTROL) { inputs[idx].type = INPUT_KEYBOARD; inputs[idx].ki.wVk = VK_CONTROL; idx++; }
    if (mod & MOD_SHIFT) { inputs[idx].type = INPUT_KEYBOARD; inputs[idx].ki.wVk = VK_SHIFT; idx++; }
    if (mod & MOD_ALT) { inputs[idx].type = INPUT_KEYBOARD; inputs[idx].ki.wVk = VK_MENU; idx++; }
    if (mod & MOD_WIN) { inputs[idx].type = INPUT_KEYBOARD; inputs[idx].ki.wVk = VK_LWIN; idx++; }

    inputs[idx].type = INPUT_KEYBOARD; inputs[idx].ki.wVk = (WORD)vk; idx++;
    inputs[idx].type = INPUT_KEYBOARD; inputs[idx].ki.wVk = (WORD)vk;
    inputs[idx].ki.dwFlags = KEYEVENTF_KEYUP; idx++;

    if (mod & MOD_WIN) { inputs[idx].type = INPUT_KEYBOARD; inputs[idx].ki.wVk = VK_LWIN; inputs[idx].ki.dwFlags = KEYEVENTF_KEYUP; idx++; }
    if (mod & MOD_ALT) { inputs[idx].type = INPUT_KEYBOARD; inputs[idx].ki.wVk = VK_MENU; inputs[idx].ki.dwFlags = KEYEVENTF_KEYUP; idx++; }
    if (mod & MOD_SHIFT) { inputs[idx].type = INPUT_KEYBOARD; inputs[idx].ki.wVk = VK_SHIFT; inputs[idx].ki.dwFlags = KEYEVENTF_KEYUP; idx++; }
    if (mod & MOD_CONTROL) { inputs[idx].type = INPUT_KEYBOARD; inputs[idx].ki.wVk = VK_CONTROL; inputs[idx].ki.dwFlags = KEYEVENTF_KEYUP; idx++; }

    // 设置模拟标志防止钩子拦截，发送按键后等待 300ms 观察前台窗口变化
    SetHookSimulating(true);
    SendInput(idx, inputs, sizeof(INPUT));
    Sleep(300);
    SetHookSimulating(false);

    // 检查前台窗口是否变化，如果变化则获取新窗口的进程名
    HWND newFG = GetForegroundWindow();
    std::wstring result;
    if (newFG && newFG != prevFG) {
        DWORD pid = 0;
        GetWindowThreadProcessId(newFG, &pid);
        result = GetProcessNameFromPid(pid);
    }
    // 恢复之前的前台窗口
    if (prevFG && IsWindow(prevFG)) SetForegroundWindow(prevFG);
    return result;
}

// ===== 扫描系统热键 =====
// 遍历所有修饰键+虚拟键组合，尝试注册全局热键，
// 如果注册失败（ERROR_HOTKEY_ALREADY_REGISTERED），说明该热键已被其他程序占用
void SettingsDialog::ScanHotkeys(HWND hwnd, AppConfig& config) {
    auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    data->scannedHotkeys.clear();

    HWND mainWnd = GetParent(hwnd);
    int tempId = 10000; // 临时热键 ID 起始值，避免与现有热键 ID 冲突

    for (int m = 0; m < SCAN_MOD_COUNT; m++) {
        for (int k = 0; k < SCAN_VK_COUNT; k++) {
            UINT mod = SCAN_MODS[m];
            int vk = SCAN_VKS[k];

            // 跳过本程序已注册的窗口隐藏热键
            if (config.bossKeyEnabled && mod == config.bossKeyMod && (UINT)vk == config.bossKeyVK) {
                continue;
            }
            // 跳过本程序已注册的一键关闭程序热键
            if (config.bossKeyEnabled && mod == config.bossKeyCloseMod && (UINT)vk == config.bossKeyCloseVK) {
                continue;
            }
            // 跳过本程序已注册的自定义热键
            if (config.customHotkeysEnabled) {
                bool isCustom = false;
                for (const auto& hk : config.customHotkeys) {
                    if (hk.mod == mod && hk.vk == (UINT)vk) { isCustom = true; break; }
                }
                if (isCustom) continue;
            }

            // 尝试注册热键，如果失败说明已被占用
            if (!RegisterHotKey(mainWnd, tempId, mod, vk)) {
                DWORD err = GetLastError();
                if (err == ERROR_HOTKEY_ALREADY_REGISTERED) {
                    ScannedHotkey sh;
                    sh.mod = mod;
                    sh.vk = (UINT)vk;
                    sh.ownerName = L"";
                    data->scannedHotkeys.push_back(sh);
                }
            } else {
                // 注册成功说明未被占用，立即注销
                UnregisterHotKey(mainWnd, tempId);
            }
            tempId++;
        }
    }

    RefreshHotkeyList(hwnd, config);
}

// ===== 获取热键列表选中项的真实数据下标 =====
// 列表在过滤/搜索后显示行号与 scannedHotkeys 向量下标错位，
// 因此插入时把真实下标存入 lParam，操作时经 lParam 取回
static int GetSelectedHotkeyIndex(HWND lv, const std::vector<SettingsDialog::ScannedHotkey>& hotkeys) {
    int sel = (int)SendMessageW(lv, LVM_GETNEXTITEM, -1, LVNI_SELECTED);
    if (sel < 0) return -1;
    LVITEMW item = {};
    item.mask = LVIF_PARAM;
    item.iItem = sel;
    if (!SendMessageW(lv, LVM_GETITEMW, 0, (LPARAM)&item)) return -1;
    int idx = (int)item.lParam;
    if (idx < 0 || idx >= (int)hotkeys.size()) return -1;
    return idx;
}

// ===== 探测热键归属程序 =====
// 选中一个已占用的热键后，通过模拟按键来识别注册该热键的程序
void SettingsDialog::ProbeHotkey(HWND hwnd, AppConfig& config) {
    auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    HWND lv = GetDlgItem(hwnd, IDC_LV_HOTKEYS);
    int sel = GetSelectedHotkeyIndex(lv, data->scannedHotkeys);
    if (sel < 0) {
        MessageBoxW(hwnd, L"请先选择一个热键",
                     L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }

    auto& sh = data->scannedHotkeys[sel];
    // 系统保护的热键无法模拟探测
    if (!IsSafeHotkeyToSimulate(sh.mod, sh.vk)) {
        sh.ownerName = L"系统保护";
        RefreshHotkeyList(hwnd, config);
        return;
    }

    // 提示用户即将模拟按键，可能会短暂激活该热键对应的功能
    int result = MessageBoxW(hwnd,
        L"即将短暂模拟该热键以识别注册程序，可能会短暂激活该功能。是否继续？",
        L"探测热键",
        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (result != IDYES) return;

    // 执行探测
    sh.ownerName = IdentifyHotkeyOwner(sh.mod, sh.vk);
    if (sh.ownerName.empty()) {
        sh.ownerName = L"[未知]";
    }

    // 清理探测过程中可能产生的消息
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        // WM_QUIT 不能被 Dispatch，需重新投递以保留退出请求
        if (msg.message == WM_QUIT) { PostQuitMessage((int)msg.wParam); break; }
        if (msg.message == WM_HOTKEY) { if (g_mainWnd && IsWindow(g_mainWnd)) SendMessageW(g_mainWnd, msg.message, msg.wParam, msg.lParam); continue; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    RefreshHotkeyList(hwnd, config);
}

// ===== 禁用选中的热键 =====
// 将选中的已占用热键添加到禁用列表，使其不再生效
void SettingsDialog::DisableHotkey(HWND hwnd, AppConfig& config) {
    auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    HWND lv = GetDlgItem(hwnd, IDC_LV_HOTKEYS);
    int sel = GetSelectedHotkeyIndex(lv, data->scannedHotkeys);
    if (sel < 0) return;

    auto hk = data->scannedHotkeys[sel];
    // 检查是否已在禁用列表中
    bool exists = false;
    for (auto& d : config.disabledHotkeys) {
        if (d.first == hk.mod && d.second == hk.vk) { exists = true; break; }
    }
    if (exists) return;

    // 检查是否与窗口隐藏热键冲突
    if (config.bossKeyEnabled) {
        if (hk.mod == config.bossKeyMod && hk.vk == config.bossKeyVK) {
            MessageBoxW(hwnd, L"该热键与窗口隐藏冲突，无法禁用",
                         L"提示", MB_OK | MB_ICONWARNING);
            return;
        }
        if (hk.mod == config.bossKeyCloseMod && hk.vk == config.bossKeyCloseVK) {
            MessageBoxW(hwnd, L"该热键与一键关闭程序热键冲突，无法禁用",
                         L"提示", MB_OK | MB_ICONWARNING);
            return;
        }
    }

    config.disabledHotkeys.push_back({hk.mod, hk.vk});
    RefreshHotkeyList(hwnd, config);
}

// ===== 启用选中的热键 =====
// 将选中的已禁用热键从禁用列表中移除，恢复其功能
void SettingsDialog::EnableHotkey(HWND hwnd, AppConfig& config) {
    auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    HWND lv = GetDlgItem(hwnd, IDC_LV_HOTKEYS);
    int sel = GetSelectedHotkeyIndex(lv, data->scannedHotkeys);
    if (sel < 0) return;

    auto hk = data->scannedHotkeys[sel];
    for (auto it = config.disabledHotkeys.begin(); it != config.disabledHotkeys.end(); ++it) {
        if (it->first == hk.mod && it->second == hk.vk) {
            config.disabledHotkeys.erase(it);
            RefreshHotkeyList(hwnd, config);
            return;
        }
    }
}

// ===== 刷新系统热键列表视图 =====
// 根据过滤条件和搜索关键词更新热键列表视图的内容
void SettingsDialog::RefreshHotkeyList(HWND hwnd, AppConfig& config) {
    auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    HWND lv = GetDlgItem(hwnd, IDC_LV_HOTKEYS);
    if (!lv) return;
    SendMessageW(lv, LVM_DELETEALLITEMS, 0, 0);

    // 获取过滤条件：0=全部, 1=已占用, 2=已禁用, 3=系统保护
    HWND cbo = GetDlgItem(hwnd, IDC_CBO_HKFILTER);
    int filter = cbo ? (int)SendMessageW(cbo, CB_GETCURSEL, 0, 0) : 0;

    // 获取搜索关键词（不区分大小写）
    wchar_t searchText[128] = {};
    HWND edt = GetDlgItem(hwnd, IDC_EDT_HKSEARCH);
    if (edt) GetWindowTextW(edt, searchText, 128);
    std::wstring search = searchText;
    for (auto& c : search) c = towlower(c);

    int idx = 0;
    for (size_t i = 0; i < data->scannedHotkeys.size(); i++) {
        const auto& hk = data->scannedHotkeys[i];
        // 判断热键状态
        bool isDisabled = false;
        for (auto& d : config.disabledHotkeys) {
            if (d.first == hk.mod && d.second == hk.vk) { isDisabled = true; break; }
        }
        bool isProtected = !IsSafeHotkeyToSimulate(hk.mod, hk.vk);

        // 根据过滤条件筛选
        if (filter == 1 && (isDisabled || isProtected)) continue;
        if (filter == 2 && !isDisabled) continue;
        if (filter == 3 && !isProtected) continue;

        std::wstring keyName = FormatHotKey(hk.mod, hk.vk);
        std::wstring owner = hk.ownerName.empty() ? L"" : hk.ownerName;
        std::wstring status;
        if (isDisabled) status = L"已禁用";
        else if (isProtected) status = L"系统保护";
        else status = L"已占用";

        // 搜索关键词匹配（在热键名、归属程序、状态中搜索）
        if (!search.empty()) {
            std::wstring combined = keyName + owner + status;
            for (auto& c : combined) c = towlower(c);
            if (combined.find(search) == std::wstring::npos) continue;
        }

        // 插入列表视图项（lParam 保存真实数据下标，过滤/搜索后行号与下标会错位）
        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = idx;
        item.iSubItem = 0;
        item.lParam = (LPARAM)i;
        item.pszText = const_cast<LPWSTR>(keyName.c_str());
        SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&item);

        item.iSubItem = 1;
        item.pszText = const_cast<LPWSTR>(owner.c_str());
        SendMessageW(lv, LVM_SETITEMW, 0, (LPARAM)&item);

        item.iSubItem = 2;
        item.pszText = const_cast<LPWSTR>(status.c_str());
        SendMessageW(lv, LVM_SETITEMW, 0, (LPARAM)&item);

        idx++;
    }
}

// ===== 刷新已禁用热键列表 =====
// 委托给 RefreshHotkeyList，因为禁用热键列表和热键列表共享同一个视图
void SettingsDialog::RefreshDisabledHotkeyList(HWND hwnd, AppConfig& config) {
    RefreshHotkeyList(hwnd, config);
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
        RegisterClassW(&wc);

        // 同时注册窗口选择对话框类
        WNDCLASSW wc2 = {};
        wc2.lpfnWndProc = SelectWndProc;
        wc2.hInstance = GetModuleHandleW(nullptr);
        wc2.lpszClassName = SELECT_CLASS;
        wc2.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc2.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc2);

        registered = true;
    }

    // 计算窗口尺寸并居中
    RECT rc = { 0, 0, CLIENT_W, CLIENT_H };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    // 创建设置对话框主窗口
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, SETTINGS_CLASS,
                                 L"键客 设置",
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                 (screenW - winW) / 2, (screenH - winH) / 2,
                                 winW, winH,
                                 parent, nullptr, GetModuleHandleW(nullptr), nullptr);

    // 将对话框数据存储到窗口的 USERDATA 中
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&data));

    // 设置全局设置窗口句柄，供主窗口消息处理使用
    SetSettingsWnd(hwnd);

    HFONT font = GetDlgFont();

    // 创建选项卡控件
    HWND tabCtrl = CreateWindowExW(0, WC_TABCONTROLW, L"",
                                     WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
                                     TAB_X, TAB_Y, TAB_W, TAB_H,
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
                                      10, BTN_Y, BTN_W, BTN_H,
                                      hwnd, (HMENU)IDC_BTN_ABOUT, GetModuleHandleW(nullptr), nullptr);
    HWND btnOk = CreateWindowExW(0, L"BUTTON", L"确定",
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   CLIENT_W - BTN_W * 3 - 30, BTN_Y, BTN_W, BTN_H,
                                   hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
    HWND btnCancel = CreateWindowExW(0, L"BUTTON", L"取消",
                                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       CLIENT_W - BTN_W * 2 - 20, BTN_Y, BTN_W, BTN_H,
                                       hwnd, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);
    HWND btnApply = CreateWindowExW(0, L"BUTTON", L"应用",
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      CLIENT_W - BTN_W - 10, BTN_Y, BTN_W, BTN_H,
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
        if (msg.message == WM_HOTKEY) { if (g_mainWnd && IsWindow(g_mainWnd)) SendMessageW(g_mainWnd, msg.message, msg.wParam, msg.lParam); continue; }
        // 处理窗口隐藏热键捕获
        if ((msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) && data.capturingHotKey) {
            UINT mod = WindowUtils::GetCurrentModifiers();
            int vk = (int)msg.wParam;
            if (vk == VK_ESCAPE) {
                // Esc 取消捕获，恢复显示原热键
                data.capturingHotKey = false;
                SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_SETHOTKEY),
                               FormatHotKey(data.workingCopy.bossKeyMod, data.workingCopy.bossKeyVK).c_str());
            } else if (vk != VK_CONTROL && vk != VK_SHIFT && vk != VK_MENU &&
                vk != VK_LWIN && vk != VK_RWIN) {
                // 非纯修饰键：记录捕获到的热键组合
                data.captureMod = mod;
                data.captureVK = (UINT)vk;
                data.capturingHotKey = false;
                data.workingCopy.bossKeyMod = mod;
                data.workingCopy.bossKeyVK = (UINT)vk;
                SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_SETHOTKEY),
                               FormatHotKey(mod, (UINT)vk).c_str());
            }
            continue;
        }
        // 处理一键关闭程序热键捕获
        if ((msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) && data.capturingCloseHotKey) {
            UINT mod = WindowUtils::GetCurrentModifiers();
            int vk = (int)msg.wParam;
            if (vk == VK_ESCAPE) {
                data.capturingCloseHotKey = false;
                SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_SETCLOSEHOTKEY),
                               FormatHotKey(data.workingCopy.bossKeyCloseMod, data.workingCopy.bossKeyCloseVK).c_str());
            } else if (vk != VK_CONTROL && vk != VK_SHIFT && vk != VK_MENU &&
                vk != VK_LWIN && vk != VK_RWIN) {
                data.captureCloseMod = mod;
                data.captureCloseVK = (UINT)vk;
                data.capturingCloseHotKey = false;
                data.workingCopy.bossKeyCloseMod = mod;
                data.workingCopy.bossKeyCloseVK = (UINT)vk;
                SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_SETCLOSEHOTKEY),
                               FormatHotKey(mod, (UINT)vk).c_str());
            }
            continue;
        }
        if (!IsWindow(hwnd)) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
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
    case 0: // 常规页面
        config.autoStart = chk(IDC_CHK_AUTOSTART);
        config.forceNumLockOn = chk(IDC_CHK_FORCENUMLOCK);
        config.disableStartupNotification = chk(IDC_CHK_DISABLE_STARTUP_NOTIFY);
        config.showLockKeyOverlay = chk(IDC_CHK_OVERLAY);
        config.showMuteOverlay = chk(IDC_CHK_MUTE_OVERLAY);
        config.overlayPrimaryOnly = chk(IDC_CHK_PRIMARY_ONLY);
        // 保存资源管理器文件夹选项下拉框（0=不改变, 1=开启, 2=关闭）
        config.explorerHideHidden = (int)SendMessageW(GetDlgItem(hwnd, IDC_CBO_EXPLORER_HIDDEN), CB_GETCURSEL, 0, 0);
        config.explorerHideExt = (int)SendMessageW(GetDlgItem(hwnd, IDC_CBO_EXPLORER_EXT), CB_GETCURSEL, 0, 0);
        config.explorerHideOS = (int)SendMessageW(GetDlgItem(hwnd, IDC_CBO_EXPLORER_OS), CB_GETCURSEL, 0, 0);
        break;
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
    if (lst) EnableWindow(lst, enabled);
    if (btnAdd) EnableWindow(btnAdd, enabled);
    if (btnDel) EnableWindow(btnDel, enabled);
}

// ===== 切换选项卡页面 =====
// 保存当前页面状态，销毁旧控件，根据新索引创建对应的页面控件
void SettingsDialog::SwitchTab(HWND hwnd, int tabIndex, AppConfig& config) {
    auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

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
    int lblW = 130;         // 标签宽度
    int smallBtnW = 80;     // 小按钮宽度
    int listBoxH = 140;     // 列表框默认高度

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
                L"功能说明：\n按键映射功能允许用户将某个按键重新定义为另一个按键，实现键盘布局的自定义调整。",
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
        makeButton(IDC_BTN_SCANHOTKEYS, L"扫描系统热键", PAGE_X, y, smallBtnW + 20, btnH);
        makeLabel(L"过滤:", PAGE_X + smallBtnW + 30, y, 40, btnH);
        CreateWindowExW(0, L"COMBOBOX", L"",
                         WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                         PAGE_X + smallBtnW + 72, y, 100, 200,
                         hwnd, (HMENU)IDC_CBO_HKFILTER, GetModuleHandleW(nullptr), nullptr);
        {
            HWND cbo = GetDlgItem(hwnd, IDC_CBO_HKFILTER);
            SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)L"全部");
            SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)L"已占用");
            SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)L"已禁用");
            SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)L"系统保护");
            SendMessageW(cbo, CB_SETCURSEL, 0, 0);
            SetControlFont(cbo, font);
        }

        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                         WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                         PAGE_X + smallBtnW + 180, y, PAGE_W - smallBtnW - 190, lineH - 2,
                         hwnd, (HMENU)IDC_EDT_HKSEARCH, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(GetDlgItem(hwnd, IDC_EDT_HKSEARCH), font);
        y += btnH + 6;

        int lvH = (PAGE_Y + PAGE_H - y - btnH * 2 - 60) / 2;
        if (lvH < 80) lvH = 80;
        HWND lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                     WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER,
                                    PAGE_X, y, PAGE_W - 10, lvH,
                                    hwnd, (HMENU)IDC_LV_HOTKEYS, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(lv, font);
        SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        ListView_SetBkColor(lv, RGB(255, 255, 255));
        ListView_SetTextColor(lv, GetSysColor(COLOR_WINDOWTEXT));
        ListView_SetTextBkColor(lv, RGB(255, 255, 255));

        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.fmt = LVCFMT_LEFT;
        col.cx = 160; col.pszText = const_cast<LPWSTR>(L"热键");
        SendMessageW(lv, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
        col.cx = 140; col.pszText = const_cast<LPWSTR>(L"归属程序");
        SendMessageW(lv, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
        col.cx = 80; col.pszText = const_cast<LPWSTR>(L"状态");
        SendMessageW(lv, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);

        y += lvH + 6;

        int customBtnW = (PAGE_W - 40) / 4;
        makeButton(IDC_BTN_PROBEHOTKEY, L"探测所属程序", PAGE_X, y, customBtnW, btnH);
        makeButton(IDC_BTN_DISABLEHOTKEY, L"禁用选中热键", PAGE_X + customBtnW + 10, y, customBtnW, btnH);
        makeButton(IDC_BTN_ENABLEHOTKEY, L"启用选中热键", PAGE_X + (customBtnW + 10) * 2, y, customBtnW, btnH);
        y += btnH + 8;

        makeChk(IDC_CHK_CUSTOM_HK_ENABLE, L"启用自定义热键", PAGE_X, y, PAGE_W - 20, config.customHotkeysEnabled);
        y += lineH + 4;

        int customLvH = PAGE_Y + PAGE_H - y - btnH - 16;
        if (customLvH < 40) customLvH = 40;
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

        y += customLvH + 6;

        makeButton(IDC_BTN_ADD_CUSTOMHK, L"新增", PAGE_X, y, customBtnW, btnH);
        makeButton(IDC_BTN_EDIT_CUSTOMHK, L"编辑选中项", PAGE_X + customBtnW + 10, y, customBtnW, btnH);
        makeButton(IDC_BTN_DEL_CUSTOMHK, L"删除选中项", PAGE_X + (customBtnW + 10) * 2, y, customBtnW, btnH);

        if (!config.customHotkeysEnabled) {
            EnableWindow(GetDlgItem(hwnd, IDC_LV_CUSTOMHK), FALSE);
            EnableWindow(GetDlgItem(hwnd, IDC_BTN_ADD_CUSTOMHK), FALSE);
            EnableWindow(GetDlgItem(hwnd, IDC_BTN_EDIT_CUSTOMHK), FALSE);
            EnableWindow(GetDlgItem(hwnd, IDC_BTN_DEL_CUSTOMHK), FALSE);
        }

        RefreshHotkeyList(hwnd, config);
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

// 枚举回调上下文：所有窗口列表
struct AllWindowsEnumCtx {
    std::vector<BoundWindowInfo>* cache;
    std::vector<BoundWindowInfo>* bound;
    HWND lstAll;
};

// 枚举回调：32 位下 GCC 的 lambda 无法隐式转换为 stdcall 的 WNDENUMPROC，
// 故用显式 CALLBACK 约定的命名函数
static BOOL CALLBACK CollectAllWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t title[512] = {};
    GetWindowTextW(hwnd, title, 512);
    if (wcslen(title) == 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return TRUE;

    std::wstring processName;
    std::wstring processPath;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        wchar_t path[MAX_PATH] = {};
        DWORD sz = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, path, &sz)) {
            processPath = path;
            std::wstring fp(path);
            size_t pos = fp.find_last_of(L"\\/");
            processName = (pos != std::wstring::npos) ? fp.substr(pos + 1) : fp;
        }
        CloseHandle(hProc);
    }

    auto* ctx = reinterpret_cast<AllWindowsEnumCtx*>(lParam);
    bool alreadyBound = false;
    for (const auto& b : *ctx->bound) {
        if (b.pid == pid || (!b.processName.empty() && _wcsicmp(b.processName.c_str(), processName.c_str()) == 0)) {
            alreadyBound = true;
            break;
        }
    }
    if (!alreadyBound) {
        BoundWindowInfo info{ title, processName, pid, processPath };
        ctx->cache->push_back(info);
        SendMessageW(ctx->lstAll, LB_ADDSTRING, 0, (LPARAM)info.DisplayName().c_str());
    }
    return TRUE;
}

// 枚举回调上下文：异常置顶窗口收集
struct TopmostEnumData {
    std::vector<HWND>* windows;
};

// 枚举回调：收集异常置顶窗口（排除系统窗口），见 IDC_BTN_FIX_ZORDER
static BOOL CALLBACK CollectTopmostWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) return TRUE;
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (!(exStyle & WS_EX_TOPMOST)) return TRUE;
    if (exStyle & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) {
        if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_POPUP) return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return TRUE;
    wchar_t className[256] = {};
    GetClassNameW(hwnd, className, 256);
    // 排除系统窗口类（任务栏、桌面等）
    static const wchar_t* sysClasses[] = {
        L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
        L"NotifyIconOverflowWindow", L"TaskListOverlayWnd",
        L"Windows.UI.Core.CoreWindow", L"ApplicationFrameWindow",
        L"Progman", L"WorkerW",
    };
    for (auto sc : sysClasses) {
        if (_wcsicmp(className, sc) == 0) return TRUE;
    }
    reinterpret_cast<TopmostEnumData*>(lParam)->windows->push_back(hwnd);
    return TRUE;
}

// ===== 刷新所有窗口列表 =====
// 枚举当前系统中所有可见窗口，排除已绑定的窗口，填充到列表框中
void SettingsDialog::RefreshAllWindowsList(HWND hwnd, AppConfig& config) {
    HWND lstAll = GetDlgItem(hwnd, IDC_LST_ALL_WINDOWS);
    if (!lstAll) return;

    auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!data) return;

    data->allWindowsCache.clear();
    SendMessageW(lstAll, LB_RESETCONTENT, 0, 0);

    AllWindowsEnumCtx ctx{ &data->allWindowsCache, &config.bossKeyWindows, lstAll };
    EnumWindows(CollectAllWindowsProc, reinterpret_cast<LPARAM>(&ctx));
}

// 从所有窗口列表添加绑定（预留接口，当前未实现）
void SettingsDialog::AddBindingFromAll(HWND hwnd, AppConfig& config) {
}

// 移除绑定到所有窗口（委托给 RemoveBossWindow）
void SettingsDialog::RemoveBindingToAll(HWND hwnd, AppConfig& config) {
    RemoveBossWindow(hwnd, config);
}

// 添加进程绑定（预留接口，当前未实现）
void SettingsDialog::AddProcessBinding(HWND hwnd, AppConfig& config) {
}

// 添加窗口绑定（预留接口，当前未实现）
void SettingsDialog::AddWindowBinding(HWND hwnd, AppConfig& config) {
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
        std::wstring src = AppConfig::VKToName(r.first);
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = (int)i;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(src.c_str());
        ListView_InsertItem(lv, &item);
        std::wstring dst = AppConfig::VKToName(r.second);
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
    bool done;          // 对话框完成标志
};

static HHOOK s_captureKeyHook = nullptr; // 全局键盘钩子句柄
static HWND s_captureKeyDlg = nullptr;   // 捕获对话框窗口句柄

// ===== 按键捕获低级键盘钩子过程 =====
// 拦截键盘输入，将非注入的按键事件转发给捕获对话框
static LRESULT CALLBACK CaptureKeyHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && s_captureKeyDlg) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        // 忽略注入的按键（避免模拟按键被捕获）
        if (kb->flags & LLKHF_INJECTED) {
            return CallNextHookEx(s_captureKeyHook, nCode, wParam, lParam);
        }
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            PostMessageW(s_captureKeyDlg, WM_CAPTURE_KEY_MSG, (WPARAM)kb->vkCode, 0);
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
        // 取消捕获
        if (data) { data->capturedVK = 0; data->done = true; }
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
        data->capturedVK = vk;
        data->done = true;
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_CLOSE:
        if (data) { data->capturedVK = 0; data->done = true; }
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
// 返回捕获到的虚拟键码，0 表示取消
static int ShowCaptureKeyDialogEx(HWND parent, const wchar_t* title, const wchar_t* line1, const wchar_t* line2) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = CaptureKeyWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = CAPTURE_KEY_CLASS;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    RECT rc = { 0, 0, 360, 120 };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    CaptureKeyData data = { 0, false };

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, CAPTURE_KEY_CLASS,
                                 title,
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                 (screenW - winW) / 2, (screenH - winH) / 2,
                                 winW, winH,
                                 parent, nullptr, GetModuleHandleW(nullptr), nullptr);
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

    s_captureKeyDlg = hwnd;

    // 通知进入捕获模式
    if (SettingsDialog::s_captureModeCallback) SettingsDialog::s_captureModeCallback(true);

    // 安装低级键盘钩子
    s_captureKeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, CaptureKeyHookProc,
                                          GetModuleHandleW(nullptr), 0);

    {
        ModalGuard guard(parent);
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        MSG msg;
        while (!data.done && IsWindow(hwnd)) {
            BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
            if (ret <= 0) { if (ret == 0) PostQuitMessage((int)msg.wParam); break; }
            if (msg.message == WM_HOTKEY) { if (g_mainWnd && IsWindow(g_mainWnd)) SendMessageW(g_mainWnd, msg.message, msg.wParam, msg.lParam); continue; }
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

    if (IsWindow(hwnd)) DestroyWindow(hwnd);

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
// 先捕获源按键，再通过虚拟键盘选择目标按键，检查循环映射后添加规则
void SettingsDialog::AddRemapEntry(HWND hwnd, AppConfig& config) {
    int sourceVK = ShowCaptureKeyDialogEx(hwnd,
        L"按键映射 - 源按键",
        L"请按下要映射的源按键...",
        L"(Fn键无法捕获)");
    if (sourceVK == 0) return;

    bool exists = false;
    for (auto& r : config.keyRemappings) {
        if (r.first == sourceVK) { exists = true; break; }
    }
    if (exists) {
        MessageBoxW(hwnd, L"该源按键已存在映射规则",
                     L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }

    int targetVK = ShowKeyboardPicker(hwnd);
    if (targetVK == 0) return;

    if (sourceVK == targetVK) {
        MessageBoxW(hwnd, L"源按键和目标按键不能相同",
                     L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    for (const auto& r : config.keyRemappings) {
        if (r.second == sourceVK) {
            MessageBoxW(hwnd, L"该目标按键已被其他按键映射为源按键，这会导致循环映射问题。\n请先删除相关映射后再试。",
                         L"提示", MB_OK | MB_ICONWARNING);
            return;
        }
    }

    config.keyRemappings.push_back({sourceVK, targetVK});
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

// 添加窗口隐藏绑定窗口（预留接口）
void SettingsDialog::AddBossWindow(HWND hwnd, AppConfig& config) {
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
        RegisterClassW(&wc);
        *pRegistered = true;
    }

    AddBindingData abd;
    abd.allWindowsCache = &data->allWindowsCache;
    abd.boundWindows = &boundWindows;
    abd.workingCopy = &config;
    abd.changed = false;

    RECT rc = { 0, 0, 520, 520 };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, className,
                                L"添加绑定",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                (screenW - winW) / 2, (screenH - winH) / 2,
                                winW, winH,
                                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
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
                                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       cx + cw - 70, y - 2, 70, 26,
                                       dlg, (HMENU)IDC_BTN_REFRESH_WINS, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(btnRefresh, font);
    y += 28;

    HWND lstAll = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                   WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
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
                                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    cx, y, 160, 30,
                                    dlg, (HMENU)IDC_BTN_ADD_PROC_BINDING, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(btnProc, font);

    HWND btnWin = CreateWindowExW(0, L"BUTTON", L"按窗口名绑定",
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
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

    HWND lblManual = CreateWindowExW(0, L"STATIC", L"手动添加窗口标题 (支持部分匹配):",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      cx, y, cw, 24, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(lblManual, font);
    y += 28;

    HWND edtManual = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                      cx, y, cw - 90, 28,
                                      dlg, (HMENU)IDC_EDT_BOSSWIN, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(edtManual, font);

    HWND btnAddManual = CreateWindowExW(0, L"BUTTON", L"添加",
                                         WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                         cx + cw - 80, y, 80, 28,
                                         dlg, (HMENU)IDC_BTN_ADDBOSS, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(btnAddManual, font);
    y += 38;

    HWND btnCancel = CreateWindowExW(0, L"BUTTON", L"关闭",
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      cx + cw - 80, y, 80, 30,
                                      dlg, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(btnCancel, font);

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
            if (msg.message == WM_HOTKEY) { if (g_mainWnd && IsWindow(g_mainWnd)) SendMessageW(g_mainWnd, msg.message, msg.wParam, msg.lParam); continue; }
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
        RefreshAllWindowsList(hwnd, config);
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

// ===== 窗口枚举回调（用于窗口选择对话框） =====
static BOOL CALLBACK EnumSelWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t title[512] = {};
    if (GetWindowTextW(hwnd, title, 512) == 0) return TRUE;
    if (wcslen(title) == 0) return TRUE;
    auto* data = reinterpret_cast<EnumSelData*>(lParam);
    SendMessageW(data->listHwnd, LB_ADDSTRING, 0, (LPARAM)title);
    return TRUE;
}

// 从窗口列表中选择窗口并填入编辑框（预留接口，当前未实现）
void SettingsDialog::SelectWindow(HWND hwnd, int listBoxId, int editId) {
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
};

// ===== 自定义热键对话框窗口过程 =====
static LRESULT CALLBACK AddHotkeyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ahd = reinterpret_cast<AddHotkeyData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!ahd) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            // 确定按钮：验证输入并保存自定义热键
            HWND edtKey = GetDlgItem(hwnd, IDC_EDT_HK_KEY);
            wchar_t keyText[64] = {};
            GetWindowTextW(edtKey, keyText, 64);
            if (wcslen(keyText) == 0) {
                MessageBoxW(hwnd, L"请设置热键", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }

            // 主键必须有效：未点捕获按钮时尝试解析手输文本，防止保存 vk=0 的无效热键
            if (ahd->captureVK == 0) {
                UINT parsedVK = (UINT)AppConfig::NameToVK(keyText);
                if (parsedVK == 0) {
                    MessageBoxW(hwnd, L"主键无效，请点击\"捕获快捷键\"按钮设置主键",
                                 L"提示", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                ahd->captureVK = parsedVK;
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
            wchar_t cmdText[MAX_PATH] = {};
            GetWindowTextW(edtCmd, cmdText, MAX_PATH);
            if (wcslen(cmdText) == 0) {
                MessageBoxW(hwnd, L"请输入命令或程序路径", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }

            // 构建自定义热键对象
            // 修饰键完全以复选框勾选状态为准（从 0 构建），
            // 用户取消勾选修饰键后保存才能生效（此前 |= 只加不减）
            CustomHotkey hk;
            hk.mod = hotkeyMod;
            hk.vk = ahd->captureVK;

            hk.command = cmdText;

            wchar_t paramsText[512] = {};
            GetWindowTextW(GetDlgItem(hwnd, IDC_EDT_HK_PARAMS), paramsText, 512);
            hk.parameters = paramsText;

            wchar_t workDirText[MAX_PATH] = {};
            GetWindowTextW(GetDlgItem(hwnd, IDC_EDT_HK_WORKDIR), workDirText, MAX_PATH);
            hk.workDir = workDirText;

            wchar_t descText[256] = {};
            GetWindowTextW(GetDlgItem(hwnd, IDC_EDT_HK_DESC), descText, 256);
            hk.description = descText;

            HWND cboWinState = GetDlgItem(hwnd, IDC_CBO_HK_WINSTATE);
            int wsSel = (int)SendMessageW(cboWinState, CB_GETCURSEL, 0, 0);
            hk.windowState = ConfigUtils::ClampWindowState(wsSel);

            hk.confirmBeforeRun = SendMessageW(GetDlgItem(hwnd, IDC_CHK_HK_CONFIRM), BM_GETCHECK, 0, 0) == BST_CHECKED;
            hk.runAsAdmin = SendMessageW(GetDlgItem(hwnd, IDC_CHK_HK_ADMIN), BM_GETCHECK, 0, 0) == BST_CHECKED;

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
            // 开始捕获热键
            ahd->capturing = true;
            SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_HK_CAPTURE), L"请按下快捷键...");
            SetFocus(hwnd);
            return 0;
        case IDC_BTN_HK_BROWSE: {
            // 浏览选择可执行文件
            OPENFILENAMEW ofn = {};
            wchar_t szFile[MAX_PATH] = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrFilter = L"可执行文件\0*.exe;*.bat;*.cmd;*.ps1\0所有文件\0*.*\0";
            ofn.nFilterIndex = 1;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                SetWindowTextW(GetDlgItem(hwnd, IDC_EDT_HK_CMD), szFile);
            }
            return 0;
        }
        case IDC_BTN_HK_WORKDIR_BROWSE: {
            // 浏览选择工作目录
            wchar_t dir[MAX_PATH] = {};
            BROWSEINFOW bi = {};
            bi.hwndOwner = hwnd;
            bi.lpszTitle = L"选择工作目录";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                SHGetPathFromIDListW(pidl, dir);
                SetWindowTextW(GetDlgItem(hwnd, IDC_EDT_HK_WORKDIR), dir);
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
        RegisterClassW(&wc);
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
    }

    RECT rc = { 0, 0, 480, 400 };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, ADD_HOTKEY_CLASS,
                                isEdit ? L"编辑自定义热键" : L"新增自定义热键",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                (screenW - winW) / 2, (screenH - winH) / 2,
                                winW, winH,
                                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
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
                                   WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                   px, py, w, lineH,
                                   dlg, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(h, font);
    };

    auto dlgChk = [&](int id, const wchar_t* text, int px, int py, int w) {
        HWND h = CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                   px, py, w, lineH,
                                   dlg, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(h, font);
    };

    auto dlgButton = [&](int id, const wchar_t* text, int px, int py, int w, int h2) {
        HWND h = CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   px, py, w, h2,
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

    dlgLabel(L"命令/程序:", cx, y, labelW, btnH);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                     editX, y, cw - labelW - 5 - 80, btnH,
                     dlg, (HMENU)(LONG_PTR)IDC_EDT_HK_CMD, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(GetDlgItem(dlg, IDC_EDT_HK_CMD), font);
    dlgButton(IDC_BTN_HK_BROWSE, L"浏览", cx + cw - 75, y, 75, btnH);

    if (isEdit) {
        SetWindowTextW(GetDlgItem(dlg, IDC_EDT_HK_CMD), config.customHotkeys[editIndex].command.c_str());
    }
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

    {
        ModalGuard guard(hwnd);
        ShowWindow(dlg, SW_SHOW);
        UpdateWindow(dlg);

        MSG msg;
        while (IsWindow(dlg)) {
            BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
            if (ret <= 0) { if (ret == 0) PostQuitMessage((int)msg.wParam); break; }
            if ((msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) && ahd.capturing) {
                UINT mod = WindowUtils::GetCurrentModifiers();
                int vk = (int)msg.wParam;
                if (vk != VK_CONTROL && vk != VK_SHIFT && vk != VK_MENU && vk != VK_LWIN && vk != VK_RWIN) {
                    ahd.captureMod = mod;
                    ahd.captureVK = (UINT)vk;
                    ahd.capturing = false;
                    SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_CTRL), BM_SETCHECK, (mod & MOD_CONTROL) ? BST_CHECKED : BST_UNCHECKED, 0);
                    SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_SHIFT), BM_SETCHECK, (mod & MOD_SHIFT) ? BST_CHECKED : BST_UNCHECKED, 0);
                    SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_ALT), BM_SETCHECK, (mod & MOD_ALT) ? BST_CHECKED : BST_UNCHECKED, 0);
                    SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_WIN), BM_SETCHECK, (mod & MOD_WIN) ? BST_CHECKED : BST_UNCHECKED, 0);
                    SetWindowTextW(GetDlgItem(dlg, IDC_EDT_HK_KEY), AppConfig::VKToName(vk).c_str());
                    SetWindowTextW(GetDlgItem(dlg, IDC_BTN_HK_CAPTURE), L"捕获快捷键");
                }
                continue;
            }
            if (msg.message == WM_HOTKEY) { if (g_mainWnd && IsWindow(g_mainWnd)) SendMessageW(g_mainWnd, msg.message, msg.wParam, msg.lParam); continue; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
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
        std::wstring keyName = FormatHotKey(hk.mod, hk.vk);
        std::wstring cmd = hk.command.empty() ? L"" : hk.command;
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
    data->capturingHotKey = true;
    SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_SETHOTKEY), L"请按下快捷键...");
    SetFocus(hwnd);
}

// ===== 开始捕获一键关闭程序热键 =====
void SettingsDialog::StartCloseHotKeyCapture(HWND hwnd) {
    auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    data->capturingCloseHotKey = true;
    SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_SETCLOSEHOTKEY), L"请按下快捷键...");
    SetFocus(hwnd);
}

// ===== 重置窗口隐藏热键为默认值（Win+`） =====
void SettingsDialog::ResetHotKey(HWND hwnd) {
    auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    data->workingCopy.bossKeyMod = MOD_WIN;
    data->workingCopy.bossKeyVK = 0xC0;
    SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_SETHOTKEY),
                   FormatHotKey(MOD_WIN, 0xC0).c_str());
}

// ===== 重置一键关闭程序热键为默认值（Win+Esc） =====
void SettingsDialog::ResetCloseHotKey(HWND hwnd) {
    auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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
            if (SettingsDialog::ApplySettings(hwnd, data->workingCopy))
                DestroyWindow(hwnd);
            return 0;
        case IDCANCEL:
            // 取消：恢复原始配置，不保存、不生效，直接关闭对话框
            *data->config = data->originalConfig;
            DestroyWindow(hwnd);
            return 0;
        case IDAPPLY:
            // 应用：仅应用设置，不关闭对话框
            SettingsDialog::ApplySettings(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_ABOUT:
            // 关于按钮
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
            // 修复窗口堆叠：取消所有异常置顶的窗口
            int confirm = MessageBoxW(hwnd,
                L"此功能用于修复 Win+D 桌面保护引发的窗口 Z 轴错乱及异常置顶问题。\n请确认是否运行修复？",
                L"修复窗口堆叠",
                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
            if (confirm != IDYES) return 0;

            // 枚举所有置顶窗口，排除系统窗口后取消其置顶状态
            std::vector<HWND> topmostWindows;
            TopmostEnumData ed{ &topmostWindows };
            EnumWindows(CollectTopmostWindowsProc, reinterpret_cast<LPARAM>(&ed));

            // 将所有异常置顶窗口设置为非置顶
            for (HWND w : topmostWindows) {
                SetWindowPos(w, HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }

            wchar_t msg[128] = {};
            swprintf_s(msg, L"已修复 %zu 个异常置顶窗口", topmostWindows.size());
            MessageBoxW(hwnd, msg, L"修复完成", MB_OK | MB_ICONINFORMATION);
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
            SettingsDialog::AddDisabledKey(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_DELKEY:
            SettingsDialog::RemoveDisabledKey(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_ADDREMAP:
            SettingsDialog::AddRemapEntry(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_DELREMAP:
            SettingsDialog::RemoveRemapEntry(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_ADDBOSS:
            SettingsDialog::AddBossWindow(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_DELBOSS:
            SettingsDialog::RemoveBossWindow(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_ADD_BOSS_BINDING:
            SettingsDialog::ShowBindingDialog(hwnd, data->workingCopy, data->workingCopy.bossKeyWindows, ADD_BINDING_CLASS);
            return 0;
        case IDC_BTN_ADD_PROC_BINDING:
            SettingsDialog::AddProcessBinding(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_ADD_WIN_BINDING:
            SettingsDialog::AddWindowBinding(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_ADD_BINDING:
            SettingsDialog::AddBindingFromAll(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_DEL_BINDING:
            SettingsDialog::RemoveBindingToAll(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_REFRESH_WINS:
            SettingsDialog::RefreshAllWindowsList(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_ADD_WIND_BINDING:
            SettingsDialog::AddWindWindow(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_DELWIND:
            SettingsDialog::RemoveWindWindow(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_SELBOSS:
            SettingsDialog::SelectWindow(hwnd, IDC_LST_BOSSWINS, IDC_EDT_BOSSWIN);
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
        case IDC_BTN_SCANHOTKEYS:
            SettingsDialog::ScanHotkeys(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_PROBEHOTKEY:
            SettingsDialog::ProbeHotkey(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_DISABLEHOTKEY:
            SettingsDialog::DisableHotkey(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_ENABLEHOTKEY:
            SettingsDialog::EnableHotkey(hwnd, data->workingCopy);
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
            SettingsDialog::AddCustomHotkey(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_EDIT_CUSTOMHK:
            SettingsDialog::EditCustomHotkey(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_DEL_CUSTOMHK:
            SettingsDialog::DeleteCustomHotkey(hwnd, data->workingCopy);
            return 0;
        case IDC_CBO_HKFILTER:
            // 热键过滤下拉框选择变化时刷新列表
            if (HIWORD(wp) == CBN_SELCHANGE) {
                SettingsDialog::RefreshHotkeyList(hwnd, data->workingCopy);
            }
            return 0;
        case IDC_EDT_HKSEARCH:
            // 热键搜索框内容变化时实时刷新列表
            if (HIWORD(wp) == EN_CHANGE) {
                SettingsDialog::RefreshHotkeyList(hwnd, data->workingCopy);
            }
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
        // 热键列表右键菜单
        if (nmhdr->idFrom == IDC_LV_HOTKEYS && nmhdr->code == NM_RCLICK) {
            auto* nmitem = reinterpret_cast<NMITEMACTIVATE*>(lp);
            if (nmitem->iItem >= 0) {
                HWND lv = GetDlgItem(hwnd, IDC_LV_HOTKEYS);
                // 经 lParam 取回真实数据下标（显示行号在过滤/搜索后会错位）
                LVITEMW rcItem = {};
                rcItem.mask = LVIF_PARAM;
                rcItem.iItem = nmitem->iItem;
                int hkIdx = -1;
                if (SendMessageW(lv, LVM_GETITEMW, 0, (LPARAM)&rcItem)) {
                    hkIdx = (int)rcItem.lParam;
                }
                if (hkIdx >= 0 && hkIdx < (int)data->scannedHotkeys.size()) {
                    SendMessageW(lv, LVM_SETITEMSTATE, nmitem->iItem, MAKELPARAM(LVIS_SELECTED, LVIS_SELECTED));

                    auto& hk = data->scannedHotkeys[hkIdx];
                    bool isDisabled = false;
                    for (auto& d : data->workingCopy.disabledHotkeys) {
                        if (d.first == hk.mod && d.second == hk.vk) { isDisabled = true; break; }
                    }

                    HMENU hMenu = CreatePopupMenu();
                    if (isDisabled) {
                        AppendMenuW(hMenu, MF_STRING, 1, L"启用该热键");
                    } else {
                        AppendMenuW(hMenu, MF_STRING, 2, L"禁用该热键");
                    }
                    if (IsSafeHotkeyToSimulate(hk.mod, hk.vk)) {
                        AppendMenuW(hMenu, MF_STRING, 3, L"探测所属程序");
                    }

                    POINT pt = nmitem->ptAction;
                    ClientToScreen(lv, &pt);
                    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                    DestroyMenu(hMenu);

                    if (cmd == 1) SettingsDialog::EnableHotkey(hwnd, data->workingCopy);
                    else if (cmd == 2) SettingsDialog::DisableHotkey(hwnd, data->workingCopy);
                    else if (cmd == 3) SettingsDialog::ProbeHotkey(hwnd, data->workingCopy);
                }
            }
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
        // 关闭窗口时恢复原始配置
        *data->config = data->originalConfig;
        if (SettingsDialog::s_applyCallback) SettingsDialog::s_applyCallback();
        DestroyWindow(hwnd);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

