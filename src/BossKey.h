#pragma once
#include <Windows.h>
#include <vector>
#include <string>
#include <mutex>
#include "Config.h"
#include "HookThread.h"

// 前向声明：仅在成员中作指针使用，避免头文件引入 COM 头
struct IMMDeviceEnumerator;
struct IAudioEndpointVolume;

// ============================================================
// BossKey: 老板键管理器
// 核心功能：一键隐藏/恢复指定窗口，支持以下触发方式：
//   - 键盘热键
//   - 鼠标中键/侧键
//   - 鼠标移至屏幕角落
//   - 闲置自动隐藏
// 附加功能：
//   - 隐藏窗口时自动静音
//   - 隐藏窗口前发送媒体暂停
//   - 一键关闭绑定进程
// ============================================================
class BossKey {
public:
    BossKey();
    ~BossKey();

    // 注册老板键热键
    bool Register(HWND hwnd, int hotkeyId, UINT modifiers, UINT vk);
    // 注销老板键热键
    void Unregister();

    // 设置绑定的窗口列表
    void SetWindows(const std::vector<BoundWindowInfo>& windows);
    // 启用/禁用隐藏时静音
    void SetMuteEnabled(bool enabled);
    // 启用/禁用隐藏当前活动窗口
    void SetHideCurrentWindow(bool enabled);
    // 启用/禁用隐藏前发送媒体暂停
    void SetSendPauseBeforeHide(bool enabled);

    // 激活老板键（隐藏窗口）
    void Activate();
    // 停用老板键（恢复窗口）
    void Deactivate();
    // 当前是否处于激活状态
    bool IsActive() const { return m_active; }
    // 获取上次停用的时间戳（用于自动隐藏的冷却判断）
    ULONGLONG GetLastDeactivateTick() const { return m_lastDeactivateTick; }
    // 切换激活/停用状态
    void Toggle();
    // 关闭所有绑定的进程（优雅关闭 -> 超时强制终止）
    // timeoutMs：等待进程退出的超时时间，关机路径可传较小值避免阻塞
    void CloseBoundProcesses(DWORD timeoutMs = PROCESS_CLOSE_TIMEOUT_MS);
    // 恢复上次异常退出时被隐藏的窗口（启动时调用），返回恢复的窗口数
    int RecoverOrphanedWindows();
    // 启动时静音恢复是否仍未完成（音频服务/端点可能尚未就绪）
    bool HasPendingMuteRestore() const { return m_muteRestorePending; }
    // 静默重试启动时未完成的静音恢复；返回 true 表示已解决。
    // 由调用方的定时器驱动，失败不弹气泡，避免开机时误报设备暂不可用
    bool RetryPendingMuteRestore();
    // 将当前被隐藏窗口的信息持久化到恢复文件（关机前调用）
    void SaveRecoverFile();
    // 延迟确认"异步隐藏是否最终生效"（由调用方的定时器驱动）：
    // 生效则重新进入激活态，保证老板键仍能恢复这些窗口；否则清理残留记录
    void VerifyPendingHide();
    // 释放缓存的音频 COM 接口：正常退出时须在 CoUninitialize 之前调用
    void ReleaseAudio();
    // 正常退出路径：若系统静音由本程序造成则立即解除。
    // 必须在 ReleaseAudio() 与 CoUninitialize 之前调用，否则 COM 已反初始化会静默失败
    void RestoreSystemMute();

    // 安装鼠标低级钩子（用于中键/侧键触发）
    bool InstallMouseHook(HWND notifyWnd);
    // 卸载鼠标钩子
    void UninstallMouseHook();
    // 更新鼠标按钮触发设置
    void UpdateMouseHookSettings(bool middleBtn, bool sideBtn1, bool sideBtn2);

private:
    // 隐藏所有绑定窗口
    void HideWindows();
    // 恢复所有隐藏窗口
    void ShowWindows();
    // 修复隐藏/恢复后残留的输入模式（鼠标捕获/菜单/移动缩放循环）：
    // 已隐藏的窗口若仍独占鼠标捕获，会让全系统鼠标点击都送给它，
    // 表现为目标窗口按钮与任务栏点击无响应（键盘不受影响）
    void HealStuckInputMode();
    // 设置系统静音/取消静音（通过 Windows Core Audio API）
    // quiet=true：恢复失败时不弹托盘气泡（启动阶段的静默重试使用）
    void SetMute(bool mute, bool forceUnmute = false, bool quiet = false);
    // 静音恢复成功后，从遗留恢复内容中移除 m5 端点记录并落盘，
    // 避免下次启动再次尝试已恢复的端点
    void DropResolvedMuteRecordsFromLegacyContent();
    // 发送媒体暂停/播放按键
    void SendMediaPause();

    // 鼠标低级钩子回调
    static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam);

    // 恢复文件的全部候选位置（exe 同目录、%APPDATA%\KeySentry）
    static std::vector<std::wstring> GetRecoverFileCandidates();
    // 首选写入路径的缓存：写失败后可切换到另一候选位置（R15-M04）
    static std::wstring& RecoverPathCache();
    // 获取首选恢复文件路径（优先 exe 同目录，目录只读时回退 AppData）
    static std::wstring GetRecoverFilePath();
    // 将恢复记录原子写入恢复文件（content 为 v2/v3/v4 文本行）：
    // 首选位置失败时尝试其他候选；deleteOtherCandidates=false 用于
    // 保留另一份无法读取的旧记录供后续诊断/提权处理（R15-M04/M05）
    static bool SaveRecoverContent(const std::wstring& content,
                                   bool deleteOtherCandidates = true);
    // 删除恢复文件（窗口恢复/进程关闭后调用）
    void ClearRecoverFile();


    bool m_active = false;          // 当前是否处于激活（隐藏）状态
    bool m_pendingVerify = false;   // 是否等待"异步隐藏延迟确认"（见 VerifyPendingHide）
    // 异步隐藏的确认重试次数：隐藏可能晚于首次确认才生效，
    // 过早清账会让迟到隐藏的窗口失去恢复记录
    int m_pendingVerifyRetries = 0;
    HWND m_hwnd = nullptr;          // 热键所属窗口句柄
    int m_hotkeyId = 0;             // 热键ID
    bool m_registered = false;      // 热键是否已注册
    bool m_muteEnabled = true;      // 隐藏时是否静音
    bool m_hideCurrentWindow = false; // 是否隐藏当前活动窗口
    bool m_sendPauseBeforeHide = false; // 隐藏前是否发送媒体暂停

    // 被隐藏的窗口信息，用于恢复时还原窗口状态
    struct HiddenWindow {
        HWND hwnd;                  // 窗口句柄
        int showCmd;                // 隐藏前的窗口显示状态（正常/最小化/最大化）
        DWORD pid;                  // 进程ID
        // 枚举时缓存的标题与进程名：写恢复文件时直接复用，
        // 避免对每个窗口再次执行跨进程 WM_GETTEXT / OpenProcess
        std::wstring title;
        std::wstring processName;
        // 隐藏请求已异步投递但尚未确认生效：窗口当前可见时也不能当作
        // “从未隐藏/已经恢复”，否则迟到的 SW_HIDE 会让它失去恢复记录（R15-M03）
        bool hidePending = false;
    };
    std::vector<HiddenWindow> m_hiddenWindows; // 被隐藏的窗口列表
    // 上一轮启动未能恢复的遗留记录（原样保存的 v2/v3/v4 行）。
    // 本会话任何 ClearRecoverFile 都必须保留它们，避免“没有可隐藏窗口”的
    // 一次切换把遗留窗口的恢复线索删除；后续保存时与新记录合并写回
    std::wstring m_legacyRecoverContent;
    // 启动时存在无法读取/异常膨胀的恢复文件：保留该文件供人工检查或提权处理，
    // 本会话的保存/清理不得删除它（R15-M05）
    bool m_recoverHasUnreadableFile = false;
    std::vector<BoundWindowInfo> m_boundWindows; // 绑定的窗口匹配规则
    // 本程序静音过的输出端点。隐藏期间默认设备可能被切换，
    // 每个端点单独保存“静音前状态”，恢复时按设备 ID 逐个还原；
    // 同一端点重复静音时保留最早快照，不能被“本程序造成的静音”覆盖。
    struct MutedEndpoint {
        std::wstring deviceId;  // 空表示无法获取设备 ID，退回当前默认端点
        bool wasMuted = false;  // 静音前该端点是否已静音
        bool valid = false;     // wasMuted 是否可信（GetMute 成功）
    };
    std::vector<MutedEndpoint> m_mutedEndpoints;
    bool m_mutedByUs = false;       // 本次静音是否由本程序发起（据此决定恢复时是否解除静音）
    // 启动恢复时因音频设备暂不可用未能解除静音：由定时器静默重试
    bool m_muteRestorePending = false;
    ULONGLONG m_lastDeactivateTick = 0; // 上次停用的时间戳

    HookThread m_mouseHookThread;   // 鼠标钩子专用线程：避免 UI 阻塞导致钩子被移除
    // 保护钩子线程与 UI 线程共享的鼠标触发设置（UI 侧只做短赋值）
    std::recursive_mutex m_mouseStateMutex;
    HWND m_mouseNotifyWnd = nullptr; // 鼠标事件通知窗口
    bool m_middleBtnEnabled = false; // 中键触发老板键
    bool m_sideBtn1Enabled = false;  // 侧键1触发老板键
    bool m_sideBtn2Enabled = false;  // 侧键2触发老板键
    // 缓存的音频设备枚举器：老板键可被鼠标中键/侧键频繁切换，
    // 复用实例避免每次隐藏/恢复都 CoCreateInstance
    IMMDeviceEnumerator* m_pEnumerator = nullptr;
    // 缓存的音量接口：按默认输出设备 ID 复用，设备变化时重建
    IAudioEndpointVolume* m_pVolume = nullptr;
    std::wstring m_volumeDeviceId;  // m_pVolume 对应的默认输出设备 ID
    // 单例指针供钩子线程回调使用：UI 线程在构造/析构时写入，必须用原子类型，
    // 避免 Stop 超时放弃线程时与回调读取形成数据竞争（R9-A8）
    static std::atomic<BossKey*> s_instance;
};
