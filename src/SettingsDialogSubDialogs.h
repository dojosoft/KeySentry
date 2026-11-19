#pragma once
#include <Windows.h>
#include <string>
#include "FluentTheme.h"
#include "Utils.h"

// ============================================================
// 子对话框跨 DPI 处理
// 子对话框按打开时的线程缩放布置，跨显示器后会收到 WM_DPICHANGED。
// 这里按新旧缩放比调整现有子控件、按新 DPI 重建字体并采用系统建议矩形。
// 故意不修改线程级缩放覆盖：否则会影响下层设置主窗口的后续绘制与布局。
// ============================================================
struct SubDialogDpiState {
    float scale = 0.0f;   // 上一次布局使用的缩放
    HFONT font = nullptr; // 本次为控件创建的字体，销毁窗口时释放
};

inline const wchar_t* const SUB_DIALOG_DPI_PROP = L"KeySentrySubDialogDpiState";

// 在 WM_NCDESTROY 中调用：释放 DPI 处理创建的字体与状态
inline void CleanupSubDialogDpiState(HWND hwnd) {
    auto* state = reinterpret_cast<SubDialogDpiState*>(
        RemovePropW(hwnd, SUB_DIALOG_DPI_PROP));
    if (!state) return;
    if (state->font) DeleteObject(state->font);
    delete state;
}

// 模态循环统一判断：DPI 消息是否应直接交给本窗口。
// 线程中其他顶层窗口的 DPI 消息必须正常派发，不能被当前模态循环吞掉（R15-A03）
inline bool IsDpiMessageForWindow(const MSG& msg, HWND hwnd) {
    return msg.message == WM_DPICHANGED && msg.hwnd == hwnd;
}

// 在 WM_DPICHANGED 中调用。fontHeight/fontWeight 与对话框创建时使用的字体一致
inline void HandleSubDialogDpiChanged(HWND hwnd, WPARAM wp, LPARAM lp,
                                      int fontHeight = 14, int fontWeight = FW_NORMAL) {
    if (!hwnd) return;
    const UINT dpi = HIWORD(wp);
    RECT work = {};
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (monitor && GetMonitorInfoW(monitor, &mi)) work = mi.rcWork;
    const float newScale = DpiUtils::ScaleForDpi(dpi, &work);

    auto* state = reinterpret_cast<SubDialogDpiState*>(
        GetPropW(hwnd, SUB_DIALOG_DPI_PROP));
    if (!state) {
        state = new SubDialogDpiState();
        // 创建时使用的是线程当前缩放（主设置窗口的 ScopedUiScale）；跨 DPI 前
        // 该值尚未被本函数修改，因此可作为旧缩放基准
        state->scale = GetUiScale();
        SetPropW(hwnd, SUB_DIALOG_DPI_PROP, state);
    }
    const float oldScale = state->scale > 0.0f ? state->scale : newScale;

    if (oldScale > 0.0f && newScale > 0.0f && oldScale != newScale) {
        ScaleChildWindows(hwnd, newScale / oldScale);
    }

    HFONT font = Fluent::CreateFontAtScale(fontHeight, fontWeight, newScale);
    if (font) {
        EnumChildWindows(hwnd, [](HWND child, LPARAM param) -> BOOL {
            SendMessageW(child, WM_SETFONT, (WPARAM)param, TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(font));
        if (state->font) DeleteObject(state->font);
        state->font = font;
    }
    state->scale = newScale;

    const RECT* suggested = reinterpret_cast<const RECT*>(lp);
    if (suggested) {
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    InvalidateRect(hwnd, nullptr, TRUE);
}

// ============================================================
// 子对话框共用的小工具与跨文件声明
// ============================================================

// 用主题内容底色擦除窗口背景：窗口类不固定系统白色画刷，
// 否则深色模式下会出现成片白色底
inline void EraseWindowBackground(HWND hwnd, WPARAM wp) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(reinterpret_cast<HDC>(wp), &rc, Fluent::SurfaceBrush());
}

// 读取控件文本（不依赖固定长度缓冲，避免长路径/长命令行被截断）
inline std::wstring ReadWindowText(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return L"";
    // 必须按 len+1 分配：GetWindowTextW 的 nMaxCount 含结尾 NUL，
    // 写入 s[len] 在标准层面是未定义行为（R8-M11）
    std::wstring s((size_t)len + 1, L'\0');
    int copied = GetWindowTextW(hwnd, &s[0], len + 1);
    if (copied <= 0) return L"";
    s.resize((size_t)copied);
    return s;
}

// 是否存在只持有独立捕获钩子的按键捕获对话框
bool IsCaptureKeyDialogActive();

// 显示按键捕获对话框（按键屏蔽专用）；返回捕获到的虚拟键码，0 表示取消
int ShowCaptureKeyDialog(HWND parent);

// 显示按键捕获对话框（通用版）；title 为标题，line1/line2 为提示文字，
// outMods/allowMods 控制并返回组合键；返回捕获到的虚拟键码，0 表示取消
int ShowCaptureKeyDialogEx(HWND parent, const wchar_t* title, const wchar_t* line1,
                           const wchar_t* line2, UINT* outMods = nullptr,
                           bool allowMods = false);

// 添加绑定对话框窗口类名：窗口隐藏与窗口置顶共用同一窗口过程，
// 按类名区分各自的窗口类注册状态
inline const wchar_t* const ADD_BINDING_CLASS = L"KeySentryAddBindingDlg";
