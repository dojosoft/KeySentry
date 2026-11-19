#pragma once
#include "SettingsDialog.h"
#include "Lang.h"
#include "FluentTheme.h"
#include "Utils.h"
#include "Resource.h"
#include <CommCtrl.h>
#include <string>

// ============================================================
// 设置页共用的小工具：页面构建与主对话框共用同一份实现
// ============================================================
inline HFONT GetDlgFont() { return Fluent::UiFont(); }
inline HFONT GetSmallFont() { return Fluent::SmallFont(); }
inline void SetControlFont(HWND hwnd, HFONT font) {
    if (hwnd) SendMessageW(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
}
inline std::wstring HotkeyButtonText(UINT mod, UINT vk) {
    if (mod == 0 && vk == 0) return Tr(L"未设置");
    return SettingsDialog::FormatHotKey(mod, vk);
}

inline bool OverlayAppearanceChecked(HWND hwnd) {
    auto checked = [&](int id) {
        HWND h = GetDlgItem(hwnd, id);
        return h && SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
    };
    return checked(IDC_CHK_OVERLAY) || checked(IDC_CHK_MUTE_OVERLAY);
}

inline void UpdateOverlayAppearanceState(HWND hwnd, bool enabled) {
    static const int ids[] = {
        IDC_CHK_PRIMARY_ONLY,
        IDC_CBO_OVERLAY_THEME,
        IDC_CBO_OVERLAY_POS,
        IDC_CBO_OVERLAY_CORNER,
        IDC_CBO_OVERLAY_OPACITY,
        IDC_CBO_OVERLAY_DELAY,
        IDC_BTN_OVERLAY_DEFAULT
    };
    for (int id : ids) {
        HWND h = GetDlgItem(hwnd, id);
        if (h) EnableWindow(h, enabled);
    }
}

// ============================================================
// 设置页构建上下文
// 页面构建器只依赖该结构，不再直接访问 SettingsDialog 的私有状态；
// 新增页面只需增加一个 BuildXxxPage 并在 BuildSettingsPage 注册。
// ============================================================
struct SettingsPageContext {
    HWND hwnd = nullptr;
    AppConfig& config;
    HFONT font = nullptr;
    int PAGE_X = 0;
    int PAGE_Y = 0;
    int PAGE_W = 0;
    int PAGE_H = 0;
    int y = 0;
    int lineH = 22;
    int btnH = 32;
    int smallBtnW = 80;

    SettingsPageContext(HWND wnd, AppConfig& cfg, HFONT fnt,
                        int px, int py, int pw, int ph,
                        int startY, int lh, int bh, int sbw)
        : hwnd(wnd), config(cfg), font(fnt),
          PAGE_X(px), PAGE_Y(py), PAGE_W(pw), PAGE_H(ph),
          y(startY), lineH(lh), btnH(bh), smallBtnW(sbw) {}

    HWND makeChk(int id, const wchar_t* text, int px, int py, int w,
                 bool checked, int h = 0) {
        HWND h2 = CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS | BS_AUTOCHECKBOX | BS_VCENTER,
                                   px, py, w, h > 0 ? h : lineH,
                                   hwnd, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(h2, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
        SetControlFont(h2, font);
        return h2;
    }

    HWND makeLabel(const wchar_t* text, int px, int py, int w, int h = 0) {
        HWND h2 = CreateWindowExW(0, L"STATIC", text,
                                   WS_CHILD | SS_LEFT | SS_CENTERIMAGE,
                                   px, py, w, h > 0 ? h : lineH,
                                   hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(h2, font);
        return h2;
    }

    HWND makeEdit(int id, int px, int py, int w) {
        HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                                   px, py, w, lineH,
                                   hwnd, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(h, font);
        Fluent::DecorateInput(h);
        return h;
    }

    HWND makeButton(int id, const wchar_t* text, int px, int py, int w, int h2) {
        HWND bh = CreateWindowExW(0, L"BUTTON", text,
                                    WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                    px, py, w, h2,
                                    hwnd, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(bh, font);
        Fluent::DecorateButton(bh, Fluent::ButtonKind::Secondary);
        return bh;
    }

    HWND makeListBox(int id, int px, int py, int w, int h2) {
        HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                   WS_CHILD | WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                                   px, py, w, h2,
                                   hwnd, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(h, font);
        Fluent::DecorateInput(h);
        return h;
    }

    HWND makeListView(int id, int px, int py, int w, int h2) {
        HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                   WS_CHILD | WS_TABSTOP | WS_BORDER | LVS_REPORT |
                                   LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                   px, py, w, h2,
                                   hwnd, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(h, font);
        ListView_SetExtendedListViewStyle(h, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        Fluent::DecorateListView(h);
        return h;
    }

    static void addLvColumn(HWND lv, int idx, const wchar_t* text, int width) {
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.pszText = const_cast<LPWSTR>(text);
        col.cx = width;
        col.fmt = LVCFMT_LEFT;
        ListView_InsertColumn(lv, idx, &col);
    }

    HWND makeGroupBox(const wchar_t* text, int px, int py, int w, int h2) {
        HWND h = CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | BS_GROUPBOX,
                                   px, py, w, h2,
                                   hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(h, font);
        Fluent::DecorateGroupBox(h);
        return h;
    }

    void BuildGeneralPage();
    void BuildKeysPage();
    void BuildRemapPage();
    void BuildHotkeyPage();
    void BuildBossPage();
    void BuildWindPage();
};

// 按索引构建页面（索引来自选项卡，越界时不做任何事）
void BuildSettingsPage(int index, SettingsPageContext& ctx);

// 页面构建完成后调整 z 序：分组框压到页面控件底部、选项卡在最底层，
// 避免分组框的自绘背景覆盖其内部复选框等控件
void LowerGroupBoxes(HWND page);

// 页面构建完成后，把所有帮助问号标记提到控件最上层，
// 避免被同页的宽控件（复选框等）覆盖而无法接收鼠标悬停
void RaiseHelpMarkers(HWND page);
