// ===== 添加绑定对话框 =====
// 支持按进程名、窗口名、手动输入三种方式为窗口隐藏/窗口置顶添加绑定。
// 从 SettingsDialog.cpp 拆出，行为保持不变。

#include "SettingsDialog.h"
#include "SettingsDialogSubDialogs.h"
#include "SettingsDialogPages.h"
#include "Utils.h"
#include "FluentTheme.h"
#include "Lang.h"

// ADD_BINDING_CLASS 由 SettingsDialogSubDialogs.h 提供（主对话框需要同一类名）

// 添加绑定对话框数据
struct AddBindingData {
    std::vector<BoundWindowInfo>* allWindowsCache;  // 所有可选窗口缓存
    std::vector<BoundWindowInfo>* boundWindows;      // 已绑定窗口列表
    bool changed;                                     // 绑定是否有变更
};

// 所有添加分支共用数量上限检查：按进程/按窗口/手动输入三个入口
// 必须一致，否则超过 64 条的绑定会在下次加载时被静默丢弃。
static bool CanAddBinding(HWND hwnd, AddBindingData* abd) {
    if (!abd || !abd->boundWindows) return false;
    if ((int)abd->boundWindows->size() < MAX_BOUND_WINDOWS) return true;
    Fluent::ShowMessageBox(hwnd, L"绑定数量已达上限（64 个），请先删除不需要的绑定", L"提示",
                           MB_OK | MB_ICONWARNING);
    return false;
}

// ===== 添加绑定对话框窗口过程 =====
// 处理按进程绑定、按窗口绑定、手动添加等操作
static LRESULT CALLBACK AddBindingWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) { EraseWindowBackground(hwnd, wp); return 1; }
    // DPI 处理不依赖 USERDATA，必须在取 abd 之前处理
    if (msg == WM_NCDESTROY) {
        CleanupSubDialogDpiState(hwnd);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (msg == WM_DPICHANGED) {
        HandleSubDialogDpiChanged(hwnd, wp, lp, 14, FW_NORMAL);
        return 0;
    }
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
            WindowEnum::EnumWindowCtx ctx{ abd->allWindowsCache, abd->boundWindows, lst,
                                           {}, GetTickCount64() + WindowEnum::WINDOW_ENUM_TITLE_BUDGET_MS };
            WindowEnum::EnumerateUnboundWindows(ctx);
            if (ctx.budgetExceeded) {
                Fluent::ShowMessageBox(hwnd, L"部分窗口因目标无响应未能读取标题，列表中可能缺少这些窗口。",
                                       L"提示", MB_OK | MB_ICONINFORMATION);
            }
            return 0;
        }
        case IDC_BTN_ADD_PROC_BINDING: {
            // 按进程名绑定：将选中窗口的进程添加到绑定列表
            HWND lst = GetDlgItem(hwnd, IDC_LST_ALL_WINDOWS);
            int sel = (int)SendMessageW(lst, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR || sel >= (int)abd->allWindowsCache->size()) {
                Fluent::ShowMessageBox(hwnd, L"请先从窗口列表中选择一个窗口", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            auto& info = (*abd->allWindowsCache)[sel];
            if (info.processName.empty()) {
                Fluent::ShowMessageBox(hwnd, L"无法获取该窗口的进程名称", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            for (const auto& b : *abd->boundWindows) {
                if (b.matchMode == MatchMode::Process && _wcsicmp(b.processName.c_str(), info.processName.c_str()) == 0) {
                    Fluent::ShowMessageBox(hwnd, L"该进程已在绑定列表中", L"提示", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
            }
            if (!CanAddBinding(hwnd, abd)) return 0;
            BoundWindowInfo binding;
            binding.matchMode = MatchMode::Process;
            binding.processName = info.processName;
            binding.processPath = info.processPath;
            binding.ClampFields();
            abd->boundWindows->push_back(binding);
            abd->changed = true;
            // 进程名是用户数据：固定标签翻译，名称原样展示（R9-M5）
            const std::wstring text = std::wstring(Tr(L"已添加进程绑定: ")) + info.processName;
            Fluent::ShowMessageBoxRaw(hwnd, text.c_str(), Tr(L"添加成功"), MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        case IDC_BTN_ADD_WIN_BINDING: {
            // 按窗口名绑定：将选中窗口的标题添加到绑定列表
            HWND lst = GetDlgItem(hwnd, IDC_LST_ALL_WINDOWS);
            int sel = (int)SendMessageW(lst, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR || sel >= (int)abd->allWindowsCache->size()) {
                Fluent::ShowMessageBox(hwnd, L"请先从窗口列表中选择一个窗口", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            auto& info = (*abd->allWindowsCache)[sel];
            if (info.title.empty()) {
                Fluent::ShowMessageBox(hwnd, L"该窗口没有标题，无法按窗口名绑定", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            for (const auto& b : *abd->boundWindows) {
                if (b.matchMode == MatchMode::Window && b.title == info.title) {
                    Fluent::ShowMessageBox(hwnd, L"该窗口已在绑定列表中", L"提示", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
            }
            if (!CanAddBinding(hwnd, abd)) return 0;
            BoundWindowInfo binding;
            binding.matchMode = MatchMode::Window;
            binding.title = info.title;
            binding.processName = info.processName;
            binding.processPath = info.processPath;
            binding.pid = info.pid;
            binding.ClampFields();
            abd->boundWindows->push_back(binding);
            abd->changed = true;
            // 窗口标题是用户数据：固定标签翻译，标题原样展示（R9-M5）
            const std::wstring text = std::wstring(Tr(L"已添加窗口绑定: ")) + info.title;
            Fluent::ShowMessageBoxRaw(hwnd, text.c_str(), Tr(L"添加成功"), MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        case IDC_BTN_ADDBOSS: {
            // 手动添加：读取编辑框中的窗口标题并添加到绑定列表
            HWND edt = GetDlgItem(hwnd, IDC_EDT_BOSSWIN);
            std::wstring name = ReadWindowText(edt);
            // 去首尾空白：全空白或单字符标题会因“标题包含”匹配到大量无关窗口
            const size_t first = name.find_first_not_of(L" \t\r\n");
            const size_t last = name.find_last_not_of(L" \t\r\n");
            if (first == std::wstring::npos) {
                Fluent::ShowMessageBox(hwnd, L"请输入至少 2 个字符的窗口标题", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            name = name.substr(first, last - first + 1);
            if (name.size() < 2) {
                Fluent::ShowMessageBox(hwnd, L"窗口标题至少需要 2 个字符，避免匹配过多窗口", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            if (name.size() > MAX_BINDING_TITLE) name.resize(MAX_BINDING_TITLE);
            // 数量上限：避免一次隐藏/置顶影响过多窗口，也防止 INI 无界增长
            if ((int)abd->boundWindows->size() >= MAX_BOUND_WINDOWS) {
                Fluent::ShowMessageBox(hwnd, L"绑定数量已达上限（64 个），请先删除不需要的绑定", L"提示",
                             MB_OK | MB_ICONWARNING);
                return 0;
            }
            for (const auto& w : *abd->boundWindows) {
                if (w.title == name) {
                    Fluent::ShowMessageBox(hwnd, L"该窗口已在列表中", L"提示", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
            }
            BoundWindowInfo binding;
            binding.matchMode = MatchMode::Window;
            binding.title = name;
            abd->boundWindows->push_back(binding);
            abd->changed = true;
            SetWindowTextW(edt, L"");
            // 手动输入的标题是用户数据：固定标签翻译，标题原样展示（R9-M5）
            const std::wstring text = std::wstring(Tr(L"已添加窗口绑定: ")) + name;
            Fluent::ShowMessageBoxRaw(hwnd, text.c_str(), Tr(L"添加成功"), MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case DM_GETDEFID:
        // 非对话框窗口：声明默认按钮，Enter 可触发「确定」
        return MAKELRESULT(IDOK, DC_HASDEFID);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, IsWindowEnabled((HWND)lp) ? Fluent::TextColor() : Fluent::TextDisabledColor());
        return (LRESULT)Fluent::SurfaceBrush();
    }
    case WM_CTLCOLOREDIT: {
        // 编辑框必须不透明：按静态控件用透明背景会让输入/选中/横向滚动留下残影
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, Fluent::SurfaceColor());
        SetTextColor(hdc, IsWindowEnabled((HWND)lp) ? Fluent::TextColor() : Fluent::TextDisabledColor());
        return (LRESULT)Fluent::SurfaceBrush();
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
        // 背景由 WM_ERASEBKGND 用主题画刷绘制（不随系统浅色/深色固定为白色）
        wc.hbrBackground = nullptr;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // 注册失败时不得标记成功：否则窗口类始终未注册，
        // CreateWindowExW 会持续返回空且不再重试（ERROR_CLASS_ALREADY_EXISTS 视为成功）
        if (!WindowUtils::RegisterClassOnce(wc)) return;
        *pRegistered = true;
    }

    AddBindingData abd;
    abd.allWindowsCache = &data->allWindowsCache;
    abd.boundWindows = &boundWindows;
    abd.changed = false;

    RECT rc = { 0, 0, ScaleUi(520), ScaleUi(520) };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int winX = 0, winY = 0;
    WindowUtils::ClampWindowToWorkArea(&winX, &winY, &winW, &winH);

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, className,
                                L"添加绑定",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                                winX, winY,
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
                                     WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                     cx, y, cw, 20, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(lblHint1, font);
    y += 22;

    HWND lblHint2 = CreateWindowExW(0, L"STATIC", L"按窗口名绑定：仅匹配包含该标题的窗口",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                     cx, y, cw, 20, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(lblHint2, font);
    y += 30;

    HWND lblManual = CreateWindowExW(0, L"STATIC", L"手动添加窗口标题 (标题包含该文字的所有窗口都会匹配):",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                      cx, y, cw, 24, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(lblManual, font);
    y += 28;

    HWND edtManual = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                                      cx, y, cw - 90, 28,
                                      dlg, (HMENU)IDC_EDT_BOSSWIN, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(edtManual, font);
    SendMessageW(edtManual, EM_SETLIMITTEXT, MAX_BINDING_TITLE, 0);

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
    Fluent::ApplyWindowChrome(dlg);
    Fluent::DecorateDialog(dlg);
    ApplyLanguageToWindowTree(dlg);

    WindowEnum::EnumWindowCtx ctx{ &data->allWindowsCache, &boundWindows, lstAll,
                                   {}, GetTickCount64() + WindowEnum::WINDOW_ENUM_TITLE_BUDGET_MS };
    WindowEnum::EnumerateUnboundWindows(ctx);
    // 提示框必须等子对话框显示、父窗口进入模态后再弹，否则父窗口仍可操作，
    // 用户可能在提示框期间销毁设置窗口并留下无父对话框（R14-A6）
    const bool titleBudgetExceeded = ctx.budgetExceeded;

    {
        ModalGuard guard(hwnd);
        ShowWindow(dlg, SW_SHOW);
        UpdateWindow(dlg);
        if (titleBudgetExceeded) {
            Fluent::ShowMessageBox(dlg, L"部分窗口因目标无响应未能读取标题，列表中可能缺少这些窗口。",
                                   L"提示", MB_OK | MB_ICONINFORMATION);
        }

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
