#include "KeyboardHook.h"
#include "HookLogic.h"
#include "Resource.h"
#include "Utils.h"
#include <algorithm>

std::atomic<KeyboardHook*> KeyboardHook::s_instance{ nullptr };

// 锁键「已按下」标志的超时兜底时长：若超过该时长仍未收到 keyup（例如 keyup 丢失），
// 视为标志失效并允许重新通知，避免按一次之后再也收不到浮层。
static const ULONGLONG LOCK_KEY_HOLD_TIMEOUT_MS = 3000;

// 钩子心跳超时：超过该时长没有回调且系统确有输入时，注入探针确认钩子是否仍存活。
// 取 60 秒，避免用户长时间只用鼠标时误判。
static constexpr ULONGLONG HOOK_HEARTBEAT_TIMEOUT_MS = 60000;
// 探针最小间隔：鼠标活动会持续刷新系统输入时间，但钩子心跳只由键盘事件更新。
// 用户长时间只用鼠标时，探针不能按心跳超时周期反复注入（每分钟一次），
// 这里限制为最多每 5 分钟一次无副作用探针（R8-M8）
static constexpr ULONGLONG HOOK_PROBE_MIN_INTERVAL_MS = 5 * 60 * 1000;
// 钩子探活重装时等待旧线程退出的上限：UI 线程不能为异常线程阻塞 5 秒，
// 超时后放弃旧线程（其状态由线程自身持有），继续尝试安装新钩子（R8-M8）
static constexpr DWORD HOOK_REINSTALL_STOP_TIMEOUT_MS = 1500;

// 修饰键位（MOD_*） -> 实际虚拟键码统一由 HookLogic.h 提供，
// 便于释放序列的纯逻辑测试（R15-A01）

// 构造一个键盘输入事件
static INPUT MakeKeyInput(int vk, bool keyUp) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = (WORD)vk;
    input.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    input.ki.dwFlags = keyUp ? KEYEVENTF_KEYUP : 0;
    if (NeedsExtendedKeyFlag(vk)) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    return input;
}

// 回滚一次 SendInput 中已实际插入的按下事件（逆序补 keyup）。
// SendInput 返回实际插入数量，前 inserted 项即已生效的部分；部分失败时
// 若不回滚目标键，它可能永久保持按下状态。
static void RollbackInjectedPrefix(const INPUT* inputs, UINT total, UINT inserted) {
    if (inserted > total) inserted = total;
    std::vector<INPUT> releases;
    releases.reserve(inserted);
    for (UINT i = 0; i < inserted; ++i) {
        if (inputs[i].type != INPUT_KEYBOARD) continue;
        if (inputs[i].ki.dwFlags & KEYEVENTF_KEYUP) continue;
        INPUT up = inputs[i];
        up.ki.dwFlags |= KEYEVENTF_KEYUP;
        releases.push_back(up);
    }
    for (size_t i = releases.size(); i > 0; --i) {
        SendInput(1, &releases[i - 1], sizeof(INPUT));
    }
}

namespace {
    // 回调入口闸门与在途计数（R14-M2）：
    // 析构时先关闸并置空 s_instance，再等待在途回调退出，之后才析构宿主成员。
    // 回调顺序为「读闸门 → 计数+1 → 读 s_instance」，析构顺序为
    // 「关闸门 → 置空 s_instance → 读计数」，两者配合可保证：
    // 回调要么看到空的 s_instance，要么被计数拦住，不会访问已析构宿主。
    std::atomic<bool> s_callbackGate{ true };
    std::atomic<int> s_callbackCount{ 0 };
    // 钩子句柄放在对象外：宿主析构后回调仍可能调用 CallNextHookEx，
    // 不能通过 this 读取句柄（R14-M2）
    std::atomic<HHOOK> s_hookHandle{ nullptr };
}

KeyboardHook::KeyboardHook()
    : m_state(std::make_shared<SharedState>()) {
    s_callbackGate.store(true, std::memory_order_release);
    s_callbackCount.store(0, std::memory_order_release);
    s_hookHandle.store(nullptr, std::memory_order_release);
    s_instance.store(this, std::memory_order_release);
}

KeyboardHook::~KeyboardHook() {
    // 先关闭回调入口并置空单例，等待在途回调离开，再停止钩子线程（R14-M2）
    s_callbackGate.store(false, std::memory_order_release);
    s_instance.store(nullptr, std::memory_order_release);
    for (int i = 0; i < 2000 && s_callbackCount.load(std::memory_order_acquire) > 0; ++i) {
        Sleep(1);
    }
    Uninstall();
}

// 锁键虚拟码 -> 下标（配合 SharedState::lockKeyHeld / lockKeyHeldTick 使用）
int KeyboardHook::LockKeyIndex(UINT vk) {
    switch (vk) {
    case VK_CAPITAL: return 0;
    case VK_NUMLOCK: return 1;
    case VK_SCROLL:  return 2;
    default:         return -1;
    }
}

// 复位所有连发抑制标志：捕获模式开始时用户可能正按住某键，
// 钩子卸载后重新安装也应视为「全新状态」，避免沿用陈旧标志而漏发通知
void KeyboardHook::ResetHeldInputState() {
    std::lock_guard<std::recursive_mutex> lk(m_state->mutex);
    SharedState& s = *m_state;
    for (int i = 0; i < 3; i++) {
        s.lockKeyHeld[i] = false;
        s.lockKeyHeldTick[i] = 0;
    }
    s.muteKeyHeld = false;
    s.muteKeyHeldTick = 0;
    // 捕获/卸载后不再跟踪此前的禁用 keydown：避免旧记录吞掉后续无关的 keyup
    s.suppressedKeyDowns.clear();
}

// 模拟输入/捕获旁路期间处理物理源键的 keyup。
// 该路径不会进入正常的 keyup 分支，若不在这里释放，映射注入的目标键与
// 修饰键会一直保持按下，直到改配置或退出时才被统一释放。
// 调用方必须已持有 state.mutex
bool KeyboardHook::HandleSimulatedRemapKeyUpLocked(SharedState& s, int vk) {
    if (!s.keyRemapEnabled || s.activeRemaps.empty()) return false;

    // 释放序列由 HookLogic 的纯函数生成，便于单元测试覆盖
    // “目标键仍被其他源键按住/修饰键被共享”等边界（R15-A01）
    const SimulatedRemapReleasePlan plan =
        BuildSimulatedRemapReleasePlan(s.activeRemaps, vk);
    if (!plan.handled) return false;

    INPUT inputs[5] = {};
    UINT n = 0;
    for (int key : plan.keys) {
        if (n >= 5) break;   // 目标键 + 最多 4 个修饰键
        inputs[n++] = MakeKeyInput(key, true);
    }
    const bool injected = (n == 0) || (SendInput(n, inputs, sizeof(INPUT)) == n);
    if (injected) {
        s.activeRemaps.erase(s.activeRemaps.begin() +
                             static_cast<std::ptrdiff_t>(plan.index));
    } else {
        // 释放失败：转入待发送队列，由锁外的探活定时器按退避间隔重试。
        // 不能在钩子回调内同步 SendInput，避免回调阻塞被系统移除
        ReleaseRemappedKeysLocked(s);
    }
    // 但该 keyup 仍要消费，否则源键会以未映射状态泄漏给前台程序
    return true;
}

// 安装 WH_KEYBOARD_LL 低级键盘钩子
// 钩子安装到专用线程：钩子回调与 UI 线程解耦，UI 中的跨进程等待
// 不会让回调超过 LowLevelHooksTimeout（否则 Windows 7+ 会静默移除钩子）
bool KeyboardHook::Install(HWND notifyWnd) {
    // 记录“用户期望钩子工作”：即使本次安装失败，探活也会继续按冷却重试（R15-M01）
    m_installDesired = true;
    m_state->notifyWnd.store(notifyWnd, std::memory_order_release);
    if (m_hookThread.Installed()) return true;

    // 安装/卸载/放弃清理只捕获共享状态，不捕获宿主 this（R15-H01/H02）
    std::shared_ptr<SharedState> state = m_state;
    const bool installed = m_hookThread.Start(
        []() -> HHOOK {
            // 不捕获 this：启动超时被放弃后，线程仍可能执行到这里（R14-M2）
            HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelProc,
                                           GetModuleHandleW(nullptr), 0);
            s_hookHandle.store(hook, std::memory_order_release);
            return hook;
        },
        [state](HHOOK hook) {
            // 卸载前补发目标键 keyup：否则按住映射键时退出会把目标键留在按下状态
            ReleaseRemappedKeysInState(*state);
            UnhookWindowsHookEx(hook);
            s_hookHandle.store(nullptr, std::memory_order_release);
        },
        [state]() {
            // 放弃路径：宿主可能已析构，只使用共享状态补发 keyup（R15-H02）
            ReleaseRemappedKeysInState(*state);
        });
    if (installed) {
        m_state->lastHookTick.store(GetTickCount64(), std::memory_order_release);
        // Stop 超时后延迟的连发状态复位：此时旧线程已确认退出，可以安全拿锁
        if (m_pendingHeldReset) {
            ResetHeldInputState();
            m_pendingHeldReset = false;
        }
    }
    return installed;
}

// 卸载键盘钩子
void KeyboardHook::Uninstall() {
    // 用户明确要求卸载：探活不再自动重装（自动恢复失败的重试见 EnsureAlive）
    m_installDesired = false;
    // 钩子与回调都归专用线程：Stop 会在线程内补发 keyup 并卸载
    const bool stopped = m_hookThread.Stop();
    m_probePending = false;
    m_state->lastHookTick.store(0, std::memory_order_release);
    if (stopped) {
        ResetHeldInputState();
        m_pendingHeldReset = false;
    } else {
        // 线程被放弃，回调可能仍持有状态锁：不能在 UI 线程等待，
        // 延迟到下一次成功安装（旧线程已确认退出）再复位（R15-H01）
        m_pendingHeldReset = true;
    }
}

// 钩子探活：Windows 在回调超过 LowLevelHooksTimeout 后，或在 UAC 安全桌面、
// 锁屏、睡眠唤醒等场景下，会静默移除钩子；此时 m_installed 仍为 true，
// Install() 也不会重装。
// 若心跳超时且系统确有输入，先注入一个无副作用的 F24 keyup 作为探针；
// 探针发出后下一轮仍无新的回调序号，说明钩子已被移除，自动重装。
// 自动恢复成功后不再弹托盘提示：功能已恢复，提示只会打扰用户。
bool KeyboardHook::EnsureAlive() {
    // 待补发的按键释放不能只依赖“下一次按键”：卡键时用户可能已经停止输入。
    // 由探活动作定时驱动，并用 try_lock 保证 UI 线程有界（R15-H03）
    RetryPendingRemapRelease();

    if (!m_installDesired) return false;

    const ULONGLONG now = GetTickCount64();
    if (now < m_nextHookReinstallTick) return false;

    if (!m_hookThread.Installed()) {
        // 自动恢复中的 Stop/Install 失败会让线程上下文被清空；只要用户仍期望
        // 钩子工作，就继续按冷却重试，而不是永久停用（R15-M01）
        const HWND notifyWnd = m_state->notifyWnd.load(std::memory_order_acquire);
        const bool restored = Install(notifyWnd);
        if (restored) {
            m_nextHookReinstallTick = 0;
        } else {
            m_nextHookReinstallTick = now + 60000;
        }
        return restored;
    }

    const ULONGLONG last = m_state->lastHookTick.load(std::memory_order_acquire);
    if (now - last < HOOK_HEARTBEAT_TIMEOUT_MS) {
        m_probePending = false;
        return true;
    }

    // 自上次钩子回调以来没有系统输入：用户可能只是空闲，不发起探针。
    // GetLastInputInfo 使用 32 位 tick（约 49.7 天回绕），不能与 64 位心跳
    // 直接比较；统一换算成“距当前时刻的毫秒数”后再比较（R14-M4）
    LASTINPUTINFO inputInfo = { sizeof(inputInfo) };
    if (!GetLastInputInfo(&inputInfo)) return true;
    const DWORD now32 = GetTickCount();
    const DWORD inputAge = now32 - inputInfo.dwTime;
    const DWORD heartbeatAge = now32 - (DWORD)last;
    if (inputAge >= heartbeatAge) return true;

    if (m_probePending) {
        // 用回调序号而不是时间戳判断：GetTickCount64 的最小步进约为 15.6ms，
        // 探针与其回调落在同一 tick 时时间戳相等，会被误判为未收到，
        // 于是无谓地重装钩子并弹出“已自动恢复”提示
        const ULONGLONG seq = m_state->callbackSeq.load(std::memory_order_acquire);
        if (seq != m_probeStartSeq) {
            // 探针已被钩子回调接收，说明钩子仍存活
            m_probePending = false;
            m_state->lastHookTick.store(GetTickCount64(), std::memory_order_release);
            return true;
        }
        // 探针未触发回调：钩子已被系统移除，重装
        m_probePending = false;
        const HWND notifyWnd = m_state->notifyWnd.load(std::memory_order_acquire);
        // 有界等待：旧线程异常卡死时不再占用 UI 线程 5 秒
        const bool stopped = m_hookThread.Stop(HOOK_REINSTALL_STOP_TIMEOUT_MS);   // 卸载线程内会补发映射 keyup
        if (!stopped) {
            // 旧线程未能退出：不叠加新的钩子线程（避免两个钩子同时处理按键），
            // 冷却后下一轮再试
            m_nextHookReinstallTick = now + 60000;
            return false;
        }
        const bool restored = Install(notifyWnd);
        if (restored) {
            m_nextHookReinstallTick = 0;
        } else {
            // 钩子线程可能已卡死：冷却后再试，避免每 5 秒阻塞 UI 线程
            m_nextHookReinstallTick = now + 60000;
        }
        return restored;
    }

    // 探针节流：鼠标活动不等于键盘可用，但也不应因此周期性注入按键。
    // 最多每 5 分钟发一次探针；期间若键盘恢复输入，心跳会自然刷新（R8-M8）
    if (now - m_lastProbeTick < HOOK_PROBE_MIN_INTERVAL_MS) return true;

    // 发送无副作用的探针：F24 keyup 不改变按键状态，正常程序会忽略。
    // 注入被 UIPI 拒绝时无法用探针判断钩子状态，本轮直接放弃，避免误重装。
    // 必须先记录回调序号再注入：注入触发的回调可能在 SendInput 返回前执行，
    // 事后记录会把已递增的序号当作初始值，反而误判钩子丢失
    m_probeStartSeq = m_state->callbackSeq.load(std::memory_order_acquire);
    INPUT probe = {};
    probe.type = INPUT_KEYBOARD;
    probe.ki.wVk = VK_F24;
    probe.ki.wScan = (WORD)MapVirtualKeyW(VK_F24, MAPVK_VK_TO_VSC);
    probe.ki.dwFlags = KEYEVENTF_KEYUP;
    if (SendInput(1, &probe, sizeof(INPUT)) != 1) {
        m_probePending = false;
        return true;
    }
    m_probePending = true;
    m_lastProbeTick = now;
    return true;
}

// 把仍处于按下状态的重映射目标键与注入的修饰键加入待释放队列，并清空活动映射。
// 调用方必须已持有 state.mutex；本函数不执行 SendInput——UI 线程持锁注入会
// 拖住钩子回调（回调需要同一把锁），超过 LowLevelHooksTimeout 后系统会移除钩子。
void KeyboardHook::ReleaseRemappedKeysLocked(SharedState& s) {
    if (!s.activeRemaps.empty()) {
        // 释放序列由 HookLogic 的纯函数生成：去重目标键、按固定逆序释放修饰键
        const std::vector<int> keys = BuildRemapReleaseKeys(s.activeRemaps);
        for (int key : keys) {
            if (std::find(s.pendingReleaseKeys.begin(), s.pendingReleaseKeys.end(), key) ==
                s.pendingReleaseKeys.end()) {
                s.pendingReleaseKeys.push_back(key);
            }
        }
        s.activeRemaps.clear();
        ++s.releaseSeq;
    }
    s.pendingRemapRelease = !s.pendingReleaseKeys.empty();
}

// 在锁外发送待释放队列。发送成功后若队列版本未变化则清空；
// 期间若有新的释放请求入队，保留待下一轮补发（重复 keyup 幂等）。
void KeyboardHook::SendPendingRemapReleaseInState(SharedState& s) {
    std::vector<int> keys;
    ULONGLONG seq = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(s.mutex);
        if (!s.pendingRemapRelease || s.pendingReleaseKeys.empty()) {
            s.pendingRemapRelease = false;
            return;
        }
        keys = s.pendingReleaseKeys;
        seq = s.releaseSeq;
    }

    std::vector<INPUT> inputs;
    inputs.reserve(keys.size());
    for (int key : keys) {
        inputs.push_back(MakeKeyInput(key, true));
    }
    const UINT sent = inputs.empty()
        ? 0
        : SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));
    const bool ok = inputs.empty() || sent == inputs.size();

    std::lock_guard<std::recursive_mutex> lk(s.mutex);
    if (ok) {
        if (s.releaseSeq == seq) {
            s.pendingReleaseKeys.clear();
            s.pendingRemapRelease = false;
        } else {
            // 发送期间又有新的键加入：保留队列，下一轮补发（多发的 keyup 无害）
            s.pendingRemapRelease = true;
        }
    } else {
        // 释放失败（如前台为提权窗口被 UIPI 拦截）：保留队列，由探活定时器重试
        s.pendingRemapRelease = true;
        s.lastRemapRetryTick = GetTickCount64();
    }
}

void KeyboardHook::SendPendingRemapRelease() {
    SendPendingRemapReleaseInState(*m_state);
}

void KeyboardHook::ReleaseRemappedKeysInState(SharedState& s) {
    {
        std::lock_guard<std::recursive_mutex> lk(s.mutex);
        ReleaseRemappedKeysLocked(s);
    }
    // 卸载/放弃路径不在按键回调内，但仍统一在锁外发送，保持调用约定一致
    SendPendingRemapReleaseInState(s);
}

// 待补发按键释放的独立重试源：由探活定时器周期调用。
// 使用 try_lock 快速探测状态；实际 SendInput 在锁外执行（R15-H03 / M3）
void KeyboardHook::RetryPendingRemapRelease() {
    {
        std::unique_lock<std::recursive_mutex> lk(m_state->mutex, std::try_to_lock);
        if (!lk.owns_lock()) return;
        SharedState& s = *m_state;
        if (!s.pendingRemapRelease) {
            m_remapRetryBackoffMs = 0;
            m_remapReleaseWarned = false;
            return;
        }

        const ULONGLONG now = GetTickCount64();
        const ULONGLONG interval = m_remapRetryBackoffMs ? m_remapRetryBackoffMs : 1000;
        if (now - s.lastRemapRetryTick < interval) return;
    }

    // 先释放锁，再注入：钩子回调只会在状态锁定期间短暂等待
    SendPendingRemapRelease();
    std::lock_guard<std::recursive_mutex> lk(m_state->mutex);
    SharedState& s = *m_state;
    if (!s.pendingRemapRelease) {
        m_remapRetryBackoffMs = 0;
        m_remapReleaseWarned = false;
        return;
    }

    // 持续失败：指数退避，避免高频注入；只向用户提示一次
    m_remapRetryBackoffMs = m_remapRetryBackoffMs == 0
                          ? 2000
                          : std::min<ULONGLONG>(m_remapRetryBackoffMs * 2, 30000);
    if (!m_remapReleaseWarned) {
        m_remapReleaseWarned = true;
        const HWND wnd = s.notifyWnd.load(std::memory_order_acquire);
        if (wnd) PostMessageW(wnd, WM_KEY_REMAP_RELEASE_FAILED, 0, 0);
    }
}

void KeyboardHook::SetDisabledKeys(const std::vector<int>& codes) {
    std::lock_guard<std::recursive_mutex> lk(m_state->mutex);
    SharedState& s = *m_state;
    s.disabledKeys = codes;
    s.disabledKeySet.clear();
    for (int code : codes) s.disabledKeySet.insert(code);
    DropHeldSuppressedKeysLocked(s);
}

void KeyboardHook::SetDisableSpecifiedKeysEnabled(bool enabled) {
    std::lock_guard<std::recursive_mutex> lk(m_state->mutex);
    SharedState& s = *m_state;
    s.disableSpecifiedKeysEnabled = enabled;
    DropHeldSuppressedKeysLocked(s);
}

// 配置变更时对“当前仍物理按住”的键取消 keyup 配对：
// 这些键的首次 keydown 可能已放行给前台，继续吞掉 keyup 会造成按键卡住；
// 放行一次多余的 keyup 无害（R9-M7）。调用方已持有 state.mutex。
void KeyboardHook::DropHeldSuppressedKeysLocked(SharedState& s) {
    for (auto it = s.suppressedKeyDowns.begin(); it != s.suppressedKeyDowns.end();) {
        if (GetAsyncKeyState(*it) & 0x8000) {
            it = s.suppressedKeyDowns.erase(it);
        } else {
            ++it;
        }
    }
}

void KeyboardHook::SetDisableNumLock(bool disable) {
    std::lock_guard<std::recursive_mutex> lk(m_state->mutex);
    m_state->disableNumLock = disable;
}

void KeyboardHook::SetShowOverlay(bool show) {
    std::lock_guard<std::recursive_mutex> lk(m_state->mutex);
    m_state->showOverlay = show;
}

void KeyboardHook::SetShowMuteOverlay(bool show) {
    std::lock_guard<std::recursive_mutex> lk(m_state->mutex);
    m_state->showMuteOverlay = show;
}

void KeyboardHook::SetKeyRemapEnabled(bool enabled) {
    {
        std::lock_guard<std::recursive_mutex> lk(m_state->mutex);
        SharedState& s = *m_state;
        // 关闭映射前先释放已注入的目标键，避免按住映射键时关闭功能造成卡键
        if (s.keyRemapEnabled && !enabled) ReleaseRemappedKeysLocked(s);
        s.keyRemapEnabled = enabled;
    }
    // 锁外补发 keyup：UI 线程持锁注入会拖住钩子回调
    SendPendingRemapRelease();
}

void KeyboardHook::SetKeyRemappings(const std::vector<KeyRemapEntry>& remappings) {
    {
        std::lock_guard<std::recursive_mutex> lk(m_state->mutex);
        SharedState& s = *m_state;
        // 映射表变更前先释放旧映射产生的按键，避免按住映射键修改配置后卡键
        ReleaseRemappedKeysLocked(s);
        // 过滤非法键码（0/>=255 无法被 SendInput 识别）：源键被吞掉却模拟不出目标键
        // 会让该键彻底失效，且用户没有任何提示
        s.remappings.clear();
        for (const auto& r : remappings) {
            if (!ConfigUtils::IsValidVK(r.sourceVK) || !ConfigUtils::IsValidVK(r.targetVK)) continue;
            // 修饰键作源键会导致整个 Ctrl/Shift/Alt/Win 被拦截，普通映射直接丢弃；
            // 「按键互换」组（swapGroup > 0）明确允许互换修饰键
            if (ConfigUtils::IsModifierKey(r.sourceVK) && r.swapGroup == 0) continue;
            UINT mods = ConfigUtils::SanitizeMods((int)r.mods);
            if (mods == 0 && r.sourceVK == r.targetVK) continue;  // 无意义的自映射
            // 同一源键只保留第一条，避免行为不确定
            bool exists = false;
            for (const auto& existing : s.remappings) {
                if (existing.sourceVK == r.sourceVK) { exists = true; break; }
            }
            if (exists) continue;
            s.remappings.push_back({ r.sourceVK, mods, r.targetVK, r.swapGroup });
        }
        // 构建源键哈希表：钩子回调每次按键只查一次，避免线性扫描
        s.remapBySource.clear();
        for (const auto& r : s.remappings) s.remapBySource.emplace(r.sourceVK, r);
    }
    // 锁外补发旧映射的 keyup
    SendPendingRemapRelease();
}

void KeyboardHook::SetSimulatingInput(bool sim) {
    std::lock_guard<std::recursive_mutex> lk(m_state->mutex);
    m_state->simulatingInput = sim;
}

void KeyboardHook::BeginSimulationTracking() {
    // 与钩子回调共用状态锁：进入模拟态和清空跟踪标志必须原子完成，
    // 否则用户在这两步之间松开修饰键时不会被记录
    std::lock_guard<std::recursive_mutex> lk(m_state->mutex);
    m_state->simulatingInput = true;
    m_state->simCtrlPhysUp.store(false, std::memory_order_release);
    m_state->simShiftPhysUp.store(false, std::memory_order_release);
}

bool KeyboardHook::WasControlReleasedDuringSimulation() const {
    return m_state->simCtrlPhysUp.load(std::memory_order_acquire);
}

bool KeyboardHook::WasShiftReleasedDuringSimulation() const {
    return m_state->simShiftPhysUp.load(std::memory_order_acquire);
}

void KeyboardHook::SetCaptureMode(bool enabled) {
    {
        std::lock_guard<std::recursive_mutex> lk(m_state->mutex);
        SharedState& s = *m_state;
        s.captureMode = enabled;
        // 进入捕获模式时复位连发标志：捕获期间钩子放行所有按键（不更新标志），
        // 若不复位，退出捕获后可能沿用捕获前的旧标志而漏发一次通知
        if (enabled) {
            ResetHeldInputState();
            // 捕获期间不再重映射，需先释放已注入的目标键，避免目标键卡住
            ReleaseRemappedKeysLocked(s);
        }
    }
    // 锁外补发 keyup
    SendPendingRemapRelease();
}

// ============================================================
// KeyboardHook::LowLevelProc - 低级键盘钩子回调
// 处理优先级：
//   1. 模拟输入/重映射/捕获模式下直接放行
//   2. 按键重映射
//   3. NumLock 禁用处理
//   4. Insert 键禁用
//   5. 指定按键屏蔽
//   6. 锁键/静音状态浮层通知
// ============================================================
LRESULT CALLBACK KeyboardHook::LowLevelProc(int nCode, WPARAM wParam, LPARAM lParam) {
    // 析构已开始：直接放行，不再接触宿主成员（R14-M2）
    if (!s_callbackGate.load(std::memory_order_acquire)) {
        return CallNextHookEx(s_hookHandle.load(std::memory_order_acquire), nCode, wParam, lParam);
    }
    // 在途计数：析构函数会等待其归零后才销毁宿主成员
    struct CallbackScope {
        CallbackScope() { s_callbackCount.fetch_add(1, std::memory_order_acq_rel); }
        ~CallbackScope() { s_callbackCount.fetch_sub(1, std::memory_order_acq_rel); }
    } callbackScope;
    // 原子读取单例：UI 线程析构/停止超时放弃线程时不会与回调形成数据竞争（R9-A8）
    KeyboardHook* const inst = s_instance.load(std::memory_order_acquire);
    if (nCode == HC_ACTION && inst) {
        // 复制共享状态后只用它：即使宿主在停止超时后被析构，状态仍由本回调持有，
        // 后续不再访问宿主成员（R15-H01/H02）
        std::shared_ptr<SharedState> state = inst->m_state;
        if (!state) {
            return CallNextHookEx(s_hookHandle.load(std::memory_order_acquire), nCode, wParam, lParam);
        }
        SharedState& s = *state;
        // 心跳：UI 定时器据此判断钩子是否仍被系统接受
        s.lastHookTick.store(GetTickCount64(), std::memory_order_release);
        // 回调序号：探针存活判断只认序号，不受系统 tick 精度影响
        s.callbackSeq.fetch_add(1, std::memory_order_acq_rel);
        // 钩子在专用线程运行后，配置/状态与 UI 线程共享：整个回调持锁，
        // 保证读到的禁用键/映射表/标志始终一致。UI 侧已改为锁外注入按键，
        // 回调不再等待 UI 线程的 SendInput，显著降低 LowLevelHooksTimeout 风险。
        std::lock_guard<std::recursive_mutex> stateLock(s.mutex);
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        // 模拟输入期间仍需收尾物理源键的 keyup：否则映射注入的目标键/修饰键
        // 会一直保持按下（老板键 Toggle 等操作可能持续数百毫秒）
        if (s.simulatingInput) {
            // 记录模拟期间物理 Ctrl/Shift 是否被用户松开：模拟结束恢复修饰键时，
            // 已松开的键不再补发 keydown，避免卡键
            if (kb && !(kb->flags & LLKHF_INJECTED) &&
                (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)) {
                switch (kb->vkCode) {
                case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
                    s.simCtrlPhysUp.store(true, std::memory_order_release);
                    break;
                case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
                    s.simShiftPhysUp.store(true, std::memory_order_release);
                    break;
                default:
                    break;
                }
            }
            if (kb && !(kb->flags & LLKHF_INJECTED) &&
                (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) &&
                HandleSimulatedRemapKeyUpLocked(s, (int)kb->vkCode)) {
                return 1;   // 源键按下已被映射吞掉，抬起也一并消费
            }
            return CallNextHookEx(s_hookHandle.load(std::memory_order_acquire), nCode, wParam, lParam);
        }

        // 捕获模式下放行所有按键（用于设置对话框中的热键捕获）
        if (s.captureMode) {
            return CallNextHookEx(s_hookHandle.load(std::memory_order_acquire), nCode, wParam, lParam);
        }

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN ||
            wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {

            // --- 按键重映射 ---
            // 跳过注入按键，防止重映射的注入按键触发二次重映射（无限递归）
            if (s.keyRemapEnabled && !(kb->flags & LLKHF_INJECTED)) {
                const auto mit = s.remapBySource.find((int)kb->vkCode);
                if (mit != s.remapBySource.end()) {
                    const KeyRemapEntry& remap = mit->second;

                    bool isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
                    auto it = std::find_if(s.activeRemaps.begin(),
                                           s.activeRemaps.end(),
                                           [&](const ActiveRemap& a) {
                                               return a.sourceVK == remap.sourceVK;
                                           });

                    if (!isUp) {
                        // 首次按下：补齐目标组合所需且当前未被物理按下的修饰键；
                        // 连发 keydown：修饰键只注入一次，仅重发目标键
                        bool firstPress = (it == s.activeRemaps.end());
                        UINT injectedMods = firstPress ? 0 : it->injectedMods;
                        INPUT inputs[5] = {};
                        UINT n = 0;

                        if (firstPress) {
                            static const UINT modOrder[] = { MOD_CONTROL, MOD_SHIFT, MOD_ALT, MOD_WIN };
                            for (UINT mod : modOrder) {
                                if (!(remap.mods & mod)) continue;
                                int mvk = (int)HookModifierVK(mod);
                                if (mvk == 0) continue;
                                if (GetAsyncKeyState(mvk) & 0x8000) continue; // 已被物理按下
                                inputs[n++] = MakeKeyInput(mvk, false);
                                injectedMods |= mod;
                            }
                        }
                        inputs[n++] = MakeKeyInput(remap.targetVK, false);

                        // 注入可能失败（UIPI 拦截、安全桌面、输入被独占）：
                        // 失败时必须放行原键，否则该次按键会被彻底吞掉、表现为按键失灵
                        const UINT inserted = SendInput(n, inputs, sizeof(INPUT));
                        if (inserted != n) {
                            // 按实际插入数量精确回滚：部分成功时目标键可能已按下，
                            // 只释放修饰键会把目标键永久留在按下状态
                            RollbackInjectedPrefix(inputs, n, inserted);
                            if (!firstPress) {
                                // 首次按下已成功注入并被吞掉，本次只是自动重复失败。
                                // 若放行本次 keydown，随后的 keyup 仍会被映射分支消费，
                                // 前台会收到没有 keyup 的按键；保持吞掉以避免源键卡住。
                                return 1;
                            }
                            return CallNextHookEx(s_hookHandle.load(std::memory_order_acquire), nCode, wParam, lParam);
                        }
                        if (firstPress) {
                            // 记录注入的修饰键：抬起、改配置、关闭功能或退出时据此释放
                            s.activeRemaps.push_back(
                                { remap.sourceVK, remap.targetVK, injectedMods });
                        }
                        return 1; // 吞掉原始按键
                    }

                    // keyup：若首次 keydown 未能注入（无记录），放行原键，
                    // 避免原键 keydown 已放行而 keyup 被吞导致按键卡住
                    if (it == s.activeRemaps.end()) {
                        return CallNextHookEx(s_hookHandle.load(std::memory_order_acquire), nCode, wParam, lParam);
                    }

                    // 多个源键可能映射到同一目标键：仅当没有其他源键仍按住该
                    // 目标键时才发送 keyup，否则另一个源键按住期间目标键会失效
                    bool targetStillHeld = false;
                    UINT otherMods = 0;
                    for (const auto& active : s.activeRemaps) {
                        if (active.sourceVK == remap.sourceVK) continue;
                        if (active.targetVK == remap.targetVK) targetStillHeld = true;
                        otherMods |= active.injectedMods;
                    }

                    INPUT inputs[5] = {};
                    UINT n = 0;
                    if (!targetStillHeld) inputs[n++] = MakeKeyInput(remap.targetVK, true);
                    // 只释放本条目注入、且其他仍按住的条目不需要的修饰键
                    UINT releaseMods = it->injectedMods & ~otherMods;
                    static const UINT releaseOrder[] = { MOD_WIN, MOD_ALT, MOD_SHIFT, MOD_CONTROL };
                    for (UINT mod : releaseOrder) {
                        if (releaseMods & mod) {
                            inputs[n++] = MakeKeyInput((int)HookModifierVK(mod), true);
                        }
                    }
                    bool injected = (n == 0) || (SendInput(n, inputs, sizeof(INPUT)) == n);
                    if (injected) {
                        s.activeRemaps.erase(it);
                    } else {
                        // keyup 注入失败：转入待释放队列，由锁外的探活定时器重试；
                        // 不在按键回调内同步重试，避免回调阻塞超时
                        ReleaseRemappedKeysLocked(s);
                    }
                    return 1;
                }
            }

            // --- NumLock 禁用 ---
            // 屏蔽物理 NumLock 按键（return 1），注入按键放行
            // 关键：物理按键被屏蔽后，Windows 状态不变（仍为 ON），
            // 但键盘固件已在物理按键时切换为 OFF，导致小键盘发出方向键扫描码。
            // 解决：发送两次 keybd_event 切换（ON→OFF→ON），使 Windows 重新
            // 发送键盘指示器状态给键盘固件，将固件恢复为数字输入模式。
            if (s.disableNumLock && kb->vkCode == VK_NUMLOCK) {
                if (kb->flags & LLKHF_INJECTED) {
                    return CallNextHookEx(s_hookHandle.load(std::memory_order_acquire), nCode, wParam, lParam);
                }
                if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                    // 仅在物理按下的第一次 keydown 处理：连发时重复注入会造成
                    // 大量 keybd_event 与浮层通知，拖垮系统
                    int idx = LockKeyIndex(VK_NUMLOCK);
                    ULONGLONG now = GetTickCount64();
                    bool firstPress = !s.lockKeyHeld[idx] ||
                                      (now - s.lockKeyHeldTick[idx]) > LOCK_KEY_HOLD_TIMEOUT_MS;
                    if (firstPress) {
                        s.lockKeyHeld[idx] = true;
                        s.lockKeyHeldTick[idx] = now;
                        // 一次 SendInput 发送两次切换（ON→OFF→ON），
                        // 键盘固件收到 ON 指示；合并调用减少回调耗时
                        INPUT inputs[4] = {
                            MakeKeyInput(VK_NUMLOCK, false), MakeKeyInput(VK_NUMLOCK, true),
                            MakeKeyInput(VK_NUMLOCK, false), MakeKeyInput(VK_NUMLOCK, true)
                        };
                        SendInput(4, inputs, sizeof(INPUT));
                        // 显示 NumLock 已锁定的浮层提示（lp=3 表示已锁定且开启）
                        if (s.showOverlay) {
                            PostMessageW(s.notifyWnd.load(std::memory_order_acquire), WM_LOCKKEY_NOTIFY,
                                         (WPARAM)VK_NUMLOCK, 3);
                        }
                    }
                } else {
                    // keyup：清除按下标志，下次按下才会再次通知
                    s.lockKeyHeld[LockKeyIndex(VK_NUMLOCK)] = false;
                }
                return 1; // 屏蔽物理按键
            }

            // --- 指定按键屏蔽 ---
            // 跳过注入按键，防止重映射到被禁用按键的注入按键被错误屏蔽。
            // 仅吞 keydown；keyup 由下面的配对逻辑决定是否吞掉：
            // 按键被按住期间用户关闭屏蔽/把该键移出列表时，放行 keyup，
            // 否则前台程序会收到只有按下、没有抬起的按键，表现为卡键（R8-M2）
            if (!(kb->flags & LLKHF_INJECTED) &&
                (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
                if (s.disableSpecifiedKeysEnabled &&
                    s.disabledKeySet.count((int)kb->vkCode) != 0) {
                    s.suppressedKeyDowns.insert((int)kb->vkCode);
                    return 1;
                }
            }
            // keyup 配对：只有对应 keydown 确实被吞过才继续吞，避免配置变更后卡键
            if (!(kb->flags & LLKHF_INJECTED) &&
                (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)) {
                if (s.suppressedKeyDowns.erase((int)kb->vkCode) != 0) {
                    return 1;
                }
            }

            // --- 锁键/静音状态浮层通知 ---
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                // CapsLock / NumLock / ScrollLock 状态变化通知（受锁键浮层开关控制）
                if (s.showOverlay) {
                    int idx = LockKeyIndex(kb->vkCode);
                    // NumLock 禁用时已在上面的专用段发送通知，此处跳过避免双重发送
                    bool handledByNumLockBranch =
                        (kb->vkCode == VK_NUMLOCK && s.disableNumLock);
                    if (idx >= 0 && !handledByNumLockBranch) {
                        // 仅在物理按下的第一次 keydown 通知：连发时状态取反推算会失真，
                        // 且高频重绘会使浮层闪烁并拖垮任务栏/DWM
                        ULONGLONG now = GetTickCount64();
                        bool firstPress = !s.lockKeyHeld[idx] ||
                                          (now - s.lockKeyHeldTick[idx]) > LOCK_KEY_HOLD_TIMEOUT_MS;
                        if (firstPress) {
                            s.lockKeyHeld[idx] = true;
                            s.lockKeyHeldTick[idx] = now;
                            bool oldState = (GetKeyState((int)kb->vkCode) & 0x0001) != 0;
                            bool newState = !oldState;
                            PostMessageW(s.notifyWnd.load(std::memory_order_acquire), WM_LOCKKEY_NOTIFY,
                                         (WPARAM)kb->vkCode, (LPARAM)(newState ? 1 : 0));
                        }
                    }
                }
                // 静音键状态变化通知（受静音浮层开关控制，与锁键浮层解耦）
                // 仅在物理按下的第一次 keydown 通知：连发时 ShowMute 会反复
                // CoCreateInstance 枚举音频设备并重绘浮层，造成资源浪费与闪烁
                if (s.showMuteOverlay && kb->vkCode == VK_VOLUME_MUTE) {
                    ULONGLONG now = GetTickCount64();
                    bool firstPress = !s.muteKeyHeld ||
                                      (now - s.muteKeyHeldTick) > LOCK_KEY_HOLD_TIMEOUT_MS;
                    if (firstPress) {
                        s.muteKeyHeld = true;
                        s.muteKeyHeldTick = now;
                        PostMessageW(s.notifyWnd.load(std::memory_order_acquire), WM_MUTE_NOTIFY, 0, 0);
                    }
                }
            } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                // 锁键抬起：清除按下标志，使下一次按下能再次通知
                int idx = LockKeyIndex(kb->vkCode);
                if (idx >= 0) s.lockKeyHeld[idx] = false;
                // 静音键抬起：同样清除标志
                if (kb->vkCode == VK_VOLUME_MUTE) s.muteKeyHeld = false;
            }
        }
    }
    return CallNextHookEx(s_hookHandle.load(std::memory_order_acquire), nCode, wParam, lParam);
}
