// ===== FluentTheme.cpp - Fluent Design 主题层实现 =====
// 说明：所有外观由本模块自绘，控件仍是标准 Win32 控件，
//       因此键盘操作、无障碍、输入法行为都保持系统原生语义。

#include "FluentTheme.h"
#include "Lang.h"
#include "Utils.h"
#include <CommCtrl.h>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace {

// 系统 DLL 加载统一复用 Utils.h 的 DpiUtils::LoadSystemModule，
// 避免本文件与 DPI 工具各维护一份相同的安全加载逻辑（R13-A13）

// 复选框/单选框子类标识：ToggleProc 内部做方向键导航时需要提前引用
constexpr UINT_PTR kToggleSubclassId = 0x464C0005;

// ============================================================
// 外观调色板：浅色 / 深色两套，运行时可切换
// System 模式读取 Windows 的浅色/深色设置与系统主题色
// ============================================================
struct Palette {
    COLORREF accent, accentHover, accentPressed, onAccent;
    COLORREF danger, dangerHoverBg, dangerPressedBg;
    COLORREF text, textSecondary, textDisabled;
    COLORREF border, borderHover, disabledBorder;
    COLORREF surface, tabStrip, hover, pressed, disabledBg;
};

Palette g_lightPalette = {
    RGB(0, 120, 212), RGB(16, 110, 190), RGB(0, 90, 158), RGB(255, 255, 255),
    RGB(196, 43, 28), RGB(255, 240, 240), RGB(255, 224, 224),
    RGB(26, 26, 26), RGB(92, 92, 92), RGB(158, 158, 158),
    RGB(212, 212, 212), RGB(160, 160, 160), RGB(232, 232, 232),
    RGB(255, 255, 255), RGB(243, 243, 243), RGB(246, 246, 246),
    RGB(237, 237, 237), RGB(245, 245, 245)
};

// 深色采用 Win11 深色近似值：内容面/页面统一为 #2B2B2B，
// 靠边框与悬停态区分层次，避免静态文字控件的底色与页面不一致
Palette g_darkPalette = {
    RGB(0, 120, 212), RGB(16, 110, 190), RGB(0, 90, 158), RGB(255, 255, 255),
    RGB(255, 153, 164), RGB(68, 39, 43), RGB(82, 43, 48),
    RGB(255, 255, 255), RGB(170, 170, 170), RGB(120, 120, 120),
    RGB(62, 62, 62), RGB(120, 120, 120), RGB(56, 56, 56),
    RGB(43, 43, 43), RGB(43, 43, 43), RGB(58, 58, 58),
    RGB(70, 70, 70), RGB(50, 50, 50)
};

Fluent::ThemeMode g_themeMode = Fluent::ThemeMode::System;
bool g_themeResolved = false;
bool g_darkTheme = false;

COLORREF BlendColor(COLORREF a, COLORREF b, int percentB) {
    const int r = GetRValue(a) + (GetRValue(b) - GetRValue(a)) * percentB / 100;
    const int g = GetGValue(a) + (GetGValue(b) - GetGValue(a)) * percentB / 100;
    const int bl = GetBValue(a) + (GetBValue(b) - GetBValue(a)) * percentB / 100;
    return RGB(r, g, bl);
}

bool ReadRegDword(HKEY root, const wchar_t* subKey, const wchar_t* value, DWORD* out) {
    DWORD type = 0;
    DWORD size = sizeof(DWORD);
    return RegGetValueW(root, subKey, value, RRF_RT_REG_DWORD, &type, out, &size) == ERROR_SUCCESS;
}

// Windows 10/11「应用模式」：AppsUseLightTheme=0 表示深色
bool SystemPrefersDarkTheme() {
    DWORD light = 1;
    if (ReadRegDword(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", &light)) {
        return light == 0;
    }
    return false;   // Win7 等无该键：使用浅色
}

// 读取系统主题色（Windows 10/11 的个性化强调色）；Win7 回退默认蓝
COLORREF SystemAccentColor() {
    DWORD value = 0;
    // AccentColorMenu 为 0xAABBGGRR
    if (ReadRegDword(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentColorMenu", &value) &&
        value != 0 && value != 0xFFFFFFFF) {
        return RGB(value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF);
    }
    // ColorizationColor 为 0xAARRGGBB
    if (ReadRegDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM",
                     L"ColorizationColor", &value) && value != 0) {
        return RGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
    }
    return RGB(0, 120, 212);
}

void ResolveTheme() {
    g_themeResolved = true;

    bool dark = false;
    if (g_themeMode == Fluent::ThemeMode::Dark) dark = true;
    else if (g_themeMode == Fluent::ThemeMode::Light) dark = false;
    else dark = SystemPrefersDarkTheme();
    // 高对比度下所有控件回退系统配色，主题色不参与绘制
    if (Fluent::IsHighContrast()) dark = false;
    g_darkTheme = dark;

    // 主题色适配：深色下适当提亮、浅色下过亮时压暗，保证主按钮文字可读
    COLORREF accent = SystemAccentColor();
    const int luma = (GetRValue(accent) * 299 + GetGValue(accent) * 587 +
                      GetBValue(accent) * 114) / 1000;
    if (dark) {
        accent = BlendColor(accent, RGB(255, 255, 255), luma < 60 ? 45 : 10);
    } else if (luma > 190) {
        accent = BlendColor(accent, RGB(0, 0, 0), 30);
    }

    Palette& p = dark ? g_darkPalette : g_lightPalette;
    p.accent = accent;
    p.accentHover = BlendColor(accent, dark ? RGB(255, 255, 255) : RGB(0, 0, 0),
                               dark ? 10 : 8);
    p.accentPressed = BlendColor(accent, dark ? RGB(255, 255, 255) : RGB(0, 0, 0),
                                 dark ? 22 : 18);
}

const Palette& P() {
    if (!g_themeResolved) ResolveTheme();
    return g_darkTheme ? g_darkPalette : g_lightPalette;
}

// ============================================================
// 字体与画刷（惰性创建；永不析构，避免进程退出时静态析构顺序问题）
// ============================================================
struct ThemeCache {
    HFONT ui = nullptr;
    HFONT semibold = nullptr;
    HFONT caption = nullptr;   // 辅助文字字体（12px）
    HBRUSH surface = nullptr;
    HBRUSH page = nullptr;
};

ThemeCache& Theme() {
    static ThemeCache* cache = new ThemeCache();
    return *cache;
}

HFONT CreateFluentFontAtScale(int logicalHeight, int weight, float scale) {
    LOGFONTW lf = {};
    lf.lfHeight = -(int)(logicalHeight * (scale > 0.0f ? scale : 1.0f) + 0.5f);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    // 中文等非拉丁字符由 GDI 字体链接自动回退到微软雅黑/宋体
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

HFONT CreateFluentFont(int logicalHeight, int weight) {
    return CreateFluentFontAtScale(logicalHeight, weight, GetUiScale());
}

void EnsureFonts() {
    ThemeCache& t = Theme();
    if (t.ui) return;
    t.ui = CreateFluentFont(14, FW_NORMAL);
    t.semibold = CreateFluentFont(14, FW_SEMIBOLD);
    t.caption = CreateFluentFont(12, FW_NORMAL);
    t.surface = CreateSolidBrush(P().surface);
    t.page = CreateSolidBrush(P().tabStrip);
}

// 清空按颜色缓存的画笔/画刷：主题或高对比度切换后旧颜色不再使用，
// 不清理会随切换次数持续累积 GDI 句柄（R14-A8）
void ClearCachedGdiObjects();

// 主题切换后重建缓存画刷（字体与字号不变）
void RebuildThemeBrushes() {
    ThemeCache& t = Theme();
    if (!t.ui) return;   // 尚未创建：EnsureFonts 会按新配色创建
    if (t.surface) { DeleteObject(t.surface); t.surface = nullptr; }
    if (t.page) { DeleteObject(t.page); t.page = nullptr; }
    t.surface = CreateSolidBrush(P().surface);
    t.page = CreateSolidBrush(P().tabStrip);
}

// 深色模式下让输入类控件的系统滚动条使用深色主题（Win10 1809+ 生效；
// 旧系统不认识该主题名，调用失败即保持默认外观）
void ApplyDarkControlTheme(HWND control) {
    typedef HRESULT(WINAPI * PFN_SetWindowTheme)(HWND, LPCWSTR, LPCWSTR);
    static PFN_SetWindowTheme setWindowTheme = []() -> PFN_SetWindowTheme {
        HMODULE uxtheme = DpiUtils::LoadSystemModule(L"uxtheme.dll");
        if (!uxtheme) return nullptr;
        return reinterpret_cast<PFN_SetWindowTheme>(
            GetProcAddress(uxtheme, "SetWindowTheme"));
    }();
    if (!setWindowTheme) return;
    if (Fluent::IsDarkTheme())
        setWindowTheme(control, L"DarkMode_Explorer", nullptr);
    else
        setWindowTheme(control, nullptr, nullptr);   // 恢复系统默认主题
}

// ============================================================
// GDI 对象缓存：按钮/复选框/边框在每次 WM_PAINT 中都会用到同几种颜色，
// 反复 Create/Delete 会造成 GDI 抖动。缓存的画刷/画笔与字体一样
// 常驻进程生命周期，数量受调色板颜色数限制。
// ============================================================
std::unordered_map<COLORREF, HBRUSH>& CachedBrushMap() {
    // 堆上永不释放：绘制可能发生在全局对象析构阶段，普通静态 map 会被提前销毁
    static auto* cache = new std::unordered_map<COLORREF, HBRUSH>();
    return *cache;
}

std::unordered_map<uint64_t, HPEN>& CachedPenMap() {
    static auto* cache = new std::unordered_map<uint64_t, HPEN>();
    return *cache;
}

HBRUSH CachedSolidBrush(COLORREF color) {
    auto& cache = CachedBrushMap();
    auto it = cache.find(color);
    if (it != cache.end()) return it->second;
    HBRUSH brush = CreateSolidBrush(color);
    cache.emplace(color, brush);
    return brush;
}

HPEN CachedPen(int width, COLORREF color) {
    auto& cache = CachedPenMap();
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(width)) << 32) |
                         static_cast<uint32_t>(color);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    HPEN pen = CreatePen(PS_SOLID, width, color);
    cache.emplace(key, pen);
    return pen;
}

void ClearCachedGdiObjects() {
    // 只能在 UI 线程、非绘制期间调用（主题切换/高对比度刷新路径）：
    // 删除后所有缓存项失效，下一次绘制会按新配色重新创建
    for (auto& kv : CachedBrushMap()) {
        if (kv.second) DeleteObject(kv.second);
    }
    CachedBrushMap().clear();
    for (auto& kv : CachedPenMap()) {
        if (kv.second) DeleteObject(kv.second);
    }
    CachedPenMap().clear();
}

// 1px 圆角描边（不填充）
void StrokeRoundRect(HDC hdc, const RECT& rc, int radius, COLORREF color) {
    HPEN pen = CachedPen(1, color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
}


// ============================================================
// 按钮：白底/强调色实底 + 4px 圆角 + 悬停/按下/禁用/焦点四态
// ============================================================
struct ButtonState {
    bool hover = false;
    bool pressed = false;
    Fluent::ButtonKind kind = Fluent::ButtonKind::Secondary;
};

void PaintFluentButton(HWND hWnd, HDC hdc, const ButtonState* state) {
    RECT rc;
    GetClientRect(hWnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    const bool disabled = !IsWindowEnabled(hWnd);
    // 按下态由子类自行记录：系统经 BM_SETSTATE 触发的经典凹陷绘制已被拦截
    const bool pressed = state && state->pressed;
    const bool focused = GetFocus() == hWnd;
    const bool hover = state && state->hover;
    const Fluent::ButtonKind kind = state ? state->kind : Fluent::ButtonKind::Secondary;

    COLORREF fill = P().surface;
    COLORREF border = P().border;
    COLORREF text = P().text;

    if (kind == Fluent::ButtonKind::Primary) {
        fill = disabled ? P().disabledBg
             : pressed  ? P().accentPressed
             : hover    ? P().accentHover
                        : P().accent;
        border = disabled ? P().disabledBorder : fill;
        text = disabled ? P().textDisabled : P().onAccent;
    } else if (kind == Fluent::ButtonKind::Danger) {
        fill = disabled ? P().disabledBg
             : pressed  ? P().dangerPressedBg
             : hover    ? P().dangerHoverBg
                        : P().surface;
        border = disabled ? P().disabledBorder : P().danger;
        text = disabled ? P().textDisabled : P().danger;
    } else {
        fill = disabled ? P().disabledBg
             : pressed  ? P().pressed
             : hover    ? P().hover
                        : P().surface;
        border = disabled ? P().disabledBorder
               : focused  ? P().accent
               : hover    ? P().borderHover
                          : P().border;
        text = disabled ? P().textDisabled : P().text;
    }

    // 先铺满内容底色，再画圆角：避免圆角外的残留像素
    FillRect(hdc, &rc, Fluent::SurfaceBrush());
    RECT body = { rc.left, rc.top, rc.right, rc.bottom };
    Fluent::FillRoundRect(hdc, body, ScaleUi(4), fill, border);

    // 焦点提示：主按钮在实底上画浅色内圈，其余按钮边框已变强调色
    if (focused && kind == Fluent::ButtonKind::Primary && !disabled) {
        RECT inner = { rc.left + ScaleUi(3), rc.top + ScaleUi(3),
                       rc.right - ScaleUi(3), rc.bottom - ScaleUi(3) };
        if (inner.right > inner.left && inner.bottom > inner.top)
            StrokeRoundRect(hdc, inner, ScaleUi(3), P().onAccent);
    }

    wchar_t label[256] = L"";
    GetWindowTextW(hWnd, label, 256);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, text);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hWnd, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = SelectObject(hdc, font ? font : Fluent::UiFont());
    RECT tr = rc;
    // 小尺寸按钮（如虚拟键盘键帽）自动收紧左右内边距，避免文字被省略
    int pad = ScaleUi(8);
    SIZE textSize = { 0, 0 };
    if (label[0]) GetTextExtentPoint32W(hdc, label, (int)wcslen(label), &textSize);
    if (textSize.cx > (rc.right - rc.left) - pad * 2) pad = ScaleUi(2);
    InflateRect(&tr, -pad, 0);
    DrawTextW(hdc, label, -1, &tr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
}

// 转交系统默认处理前暂停该控件的重绘：系统会在按下/选中时先绘制一次经典样式，
// 随后再被 Fluent 自绘覆盖，既闪一下又产生一次多余绘制。
// 这里只借用系统处理来更新内部状态与行为，绘制统一交给随后的 Fluent WM_PAINT。
static LRESULT DefaultProcNoPaint(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    SendMessageW(hWnd, WM_SETREDRAW, FALSE, 0);
    LRESULT result = DefSubclassProc(hWnd, msg, wp, lp);
    SendMessageW(hWnd, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hWnd, nullptr, FALSE);
    return result;
}

LRESULT CALLBACK ButtonProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp,
                            UINT_PTR /*id*/, DWORD_PTR ref) {
    auto* state = reinterpret_cast<ButtonState*>(ref);
    switch (msg) {
    case WM_ERASEBKGND:
        // 运行中开启高对比度后，已安装的子类仍然存在：把绘制交还系统，
        // 避免继续使用主题调色板导致低对比度
        if (Fluent::IsHighContrast()) break;
        return 1;
    case WM_MOUSEMOVE:
        if (state && !state->hover) {
            state->hover = true;
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        if (state && state->hover) {
            state->hover = false;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
    case WM_LBUTTONDOWN:
        if (state) state->pressed = true;
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    case WM_LBUTTONUP:
        if (state) state->pressed = false;
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    case WM_CAPTURECHANGED:
        // 拖出按钮后系统会取消按下态，视觉同步复位
        if (state && state->pressed) {
            state->pressed = false;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
    case BM_SETSTATE: {
        // 必须交给系统更新内部按下状态（否则抬起时不会发送 BN_CLICKED），
        // 但不要让它绘制经典凹陷
        return DefaultProcNoPaint(hWnd, msg, wp, lp);
    }
    case WM_KEYDOWN:
    case WM_KEYUP:
        if (wp == VK_SPACE) InvalidateRect(hWnd, nullptr, FALSE);
        break;
    case WM_ENABLE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_SETTEXT:
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    case WM_PRINTCLIENT: {
        if (Fluent::IsHighContrast()) break;
        HDC hdc = reinterpret_cast<HDC>(wp);
        if (hdc) PaintFluentButton(hWnd, hdc, state);
        return 0;
    }
    case WM_NCDESTROY:
        delete state;
        break;
    case WM_PAINT: {
        if (Fluent::IsHighContrast()) break;
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        PaintFluentButton(hWnd, hdc, state);
        EndPaint(hWnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefSubclassProc(hWnd, msg, wp, lp);
}

// ============================================================
// 复选框 / 单选框：Fluent 圆角方框与圆点，选中态使用强调色
// ============================================================
struct ToggleState {
    bool hover = false;
    LONG_PTR originalType = BS_AUTOCHECKBOX;   // 控件原生类型（保留系统状态与无障碍语义）
};

bool IsRadioType(LONG_PTR type) {
    return type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
}

void PaintFluentToggle(HWND hWnd, HDC hdc, const ToggleState* state) {
    RECT rc;
    GetClientRect(hWnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    FillRect(hdc, &rc, Fluent::SurfaceBrush());

    const LONG_PTR type = state ? state->originalType
                                : (GetWindowLongPtrW(hWnd, GWL_STYLE) & BS_TYPEMASK);
    const bool isRadio = IsRadioType(type);
    const bool disabled = !IsWindowEnabled(hWnd);
    const bool hover = state && state->hover;
    // 选中状态由系统按钮维护：owner-draw 会丢失状态与无障碍语义，
    // 这里保留原生类型、只接管绘制
    const int checkState = (int)SendMessageW(hWnd, BM_GETCHECK, 0, 0);
    const bool checked = (checkState == BST_CHECKED || checkState == BST_INDETERMINATE);

    const int box = ScaleUi(16);
    int top = rc.top + ((rc.bottom - rc.top) - box) / 2;
    if (top < rc.top) top = rc.top;
    RECT boxRect = { rc.left + ScaleUi(1), top, rc.left + ScaleUi(1) + box, top + box };

    const COLORREF accent = disabled ? P().textDisabled : P().accent;
    const COLORREF idleBorder = disabled ? P().disabledBorder
                               : hover    ? P().textSecondary
                                          : P().border;

    if (isRadio) {
        // 未选中：白底灰圆环；选中：强调色圆环 + 强调色圆心
        HBRUSH fill = CachedSolidBrush(disabled ? P().disabledBg : P().surface);
        HPEN ring = CachedPen(checked ? ScaleUi(2) : 1, checked ? accent : idleBorder);
        HGDIOBJ oldBrush = SelectObject(hdc, fill);
        HGDIOBJ oldPen = SelectObject(hdc, ring);
        Ellipse(hdc, boxRect.left, boxRect.top, boxRect.right, boxRect.bottom);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        if (checked) {
            const int inset = box * 32 / 100;
            HBRUSH dot = CachedSolidBrush(accent);
            HGDIOBJ prevBrush = SelectObject(hdc, dot);
            HGDIOBJ prevPen = SelectObject(hdc, GetStockObject(NULL_PEN));
            Ellipse(hdc, boxRect.left + inset, boxRect.top + inset,
                    boxRect.right - inset, boxRect.bottom - inset);
            SelectObject(hdc, prevPen);
            SelectObject(hdc, prevBrush);
        }
    } else {
        // 禁用且勾选时不再用强调色实底：否则看起来仍像可点击的启用控件，
        // 改用与禁用文字同色的灰底灰勾
        const COLORREF boxFill = checked ? (disabled ? P().disabledBg : accent)
                                         : (disabled ? P().disabledBg : P().surface);
        const COLORREF boxBorder = checked ? (disabled ? P().disabledBorder : accent) : idleBorder;
        Fluent::FillRoundRect(hdc, boxRect, ScaleUi(3), boxFill, boxBorder);
        const COLORREF markColor = disabled ? P().textDisabled : P().onAccent;
        if (checkState == BST_CHECKED) {
            // 对勾
            POINT tick[3] = {
                { boxRect.left + box * 27 / 100, boxRect.top + box * 52 / 100 },
                { boxRect.left + box * 43 / 100, boxRect.top + box * 68 / 100 },
                { boxRect.left + box * 74 / 100, boxRect.top + box * 34 / 100 }
            };
            int penWidth = ScaleUi(2);
            if (penWidth < 1) penWidth = 1;
            HPEN pen = CachedPen(penWidth, markColor);
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            Polyline(hdc, tick, 3);
            SelectObject(hdc, oldPen);
        } else if (checkState == BST_INDETERMINATE) {
            // 半选：横杠
            RECT bar = { boxRect.left + box * 25 / 100, boxRect.top + box * 45 / 100,
                         boxRect.right - box * 25 / 100, boxRect.top + box * 56 / 100 };
            HBRUSH mark = CachedSolidBrush(markColor);
            FillRect(hdc, &bar, mark);
        }
    }

    wchar_t text[512] = L"";
    GetWindowTextW(hWnd, text, 512);
    SetBkMode(hdc, TRANSPARENT);
    // 高对比度下背景由 SurfaceBrush 换成系统色，文字也必须跟随系统色，
    // 否则运行中开启高对比度会出现"黑底黑字"
    SetTextColor(hdc, disabled ? Fluent::TextDisabledColor() : Fluent::TextColor());
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hWnd, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = SelectObject(hdc, font ? font : Fluent::UiFont());
    RECT tr = { boxRect.right + ScaleUi(8), rc.top, rc.right, rc.bottom };
    DrawTextW(hdc, text, -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(hdc, oldFont);

    if (GetFocus() == hWnd) {
        RECT focusRect = { boxRect.right + ScaleUi(4), rc.top + ScaleUi(1),
                           rc.right - ScaleUi(1), rc.bottom - ScaleUi(1) };
        if (focusRect.right > focusRect.left && focusRect.bottom > focusRect.top)
            DrawFocusRect(hdc, &focusRect);
    }
}

// 清除同组其他单选框（以 WS_GROUP 为边界）
void ClearRadioGroup(HWND hWnd) {
    if (!GetParent(hWnd)) return;
    // 自己带 WS_GROUP 时本窗口就是组起点，向前不存在同组元素
    if (!(GetWindowLongPtrW(hWnd, GWL_STYLE) & WS_GROUP)) {
        for (HWND w = GetWindow(hWnd, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
            const bool groupStart = (GetWindowLongPtrW(w, GWL_STYLE) & WS_GROUP) != 0;
            if (IsRadioType(GetWindowLongPtrW(w, GWL_STYLE) & BS_TYPEMASK)) {
                SendMessageW(w, BM_SETCHECK, BST_UNCHECKED, 0);
                InvalidateRect(w, nullptr, FALSE);
            }
            if (groupStart) break;
        }
    }
    for (HWND w = GetWindow(hWnd, GW_HWNDNEXT); w; w = GetWindow(w, GW_HWNDNEXT)) {
        if (GetWindowLongPtrW(w, GWL_STYLE) & WS_GROUP) break;
        if (IsRadioType(GetWindowLongPtrW(w, GWL_STYLE) & BS_TYPEMASK)) {
            SendMessageW(w, BM_SETCHECK, BST_UNCHECKED, 0);
            InvalidateRect(w, nullptr, FALSE);
        }
    }
}

// 由本模块完成切换：状态仍存进系统（BM_SETCHECK），无障碍工具可正确读取，
// 同时完全不让系统参与绘制，避免经典样式闪现或覆盖自绘文字
void ToggleCurrentState(HWND hWnd, LONG_PTR type) {
    if (IsRadioType(type)) {
        ClearRadioGroup(hWnd);
        SendMessageW(hWnd, BM_SETCHECK, BST_CHECKED, 0);
    } else if (type == BS_3STATE || type == BS_AUTO3STATE) {
        const int cur = (int)SendMessageW(hWnd, BM_GETCHECK, 0, 0);
        SendMessageW(hWnd, BM_SETCHECK,
                     cur == BST_UNCHECKED ? BST_CHECKED
                   : cur == BST_CHECKED ? BST_INDETERMINATE : BST_UNCHECKED, 0);
    } else {
        const int cur = (int)SendMessageW(hWnd, BM_GETCHECK, 0, 0);
        SendMessageW(hWnd, BM_SETCHECK, cur == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
    }
    InvalidateRect(hWnd, nullptr, FALSE);
    HWND parent = GetParent(hWnd);
    if (parent)
        SendMessageW(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hWnd), BN_CLICKED), (LPARAM)hWnd);
}

// 复选框/单选框保留原生类型：选中状态、点击切换、空格/方向键、
// 单选组互斥与无障碍状态全部由系统维护，本模块只接管绘制
LRESULT CALLBACK ToggleProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp,
                            UINT_PTR /*id*/, DWORD_PTR ref) {
    auto* state = reinterpret_cast<ToggleState*>(ref);
    switch (msg) {
    case WM_ERASEBKGND:
        // 运行中开启高对比度：已安装的子类把绘制交还系统
        if (Fluent::IsHighContrast()) break;
        return 1;
    case WM_MOUSEMOVE:
        if (state && !state->hover) {
            state->hover = true;
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;   // 不交给系统：避免其绘制热态
    case WM_MOUSELEAVE:
        if (state && state->hover) {
            state->hover = false;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (!IsWindowEnabled(hWnd)) return 0;
        SetFocus(hWnd);
        SetCapture(hWnd);
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hWnd) ReleaseCapture();
        if (IsWindowEnabled(hWnd)) {
            RECT rcClient;
            GetClientRect(hWnd, &rcClient);
            POINT pt = { (int)(short)LOWORD(lp), (int)(short)HIWORD(lp) };
            if (PtInRect(&rcClient, pt)) {
                ToggleCurrentState(hWnd, state ? state->originalType
                                               : (GetWindowLongPtrW(hWnd, GWL_STYLE) & BS_TYPEMASK));
            }
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        return 0;
    case WM_KEYDOWN:
        // 空格在抬键时切换：按住不放不会连续切换
        if (wp == VK_SPACE) return 0;
        break;
    case WM_KEYUP:
        if (wp == VK_SPACE && IsWindowEnabled(hWnd)) {
            ToggleCurrentState(hWnd, state ? state->originalType
                                           : (GetWindowLongPtrW(hWnd, GWL_STYLE) & BS_TYPEMASK));
            return 0;
        }
        break;
    case BM_CLICK:
        // 无障碍工具（读屏的 DoDefaultAction 等）以 BM_CLICK 触发时，
        // 同样交给本模块切换，避免系统走经典绘制路径
        if (IsWindowEnabled(hWnd))
            ToggleCurrentState(hWnd, state ? state->originalType
                                           : (GetWindowLongPtrW(hWnd, GWL_STYLE) & BS_TYPEMASK));
        return 0;
    case WM_ENABLE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_SETTEXT:
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    case WM_PRINTCLIENT: {
        if (Fluent::IsHighContrast()) break;
        HDC hdc = reinterpret_cast<HDC>(wp);
        if (hdc) PaintFluentToggle(hWnd, hdc, state);
        return 0;
    }
    case WM_NCDESTROY:
        delete state;
        break;
    case WM_PAINT: {
        if (Fluent::IsHighContrast()) break;
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        PaintFluentToggle(hWnd, hdc, state);
        EndPaint(hWnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefSubclassProc(hWnd, msg, wp, lp);
}

// ============================================================
// 下拉框：自绘显示区（白底圆角 + 箭头），下拉列表沿用系统绘制
// ============================================================
struct ComboState {
    bool hover = false;
};

void PaintFluentCombo(HWND hWnd, HDC hdc, const ComboState* state) {
    RECT rc;
    GetClientRect(hWnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    const bool disabled = !IsWindowEnabled(hWnd);
    const bool focused = (GetFocus() == hWnd);
    const bool hover = state && state->hover;

    COLORREF border = disabled ? P().disabledBorder
                    : focused  ? P().accent
                    : hover    ? P().borderHover
                               : P().border;
    COLORREF fill = disabled ? P().disabledBg : P().surface;

    // 先铺父窗口底色，再画圆角，避免圆角外残留旧像素
    FillRect(hdc, &rc, Fluent::SurfaceBrush());
    Fluent::FillRoundRect(hdc, rc, ScaleUi(3), fill, border);

    const int arrowWidth = ScaleUi(22);
    const int centerX = rc.right - arrowWidth / 2 - ScaleUi(2);
    const int centerY = rc.top + (rc.bottom - rc.top) / 2;
    const int half = ScaleUi(3);
    POINT arrow[3] = {
        { centerX - half, centerY - ScaleUi(2) },
        { centerX + half, centerY - ScaleUi(2) },
        { centerX,        centerY + ScaleUi(2) }
    };
    COLORREF arrowColor = disabled ? P().textDisabled : P().textSecondary;
    HBRUSH arrowBrush = CachedSolidBrush(arrowColor);
    HPEN arrowPen = CachedPen(1, arrowColor);
    HGDIOBJ oldBrush = SelectObject(hdc, arrowBrush);
    HGDIOBJ oldPen = SelectObject(hdc, arrowPen);
    Polygon(hdc, arrow, 3);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);

    int selected = (int)SendMessageW(hWnd, CB_GETCURSEL, 0, 0);
    // CB_GETLBTEXT 不接收缓冲区长度，固定栈缓冲会被超长条目写穿：
    // 先取实际长度再按需分配（上限兜底，避免异常条目申请超大内存）
    std::wstring text;
    if (selected >= 0) {
        LRESULT len = SendMessageW(hWnd, CB_GETLBTEXTLEN, (WPARAM)selected, 0);
        if (len > 0 && len <= 4096) {
            text.resize((size_t)len + 1, L'\0');
            SendMessageW(hWnd, CB_GETLBTEXT, (WPARAM)selected, (LPARAM)&text[0]);
            text.resize(wcslen(text.c_str()));
        }
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, disabled ? P().textDisabled : P().text);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hWnd, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = SelectObject(hdc, font ? font : Fluent::UiFont());
    RECT tr = { rc.left + ScaleUi(8), rc.top, rc.right - arrowWidth, rc.bottom };
    DrawTextW(hdc, text.c_str(), -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(hdc, oldFont);
}

LRESULT CALLBACK ComboProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp,
                           UINT_PTR /*id*/, DWORD_PTR ref) {
    auto* state = reinterpret_cast<ComboState*>(ref);
    switch (msg) {
    case WM_ERASEBKGND:
        // 运行中开启高对比度：已安装的子类把绘制交还系统
        if (Fluent::IsHighContrast()) break;
        return 1;
    case WM_MOUSEMOVE:
        if (state && !state->hover) {
            state->hover = true;
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        if (state && state->hover) {
            state->hover = false;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
    case WM_ENABLE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK: {
        // 系统在下拉按钮按下时会直接绘制经典凹陷，禁止其在默认处理中绘制
        return DefaultProcNoPaint(hWnd, msg, wp, lp);
    }
    case WM_PRINTCLIENT: {
        if (Fluent::IsHighContrast()) break;
        HDC hdc = reinterpret_cast<HDC>(wp);
        if (hdc) PaintFluentCombo(hWnd, hdc, state);
        return 0;
    }
    case WM_NCDESTROY:
        delete state;
        break;
    case WM_PAINT: {
        if (Fluent::IsHighContrast()) break;
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        PaintFluentCombo(hWnd, hdc, state);
        EndPaint(hWnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefSubclassProc(hWnd, msg, wp, lp);
}

// ============================================================
// 列表表头：白底、12px 灰色文字、底部分隔线与列间竖线（Fluent 表格）
// ============================================================
void PaintHeader(HWND header, HDC hdc) {
    RECT rc;
    GetClientRect(header, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    // 父列表被禁用时表头同步呈灰底灰字
    HWND listView = GetParent(header);
    const bool headerEnabled = !listView || IsWindowEnabled(listView) != FALSE;
    const bool hc = Fluent::IsHighContrast();
    HBRUSH disabledBrush = nullptr;
    if (headerEnabled) {
        FillRect(hdc, &rc, Fluent::SurfaceBrush());
    } else {
        disabledBrush = CachedSolidBrush(hc ? GetSysColor(COLOR_3DFACE) : P().disabledBg);
        FillRect(hdc, &rc, disabledBrush);
    }
    // 高对比度下表头底色已换成系统色，文字改用系统文字色保证可读
    const COLORREF headerText = headerEnabled
        ? (hc ? GetSysColor(COLOR_BTNTEXT) : P().textSecondary)
        : (hc ? GetSysColor(COLOR_GRAYTEXT) : P().textDisabled);
    const int count = Header_GetItemCount(header);

    // 表格框线：底部分隔线 + 列间竖线，保持各列区域边界可辨
    if (rc.bottom > 1) {
        RECT line = { rc.left, rc.bottom - 1, rc.right, rc.bottom };
        HBRUSH brush = CachedSolidBrush(P().border);
        FillRect(hdc, &line, brush);
    }
    for (int i = 1; i < count; ++i) {
        RECT item;
        if (!Header_GetItemRect(header, i, &item)) continue;
        const int x = item.left;
        if (x <= rc.left) continue;
        RECT line = { x, rc.top + ScaleUi(5), x + 1, rc.bottom - ScaleUi(5) };
        if (line.bottom <= line.top) continue;
        HBRUSH brush = CachedSolidBrush(P().border);
        FillRect(hdc, &line, brush);
    }

    SetBkMode(hdc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(hdc, Fluent::SmallFont());

    for (int i = 0; i < count; ++i) {
        RECT item;
        if (!Header_GetItemRect(header, i, &item)) continue;

        wchar_t text[128] = L"";
        HDITEMW info = {};
        info.mask = HDI_TEXT | HDI_FORMAT;
        info.pszText = text;
        info.cchTextMax = 128;
        if (!Header_GetItem(header, i, &info)) continue;

        UINT align = DT_LEFT;
        if (info.fmt & HDF_CENTER) align = DT_CENTER;
        else if (info.fmt & HDF_RIGHT) align = DT_RIGHT;

        SetTextColor(hdc, headerText);
        RECT tr = item;
        tr.left += ScaleUi(10);
        tr.right -= ScaleUi(6);
        DrawTextW(hdc, text, -1, &tr,
                  align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        // 排序列指示箭头
        if (info.fmt & (HDF_SORTUP | HDF_SORTDOWN)) {
            const bool ascending = (info.fmt & HDF_SORTUP) != 0;
            const int cx = item.right - ScaleUi(10);
            const int cy = item.top + (item.bottom - item.top) / 2;
            const int half = ScaleUi(3);
            const int rise = ScaleUi(2);
            POINT arrow[3] = {};
            if (ascending) {
                arrow[0] = { cx - half, cy + rise };
                arrow[1] = { cx + half, cy + rise };
                arrow[2] = { cx,        cy - rise };
            } else {
                arrow[0] = { cx - half, cy - rise };
                arrow[1] = { cx + half, cy - rise };
                arrow[2] = { cx,        cy + rise };
            }
            HBRUSH brush = CachedSolidBrush(headerText);
            HPEN pen = CachedPen(1, headerText);
            HGDIOBJ oldBrush = SelectObject(hdc, brush);
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            Polygon(hdc, arrow, 3);
            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBrush);
        }
    }
    SelectObject(hdc, oldFont);
    // disabledBrush 为缓存对象，不能在此释放
}

LRESULT CALLBACK HeaderProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp,
                            UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
        // 表头按下时会自行绘制热态，禁止其在默认处理中绘制
        return DefaultProcNoPaint(hWnd, msg, wp, lp);
    }
    case WM_PRINTCLIENT: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        if (hdc) PaintHeader(hWnd, hdc);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        PaintHeader(hWnd, hdc);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_SETFONT:
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    default:
        break;
    }
    return DefSubclassProc(hWnd, msg, wp, lp);
}

// ============================================================
// 数值调节按钮（UpDown）：白底、灰色小三角、悬停高亮
// ============================================================
struct SpinState {
    int hot = -1;   // 0=上 1=下
};

void PaintFluentSpin(HWND hWnd, HDC hdc, const SpinState* state) {
    RECT rc;
    GetClientRect(hWnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    const bool disabled = !IsWindowEnabled(hWnd);
    FillRect(hdc, &rc, Fluent::SurfaceBrush());

    const int mid = rc.top + (rc.bottom - rc.top) / 2;
    RECT upRect = { rc.left, rc.top, rc.right, mid };
    RECT downRect = { rc.left, mid, rc.right, rc.bottom };

    if (!disabled && state && state->hot >= 0) {
        HBRUSH brush = CachedSolidBrush(P().hover);
        FillRect(hdc, state->hot == 0 ? &upRect : &downRect, brush);
    }
    // 上下两半之间的细分隔线
    if (mid - rc.top > ScaleUi(6)) {
        RECT line = { rc.left + ScaleUi(3), mid, rc.right - ScaleUi(3), mid + 1 };
        HBRUSH brush = CachedSolidBrush(P().border);
        FillRect(hdc, &line, brush);
    }

    const COLORREF color = disabled ? P().textDisabled : P().textSecondary;
    const int cx = rc.left + (rc.right - rc.left) / 2;
    const int half = ScaleUi(3) > 0 ? ScaleUi(3) : 3;
    const int rise = ScaleUi(2) > 0 ? ScaleUi(2) : 2;
    const int upCenter = rc.top + (mid - rc.top) / 2;
    const int downCenter = mid + (rc.bottom - mid) / 2;

    POINT upArrow[3] = { { cx - half, upCenter + rise },
                         { cx + half, upCenter + rise },
                         { cx,        upCenter - rise } };
    POINT downArrow[3] = { { cx - half, downCenter - rise },
                           { cx + half, downCenter - rise },
                           { cx,        downCenter + rise } };

    HBRUSH brush = CachedSolidBrush(color);
    HPEN pen = CachedPen(1, color);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    Polygon(hdc, upArrow, 3);
    Polygon(hdc, downArrow, 3);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
}

LRESULT CALLBACK SpinProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp,
                          UINT_PTR /*id*/, DWORD_PTR ref) {
    // 高对比度下主题调色板与系统配色不一致（箭头/分隔线可能对比度不足），
    // 与按钮/下拉框保持一致，把绘制交还系统；不影响状态对象的销毁。
    if ((msg == WM_ERASEBKGND || msg == WM_PAINT || msg == WM_PRINTCLIENT) &&
        Fluent::IsHighContrast()) {
        return DefSubclassProc(hWnd, msg, wp, lp);
    }
    auto* state = reinterpret_cast<SpinState*>(ref);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        if (state) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            int y = (int)(short)HIWORD(lp);
            int hot = (y < (rc.bottom - rc.top) / 2) ? 0 : 1;
            if (hot != state->hot) {
                state->hot = hot;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    }
    case WM_MOUSELEAVE:
        if (state && state->hot != -1) {
            state->hot = -1;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
        // 系统在按下时会直接绘制凹陷，禁止其在默认处理中绘制
        return DefaultProcNoPaint(hWnd, msg, wp, lp);
    }
    case WM_ENABLE:
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    case WM_PRINTCLIENT: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        if (hdc) PaintFluentSpin(hWnd, hdc, state);
        return 0;
    }
    case WM_NCDESTROY:
        delete state;
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        PaintFluentSpin(hWnd, hdc, state);
        EndPaint(hWnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefSubclassProc(hWnd, msg, wp, lp);
}

// ============================================================
// 分组框：Windows 标准样式——与页面同底色、1px 圆角边框、
// 标题嵌在顶边框线上，不再画成独立的白色卡片
// ============================================================
void PaintGroupBox(HWND hWnd, HDC hdc) {
    RECT rc;
    GetClientRect(hWnd, &rc);
    if (rc.right <= 2 || rc.bottom <= 2) return;

    // 背景与选项卡页面一致，避免浅色主题下出现突兀的白色块
    FillRect(hdc, &rc, Fluent::SurfaceBrush());

    wchar_t title[256] = L"";
    GetWindowTextW(hWnd, title, 256);

    HGDIOBJ oldFont = SelectObject(hdc, Fluent::UiFontSemibold());
    SIZE titleSize = {};
    if (title[0]) {
        GetTextExtentPoint32W(hdc, title, (int)wcslen(title), &titleSize);
    }
    TEXTMETRICW tm = {};
    GetTextMetricsW(hdc, &tm);
    const int titleH = tm.tmHeight;

    // 顶边框放在标题垂直中线处；标题较高（高 DPI）时随之下移
    int lineY = ScaleUi(8);
    if (title[0] && titleH / 2 > lineY) lineY = titleH / 2;
    const int left = rc.left;
    const int right = rc.right - 1;
    const int bottom = rc.bottom - 1;
    const int titleX = left + ScaleUi(12);

    RECT border = { left, lineY, right, bottom };
    StrokeRoundRect(hdc, border, ScaleUi(4), P().border);

    if (title[0]) {
        // 在边框线上为标题挖出缺口：文字使用页面底色擦除线段，
        // 与系统 BS_GROUPBOX 的标题位置一致
        int titleW = titleSize.cx;
        const int maxTitleW = right - titleX - ScaleUi(12);
        if (maxTitleW > 0 && titleW > maxTitleW) titleW = maxTitleW;
        const int gapPad = ScaleUi(6);
        RECT gap = { titleX - gapPad, lineY - titleH / 2,
                     titleX + titleW + gapPad, lineY + titleH / 2 };
        FillRect(hdc, &gap, Fluent::SurfaceBrush());
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, Fluent::TextColor());
        RECT tr = { titleX, lineY - titleH / 2,
                    titleX + titleW + ScaleUi(2), lineY + titleH / 2 };
        DrawTextW(hdc, title, -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(hdc, oldFont);
}

LRESULT CALLBACK GroupBoxProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp,
                              UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    // 高对比度下卡片边框使用固定主题色，可能与系统背景冲突；
    // 绘制交还系统，与已回退的按钮/复选/下拉框保持一致。
    if ((msg == WM_ERASEBKGND || msg == WM_PAINT || msg == WM_PRINTCLIENT) &&
        Fluent::IsHighContrast()) {
        return DefSubclassProc(hWnd, msg, wp, lp);
    }
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(hWnd, &rc);
        FillRect(reinterpret_cast<HDC>(wp), &rc, Fluent::SurfaceBrush());
        return 1;
    }
    if (msg == WM_PRINTCLIENT) {
        HDC hdc = reinterpret_cast<HDC>(wp);
        if (hdc) PaintGroupBox(hWnd, hdc);
        return 0;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        PaintGroupBox(hWnd, hdc);
        EndPaint(hWnd, &ps);
        return 0;
    }
    return DefSubclassProc(hWnd, msg, wp, lp);
}

// ============================================================
// 输入类控件：去掉系统黑色下沉边框，改画 1px 灰色圆角细线
// ============================================================
bool IsStandaloneSingleLineEdit(HWND hWnd) {
    wchar_t cls[32] = L"";
    GetClassNameW(hWnd, cls, 32);
    if (_wcsicmp(cls, L"EDIT") != 0) return false;
    if (GetWindowLongPtrW(hWnd, GWL_STYLE) & ES_MULTILINE) return false;
    wchar_t parentCls[32] = L"";
    HWND parent = GetParent(hWnd);
    if (parent) GetClassNameW(parent, parentCls, 32);
    return _wcsicmp(parentCls, L"COMBOBOX") != 0;
}

// 单行输入框顶部留白：让文字与光标在框内垂直居中
int GetEditTopPadding(HWND hWnd, int winHeight) {
    if (winHeight <= 0 || !IsStandaloneSingleLineEdit(hWnd)) return 0;
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hWnd, WM_GETFONT, 0, 0));
    HDC hdc = GetDC(hWnd);
    if (!hdc) return 0;
    TEXTMETRICW tm = {};
    HGDIOBJ oldFont = font ? SelectObject(hdc, font) : nullptr;
    GetTextMetricsW(hdc, &tm);
    if (oldFont) SelectObject(hdc, oldFont);
    ReleaseDC(hWnd, hdc);
    if (tm.tmHeight <= 0) return 0;
    int pad = (winHeight - (int)tm.tmHeight) / 2;
    if (pad < 1) return 0;
    if (pad > ScaleUi(6)) pad = ScaleUi(6);
    return pad;
}

void StripNativeBorder(HWND hWnd) {
    wchar_t cls[32] = L"";
    GetClassNameW(hWnd, cls, 32);
    // 列表保留 1px 原生边框，避免自绘框线压到表头
    if (_wcsicmp(cls, L"SysListView32") == 0) return;

    LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
    bool changed = false;
    if (style & WS_BORDER) {
        style &= ~(LONG_PTR)WS_BORDER;
        changed = true;
    }
    if (exStyle & WS_EX_CLIENTEDGE) {
        exStyle &= ~(LONG_PTR)WS_EX_CLIENTEDGE;
        changed = true;
    }
    if (changed) {
        SetWindowLongPtrW(hWnd, GWL_STYLE, style);
        SetWindowLongPtrW(hWnd, GWL_EXSTYLE, exStyle);
        SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
}

void DrawControlFrame(HWND hWnd, HDC hdc) {
    RECT rc;
    GetWindowRect(hWnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    // 单行输入框的文字区下移后，顶部非客户区需填充内容底色，避免残留旧像素
    if (IsStandaloneSingleLineEdit(hWnd)) {
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        POINT pt = { rcClient.left, rcClient.top };
        ClientToScreen(hWnd, &pt);
        int cy = pt.y - rc.top;
        if (cy > 0) {
            RECT band = { 0, 0, w, cy };
            const bool hc = Fluent::IsHighContrast();
            HBRUSH brush = CachedSolidBrush(
                hc ? GetSysColor(COLOR_WINDOW)
                   : (IsWindowEnabled(hWnd) ? P().surface : P().disabledBg));
            FillRect(hdc, &band, brush);
        }
    }

    // 禁用态仍保留可辨的框线：列表在禁用时会以灰底绘制，
    // 过浅的边框会让整个区域看起来没有边界。
    // 高对比度下使用系统窗口文字/边框色，避免运行中切换后仍是主题灰线
    COLORREF color;
    if (Fluent::IsHighContrast()) {
        color = GetSysColor(COLOR_3DSHADOW);
    } else {
        color = IsWindowEnabled(hWnd) ? P().border : P().disabledBorder;
    }
    wchar_t cls[32] = L"";
    GetClassNameW(hWnd, cls, 32);
    if (_wcsicmp(cls, L"SysListView32") == 0) {
        HPEN pen = CachedPen(1, color);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, 0, 0, w, h);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
    } else {
        RECT body = { 0, 0, w, h };
        StrokeRoundRect(hdc, body, ScaleUi(3), color);
    }
}

LRESULT CALLBACK FrameProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp,
                           UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    switch (msg) {
    case WM_NCPAINT: {
        LRESULT result = DefSubclassProc(hWnd, msg, wp, lp);
        HDC hdc = GetWindowDC(hWnd);
        if (hdc) { DrawControlFrame(hWnd, hdc); ReleaseDC(hWnd, hdc); }
        return result;
    }
    case WM_PAINT: {
        LRESULT result = DefSubclassProc(hWnd, msg, wp, lp);
        // 禁用态列表由系统以 3D 面灰绘制，与白色内容面冲突：
        // 这里把最后一项之后的空白区域补白（表头是子窗口，会被 WS_CLIPCHILDREN 裁掉）
        wchar_t cls[32] = L"";
        GetClassNameW(hWnd, cls, 32);
        if (_wcsicmp(cls, L"SysListView32") == 0 && !IsWindowEnabled(hWnd)) {
            RECT rcClient;
            GetClientRect(hWnd, &rcClient);
            HDC hdcClient = GetDC(hWnd);
            if (hdcClient) {
                // 禁用态统一使用禁用底色：系统还会为禁用列表额外绘制一圈
                // 3D 浅色边（表头下方与左侧各约 3px），必须一并覆盖
                HBRUSH bg = CachedSolidBrush(Fluent::IsHighContrast()
                                                 ? GetSysColor(COLOR_3DFACE)
                                                 : P().disabledBg);

                int hdrBottom = rcClient.top;
                bool haveHeader = false;
                HWND header = ListView_GetHeader(hWnd);
                if (header) {
                    RECT hdr;
                    if (GetWindowRect(header, &hdr)) {
                        MapWindowPoints(HWND_DESKTOP, hWnd, reinterpret_cast<POINT*>(&hdr), 2);
                        if (hdr.bottom > rcClient.top && hdr.bottom < rcClient.bottom) {
                            hdrBottom = hdr.bottom;
                            haveHeader = true;
                        }
                    }
                }
                const int edge = ScaleUi(3);
                if (haveHeader) {
                    RECT topBand = { rcClient.left, hdrBottom, rcClient.right, hdrBottom + edge };
                    FillRect(hdcClient, &topBand, bg);
                }
                RECT leftBand = { rcClient.left, hdrBottom, rcClient.left + edge, rcClient.bottom };
                FillRect(hdcClient, &leftBand, bg);

                // 最后一项之后的空白区域同样使用禁用底色
                int emptyTop = hdrBottom;
                const int count = ListView_GetItemCount(hWnd);
                if (count > 0) {
                    RECT lastItem;
                    if (ListView_GetItemRect(hWnd, count - 1, &lastItem, LVIR_BOUNDS))
                        emptyTop = lastItem.bottom;
                }
                // 最后一项被滚动到视口上方时矩形为负值，夹紧到表头底部
                if (emptyTop < hdrBottom) emptyTop = hdrBottom;
                if (emptyTop < rcClient.bottom) {
                    RECT empty = { rcClient.left, emptyTop, rcClient.right, rcClient.bottom };
                    FillRect(hdcClient, &empty, bg);
                }
                ReleaseDC(hWnd, hdcClient);
            }
        }
        HDC hdc = GetWindowDC(hWnd);
        if (hdc) { DrawControlFrame(hWnd, hdc); ReleaseDC(hWnd, hdc); }
        return result;
    }
    case WM_NCCALCSIZE: {
        if (wp) {
            LRESULT result = DefSubclassProc(hWnd, msg, wp, lp);
            NCCALCSIZE_PARAMS* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
            // 独立输入框四周各留 1px 非客户区承载描边：
            // 若框线画在客户区内，文本横向滚动或光标刷新会把它擦掉
            if (IsStandaloneSingleLineEdit(hWnd)) {
                RECT* rc = &params->rgrc[0];
                if (rc->right - rc->left > 3 && rc->bottom - rc->top > 3) {
                    int pad = GetEditTopPadding(hWnd, rc->bottom - rc->top);
                    rc->left += 1;
                    rc->right -= 1;
                    rc->bottom -= 1;
                    rc->top += 1 + pad;
                }
            }
            return result;
        }
        break;
    }
    case WM_SETFONT: {
        LRESULT result = DefSubclassProc(hWnd, msg, wp, lp);
        if (IsStandaloneSingleLineEdit(hWnd)) {
            SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        return result;
    }
    case WM_ENABLE:
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    default:
        break;
    }
    return DefSubclassProc(hWnd, msg, wp, lp);
}

// ============================================================
// 选项卡：浅灰标签条 + 白底选中项与页面连成一体
// ============================================================
// ============================================================
// 选项卡：Fluent 导航样式 —— 白色标签条、无边框、
// 选中项文字加粗并在底部显示强调色指示条
// ============================================================
struct TabState {
    int hover = -1;
};

void PaintTabControl(HWND hWnd, HDC target, const TabState* state) {
    RECT rcClient;
    GetClientRect(hWnd, &rcClient);
    const int width = rcClient.right - rcClient.left;
    const int height = rcClient.bottom - rcClient.top;
    if (width <= 0 || height <= 0) return;

    RECT rcPage = rcClient;
    TabCtrl_AdjustRect(hWnd, FALSE, &rcPage);
    const int stripBottom = (rcPage.top > 0 && rcPage.top < height) ? rcPage.top : height;

    HDC mem = CreateCompatibleDC(target);
    HBITMAP bitmap = mem ? CreateCompatibleBitmap(target, width, height) : nullptr;
    if (!mem || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (mem) DeleteDC(mem);
        return;
    }
    HGDIOBJ oldBitmap = SelectObject(mem, bitmap);

    // 标签条与页面同色（白），整块客户区一次性重绘；
    // 选项卡已压到页面最底层并带 WS_CLIPSIBLINGS，页面控件区域会被自动裁剪
    // 高对比度下改用系统面/系统文字色，避免运行中切换后出现黑底黑字
    const bool hc = Fluent::IsHighContrast();
    RECT whole = { 0, 0, width, height };
    FillRect(mem, &whole, hc ? GetSysColorBrush(COLOR_3DFACE) : Fluent::SurfaceBrush());
    // 标签条底部分隔线：仅在选中项之外显示，保持 Fluent 的轻分隔感
    if (stripBottom > 1 && stripBottom < height) {
        RECT line = { 0, stripBottom - 1, width, stripBottom };
        HBRUSH brush = CachedSolidBrush(hc ? GetSysColor(COLOR_3DSHADOW) : P().border);
        FillRect(mem, &line, brush);
    }

    const int selected = TabCtrl_GetCurSel(hWnd);
    const int count = TabCtrl_GetItemCount(hWnd);
    HFONT fontNormal = Fluent::UiFont();
    HFONT fontSelected = Fluent::UiFontSemibold();
    SetBkMode(mem, TRANSPARENT);

    for (int i = 0; i < count; ++i) {
        RECT item;
        if (!TabCtrl_GetItemRect(hWnd, i, &item)) continue;
        const bool isSelected = (i == selected);
        const bool isHover = (state && state->hover == i && !isSelected);

        if (isHover && !hc) {
            // 悬停：浅灰圆角底（高对比度下不画，避免浅底浅字）
            RECT hoverRect = item;
            InflateRect(&hoverRect, -ScaleUi(2), 0);
            Fluent::FillRoundRect(mem, hoverRect, ScaleUi(3), P().hover, P().hover);
        }
        if (isSelected && !hc) {
            // 选中：底部 2px 强调色指示条
            int barHeight = ScaleUi(2);
            if (barHeight < 2) barHeight = 2;
            RECT bar = { item.left + ScaleUi(12), item.bottom - barHeight,
                         item.right - ScaleUi(12), item.bottom };
            if (bar.right > bar.left) {
                HBRUSH brush = CachedSolidBrush(P().accent);
                FillRect(mem, &bar, brush);
            }
        }

        wchar_t text[128] = L"";
        TCITEMW itemInfo = {};
        itemInfo.mask = TCIF_TEXT;
        itemInfo.pszText = text;
        itemInfo.cchTextMax = 128;
        TabCtrl_GetItem(hWnd, i, &itemInfo);

        SetTextColor(mem, hc ? GetSysColor(COLOR_BTNTEXT)
                             : (isSelected ? P().text : P().textSecondary));
        HGDIOBJ oldFont = SelectObject(mem, isSelected ? fontSelected : fontNormal);
        RECT tr = item;
        if (tr.bottom > ScaleUi(4)) tr.bottom -= ScaleUi(4);
        DrawTextW(mem, text, -1, &tr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(mem, oldFont);
    }

    BitBlt(target, 0, 0, width, height, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(mem);
}

LRESULT CALLBACK TabProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp,
                         UINT_PTR /*id*/, DWORD_PTR ref) {
    auto* state = reinterpret_cast<TabState*>(ref);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        if (state) {
            TCHITTESTINFO hit = {};
            hit.pt.x = (int)(short)LOWORD(lp);
            hit.pt.y = (int)(short)HIWORD(lp);
            int index = TabCtrl_HitTest(hWnd, &hit);
            if (index != state->hover) {
                state->hover = index;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    }
    case WM_MOUSELEAVE:
        if (state && state->hover != -1) {
            state->hover = -1;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
        // 切换标签时系统会自行绘制选中态，禁止其在默认处理中绘制
        return DefaultProcNoPaint(hWnd, msg, wp, lp);
    }
    case WM_PRINTCLIENT: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        if (hdc) PaintTabControl(hWnd, hdc, state);
        return 0;
    }
    case WM_NCDESTROY:
        delete state;
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        PaintTabControl(hWnd, hdc, state);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_SETFONT:
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    default:
        break;
    }
    return DefSubclassProc(hWnd, msg, wp, lp);
}

// 子类化标识：同一控件只挂一次，避免重复装饰
constexpr UINT_PTR kButtonSubclassId = 0x464C0001;
constexpr UINT_PTR kGroupBoxSubclassId = 0x464C0002;
constexpr UINT_PTR kFrameSubclassId = 0x464C0003;
constexpr UINT_PTR kTabSubclassId = 0x464C0004;
constexpr UINT_PTR kComboSubclassId = 0x464C0006;
constexpr UINT_PTR kHeaderSubclassId = 0x464C0007;
constexpr UINT_PTR kSpinSubclassId = 0x464C0008;
constexpr UINT_PTR kListViewSubclassId = 0x464C0009;

void EnsureSubclass(HWND hWnd, SUBCLASSPROC proc, UINT_PTR id, DWORD_PTR ref) {
    DWORD_PTR existing = 0;
    if (!GetWindowSubclass(hWnd, proc, id, &existing))
        SetWindowSubclass(hWnd, proc, id, ref);
}

}  // namespace

namespace Fluent {

void SetThemeMode(ThemeMode mode) {
    g_themeMode = mode;
    // 每次都重新解析：System 模式下系统浅色/深色或主题色可能已变化
    g_themeResolved = false;
    ResolveTheme();
    RebuildThemeBrushes();
    // 主题变化后按颜色缓存的画笔/画刷全部失效，统一清空避免累积 GDI 句柄（R14-A8）
    ClearCachedGdiObjects();
}

ThemeMode GetThemeMode() { return g_themeMode; }

bool IsDarkTheme() {
    if (!g_themeResolved) ResolveTheme();
    return g_darkTheme;
}

COLORREF LinkColor() {
    if (IsHighContrast()) return GetSysColor(COLOR_HOTLIGHT);
    return IsDarkTheme() ? RGB(96, 205, 255) : RGB(0, 102, 204);
}

HFONT UiFont() { EnsureFonts(); return Theme().ui; }
HFONT UiFontSemibold() { EnsureFonts(); return Theme().semibold; }
HFONT SmallFont() { EnsureFonts(); return Theme().caption; }

// DPI 变化后按当前缩放重建字体。旧字体可能仍被现存控件引用，
// 不能立即 DeleteObject；改为替换缓存并让旧字体常驻到进程结束。
void RebuildFontsForDpi() {
    ThemeCache& t = Theme();
    if (!t.ui) return;
    t.ui = CreateFluentFont(14, FW_NORMAL);
    t.semibold = CreateFluentFont(14, FW_SEMIBOLD);
    t.caption = CreateFluentFont(12, FW_NORMAL);
}

// 按指定缩放创建主题字体：供子对话框跨 DPI 时使用，调用方负责删除返回值
HFONT CreateFontAtScale(int logicalHeight, int weight, float scale) {
    if (scale <= 0.0f) scale = GetUiScale();
    return CreateFluentFontAtScale(logicalHeight, weight, scale);
}

namespace {
bool QueryHighContrast() {
    HIGHCONTRASTW hc = {};
    hc.cbSize = sizeof(hc);
    if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)) return false;
    return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

bool& HighContrastCache() {
    static bool cached = QueryHighContrast();
    return cached;
}
}

bool IsHighContrast() {
    // 缓存结果：SurfaceBrush/TextColor/各控件 WM_PAINT 都会高频调用，
    // 每帧数十次 SystemParametersInfoW 会造成不必要的绘制开销。
    // 系统设置变化时由 Main 的 WM_SETTINGCHANGE 调用 RefreshHighContrastCache。
    return HighContrastCache();
}

void RefreshHighContrastCache() {
    HighContrastCache() = QueryHighContrast();
    // 高对比度会改变主题解析结果（ResolveTheme 在高对比度下强制浅色）。
    // 只更新 HC 缓存会让“关闭高对比度”后仍停留在强制浅色状态，
    // 必须重新解析并重建主题画刷，再清空按颜色缓存的 GDI 对象（R15-M10）
    g_themeResolved = false;
    ResolveTheme();
    RebuildThemeBrushes();
    ClearCachedGdiObjects();
}

COLORREF TextColor() {
    return IsHighContrast() ? GetSysColor(COLOR_WINDOWTEXT) : P().text;
}

COLORREF TextDisabledColor() {
    return IsHighContrast() ? GetSysColor(COLOR_GRAYTEXT) : P().textDisabled;
}

COLORREF SurfaceColor() {
    return IsHighContrast() ? GetSysColor(COLOR_WINDOW) : P().surface;
}

COLORREF DisabledBackgroundColor() {
    return IsHighContrast() ? GetSysColor(COLOR_3DFACE) : P().disabledBg;
}

COLORREF GridLineColor() {
    return IsHighContrast() ? GetSysColor(COLOR_3DLIGHT) : P().border;
}

HPEN GridPen() {
    // 复用主题层的 GDI 缓存，避免列表逐行 CreatePen/DeleteObject
    return CachedPen(1, GridLineColor());
}

HBRUSH SurfaceBrush() {
    if (IsHighContrast()) return GetSysColorBrush(COLOR_WINDOW);
    EnsureFonts();
    return Theme().surface;
}

HBRUSH PageBrush() {
    if (IsHighContrast()) return GetSysColorBrush(COLOR_3DFACE);
    EnsureFonts();
    return Theme().page;
}

void FillRoundRect(HDC hdc, const RECT& rc, int radius, COLORREF fill, COLORREF border) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    HBRUSH brush = CachedSolidBrush(fill);
    HPEN pen = CachedPen(1, border);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
}

void DecorateButton(HWND button, ButtonKind kind) {
    if (!button || IsHighContrast()) return;
    EnsureFonts();

    DWORD_PTR existing = 0;
    if (GetWindowSubclass(button, ButtonProc, kButtonSubclassId, &existing)) {
        auto* state = reinterpret_cast<ButtonState*>(existing);
        if (state && state->kind != kind) {
            state->kind = kind;
            InvalidateRect(button, nullptr, FALSE);
        }
        return;
    }

    // 未显式设置字体时才套用主题字体，保留调用方刻意的排版（如虚拟键盘键帽）
    if (!SendMessageW(button, WM_GETFONT, 0, 0))
        SendMessageW(button, WM_SETFONT, (WPARAM)UiFont(), FALSE);

    auto* state = new ButtonState();
    state->kind = kind;
    if (!SetWindowSubclass(button, ButtonProc, kButtonSubclassId, reinterpret_cast<DWORD_PTR>(state)))
        delete state;   // 挂接失败时由调用方回收，否则状态对象泄漏
    InvalidateRect(button, nullptr, TRUE);
}

void DecorateGroupBox(HWND groupBox) {
    if (!groupBox || IsHighContrast()) return;
    EnsureFonts();
    // 卡片背景会被页面上的兄弟控件覆盖，WS_CLIPSIBLINGS 让重绘裁剪到控件之外
    LONG_PTR style = GetWindowLongPtrW(groupBox, GWL_STYLE);
    if (!(style & WS_CLIPSIBLINGS))
        SetWindowLongPtrW(groupBox, GWL_STYLE, style | WS_CLIPSIBLINGS);
    // 分组框完全由本模块自绘；标题使用半粗体，与正文字段区分
    SendMessageW(groupBox, WM_SETFONT, (WPARAM)UiFontSemibold(), FALSE);
    EnsureSubclass(groupBox, GroupBoxProc, kGroupBoxSubclassId, 0);
    InvalidateRect(groupBox, nullptr, TRUE);
}

void DecorateToggle(HWND toggle) {
    if (!toggle || IsHighContrast()) return;
    EnsureFonts();
    // 复选框通常很宽，末端可能压着帮助问号等更高 z 序的小控件。
    // 没有 WS_CLIPSIBLINGS 时它重绘会覆盖上层兄弟窗口，造成问号被"擦掉"。
    LONG_PTR style = GetWindowLongPtrW(toggle, GWL_STYLE);
    if (!(style & WS_CLIPSIBLINGS)) {
        SetWindowLongPtrW(toggle, GWL_STYLE, style | WS_CLIPSIBLINGS);
    }
    // 未显式设置字体时套用主题正文，保留调用方刻意的排版
    if (!SendMessageW(toggle, WM_GETFONT, 0, 0))
        SendMessageW(toggle, WM_SETFONT, (WPARAM)UiFont(), FALSE);

    DWORD_PTR existing = 0;
    if (!GetWindowSubclass(toggle, ToggleProc, kToggleSubclassId, &existing)) {
        // 保留原生按钮类型：选中状态、点击/空格/方向键切换、单选组互斥
        // 以及无障碍状态都由系统维护，本模块只接管绘制
        auto* state = new ToggleState();
        state->originalType = GetWindowLongPtrW(toggle, GWL_STYLE) & BS_TYPEMASK;
        if (!SetWindowSubclass(toggle, ToggleProc, kToggleSubclassId,
                               reinterpret_cast<DWORD_PTR>(state)))
            delete state;
    }
    InvalidateRect(toggle, nullptr, TRUE);
}

void DecorateCombo(HWND combo) {
    if (!combo || IsHighContrast()) return;
    EnsureFonts();
    ApplyDarkControlTheme(combo);
    DWORD_PTR existing = 0;
    if (!GetWindowSubclass(combo, ComboProc, kComboSubclassId, &existing)) {
        auto* state = new ComboState();
        if (!SetWindowSubclass(combo, ComboProc, kComboSubclassId,
                               reinterpret_cast<DWORD_PTR>(state)))
            delete state;
    }
    InvalidateRect(combo, nullptr, TRUE);
}

void DecorateInput(HWND control) {
    if (!control || IsHighContrast()) return;
    EnsureFonts();
    ApplyDarkControlTheme(control);
    StripNativeBorder(control);
    EnsureSubclass(control, FrameProc, kFrameSubclassId, 0);
    // 单行输入框需要重算客户区，让文字与光标垂直居中
    wchar_t cls[32] = L"";
    GetClassNameW(control, cls, 32);
    if (_wcsicmp(cls, L"EDIT") == 0) {
        SetWindowPos(control, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    InvalidateRect(control, nullptr, TRUE);
}

// 列表视图只挂一个轻量子类：阻止 Enter 冒泡到对话框默认按钮。
// 否则用户在列表里按 Enter 会直接触发"确定"，把设置应用并关闭窗口
LRESULT CALLBACK ListViewProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp,
                              UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    if (msg == WM_GETDLGCODE) {
        const MSG* m = reinterpret_cast<const MSG*>(lp);
        if (m && m->message == WM_KEYDOWN && m->wParam == VK_RETURN)
            return DLGC_WANTALLKEYS;
    }
    return DefSubclassProc(hWnd, msg, wp, lp);
}

void DecorateListView(HWND listView) {
    if (!listView) return;
    // 拦截 Enter：列表内回车不应触发对话框默认按钮（高对比度下同样生效）
    EnsureSubclass(listView, ListViewProc, kListViewSubclassId, 0);
    if (IsHighContrast()) {
        // 运行中开启高对比度：清掉此前设置的自绘配色，恢复系统列表配色，
        // 否则可能残留深色文字 + 系统深色背景等原因导致不可读
        ListView_SetBkColor(listView, GetSysColor(COLOR_WINDOW));
        ListView_SetTextBkColor(listView, GetSysColor(COLOR_WINDOW));
        ListView_SetTextColor(listView, GetSysColor(COLOR_WINDOWTEXT));
        InvalidateRect(listView, nullptr, TRUE);
        return;
    }
    EnsureFonts();
    ApplyDarkControlTheme(listView);

    DWORD exStyle = (DWORD)SendMessageW(listView, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0);
    exStyle |= LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP;
    // 深色模式下关闭系统网格线（系统用浅灰绘制，在深色底上对比过强），
    // 改由父窗口在 NM_CUSTOMDRAW 中用接近背景的深灰重绘；浅色模式保持系统绘制
    if (IsDarkTheme()) exStyle &= ~LVS_EX_GRIDLINES;
    else exStyle |= LVS_EX_GRIDLINES;
    SendMessageW(listView, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, (LPARAM)exStyle);
    SendMessageW(listView, WM_SETFONT, (WPARAM)UiFont(), FALSE);
    HWND header = (HWND)SendMessageW(listView, LVM_GETHEADER, 0, 0);
    if (header) {
        // Fluent 表头：白底、12px 灰色文字、底部轻分隔线
        SendMessageW(header, WM_SETFONT, (WPARAM)SmallFont(), TRUE);
        DWORD_PTR existingHeader = 0;
        if (!GetWindowSubclass(header, HeaderProc, kHeaderSubclassId, &existingHeader))
            SetWindowSubclass(header, HeaderProc, kHeaderSubclassId, 0);
        InvalidateRect(header, nullptr, TRUE);
    }
    // 禁用态使用灰底灰字，让"未启用"在界面上有明确反馈
    const bool listEnabled = IsWindowEnabled(listView) != FALSE;
    ListView_SetBkColor(listView, listEnabled ? P().surface : P().disabledBg);
    ListView_SetTextBkColor(listView, listEnabled ? P().surface : P().disabledBg);
    ListView_SetTextColor(listView, listEnabled ? P().text : P().textDisabled);


    // 保留 1px 非客户区边框 + WS_CLIPCHILDREN：自绘描边只落在边框区，
    // 列表滚动时不会盖住表头，避免表头框线闪烁
    LONG_PTR style = GetWindowLongPtrW(listView, GWL_STYLE);
    LONG_PTR winExStyle = GetWindowLongPtrW(listView, GWL_EXSTYLE);
    bool changed = false;
    if (!(style & WS_BORDER)) { style |= WS_BORDER; changed = true; }
    if (!(style & WS_CLIPCHILDREN)) { style |= WS_CLIPCHILDREN; changed = true; }
    if (winExStyle & WS_EX_CLIENTEDGE) { winExStyle &= ~(LONG_PTR)WS_EX_CLIENTEDGE; changed = true; }
    if (changed) {
        SetWindowLongPtrW(listView, GWL_STYLE, style);
        SetWindowLongPtrW(listView, GWL_EXSTYLE, winExStyle);
        SetWindowPos(listView, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    // 主题切换后重新绘制（网格线绘制方式随深浅色变化）
    InvalidateRect(listView, nullptr, TRUE);

    EnsureSubclass(listView, FrameProc, kFrameSubclassId, 0);
    InvalidateRect(listView, nullptr, TRUE);
}

void DecorateTab(HWND tab) {
    if (!tab || IsHighContrast()) return;
    EnsureFonts();
    // 页面区域与标签条都由本模块自绘；WS_CLIPSIBLINGS 让页面上的
    // 兄弟控件（分组框、按钮等）不被标签条背景覆盖
    LONG_PTR style = GetWindowLongPtrW(tab, GWL_STYLE);
    if (!(style & WS_CLIPSIBLINGS)) {
        SetWindowLongPtrW(tab, GWL_STYLE, style | WS_CLIPSIBLINGS);
        SetWindowPos(tab, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    SendMessageW(tab, WM_SETFONT, (WPARAM)UiFont(), FALSE);
    DWORD_PTR existing = 0;
    if (!GetWindowSubclass(tab, TabProc, kTabSubclassId, &existing)) {
        auto* state = new TabState();
        if (!SetWindowSubclass(tab, TabProc, kTabSubclassId, reinterpret_cast<DWORD_PTR>(state)))
            delete state;
    }
    InvalidateRect(tab, nullptr, TRUE);
}

void DecorateDialog(HWND root) {
    if (!root) return;
    if (IsHighContrast()) {
        // 高对比度下不接管新控件绘制，但此前已自绘的列表仍带着旧配色，
        // 需要恢复系统配色（同时保证 Enter 拦截子类已安装）
        EnumChildWindows(root, [](HWND child, LPARAM) -> BOOL {
            wchar_t cls[32] = L"";
            GetClassNameW(child, cls, 32);
            if (_wcsicmp(cls, L"SysListView32") == 0) DecorateListView(child);
            return TRUE;
        }, 0);
        return;
    }
    EnsureFonts();
    EnumChildWindows(root, [](HWND child, LPARAM) -> BOOL {
        wchar_t cls[32] = L"";
        GetClassNameW(child, cls, 32);

        if (_wcsicmp(cls, L"BUTTON") == 0) {
            LONG_PTR type = GetWindowLongPtrW(child, GWL_STYLE) & BS_TYPEMASK;
            if (type == BS_GROUPBOX) {
                DecorateGroupBox(child);
            } else if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) {
                // 已显式装饰过的按钮（如强调色主按钮）保持原有类型；
                // 未装饰的对话框主操作（IDOK）自动使用强调色
                DWORD_PTR existing = 0;
                if (!GetWindowSubclass(child, ButtonProc, kButtonSubclassId, &existing)) {
                    const bool primary = (GetDlgCtrlID(child) == IDOK);
                    DecorateButton(child, primary ? ButtonKind::Primary : ButtonKind::Secondary);
                }
            } else if (type == BS_CHECKBOX || type == BS_AUTOCHECKBOX ||
                       type == BS_3STATE || type == BS_AUTO3STATE ||
                       type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON) {
                DecorateToggle(child);
            }
        } else if (_wcsicmp(cls, L"EDIT") == 0) {
            wchar_t parentCls[32] = L"";
            HWND parent = GetParent(child);
            if (parent) GetClassNameW(parent, parentCls, 32);
            // 可编辑下拉框内嵌的编辑区只去掉系统边框：外框已由组合框绘制
            if (_wcsicmp(parentCls, L"COMBOBOX") == 0) {
                StripNativeBorder(child);
            } else {
                DecorateInput(child);
            }
        } else if (_wcsicmp(cls, L"COMBOBOX") == 0) {
            DecorateInput(child);
            DecorateCombo(child);
        } else if (_wcsicmp(cls, L"LISTBOX") == 0) {
            DecorateInput(child);
        } else if (_wcsicmp(cls, L"SysListView32") == 0) {
            DecorateListView(child);
        } else if (_wcsicmp(cls, UPDOWN_CLASSW) == 0) {
            DWORD_PTR existingSpin = 0;
            if (!GetWindowSubclass(child, SpinProc, kSpinSubclassId, &existingSpin)) {
                auto* state = new SpinState();
                if (!SetWindowSubclass(child, SpinProc, kSpinSubclassId,
                                       reinterpret_cast<DWORD_PTR>(state)))
                    delete state;
            }
            InvalidateRect(child, nullptr, TRUE);
        } else if (_wcsicmp(cls, WC_TABCONTROLW) == 0) {
            DecorateTab(child);
        }
        return TRUE;
    }, 0);
}

// ============================================================
// 主题化系统消息框
// 系统 MessageBox 没有公开的深色 API；这里用调用线程的 CBT 钩子，
// 在 #32770 激活时安装子类，接管背景与文字颜色。高对比度/浅色模式
// 仍使用系统默认绘制，避免破坏可访问性。
// ============================================================
namespace {

struct MessageBoxThemeCtx {
    HBRUSH bg = nullptr;
    COLORREF bgColor = RGB(43, 43, 43);
    COLORREF text = RGB(255, 255, 255);
};

constexpr UINT_PTR kMessageBoxSubclassId = 0x464C0006;
// 用栈保存上下文：消息框可能嵌套（定时器/托盘回调再弹提示），
// 单个指针会被内层覆盖，导致外层后续激活失去主题（R13-A10）
thread_local std::vector<MessageBoxThemeCtx*> g_msgBoxCtxStack;

MessageBoxThemeCtx* CurrentMsgBoxCtx() {
    return g_msgBoxCtxStack.empty() ? nullptr : g_msgBoxCtxStack.back();
}

LRESULT CALLBACK MessageBoxSubclassProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp,
                                        UINT_PTR, DWORD_PTR ref) {
    auto* ctx = reinterpret_cast<MessageBoxThemeCtx*>(ref);
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT rc = {};
        GetClientRect(hWnd, &rc);
        FillRect(reinterpret_cast<HDC>(wp), &rc, ctx->bg);
        return 1;
    }
    case WM_CTLCOLORDLG:
        return reinterpret_cast<LRESULT>(ctx->bg);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        SetTextColor(hdc, ctx->text);
        SetBkColor(hdc, ctx->bgColor);
        return reinterpret_cast<LRESULT>(ctx->bg);
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(hWnd, MessageBoxSubclassProc, kMessageBoxSubclassId);
        break;
    }
    return DefSubclassProc(hWnd, msg, wp, lp);
}

LRESULT CALLBACK MessageBoxCbtProc(int code, WPARAM wp, LPARAM lp) {
    MessageBoxThemeCtx* ctx = CurrentMsgBoxCtx();
    if (code == HCBT_ACTIVATE && ctx) {
        HWND hwnd = reinterpret_cast<HWND>(wp);
        wchar_t cls[16] = L"";
        GetClassNameW(hwnd, cls, 16);
        if (_wcsicmp(cls, L"#32770") == 0) {
            SetWindowSubclass(hwnd, MessageBoxSubclassProc, kMessageBoxSubclassId,
                              reinterpret_cast<DWORD_PTR>(ctx));
            ApplyDarkControlTheme(hwnd);
            Fluent::ApplyWindowChrome(hwnd);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

}  // namespace

int ShowMessageBox(HWND owner, const wchar_t* text, const wchar_t* caption, UINT type) {
    // 统一按当前语言翻译消息与标题；拼接型消息由 Lang 内部做片段替换
    const std::wstring trText = TranslateMessageText(text ? text : L"");
    const std::wstring trCaption = TranslateMessageText(caption ? caption : L"");
    return ShowMessageBoxRaw(owner, trText.c_str(), trCaption.c_str(), type);
}

int ShowMessageBoxRaw(HWND owner, const wchar_t* text, const wchar_t* caption, UINT type) {
    if (!text) text = L"";
    if (!caption) caption = L"";
    if (!IsDarkTheme() || IsHighContrast()) {
        return MessageBoxW(owner, text, caption, type);
    }
    MessageBoxThemeCtx ctx;
    // 消息框跨模态循环持有画刷，不能复用会被主题切换删除的缓存画刷（R14-M5）；
    // 为本消息框单独创建，返回后立即释放
    ctx.bg = CreateSolidBrush(SurfaceColor());
    ctx.bgColor = SurfaceColor();
    ctx.text = TextColor();
    HHOOK hook = SetWindowsHookExW(WH_CBT, MessageBoxCbtProc, nullptr, GetCurrentThreadId());
    g_msgBoxCtxStack.push_back(&ctx);
    const int result = MessageBoxW(owner, text, caption, type);
    // 弹出本层上下文：嵌套消息框返回后，外层仍能取到自己的上下文（R13-A10）
    if (!g_msgBoxCtxStack.empty() && g_msgBoxCtxStack.back() == &ctx)
        g_msgBoxCtxStack.pop_back();
    if (hook) UnhookWindowsHookEx(hook);
    if (ctx.bg) DeleteObject(ctx.bg);
    return result;
}

void ApplyWindowChrome(HWND topLevel) {
    if (!topLevel) return;
    // Win11：DWMWA_WINDOW_CORNER_PREFERENCE = 33，值为 DWMWCP_ROUND = 2。
    // Win7/10 调用会返回失败，属预期，忽略即可
    typedef HRESULT(WINAPI * PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
    static PFN_DwmSetWindowAttribute setAttribute = []() -> PFN_DwmSetWindowAttribute {
        HMODULE dwmapi = DpiUtils::LoadSystemModule(L"dwmapi.dll");
        if (!dwmapi) return nullptr;
        return reinterpret_cast<PFN_DwmSetWindowAttribute>(
            GetProcAddress(dwmapi, "DwmSetWindowAttribute"));
    }();
    if (!setAttribute) return;
    // 深色标题栏：DWMWA_USE_IMMERSIVE_DARK_MODE 在 Win10 1809 为 19、
    // Win10 2004+/Win11 为 20；两者都尝试，失败即忽略（Win7/8 无此属性）
    BOOL darkTitle = IsDarkTheme() ? TRUE : FALSE;
    if (FAILED(setAttribute(topLevel, 20, &darkTitle, sizeof(darkTitle))))
        setAttribute(topLevel, 19, &darkTitle, sizeof(darkTitle));

    const DWORD cornerPreferenceAttribute = 33;
    const int round = 2;
    setAttribute(topLevel, cornerPreferenceAttribute, &round, sizeof(round));
}

}  // namespace Fluent
