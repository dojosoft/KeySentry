#pragma once
#include <Windows.h>
#include <string>
#include <vector>

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
    // 显示静音状态浮层
    void ShowMute(bool muted);
    // 获取当前系统音频状态（是否静音、音量百分比）
    void GetAudioState(bool& isMuted, int& volumeLevel);
    // 设置是否仅在主显示器显示浮层
    void SetPrimaryOnly(bool primaryOnly);

private:
    // 浮层窗口过程
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    // 创建单个浮层窗口
    HWND CreateOverlayWnd();
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
    HINSTANCE m_hInst = nullptr;    // 应用实例句柄

    static const wchar_t* CLASS_NAME; // 浮层窗口类名

    // --- 浮层尺寸和布局常量 ---
    static const int OVERLAY_SIZE = 130;              // 浮层宽度和高度（像素）
    static const int OVERLAY_ALPHA = 170;             // 浮层透明度（0-255）
    static const int OVERLAY_SHOW_DURATION_MS = 1000; // 浮层显示持续时间（毫秒）
    static const int OVERLAY_KEY_HEIGHT = 98;         // 锁键浮层高度（比静音浮层矮）
    static const int OVERLAY_BOTTOM_MARGIN = 50;      // 浮层距屏幕底部的边距
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
