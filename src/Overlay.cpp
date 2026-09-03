#include "Overlay.h"
#include "Resource.h"
#include "Utils.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>

const wchar_t* OverlayWindow::CLASS_NAME = L"KeySentryOverlayWnd";

OverlayWindow::OverlayWindow() = default;

OverlayWindow::~OverlayWindow() {
    Destroy();
}

void OverlayWindow::SetPrimaryOnly(bool primaryOnly) {
    m_primaryOnly = primaryOnly;
}

// 创建单个浮层窗口：分层、置顶、工具窗口、透明鼠标穿透
HWND OverlayWindow::CreateOverlayWnd() {
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        CLASS_NAME, L"",
        WS_POPUP,
        0, 0, OVERLAY_SIZE, OVERLAY_SIZE,
        nullptr, nullptr, m_hInst, this
    );
    if (hwnd) {
        SetLayeredWindowAttributes(hwnd, 0, OVERLAY_ALPHA, LWA_ALPHA);
    }
    return hwnd;
}

// ============================================================
// GetTargetMonitors - 获取目标显示器列表
// 如果 primaryOnly 为 true，仅返回主显示器
// 否则返回所有显示器
// ============================================================
// 枚举回调：32 位下 GCC 的 lambda 无法隐式转换为 stdcall 的 MONITORENUMPROC，
// 故用显式 CALLBACK 约定的命名函数
static BOOL CALLBACK CollectPrimaryMonitorProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
    auto* pMons = reinterpret_cast<std::vector<HMONITOR>*>(lParam);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);
    if (mi.dwFlags & MONITORINFOF_PRIMARY) {
        pMons->push_back(hMon);
        return FALSE;
    }
    return TRUE;
}

static BOOL CALLBACK CollectAllMonitorsProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
    reinterpret_cast<std::vector<HMONITOR>*>(lParam)->push_back(hMon);
    return TRUE;
}

std::vector<HMONITOR> OverlayWindow::GetTargetMonitors() {
    std::vector<HMONITOR> monitors;

    if (m_primaryOnly) {
        // 获取主显示器
        POINT pt = { 0, 0 };
        HMONITOR hPrimary = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(hPrimary, &mi);
        if (mi.dwFlags & MONITORINFOF_PRIMARY) {
            monitors.push_back(hPrimary);
        } else {
            // 回退：枚举查找主显示器
            EnumDisplayMonitors(nullptr, nullptr, CollectPrimaryMonitorProc,
                                reinterpret_cast<LPARAM>(&monitors));
            if (monitors.empty()) {
                monitors.push_back(hPrimary);
            }
        }
    } else {
        // 枚举所有显示器
        EnumDisplayMonitors(nullptr, nullptr, CollectAllMonitorsProc,
                            reinterpret_cast<LPARAM>(&monitors));
    }

    return monitors;
}

// 确保浮层窗口数量足够，不足则创建新窗口
void OverlayWindow::EnsureWindowCount(int count) {
    while ((int)m_hwnds.size() < count) {
        HWND hwnd = CreateOverlayWnd();
        if (hwnd) {
            m_hwnds.push_back(hwnd);
        } else {
            break;
        }
    }
}

// ============================================================
// OverlayWindow::Create - 初始化浮层窗口
// 注册窗口类、创建初始窗口、创建 GDI 资源
// ============================================================
bool OverlayWindow::Create(HINSTANCE hInst) {
    m_hInst = hInst;

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = CLASS_NAME;
        wc.hbrBackground = nullptr;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&wc)) {
            if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        }
        registered = true;
    }

    HWND hwnd = CreateOverlayWnd();
    if (!hwnd) return false;
    m_hwnds.push_back(hwnd);

    // 创建字体和画刷资源
    m_fontLarge = CreateUiFont(30, FW_BOLD);
    m_fontMedium = CreateUiFont(14, FW_SEMIBOLD);
    m_fontSmall = CreateUiFont(12, FW_NORMAL);
    m_bgBrush = CreateSolidBrush(RGB(30, 30, 42));       // 深色背景
    m_barBgBrush = CreateSolidBrush(RGB(55, 55, 75));     // 音量条背景
    m_barFillBrush = CreateSolidBrush(RGB(100, 200, 130));// 绿色填充
    m_barMuteBrush = CreateSolidBrush(RGB(200, 80, 80));  // 红色静音

    return true;
}

// 销毁所有浮层窗口和 GDI 资源
void OverlayWindow::Destroy() {
    if (m_fontLarge) { DeleteObject(m_fontLarge); m_fontLarge = nullptr; }
    if (m_fontMedium) { DeleteObject(m_fontMedium); m_fontMedium = nullptr; }
    if (m_fontSmall) { DeleteObject(m_fontSmall); m_fontSmall = nullptr; }
    if (m_bgBrush) { DeleteObject(m_bgBrush); m_bgBrush = nullptr; }
    if (m_barBgBrush) { DeleteObject(m_barBgBrush); m_barBgBrush = nullptr; }
    if (m_barFillBrush) { DeleteObject(m_barFillBrush); m_barFillBrush = nullptr; }
    if (m_barMuteBrush) { DeleteObject(m_barMuteBrush); m_barMuteBrush = nullptr; }
    for (HWND hwnd : m_hwnds) {
        if (hwnd) DestroyWindow(hwnd);
    }
    m_hwnds.clear();
}

// ============================================================
// OverlayWindow::GetAudioState - 获取当前系统音频状态
// 通过 Windows Core Audio API 查询默认音频输出设备
// ============================================================
void OverlayWindow::GetAudioState(bool& isMuted, int& volumeLevel) {
    isMuted = false;
    volumeLevel = 0;

    // COM 已在主线程 wWinMain 中初始化，无需重复初始化

    IMMDeviceEnumerator* pEnumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                           __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&pEnumerator));
    if (FAILED(hr)) return;

    IMMDevice* pDevice = nullptr;
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    pEnumerator->Release();
    if (FAILED(hr)) return;

    IAudioEndpointVolume* pVolume = nullptr;
    hr = pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                            nullptr, reinterpret_cast<void**>(&pVolume));
    pDevice->Release();
    if (FAILED(hr)) return;

    BOOL muted = FALSE;
    float vol = 0.0f;
    if (SUCCEEDED(pVolume->GetMute(&muted)))
        isMuted = muted != FALSE;
    if (SUCCEEDED(pVolume->GetMasterVolumeLevelScalar(&vol)))
        volumeLevel = (int)(vol * 100.0f + 0.5f);
    pVolume->Release();

    if (volumeLevel > 100) volumeLevel = 100;
}

// ============================================================
// OverlayWindow::Show - 显示锁键状态浮层
// 在每个目标显示器底部居中显示，1秒后自动隐藏
// ============================================================
void OverlayWindow::Show(int vkCode, bool state, bool locked) {
    if (m_hwnds.empty()) return;

    m_vkCode = vkCode;
    m_state = state;
    m_locked = locked;
    m_showMuteOverlay = false;

    auto monitors = GetTargetMonitors();
    EnsureWindowCount((int)monitors.size());

    // 在每个显示器上定位并显示浮层
    for (size_t i = 0; i < monitors.size() && i < m_hwnds.size(); i++) {
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(monitors[i], &mi);
        int screenW = mi.rcWork.right - mi.rcWork.left;
        int x = (screenW - OVERLAY_SIZE) / 2 + mi.rcWork.left;
        int y = mi.rcWork.bottom - OVERLAY_KEY_HEIGHT - OVERLAY_BOTTOM_MARGIN;

        SetWindowPos(m_hwnds[i], HWND_TOPMOST, x, y, OVERLAY_SIZE, OVERLAY_KEY_HEIGHT,
                     SWP_SHOWWINDOW | SWP_NOACTIVATE);
        InvalidateRect(m_hwnds[i], nullptr, TRUE);
        KillTimer(m_hwnds[i], TIMER_OVERLAY);
        SetTimer(m_hwnds[i], TIMER_OVERLAY, OVERLAY_SHOW_DURATION_MS, nullptr);
    }

    // 隐藏多余的浮层窗口
    for (size_t i = monitors.size(); i < m_hwnds.size(); i++) {
        ShowWindow(m_hwnds[i], SW_HIDE);
        KillTimer(m_hwnds[i], TIMER_OVERLAY);
    }
}

// ============================================================
// OverlayWindow::ShowMute - 显示静音状态浮层
// 显示喇叭图标、静音/取消静音标签、音量百分比和音量条
// ============================================================
void OverlayWindow::ShowMute(bool muted) {
    if (m_hwnds.empty()) return;

    m_showMuteOverlay = true;

    bool isMuted;
    GetAudioState(isMuted, m_audioVolume);
    m_audioMuted = muted;

    auto monitors = GetTargetMonitors();
    EnsureWindowCount((int)monitors.size());

    for (size_t i = 0; i < monitors.size() && i < m_hwnds.size(); i++) {
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(monitors[i], &mi);
        int screenW = mi.rcWork.right - mi.rcWork.left;
        int x = (screenW - OVERLAY_SIZE) / 2 + mi.rcWork.left;
        int y = mi.rcWork.bottom - OVERLAY_SIZE - OVERLAY_BOTTOM_MARGIN;

        SetWindowPos(m_hwnds[i], HWND_TOPMOST, x, y, OVERLAY_SIZE, OVERLAY_SIZE,
                     SWP_SHOWWINDOW | SWP_NOACTIVATE);
        InvalidateRect(m_hwnds[i], nullptr, TRUE);
        KillTimer(m_hwnds[i], TIMER_OVERLAY);
        SetTimer(m_hwnds[i], TIMER_OVERLAY, OVERLAY_SHOW_DURATION_MS, nullptr);
    }

    for (size_t i = monitors.size(); i < m_hwnds.size(); i++) {
        ShowWindow(m_hwnds[i], SW_HIDE);
        KillTimer(m_hwnds[i], TIMER_OVERLAY);
    }
}

// ============================================================
// DrawSpeakerIcon - 绘制喇叭图标
//   - 非静音时绘制声波弧线
//   - 静音时绘制红色 X 号
// ============================================================
static void DrawSpeakerIcon(HDC hdc, int cx, int cy, int size, bool muted) {
    int s = size;
    int halfS = s / 2;

    // 喇叭主体轮廓点
    POINT pts[4] = {
        { cx - halfS, cy - halfS / 3 },
        { cx - halfS / 3, cy - halfS / 3 },
        { cx + halfS / 4, cy - halfS },
        { cx + halfS / 4, cy + halfS }
    };

    HPEN pen = CreatePen(PS_SOLID, 2, muted ? RGB(200, 80, 80) : RGB(180, 220, 200));
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));

    // 绘制喇叭轮廓
    MoveToEx(hdc, pts[0].x, pts[0].y, nullptr);
    LineTo(hdc, pts[1].x, pts[1].y);
    LineTo(hdc, pts[2].x, pts[2].y);
    LineTo(hdc, pts[3].x, pts[3].y);
    LineTo(hdc, pts[1].x, pts[1].y + 2 * halfS / 3);
    LineTo(hdc, pts[0].x, pts[0].y + 2 * halfS / 3);
    LineTo(hdc, pts[0].x, pts[0].y);

    // 非静音时绘制声波弧线
    if (!muted) {
        int waveX = cx + halfS / 3;
        Arc(hdc, waveX, cy - halfS / 2, waveX + halfS, cy + halfS / 2,
             waveX, cy - halfS / 4, waveX, cy + halfS / 4);
        Arc(hdc, waveX - halfS / 4, cy - halfS * 3 / 4, waveX + halfS * 3 / 4, cy + halfS * 3 / 4,
             waveX, cy - halfS / 2, waveX, cy + halfS / 2);
    }

    // 静音时绘制红色 X 号
    if (muted) {
        HPEN xPen = CreatePen(PS_SOLID, 2, RGB(200, 80, 80));
        HGDIOBJ oldXPen = SelectObject(hdc, xPen);
        MoveToEx(hdc, cx - halfS, cy - halfS, nullptr);
        LineTo(hdc, cx + halfS, cy + halfS);
        MoveToEx(hdc, cx + halfS, cy - halfS, nullptr);
        LineTo(hdc, cx - halfS, cy + halfS);
        SelectObject(hdc, oldXPen);
        DeleteObject(xPen);
    }

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

// ============================================================
// OverlayWindow::WndProc - 浮层窗口过程
// 处理绘制、定时器（自动隐藏）等消息
// ============================================================
LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE: {
        // 保存 this 指针到窗口用户数据
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    case WM_ERASEBKGND:
        // 禁止背景擦除，减少闪烁
        return 1;
    case WM_PAINT: {
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);
        int W = rc.right;
        int H = rc.bottom;

        // 填充深色背景
        FillRect(hdc, &rc, self->m_bgBrush);

        // 绘制边框
        HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(60, 60, 80));
        HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
        HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, rc.left, rc.top, rc.right - 1, rc.bottom - 1);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(borderPen);

        SetBkMode(hdc, TRANSPARENT);

        if (self->m_showMuteOverlay) {
            // --- 静音浮层绘制 ---
            DrawSpeakerIcon(hdc, W / 2, OVERLAY_ICON_Y, OVERLAY_ICON_SIZE, self->m_audioMuted);

            // 静音/取消静音标签
            const wchar_t* muteLabel = self->m_audioMuted ? L"静音" : L"取消静音";
            COLORREF muteColor = self->m_audioMuted ? RGB(200, 80, 80) : RGB(100, 200, 130);
            RECT labelRect = { 0, OVERLAY_MUTE_LABEL_TOP, W, OVERLAY_MUTE_LABEL_BOTTOM };
            SetTextColor(hdc, muteColor);
            HFONT oldFontM = (HFONT)SelectObject(hdc, self->m_fontMedium);
            DrawTextW(hdc, muteLabel, -1, &labelRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, oldFontM);

            // 非静音时显示音量百分比
            if (!self->m_audioMuted) {
                wchar_t volText[16] = {};
                swprintf_s(volText, L"%d%%", self->m_audioVolume);
                RECT volRect = { 0, OVERLAY_VOL_LABEL_TOP, W, OVERLAY_VOL_LABEL_BOTTOM };
                SetTextColor(hdc, RGB(180, 220, 200));
                HFONT oldFontS = (HFONT)SelectObject(hdc, self->m_fontSmall);
                DrawTextW(hdc, volText, -1, &volRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(hdc, oldFontS);
            }

            // 绘制音量条
            int barLeft = OVERLAY_BAR_MARGIN;
            int barRight = W - OVERLAY_BAR_MARGIN;
            int barTop = self->m_audioMuted ? OVERLAY_MUTE_LABEL_BOTTOM + 4 : OVERLAY_VOL_LABEL_BOTTOM + 6;
            int barBottom = barTop + OVERLAY_BAR_HEIGHT;
            int barW = barRight - barLeft;

            // 音量条背景
            RECT barBgRect = { barLeft, barTop, barRight, barBottom };
            FillRect(hdc, &barBgRect, self->m_barBgBrush);

            if (self->m_audioMuted) {
                // 静音时整个音量条填充红色
                RECT barMuteRect = { barLeft, barTop, barRight, barBottom };
                FillRect(hdc, &barMuteRect, self->m_barMuteBrush);
            } else {
                // 非静音时按音量百分比填充绿色
                int fillW = barW * self->m_audioVolume / 100;
                if (fillW > 0) {
                    RECT barFillRect = { barLeft, barTop, barLeft + fillW, barBottom };
                    FillRect(hdc, &barFillRect, self->m_barFillBrush);
                }
            }

            // 音量条边框
            HPEN barPen = CreatePen(PS_SOLID, 1, RGB(70, 70, 90));
            HPEN prevPen2 = (HPEN)SelectObject(hdc, barPen);
            HBRUSH prevBrush2 = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, barLeft, barTop, barRight, barBottom);
            SelectObject(hdc, prevBrush2);
            SelectObject(hdc, prevPen2);
            DeleteObject(barPen);
        } else {
            // --- 锁键浮层绘制 ---
            const wchar_t* iconText = L"";
            COLORREF iconColor;
            // 根据锁键类型和状态选择图标文字和颜色
            if (self->m_vkCode == VK_CAPITAL) {
                iconText = self->m_state ? L"AA" : L"aa";
                iconColor = self->m_state ? RGB(100, 200, 130) : RGB(160, 160, 180);
            } else if (self->m_vkCode == VK_NUMLOCK) {
                iconText = self->m_state ? L"123" : L"↕↔";
                iconColor = self->m_state ? RGB(100, 200, 130) : RGB(160, 160, 180);
            } else if (self->m_vkCode == VK_SCROLL) {
                iconText = self->m_state ? L"SCR" : L"scr";
                iconColor = self->m_state ? RGB(100, 200, 130) : RGB(160, 160, 180);
            }

            // 绘制锁键图标
            RECT iconRect = { 0, OVERLAY_KEY_ICON_TOP, W, OVERLAY_KEY_ICON_BOTTOM };
            SetTextColor(hdc, iconColor);
            HFONT oldFontL = (HFONT)SelectObject(hdc, self->m_fontLarge);
            DrawTextW(hdc, iconText, -1, &iconRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, oldFontL);

            // 绘制锁键名称和状态标签
            const wchar_t* keyName = L"";
            if (self->m_vkCode == VK_CAPITAL) keyName = L"Caps Lock";
            else if (self->m_vkCode == VK_NUMLOCK) keyName = L"Num Lock";
            else if (self->m_vkCode == VK_SCROLL) keyName = L"Scroll Lock";

            wchar_t label[64] = {};
            if (self->m_locked) {
                swprintf_s(label, L"%s: 锁", keyName);
            } else {
                swprintf_s(label, L"%s: %s", keyName, self->m_state ? L"开" : L"关");
            }

            RECT labelRect = { 0, OVERLAY_KEY_LABEL_TOP, W, OVERLAY_KEY_LABEL_BOTTOM };
            SetTextColor(hdc, RGB(200, 200, 220));
            HFONT oldFontM = (HFONT)SelectObject(hdc, self->m_fontMedium);
            DrawTextW(hdc, label, -1, &labelRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, oldFontM);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        // 定时器到期，隐藏浮层
        if (wp == TIMER_OVERLAY) {
            KillTimer(hwnd, TIMER_OVERLAY);
            ShowWindow(hwnd, SW_HIDE);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_OVERLAY);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}
