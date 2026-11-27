#pragma once
#include <Windows.h>
#include <atomic>
#include <memory>
#include <vector>
#include <utility>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include "Config.h"
#include "HookThread.h"

// 映射按键抬起补发持续失败（UIPI/安全软件拦截）：通知主窗口提示一次
inline constexpr UINT WM_KEY_REMAP_RELEASE_FAILED = WM_APP + 121;

// ============================================================
// KeyboardHook: 低级键盘钩子管理器
// 通过 WH_KEYBOARD_LL 钩子拦截全局键盘事件，实现以下功能：
//   - 屏蔽指定按键
//   - 禁用 NumLock 键
//   - 强制 Insert 模式（屏蔽 Insert 键）
//   - 按键重映射
//   - 锁键状态浮层通知
//   - 静音状态浮层通知
// ============================================================
class KeyboardHook {
public:
    KeyboardHook();
    ~KeyboardHook();

    // 安装低级键盘钩子，notifyWnd 为接收通知消息的窗口
    bool Install(HWND notifyWnd);
    // 卸载键盘钩子
    void Uninstall();
    // 钩子探活：Windows 在回调超时后会静默移除钩子，且 SetWindowsHookEx
    // 的返回值无法反映这一状态。由 UI 定时器周期调用，必要时自动重装。
    bool EnsureAlive();

    // 设置被禁用的按键列表
    void SetDisabledKeys(const std::vector<int>& codes);
    // 启用/禁用指定按键屏蔽功能
    void SetDisableSpecifiedKeysEnabled(bool enabled);
    // 启用/禁用 NumLock 键屏蔽
    void SetDisableNumLock(bool disable);
    // 启用/禁用锁键状态浮层通知
    void SetShowOverlay(bool show);
    // 启用/禁用静音状态浮层通知（与锁键浮层相互独立）
    void SetShowMuteOverlay(bool show);
    // 启用/禁用按键重映射
    void SetKeyRemapEnabled(bool enabled);
    // 设置按键重映射表：源键 -> 单键或组合键（mods 为 0 表示单键）
    void SetKeyRemappings(const std::vector<KeyRemapEntry>& remappings);
    // 标记当前正在模拟输入（避免钩子递归拦截自己发送的按键）
    void SetSimulatingInput(bool sim);
    // 启用/禁用热键捕获模式（捕获模式下不拦截任何按键）
    void SetCaptureMode(bool enabled);

private:
    // 低级键盘钩子回调函数
    static LRESULT CALLBACK LowLevelProc(int nCode, WPARAM wParam, LPARAM lParam);

    // 已注入且仍处于按下状态的重映射记录。
    // injectedMods 记录为凑出目标组合而注入的修饰键，抬起时需精确释放，
    // 避免用户按住源键期间修改/关闭映射或退出程序后修饰键卡住。
    struct ActiveRemap {
        int sourceVK = 0;
        int targetVK = 0;
        UINT injectedMods = 0;
    };

    // 钩子回调需要的全部运行状态。回调与安装/卸载/放弃清理闭包共享该对象，
    // 只捕获 shared_ptr、不捕获宿主 this：停止超时放弃线程后宿主可能已析构，
    // 线程仍能安全补发按键释放并完成卸载（R15-H01/H02）。
    struct SharedState {
        // 保护回调与 UI 线程共享的配置/状态。UI 侧只做短赋值，
        // 不会在持有锁时执行跨进程等待；递归锁允许回调内 SendInput 重入。
        std::recursive_mutex mutex;
        // 通知窗口用原子读取：UI 线程更新时无需拿锁，避免被卡住的回调阻塞
        std::atomic<HWND> notifyWnd{ nullptr };
        std::vector<int> disabledKeys;      // 被禁用的按键虚拟码列表（保留供 UI/序列化）
        std::unordered_set<int> disabledKeySet; // 钩子回调 O(1) 查询
        // 已被本钩子吞掉 keydown、尚未收到 keyup 的按键。
        // 只在配对存在时吞 keyup：若按键在按住期间被移出禁用列表或功能被关闭，
        // 放行 keyup，避免前台程序收到"只有按下没有抬起"的卡键（R8-M2）
        std::unordered_set<int> suppressedKeyDowns;
        std::vector<KeyRemapEntry> remappings; // 按键重映射表
        std::unordered_map<int, KeyRemapEntry> remapBySource; // 源键 -> 映射（O(1) 查询）
        bool disableSpecifiedKeysEnabled = false; // 是否启用指定按键屏蔽
        bool disableNumLock = false;     // 是否禁用 NumLock 键
        bool showOverlay = true;         // 是否显示锁键状态浮层
        bool showMuteOverlay = false;    // 是否显示静音状态浮层
        bool keyRemapEnabled = false;    // 是否启用按键重映射
        bool simulatingInput = false;    // 正在模拟输入（用于防止递归拦截）
        bool captureMode = false;        // 热键捕获模式（不拦截任何按键）
        std::vector<ActiveRemap> activeRemaps;
        // 抬起补发失败标记：UIPI/安全桌面会临时拒绝 SendInput。
        // 待释放键先入队，实际 SendInput 由持有锁之外的线程执行，
        // 避免 UI 线程在持锁时注入按键、拖住钩子回调导致系统移除钩子。
        bool pendingRemapRelease = false;
        ULONGLONG lastRemapRetryTick = 0;
        std::vector<int> pendingReleaseKeys; // 待补发 keyup 的虚拟键码（去重）
        ULONGLONG releaseSeq = 0;            // 待释放队列版本：发送成功时只清理同一版本
        // 锁键/静音键连发抑制：只在物理按下的第一次 keydown 通知，keyup 时清除标志。
        // 锁键被按住时系统会持续产生 keydown 连发消息，若不抑制会高频触发浮层
        // 置顶重排与重绘，导致浮层闪烁并使任务栏/DWM 卡顿。
        bool lockKeyHeld[3] = { false, false, false }; // Caps/Num/Scroll 是否已按下
        ULONGLONG lockKeyHeldTick[3] = { 0, 0, 0 };    // 按下时刻（超时兜底，防止标志卡住）
        bool muteKeyHeld = false;
        ULONGLONG muteKeyHeldTick = 0;
        // 探活心跳：回调线程更新，UI 线程只读取
        std::atomic<ULONGLONG> lastHookTick{ 0 };
        // 回调序号：每收到一次键盘事件递增。探针是否被钩子接收只用序号判断，
        // 不能用 GetTickCount64 时间戳——系统 tick 约 15.6ms，探针与回调落在
        // 同一 tick 时时间戳相等，会被误判为“钩子已丢失”而触发无谓重装
        std::atomic<ULONGLONG> callbackSeq{ 0 };
        // 模拟按键期间是否观察到物理 Ctrl/Shift 抬起：用于避免用户中途松开后
        // 仍补发 keydown 导致修饰键卡住
        std::atomic<bool> simCtrlPhysUp{ false };
        std::atomic<bool> simShiftPhysUp{ false };
    };

    // 钩子线程与 UI 线程共享的状态对象；线程侧持有自己的 shared_ptr 副本
    std::shared_ptr<SharedState> m_state;
    HookThread m_hookThread;           // 专用钩子线程：回调不再受 UI 阻塞影响

    // 用户是否期望钩子处于安装状态：自动恢复失败后仍据此继续重试（R15-M01）。
    // 仅在 UI 线程读写
    bool m_installDesired = false;
    // Stop 超时后不能在 UI 线程等待可能仍被回调持有的状态锁：
    // 延迟到下一次确认旧线程已退出的成功安装后再复位连发状态（R15-H01）
    bool m_pendingHeldReset = false;

    // 探活状态：UI 线程独占
    bool m_probePending = false;
    ULONGLONG m_probeStartSeq = 0;   // 探针发出前的回调序号，用于确认探针是否被接收
    ULONGLONG m_lastProbeTick = 0;   // 上次注入探针的时间（探针节流，避免鼠标活动导致频繁注入）
    // 重装失败后的冷却时间：避免钩子线程异常时每个探活周期都阻塞 UI 5 秒
    ULONGLONG m_nextHookReinstallTick = 0;
    // 待补发按键释放的独立重试退避（UI 线程独占，R15-H03）
    ULONGLONG m_remapRetryBackoffMs = 0;
    bool m_remapReleaseWarned = false;

    // 把待释放的目标键/修饰键加入待发送队列并清空 activeRemaps。
    // Locked 版本要求调用方已持有 state.mutex，且不会执行 SendInput。
    static void ReleaseRemappedKeysLocked(SharedState& state);
    // 在锁外发送待释放队列；成功且队列未被并发追加时清空。
    static void SendPendingRemapReleaseInState(SharedState& state);
    // 实例包装：由 UI 线程/探活定时器在解锁后调用
    void SendPendingRemapRelease();
    // 模拟按键开始：原子地进入模拟态并复位物理修饰键抬起跟踪。
    // 与读取 heldAgg 配合，避免“读取后立即松开”导致后续补发 keydown 卡键。
public:
    void BeginSimulationTracking();
    bool WasControlReleasedDuringSimulation() const;
    bool WasShiftReleasedDuringSimulation() const;
private:
    // 卸载/放弃路径：入队后在锁外发送（只依赖共享状态，不访问宿主）
    static void ReleaseRemappedKeysInState(SharedState& state);
    // 模拟输入旁路期间处理物理源键的 keyup：释放该源键注入的目标键/修饰键。
    // 返回 true 表示该 keyup 属于已激活映射、已被消费；调用方已持有 state.mutex
    static bool HandleSimulatedRemapKeyUpLocked(SharedState& state, int vk);

    // 锁键虚拟码 -> 下标：CAPITAL->0, NUMLOCK->1, SCROLL->2，其他返回 -1
    static int LockKeyIndex(UINT vk);
    // 复位所有「按键已按下」连发抑制标志（捕获模式开始、钩子卸载时调用）
    void ResetHeldInputState();
    // 配置变更时放弃“当前物理按住”按键的 keyup 配对：
    // 首次 keydown 可能已交给前台，若继续吞掉 keyup 会让前台卡键（R9-M7）。
    // 调用方必须已持有 state.mutex
    static void DropHeldSuppressedKeysLocked(SharedState& state);
    // 由探活定时器驱动的独立重试：不依赖后续按键事件，且用 try_lock 保证有界
    void RetryPendingRemapRelease();

    // 单例指针供钩子线程回调使用：UI 线程在构造/析构时写入，必须用原子类型，
    // 避免 Stop 超时放弃线程时与回调读取形成数据竞争（R9-A8）
    static std::atomic<KeyboardHook*> s_instance;
};
