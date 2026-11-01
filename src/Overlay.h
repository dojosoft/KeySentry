#pragma once
#include <Windows.h>
#include <mmdeviceapi.h>
#include <string>
#include <vector>

// 前向声明：仅作指针成员使用，避免头文件引入 endpointvolume.h
struct IAudioEndpointVolume;

// ============================================================
// OverlayWindow: 锁键/静音状态浮层提示窗口
// 在屏幕底部居中显示一个半透明浮层，提示当前锁键或静音状态
// 支持多显示器：每个显示器上各显示一个浮层
// ============================================================
class OverlayWindow {
public:
    OverlayWindow();
    ~OverlayWindow();

    // 创建浮层窗口类和初始窗口
    bool Create(HINSTANCE hInst);
    // 销毁所有浮层窗口和 GDI 资源
    void Destroy();
    // 显示锁键状态浮层（CapsLock/NumLock/ScrollLock）
    void Show(int vkCode, bool state, bool locked = false);
    // 显示静音状态浮层（内部自行查询当前静音状态与音量）
    void ShowMute();
    // 获取当前系统音频状态（是否静音、音量百分比）
    // 返回 false 表示音频端点不可用（此时输出参数无意义，调用方应跳过提示）
    // volumeLevel 为 -1 表示音量读取失败（设备不报告主音量），调用方应隐藏百分比
    bool GetAudioState(bool& isMuted, int& volumeLevel);
    // 设置是否仅在主显示器显示浮层
    void SetPrimaryOnly(bool primaryOnly);
    // 设置浮层外观与显示时长：
    //   position 0=底部(默认) 1=顶部 2=居中 3=左侧 4=右侧
    //   theme    0=深色(默认) 1=浅色
    //   corner   0=直角 1=圆角(默认)
    //   opacity  0=高透明 1=适中 2=低透明(默认) 3=不透明
    //   delay    0=快速(0.5s) 1=适中(1s，默认) 2=缓慢(2s) 3=保持显示
    void SetAppearance(int position, int theme, int corner, int opacityLevel, int delayLevel);
    // 立即收起所有浮层：两项提示都关闭时调用，
    // 否则"保持显示"档位下已显示的浮层不会自动消失
    void HideAll();

private:
    // 浮层窗口过程
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    // 创建单个浮层窗口
    HWND CreateOverlayWnd();
    // 依据主题重建背景相关画刷
    void RecreateBrushes();
    // 将浮层内容渲染到 32 位离屏位图，并以逐像素透明度提交（圆角边缘抗锯齿）
    void Render(HWND hwnd, int w, int h);
    // 按当前内容与尺寸立即渲染所有浮层窗口（仅内容/尺寸变化时调用）
    void RenderNow();
    // 在指定 DC 上绘制浮层内容（背景、边框、图标与文字）
    void PaintContent(HDC hdc, int w, int h);
    // 重建离屏缓冲（尺寸变化时）
    bool EnsureBackBuffer(int w, int h);
    // 释放离屏缓冲
    void DestroyBackBuffer();
    // 依据位置设置计算浮层在指定工作区内的左上角坐标
    void ComputePosition(const RECT& rcWork, int ovW, int ovH, int* x, int* y) const;
    // 主题相关颜色
    COLORREF BorderColor() const;
    COLORREF LabelColor() const;
    COLORREF DimColor() const;
    COLORREF VolumeTextColor() const;
    // 获取目标显示器列表（根据 primaryOnly 设置）
    std::vector<HMONITOR> GetTargetMonitors();
    // 确保浮层窗口数量足够（每个显示器一个）
    void EnsureWindowCount(int count);

    std::vector<HWND> m_hwnds;      // 浮层窗口句柄列表（每个显示器一个）
    HFONT m_fontLarge = nullptr;    // 大号字体（锁键图标）
    HFONT m_fontMedium = nullptr;   // 中号字体（标签文字）
    HFONT m_fontSmall = nullptr;    // 小号字体（音量百分比）
    HBRUSH m_bgBrush = nullptr;     // 背景画刷（深色）
    HBRUSH m_barBgBrush = nullptr;  // 音量条背景画刷
    HBRUSH m_barFillBrush = nullptr;// 音量条填充画刷（绿色）
    HBRUSH m_barMuteBrush = nullptr;// 静音状态画刷（红色）

    int m_vkCode = 0;               // 当前显示的锁键虚拟码
    bool m_state = false;           // 当前锁键状态
    bool m_locked = false;          // 是否处于锁定状态（已禁用切换）
    bool m_audioMuted = false;      // 当前是否静音
    int m_audioVolume = 0;          // 当前音量百分比
    bool m_showMuteOverlay = false; // 是否显示静音浮层（而非锁键浮层）
    bool m_primaryOnly = false;     // 是否仅在主显示器显示
    int m_position = 0;             // 浮层位置（0=底部 1=顶部 2=居中 3=左侧 4=右侧）
    int m_theme = 0;                // 主题（0=深色 1=浅色）
    int m_corner = 1;               // 边框（0=直角 1=圆角）
    int m_alpha = OVERLAY_ALPHA_LOW_TRANSPARENCY; // 当前浮层不透明度（0-255），默认低透明档
    int m_showDurationMs = 1000;    // 浮层显示时长（毫秒）；0 表示保持显示不自动隐藏
    // 离屏渲染缓冲：逐像素透明度需要 32 位 DIB，尺寸随浮层变化重建
    HDC m_memDC = nullptr;
    HBITMAP m_memBmp = nullptr;
    HBITMAP m_memOldBmp = nullptr;
    void* m_memBits = nullptr;
    int m_memW = 0;
    int m_memH = 0;
    HINSTANCE m_hInst = nullptr;    // 应用实例句柄
    ULONGLONG m_lastShowTick = 0;   // 上次真正显示浮层的时刻（用于连发去抖）
    // 缓存的音频设备枚举器：MMDeviceEnumerator 可复用，避免每次查询都 CoCreateInstance
    IMMDeviceEnumerator* m_pEnumerator = nullptr;
    // 缓存的默认输出设备音量接口：静音键会触发最多 10 次轮询查询，
    // 复用该接口可避免每次都 Activate 一次 COM 对象
    IAudioEndpointVolume* m_pVolume = nullptr;
    std::wstring m_volumeDeviceId;    // m_pVolume 对应的设备 ID，默认设备切换后据此重建

    static const wchar_t* CLASS_NAME; // 浮层窗口类名

    // --- 浮层尺寸和布局常量 ---
    static const int OVERLAY_SIZE = 130;              // 浮层宽度和高度（像素）
    static const int OVERLAY_ALPHA = 170;             // 浮层不透明度（0-255），适中档
    static const int OVERLAY_ALPHA_TRANSPARENT = 110; // 高透明档
    static const int OVERLAY_ALPHA_LOW_TRANSPARENCY = 225; // 低透明档
    static const int OVERLAY_CORNER_RADIUS = 16;      // 圆角半径（Mac 风格）
    static const int OVERLAY_DURATION_FAST_MS = 500;  // 快速档显示时长
    static const int OVERLAY_DURATION_MEDIUM_MS = 1000; // 适中档显示时长
    static const int OVERLAY_DURATION_SLOW_MS = 2000; // 缓慢档显示时长
    static const int OVERLAY_KEY_HEIGHT = 98;         // 锁键浮层高度（比静音浮层矮）
    static const int OVERLAY_BOTTOM_MARGIN = 50;      // 浮层距屏幕边缘的留空距离（各位置通用）
    // 连发去抖：距上次真正显示不足该毫秒数的重复通知直接丢弃，
    // 防止按键连发时高频置顶重绘（浮层闪烁、任务栏卡顿）
    static const ULONGLONG OVERLAY_MIN_REPEAT_MS = 80;
    static const int OVERLAY_BAR_MARGIN = 12;         // 音量条左右边距
    static const int OVERLAY_BAR_HEIGHT = 10;         // 音量条高度
    static const int OVERLAY_ICON_Y = 30;             // 喇叭图标中心 Y 坐标
    static const int OVERLAY_ICON_SIZE = 24;          // 喇叭图标大小
    static const int OVERLAY_MUTE_LABEL_TOP = 52;     // 静音标签顶部 Y
    static const int OVERLAY_MUTE_LABEL_BOTTOM = 72;  // 静音标签底部 Y
    static const int OVERLAY_VOL_LABEL_TOP = 72;      // 音量标签顶部 Y
    static const int OVERLAY_VOL_LABEL_BOTTOM = 90;   // 音量标签底部 Y
    static const int OVERLAY_KEY_ICON_TOP = 8;        // 锁键图标顶部 Y
    static const int OVERLAY_KEY_ICON_BOTTOM = 52;    // 锁键图标底部 Y
    static const int OVERLAY_KEY_LABEL_TOP = 56;      // 锁键标签顶部 Y
    static const int OVERLAY_KEY_LABEL_BOTTOM = 82;   // 锁键标签底部 Y
};
