#include "Overlay.h"
#include "Resource.h"
#include "Utils.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <algorithm>
#include <cmath>

const wchar_t* OverlayWindow::CLASS_NAME = L"KeySentryOverlayWnd";

OverlayWindow::OverlayWindow() = default;

OverlayWindow::~OverlayWindow() {
    Destroy();
}

void OverlayWindow::SetPrimaryOnly(bool primaryOnly) {
    m_primaryOnly = primaryOnly;
}

// ============================================================
// SetAppearance - 设置浮层外观（位置/主题/边框/透明度/显示时长）
// ============================================================
void OverlayWindow::SetAppearance(int position, int theme, int corner, int opacityLevel, int delayLevel) {
    // 手改 INI 的越界值一律收敛到默认，避免出现无意义的绘制结果
    m_position = (position >= 0 && position <= 4) ? position : 0;
    m_theme = (theme >= 0 && theme <= 1) ? theme : 0;
    m_corner = (corner >= 0 && corner <= 1) ? corner : 1;
    if (opacityLevel < 0 || opacityLevel > 3) opacityLevel = 2;
    m_alpha = (opacityLevel == 0) ? OVERLAY_ALPHA_TRANSPARENT  // 高透明
            : (opacityLevel == 1) ? OVERLAY_ALPHA              // 适中
            : (opacityLevel == 2) ? OVERLAY_ALPHA_LOW_TRANSPARENCY // 低透明
            : 255;                                             // 不透明
    if (delayLevel < 0 || delayLevel > 3) delayLevel = 1;
    m_showDurationMs = (delayLevel == 0) ? OVERLAY_DURATION_FAST_MS
                     : (delayLevel == 2) ? OVERLAY_DURATION_SLOW_MS
                     : (delayLevel == 3) ? 0                      // 保持显示
                     : OVERLAY_DURATION_MEDIUM_MS;

    // 主题变化会影响背景画刷；隐藏中的窗口也需立即更新，下次显示即为新样式
    RecreateBrushes();

    for (HWND hwnd : m_hwnds) {
        if (!hwnd) continue;
        // 由"保持显示"切换为定时隐藏时，为当前仍显示的浮层重新计时，
        // 否则它会一直留在屏幕上；切到"保持显示"则取消自动隐藏定时器
        if (m_showDurationMs > 0 && IsWindowVisible(hwnd)) {
            KillTimer(hwnd, TIMER_OVERLAY);
            SetTimer(hwnd, TIMER_OVERLAY, (UINT)m_showDurationMs, nullptr);
        } else if (m_showDurationMs == 0) {
            KillTimer(hwnd, TIMER_OVERLAY);
        }
    }

    // 已显示的浮层必须立即按新位置/显示器集合重新定位：RenderNow 只按当前窗口矩形
    // 重绘、不移动窗口。否则"保持显示"档位下改完位置点"应用"，浮层会一直停在旧位置，
    // 直到下一次按键触发；同理刚开启"仅在主屏显示提示"时副屏浮层也不会立即隐藏
    bool anyVisible = false;
    for (HWND h : m_hwnds) {
        if (h && IsWindowVisible(h)) { anyVisible = true; break; }
    }
    // 没有可见浮层时不创建/移动窗口：下次 Show 会按新配置自行定位
    if (anyVisible) {
        auto monitors = GetTargetMonitors();
        EnsureWindowCount((int)monitors.size());
        int ovW = ScaleUi(OVERLAY_SIZE);
        int ovH = m_showMuteOverlay ? ovW : ScaleUi(OVERLAY_KEY_HEIGHT);
        for (size_t i = 0; i < m_hwnds.size(); ++i) {
            HWND h = m_hwnds[i];
            if (!h) continue;
            if (i >= monitors.size()) {
                // 目标显示器减少（例如开启"仅在主屏显示"）：多余浮层隐藏并停掉自动隐藏定时器
                if (IsWindowVisible(h)) {
                    ShowWindow(h, SW_HIDE);
                    KillTimer(h, TIMER_OVERLAY);
                }
                continue;
            }
            if (!IsWindowVisible(h)) continue;
            MONITORINFO mi = { sizeof(mi) };
            if (!GetMonitorInfoW(monitors[i], &mi)) continue;
            int x = 0, y = 0;
            ComputePosition(mi.rcWork, ovW, ovH, &x, &y);
            // 不改变 Z-order：已置顶，重复 HWND_TOPMOST 会让任务栏/DWM 重新排序
            SetWindowPos(h, nullptr, x, y, ovW, ovH, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    // 外观变化：隐藏中的窗口也同步一次离屏内容，下次显示即最新样式
    RenderNow();
}

// 立即收起所有浮层（同时停掉自动隐藏定时器）。
// 关闭两项提示时必须主动调用：浮层不会因为配置变化自己消失，
// "保持显示"档位下更会一直留在屏幕上
void OverlayWindow::HideAll() {
    for (HWND hwnd : m_hwnds) {
        if (!hwnd) continue;
        KillTimer(hwnd, TIMER_OVERLAY);
        if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
    }
}

// 依据主题重建背景画刷（深色为深底浅字，浅色为浅底深字）
void OverlayWindow::RecreateBrushes() {
    if (m_bgBrush) { DeleteObject(m_bgBrush); m_bgBrush = nullptr; }
    if (m_barBgBrush) { DeleteObject(m_barBgBrush); m_barBgBrush = nullptr; }
    m_bgBrush = CreateSolidBrush(m_theme == 1 ? RGB(246, 246, 250) : RGB(30, 30, 42));
    m_barBgBrush = CreateSolidBrush(m_theme == 1 ? RGB(212, 212, 224) : RGB(55, 55, 75));
}

// ============================================================
// 离屏缓冲：逐像素透明度（圆角抗锯齿）需要 32 位 DIB
// ============================================================
bool OverlayWindow::EnsureBackBuffer(int w, int h) {
    if (m_memDC && m_memW == w && m_memH == h) return true;
    DestroyBackBuffer();

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;  // 负高度表示自上而下的位图，与矩形坐标一致
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    m_memBmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &m_memBits, nullptr, 0);
    m_memDC = CreateCompatibleDC(screen);
    if (screen) ReleaseDC(nullptr, screen);
    if (!m_memBmp || !m_memDC) {
        DestroyBackBuffer();
        return false;
    }
    m_memOldBmp = (HBITMAP)SelectObject(m_memDC, m_memBmp);
    m_memW = w;
    m_memH = h;
    return true;
}

void OverlayWindow::DestroyBackBuffer() {
    if (m_memDC) {
        if (m_memOldBmp) SelectObject(m_memDC, m_memOldBmp);
        DeleteDC(m_memDC);
        m_memDC = nullptr;
    }
    m_memOldBmp = nullptr;
    if (m_memBmp) { DeleteObject(m_memBmp); m_memBmp = nullptr; }
    m_memBits = nullptr;
    m_memW = m_memH = 0;
}

// ============================================================
// Render - 绘制到离屏位图并按像素提交给分层窗口
// 圆角处按像素覆盖率平滑混合，消除窗口区域裁剪造成的锯齿
// ============================================================
void OverlayWindow::Render(HWND hwnd, int w, int h) {
    if (!hwnd || w <= 0 || h <= 0) return;
    if (!EnsureBackBuffer(w, h)) return;

    PaintContent(m_memDC, w, h);

    // 逐像素处理：主体为整体不透明度，圆角区域按圆形覆盖率给出抗锯齿边缘。
    // UpdateLayeredWindow 要求源位图为"预乘 alpha"格式，故 RGB 需与该像素的
    // 透明度同比缩放后写回；否则半透明区域的颜色偏亮，圆角外沿会出现白边
    BYTE* px = reinterpret_cast<BYTE*>(m_memBits);
    if (px) {
        BYTE base = (BYTE)m_alpha;
        int r = ScaleUi(OVERLAY_CORNER_RADIUS);
        bool rounded = (m_corner == 1) && r > 0 && r < w && r < h;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int cov = 255;  // 覆盖率（0-255），直边区域恒为满
                if (rounded) {
                    // 仅四角区域需要按圆形边界计算覆盖率，其余像素为满覆盖
                    int cxo = -1, cyo = -1;
                    if (x < r && y < r) { cxo = r; cyo = r; }
                    else if (x >= w - r && y < r) { cxo = w - r; cyo = r; }
                    else if (x < r && y >= h - r) { cxo = r; cyo = h - r; }
                    else if (x >= w - r && y >= h - r) { cxo = w - r; cyo = h - r; }
                    if (cxo >= 0) {
                        double dx = (x + 0.5) - cxo;
                        double dy = (y + 0.5) - cyo;
                        double dist = sqrt(dx * dx + dy * dy);
                        double c = r - dist + 0.5;
                        if (c <= 0.0) cov = 0;
                        else if (c >= 1.0) cov = 255;
                        else cov = (int)(c * 255.0);
                    }
                }
                BYTE a = (BYTE)(base * cov / 255);
                BYTE* p = px + (y * w + x) * 4;
                p[0] = (BYTE)(p[0] * a / 255);  // 预乘 alpha：B/G/R 同比缩放
                p[1] = (BYTE)(p[1] * a / 255);
                p[2] = (BYTE)(p[2] * a / 255);
                p[3] = a;
            }
        }
    }

    RECT wr = {};
    GetWindowRect(hwnd, &wr);
    POINT ptDst = { wr.left, wr.top };
    POINT ptSrc = { 0, 0 };
    SIZE sz = { w, h };
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(hwnd, nullptr, &ptDst, &sz, m_memDC, &ptSrc, 0, &blend, ULW_ALPHA);
}

// ============================================================
// RenderNow - 按当前内容/尺寸立即渲染当前可见的浮层窗口
// 只在内容或尺寸真正发生变化时调用（显示浮层、切换静音浮层、应用外观设置），
// 不再由系统重绘消息驱动整窗逐像素处理，避免在承载键盘钩子的 UI 线程上
// 做与内容无关的同步重活。
// 隐藏窗口（显示器减少后残留的多余窗口）跳过：它们在下一次 Show()/ShowMute()
// 被重新显示后紧接着还有一次 RenderNow，无需提前做逐像素处理
// ============================================================
void OverlayWindow::RenderNow() {
    for (HWND hwnd : m_hwnds) {
        if (!hwnd || !IsWindowVisible(hwnd)) continue;
        RECT rc;
        GetClientRect(hwnd, &rc);
        Render(hwnd, rc.right, rc.bottom);
    }
}

// 计算浮层左上角坐标：底部/顶部按边距贴边，左侧/右侧纵向居中，居中则双向居中
void OverlayWindow::ComputePosition(const RECT& rcWork, int ovW, int ovH, int* x, int* y) const {
    int margin = ScaleUi(OVERLAY_BOTTOM_MARGIN);
    int workW = rcWork.right - rcWork.left;
    int workH = rcWork.bottom - rcWork.top;
    switch (m_position) {
    case 1:  // 顶部
        *x = rcWork.left + (workW - ovW) / 2;
        *y = rcWork.top + margin;
        break;
    case 2:  // 居中
        *x = rcWork.left + (workW - ovW) / 2;
        *y = rcWork.top + (workH - ovH) / 2;
        break;
    case 3:  // 左侧
        *x = rcWork.left + margin;
        *y = rcWork.top + (workH - ovH) / 2;
        break;
    case 4:  // 右侧
        *x = rcWork.right - ovW - margin;
        *y = rcWork.top + (workH - ovH) / 2;
        break;
    default: // 底部
        *x = rcWork.left + (workW - ovW) / 2;
        *y = rcWork.bottom - ovH - margin;
        break;
    }
}

// 主题相关颜色
COLORREF OverlayWindow::BorderColor() const {
    return m_theme == 1 ? RGB(202, 202, 214) : RGB(60, 60, 80);
}

COLORREF OverlayWindow::LabelColor() const {
    return m_theme == 1 ? RGB(45, 45, 60) : RGB(200, 200, 220);
}

COLORREF OverlayWindow::DimColor() const {
    return m_theme == 1 ? RGB(120, 120, 132) : RGB(160, 160, 180);
}

COLORREF OverlayWindow::VolumeTextColor() const {
    return m_theme == 1 ? RGB(30, 120, 65) : RGB(180, 220, 200);
}

// 创建单个浮层窗口：分层、置顶、工具窗口、透明鼠标穿透
HWND OverlayWindow::CreateOverlayWnd() {
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        CLASS_NAME, L"",
        WS_POPUP,
        0, 0, ScaleUi(OVERLAY_SIZE), ScaleUi(OVERLAY_SIZE),
        nullptr, nullptr, m_hInst, this
    );
    // 分层窗口在首次 UpdateLayeredWindow 前不显示内容，无需在此设置透明度
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
        // 取不到显示器信息时不返回该显示器：调用方会用 rcWork 计算浮层位置，
        // 失败时 rcWork 全为 0，会把浮层画到 (0,0) 附近
        if (!GetMonitorInfoW(hPrimary, &mi)) return monitors;
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
        // 剔除信息不可用的显示器，避免浮层定位到 (0,0)
        monitors.erase(
            std::remove_if(monitors.begin(), monitors.end(), [](HMONITOR hMon) {
                MONITORINFO mi = { sizeof(mi) };
                return !GetMonitorInfoW(hMon, &mi) || mi.rcWork.right <= mi.rcWork.left ||
                       mi.rcWork.bottom <= mi.rcWork.top;
            }),
            monitors.end());
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
    RecreateBrushes();                                    // 背景画刷按当前主题创建
    m_barFillBrush = CreateSolidBrush(RGB(100, 200, 130));// 绿色填充
    m_barMuteBrush = CreateSolidBrush(RGB(200, 80, 80));  // 红色静音

    return true;
}

// 销毁所有浮层窗口和 GDI 资源
void OverlayWindow::Destroy() {
    // 先销毁窗口再释放 GDI/COM 资源：窗口过程仍会引用画刷与字体，
    // 若在此期间触发 WM_PAINT 会造成已释放对象被引用
    for (HWND hwnd : m_hwnds) {
        if (hwnd) DestroyWindow(hwnd);
    }
    m_hwnds.clear();
    DestroyBackBuffer();
    if (m_pVolume) { m_pVolume->Release(); m_pVolume = nullptr; }
    m_volumeDeviceId.clear();
    if (m_pEnumerator) { m_pEnumerator->Release(); m_pEnumerator = nullptr; }
    if (m_fontLarge) { DeleteObject(m_fontLarge); m_fontLarge = nullptr; }
    if (m_fontMedium) { DeleteObject(m_fontMedium); m_fontMedium = nullptr; }
    if (m_fontSmall) { DeleteObject(m_fontSmall); m_fontSmall = nullptr; }
    if (m_bgBrush) { DeleteObject(m_bgBrush); m_bgBrush = nullptr; }
    if (m_barBgBrush) { DeleteObject(m_barBgBrush); m_barBgBrush = nullptr; }
    if (m_barFillBrush) { DeleteObject(m_barFillBrush); m_barFillBrush = nullptr; }
    if (m_barMuteBrush) { DeleteObject(m_barMuteBrush); m_barMuteBrush = nullptr; }
}

// ============================================================
// OverlayWindow::GetAudioState - 获取当前系统音频状态
// 通过 Windows Core Audio API 查询默认音频输出设备
// ============================================================
bool OverlayWindow::GetAudioState(bool& isMuted, int& volumeLevel) {
    isMuted = false;
    volumeLevel = 0;

    // COM 已在主线程 wWinMain 中初始化，无需重复初始化

    // 复用缓存的设备枚举器，避免每次查询都 CoCreateInstance（枚举器与具体设备无关，
    // 默认设备切换后 GetDefaultAudioEndpoint 会返回新的默认设备，无需重建）
    if (!m_pEnumerator) {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                               __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&m_pEnumerator));
        if (FAILED(hr)) {
            m_pEnumerator = nullptr;
            return false;
        }
    }

    IMMDevice* pDevice = nullptr;
    HRESULT hr = m_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    if (FAILED(hr)) return false;

    // 读取默认设备 ID：设备切换（插拔耳机、切换输出）时需重建音量接口，
    // 否则会一直查询旧设备的静音/音量状态
    LPWSTR devId = nullptr;
    std::wstring deviceId;
    if (SUCCEEDED(pDevice->GetId(&devId)) && devId) {
        deviceId = devId;
        CoTaskMemFree(devId);
    }
    if (!m_pVolume || deviceId != m_volumeDeviceId) {
        if (m_pVolume) { m_pVolume->Release(); m_pVolume = nullptr; }
        if (SUCCEEDED(pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                                        nullptr, reinterpret_cast<void**>(&m_pVolume)))) {
            m_volumeDeviceId = deviceId;
        } else {
            m_pVolume = nullptr;
            m_volumeDeviceId.clear();
        }
    }
    pDevice->Release();
    if (!m_pVolume) return false;

    BOOL muted = FALSE;
    if (FAILED(m_pVolume->GetMute(&muted))) {
        // 端点已失效（设备被移除/切换等）：释放缓存，下次查询时重新创建
        m_pVolume->Release();
        m_pVolume = nullptr;
        m_volumeDeviceId.clear();
        return false;
    }
    isMuted = muted != FALSE;

    float vol = 0.0f;
    if (SUCCEEDED(m_pVolume->GetMasterVolumeLevelScalar(&vol))) {
        volumeLevel = (int)(vol * 100.0f + 0.5f);
        if (volumeLevel > 100) volumeLevel = 100;
    } else {
        // 音量读取失败不算整体失败：部分虚拟声卡/独占模式不报告主音量，
        // 若据此判定失败会让静音浮层在这类设备上完全不显示（muted 状态仍有效）。
        // 但必须输出 -1 表示"音量未知"，否则绘制层会把调用方传入的 0 当成真实音量
        // 显示成 "0%"，与实际不符
        volumeLevel = -1;
    }
    return true;
}

// ============================================================
// OverlayWindow::Show - 显示锁键状态浮层
// 在每个目标显示器底部居中显示，1秒后自动隐藏
// ============================================================
void OverlayWindow::Show(int vkCode, bool state, bool locked) {
    if (m_hwnds.empty()) return;

    // 连发/重复通知防护：内容完全一致且距上次显示不足去抖间隔时直接返回，
    // 不再执行置顶重排、重绘与定时器重置（否则会使浮层闪烁并拖垮任务栏/DWM）
    bool visible = IsWindowVisible(m_hwnds[0]) != FALSE;
    ULONGLONG now = GetTickCount64();
    bool sameContent = (!m_showMuteOverlay && m_vkCode == vkCode &&
                        m_state == state && m_locked == locked);
    bool tooSoon = (m_lastShowTick != 0) && (now - m_lastShowTick < OVERLAY_MIN_REPEAT_MS);
    // 仅在"内容一致且过密"时丢弃（连发抑制）；内容确实变化时即使很近也照常显示，
    // 避免快速切换不同锁键状态被误当作连发丢弃
    if (visible && sameContent && tooSoon) return;
    m_lastShowTick = now;

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
        int ovW = ScaleUi(OVERLAY_SIZE);
        int ovH = ScaleUi(OVERLAY_KEY_HEIGHT);
        int x = 0, y = 0;
        ComputePosition(mi.rcWork, ovW, ovH, &x, &y);

        // 仅在窗口原本隐藏时才重新置顶显示，避免可见状态下的重复 SetWindowPos
        if (!IsWindowVisible(m_hwnds[i])) {
            SetWindowPos(m_hwnds[i], HWND_TOPMOST, x, y, ovW, ovH,
                         SWP_SHOWWINDOW | SWP_NOACTIVATE);
        } else {
            // 已可见时仍校正位置/尺寸（锁键与静音浮层高度不同、显示器布局可能变化），
            // 但不改变 Z-order，避免重复置顶重排
            SetWindowPos(m_hwnds[i], nullptr, x, y, ovW, ovH,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        KillTimer(m_hwnds[i], TIMER_OVERLAY);
        // 保持显示档（时长为 0）只置顶显示，不启动自动隐藏定时器
        if (m_showDurationMs > 0) {
            SetTimer(m_hwnds[i], TIMER_OVERLAY, (UINT)m_showDurationMs, nullptr);
        }
    }

    // 隐藏多余的浮层窗口
    for (size_t i = monitors.size(); i < m_hwnds.size(); i++) {
        ShowWindow(m_hwnds[i], SW_HIDE);
        KillTimer(m_hwnds[i], TIMER_OVERLAY);
    }
    // 内容/尺寸已变化，由内容驱动渲染一次，不再依赖系统重绘消息
    RenderNow();
}

// ============================================================
// OverlayWindow::ShowMute - 显示静音状态浮层
// 显示喇叭图标、静音/取消静音标签、音量百分比和音量条
// ============================================================
void OverlayWindow::ShowMute() {
    if (m_hwnds.empty()) return;

    // 先查询当前音频状态，用于判断浮层内容是否真的变化
    bool isMuted = false;
    int volume = 0;
    // 端点不可用时不显示浮层：否则会把默认值（未静音/0%）当作真实状态展示
    if (!GetAudioState(isMuted, volume)) return;

    // 连发去抖：静音键被按住时系统会持续产生 keydown，仅在"内容一致且过密"时丢弃；
    // 静音状态确实切换时即使间隔很近也照常显示，避免真实状态变化被误丢
    bool visible = IsWindowVisible(m_hwnds[0]) != FALSE;
    ULONGLONG now = GetTickCount64();
    bool sameContent = (m_showMuteOverlay && m_audioMuted == isMuted && m_audioVolume == volume);
    bool tooSoon = (m_lastShowTick != 0) && (now - m_lastShowTick < OVERLAY_MIN_REPEAT_MS);
    if (visible && sameContent && tooSoon) return;
    m_lastShowTick = now;

    m_showMuteOverlay = true;
    m_audioVolume = volume;
    m_audioMuted = isMuted;

    auto monitors = GetTargetMonitors();
    EnsureWindowCount((int)monitors.size());

    for (size_t i = 0; i < monitors.size() && i < m_hwnds.size(); i++) {
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(monitors[i], &mi);
        int ovSize = ScaleUi(OVERLAY_SIZE);
        int x = 0, y = 0;
        ComputePosition(mi.rcWork, ovSize, ovSize, &x, &y);

        // 仅在窗口原本隐藏时才重新置顶显示
        if (!IsWindowVisible(m_hwnds[i])) {
            SetWindowPos(m_hwnds[i], HWND_TOPMOST, x, y, ovSize, ovSize,
                         SWP_SHOWWINDOW | SWP_NOACTIVATE);
        } else {
            // 已可见时仍校正位置/尺寸（从锁键浮层切换过来时高度不同），不改变 Z-order
            SetWindowPos(m_hwnds[i], nullptr, x, y, ovSize, ovSize,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        KillTimer(m_hwnds[i], TIMER_OVERLAY);
        // 保持显示档（时长为 0）只置顶显示，不启动自动隐藏定时器
        if (m_showDurationMs > 0) {
            SetTimer(m_hwnds[i], TIMER_OVERLAY, (UINT)m_showDurationMs, nullptr);
        }
    }

    for (size_t i = monitors.size(); i < m_hwnds.size(); i++) {
        ShowWindow(m_hwnds[i], SW_HIDE);
        KillTimer(m_hwnds[i], TIMER_OVERLAY);
    }
    // 内容/尺寸已变化，由内容驱动渲染一次，不再依赖系统重绘消息
    RenderNow();
}

// ============================================================
// DrawSpeakerIcon - 绘制喇叭图标
//   - 非静音时绘制声波弧线
//   - 静音时绘制红色 X 号
// ============================================================
static void DrawSpeakerIcon(HDC hdc, int cx, int cy, int size, bool muted, bool lightTheme) {
    int s = size;
    int halfS = s / 2;

    // 喇叭主体轮廓点
    POINT pts[4] = {
        { cx - halfS, cy - halfS / 3 },
        { cx - halfS / 3, cy - halfS / 3 },
        { cx + halfS / 4, cy - halfS },
        { cx + halfS / 4, cy + halfS }
    };

    // 图标线色按主题适配：浅色主题使用更深的绿/红以保证对比度
    COLORREF normalColor = lightTheme ? RGB(30, 120, 65) : RGB(180, 220, 200);
    COLORREF muteColor = lightTheme ? RGB(190, 60, 60) : RGB(200, 80, 80);
    HPEN pen = CreatePen(PS_SOLID, 2, muted ? muteColor : normalColor);
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
        HPEN xPen = CreatePen(PS_SOLID, 2, muteColor);
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
// OverlayWindow::PaintContent - 绘制浮层内容
// 仅绘制到离屏位图，尺寸由调用方给出；透明度与圆角由 Render 统一处理
// ============================================================
void OverlayWindow::PaintContent(HDC hdc, int w, int h) {
    RECT rc = { 0, 0, w, h };

    // 填充背景（按主题：深色/浅色）
    // 平面化外观：不绘制窗口外缘描边。轮廓形状（圆角/直角）与边缘抗锯齿完全由
    // Render 按 alpha 覆盖率生成；额外加一圈描边会在右侧与下侧形成比背景更亮的
    // "高光"线，破坏平面感
    FillRect(hdc, &rc, m_bgBrush);

    SetBkMode(hdc, TRANSPARENT);

    if (m_showMuteOverlay) {
        // --- 静音浮层绘制 ---
        DrawSpeakerIcon(hdc, w / 2, ScaleUi(OVERLAY_ICON_Y), ScaleUi(OVERLAY_ICON_SIZE),
                        m_audioMuted, m_theme == 1);

        // 静音/取消静音标签
        const wchar_t* muteLabel = m_audioMuted ? L"静音" : L"取消静音";
        COLORREF muteColor = m_audioMuted
            ? (m_theme == 1 ? RGB(190, 60, 60) : RGB(200, 80, 80))
            : (m_theme == 1 ? RGB(30, 140, 70) : RGB(100, 200, 130));
        RECT labelRect = { 0, ScaleUi(OVERLAY_MUTE_LABEL_TOP), w, ScaleUi(OVERLAY_MUTE_LABEL_BOTTOM) };
        SetTextColor(hdc, muteColor);
        HFONT oldFontM = (HFONT)SelectObject(hdc, m_fontMedium);
        DrawTextW(hdc, muteLabel, -1, &labelRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFontM);

        // 非静音时显示音量百分比（音量未知时 m_audioVolume 为 -1，跳过）
        if (!m_audioMuted && m_audioVolume >= 0) {
            wchar_t volText[16] = {};
            swprintf_s(volText, L"%d%%", m_audioVolume);
            RECT volRect = { 0, ScaleUi(OVERLAY_VOL_LABEL_TOP), w, ScaleUi(OVERLAY_VOL_LABEL_BOTTOM) };
            SetTextColor(hdc, VolumeTextColor());
            HFONT oldFontS = (HFONT)SelectObject(hdc, m_fontSmall);
            DrawTextW(hdc, volText, -1, &volRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, oldFontS);
        }

        // 绘制音量条
        int barLeft = ScaleUi(OVERLAY_BAR_MARGIN);
        int barRight = w - ScaleUi(OVERLAY_BAR_MARGIN);
        int barTop = m_audioMuted ? ScaleUi(OVERLAY_MUTE_LABEL_BOTTOM) + ScaleUi(4)
                                  : ScaleUi(OVERLAY_VOL_LABEL_BOTTOM) + ScaleUi(6);
        int barBottom = barTop + ScaleUi(OVERLAY_BAR_HEIGHT);
        int barW = barRight - barLeft;

        // 音量条背景
        RECT barBgRect = { barLeft, barTop, barRight, barBottom };
        FillRect(hdc, &barBgRect, m_barBgBrush);

        if (m_audioMuted) {
            // 静音时整个音量条填充红色
            RECT barMuteRect = { barLeft, barTop, barRight, barBottom };
            FillRect(hdc, &barMuteRect, m_barMuteBrush);
        } else {
            // 非静音时按音量百分比填充绿色
            int fillW = barW * m_audioVolume / 100;
            if (fillW > 0) {
                RECT barFillRect = { barLeft, barTop, barLeft + fillW, barBottom };
                FillRect(hdc, &barFillRect, m_barFillBrush);
            }
        }

        // 音量条边框
        HPEN barPen = CreatePen(PS_SOLID, 1, BorderColor());
        HPEN prevPen2 = (HPEN)SelectObject(hdc, barPen);
        HBRUSH prevBrush2 = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, barLeft, barTop, barRight, barBottom);
        SelectObject(hdc, prevBrush2);
        SelectObject(hdc, prevPen2);
        DeleteObject(barPen);
    } else {
        // --- 锁键浮层绘制 ---
        const wchar_t* iconText = L"";
        COLORREF iconColor = DimColor();  // 默认非激活色，避免未初始化
        COLORREF activeColor = m_theme == 1 ? RGB(30, 140, 70) : RGB(100, 200, 130);
        // 根据锁键类型和状态选择图标文字和颜色
        if (m_vkCode == VK_CAPITAL) {
            iconText = m_state ? L"AA" : L"aa";
            iconColor = m_state ? activeColor : DimColor();
        } else if (m_vkCode == VK_NUMLOCK) {
            iconText = m_state ? L"123" : L"↕↔";
            iconColor = m_state ? activeColor : DimColor();
        } else if (m_vkCode == VK_SCROLL) {
            iconText = m_state ? L"SCR" : L"scr";
            iconColor = m_state ? activeColor : DimColor();
        }

        // 绘制锁键图标
        RECT iconRect = { 0, ScaleUi(OVERLAY_KEY_ICON_TOP), w, ScaleUi(OVERLAY_KEY_ICON_BOTTOM) };
        SetTextColor(hdc, iconColor);
        HFONT oldFontL = (HFONT)SelectObject(hdc, m_fontLarge);
        DrawTextW(hdc, iconText, -1, &iconRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFontL);

        // 绘制锁键名称和状态标签
        const wchar_t* keyName = L"";
        if (m_vkCode == VK_CAPITAL) keyName = L"Caps Lock";
        else if (m_vkCode == VK_NUMLOCK) keyName = L"Num Lock";
        else if (m_vkCode == VK_SCROLL) keyName = L"Scroll Lock";

        wchar_t label[64] = {};
        if (m_locked) {
            swprintf_s(label, L"%s: 锁", keyName);
        } else {
            swprintf_s(label, L"%s: %s", keyName, m_state ? L"开" : L"关");
        }

        RECT labelRect = { 0, ScaleUi(OVERLAY_KEY_LABEL_TOP), w, ScaleUi(OVERLAY_KEY_LABEL_BOTTOM) };
        SetTextColor(hdc, LabelColor());
        HFONT oldFontM = (HFONT)SelectObject(hdc, m_fontMedium);
        DrawTextW(hdc, label, -1, &labelRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFontM);
    }
}

// ============================================================
// OverlayWindow::WndProc - 浮层窗口过程
// 处理绘制、定时器（自动隐藏）等消息
// ============================================================
LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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
        // 分层窗口的内容完全由 UpdateLayeredWindow 提交，WM_PAINT 不代表任何
        // 内容变化：此处仅做 BeginPaint/EndPaint 校验重绘区域，不执行渲染
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
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
