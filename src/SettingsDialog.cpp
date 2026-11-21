// ===== SettingsDialog.cpp - 设置对话框实现 =====
// 包含键客 KeySentry 设置对话框的所有界面创建、事件处理和配置管理逻辑。
// 对话框包含六个选项卡：常规、按键屏蔽、按键映射、热键管理、窗口隐藏、窗口置顶。

#include "SettingsDialog.h"
#include "SettingsDialogPages.h"
#include "Lang.h"
#include "SettingsDialogSubDialogs.h"
#include "SettingsDialogInlineCapture.h"
#include "Resource.h"
#include "Utils.h"
#include "WinDGuard.h"
#include "FluentTheme.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>

// 外部函数声明
extern void ShowAboutDialog(HWND parent);   // 显示关于对话框
extern void SetSettingsWnd(HWND wnd);       // 设置设置窗口句柄到全局，供主窗口消息处理使用

// ===== 静态回调指针初始化 =====
void (*SettingsDialog::s_applyCallback)() = nullptr;
void (*SettingsDialog::s_captureModeCallback)(bool) = nullptr;
void (*SettingsDialog::s_uiPreferenceCallback)() = nullptr;

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

// ===== 设置外观/语言偏好回调 =====
void SettingsDialog::SetUiPreferenceCallback(void (*callback)()) {
    s_uiPreferenceCallback = callback;
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
// 字体统一由 FluentTheme 提供（Segoe UI，按 DPI 缩放）；
// GetDlgFont/GetSmallFont/SetControlFont 由 SettingsDialogPages.h 提供，
// 页面构建与主对话框共用同一实现。主题字体常驻进程，按 DPI 重建，
// 不需要（也不能）在关闭对话框时删除，避免仍被其他窗口引用。

// ===== 清理选项卡页面控件 =====
// 销毁除选项卡控件和底部按钮之外的所有子控件
static void CleanupTabPageControls(HWND hwnd) {
    HWND child = GetWindow(hwnd, GW_CHILD);
    while (child) {
        HWND next = GetWindow(child, GW_HWNDNEXT);
        LONG_PTR id = GetWindowLongPtrW(child, GWLP_ID);
        // 保留选项卡、确定/取消/应用/关于按钮及外观/语言图标按钮，销毁其余控件
        if (id != IDC_TAB_MAIN && id != IDOK && id != IDCANCEL && id != IDAPPLY &&
            id != IDC_BTN_ABOUT && id != IDC_BTN_THEME && id != IDC_BTN_LANG) {
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

// ===== 重新排布选项卡与底部按钮 =====
// DPI 变化时窗口客户区尺寸改变，选项卡、按钮需要按新缩放重新定位；
// 页面控件由 SwitchTab 销毁重建，不在这里处理。
static void LayoutDialogChrome(HWND hwnd, HFONT font) {
    HWND tab = GetDlgItem(hwnd, IDC_TAB_MAIN);
    if (!tab) return;
    MoveWindow(tab, ScaleUi(TAB_X), ScaleUi(TAB_Y),
               ScaleUi(TAB_W), ScaleUi(TAB_H), TRUE);
    SetControlFont(tab, font);

    int pageX = 0, pageY = 0, pageW = 0, pageH = 0;
    GetPageArea(tab, hwnd, &pageX, &pageY, &pageW, &pageH);
    const int tableRight = pageX + pageW - 10;

    struct ButtonLayout { int id; int logicalX; };
    const int iconBtnW = BTN_H;   // 方形小图标按钮
    const ButtonLayout buttons[] = {
        { IDC_BTN_ABOUT, pageX },
        { IDC_BTN_THEME, pageX + BTN_W + 8 },
        { IDC_BTN_LANG,  pageX + BTN_W + 8 + iconBtnW + 6 },
        { IDOK,          tableRight - BTN_W * 3 - 20 },
        { IDCANCEL,      tableRight - BTN_W * 2 - 10 },
        { IDAPPLY,       tableRight - BTN_W },
    };
    for (const auto& b : buttons) {
        HWND h = GetDlgItem(hwnd, b.id);
        if (!h) continue;
        const int w = (b.id == IDC_BTN_THEME || b.id == IDC_BTN_LANG) ? iconBtnW : BTN_W;
        MoveWindow(h, ScaleUi(b.logicalX), ScaleUi(BTN_Y),
                   ScaleUi(w), ScaleUi(BTN_H), TRUE);
        SetControlFont(h, font);
    }
}

// ===== 缩放当前选项卡页面控件 =====
// 页面控件仍按逻辑坐标创建，构建完成后一次性放大到物理坐标；
// 选项卡与底部按钮已按物理坐标创建，必须跳过，否则会被二次放大。
static void ScalePageControls(HWND hwnd) {
    static const LONG_PTR reserved[] = { IDC_TAB_MAIN, IDOK, IDCANCEL, IDAPPLY,
                                         IDC_BTN_ABOUT, IDC_BTN_THEME, IDC_BTN_LANG };
    ScaleChildWindows(hwnd, GetUiScale(), reserved, (int)(sizeof(reserved) / sizeof(reserved[0])));
}

// ===== 页面 z 序整理 =====
// 选项卡窗口覆盖整个页面区域，必须排在所有页面控件之下。
// 页面控件与分组框卡片都在选项卡之后创建，本就位于其上方；
// 这里只需把选项卡压到 z 序最底部，不能反过来插卡片——SetWindowPos 的
// hWndInsertAfter 是把窗口放到目标窗口*下方*，会把卡片埋到选项卡背后。
// 也不能把控件逐个提到顶层：那会打乱 Tab 键顺序与单选按钮分组（二者都依赖 z 序）。
static void ReorderPageZOrder(HWND hwnd) {
    HWND tab = GetDlgItem(hwnd, IDC_TAB_MAIN);
    if (!tab) return;

    SetWindowPos(tab, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // 这里不做同步整窗重绘：本页控件此时仍是隐藏状态，强制 RDW_UPDATENOW
    // 会先擦出一帧只有空背景的页面（切页闪白），随后显示控件再画一次。
    // 统一由 SwitchTab 末尾的一次异步 InvalidateRect 完成刷新。
}

// ===== 初始焦点 =====
// 放到当前页面左上角第一个可聚焦控件，避免一打开设置就落到右下角「关于」，
// 也避免首次 Tab 从窗口末尾开始遍历
static void FocusFirstPageControl(HWND hwnd) {
    HWND best = nullptr;
    LONG bestY = LONG_MAX, bestX = LONG_MAX;
    for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        const LONG_PTR id = GetWindowLongPtrW(child, GWLP_ID);
        if (id == IDC_TAB_MAIN || id == IDOK || id == IDCANCEL || id == IDAPPLY ||
            id == IDC_BTN_ABOUT || id == IDC_BTN_THEME || id == IDC_BTN_LANG)
            continue;
        if (!IsWindowVisible(child) || !IsWindowEnabled(child)) continue;
        if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_TABSTOP)) continue;
        RECT rc;
        if (!GetWindowRect(child, &rc)) continue;
        if (rc.top < bestY || (rc.top == bestY && rc.left < bestX)) {
            best = child;
            bestY = rc.top;
            bestX = rc.left;
        }
    }
    if (best) SetFocus(best);
}

// ===== 外观 / 语言图标按钮 =====
// 这两个按钮是「关于」右侧的快捷开关：衣服图标循环外观，地球图标切换语言。
// 使用 BS_OWNERDRAW 自绘，避免引入图标资源，也便于跟随 Fluent 主题配色。
static const wchar_t* ThemeModeName(int mode) {
    switch (mode) {
    case 1: return L"浅色";
    case 2: return L"深色";
    default: return L"跟随系统";
    }
}

static std::wstring ThemeTipText(const AppConfig& config) {
    // 明确说明快捷开关立即保存：与页面内其他设置的「取消」语义不同（R8-A6）
    return std::wstring(Tr(L"外观：")) + Tr(ThemeModeName(config.appTheme)) +
           Tr(L"（点击切换，立即生效并保存）");
}

static std::wstring LanguageTipText(const AppConfig& config) {
    return std::wstring(Tr(L"语言：")) +
           (config.appLanguage == 1 ? L"English" : L"中文") +
           Tr(L"（点击切换，立即生效并保存）");
}

static HWND CreateIconTip(HWND owner, HWND tool, const std::wstring& text) {
    HWND tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                               WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                               owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!tip) return nullptr;
    TOOLINFOW ti = {};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = owner;
    ti.uId = (UINT_PTR)tool;
    ti.lpszText = const_cast<LPWSTR>(text.c_str());
    SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, (LPARAM)ScaleUi(320));
    return tip;
}

static void UpdateIconTips(HWND hwnd, SettingsDialog::DialogData* data) {
    if (!data) return;
    HWND themeBtn = GetDlgItem(hwnd, IDC_BTN_THEME);
    HWND langBtn = GetDlgItem(hwnd, IDC_BTN_LANG);
    if (data->themeTip && themeBtn) {
        TOOLINFOW ti = {};
        ti.cbSize = sizeof(ti);
        ti.hwnd = hwnd;
        ti.uId = (UINT_PTR)themeBtn;
        const std::wstring text = ThemeTipText(data->workingCopy);
        ti.lpszText = const_cast<LPWSTR>(text.c_str());
        SendMessageW(data->themeTip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    }
    if (data->langTip && langBtn) {
        TOOLINFOW ti = {};
        ti.cbSize = sizeof(ti);
        ti.hwnd = hwnd;
        ti.uId = (UINT_PTR)langBtn;
        const std::wstring text = LanguageTipText(data->workingCopy);
        ti.lpszText = const_cast<LPWSTR>(text.c_str());
        SendMessageW(data->langTip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    }
}

// 自绘图标：先以 4 倍分辨率绘制再缩小，得到平滑的曲线边缘。
// GDI 没有内建抗锯齿，直接画 1px 折线会显得毛糙；超采样后使用 HALFTONE
// 缩放可在不引入 GDI+ 依赖的前提下显著提升图标质感。
static void PaintIconGlyph(HDC target, int cx, int cy, int logicalSize,
                           bool themeIcon, COLORREF lineColor, COLORREF bgColor) {
    const int size = ScaleUi(logicalSize);
    if (size < 8) return;
    const int ss = 4;
    const int big = size * ss;

    HDC mem = CreateCompatibleDC(target);
    HBITMAP bmp = mem ? CreateCompatibleBitmap(target, big, big) : nullptr;
    if (!mem || !bmp) {
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    RECT all = { 0, 0, big, big };
    HBRUSH bg = CreateSolidBrush(bgColor);
    FillRect(mem, &all, bg);
    DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);

    // 以 24×24 逻辑网格设计图标，按超采样比例映射到离屏位图
    const double k = (double)big / 24.0;
    auto X = [k](double v) { return (int)std::lround(v * k); };

    LOGBRUSH lb = {};
    lb.lbStyle = BS_SOLID;
    lb.lbColor = lineColor;
    const int penW = (std::max)(1, (int)std::lround(1.05 * k));
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_JOIN_ROUND | PS_ENDCAP_ROUND,
                            penW, &lb, 0, nullptr);
    HGDIOBJ oldPen = SelectObject(mem, pen);
    HGDIOBJ oldBrush = SelectObject(mem, GetStockObject(NULL_BRUSH));

    if (themeIcon) {
        // T 恤填充色跟随当前外观模式：浅色=浅衣服，深色=深衣服，
        // 跟随系统=左深右浅，便于从图标直接判断当前模式
        const COLORREF lightFill = RGB(236, 236, 236);
        const COLORREF darkFill = RGB(58, 58, 58);

        struct DesignPoint { double x, y; };
        const DesignPoint design[] = {
            { 6.0,  3.6}, { 9.4,  2.6}, {10.8,  5.0}, {12.0,  6.0},
            {13.2,  5.0}, {14.6,  2.6}, {18.0,  3.6}, {21.4,  7.4},
            {18.2, 10.6}, {16.0,  9.0}, {16.0, 20.6}, { 8.0, 20.6},
            { 8.0,  9.0}, { 5.8, 10.6}, { 2.6,  7.4},
        };
        POINT pts[(int)(sizeof(design) / sizeof(design[0]))] = {};
        for (int i = 0; i < (int)(sizeof(design) / sizeof(design[0])); ++i) {
            pts[i].x = X(design[i].x);
            pts[i].y = X(design[i].y);
        }

        auto fillHalf = [&](COLORREF fillColor, int clipLeft, int clipRight) {
            HBRUSH brush = CreateSolidBrush(fillColor);
            const int saved = SaveDC(mem);
            SelectObject(mem, brush);
            SelectObject(mem, GetStockObject(NULL_PEN));
            IntersectClipRect(mem, clipLeft, 0, clipRight, big);
            Polygon(mem, pts, (int)(sizeof(pts) / sizeof(pts[0])));
            RestoreDC(mem, saved);
            DeleteObject(brush);
        };

        const Fluent::ThemeMode mode = Fluent::GetThemeMode();
        if (mode == Fluent::ThemeMode::System) {
            fillHalf(darkFill, 0, big / 2);
            fillHalf(lightFill, big / 2, big);
        } else {
            fillHalf(mode == Fluent::ThemeMode::Dark ? darkFill : lightFill, 0, big);
        }

        // 轮廓与领口统一在填充之后绘制，避免被填充覆盖
        SelectObject(mem, pen);
        SelectObject(mem, GetStockObject(NULL_BRUSH));
        Polygon(mem, pts, (int)(sizeof(pts) / sizeof(pts[0])));

        POINT collar[4] = {
            { X(9.4),  X(2.6) },
            { X(10.6), X(6.4) },
            { X(13.4), X(6.4) },
            { X(14.6), X(2.6) },
        };
        PolyBezier(mem, collar, 4);
    } else {
        // 地球：外圆 + 中央经线 + 赤道 + 两条纬线，形成简洁的经纬网格
        Ellipse(mem, X(3.2), X(3.2), X(20.8), X(20.8));
        Ellipse(mem, X(8.0), X(3.2), X(16.0), X(20.8));
        MoveToEx(mem, X(3.2), X(12.0), nullptr);
        LineTo(mem, X(20.8), X(12.0));

        POINT northLat[4] = {
            { X(4.6),  X(9.1) },
            { X(8.0),  X(7.3) },
            { X(16.0), X(7.3) },
            { X(19.4), X(9.1) },
        };
        PolyBezier(mem, northLat, 4);
        POINT southLat[4] = {
            { X(4.6),  X(14.9) },
            { X(8.0),  X(16.7) },
            { X(16.0), X(16.7) },
            { X(19.4), X(14.9) },
        };
        PolyBezier(mem, southLat, 4);
    }

    SelectObject(mem, oldBrush);
    SelectObject(mem, oldPen);
    DeleteObject(pen);

    SetStretchBltMode(target, HALFTONE);
    SetBrushOrgEx(target, 0, 0, nullptr);
    StretchBlt(target, cx - size / 2, cy - size / 2, size, size,
               mem, 0, 0, big, big, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// 自绘图标：衣服（外观）与地球（语言）
static void PaintIconButton(const DRAWITEMSTRUCT* dis) {
    if (!dis) return;
    HDC hdc = dis->hDC;
    const RECT rc = dis->rcItem;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const COLORREF bg = pressed ? Fluent::DisabledBackgroundColor()
                                : Fluent::SurfaceColor();
    Fluent::FillRoundRect(hdc, rc, ScaleUi(6), bg, Fluent::GridLineColor());

    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;
    PaintIconGlyph(hdc, cx, cy, 21, dis->CtlID == IDC_BTN_THEME,
                   Fluent::TextColor(), bg);

    // 键盘焦点提示：自绘按钮没有系统焦点框，Tab 到达时必须自行绘制，
    // 否则纯键盘用户无法确认当前焦点（R8-M10）
    if (dis->itemState & ODS_FOCUS) {
        RECT fr = rc;
        InflateRect(&fr, -ScaleUi(2), -ScaleUi(2));
        DrawFocusRect(hdc, &fr);
    }
}

// 语言切换后重建设置窗口的静态文案与当前页
static void RebuildForLanguage(HWND hwnd) {
    auto* data = reinterpret_cast<SettingsDialog::DialogData*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!data) return;

    SetWindowTextW(hwnd, Tr(L"键客 设置"));
    HWND tab = GetDlgItem(hwnd, IDC_TAB_MAIN);
    if (tab) {
        static const wchar_t* const titles[] = {
            L"常规", L"按键屏蔽", L"按键映射", L"热键管理", L"窗口隐藏", L"窗口置顶"
        };
        for (int i = 0; i < (int)(sizeof(titles) / sizeof(titles[0])); ++i) {
            TCITEMW item = {};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<LPWSTR>(Tr(titles[i]));
            TabCtrl_SetItem(tab, i, &item);
        }
    }
    SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_ABOUT), Tr(L"关于"));
    SetWindowTextW(GetDlgItem(hwnd, IDOK), Tr(L"确定"));
    SetWindowTextW(GetDlgItem(hwnd, IDCANCEL), Tr(L"取消"));
    SetWindowTextW(GetDlgItem(hwnd, IDAPPLY), Tr(L"应用"));
    // 图标按钮的无障碍名称同样随语言切换
    SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_THEME), Tr(L"外观"));
    SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_LANG), Tr(L"语言"));
    UpdateIconTips(hwnd, data);

    // 销毁并按当前语言重建页面控件
    if (data->currentTab >= 0) {
        data->workingCopy.tabState = data->currentTab;
        SettingsDialog::SaveCurrentTabState(hwnd, data->workingCopy);
        SettingsDialog::SwitchTab(hwnd, data->currentTab, data->workingCopy);
    }
    ApplyLanguageToWindowTree(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
}

// ===== 设置对话框主窗口过程（前向声明） =====
static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// ===== 窗口类名常量 =====
static const wchar_t* SETTINGS_CLASS = L"KeySentrySettingsDlg";

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
        // 背景由 WM_ERASEBKGND 用主题画刷绘制（不随系统浅色/深色固定为白色）
        wc.hbrBackground = nullptr;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // 注册失败时不得标记成功：否则窗口类始终未注册，
        // CreateWindowExW 会持续返回空且不再重试（ERROR_CLASS_ALREADY_EXISTS 视为成功）
        if (!WindowUtils::RegisterClassOnce(wc)) return 0;

        registered = true;
    }

    // 按目标显示器 DPI 设置线程级缩放：设置窗口在副屏打开时按该屏
    // 的 DPI 布局，而不是始终使用主屏/系统 DPI。
    HWND targetForDpi = (parent && IsWindowVisible(parent)) ? parent : nullptr;
    RECT targetWork = {};
    DpiUtils::GetWorkAreaForWindow(targetForDpi, &targetWork);
    UINT targetDpi = 0;
    if (targetForDpi) {
        targetDpi = DpiUtils::GetDpiForWindowSafe(targetForDpi);
    } else {
        POINT pt = { 0, 0 };
        GetCursorPos(&pt);
        targetDpi = DpiUtils::GetDpiForMonitorSafe(
            MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY));
    }
    ScopedUiScale scopedScale(DpiUtils::ScaleForDpi(targetDpi, &targetWork));

    // 计算窗口尺寸并居中（按目标显示器 DPI 放大，保证高分辨率下不偏小）
    RECT rc = { 0, 0, ScaleUi(CLIENT_W), ScaleUi(CLIENT_H) };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int winX = 0, winY = 0;
    // 在目标显示器工作区内居中，避免设置窗口总是出现在主屏
    WindowUtils::CenterWindowInWorkArea(targetWork, &winX, &winY, &winW, &winH);

    // 创建设置对话框主窗口
    // WS_EX_CONTROLPARENT：非对话框窗口承载控件时，IsDialogMessage 才能在其子控件间遍历
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, SETTINGS_CLASS,
                                 Tr(L"键客 设置"),
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                                 winX, winY,
                                 winW, winH,
                                 parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    // 窗口创建失败（极端资源耗尽）：直接返回，避免以空句柄创建子控件产生孤儿窗口
    if (!hwnd) return 0;

    // 实际落点显示器 DPI 与预估不一致时（如窗口被系统夹到相邻屏幕），
    // 按实际 DPI 重新设置缩放并调整客户区尺寸
    const UINT actualDpi = DpiUtils::GetDpiForWindowSafe(hwnd);
    if (actualDpi != targetDpi) {
        UiScaleOverrideStorage() = DpiUtils::ScaleForDpi(actualDpi, &targetWork);
        RECT rc2 = { 0, 0, ScaleUi(CLIENT_W), ScaleUi(CLIENT_H) };
        AdjustWindowRectEx(&rc2, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE,
                           WS_EX_DLGMODALFRAME);
        int w2 = rc2.right - rc2.left;
        int h2 = rc2.bottom - rc2.top;
        int x2 = winX, y2 = winY;
        WindowUtils::CenterWindowInWorkArea(targetWork, &x2, &y2, &w2, &h2);
        SetWindowPos(hwnd, nullptr, x2, y2, w2, h2, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // 若主题字体已按进程 DPI 创建（上一次关闭时重建），必须按本次目标显示器
    // 的缩放再重建一次，否则控件放大而文字仍沿用旧字号（R14-M6）。
    // 字体尚未创建时该调用为空操作，由下面的 GetDlgFont 按当前缩放创建。
    Fluent::RebuildFontsForDpi();

    // Windows 11 下启用系统圆角（其他系统为空操作）
    Fluent::ApplyWindowChrome(hwnd);
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
    Fluent::DecorateTab(tabCtrl); // Fluent 选项卡：浅灰标签条 + 与页面融合的选中项

    // 添加六个选项卡标签
    TCITEMW tie = {};
    tie.mask = TCIF_TEXT;
    tie.pszText = const_cast<LPWSTR>(L"常规");
    TabCtrl_InsertItem(tabCtrl, 0, &tie);
    tie.pszText = const_cast<LPWSTR>(L"按键屏蔽");
    TabCtrl_InsertItem(tabCtrl, 1, &tie);
    tie.pszText = const_cast<LPWSTR>(L"按键映射");
    TabCtrl_InsertItem(tabCtrl, 2, &tie);
    tie.pszText = const_cast<LPWSTR>(L"热键管理");
    TabCtrl_InsertItem(tabCtrl, 3, &tie);
    tie.pszText = const_cast<LPWSTR>(L"窗口隐藏");
    TabCtrl_InsertItem(tabCtrl, 4, &tie);
    tie.pszText = const_cast<LPWSTR>(L"窗口置顶");
    TabCtrl_InsertItem(tabCtrl, 5, &tie);

    // 底部按钮与页面表格左右边框对齐：关于左对齐表格左边框，
    // 应用右对齐表格右边框（页面控件宽度为 PAGE_W - 10）
    int pageX = 0, pageY = 0, pageW = 0, pageH = 0;
    GetPageArea(tabCtrl, hwnd, &pageX, &pageY, &pageW, &pageH);
    const int tableRight = pageX + pageW - 10;

    // 创建底部按钮：关于、确定、取消、应用
    HWND btnAbout = CreateWindowExW(0, L"BUTTON", L"关于",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      ScaleUi(pageX), ScaleUi(BTN_Y), ScaleUi(BTN_W), ScaleUi(BTN_H),
                                      hwnd, (HMENU)IDC_BTN_ABOUT, GetModuleHandleW(nullptr), nullptr);
    HWND btnOk = CreateWindowExW(0, L"BUTTON", L"确定",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   ScaleUi(tableRight - BTN_W * 3 - 20), ScaleUi(BTN_Y),
                                   ScaleUi(BTN_W), ScaleUi(BTN_H),
                                   hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
    HWND btnCancel = CreateWindowExW(0, L"BUTTON", L"取消",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                       ScaleUi(tableRight - BTN_W * 2 - 10), ScaleUi(BTN_Y),
                                       ScaleUi(BTN_W), ScaleUi(BTN_H),
                                       hwnd, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);
    HWND btnApply = CreateWindowExW(0, L"BUTTON", L"应用",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      ScaleUi(tableRight - BTN_W), ScaleUi(BTN_Y),
                                      ScaleUi(BTN_W), ScaleUi(BTN_H),
                                      hwnd, (HMENU)IDAPPLY, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(btnAbout, font);
    SetControlFont(btnOk, font);
    SetControlFont(btnCancel, font);
    SetControlFont(btnApply, font);
    // Fluent 按钮：确定为唯一主操作（强调色实底），其余为次要按钮
    Fluent::DecorateButton(btnOk, Fluent::ButtonKind::Primary);
    Fluent::DecorateButton(btnApply, Fluent::ButtonKind::Secondary);
    Fluent::DecorateButton(btnCancel, Fluent::ButtonKind::Secondary);
    Fluent::DecorateButton(btnAbout, Fluent::ButtonKind::Secondary);

    // 「关于」右侧的外观 / 语言图标按钮（自绘，不参与 Fluent 按钮装饰）
    HWND btnTheme = CreateWindowExW(0, L"BUTTON", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                    ScaleUi(pageX + BTN_W + 8), ScaleUi(BTN_Y),
                                    ScaleUi(BTN_H), ScaleUi(BTN_H),
                                    hwnd, (HMENU)IDC_BTN_THEME, GetModuleHandleW(nullptr), nullptr);
    HWND btnLang = CreateWindowExW(0, L"BUTTON", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                   ScaleUi(pageX + BTN_W + 8 + BTN_H + 6), ScaleUi(BTN_Y),
                                   ScaleUi(BTN_H), ScaleUi(BTN_H),
                                   hwnd, (HMENU)IDC_BTN_LANG, GetModuleHandleW(nullptr), nullptr);
    // 自绘按钮不显示窗口文本，但必须提供文本作为屏幕阅读器的无障碍名称；
    // 工具提示只覆盖鼠标悬停，不能替代可访问名称（R8-M10）
    SetWindowTextW(btnTheme, Tr(L"外观"));
    SetWindowTextW(btnLang, Tr(L"语言"));
    data.themeTip = CreateIconTip(hwnd, btnTheme, ThemeTipText(data.workingCopy));
    data.langTip = CreateIconTip(hwnd, btnLang, LanguageTipText(data.workingCopy));

    // Tab 顺序按 z 序自上而下。按视觉从左到右逐次提到顶层，
    // 最终自上而下为 关于 → 确定 → 取消 → 应用
    SetWindowPos(btnApply, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(btnCancel, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(btnOk, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(btnLang, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(btnTheme, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(btnAbout, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // 切换到上次选中的选项卡
    SwitchTab(hwnd, data.workingCopy.tabState, data.workingCopy);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    FocusFirstPageControl(hwnd);

    // 打开设置时检查热键是否被其他程序占用。只做占用检测，
    // 不重复提示逻辑冲突/危险快捷键，避免每次进入设置都被打扰（R16）
    {
        const std::wstring occupied = PreCheckHotkeyConflicts(data.workingCopy, true);
        if (!occupied.empty()) {
            const std::wstring msg =
                Tr(L"检测到以下热键被其他程序占用：\n\n") + occupied;
            Fluent::ShowMessageBoxRaw(hwnd, msg.c_str(), Tr(L"热键被占用"),
                                      MB_OK | MB_ICONWARNING);
        }
    }

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
            if (vk == VK_ESCAPE && mod == 0) {
                // 仅无修饰键的 Esc 取消捕获；Win+Esc 等组合必须能被正常录入
                // （一键关闭程序热键的默认值就是 Win+Esc）
                if (data.capturingHotKey) {
                    data.capturingHotKey = false;
                    // 仅当另一项捕获也未进行时才卸载吞键钩子，避免相互干扰
                    if (!data.capturingCloseHotKey) EndInlineCapture(hwnd);
                    SetWindowTextW(btn,
                                   HotkeyButtonText(data.workingCopy.bossKeyMod,
                                                    data.workingCopy.bossKeyVK).c_str());
                } else {
                    data.capturingCloseHotKey = false;
                    if (!data.capturingHotKey) EndInlineCapture(hwnd);
                    SetWindowTextW(btn,
                                   HotkeyButtonText(data.workingCopy.bossKeyCloseMod,
                                                    data.workingCopy.bossKeyCloseVK).c_str());
                }
            } else if (mod == 0) {
                // 拒绝无修饰键的裸键：注册后会在全系统劫持该普通按键（如 A/回车）
                SetWindowTextW(btn, Tr(L"请使用 修饰键+按键 的组合..."));
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
        // Ctrl+Tab / Ctrl+Shift+Tab 切换选项卡：选项卡窗口被压在卡片之下，
        // 无法加入 Tab 序，这里单独提供键盘切页
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB &&
            (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
            HWND tab = GetDlgItem(hwnd, IDC_TAB_MAIN);
            if (tab) {
                const int count = TabCtrl_GetItemCount(tab);
                int sel = TabCtrl_GetCurSel(tab);
                if (count > 0 && sel >= 0) {
                    const bool back = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                    sel = (sel + (back ? count - 1 : 1)) % count;
                    SwitchTab(hwnd, sel, data.workingCopy);
                }
            }
            continue;
        }
        // DPI 变化消息必须直接交给窗口过程：IsDialogMessage 只处理键盘导航，
        // 不能让它吞掉 WM_DPICHANGED，否则跨屏拖动后布局不会重建。
        // 统一用共享判断，只拦截目标为设置主窗口的消息（R15-M09/A03）
        if (IsDpiMessageForWindow(msg, hwnd)) {
            SendMessageW(hwnd, msg.message, msg.wParam, msg.lParam);
            continue;
        }
        // 键盘导航：Tab 切换焦点、Enter 触发默认按钮（确定）、Esc 取消
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 兜底：捕获过程中若对话框被关闭，复位捕获状态并卸载吞键钩子，
    // 避免键盘钩子长期放行/吞掉按键；EndInlineCapture 内部按归属判断，
    // 若钩子已被其他捕获窗口接管则不会误拆
    if (data.capturingHotKey || data.capturingCloseHotKey || InlineCaptureOwner() == hwnd) {
        data.capturingHotKey = false;
        data.capturingCloseHotKey = false;
        EndInlineCapture(hwnd);
    }

    // 兜底：异常退出路径（GetMessage 返回 0/-1）下窗口可能仍存活，
    // 此时 GWLP_USERDATA 指向本函数栈上的 data，必须销毁窗口避免悬垂
    if (IsWindow(hwnd)) DestroyWindow(hwnd);

    // 对话框关闭后按系统/主窗口缩放重建主题字体，避免主界面或下一次
    // 打开设置时继续使用副屏 DPI 的字号。旧字体常驻进程，不在此删除。
    {
        ScopedUiScale resetToGlobal(0.0f);  // 0 表示回退进程级缩放
        Fluent::RebuildFontsForDpi();
    }
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
        config.disableNumLock = chk(IDC_CHK_DISABLENUM);
        config.disableStartupNotification = chk(IDC_CHK_DISABLE_STARTUP_NOTIFY);
        config.showLockKeyOverlay = chk(IDC_CHK_OVERLAY);
        config.showMuteOverlay = chk(IDC_CHK_MUTE_OVERLAY);
        config.overlayPrimaryOnly = chk(IDC_CHK_PRIMARY_ONLY);
        // 浮窗外观：下拉框选中项越界时收敛到默认值，避免写入非法配置
        auto styleCombo = [&](int id, int maxSel, int defVal) -> int {
            // 控件不存在时 GetDlgItem 返回 NULL，CB_GETCURSEL 会拿到 0 并被误当成
            // 合法选中项，因此先判空、直接返回该项对应的配置默认值
            HWND cbo = GetDlgItem(hwnd, id);
            if (!cbo) return defVal;
            int sel = (int)SendMessageW(cbo, CB_GETCURSEL, 0, 0);
            if (sel < 0 || sel > maxSel) return defVal;
            return sel;
        };
        config.overlayPosition = styleCombo(IDC_CBO_OVERLAY_POS, 4, 0);
        config.overlayTheme = styleCombo(IDC_CBO_OVERLAY_THEME, 1, 0);
        config.overlayCorner = styleCombo(IDC_CBO_OVERLAY_CORNER, 1, 1);
        config.overlayOpacity = styleCombo(IDC_CBO_OVERLAY_OPACITY, 3, 2);
        config.overlayDelay = styleCombo(IDC_CBO_OVERLAY_DELAY, 3, 1);
        // 保存资源管理器文件夹选项下拉框（0=不改变, 1=开启, 2=关闭）
        // CB_GETCURSEL 失败返回 CB_ERR(-1)，必须收敛到 0~2，避免把非法值写入配置
        auto combo = [&](int id) -> int {
            HWND cbo = GetDlgItem(hwnd, id);
            if (!cbo) return 0;
            int sel = (int)SendMessageW(cbo, CB_GETCURSEL, 0, 0);
            if (sel < 0) sel = 0;
            if (sel > 2) sel = 2;
            return sel;
        };
        config.explorerHideHidden = combo(IDC_CBO_EXPLORER_HIDDEN);
        config.explorerHideExt = combo(IDC_CBO_EXPLORER_EXT);
        config.explorerHideOS = combo(IDC_CBO_EXPLORER_OS);
        break;
    }
    case 1: // 按键屏蔽页面
        config.disableSpecifiedKeysEnabled = chk(IDC_CHK_DISABLEKEYS);
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
            // 读取自动隐藏时间的数值（1~120 分钟）。
            // 优先读取伴随编辑框：用户键入数字后直接按 Enter 触发默认按钮时，
            // 编辑框可能尚未失焦，UDM_GETPOS 仍是旧位置（R13-A12）
            int minutes = 0;
            HWND edt = GetDlgItem(hwnd, IDC_EDT_AUTO_HIDE_TIME);
            if (edt) minutes = ConfigUtils::SafeParseInt(ReadWindowText(edt), 0);
            if (minutes < 1) {
                HWND spin = GetDlgItem(hwnd, IDC_SPIN_AUTO_HIDE_TIME);
                if (spin) {
                    // UDM_GETPOS 高 16 位是错误标志，必须取 LOWORD
                    minutes = (int)LOWORD(SendMessageW(spin, UDM_GETPOS, 0, 0));
                }
            }
            if (minutes < 1) minutes = 1;
            if (minutes > MAX_AUTO_HIDE_MINUTES) minutes = MAX_AUTO_HIDE_MINUTES;
            config.bossKeyAutoHideTime = minutes;
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
    // 立即刷新表格的禁用配色（灰底灰字），无需切换选项卡
    Fluent::DecorateListView(lst);
}

// ===== 更新按键映射控件的启用/禁用状态 =====
// 未勾选「启用按键映射」时，列表与添加/互换/删除按钮整体置灰
void SettingsDialog::UpdateRemapState(HWND hwnd, bool enabled) {
    static const int ids[] = {
        IDC_LST_REMAP, IDC_BTN_ADDREMAP, IDC_BTN_ADD_SWAP, IDC_BTN_DELREMAP
    };
    for (int id : ids) {
        HWND h = GetDlgItem(hwnd, id);
        if (h) EnableWindow(h, enabled);
    }
    // 立即刷新表格的禁用配色，无需切换选项卡
    Fluent::DecorateListView(GetDlgItem(hwnd, IDC_LST_REMAP));
}

// ===== 浮窗外观控件：是否处于可用状态 =====
// 只有「显示锁定键状态提示」或「显示静音状态提示」至少一项开启时，
// 位置/颜色/边框/透明度/延时等外观设置才有意义
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
            SetWindowTextW(btn, HotkeyButtonText(
                data->workingCopy.bossKeyCloseMod, data->workingCopy.bossKeyCloseVK).c_str());
        }
    }
    EndInlineCapture(hwnd);
}

// ===== 页面控件"先隐藏创建、装饰后再显示" =====
// 背景：切换选项卡时页面控件是在已可见的设置窗口上创建的。若带 WS_VISIBLE 创建，
// 控件会在子类化（Fluent 自绘）之前先被系统按经典样式绘制一次，随后才被覆盖，
// 表现为切换标签瞬间闪出旧样式，同时产生一次多余的绘制。
// 因此页面构建器创建控件时一律不带 WS_VISIBLE，装饰、缩放、整理层级完成后
// 再由 PageBuildScope 统一显示，保证首次绘制就落在 Fluent 子类过程里。
namespace {
class PageBuildScope {
public:
    explicit PageBuildScope(HWND parent) : m_parent(parent) {
        for (HWND child = GetWindow(parent, GW_CHILD); child;
             child = GetWindow(child, GW_HWNDNEXT)) {
            m_before.insert(child);
        }
    }
    // 显示本次构建新增的直接子控件（已装饰完成）
    void ShowNewChildren() {
        if (!IsWindow(m_parent)) return;
        HWND child = GetWindow(m_parent, GW_CHILD);
        while (child) {
            HWND next = GetWindow(child, GW_HWNDNEXT);
            if (m_before.find(child) == m_before.end()) {
                ShowWindow(child, SW_SHOW);
                // 列表等控件在显示后不会自动重画整个客户区（系统只补无效区域），
                // 这里显式标记一次，保证首帧就是完整的 Fluent 外观而非白底
                InvalidateRect(child, nullptr, TRUE);
            }
            child = next;
        }
        // ShowWindow 会把子控件提到同级 z 序顶部，反向提升后先创建的宽控件
        // 会压住后创建的问号标记；分组框则会被抬到复选框之上。
        // 统一重排：分组框/选项卡在最底层，问号标记在最上层。
        LowerGroupBoxes(m_parent);
        RaiseHelpMarkers(m_parent);
    }
private:
    HWND m_parent;
    std::unordered_set<HWND> m_before;
};

}  // namespace

// 让列表最后一列填满客户区剩余宽度。
// 列表被禁用时 comctl32 只按"最后一列"范围绘制行背景，列尾到边框之间的空白
// 会用系统 COLOR_WINDOW 绘制（深色模式下呈白色块），因此必须消除这段空白
static void FitListLastColumn(HWND lv) {
    if (!lv) return;
    HWND header = ListView_GetHeader(lv);
    const int count = header ? Header_GetItemCount(header) : 0;
    if (count <= 0) return;
    RECT rc;
    GetClientRect(lv, &rc);
    int used = 0;
    for (int i = 0; i < count; ++i) used += ListView_GetColumnWidth(lv, i);
    const int extra = (rc.right - rc.left) - used;
    if (extra <= 0) return;   // 已有横向滚动条时不压缩列宽
    ListView_SetColumnWidth(lv, count - 1,
                            ListView_GetColumnWidth(lv, count - 1) + extra);
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
    // 本页控件先以隐藏状态创建，统一装饰完成后再显示：
    // 避免系统在子类化之前按经典样式先绘制一次
    PageBuildScope pageBuild(hwnd);

    HWND tabCtrl = GetDlgItem(hwnd, IDC_TAB_MAIN);
    if (!tabCtrl) return;   // 选项卡创建失败：不再按无效区域构建页面
    TabCtrl_SetCurSel(tabCtrl, tabIndex); // 更新选项卡控件的选中状态

    HFONT font = GetDlgFont();
    int PAGE_X, PAGE_Y, PAGE_W, PAGE_H;
    GetPageArea(tabCtrl, hwnd, &PAGE_X, &PAGE_Y, &PAGE_W, &PAGE_H);

    // 页面布局参数
    int y = PAGE_Y;
    int lineH = 22;         // 行高
    int btnH = BTN_H;       // 按钮高度
    int smallBtnW = 80;     // 小按钮宽度

    // 页面控件由 SettingsDialogPages.cpp 的注册表构建器创建；
    // 新增页面只需增加 BuildXxxPage 并注册，不再修改本函数。
    SettingsPageContext pageCtx(hwnd, config, font, PAGE_X, PAGE_Y, PAGE_W, PAGE_H,
                                y, lineH, btnH, smallBtnW);
    BuildSettingsPage(tabIndex, pageCtx);

    Fluent::DecorateDialog(hwnd);

    // 页面控件按逻辑坐标创建，构建完成后统一放大到当前 DPI 的物理尺寸
    ScalePageControls(hwnd);

    // DPI 缩放后让各列表最后一列填满客户区，避免列尾露出系统白块
    EnumChildWindows(hwnd, [](HWND child, LPARAM) -> BOOL {
        wchar_t cls[32] = L"";
        GetClassNameW(child, cls, 32);
        if (_wcsicmp(cls, L"SysListView32") == 0) FitListLastColumn(child);
        return TRUE;
    }, 0);

    // 卡片压在选项卡之上、其余控件压在卡片之上（含整页重绘）
    ReorderPageZOrder(hwnd);

    // 缩放与层级整理完成后再显示本页新建控件：
    // 首次绘制即为最终尺寸与 Fluent 样式，不会闪出未放大的小控件
    pageBuild.ShowNewChildren();

    // 英文模式下翻译本页控件、选项卡与底部按钮（幂等，英文文本不会再次命中）
    ApplyLanguageToWindowTree(hwnd);

    // 页面重建完成后刷新一次，保证背景、选项卡与卡片层级同步
    InvalidateRect(hwnd, nullptr, TRUE);
}

// ===== 应用设置 =====
// 保存当前页面状态，检查热键冲突，将工作副本写入实际配置
bool SettingsDialog::ApplySettings(HWND hwnd, AppConfig& config) {
    auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    config.tabState = data->currentTab;
    SaveCurrentTabState(hwnd, config);

    // 保存前检查自定义热键总长度：profile API 对超长单值支持有限，
    // 超过上限时保存会失败且用户难以定位，这里提前给出明确提示
    {
        size_t totalChars = 0;
        for (const auto& hk : data->workingCopy.customHotkeys) {
            totalChars += hk.Serialize().size() + 2;  // +2 为 ;; 分隔符
        }
        if (totalChars > MAX_CUSTOM_ENTRIES_CHARS) {
            Fluent::ShowMessageBox(hwnd,
                        L"自定义热键的总内容过长，无法安全写入配置文件。\n"
                        L"请缩短命令/参数，或减少自定义热键数量后重试。\n"
                        L"（建议序列化总长度不超过 30000 字符）",
                        L"提示", MB_OK | MB_ICONWARNING);
            return false;
        }
    }

    // 窗口绑定同样写入 INI 单值：先截断超长字段并检查总长度，
    // 避免 64 条长路径绑定把整个配置文件撑过 profile API 上限
    {
        auto clampAndMeasure = [](std::vector<BoundWindowInfo>& list) -> size_t {
            size_t total = 0;
            for (auto& b : list) {
                b.ClampFields();
                total += b.Serialize().size() + 2;
            }
            return total;
        };
        if (clampAndMeasure(data->workingCopy.bossKeyWindows) > MAX_BOUND_WINDOWS_CHARS ||
            clampAndMeasure(data->workingCopy.winDGuardWindows) > MAX_BOUND_WINDOWS_CHARS) {
            Fluent::ShowMessageBox(hwnd,
                        L"窗口绑定内容总长过长，无法安全写入配置文件。\n"
                        L"请减少绑定数量或缩短窗口标题/进程路径后重试。\n"
                        L"（每组绑定建议不超过 30000 字符）",
                        L"提示", MB_OK | MB_ICONWARNING);
            return false;
        }
    }

    // 检查热键冲突
    std::wstring conflicts = PreCheckHotkeyConflicts(data->workingCopy);
    if (!conflicts.empty()) {
        // 冲突列表含用户填写的热键描述：固定文案在此翻译，整段走 raw 显示，
        // 避免片段替换改写用户数据（R10-M2）
        std::wstring msg = Tr(L"以下热键存在冲突或被占用：\n\n") + conflicts +
                           Tr(L"\n是否仍要应用设置？");
        if (Fluent::ShowMessageBoxRaw(hwnd, msg.c_str(), Tr(L"热键冲突"),
                                      MB_YESNO | MB_ICONWARNING) != IDYES) {
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
        // 动态刷新不走窗口树翻译，键名必须在这里翻译，否则英文模式下
        // 新增/删除条目后列表会重新出现中文键名（R8-M4）
        std::wstring name = TrStr(AppConfig::VKToName(config.disabledKeyCodes[i]));
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = (int)i;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(name.c_str());
        ListView_InsertItem(lv, &item);
        // 列名为“按键代码”，必须以十六进制展示；此前把十进制直接拼在 0x 后，
        // 例如 A(65) 会显示成 0x65，与真实键码 0x41 不符（R9-A4）
        wchar_t codeBuf[16] = {};
        swprintf_s(codeBuf, L"0x%02X", (unsigned)config.disabledKeyCodes[i]);
        std::wstring code = codeBuf;
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
        std::wstring mode = (w.matchMode == MatchMode::Process) ? Tr(L"进程") : Tr(L"窗口");
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = (int)i;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(mode.c_str());
        ListView_InsertItem(lv, &item);
        std::wstring display = w.DisplayName();
        // 按匹配模式取显示文本，不能靠 "[P] " 前缀猜测：
        // 标题恰好以 "[P]" 开头时会被误判成进程名
        if (w.matchMode == MatchMode::Process) {
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
        std::wstring mode = (w.matchMode == MatchMode::Process) ? Tr(L"进程") : Tr(L"窗口");
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = (int)i;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(mode.c_str());
        ListView_InsertItem(lv, &item);
        std::wstring display = w.DisplayName();
        if (w.matchMode == MatchMode::Process) {
            display = w.processName;
            if (!w.processPath.empty()) display += L" (" + w.processPath + L")";
        }
        ListView_SetItemText(lv, (int)i, 1, const_cast<LPWSTR>(display.c_str()));
    }
}

// ===== 映射列表显示行 =====
// 按键互换在底层仍保存为 A→B 与 B→A 两条记录（钩子按该形式生效），
// 列表只显示一行：「原始按键 A | 互换 | 映射目标 B」
struct RemapRow {
    int configIndex;   // 对应配置条目的下标
    bool isSwap;       // true=按键互换
};

static std::vector<RemapRow> BuildRemapRows(const AppConfig& config) {
    std::vector<RemapRow> rows;
    std::vector<int> seenGroups;
    for (int i = 0; i < (int)config.keyRemappings.size(); ++i) {
        const auto& r = config.keyRemappings[(size_t)i];
        if (r.swapGroup > 0) {
            // 同一个互换组只显示第一条，避免出现两行
            if (std::find(seenGroups.begin(), seenGroups.end(), r.swapGroup) != seenGroups.end())
                continue;
            seenGroups.push_back(r.swapGroup);
        }
        rows.push_back({ i, r.swapGroup > 0 });
    }
    return rows;
}

// ===== 刷新按键映射列表视图 =====
void SettingsDialog::RefreshRemapList(HWND hwnd, AppConfig& config) {
    HWND lv = GetDlgItem(hwnd, IDC_LST_REMAP);
    if (!lv) return;
    ListView_DeleteAllItems(lv);

    const std::vector<RemapRow> rows = BuildRemapRows(config);
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = config.keyRemappings[(size_t)rows[i].configIndex];
        std::wstring src = TrStr(AppConfig::VKToName(r.sourceVK));
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = (int)i;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(src.c_str());
        ListView_InsertItem(lv, &item);

        // 方向列：普通映射 / 按键互换（带方向符号）
        ListView_SetItemText(lv, (int)i, 1,
                             const_cast<LPWSTR>(rows[i].isSwap ? Tr(L"互换 ⇄") : Tr(L"映射 →")));
        // 互换行显示对端按键；普通映射显示目标单键或组合键
        std::wstring dst = rows[i].isSwap
            ? TrStr(AppConfig::VKToName(r.targetVK))
            : FormatHotKey(r.mods, (UINT)r.targetVK);
        ListView_SetItemText(lv, (int)i, 2, const_cast<LPWSTR>(dst.c_str()));
    }
}

// ===== 添加屏蔽按键 =====
// 弹出按键捕获对话框，捕获用户按下的按键并添加到屏蔽列表
void SettingsDialog::AddDisabledKey(HWND hwnd, AppConfig& config) {
    int vk = ShowCaptureKeyDialog(hwnd);
    if (vk == 0) return;

    // 捕获层在单键模式下也会把修饰键上报（用户按 Ctrl+C 时会先捕获到 Ctrl），
    // 禁用修饰键会让全部组合键失效，必须在这里拦下
    if (ConfigUtils::IsDangerousDisabledKey(vk)) {
        Fluent::ShowMessageBox(hwnd, L"修饰键（Ctrl/Shift/Alt）不能加入屏蔽列表，\n"
                          L"否则所有依赖该键的组合键都会失效。",
                     L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    if (std::find(config.disabledKeyCodes.begin(), config.disabledKeyCodes.end(), vk) != config.disabledKeyCodes.end()) {
        Fluent::ShowMessageBox(hwnd, L"该按键已在屏蔽列表中",
                     L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if ((int)config.disabledKeyCodes.size() >= MAX_DISABLED_KEYS) {
        Fluent::ShowMessageBox(hwnd, L"屏蔽按键数量已达上限（256 个），请先删除不需要的条目。",
                     L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    config.disabledKeyCodes.push_back(vk);
    RefreshDisabledKeyList(hwnd, config);
}

// ===== 删除禁用按键 =====
// 从禁用按键列表中删除选中的项
void SettingsDialog::RemoveDisabledKey(HWND hwnd, AppConfig& config) {
    HWND lv = GetDlgItem(hwnd, IDC_LST_DISABLED);
    if (!lv) return;
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
        Fluent::ShowMessageBox(hwnd, L"修饰键（Ctrl/Shift/Alt/Win）不能作为映射源键，\n否则会导致该修饰键整体失效。",
                     L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    bool exists = false;
    for (const auto& r : config.keyRemappings) {
        if (r.sourceVK == sourceVK) { exists = true; break; }
    }
    if (exists) {
        Fluent::ShowMessageBox(hwnd, L"该源按键已存在映射规则",
                     L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // 目标按键改由虚拟键盘选择：即使目标按键物理损坏也能完成映射；
    // 勾选修饰键后点击主键即可录入 Ctrl+C 之类的组合
    UINT targetMods = 0;
    int targetVK = ShowKeyboardPicker(hwnd, &targetMods, true);
    if (targetVK == 0) return;

    if (sourceVK == targetVK && targetMods == 0) {
        Fluent::ShowMessageBox(hwnd, L"源按键和目标按键不能相同",
                     L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    // 目标主键若本身是其他规则的源键，会形成循环映射
    for (const auto& r : config.keyRemappings) {
        if (r.sourceVK == targetVK) {
            Fluent::ShowMessageBox(hwnd, L"该目标按键已被其他按键映射为源按键，这会导致循环映射问题。\n请先删除相关映射后再试。",
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
            L"如需恢复原按键输入，请在本页删除该映射。\r\n\r\n"
            L"是否继续添加？";
        if (Fluent::ShowMessageBox(hwnd, msg.c_str(), L"单键失效提醒",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
            return;
        }
    }

    if ((int)config.keyRemappings.size() >= MAX_KEY_REMAPS) {
        Fluent::ShowMessageBox(hwnd, L"按键映射数量已达上限（256 条），请先删除不需要的规则。",
                     L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    config.keyRemappings.push_back({ sourceVK, targetMods, targetVK });
    RefreshRemapList(hwnd, config);
}

// ===== 添加按键互换 =====
// 捕获两个按键后生成 A→B 与 B→A 两条同组映射（钩子按双向记录生效）；
// 列表中只显示一行「A | 互换 | B」，删除该行会移除整组两条记录。
// 允许修饰键参与互换（如 Ctrl↔Alt）
void SettingsDialog::AddSwapMapping(HWND hwnd, AppConfig& config) {
    int first = ShowCaptureKeyDialogEx(hwnd,
        L"添加按键互换 - 第一个按键",
        L"请按下要互换的第一个按键...",
        L"(Fn键无法捕获；修饰键也可互换)");
    if (first == 0) return;

    int second = ShowCaptureKeyDialogEx(hwnd,
        L"添加按键互换 - 第二个按键",
        L"请按下要互换的第二个按键...",
        L"(Fn键无法捕获；修饰键也可互换)");
    if (second == 0) return;

    if (first == second) {
        Fluent::ShowMessageBox(hwnd, L"两个按键不能相同，无法互换。",
                     L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    // 与已有规则冲突检查：两个键都不能出现在现有映射的源键或目标键中，
    // 否则互换后的输出行为会与既有规则叠加，难以预期
    auto inUse = [&](int vk) -> bool {
        for (const auto& r : config.keyRemappings) {
            if (r.sourceVK == vk || r.targetVK == vk) return true;
        }
        return false;
    };
    if (inUse(first) || inUse(second)) {
        Fluent::ShowMessageBox(hwnd,
                    L"其中一个按键已存在于现有映射规则中。\n"
                    L"请先删除相关规则，或选择其他按键。",
                    L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    // 组号取现有最大值 +1，保证同组两条记录唯一
    int group = 1;
    for (const auto& r : config.keyRemappings) {
        if (r.swapGroup >= group) group = r.swapGroup + 1;
    }

    std::wstring nameA = AppConfig::VKToName(first);
    std::wstring nameB = AppConfig::VKToName(second);

    std::wstring msg = L"将互换「" + nameA + L"」与「" + nameB + L"」：\n\n"
        L"• 按下「" + nameA + L"」将输出「" + nameB + L"」\n"
        L"• 按下「" + nameB + L"」将输出「" + nameA + L"」\n\n"
        L"该互换在列表中显示为一行，删除时两个方向一并移除。\n"
        L"注意：互换靠模拟按键实现，输出按键仍会经过其他程序和系统，"
        L"若该键已被它们注册为热键，对应功能仍可能被触发。";
    if (ConfigUtils::IsModifierKey(first) || ConfigUtils::IsModifierKey(second)) {
        msg += L"\n\n本次互换包含修饰键，所有依赖该修饰键的组合键"
               L"都会按互换后的按键处理。";
    }
    msg += L"\n\n是否继续？";
    if (Fluent::ShowMessageBox(hwnd, msg.c_str(), L"按键互换",
                    MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }

    if ((int)config.keyRemappings.size() + 2 > MAX_KEY_REMAPS) {
        Fluent::ShowMessageBox(hwnd, L"按键映射数量已达上限（256 条），无法再添加互换组。",
                     L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    config.keyRemappings.push_back({ first, 0, second, group });
    config.keyRemappings.push_back({ second, 0, first, group });
    RefreshRemapList(hwnd, config);
}

// ===== 删除按键映射规则 =====
// 互换条目在列表中只占一行，删除时按组号移除底层两条记录
void SettingsDialog::RemoveRemapEntry(HWND hwnd, AppConfig& config) {
    HWND lv = GetDlgItem(hwnd, IDC_LST_REMAP);
    if (!lv) return;

    const std::vector<RemapRow> rows = BuildRemapRows(config);
    const int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= (int)rows.size()) return;

    const RemapRow& row = rows[(size_t)sel];
    if (row.isSwap) {
        const int group = config.keyRemappings[(size_t)row.configIndex].swapGroup;
        std::vector<KeyRemapEntry> kept;
        kept.reserve(config.keyRemappings.size());
        for (const auto& r : config.keyRemappings) {
            if (r.swapGroup != group) kept.push_back(r);
        }
        config.keyRemappings.swap(kept);
    } else {
        config.keyRemappings.erase(config.keyRemappings.begin() + row.configIndex);
    }
    RefreshRemapList(hwnd, config);
}

// ===== 删除窗口隐藏绑定 =====
void SettingsDialog::RemoveBossWindow(HWND hwnd, AppConfig& config) {
    HWND lv = GetDlgItem(hwnd, IDC_LST_BOUND_WINDOWS);
    if (!lv) return;
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
    if (!lv) return;
    int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (sel < 0) return;
    if (sel < (int)config.winDGuardWindows.size()) {
        config.winDGuardWindows.erase(config.winDGuardWindows.begin() + sel);
        RefreshWindWindowList(hwnd, config);
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
        Fluent::ShowMessageBox(hwnd, L"请先选择要编辑的热键", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    LVITEMW item = {};
    item.mask = LVIF_PARAM;
    item.iItem = sel;
    SendMessageW(lv, LVM_GETITEMW, 0, (LPARAM)&item);
    int configIdx = (int)item.lParam;
    if (configIdx < 0 || configIdx >= (int)config.customHotkeys.size()) {
        Fluent::ShowMessageBox(hwnd, L"该热键无法编辑", L"提示", MB_OK | MB_ICONWARNING);
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
        Fluent::ShowMessageBox(hwnd, L"请先选择要删除的热键", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    LVITEMW item = {};
    item.mask = LVIF_PARAM;
    item.iItem = sel;
    SendMessageW(lv, LVM_GETITEMW, 0, (LPARAM)&item);
    int configIdx = (int)item.lParam;
    if (configIdx < 0 || configIdx >= (int)config.customHotkeys.size()) {
        Fluent::ShowMessageBox(hwnd, L"该热键无法删除", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    config.customHotkeys.erase(config.customHotkeys.begin() + configIdx);
    RefreshCustomHotkeyList(hwnd, config);
}

// ===== 刷新自定义热键列表视图 =====
void SettingsDialog::RefreshCustomHotkeyList(HWND hwnd, AppConfig& config) {
    HWND lv = GetDlgItem(hwnd, IDC_LV_CUSTOMHK);
    if (!lv) return;
    // 记录当前选中项对应的配置下标，刷新后恢复选中，避免编辑一条后焦点/选中丢失
    int keepParam = -1;
    const int oldSel = (int)SendMessageW(lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (oldSel >= 0) {
        LVITEMW keep = {};
        keep.mask = LVIF_PARAM;
        keep.iItem = oldSel;
        if (SendMessageW(lv, LVM_GETITEMW, 0, (LPARAM)&keep)) keepParam = (int)keep.lParam;
    }
    // 批量插入期间暂停重绘，避免列表逐行闪烁
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    SendMessageW(lv, LVM_DELETEALLITEMS, 0, 0);
    int idx = 0;
    for (size_t i = 0; i < config.customHotkeys.size(); i++) {
        const auto& hk = config.customHotkeys[i];
        if (hk.mod == 0 && hk.vk == 0) continue;
        // mod/vk 任一为 0 的条目不会被注册（ApplyConfig 按 || 跳过），
        // 明确标注为无效，避免列表看起来正常却始终不生效；仍保留行以便选中删除
        std::wstring keyName = (hk.mod == 0 || hk.vk == 0)
            ? Tr(L"（无效）") : FormatHotKey(hk.mod, hk.vk);
        // 模拟按键模式显示被模拟的按键/组合键，命令模式显示命令路径
        std::wstring cmd = hk.simulateKey
            ? (Tr(L"模拟按键: ") + FormatHotKey(hk.targetMod, hk.targetVK))
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
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, nullptr, TRUE);
    // 恢复原来的选中项（按配置下标匹配，列表中会跳过无效条目，不能直接用行号）
    if (keepParam >= 0) {
        for (int i = 0; i < idx; ++i) {
            LVITEMW it = {};
            it.mask = LVIF_PARAM;
            it.iItem = i;
            if (SendMessageW(lv, LVM_GETITEMW, 0, (LPARAM)&it) && (int)it.lParam == keepParam) {
                ListView_SetItemState(lv, i, LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(lv, i, FALSE);
                break;
            }
        }
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
                       HotkeyButtonText(data->workingCopy.bossKeyCloseMod,
                                        data->workingCopy.bossKeyCloseVK).c_str());
    }
    data->capturingHotKey = true;
    SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_SETHOTKEY), Tr(L"请按下快捷键..."));
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
    SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_SETCLOSEHOTKEY), Tr(L"请按下快捷键..."));
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

// ===== 清除一键关闭程序热键 =====
// 清除后该热键不再注册；v5 的默认值为 Win+Esc，但“清除”按钮表示用户
// 明确不需要该热键，与“恢复默认”语义不同（R9-A5）
void SettingsDialog::ResetCloseHotKey(HWND hwnd) {
    auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    // 与 ResetHotKey 对称处理，避免捕获态残留
    CancelInlineCapture(data, hwnd);
    data->workingCopy.bossKeyCloseMod = 0;
    data->workingCopy.bossKeyCloseVK = 0;
    SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_SETCLOSEHOTKEY), Tr(L"未设置"));
}

// ===== 恢复一键关闭程序热键为默认值（Win+Esc） =====
// 与「清除」区分：清除表示用户明确不需要该热键，本函数恢复产品默认值
void SettingsDialog::DefaultCloseHotKey(HWND hwnd) {
    auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    // 与 ResetCloseHotKey 对称处理，避免捕获态残留覆盖默认值
    CancelInlineCapture(data, hwnd);
    data->workingCopy.bossKeyCloseMod = MOD_WIN;
    data->workingCopy.bossKeyCloseVK = VK_ESCAPE;
    SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_SETCLOSEHOTKEY),
                   HotkeyButtonText(MOD_WIN, VK_ESCAPE).c_str());
}

// ===== 深色模式列表网格线 =====
// 系统网格线固定为浅灰，在深色底上对比过强：关闭 LVS_EX_GRIDLINES 后，
// 由父窗口在此用接近内容底色的深灰重绘行底横线与列间竖线
static void DrawDarkListGrid(NMLVCUSTOMDRAW* cd) {
    HWND lv = cd->nmcd.hdr.hwndFrom;
    HDC hdc = cd->nmcd.hdc;
    const int item = (int)cd->nmcd.dwItemSpec;
    RECT rcItem;
    if (!ListView_GetItemRect(lv, item, &rcItem, LVIR_BOUNDS)) return;
    if (rcItem.right <= rcItem.left || rcItem.bottom <= rcItem.top) return;

    // 使用主题层缓存的画笔：列表滚动时逐行 CreatePen/DeleteObject 会造成 GDI 抖动
    HPEN pen = Fluent::GridPen();
    HGDIOBJ oldPen = SelectObject(hdc, pen);

    // 行底横线
    MoveToEx(hdc, rcItem.left, rcItem.bottom - 1, nullptr);
    LineTo(hdc, rcItem.right, rcItem.bottom - 1);

    // 列间竖线（最后一列右侧由控件边框负责）
    HWND header = ListView_GetHeader(lv);
    const int columns = header ? Header_GetItemCount(header) : 1;
    for (int c = 0; c + 1 < columns; ++c) {
        RECT rcSub;
        if (ListView_GetSubItemRect(lv, item, c, LVIR_BOUNDS, &rcSub)) {
            const int x = rcSub.right - 1;
            if (x >= rcItem.left && x < rcItem.right) {
                MoveToEx(hdc, x, rcItem.top, nullptr);
                LineTo(hdc, x, rcItem.bottom - 1);
            }
        }
    }

    SelectObject(hdc, oldPen);
    // 画笔由主题层缓存，不能在此删除
}

// ===== 设置对话框主窗口过程 =====
// 处理所有控件事件、选项卡切换、右键菜单等消息
static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) { EraseWindowBackground(hwnd, wp); return 1; }
    auto* data = reinterpret_cast<SettingsDialog::DialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!data) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_DRAWITEM: {
        // 「关于」右侧的外观 / 语言小按钮由本窗口自绘
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (dis && (dis->CtlID == IDC_BTN_THEME || dis->CtlID == IDC_BTN_LANG)) {
            PaintIconButton(dis);
            return TRUE;
        }
        break;
    }
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
        case IDC_BTN_THEME:
            // 外观快捷切换：跟随系统 → 浅色 → 深色 → 跟随系统
            if (HIWORD(wp) == BN_CLICKED) {
                const int mode = (data->workingCopy.appTheme + 1) % 3;
                data->workingCopy.appTheme = mode;
                data->originalConfig.appTheme = mode;   // 快捷开关不随「取消」回滚
                data->config->appTheme = mode;
                if (SettingsDialog::s_uiPreferenceCallback) {
                    SettingsDialog::s_uiPreferenceCallback();
                }
                UpdateIconTips(hwnd, data);
                InvalidateRect(GetDlgItem(hwnd, IDC_BTN_THEME), nullptr, TRUE);
            }
            return 0;
        case IDC_BTN_LANG:
            // 语言快捷切换：中文 ↔ English
            if (HIWORD(wp) == BN_CLICKED) {
                const int lang = (data->workingCopy.appLanguage == 0) ? 1 : 0;
                data->workingCopy.appLanguage = lang;
                data->originalConfig.appLanguage = lang; // 快捷开关不随「取消」回滚
                data->config->appLanguage = lang;
                if (SettingsDialog::s_uiPreferenceCallback) {
                    SettingsDialog::s_uiPreferenceCallback();
                }
                RebuildForLanguage(hwnd);
            }
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
                // 立即刷新绑定表格的禁用配色
                Fluent::DecorateListView(GetDlgItem(hwnd, IDC_LST_BOUND_WINDOWS));
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
                // 与"显示静音状态提示"任一开启时才允许设置外观
                UpdateOverlayAppearanceState(hwnd, OverlayAppearanceChecked(hwnd));
            }
            return 0;
        }

        case IDC_CHK_MUTE_OVERLAY: {
            if (HIWORD(wp) == BN_CLICKED) {
                bool checked = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                data->workingCopy.showMuteOverlay = checked;
                // 与"显示锁定键状态提示"任一开启时才允许设置外观
                UpdateOverlayAppearanceState(hwnd, OverlayAppearanceChecked(hwnd));
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

        case IDC_BTN_OVERLAY_DEFAULT: {
            // 恢复浮窗默认外观：颜色-深色、位置-底部、边框-圆角、透明度-低透明、延时-适中
            if (HIWORD(wp) == BN_CLICKED) {
                data->workingCopy.overlayTheme = 0;
                data->workingCopy.overlayPosition = 0;
                data->workingCopy.overlayCorner = 1;
                data->workingCopy.overlayOpacity = 2;
                data->workingCopy.overlayDelay = 1;
                const struct { int id; int sel; } pairs[] = {
                    { IDC_CBO_OVERLAY_THEME, 0 },
                    { IDC_CBO_OVERLAY_POS, 0 },
                    { IDC_CBO_OVERLAY_CORNER, 1 },
                    { IDC_CBO_OVERLAY_OPACITY, 2 },
                    { IDC_CBO_OVERLAY_DELAY, 1 }
                };
                for (const auto& p : pairs) {
                    HWND h = GetDlgItem(hwnd, p.id);
                    if (h) SendMessageW(h, CB_SETCURSEL, p.sel, 0);
                }
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
                // 立即刷新绑定表格的禁用配色
                Fluent::DecorateListView(GetDlgItem(hwnd, IDC_LST_WINDWINS));
            }
            return 0;
        }

        case IDC_BTN_FIX_ZORDER: {
            // 修复窗口堆叠：只恢复键客自己记录过的窗口
            // 确认框/结果框需要键盘操作，先取消内联捕获避免吞键钩子让弹窗键盘失灵
            CancelInlineCapture(data, hwnd);
            int confirm = Fluent::ShowMessageBox(hwnd,
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
            const bool skippedByGuard = data->config->winDGuardEnabled;
            if (!skippedByGuard) {
                repaired = WinDGuard::RecoverOrphanedStyles(false);
            }

            wchar_t buf[256] = {};
            if (skippedByGuard) {
                swprintf_s(buf, L"窗口置顶守护正在启用，无法执行修复。\n请先在“窗口置顶”页关闭守护并应用，再运行本修复。");
            } else if (repaired > 0) {
                swprintf_s(buf, L"已修复 %d 个窗口的置顶/最小化按钮状态", repaired);
            } else {
                swprintf_s(buf, L"未发现由键客造成异常的窗口");
            }
            Fluent::ShowMessageBox(hwnd, buf, L"修复完成", MB_OK | MB_ICONINFORMATION);
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
        case IDC_BTN_ADD_SWAP:
            // 按键互换：捕获两个按键并生成联动的双向映射
            CancelInlineCapture(data, hwnd);
            SettingsDialog::AddSwapMapping(hwnd, data->workingCopy);
            return 0;
        case IDC_BTN_DELREMAP:
            CancelInlineCapture(data, hwnd);
            SettingsDialog::RemoveRemapEntry(hwnd, data->workingCopy);
            return 0;
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
        case IDC_BTN_DEFAULTCLOSEHOTKEY:
            SettingsDialog::DefaultCloseHotKey(hwnd);
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
                // 立即刷新热键表格的禁用配色
                Fluent::DecorateListView(GetDlgItem(hwnd, IDC_LV_CUSTOMHK));
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
        // 列表行配色：列表被禁用时 comctl32 会忽略 ListView_SetBkColor，
        // 改用系统窗口色绘制行背景（深色模式下呈白色行），必须在自定义绘制中覆盖。
        // 深色模式还需在行绘制后补画柔和网格线
        if (nmhdr->code == NM_CUSTOMDRAW) {
            wchar_t cls[32] = L"";
            GetClassNameW(nmhdr->hwndFrom, cls, 32);
            if (_wcsicmp(cls, L"SysListView32") == 0) {
                auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    const bool listEnabled = IsWindowEnabled(nmhdr->hwndFrom) != FALSE;
                    cd->clrTextBk = listEnabled ? Fluent::SurfaceColor()
                                                : Fluent::DisabledBackgroundColor();
                    cd->clrText = listEnabled ? Fluent::TextColor()
                                              : Fluent::TextDisabledColor();
                    return Fluent::IsDarkTheme() ? (CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT)
                                                 : CDRF_NEWFONT;
                }
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT) {
                    if (Fluent::IsDarkTheme()) DrawDarkListGrid(cd);
                    return CDRF_DODEFAULT;
                }
            }
        }
        // 选项卡切换通知
        if (nmhdr->idFrom == IDC_TAB_MAIN && nmhdr->code == TCN_SELCHANGE) {
            int sel = TabCtrl_GetCurSel(GetDlgItem(hwnd, IDC_TAB_MAIN));
            SettingsDialog::SwitchTab(hwnd, sel, data->workingCopy);
            break;
        }
        break;
    }

    case DM_GETDEFID:
        // 非对话框窗口：向 IsDialogMessage 声明默认按钮，Enter 才会触发「确定」
        return MAKELRESULT(IDOK, DC_HASDEFID);

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        // 控件文字背景透明，底色为 Fluent 内容面；禁用控件文字置灰
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, IsWindowEnabled((HWND)lp) ? Fluent::TextColor() : Fluent::TextDisabledColor());
        return (LRESULT)Fluent::SurfaceBrush();
    }
    case WM_CTLCOLOREDIT: {
        // 编辑框需要不透明背景；深色主题下不能沿用系统白色
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, Fluent::SurfaceColor());
        SetTextColor(hdc, IsWindowEnabled((HWND)lp) ? Fluent::TextColor() : Fluent::TextDisabledColor());
        return (LRESULT)Fluent::SurfaceBrush();
    }

    case WM_ACTIVATE: {
        // 捕获期间窗口失焦（用户切到其他程序、被置顶窗口遮挡）：
        // 必须立即取消捕获并卸载吞键钩子，否则钩子会继续吞掉全系统按键，
        // 用户表现为"键盘失灵"，且没有直观的恢复入口（R8-H4）
        if (LOWORD(wp) == WA_INACTIVE) {
            CancelInlineCapture(data, hwnd);
            break;
        }
        // 窗口创建时可能还不是前台窗口（SetFocus 会失败）。首次被激活且
        // 还没有任何子控件获得焦点时补一次初始焦点，避免落在「关于」上。
        // 焦点可能被系统先设到窗口本身（GetFocus()==hwnd），同样需要补
        HWND focused = GetFocus();
        if (LOWORD(wp) != WA_INACTIVE && (focused == nullptr || focused == hwnd))
            FocusFirstPageControl(hwnd);
        break;
    }
    case WM_DPICHANGED: {
        // 窗口被拖到不同 DPI 的显示器：按新 DPI 重建主题字体、选项卡/按钮
        // 与当前页面控件。ScopedUiScale 的线程覆盖值同步更新，后续绘制/布局
        // 都使用新缩放。
        const UINT dpi = HIWORD(wp);
        RECT work = {};
        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (monitor && GetMonitorInfoW(monitor, &mi)) work = mi.rcWork;
        UiScaleOverrideStorage() = DpiUtils::ScaleForDpi(dpi, &work);
        Fluent::RebuildFontsForDpi();

        RECT* suggested = reinterpret_cast<RECT*>(lp);
        if (suggested) {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        LayoutDialogChrome(hwnd, Fluent::UiFont());
        if (data->currentTab >= 0) {
            // 同索引 SwitchTab 不会触发 SaveCurrentTabState（该逻辑只在切页时执行），
            // 而下面会销毁并重建当前页控件。必须先强制保存下拉框/数值框，
            // 否则窗口拖到另一 DPI 屏幕后未应用的选择会回退到工作副本旧值。
            data->workingCopy.tabState = data->currentTab;
            SettingsDialog::SaveCurrentTabState(hwnd, data->workingCopy);
            SettingsDialog::SwitchTab(hwnd, data->currentTab, data->workingCopy);
        }
        FocusFirstPageControl(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_DESTROY:
        // 兜底：无论从哪条路径销毁窗口，都必须归还本窗口持有的吞键钩子，
        // 否则钩子会持续吞掉全系统按键且没有 UI 可以结束捕获
        if (InlineCaptureOwner() == hwnd) EndInlineCapture(hwnd);
        break;
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

