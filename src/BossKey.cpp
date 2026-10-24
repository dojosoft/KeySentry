#include "BossKey.h"
#include "Resource.h"
#include "Utils.h"
#include <algorithm>
#include <string.h>
#include <unordered_set>
#include <ShlObj.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

BossKey* BossKey::s_instance = nullptr;

// 异步隐藏的延迟确认时长：给"短暂无响应"的目标进程留出恢复并处理隐藏请求的时间。
// 过短会漏判，过长会让老板键在这段时间内表现为"未激活"
static const UINT BOSSKEY_VERIFY_DELAY_MS = 800;

// ============================================================
// 跨进程窗口操作的非阻塞辅助
// 背景：这些操作运行在承载 WH_MOUSE_LL 鼠标钩子的 UI 线程上。
//       GetWindowTextW / ShowWindow 会向目标窗口线程同步发送消息，
//       目标进程（如浏览器）一时无响应时会把该线程卡住，导致低级
//       鼠标钩子无法派发，表现为"全系统鼠标点击无响应、键盘正常"。
// ============================================================
namespace {
    // 通知窗口取消内部模式（鼠标捕获、菜单/拖拽循环等），
    // 避免隐藏一个仍持有鼠标捕获的窗口后，鼠标输入被该窗口独占
    void CancelWindowModes(HWND hwnd, UINT timeoutMs = 50) {
        if (!IsWindow(hwnd)) return;
        DWORD_PTR result = 0;
        SendMessageTimeoutW(hwnd, WM_CANCELMODE, 0, 0,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, timeoutMs, &result);
    }

    // 安全地显示/隐藏窗口，返回 true 表示走了异步投递。
    // 对其他进程的窗口一律异步：ShowWindow 会同步等待目标线程处理消息，
    // 目标只需短暂繁忙（脚本、同步 IO、弹模态）就足以把承载低级键鼠钩子的
    // UI 线程一起挂住；IsHungAppWindow 只识别已无响应(≥5s)的窗口，不能依赖。
    bool SetWindowShowState(HWND hwnd, int showCmd) {
        if (!IsWindow(hwnd)) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId() || IsHungAppWindow(hwnd)) {
            return ShowWindowAsync(hwnd, showCmd) != FALSE;
        }
        ShowWindow(hwnd, showCmd);
        return false;
    }

    // 有界等待异步窗口操作生效：最多 100ms 且与窗口数量无关，
    // 既不长时间阻塞钩子线程，又能识别真正失败（如提权窗口无法隐藏）
    void WaitForVisibility(const std::vector<HWND>& hwnds, bool expectVisible) {
        for (int i = 0; i < 10; ++i) {
            bool settled = true;
            for (HWND h : hwnds) {
                if (!IsWindow(h)) continue;
                if ((IsWindowVisible(h) != FALSE) != expectVisible && !IsHungAppWindow(h)) {
                    settled = false;
                    break;
                }
            }
            if (settled) return;
            Sleep(10);
        }
    }

    // 关键系统进程不参与强制关闭，避免误绑 explorer.exe 等导致桌面/登录会话异常
    // cache：复用进程路径查询结果，避免同一 pid 在一次关闭流程中被反复 OpenProcess
    bool IsCriticalProcess(DWORD pid, ProcessUtils::PathCache& cache) {
        const std::wstring& name = cache.Get(pid).name;
        static const wchar_t* critical[] = {
            L"explorer.exe", L"winlogon.exe", L"csrss.exe", L"wininit.exe",
            L"services.exe", L"smss.exe", L"lsass.exe", L"dwm.exe",
            L"System", L"Registry"
        };
        for (auto n : critical) {
            if (_wcsicmp(name.c_str(), n) == 0) return true;
        }
        return false;
    }
}

BossKey::BossKey() { s_instance = this; }

BossKey::~BossKey() {
    UninstallMouseHook();
    Unregister();
    // 析构时恢复所有隐藏的窗口并取消静音（异常退出路径的兜底）；
    // 正常退出/关机路径已由 WM_DESTROY / WM_ENDSESSION 调用 Deactivate 覆盖
    if (m_active) Deactivate();
    // 释放缓存的音频设备枚举器（须在 Deactivate 内的 SetMute 之后）
    ReleaseAudio();
    s_instance = nullptr;
}

// 释放缓存的音频 COM 接口：正常退出时应在 CoUninitialize 之前调用
// （g_bossKey 是全局对象，其析构发生在 wWinMain 中的 CoInitGuard 之后，
//  届时再 Release 属于在已反初始化的线程上访问 COM 对象）
void BossKey::ReleaseAudio() {
    if (m_pVolume) { m_pVolume->Release(); m_pVolume = nullptr; }
    if (m_pEnumerator) { m_pEnumerator->Release(); m_pEnumerator = nullptr; }
    m_volumeDeviceId.clear();
    m_mutedDeviceId.clear();
}

// 注册老板键热键，先注销旧热键再注册新热键
bool BossKey::Register(HWND hwnd, int hotkeyId, UINT modifiers, UINT vk) {
    Unregister();
    m_hwnd = hwnd;
    m_hotkeyId = hotkeyId;
    m_modifiers = modifiers;
    m_vk = vk;
    m_registered = RegisterHotKey(hwnd, hotkeyId, modifiers, vk) != 0;
    return m_registered;
}

// 注销老板键热键
void BossKey::Unregister() {
    if (m_registered && m_hwnd) {
        UnregisterHotKey(m_hwnd, m_hotkeyId);
        m_registered = false;
    }
}

void BossKey::SetWindows(const std::vector<BoundWindowInfo>& windows) { m_boundWindows = windows; }
void BossKey::SetMuteEnabled(bool enabled) { m_muteEnabled = enabled; }
void BossKey::SetHideCurrentWindow(bool enabled) { m_hideCurrentWindow = enabled; }
void BossKey::SetSendPauseBeforeHide(bool enabled) { m_sendPauseBeforeHide = enabled; }

// EnumWindows 回调的上下文数据
struct EnumBossData {
    const std::vector<BoundWindowInfo>* boundWindows;
    std::vector<HWND>* results;
    ProcessUtils::PathCache* cache;  // 单次枚举内复用进程路径，避免同一 pid 重复 OpenProcess
};

// EnumWindows 回调：收集匹配绑定规则的可见窗口
static BOOL CALLBACK EnumBossWindowsProc(HWND hwnd, LPARAM lParam) {
    if (IsWindowVisible(hwnd) == FALSE) return TRUE;
    // 跳过任务栏/桌面等外壳窗口，避免误绑 explorer.exe 时把外壳一起隐藏
    if (WindowUtils::IsShellWindow(hwnd)) return TRUE;
    auto* data = reinterpret_cast<EnumBossData*>(lParam);

    for (const auto& info : *data->boundWindows) {
        // 使用带缓存的匹配：进程匹配模式下按 pid 复用路径查询结果
        if (info.Matches(hwnd, *data->cache)) {
            data->results->push_back(hwnd);
            return TRUE;
        }
    }
    return TRUE;
}

// ============================================================
// BossKey::HideWindows - 隐藏所有绑定窗口
// 流程：
//   1. 枚举所有匹配绑定规则的可见窗口
//   2. 可选：隐藏当前活动窗口
//   3. 可选：发送媒体暂停
//   4. 隐藏所有窗口（SW_HIDE）
//   5. 可选：静音系统音频
// ============================================================
void BossKey::HideWindows() {
    // 新一轮隐藏：作废上一次"异步隐藏延迟确认"，避免旧记录干扰本次判定
    if (m_pendingVerify) {
        m_pendingVerify = false;
        if (m_hwnd) KillTimer(m_hwnd, TIMER_BOSSKEY_VERIFY);
    }
    m_hiddenWindows.clear();

    // 枚举匹配绑定规则的窗口（枚举期间复用进程路径缓存）
    std::vector<HWND> matching;
    ProcessUtils::PathCache cache;
    EnumBossData data{ &m_boundWindows, &matching, &cache };
    EnumWindows(EnumBossWindowsProc, reinterpret_cast<LPARAM>(&data));

    // 保存窗口信息（显示状态、进程ID），用于恢复
    for (HWND hwnd : matching) {
        WINDOWPLACEMENT wp = { sizeof(wp) };
        GetWindowPlacement(hwnd, &wp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        m_hiddenWindows.push_back({ hwnd, (int)wp.showCmd, pid });
    }

    // 可选：隐藏当前前台窗口（排除自身进程与桌面/任务栏等外壳窗口）
    if (m_hideCurrentWindow) {
        HWND fg = GetForegroundWindow();
        if (fg) {
            DWORD fgPid = 0;
            GetWindowThreadProcessId(fg, &fgPid);
            if (fgPid == GetCurrentProcessId()) fg = nullptr;
            // 焦点在桌面(Progman)/任务栏(Shell_TrayWnd)时按老板键会隐藏外壳，
            // 导致桌面图标/任务栏消失，必须排除
            if (fg && WindowUtils::IsShellWindow(fg)) fg = nullptr;
            if (fg) {
                // 避免重复添加
                bool alreadyInList = false;
                for (const auto& hw : m_hiddenWindows) {
                    if (hw.hwnd == fg) { alreadyInList = true; break; }
                }
                if (!alreadyInList) {
                    WINDOWPLACEMENT wp = { sizeof(wp) };
                    GetWindowPlacement(fg, &wp);
                    m_hiddenWindows.push_back({ fg, (int)wp.showCmd, fgPid });
                }
            }
        }
    }

    // 隐藏所有窗口；跨进程窗口改用异步投递，避免目标短暂繁忙时拖住钩子线程
    std::vector<HWND> targets;
    targets.reserve(m_hiddenWindows.size());
    bool anyAsync = false;
    for (auto& hw : m_hiddenWindows) {
        if (!IsWindow(hw.hwnd)) continue;
        // 先释放目标窗口可能持有的鼠标捕获，避免隐藏后鼠标输入仍被
        // 无响应窗口独占
        CancelWindowModes(hw.hwnd);
        if (SetWindowShowState(hw.hwnd, SW_HIDE)) anyAsync = true;
        targets.push_back(hw.hwnd);
    }
    if (anyAsync) WaitForVisibility(targets, false);

    // 记录所有已投递隐藏请求的窗口（含异步尚未生效的），避免漏恢复导致
    // 窗口被隐藏后既不在列表里、也无法通过恢复文件找回
    std::vector<HiddenWindow> recorded;
    recorded.reserve(m_hiddenWindows.size());
    bool anyConfirmed = false;
    for (auto& hw : m_hiddenWindows) {
        if (!IsWindow(hw.hwnd)) continue;
        recorded.push_back(hw);
        // 仅以"确实不可见"作为隐藏成功的判据：IsHungAppWindow 只说明目标无响应，
        // 挂起的窗口往往仍然可见，误判为成功会导致窗口没藏起来却静音/暂停媒体
        if (IsWindowVisible(hw.hwnd) == FALSE) anyConfirmed = true;
    }
    m_hiddenWindows.swap(recorded);

    // 没有任何窗口可隐藏（如绑定程序未运行）：退出激活状态
    if (m_hiddenWindows.empty()) {
        m_active = false;
        ClearRecoverFile();
        return;
    }

    // 异步隐藏可能未在有界等待内生效：仍写入恢复记录，防止随后程序崩溃时
    // 窗口已隐藏却无法找回；但不暂停媒体、不静音，避免隐藏实际失败
    // （如提权窗口）时误操作系统状态
    if (!anyConfirmed) {
        // 没有任何窗口确认隐藏成功（目标挂起或受 UIPI 限制）：先复位激活状态。
        // 否则角落触发与闲置自动隐藏会被 IsActive() 长期抑制，
        // 托盘菜单还会显示"恢复隐藏窗口"，与实际状态不符
        m_active = false;
        if (anyAsync) {
            // 异步隐藏仍可能稍后生效：保留记录并安排延迟确认，
            // 生效时重新进入激活态，保证这些窗口仍可用老板键恢复
            SaveRecoverFile();
            m_pendingVerify = true;
            if (m_hwnd) SetTimer(m_hwnd, TIMER_BOSSKEY_VERIFY, BOSSKEY_VERIFY_DELAY_MS, nullptr);
        } else {
            m_hiddenWindows.clear();
            ClearRecoverFile();
        }
        return;
    }

    // 媒体暂停放在确认隐藏之后：没有可隐藏窗口时不再切换播放状态
    if (m_sendPauseBeforeHide) SendMediaPause();

    // 持久化被隐藏窗口的信息，用于程序异常退出后恢复
    SaveRecoverFile();

    // 可选：静音系统音频（仅当确有窗口被隐藏时才静音，避免无恢复契机地静音全系统）
    if (m_muteEnabled) SetMute(true);
}

// ============================================================
// BossKey::ShowWindows - 恢复所有隐藏窗口
// 按隐藏前的显示状态恢复，最大化的窗口同时置前
// ============================================================
void BossKey::ShowWindows() {
    // 用户已显式恢复：作废延迟确认，避免其随后把状态又改回"激活"
    if (m_pendingVerify) {
        m_pendingVerify = false;
        if (m_hwnd) KillTimer(m_hwnd, TIMER_BOSSKEY_VERIFY);
    }
    HWND lastForeground = nullptr;
    bool anyAsync = false;
    std::vector<HWND> targets;
    targets.reserve(m_hiddenWindows.size());
    for (auto& hw : m_hiddenWindows) {
        if (!IsWindow(hw.hwnd)) continue;
        // 校验进程 ID：句柄可能已被回收复用，避免恢复无关窗口
        DWORD wpid = 0;
        GetWindowThreadProcessId(hw.hwnd, &wpid);
        if (hw.pid != 0 && wpid != hw.pid) continue;
        // 窗口已可见（隐藏失败或用户已手动恢复）：保持现状，
        // 避免改动用户当前状态或抢走焦点
        if (IsWindowVisible(hw.hwnd)) continue;
        if (SetWindowShowState(hw.hwnd, hw.showCmd)) {
            anyAsync = true;
            targets.push_back(hw.hwnd);
        }
        // 记录最后一个需要前置的窗口，恢复完成后只前置它，避免依次抢焦点
        if (hw.showCmd == SW_SHOWMAXIMIZED || hw.showCmd == SW_SHOWNORMAL) {
            lastForeground = hw.hwnd;
        }
    }
    // 异步恢复时先有界等待窗口显现，否则 SetForegroundWindow 对隐藏窗口无效
    if (anyAsync) WaitForVisibility(targets, true);
    if (lastForeground && !IsHungAppWindow(lastForeground) &&
        IsWindowVisible(lastForeground)) {
        SetForegroundWindow(lastForeground);
    }

    // 逐项复核恢复结果：仍未显示出来的窗口（挂起、受 UIPI 限制、异步投递尚未完成）
    // 必须保留记录与恢复文件，否则这些窗口既无法再用老板键恢复，重启程序也找不回来
    std::vector<HiddenWindow> remaining;
    remaining.reserve(m_hiddenWindows.size());
    for (const auto& hw : m_hiddenWindows) {
        if (!IsWindow(hw.hwnd)) continue;               // 窗口已销毁，无需保留
        DWORD wpid = 0;
        GetWindowThreadProcessId(hw.hwnd, &wpid);
        if (hw.pid != 0 && wpid != hw.pid) continue;    // 句柄已被复用，记录作废
        if (IsWindowVisible(hw.hwnd)) continue;         // 已恢复成功
        remaining.push_back(hw);
    }
    m_hiddenWindows.swap(remaining);

    if (m_hiddenWindows.empty()) {
        // 全部恢复成功（或窗口已销毁）：退出激活状态并清除恢复文件
        m_active = false;
        ClearRecoverFile();
        return;
    }

    // 仍有窗口没显示出来：保持激活态，保留恢复记录，并安排延迟确认。
    // 稍后若确实显示成功，VerifyPendingHide 会清理记录与恢复文件
    m_active = true;
    SaveRecoverFile();
    m_pendingVerify = true;
    if (m_hwnd) SetTimer(m_hwnd, TIMER_BOSSKEY_VERIFY, BOSSKEY_VERIFY_DELAY_MS, nullptr);
}

// ============================================================
// BossKey::VerifyPendingHide - 延迟确认异步隐藏是否最终生效
// 场景：隐藏时所有目标都走异步投递且在有界等待内仍未隐藏，
//       此时先复位激活状态以免抑制角落触发/自动隐藏；
//       若稍后隐藏真正生效，则收回激活状态，保证仍可用老板键恢复。
// ============================================================
void BossKey::VerifyPendingHide() {
    if (!m_pendingVerify) return;
    m_pendingVerify = false;

    // 保留仍然隐藏的窗口记录：它们既可能来自"迟到的隐藏已生效"，
    // 也可能来自上一轮恢复失败。失效句柄与已恢复的窗口一并剔除
    std::vector<HiddenWindow> remaining;
    remaining.reserve(m_hiddenWindows.size());
    for (const auto& hw : m_hiddenWindows) {
        // 句柄与进程仍需有效：窗口已销毁或句柄被复用时不能据此判定"已隐藏"
        if (!IsWindow(hw.hwnd)) continue;
        DWORD wpid = 0;
        GetWindowThreadProcessId(hw.hwnd, &wpid);
        if (hw.pid != 0 && wpid != hw.pid) continue;
        if (!IsWindowVisible(hw.hwnd)) remaining.push_back(hw);
    }

    if (!remaining.empty()) {
        // 迟到的隐藏已生效（或恢复仍未完成）：重新进入激活态并刷新恢复文件，
        // 保证这些窗口仍能用老板键恢复
        m_hiddenWindows.swap(remaining);
        m_active = true;
        SaveRecoverFile();
        return;
    }

    // 没有任何窗口处于隐藏状态：清理残留记录与恢复文件，并退出激活态
    m_hiddenWindows.clear();
    m_active = false;
    ClearRecoverFile();
}

// ============================================================
// BossKey::SetMute - 通过 Windows Core Audio API 设置系统静音
//   mute=true: 记录当前静音状态后静音
//   mute=false: 恢复到隐藏前的静音状态（除非 forceUnmute）
// ============================================================
void BossKey::SetMute(bool mute, bool forceUnmute) {
    // COM 已在主线程 wWinMain 中初始化，无需重复初始化

    // 复用缓存的设备枚举器：老板键可由鼠标中键/侧键频繁切换，
    // 若每次都 CoCreateInstance 会产生大量 COM 实例创建开销
    if (!m_pEnumerator) {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                               __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&m_pEnumerator));
        if (FAILED(hr)) { m_pEnumerator = nullptr; return; }
    }

    IMMDevice* pDevice = nullptr;
    HRESULT hr = m_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    if (FAILED(hr)) return;

    // 读取默认输出设备 ID：设备变化（插拔耳机/切换 HDMI）后旧接口仍指向失效设备，
    // 写入会被静默忽略，必须按设备 ID 重建
    LPWSTR devId = nullptr;
    bool hasDeviceId = SUCCEEDED(pDevice->GetId(&devId)) && devId != nullptr;
    std::wstring deviceId = hasDeviceId ? devId : L"";
    if (devId) CoTaskMemFree(devId);

    if (!m_pVolume || (hasDeviceId && deviceId != m_volumeDeviceId)) {
        if (m_pVolume) { m_pVolume->Release(); m_pVolume = nullptr; }
        m_volumeDeviceId = deviceId;
        hr = pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                               nullptr, reinterpret_cast<void**>(&m_pVolume));
        if (FAILED(hr)) m_pVolume = nullptr;
    }
    pDevice->Release();
    if (!m_pVolume) return;

    if (mute) {
        // 记录静音前的状态与所属设备，恢复时使用
        BOOL wasMuted = FALSE;
        HRESULT getHr = m_pVolume->GetMute(&wasMuted);
        HRESULT setHr = m_pVolume->SetMute(TRUE, nullptr);
        // 静音没生效时不能标记为"本程序静音"：否则恢复阶段会去改一个
        // 本程序从未改过的系统状态
        if (FAILED(setHr)) {
            m_mutedByUs = false;
            m_wasMutedValid = false;
            return;
        }
        if (SUCCEEDED(getHr)) {
            m_wasMuted = wasMuted != FALSE;
            m_wasMutedValid = true;
        } else {
            // 读不到原状态：标记无效，恢复时按"取消静音"处理（宁可漏静音，
            // 也不能把系统永久静音）
            m_wasMuted = false;
            m_wasMutedValid = false;
        }
        m_mutedDeviceId = m_volumeDeviceId;
        m_mutedByUs = true;  // 标记由本程序发起静音，恢复时据此解除（不依赖 m_muteEnabled 当前值）
    } else {
        // 隐藏期间默认输出设备已变化：新设备从未被本程序静音，保持其当前状态，
        // 否则会把旧设备的静音记录套用到新设备上
        if (m_mutedDeviceId != m_volumeDeviceId) {
            m_mutedByUs = false;
            m_wasMutedValid = false;
            return;
        }
        // 恢复：如果之前不是静音状态则取消静音，或强制取消静音
        BOOL restoreMute = (!forceUnmute && m_wasMutedValid && m_wasMuted) ? TRUE : FALSE;
        m_pVolume->SetMute(restoreMute, nullptr);
        m_mutedByUs = false;
        m_wasMutedValid = false;
    }
}

// 发送媒体暂停/播放按键
void BossKey::SendMediaPause() {
    INPUT inp = {};
    inp.type = INPUT_KEYBOARD;
    inp.ki.wVk = VK_MEDIA_PLAY_PAUSE;
    inp.ki.wScan = (WORD)MapVirtualKeyW(VK_MEDIA_PLAY_PAUSE, MAPVK_VK_TO_VSC);
    // 按下失败（如被 UIPI 拦截）时不再补发抬起，避免留下卡键状态
    if (SendInput(1, &inp, sizeof(INPUT)) == 0) return;
    inp.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &inp, sizeof(INPUT));
}

void BossKey::Activate() {
    // 先结算未决的延迟确认：若上次的异步隐藏已迟到生效，本次按键应按"恢复"处理，
    // 否则那个窗口会一直留在隐藏状态且无法用老板键找回
    if (m_pendingVerify) {
        VerifyPendingHide();
        if (m_active) { Deactivate(); return; }
    }
    if (!m_active) { m_active = true; HideWindows(); }
}

// 停用老板键：恢复窗口、取消静音、记录停用时间
void BossKey::Deactivate() {
    if (!m_active) return;
    m_active = false;
    ShowWindows();
    // ShowWindows 若有窗口未能恢复，会把 m_active 重新置为 true 并保留恢复记录：
    // 此时窗口实际仍处于隐藏状态，不能解除静音、也不应消耗自动隐藏冷却，
    // 否则会出现"窗口还藏着、声音却已恢复"
    if (m_active) return;
    // 依据"是否由本程序静音"而非当前静音开关状态解除静音：
    // 隐藏期间用户若关闭"隐藏时静音"，此处仍应恢复，避免系统被永久静音
    if (m_mutedByUs) SetMute(false);
    m_lastDeactivateTick = GetTickCount64();
}

void BossKey::Toggle() { if (m_active) Deactivate(); else Activate(); }

// ============================================================
// BossKey::CloseBoundProcesses - 关闭所有绑定的进程
// 流程：
//   1. 收集所有隐藏窗口和可见匹配窗口的进程ID
//   2. 打开进程句柄
//   3. 发送 WM_CLOSE 优雅关闭
//   4. 等待进程退出（超时 PROCESS_CLOSE_TIMEOUT_MS）
//   5. 超时后强制终止（TerminateProcess）
// ============================================================
void BossKey::CloseBoundProcesses(DWORD timeoutMs) {
    std::unordered_set<DWORD> closedPids;
    // 进程路径缓存：本函数会多次判断同一 PID 是否为关键进程，缓存可避免重复 OpenProcess
    ProcessUtils::PathCache cache;

    // 收集隐藏窗口的进程ID
    for (auto& hw : m_hiddenWindows) {
        if (!IsWindow(hw.hwnd)) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(hw.hwnd, &pid);
        if (pid == 0 || pid == GetCurrentProcessId()) continue;
        // 关键系统进程（资源管理器/登录/音频等）不允许被强制关闭
        if (IsCriticalProcess(pid, cache)) continue;
        closedPids.insert(pid);
    }

    // 收集可见匹配窗口的进程ID（复用 EnumBossData 结构，同样复用进程路径缓存）
    std::vector<HWND> visibleMatching;
    EnumBossData cdata{ &m_boundWindows, &visibleMatching, &cache };
    EnumWindows(EnumBossWindowsProc, reinterpret_cast<LPARAM>(&cdata));

    // 先剔除自身进程与关键系统进程的窗口：后续 WM_CLOSE 直接遍历该列表，
    // 若只过滤 PID 集合而不剔除窗口，仍会向任务栏/桌面窗口发送关闭消息
    visibleMatching.erase(
        std::remove_if(visibleMatching.begin(), visibleMatching.end(), [&cache](HWND hwnd) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            return pid == 0 || pid == GetCurrentProcessId() || IsCriticalProcess(pid, cache);
        }),
        visibleMatching.end());

    for (HWND hwnd : visibleMatching) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == 0) continue;
        closedPids.insert(pid);
    }

    // 打开进程句柄，用于后续等待和强制终止
    struct ProcHandle { DWORD pid; HANDLE handle; };
    std::vector<ProcHandle> procHandles;
    for (DWORD pid : closedPids) {
        HANDLE hProc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
        if (hProc) procHandles.push_back({ pid, hProc });
    }

    // 先尝试优雅关闭（发送 WM_CLOSE）
    for (auto& hw : m_hiddenWindows) {
        if (!IsWindow(hw.hwnd)) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(hw.hwnd, &pid);
        if (IsCriticalProcess(pid, cache)) continue;
        PostMessageW(hw.hwnd, WM_CLOSE, 0, 0);
    }
    for (HWND hwnd : visibleMatching) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }

    // 等待进程退出（使用 WaitForMultipleObjects 替代轮询，减少 CPU 唤醒）
    // WaitForMultipleObjects 最多等待 MAXIMUM_WAIT_OBJECTS(64) 个对象，需分批处理
    // 关机路径（WM_ENDSESSION）传入较短超时，避免阻塞系统关机
    ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < timeoutMs) {
        // 收集仍存活的进程句柄
        std::vector<HANDLE> alive;
        for (auto& ph : procHandles) {
            if (WaitForSingleObject(ph.handle, 0) != WAIT_OBJECT_0) {
                alive.push_back(ph.handle);
            }
        }
        if (alive.empty()) break;
        // 分批等待，每批最多 MAXIMUM_WAIT_OBJECTS 个；
        // 每批按剩余预算重算超时，避免多批串行等待导致总时长超出 timeoutMs
        for (size_t batchStart = 0; batchStart < alive.size(); batchStart += MAXIMUM_WAIT_OBJECTS) {
            ULONGLONG remain = timeoutMs - (GetTickCount64() - start);
            DWORD batchWait = (DWORD)std::min((ULONGLONG)500, remain);
            if (batchWait == 0) break;
            size_t batchSize = std::min(alive.size() - batchStart, (size_t)MAXIMUM_WAIT_OBJECTS);
            WaitForMultipleObjects((DWORD)batchSize, &alive[batchStart], FALSE, batchWait);
        }
    }

    // 超时后强制终止未退出的进程
    for (auto& ph : procHandles) {
        if (WaitForSingleObject(ph.handle, 0) != WAIT_OBJECT_0) {
            TerminateProcess(ph.handle, 0);
        }
        CloseHandle(ph.handle);
    }

    m_hiddenWindows.clear();
    if (m_active) {
        m_active = false;
        if (m_mutedByUs) SetMute(false, true);
        // 记录停用时间，使自动隐藏冷却生效（与 Deactivate 保持一致）
        m_lastDeactivateTick = GetTickCount64();
    }
    // 绑定进程已关闭，清除恢复文件
    ClearRecoverFile();
}

// ============================================================
// 恢复文件机制：防止程序异常退出后被隐藏窗口永久失联
//   - HideWindows 后将被隐藏窗口信息（pid/显示状态/句柄/标题）写入
//     exe 同目录的 KeySentry.recover
//   - ShowWindows / CloseBoundProcesses 正常恢复后删除
//   - 下次启动时 RecoverOrphanedWindows 读取并恢复
// ============================================================
std::wstring BossKey::GetRecoverFilePath() {
    // 路径在进程生命周期内固定（exe 目录与可写性不变），缓存探测结果，
    // 避免每次创建/删除恢复文件都创建一个临时探测文件
    static const std::wstring cachedPath = []() -> std::wstring {
        // 动态缓冲取 exe 路径：固定 MAX_PATH 在长路径安装目录下会截断，
        // 进而把恢复文件写到错误目录
        std::wstring dir = ProcessUtils::GetModuleDir();

        // 目录策略与 INI 一致：优先 exe 同目录（便携模式）；
        // 目录只读（如 Program Files）时回退 %APPDATA%\KeySentry，
        // 避免恢复文件写入静默失败导致崩溃后无法恢复被隐藏的窗口
        std::wstring testPath = dir + L".~rtest.tmp";
        HANDLE hTest = CreateFileW(testPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (hTest != INVALID_HANDLE_VALUE) {
            CloseHandle(hTest);  // 关闭即自动删除测试文件
            return dir + L"KeySentry.recover";
        }

        std::wstring appData = ProcessUtils::GetAppDataDir();
        if (!appData.empty()) {
            std::wstring ad = appData + L"\\KeySentry";
            CreateDirectoryW(ad.c_str(), nullptr);
            return ad + L"\\KeySentry.recover";
        }
        return dir + L"KeySentry.recover";  // 获取 AppData 失败时仍返回原路径
    }();
    return cachedPath;
}

// 取进程可执行文件名（不含目录）：用于恢复记录的二次校验，
// 避免重启后 PID 被复用而误显示同标题的无关窗口
static std::wstring GetProcessBaseName(DWORD pid) {
    return ProcessUtils::GetProcessName(pid);
}

void BossKey::SaveRecoverFile() {
    if (m_hiddenWindows.empty()) {
        ClearRecoverFile();
        return;
    }

    // 每行格式：v2 \t pid \t showCmd \t hwnd \t 进程名 \t 窗口标题
    // "v2" 前缀用于区分旧格式（pid \t showCmd \t hwnd \t 标题）；
    // 标题可含 \t，故进程名之后的内容整体视为标题
    std::wstring content;
    for (const auto& hw : m_hiddenWindows) {
        // 安全读取标题：隐藏动作发生在持钩子线程上，不能同步阻塞在无响应窗口
        std::wstring title = WindowUtils::GetTextSafe(hw.hwnd);
        // 标题可能包含换行（部分程序允许），会破坏按行解析的恢复文件，统一替换为空格
        for (auto& ch : title) {
            if (ch == L'\r' || ch == L'\n') ch = L' ';
        }
        std::wstring procName = GetProcessBaseName(hw.pid);
        // 用拼接而非固定缓冲区格式化：标题过长时 swprintf_s 会失败并丢弃整行记录
        wchar_t prefix[64] = {};
        swprintf_s(prefix, L"v2\t%lu\t%d\t%llu\t",
                   (unsigned long)hw.pid, hw.showCmd,
                   (unsigned long long)(uintptr_t)hw.hwnd);
        content += prefix;
        content += procName;
        content += L'\t';
        content += title;
        content += L'\n';
    }

    // 转为 UTF-8 写入文件（支持中文窗口标题与中文路径）
    int bytes = WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return;
    std::string utf8((size_t)bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, &utf8[0], bytes, nullptr, nullptr);

    // 原子写入：先写临时文件再重命名替换，避免崩溃时恢复文件被截断损坏
    std::wstring path = GetRecoverFilePath();
    std::wstring tmpPath = path + L".tmp";
    HANDLE hFile = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(hFile, utf8.data(), (DWORD)(bytes - 1), &written, nullptr);  // 不写结尾 NUL
    CloseHandle(hFile);
    // 重命名失败时清理临时文件，避免 .tmp 残留
    if (!MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmpPath.c_str());
    }
}

void BossKey::ClearRecoverFile() {
    DeleteFileW(GetRecoverFilePath().c_str());
}

// 枚举回调上下文：按进程 ID + 进程名 + 标题精确匹配查找窗口
struct RecoverFindCtx {
    DWORD pid;
    const std::wstring* title;
    const std::wstring* procName;  // 旧格式记录为空，跳过进程名校验
    int showCmd;                   // 记录的原显示状态，恢复时按原样显示
    bool found;
};

// 枚举回调：32 位下 GCC 的 lambda 无法隐式转换为 stdcall 的 WNDENUMPROC，
// 故用显式 CALLBACK 约定的命名函数
static BOOL CALLBACK RecoverFindWindowProc(HWND h, LPARAM lp) {
    auto* ctx = reinterpret_cast<RecoverFindCtx*>(lp);
    DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    if (p != ctx->pid) return TRUE;
    // 进程名校验：重启后 PID 可能被复用，仅凭 PID + 标题仍可能命中无关窗口
    if (!ctx->procName->empty()) {
        std::wstring name = GetProcessBaseName(p);
        if (_wcsicmp(name.c_str(), ctx->procName->c_str()) != 0) return TRUE;
    }
    // 安全读取标题：枚举可能遇到无响应进程，避免阻塞启动流程
    std::wstring t = WindowUtils::GetTextSafe(h);
    if (t.empty()) return TRUE;
    // 精确匹配标题（而非子串）：子串匹配可能命中标题更长的无关窗口并错误显示
    if (t == *ctx->title) {
        // 按记录的显示状态恢复（最大化/最小化），而不是一律普通显示
        if (!IsWindowVisible(h)) SetWindowShowState(h, ctx->showCmd);
        ctx->found = true;
        return FALSE;  // 找到即停止枚举
    }
    return TRUE;
}

// 恢复上次异常退出时被隐藏的窗口，返回成功恢复的窗口数
// 优先用记录的窗口句柄（同一次会话内仍有效），失效则按进程 ID + 标题匹配
int BossKey::RecoverOrphanedWindows() {
    HANDLE hFile = CreateFileW(GetRecoverFilePath().c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return 0;

    std::string utf8;
    char buf[4096];
    DWORD read = 0;
    while (ReadFile(hFile, buf, sizeof(buf), &read, nullptr) && read > 0) {
        utf8.append(buf, (size_t)read);
    }
    CloseHandle(hFile);

    // 恢复文件在完成全部恢复动作后才删除：若在恢复过程中程序再次异常退出，
    // 文件仍保留，下次启动可继续恢复，避免被隐藏的窗口永久失联
    if (utf8.empty()) { ClearRecoverFile(); return 0; }

    // UTF-8 转宽字符
    int wchars = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (wchars <= 0) { ClearRecoverFile(); return 0; }
    std::wstring content((size_t)wchars, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &content[0], wchars);

    int recovered = 0;
    size_t lineStart = 0;
    while (lineStart < content.size()) {
        size_t lineEnd = content.find(L'\n', lineStart);
        if (lineEnd == std::wstring::npos) lineEnd = content.size();
        std::wstring line = content.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;

        // 解析：v2 前缀为新格式（pid \t showCmd \t hwnd \t 进程名 \t 标题），
        // 其余按旧格式（pid \t showCmd \t hwnd \t 标题）兼容处理
        bool v2 = line.compare(0, 3, L"v2\t") == 0;
        size_t pos = v2 ? 3 : 0;
        size_t t1 = line.find(L'\t', pos);
        if (t1 == std::wstring::npos) continue;
        size_t t2 = line.find(L'\t', t1 + 1);
        if (t2 == std::wstring::npos) continue;
        size_t t3 = line.find(L'\t', t2 + 1);
        if (t3 == std::wstring::npos) continue;

        std::wstring procName;
        size_t titleStart = t3 + 1;
        if (v2) {
            size_t t4 = line.find(L'\t', t3 + 1);
            if (t4 == std::wstring::npos) continue;
            procName = line.substr(t3 + 1, t4 - t3 - 1);
            titleStart = t4 + 1;
        }

        DWORD pid = (DWORD)wcstoul(line.substr(pos, t1 - pos).c_str(), nullptr, 10);
        int showCmd = (int)wcstol(line.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 10);
        HWND hwnd = (HWND)(uintptr_t)wcstoull(line.substr(t2 + 1, t3 - t2 - 1).c_str(), nullptr, 10);
        std::wstring title = line.substr(titleStart);
        if (showCmd <= 0) showCmd = SW_SHOWNORMAL;

        // 路径 1：原窗口句柄仍有效（同一会话内程序崩溃后重启）
        bool done = false;
        if (hwnd && IsWindow(hwnd)) {
            DWORD wpid = 0;
            GetWindowThreadProcessId(hwnd, &wpid);
            if (wpid == pid) {
                if (!IsWindowVisible(hwnd)) SetWindowShowState(hwnd, showCmd);
                done = true;
            }
        }

        // 路径 2：句柄失效（跨会话/句柄被复用），按进程 ID + 进程名 + 标题精确匹配
        if (!done && !title.empty()) {
            RecoverFindCtx fctx{ pid, &title, &procName, showCmd, false };
            EnumWindows(RecoverFindWindowProc, reinterpret_cast<LPARAM>(&fctx));
            done = fctx.found;
        }

        if (done) recovered++;
    }

    // 恢复动作已全部完成，删除恢复文件
    ClearRecoverFile();
    return recovered;
}

// 安装 WH_MOUSE_LL 低级鼠标钩子
bool BossKey::InstallMouseHook(HWND notifyWnd) {
    if (m_mouseHook) return true;
    m_mouseNotifyWnd = notifyWnd;
    m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    return m_mouseHook != nullptr;
}

// 卸载鼠标钩子
void BossKey::UninstallMouseHook() {
    if (m_mouseHook) { UnhookWindowsHookEx(m_mouseHook); m_mouseHook = nullptr; }
}

// 更新鼠标按钮触发设置
void BossKey::UpdateMouseHookSettings(bool middleBtn, bool sideBtn1, bool sideBtn2) {
    m_middleBtnEnabled = middleBtn;
    m_sideBtn1Enabled = sideBtn1;
    m_sideBtn2Enabled = sideBtn2;
}

// ============================================================
// BossKey::MouseProc - 鼠标低级钩子回调
// 拦截中键和侧键点击，触发老板键切换
// 同时拦截对应的按键释放事件，避免事件传递到其他应用
// ============================================================
LRESULT CALLBACK BossKey::MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    // 忽略注入事件（远程桌面/自动化工具）：只有物理鼠标按键才应触发老板键，
    // 与键盘钩子对 LLKHF_INJECTED 的处理保持一致
    auto* msIn = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    // 设置对话框/热键捕获取键期间不响应老板键鼠标触发，避免用户在拾取按键
    // 或修改配置时被意外隐藏窗口
    if (nCode == HC_ACTION && s_instance && !g_suppressHotkeys &&
        msIn && !(msIn->flags & LLMHF_INJECTED)) {
        // 中键按下：触发老板键并吞掉事件
        if (wParam == WM_MBUTTONDOWN && s_instance->m_middleBtnEnabled && s_instance->m_mouseNotifyWnd) {
            PostMessageW(s_instance->m_mouseNotifyWnd, WM_BOSS_MOUSE_TOGGLE, 0, 0);
            return 1;
        }
        // 中键释放：吞掉事件（判空条件与按下分支保持一致，避免钩子已解绑时仍吞消息）
        if (wParam == WM_MBUTTONUP && s_instance->m_middleBtnEnabled && s_instance->m_mouseNotifyWnd) {
            return 1;
        }
        // 中键双击消息同样吞掉：否则双击时第二次按下会泄漏给应用（不重复触发切换）
        if (wParam == WM_MBUTTONDBLCLK && s_instance->m_middleBtnEnabled && s_instance->m_mouseNotifyWnd) {
            return 1;
        }
        // 侧键按下：检查是否为启用的侧键
        if (wParam == WM_XBUTTONDOWN && s_instance->m_mouseNotifyWnd) {
            auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            int button = HIWORD(ms->mouseData);
            if ((button == XBUTTON1 && s_instance->m_sideBtn1Enabled) ||
                (button == XBUTTON2 && s_instance->m_sideBtn2Enabled)) {
                PostMessageW(s_instance->m_mouseNotifyWnd, WM_BOSS_MOUSE_TOGGLE, 0, 0);
                return 1;
            }
        }
        // 侧键释放：吞掉事件
        if (wParam == WM_XBUTTONUP && s_instance->m_mouseNotifyWnd) {
            auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            int button = HIWORD(ms->mouseData);
            if ((button == XBUTTON1 && s_instance->m_sideBtn1Enabled) ||
                (button == XBUTTON2 && s_instance->m_sideBtn2Enabled)) {
                return 1;
            }
        }
        // 侧键双击消息：同样吞掉，避免事件泄漏到其他应用（不重复触发切换）
        if (wParam == WM_XBUTTONDBLCLK && s_instance->m_mouseNotifyWnd) {
            auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            int button = HIWORD(ms->mouseData);
            if ((button == XBUTTON1 && s_instance->m_sideBtn1Enabled) ||
                (button == XBUTTON2 && s_instance->m_sideBtn2Enabled)) {
                return 1;
            }
        }
    }
    return CallNextHookEx(s_instance ? s_instance->m_mouseHook : nullptr, nCode, wParam, lParam);
}
