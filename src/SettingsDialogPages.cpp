// SettingsDialogPages.cpp - page builders extracted from SettingsDialog.cpp
// Add a page by implementing BuildXxxPage and registering it below.

#include "SettingsDialogPages.h"
#include "Lang.h"

// ============================================================
// 帮助问号与浮窗提示
// 页面选项较多、说明文字不宜长期占用布局空间，因此使用可悬停/点击的
// 问号标记，在设置窗口上方弹出 Fluent 风格的多行说明浮窗。
// ============================================================
namespace {

constexpr wchar_t HELP_MARKER_CLASS[] = L"KeySentryHelpMarker";
constexpr wchar_t HELP_POPUP_CLASS[] = L"KeySentryHelpPopup";

const wchar_t* const HELP_STARTUP_EXPLORER =
    L"每次软件启动时，会按此设置全局重置文件夹选项，实现开机初始化设置。";
const wchar_t* const HELP_DISABLE_KEYS =
    L"允许用户阻止特定按键的输入，实现按键的屏蔽，避免误操作。";
const wchar_t* const HELP_KEY_REMAP =
    L"1.按键映射为组合键后，单独按下原始按键将失效；\n"
    L"2.请勿创建循环嵌套映射，发生按键冲突时请删除对应映射项。";
const wchar_t* const HELP_CUSTOM_HOTKEY =
    L"添加热键执行命令/程序，或通过热键模拟按键和组合键。";
const wchar_t* const HELP_WIND_GUARD =
    L"1.对指定窗口保持置顶，禁止窗口最小化。\n"
    L"2.出现窗口异常时，点击「修复窗口层级」取消所有窗口置顶。";

// 常规页「状态提示」分组说明
const wchar_t* const HELP_OVERLAY =
    L"按下 Caps Lock / Num Lock / Scroll Lock 或静音键时，在屏幕上显示当前状态浮层。";
// 窗口隐藏页「启用窗口隐藏」复选框说明
const wchar_t* const HELP_BOSS_ENABLE =
    L"快速隐藏/恢复指定窗口，保护隐私（老板键）。";

// 窗口隐藏页分组框说明（子项复选框不再提供问号浮窗）
const wchar_t* const HELP_BOSS_HOTKEY =
    L"设置隐藏/显示窗口的全局热键，默认 Win+`。";
const wchar_t* const HELP_BOSS_MOUSE =
    L"鼠标隐藏：按下鼠标中键/侧键，或把鼠标移到屏幕对应角落，即可快速隐藏或恢复窗口。";
const wchar_t* const HELP_BOSS_OTHER =
    L"隐藏时静音：隐藏窗口时同时静音；\n"
    L"隐藏当前窗口：同时隐藏当前活动窗口；\n"
    L"暂停媒体：隐藏前发送媒体暂停命令；\n"
    L"闲置自动隐藏：无操作达到设定分钟数后自动隐藏；\n"
    L"退出键客时关闭已隐藏程序：退出时结束被隐藏的程序。";

struct HelpMarkerData {
    std::wstring text;
    bool hover = false;
};

HWND g_helpPopup = nullptr;
HWND g_helpOwner = nullptr;
std::wstring g_helpText;

void HideHelpPopup(HWND marker) {
    // 只有当前 popup 属于该 marker 时才隐藏，避免旧 marker 的
    // WM_MOUSELEAVE 抢在新 marker 显示之后把新提示关掉
    if (marker && g_helpOwner != marker) return;
    if (g_helpPopup) ShowWindow(g_helpPopup, SW_HIDE);
    g_helpOwner = nullptr;
}

int HelpMeasureTextWidth(HFONT font, const wchar_t* text) {
    if (!font || !text || !*text) return 0;
    HDC hdc = GetDC(nullptr);
    if (!hdc) return 0;
    HGDIOBJ oldFont = SelectObject(hdc, font);
    SIZE size = {};
    GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &size);
    SelectObject(hdc, oldFont);
    ReleaseDC(nullptr, hdc);
    return size.cx;
}

LRESULT CALLBACK HelpPopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SIZE: {
        // 圆角改由窗口区域裁出：只靠 RoundRect 绘制时，四个圆角外的像素
        // 不会被 WM_ERASEBKGND 擦除，窗口初始的黑色表面会残留成四个黑点
        const int radius = ScaleUi(6);
        HRGN region = CreateRoundRectRgn(0, 0, LOWORD(lp) + 1, HIWORD(lp) + 1,
                                         radius * 2, radius * 2);
        if (region) {
            // 成功后区域归系统所有；失败时由本函数释放
            if (!SetWindowRgn(hwnd, region, TRUE)) DeleteObject(region);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            Fluent::FillRoundRect(hdc, rc, ScaleUi(6),
                                  Fluent::SurfaceColor(), Fluent::GridLineColor());
            RECT tr = { rc.left + ScaleUi(12), rc.top + ScaleUi(9),
                        rc.right - ScaleUi(12), rc.bottom - ScaleUi(9) };
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, Fluent::TextColor());
            HGDIOBJ oldFont = SelectObject(hdc, Fluent::SmallFont());
            DrawTextW(hdc, g_helpText.c_str(), -1, &tr,
                      DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(hdc, oldFont);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_NCDESTROY:
        if (g_helpPopup == hwnd) {
            g_helpPopup = nullptr;
            g_helpOwner = nullptr;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ShowHelpPopup(HWND marker, const std::wstring& text) {
    if (!marker || text.empty()) return;

    if (!g_helpPopup) {
        HWND owner = GetAncestor(marker, GA_ROOT);
        g_helpPopup = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            HELP_POPUP_CLASS, L"", WS_POPUP,
            0, 0, 0, 0, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!g_helpPopup) return;
    }

    // 帮助说明以中文原文定义，显示时按当前语言翻译（支持多行拼接文本）
    g_helpText = TranslateMessageText(text);
    g_helpOwner = marker;

    const int maxTextW = ScaleUi(360);
    HDC hdc = GetDC(nullptr);
    if (!hdc) return;
    HGDIOBJ oldFont = SelectObject(hdc, Fluent::SmallFont());
    RECT calc = { 0, 0, maxTextW, 0 };
    // 必须用翻译后的文本测量：英文通常比中文更长，按原文测量会让
    // 浮窗尺寸不足、显示文本被裁剪（R9-M1）
    DrawTextW(hdc, g_helpText.c_str(), -1, &calc,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
    ReleaseDC(nullptr, hdc);

    const int padX = ScaleUi(12);
    const int padY = ScaleUi(9);
    int w = calc.right - calc.left + padX * 2;
    int h = calc.bottom - calc.top + padY * 2;
    if (w < ScaleUi(140)) w = ScaleUi(140);
    if (h < ScaleUi(34)) h = ScaleUi(34);

    RECT markerRect = {};
    GetWindowRect(marker, &markerRect);
    MONITORINFO mi = { sizeof(mi) };
    RECT work = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    HMONITOR monitor = MonitorFromWindow(marker, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &mi)) work = mi.rcWork;

    int x = markerRect.left;
    int y = markerRect.bottom + ScaleUi(6);
    if (x + w > work.right - 4) x = work.right - w - 4;
    if (x < work.left + 4) x = work.left + 4;
    if (y + h > work.bottom - 4) y = markerRect.top - h - ScaleUi(6);
    if (y < work.top + 4) y = work.top + 4;

    SetWindowPos(g_helpPopup, HWND_TOPMOST, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_helpPopup, nullptr, TRUE);
}

LRESULT CALLBACK HelpMarkerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<HelpMarkerData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, Fluent::SurfaceBrush());

        const int size = (rc.right < rc.bottom) ? rc.right : rc.bottom;
        if (size > 2) {
            const COLORREF border = (data && data->hover)
                                  ? Fluent::TextColor()
                                  : Fluent::GridLineColor();
            const COLORREF fill = (data && data->hover)
                                ? Fluent::DisabledBackgroundColor()
                                : Fluent::SurfaceColor();
            HPEN pen = CreatePen(PS_SOLID, 1, border);
            HBRUSH brush = CreateSolidBrush(fill);
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBrush = SelectObject(hdc, brush);
            Ellipse(hdc, ScaleUi(1), ScaleUi(1), size - ScaleUi(1), size - ScaleUi(1));
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(brush);
            DeleteObject(pen);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, Fluent::TextColor());
            HGDIOBJ oldFont = SelectObject(hdc, Fluent::SmallFont());
            RECT tr = rc;
            DrawTextW(hdc, L"?", -1, &tr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(hdc, oldFont);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    case WM_MOUSEMOVE:
        if (data) {
            if (!data->hover) {
                data->hover = true;
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (g_helpOwner != hwnd || !g_helpPopup || !IsWindowVisible(g_helpPopup)) {
                ShowHelpPopup(hwnd, data->text);
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        if (data && data->hover) {
            data->hover = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        HideHelpPopup(hwnd);
        return 0;
    case WM_LBUTTONUP:
        if (data) {
            SetFocus(hwnd);
            ShowHelpPopup(hwnd, data->text);
        }
        return 0;
    case WM_KEYDOWN:
        if (data && (wp == VK_RETURN || wp == VK_SPACE)) {
            ShowHelpPopup(hwnd, data->text);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            HideHelpPopup(hwnd);
            return 0;
        }
        break;
    case WM_NCDESTROY:
        HideHelpPopup(hwnd);
        delete data;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool EnsureHelpClasses() {
    static const bool ready = []() -> bool {
        WNDCLASSW popup = {};
        popup.lpfnWndProc = HelpPopupProc;
        popup.hInstance = GetModuleHandleW(nullptr);
        popup.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        popup.lpszClassName = HELP_POPUP_CLASS;
        if (!WindowUtils::RegisterClassOnce(popup)) return false;

        WNDCLASSW marker = {};
        marker.lpfnWndProc = HelpMarkerProc;
        marker.hInstance = GetModuleHandleW(nullptr);
        marker.hCursor = LoadCursorW(nullptr, IDC_HAND);
        marker.lpszClassName = HELP_MARKER_CLASS;
        return WindowUtils::RegisterClassOnce(marker);
    }();
    return ready;
}

HWND CreateHelpMarker(HWND parent, int x, int y, const wchar_t* helpText) {
    if (!EnsureHelpClasses()) return nullptr;
    // 页面控件先按逻辑坐标创建，随后由 ScalePageControls 统一放大；
    // 这里必须使用逻辑尺寸，不能提前 ScaleUi，否则高 DPI 下会二次放大。
    const int size = 16;
    HWND marker = CreateWindowExW(
        0, HELP_MARKER_CLASS, L"?",
        WS_CHILD | WS_VISIBLE,
        x, y, size, size, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!marker) return nullptr;
    SetWindowLongPtrW(marker, GWLP_USERDATA,
                      (LONG_PTR)new HelpMarkerData{ helpText ? helpText : L"" });
    return marker;
}

// 在复选框文字之后放置问号；rowH 用于垂直居中。
// 字体宽度是物理像素，页面布局是逻辑坐标，需要除以当前缩放系数。
void AddCheckHelp(HWND parent, int px, int py, HFONT font, int rowH,
                  const wchar_t* label, const wchar_t* helpText) {
    // 文本会在页面构建完成后统一翻译，这里必须按目标语言测量，
    // 否则英文模式下问号会叠在文字上（R9-M1）
    const std::wstring display = Tr(label);
    const float scale = GetUiScale();
    const int textW = (scale > 0.0f)
                    ? (int)(HelpMeasureTextWidth(font, display.c_str()) / scale + 0.5f)
                    : HelpMeasureTextWidth(font, display.c_str());
    const int textLeft = 1 + 16 + 8;
    CreateHelpMarker(parent,
                     px + textLeft + textW + 6,
                     py + (rowH - 16) / 2,
                     helpText);
}

// 在分组框标题之后放置问号（同样使用逻辑坐标）
void AddGroupHelp(HWND parent, int gbX, int gbY,
                  const wchar_t* title, const wchar_t* helpText) {
    const std::wstring display = Tr(title);
    const float scale = GetUiScale();
    const int textW = (scale > 0.0f)
                    ? (int)(HelpMeasureTextWidth(Fluent::UiFontSemibold(), display.c_str()) / scale + 0.5f)
                    : HelpMeasureTextWidth(Fluent::UiFontSemibold(), display.c_str());
    CreateHelpMarker(parent,
                     gbX + 12 + textW + 5,
                     gbY,
                     helpText);
}

// 按当前语言测量标签宽度（逻辑坐标）：让右侧按钮紧贴文字，
// 避免固定偏移在中文界面留下大段空白、英文界面又发生重叠
int HelpMeasureLabelLogicalWidth(HFONT font, const wchar_t* label) {
    const std::wstring display = Tr(label);
    const float scale = GetUiScale();
    const int width = HelpMeasureTextWidth(font, display.c_str());
    return scale > 0.0f ? (int)(width / scale + 0.5f) : width;
}

}  // namespace

// 把所有帮助问号标记提到页面控件最上层。
// ShowWindow(SW_SHOW) 会把子控件提到同级 z 序顶部，页面按创建顺序反向提升后，
// 先创建的宽控件（复选框等）会压住后创建的问号标记，导致鼠标无法命中、
// 问号看起来"消失"。页面构建完成后统一提升一次（R14 修复）。
void LowerGroupBoxes(HWND page) {
    if (!page) return;
    // 分组框是整块自绘背景，必须位于同页子控件下方。
    // ShowWindow 会把页面控件按创建顺序反向提到 z 序顶部，先创建的分组框
    // 会被抬到复选框之上；加上 WS_CLIPSIBLINGS 后会把复选框整片裁掉，
    // 表现为选项文字空白。这里统一把分组框压到底部。
    // 先收集再统一压到底部：SetWindowPos 会改变 z 序，边遍历边调整可能
    // 反复绕回同一批分组框形成死循环（与问号标记的提升同理）。
    std::vector<HWND> groupBoxes;
    for (HWND child = GetWindow(page, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        if ((GetWindowLongPtrW(child, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX) {
            groupBoxes.push_back(child);
        }
    }
    for (HWND groupBox : groupBoxes) {
        SetWindowPos(groupBox, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    // 选项卡负责整页背景，必须位于分组框之下
    HWND tab = GetDlgItem(page, IDC_TAB_MAIN);
    if (tab) {
        SetWindowPos(tab, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void RaiseHelpMarkers(HWND page) {
    if (!page) return;
    // 先收集再统一提升：SetWindowPos 会改变 z 序，边遍历边修改容易绕回
    // 已处理的窗口形成死循环。“窗口隐藏”页问号最多，曾因此整页卡死。
    std::vector<HWND> markers;
    for (HWND child = GetWindow(page, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t cls[64] = {};
        if (GetClassNameW(child, cls, 64) && wcscmp(cls, HELP_MARKER_CLASS) == 0) {
            markers.push_back(child);
        }
    }
    for (HWND marker : markers) {
        SetWindowPos(marker, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void SettingsPageContext::BuildGeneralPage() {
        int gbPad = 12;
        int gbW = PAGE_W - 10;

        {   // 启动与退出分组
            int gbX = PAGE_X;
            int gbY = y;
            int innerY = y + 24;

            HWND gbStartup = CreateWindowExW(0, L"BUTTON", L"启动与退出",
                                               WS_CHILD | BS_GROUPBOX,
                                               gbX, gbY, gbW, 100,
                                               hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SetControlFont(gbStartup, font);

            makeChk(IDC_CHK_AUTOSTART, L"开机自动启动", gbX + gbPad, innerY, gbW - gbPad * 2, config.autoStart);
            innerY += lineH + 8;
            makeChk(IDC_CHK_FORCENUMLOCK, L"启动后开启 NumLock", gbX + gbPad, innerY, gbW - gbPad * 2, config.forceNumLockOn);
            innerY += lineH + 8;
            // 从「按键屏蔽」页移入：与 NumLock 启动行为放在一起，便于统一配置
            makeChk(IDC_CHK_DISABLENUM, L"保持 NumLock 开启状态", gbX + gbPad, innerY, gbW - gbPad * 2, config.disableNumLock);
            innerY += lineH + 8;
            makeChk(IDC_CHK_DISABLE_STARTUP_NOTIFY, L"启动时不显示通知", gbX + gbPad, innerY, gbW - gbPad * 2, config.disableStartupNotification);
            innerY += lineH + 8;

            // 一键关闭程序热键放在「启动与退出」分组内，与退出行为归在一起。
            // 标签宽度按当前语言测量，按钮紧跟文字右侧：中文界面不再留下大段空白，
            // 英文界面也不会因固定 155px 而重叠
            const int closeLabelW = HelpMeasureLabelLogicalWidth(font, L"关闭程序热键:");
            makeLabel(L"关闭程序热键:", gbX + gbPad, innerY, closeLabelW, btnH);
            const int closeBtnX = gbX + gbPad + closeLabelW + 8;
            makeButton(IDC_BTN_SETCLOSEHOTKEY, HotkeyButtonText(config.bossKeyCloseMod, config.bossKeyCloseVK).c_str(),
                        closeBtnX, innerY, 180, btnH);
            makeButton(IDC_BTN_RESETCLOSEHOTKEY, L"清除",
                        closeBtnX + 185, innerY, 55, btnH);
            // 「清除」表示不注册该热键；「默认」恢复为 Win+Esc，两者语义不同
            makeButton(IDC_BTN_DEFAULTCLOSEHOTKEY, L"默认",
                        closeBtnX + 185 + 60, innerY, 55, btnH);
            innerY += btnH + 8;

            MoveWindow(gbStartup, gbX, gbY, gbW, innerY - gbY + 10, TRUE);
            y = innerY + 14;
        }

        {   // 状态提示分组
            int gbX = PAGE_X;
            int gbY = y;
            int innerY = y + 24;

            HWND gbOverlay = CreateWindowExW(0, L"BUTTON", L"状态提示",
                                               WS_CHILD | BS_GROUPBOX,
                                               gbX, gbY, gbW, 80,
                                               hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SetControlFont(gbOverlay, font);
            AddGroupHelp(hwnd, gbX, gbY, L"状态提示", HELP_OVERLAY);

            makeChk(IDC_CHK_OVERLAY, L"启用锁定键状态提示 (Caps/Num/Scroll Lock)", gbX + gbPad, innerY, gbW - gbPad * 2, config.showLockKeyOverlay);
            innerY += lineH + 8;
            makeChk(IDC_CHK_MUTE_OVERLAY, L"启用静音状态提示", gbX + gbPad, innerY, gbW - gbPad * 2, config.showMuteOverlay);
            innerY += lineH + 8;
            makeChk(IDC_CHK_PRIMARY_ONLY, L"仅在主屏显示", gbX + gbPad, innerY, gbW - gbPad * 2, config.overlayPrimaryOnly);
            innerY += lineH + 8;

            // 浮窗外观自定义：颜色 / 位置 / 边框 一行三列，透明度 / 延时 接在下一行
            // 下拉框宽度与下方"隐藏文件和文件夹"等选项保持一致
            HFONT smallFont = GetSmallFont();
            int lblW3 = 90;     // 标签宽度（容纳英文 "Transparency"）
            int cboW3 = 110;    // 下拉框宽度（容纳英文 "High transparency" 约 104px）
            int cboH3 = 21;     // 下拉框/按钮高度
            int colPitch = lblW3 + 6 + cboW3 + 28;  // 列间距
            auto makeOverlayCombo = [&](int id, const wchar_t* labelText, int col,
                                        int py, std::initializer_list<const wchar_t*> items, int selVal) {
                int colX = gbX + gbPad + col * colPitch;
                makeLabel(labelText, colX, py, lblW3, cboH3);
                HWND cbo = CreateWindowExW(0, L"COMBOBOX", L"",
                                            WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                            colX + lblW3 + 6, py, cboW3, 200,
                                            hwnd, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
                SendMessageW(cbo, WM_SETFONT, (WPARAM)smallFont, TRUE);
                for (const wchar_t* it : items) SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)it);
                SendMessageW(cbo, CB_SETCURSEL, selVal, 0);
            };
            makeOverlayCombo(IDC_CBO_OVERLAY_THEME, L"颜色", 0, innerY,
                             { L"深色", L"浅色" }, config.overlayTheme);
            makeOverlayCombo(IDC_CBO_OVERLAY_POS, L"位置", 1, innerY,
                             { L"底部", L"顶部", L"居中", L"左侧", L"右侧" }, config.overlayPosition);
            makeOverlayCombo(IDC_CBO_OVERLAY_CORNER, L"边框", 2, innerY,
                             { L"直角", L"圆角" }, config.overlayCorner);
            innerY += cboH3 + 8;
            makeOverlayCombo(IDC_CBO_OVERLAY_OPACITY, L"透明度", 0, innerY,
                             { L"高透明", L"适中", L"低透明", L"不透明" }, config.overlayOpacity);
            makeOverlayCombo(IDC_CBO_OVERLAY_DELAY, L"延时", 1, innerY,
                             { L"快速", L"适中", L"缓慢", L"保持显示" }, config.overlayDelay);
            // "默认"按钮放在延时右侧（即上一行"边框"所在的第三列），
            // 左边缘与上一行"边框"文字左对齐
            makeButton(IDC_BTN_OVERLAY_DEFAULT, L"恢复默认",
                       gbX + gbPad + 2 * colPitch, innerY, cboW3, cboH3);
            innerY += cboH3 + 8;

            MoveWindow(gbOverlay, gbX, gbY, gbW, innerY - gbY + 10, TRUE);
            // 两个提示开关都未开启时，外观设置与"仅在主屏显示提示"整体置灰
            UpdateOverlayAppearanceState(hwnd, OverlayAppearanceChecked(hwnd));
            y = innerY + 14;
        }

        {   // 软件启动时设置分组（重置资源管理器文件夹选项）
            int gbX = PAGE_X;
            int gbY = y;
            int innerY = y + 24;
            // 标签列留足英文宽度（"Hide protected operating system files"）
            int lblW = 235;
            int cboW = 125;   // 放下"保持原状"/"Leave unchanged"等较长选项，避免文字被截断
            int cboH = 21;
            int cboX = gbX + gbPad + lblW + 8;

            HWND gbExplorer = CreateWindowExW(0, L"BUTTON", L"软件启动时设置",
                                               WS_CHILD | BS_GROUPBOX,
                                               gbX, gbY, gbW, 80,
                                               hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SetControlFont(gbExplorer, font);
            AddGroupHelp(hwnd, gbX, gbY, L"软件启动时设置", HELP_STARTUP_EXPLORER);

            // 使用全局懒加载字体（主题层常驻缓存），避免每次切页泄漏 GDI 句柄
            HFONT smallFont = GetSmallFont();

            auto makeExplorerCombo = [&](int id, const wchar_t* labelText, int selVal) {
                makeLabel(labelText, gbX + gbPad, innerY, lblW, cboH);
                HWND cbo = CreateWindowExW(0, L"COMBOBOX", L"",
                                            WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                            cboX, innerY, cboW, 200,
                                            hwnd, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
                SendMessageW(cbo, WM_SETFONT, (WPARAM)smallFont, TRUE);
                SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)L"保持原状");
                SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)L"隐藏");
                SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)L"显示");
                SendMessageW(cbo, CB_SETCURSEL, selVal, 0);
            };

            makeExplorerCombo(IDC_CBO_EXPLORER_HIDDEN, L"隐藏文件和文件夹", config.explorerHideHidden);
            innerY += cboH + 8;
            makeExplorerCombo(IDC_CBO_EXPLORER_EXT, L"隐藏已知文件类型扩展名", config.explorerHideExt);
            innerY += cboH + 8;
            makeExplorerCombo(IDC_CBO_EXPLORER_OS, L"隐藏受保护的操作系统文件", config.explorerHideOS);
            innerY += cboH + 8;

            MoveWindow(gbExplorer, gbX, gbY, gbW, innerY - gbY + 10, TRUE);
            y = innerY + 14;
        }
}

void SettingsPageContext::BuildKeysPage() {
        makeChk(IDC_CHK_DISABLEKEYS, L"启用按键屏蔽", PAGE_X, y, PAGE_W - 20, config.disableSpecifiedKeysEnabled);
        AddCheckHelp(hwnd, PAGE_X, y, font, lineH, L"启用按键屏蔽", HELP_DISABLE_KEYS);
        y += lineH + 8;

        makeButton(IDC_BTN_ADDKEY, L"添加屏蔽按键", PAGE_X, y, smallBtnW * 2 + 10, btnH);
        makeButton(IDC_BTN_DELKEY, L"删除选中项", PAGE_X + smallBtnW * 2 + 20, y, smallBtnW * 2 + 10, btnH);
        y += btnH + 6;

        // 说明文字已改为问号浮窗，列表下沿与「热键管理」页保持一致
        int lstH1 = PAGE_Y + PAGE_H - y - 10;
        if (lstH1 < 60) lstH1 = 60;
        {
            HWND lv = makeListView(IDC_LST_DISABLED, PAGE_X, y, PAGE_W - 10, lstH1);
            int lvW = PAGE_W - 30;
            addLvColumn(lv, 0, L"按键名称", lvW * 2 / 3);
            addLvColumn(lv, 1, L"按键代码", lvW / 3);
        }
        SettingsDialog::RefreshDisabledKeyList(hwnd, config);
        SettingsDialog::UpdateDisableKeysState(hwnd, config.disableSpecifiedKeysEnabled);
}

void SettingsPageContext::BuildRemapPage() {
        makeChk(IDC_CHK_KEYREMAP, L"启用按键映射", PAGE_X, y, PAGE_W - 20, config.keyRemapEnabled);
        AddCheckHelp(hwnd, PAGE_X, y, font, lineH, L"启用按键映射", HELP_KEY_REMAP);
        y += lineH + 8;

        makeButton(IDC_BTN_ADDREMAP, L"添加按键映射", PAGE_X, y, smallBtnW * 2 + 10, btnH);
        makeButton(IDC_BTN_ADD_SWAP, L"添加按键互换", PAGE_X + 180, y, smallBtnW * 2 + 10, btnH);
        makeButton(IDC_BTN_DELREMAP, L"删除选中项", PAGE_X + 360, y, smallBtnW * 2 + 10, btnH);
        y += btnH + 6;

        // 说明文字已改为问号浮窗，列表下沿与「热键管理」页保持一致
        int lstH2 = PAGE_Y + PAGE_H - y - 10;
        if (lstH2 < 60) lstH2 = 60;
        {
            HWND lv = makeListView(IDC_LST_REMAP, PAGE_X, y, PAGE_W - 10, lstH2);
            int lvW = PAGE_W - 30;
            addLvColumn(lv, 0, L"原始按键", lvW * 36 / 100);
            addLvColumn(lv, 1, L"方向", lvW * 16 / 100);
            addLvColumn(lv, 2, L"映射目标", lvW * 48 / 100);
        }
        SettingsDialog::RefreshRemapList(hwnd, config);
        SettingsDialog::UpdateRemapState(hwnd, config.keyRemapEnabled);
}

void SettingsPageContext::BuildHotkeyPage() {
        makeChk(IDC_CHK_CUSTOM_HK_ENABLE, L"启用自定义热键", PAGE_X, y, PAGE_W - 20, config.customHotkeysEnabled);
        AddCheckHelp(hwnd, PAGE_X, y, font, lineH, L"启用自定义热键", HELP_CUSTOM_HOTKEY);
        y += lineH + 8;

        // 按钮行放在列表框上方，排版与"按键映射"页保持一致
        makeButton(IDC_BTN_ADD_CUSTOMHK, L"添加自定义热键", PAGE_X, y, smallBtnW * 2 + 10, btnH);
        makeButton(IDC_BTN_EDIT_CUSTOMHK, L"编辑选中项", PAGE_X + smallBtnW * 2 + 20, y, smallBtnW * 2 + 10, btnH);
        makeButton(IDC_BTN_DEL_CUSTOMHK, L"删除选中项", PAGE_X + (smallBtnW * 2 + 20) * 2, y, smallBtnW * 2 + 10, btnH);
        y += btnH + 6;

        int customLvH = PAGE_Y + PAGE_H - y - 10;
        if (customLvH < 60) customLvH = 60;
        HWND customLv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                         WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER,
                                         PAGE_X, y, PAGE_W - 10, customLvH,
                                         hwnd, (HMENU)IDC_LV_CUSTOMHK, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(customLv, font);
        SendMessageW(customLv, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        // 手工创建的列表同样接入 Fluent 装饰：高对比度下也安装 Enter 拦截子类，
        // 避免列表内回车被 IsDialogMessage 转成"确定"
        Fluent::DecorateListView(customLv);

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
            // 列表背景色按启用态选择：禁用后必须重装饰，否则空白区仍是启用色（R14-A5）
            Fluent::DecorateListView(GetDlgItem(hwnd, IDC_LV_CUSTOMHK));
        }

        SettingsDialog::RefreshCustomHotkeyList(hwnd, config);
}

void SettingsPageContext::BuildBossPage() {
        int rowH = 24;
        int gap = 6;
        int gbPad = 12;
        int gbW = PAGE_W - 10;
        int col3W = (gbW - gbPad * 2 - 16) / 3;

        makeChk(IDC_CHK_BOSSKEY, L"启用窗口隐藏", PAGE_X, y, PAGE_W - 20, config.bossKeyEnabled);
        AddCheckHelp(hwnd, PAGE_X, y, font, lineH, L"启用窗口隐藏", HELP_BOSS_ENABLE);
        y += rowH + 6;

        {
            int gbX = PAGE_X;
            int gbY = y;
            int innerY = y + 24;

            HWND gbHotkey = CreateWindowExW(0, L"BUTTON", L"键盘热键",
                                             WS_CHILD | BS_GROUPBOX,
                                             gbX, gbY, gbW, 60,
                                             hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SetControlFont(gbHotkey, font);
            AddGroupHelp(hwnd, gbX, gbY, L"键盘热键", HELP_BOSS_HOTKEY);

            // 标签宽度按当前语言测量，避免中文界面按钮离文字过远，
            // 同时保证英文长标签不会与按钮重叠
            const int bossHotkeyLabelW =
                HelpMeasureLabelLogicalWidth(font, L"隐藏/显示窗口热键:");
            makeLabel(L"隐藏/显示窗口热键:", gbX + gbPad, innerY, bossHotkeyLabelW, btnH);
            const int bossHotkeyBtnX = gbX + gbPad + bossHotkeyLabelW + 8;
            makeButton(IDC_BTN_SETHOTKEY, SettingsDialog::FormatHotKey(config.bossKeyMod, config.bossKeyVK).c_str(),
                        bossHotkeyBtnX, innerY, 180, btnH);
            makeButton(IDC_BTN_RESETHOTKEY, L"默认",
                        bossHotkeyBtnX + 185, innerY, 55, btnH);
            innerY += btnH + 8;

            MoveWindow(gbHotkey, gbX, gbY, gbW, innerY - gbY + 4, TRUE);
            y = innerY + 10;
        }

        {
            int gbX = PAGE_X;
            int gbY = y;
            int innerY = y + 24;

            HWND gbMouse = CreateWindowExW(0, L"BUTTON", L"鼠标隐藏",
                                            WS_CHILD | BS_GROUPBOX,
                                            gbX, gbY, gbW, 100,
                                            hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SetControlFont(gbMouse, font);
            AddGroupHelp(hwnd, gbX, gbY, L"鼠标隐藏", HELP_BOSS_MOUSE);

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
            y = innerY + 10;
        }

        {
            int gbX = PAGE_X;
            int gbY = y;
            int innerY = y + 24;

            HWND gbOther = CreateWindowExW(0, L"BUTTON", L"其他选项",
                                            WS_CHILD | BS_GROUPBOX,
                                            gbX, gbY, gbW, 100,
                                            hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SetControlFont(gbOther, font);
            AddGroupHelp(hwnd, gbX, gbY, L"其他选项", HELP_BOSS_OTHER);

            // 按用户要求互换：第一列「隐藏当前窗口」，第二列「隐藏时静音」
            makeChk(IDC_CHK_BOSS_HIDE_CURRENT, L"隐藏当前窗口", gbX + gbPad, innerY, col3W, config.bossKeyHideCurrent);
            makeChk(IDC_CHK_BOSSMUTE, L"隐藏时静音", gbX + gbPad + col3W + 8, innerY, col3W, config.bossKeyMute);
            makeChk(IDC_CHK_BOSS_SEND_PAUSE, L"暂停媒体", gbX + gbPad + (col3W + 8) * 2, innerY, col3W, config.bossKeySendPause);
            innerY += rowH + gap + 4;
            makeChk(IDC_CHK_BOSS_AUTO_HIDE, L"闲置自动隐藏", gbX + gbPad, innerY, 120, config.bossKeyAutoHide);
            {
                int timeX = gbX + gbPad + 124;
                HWND edtTime = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                                WS_CHILD | WS_TABSTOP | WS_BORDER | ES_NUMBER,
                                                timeX, innerY, 38, rowH,
                                                hwnd, (HMENU)IDC_EDT_AUTO_HIDE_TIME, GetModuleHandleW(nullptr), nullptr);
                SetControlFont(edtTime, font);

                HWND spin = CreateWindowExW(0, UPDOWN_CLASSW, L"",
                                             WS_CHILD | UDS_SETBUDDYINT | UDS_ARROWKEYS,
                                             timeX + 38, innerY, 18, rowH,
                                             hwnd, (HMENU)IDC_SPIN_AUTO_HIDE_TIME, GetModuleHandleW(nullptr), nullptr);
                SendMessageW(spin, UDM_SETBUDDY, (WPARAM)edtTime, 0);
                SendMessageW(spin, UDM_SETRANGE, 0, MAKELPARAM(MAX_AUTO_HIDE_MINUTES, 1));
                SendMessageW(spin, UDM_SETPOS, 0, config.bossKeyAutoHideTime);

                HWND lblMin = CreateWindowExW(0, L"STATIC", L"分钟",
                                                WS_CHILD | SS_LEFT | SS_CENTERIMAGE,
                                                timeX + 58, innerY, 35, rowH,
                                                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
                SetControlFont(lblMin, font);
            }
            makeChk(IDC_CHK_BOSS_CLOSE_ON_EXIT, L"退出键客时关闭已隐藏程序", gbX + gbPad + col3W + 8, innerY, gbW - gbPad - col3W - 8, config.bossKeyCloseOnExit);
            innerY += rowH + 8;

            MoveWindow(gbOther, gbX, gbY, gbW, innerY - gbY + 4, TRUE);
            y = innerY + 10;
        }

        // 按钮宽度与「按键映射」页的“添加按键映射”保持一致（smallBtnW * 2 + 10），
        // 间距同步取 180，避免加宽后两个按钮重叠
        makeButton(IDC_BTN_ADD_BOSS_BINDING, L"添加绑定窗口/进程", PAGE_X, y, smallBtnW * 2 + 10, btnH);
        makeButton(IDC_BTN_DELBOSS, L"删除选中项", PAGE_X + 180, y, smallBtnW * 2 + 10, btnH);
        y += btnH + 6;
        {
            int listH = PAGE_Y + PAGE_H - y - 12;
            if (listH < 60) listH = 60;
            HWND lstBound = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                             WS_CHILD | WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                             PAGE_X, y, PAGE_W - 10, listH,
                                             hwnd, (HMENU)IDC_LST_BOUND_WINDOWS, GetModuleHandleW(nullptr), nullptr);
            SetControlFont(lstBound, font);
            ListView_SetExtendedListViewStyle(lstBound, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            // 与热键管理列表一致：高对比度下也安装 Enter 拦截子类
            Fluent::DecorateListView(lstBound);
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
            y += listH;
        }
        SettingsDialog::RefreshBossWindowList(hwnd, config);

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
            // 列表背景色按启用态选择：切换启用状态后必须重装饰（R14-A5）
            Fluent::DecorateListView(GetDlgItem(hwnd, IDC_LST_BOUND_WINDOWS));
        }
}

void SettingsPageContext::BuildWindPage() {
        makeChk(IDC_CHK_WINDGUARD, L"启用窗口置顶", PAGE_X, y, 200, config.winDGuardEnabled, btnH);
        AddCheckHelp(hwnd, PAGE_X, y, font, btnH, L"启用窗口置顶", HELP_WIND_GUARD);
        y += btnH + 14;

        // 绑定与修复按钮上移到列表上方，并移除“已绑定的窗口/进程”标题；
        // 前两个按钮宽度与「按键映射」页保持一致
        makeButton(IDC_BTN_ADD_WIND_BINDING, L"添加绑定窗口/进程", PAGE_X, y, smallBtnW * 2 + 10, btnH);
        makeButton(IDC_BTN_DELWIND, L"删除选中项", PAGE_X + 180, y, smallBtnW * 2 + 10, btnH);
        makeButton(IDC_BTN_FIX_ZORDER, L"修复窗口堆叠", PAGE_X + PAGE_W - 10 - 140, y, 140, btnH);
        y += btnH + 6;

        {
            // 按钮已移到列表上方：列表使用剩余高度，底部只保留页面边距
            int lstH5 = PAGE_Y + PAGE_H - y - 12;
            if (lstH5 < 60) lstH5 = 60;
            HWND lv = makeListView(IDC_LST_WINDWINS, PAGE_X, y, PAGE_W - 10, lstH5);
            int lvW = PAGE_W - 30;
            addLvColumn(lv, 0, L"匹配模式", lvW / 4);
            addLvColumn(lv, 1, L"标题/进程名", lvW * 3 / 4);
            y += lstH5;
        }

        SettingsDialog::RefreshWindWindowList(hwnd, config);

        {
            bool enabled = config.winDGuardEnabled;
            static const int windCtrlIds[] = {
                IDC_LST_WINDWINS, IDC_BTN_ADD_WIND_BINDING, IDC_BTN_DELWIND
            };
            for (int id : windCtrlIds) {
                HWND h = GetDlgItem(hwnd, id);
                if (h) EnableWindow(h, enabled);
            }
            // 列表背景色按启用态选择：切换启用状态后必须重装饰（R14-A5）
            Fluent::DecorateListView(GetDlgItem(hwnd, IDC_LST_WINDWINS));
        }
}


void BuildSettingsPage(int index, SettingsPageContext& ctx) {
    using Builder = void (SettingsPageContext::*)();
    static const Builder builders[] = {
        &SettingsPageContext::BuildGeneralPage,
        &SettingsPageContext::BuildKeysPage,
        &SettingsPageContext::BuildRemapPage,
        &SettingsPageContext::BuildHotkeyPage,
        &SettingsPageContext::BuildBossPage,
        &SettingsPageContext::BuildWindPage,
    };
    const int count = (int)(sizeof(builders) / sizeof(builders[0]));
    if (index >= 0 && index < count) (ctx.*builders[index])();
}
