// ===== 虚拟键盘选择器 =====
// 用于按键映射与自定义热键中选取目标按键的可视化键盘界面。
// 从 SettingsDialog.cpp 拆出，行为保持不变。

#include "SettingsDialog.h"
#include "SettingsDialogSubDialogs.h"
#include "Utils.h"
#include "FluentTheme.h"
#include "Lang.h"
#include <string>

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
    if (msg == WM_ERASEBKGND) { EraseWindowBackground(hwnd, wp); return 1; }
    // DPI 处理不依赖 USERDATA，必须在取 pd 之前处理
    if (msg == WM_NCDESTROY) {
        CleanupSubDialogDpiState(hwnd);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (msg == WM_DPICHANGED) {
        // 虚拟键盘按钮按 12px 字体创建，跨 DPI 后保持一致
        HandleSubDialogDpiChanged(hwnd, wp, lp, 12, FW_NORMAL);
        return 0;
    }
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
                    Fluent::ShowMessageBox(hwnd, L"修饰键不能作为组合键的主键。\n请先勾选下方的 Ctrl/Shift/Alt/Win，再点击要使用的主键。",
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
    case DM_GETDEFID:
        // 非对话框窗口：声明默认按钮，Enter 可触发「确定」
        return MAKELRESULT(IDOK, DC_HASDEFID);
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC: {
        // 设置按钮和静态控件的背景为透明过渡，底色为 Fluent 白色内容面
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, IsWindowEnabled((HWND)lp) ? Fluent::TextColor() : Fluent::TextDisabledColor());
        return (LRESULT)Fluent::SurfaceBrush();
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
        // 背景由 WM_ERASEBKGND 用主题画刷绘制（不随系统浅色/深色固定为白色）
        wc.hbrBackground = nullptr;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // 注册失败时不得标记成功：否则窗口类始终未注册，
        // CreateWindowExW 会持续返回空且不再重试（ERROR_CLASS_ALREADY_EXISTS 视为成功）
        if (!WindowUtils::RegisterClassOnce(wc)) return 0;
        registered = true;
    }

    // 计算窗口尺寸并居中显示（高度预留底部"取消"按钮，按系统 DPI 放大）
    RECT rc = { 0, 0, ScaleUi(710), ScaleUi(292) };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int winX = 0, winY = 0;
    WindowUtils::ClampWindowToWorkArea(&winX, &winY, &winW, &winH);

    KbPickerData pd = { 0, 0, allowMods, false };

    // WS_EX_CONTROLPARENT：非对话框窗口承载控件时，IsDialogMessage 需要它
    // 才能在子控件之间正确进行 Tab 导航
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, KB_PICKER_CLASS,
                                 allowMods ? L"选择目标按键或组合键" : L"选择映射按键",
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                                 winX, winY,
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
    Fluent::ApplyWindowChrome(hwnd);
    Fluent::DecorateDialog(hwnd);
    ApplyLanguageToWindowTree(hwnd);

    // 模态消息循环
    {
        ModalGuard guard(parent);
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
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
