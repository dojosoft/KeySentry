#include "BossKey.h"
#include "Resource.h"
#include "Utils.h"
#include <algorithm>
#include <unordered_set>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

BossKey* BossKey::s_instance = nullptr;

BossKey::BossKey() { s_instance = this; }

BossKey::~BossKey() {
    UninstallMouseHook();
    Unregister();
    // 析构时恢复所有隐藏的窗口
    if (m_active) {
        ShowWindows();
        m_active = false;
    }
    s_instance = nullptr;
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
};

// EnumWindows 回调：收集匹配绑定规则的可见窗口
static BOOL CALLBACK EnumBossWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    auto* data = reinterpret_cast<EnumBossData*>(lParam);

    for (const auto& info : *data->boundWindows) {
        if (info.Matches(hwnd)) {
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
    m_hiddenWindows.clear();

    // 枚举匹配绑定规则的窗口
    std::vector<HWND> matching;
    EnumBossData data{ &m_boundWindows, &matching };
    EnumWindows(EnumBossWindowsProc, reinterpret_cast<LPARAM>(&data));

    // 保存窗口信息（显示状态、进程ID、路径），用于恢复
    for (HWND hwnd : matching) {
        WINDOWPLACEMENT wp = { sizeof(wp) };
        GetWindowPlacement(hwnd, &wp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        std::wstring procPath = ProcessUtils::GetProcessPath(pid);
        m_hiddenWindows.push_back({ hwnd, (int)wp.showCmd, pid, procPath });
    }

    // 可选：隐藏当前前台窗口（排除自身进程的窗口）
    if (m_hideCurrentWindow) {
        HWND fg = GetForegroundWindow();
        if (fg) {
            DWORD fgPid = 0;
            GetWindowThreadProcessId(fg, &fgPid);
            if (fgPid == GetCurrentProcessId()) fg = nullptr;
            if (fg) {
                // 避免重复添加
                bool alreadyInList = false;
                for (const auto& hw : m_hiddenWindows) {
                    if (hw.hwnd == fg) { alreadyInList = true; break; }
                }
                if (!alreadyInList) {
                    WINDOWPLACEMENT wp = { sizeof(wp) };
                    GetWindowPlacement(fg, &wp);
                    std::wstring procPath = ProcessUtils::GetProcessPath(fgPid);
                    m_hiddenWindows.push_back({ fg, (int)wp.showCmd, fgPid, procPath });
                }
            }
        }
    }

    // 可选：在隐藏前发送媒体暂停/播放按键
    if (m_sendPauseBeforeHide) SendMediaPause();

    // 隐藏所有窗口
    for (auto& hw : m_hiddenWindows) {
        ShowWindow(hw.hwnd, SW_HIDE);
    }

    // 持久化被隐藏窗口的信息，用于程序异常退出后恢复
    SaveRecoverFile();

    // 可选：静音系统音频
    if (m_muteEnabled) SetMute(true);
}

// ============================================================
// BossKey::ShowWindows - 恢复所有隐藏窗口
// 按隐藏前的显示状态恢复，最大化的窗口同时置前
// ============================================================
void BossKey::ShowWindows() {
    for (auto& hw : m_hiddenWindows) {
        if (!IsWindow(hw.hwnd)) continue;
        ShowWindow(hw.hwnd, hw.showCmd);
        // 恢复窗口后前置，确保用户能看到
        if (hw.showCmd == SW_SHOWMAXIMIZED || hw.showCmd == SW_SHOWNORMAL) {
            SetForegroundWindow(hw.hwnd);
        }
    }
    m_hiddenWindows.clear();
    // 窗口已恢复，清除恢复文件
    ClearRecoverFile();
}

// ============================================================
// BossKey::SetMute - 通过 Windows Core Audio API 设置系统静音
//   mute=true: 记录当前静音状态后静音
//   mute=false: 恢复到隐藏前的静音状态（除非 forceUnmute）
// ============================================================
void BossKey::SetMute(bool mute, bool forceUnmute) {
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

    if (mute) {
        // 记录静音前的状态，恢复时使用
        BOOL wasMuted = FALSE;
        if (SUCCEEDED(pVolume->GetMute(&wasMuted)))
            m_wasMuted = wasMuted != FALSE;
        pVolume->SetMute(TRUE, nullptr);
    } else {
        // 恢复：如果之前不是静音状态则取消静音，或强制取消静音
        pVolume->SetMute(forceUnmute ? FALSE : (m_wasMuted ? TRUE : FALSE), nullptr);
    }

    pVolume->Release();
}

// 发送媒体暂停/播放按键
void BossKey::SendMediaPause() {
    INPUT inp = {};
    inp.type = INPUT_KEYBOARD;
    inp.ki.wVk = VK_MEDIA_PLAY_PAUSE;
    inp.ki.wScan = (WORD)MapVirtualKeyW(VK_MEDIA_PLAY_PAUSE, MAPVK_VK_TO_VSC);
    SendInput(1, &inp, sizeof(INPUT));
    inp.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &inp, sizeof(INPUT));
}

void BossKey::Activate() { if (!m_active) { m_active = true; HideWindows(); } }

// 停用老板键：恢复窗口、取消静音、记录停用时间
void BossKey::Deactivate() {
    if (!m_active) return;
    m_active = false;
    ShowWindows();
    if (m_muteEnabled) SetMute(false);
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

    // 收集隐藏窗口的进程ID
    for (auto& hw : m_hiddenWindows) {
        if (!IsWindow(hw.hwnd)) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(hw.hwnd, &pid);
        if (pid == 0 || pid == GetCurrentProcessId()) continue;
        closedPids.insert(pid);
    }

    // 收集可见匹配窗口的进程ID（复用 EnumBossData 结构）
    std::vector<HWND> visibleMatching;
    EnumBossData cdata{ &m_boundWindows, &visibleMatching };
    EnumWindows(EnumBossWindowsProc, reinterpret_cast<LPARAM>(&cdata));

    for (HWND hwnd : visibleMatching) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == 0 || pid == GetCurrentProcessId()) continue;
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
        if (IsWindow(hw.hwnd)) {
            PostMessageW(hw.hwnd, WM_CLOSE, 0, 0);
        }
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
        // 分批等待，每批最多 MAXIMUM_WAIT_OBJECTS 个
        DWORD waitMs = (DWORD)std::min((ULONGLONG)500, timeoutMs - (GetTickCount64() - start));
        if (waitMs == 0) break;
        for (size_t batchStart = 0; batchStart < alive.size(); batchStart += MAXIMUM_WAIT_OBJECTS) {
            size_t batchSize = std::min(alive.size() - batchStart, (size_t)MAXIMUM_WAIT_OBJECTS);
            WaitForMultipleObjects((DWORD)batchSize, &alive[batchStart], FALSE, waitMs);
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
        if (m_muteEnabled) SetMute(false, true);
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
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring path(exePath);
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) path = path.substr(0, pos + 1);
    return path + L"KeySentry.recover";
}

void BossKey::SaveRecoverFile() {
    if (m_hiddenWindows.empty()) {
        ClearRecoverFile();
        return;
    }

    // 每行格式：pid \t showCmd \t hwnd \t 窗口标题（标题可含 \t，取第 3 个 \t 之后）
    std::wstring content;
    for (const auto& hw : m_hiddenWindows) {
        wchar_t title[512] = {};
        if (IsWindow(hw.hwnd)) GetWindowTextW(hw.hwnd, title, 512);
        wchar_t line[1024] = {};
        swprintf_s(line, L"%lu\t%d\t%llu\t%s\n",
                   (unsigned long)hw.pid, hw.showCmd,
                   (unsigned long long)(uintptr_t)hw.hwnd, title);
        content += line;
    }

    // 转为 UTF-8 写入文件（支持中文窗口标题与中文路径）
    int bytes = WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return;
    std::string utf8((size_t)bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, &utf8[0], bytes, nullptr, nullptr);

    HANDLE hFile = CreateFileW(GetRecoverFilePath().c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(hFile, utf8.data(), (DWORD)(bytes - 1), &written, nullptr);  // 不写结尾 NUL
    CloseHandle(hFile);
}

void BossKey::ClearRecoverFile() {
    DeleteFileW(GetRecoverFilePath().c_str());
}

// 枚举回调上下文：按进程 ID + 标题子串查找窗口
struct RecoverFindCtx {
    DWORD pid;
    const std::wstring* title;
    bool found;
};

// 枚举回调：32 位下 GCC 的 lambda 无法隐式转换为 stdcall 的 WNDENUMPROC，
// 故用显式 CALLBACK 约定的命名函数
static BOOL CALLBACK RecoverFindWindowProc(HWND h, LPARAM lp) {
    auto* ctx = reinterpret_cast<RecoverFindCtx*>(lp);
    DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    if (p != ctx->pid) return TRUE;
    wchar_t t[512] = {};
    if (GetWindowTextW(h, t, 512) == 0) return TRUE;
    if (wcsstr(t, ctx->title->c_str()) != nullptr) {
        if (!IsWindowVisible(h)) ShowWindow(h, SW_SHOWNORMAL);
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
    // 恢复文件是一次性的，无论恢复结果如何都删除
    ClearRecoverFile();

    if (utf8.empty()) return 0;

    // UTF-8 转宽字符
    int wchars = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (wchars <= 0) return 0;
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

        // 解析：pid \t showCmd \t hwnd \t 标题
        size_t t1 = line.find(L'\t');
        if (t1 == std::wstring::npos) continue;
        size_t t2 = line.find(L'\t', t1 + 1);
        if (t2 == std::wstring::npos) continue;
        size_t t3 = line.find(L'\t', t2 + 1);
        if (t3 == std::wstring::npos) continue;

        DWORD pid = (DWORD)wcstoul(line.substr(0, t1).c_str(), nullptr, 10);
        int showCmd = (int)wcstol(line.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 10);
        HWND hwnd = (HWND)(uintptr_t)wcstoull(line.substr(t2 + 1, t3 - t2 - 1).c_str(), nullptr, 10);
        std::wstring title = line.substr(t3 + 1);
        if (showCmd <= 0) showCmd = SW_SHOWNORMAL;

        // 路径 1：原窗口句柄仍有效（同一会话内程序崩溃后重启）
        bool done = false;
        if (hwnd && IsWindow(hwnd)) {
            DWORD wpid = 0;
            GetWindowThreadProcessId(hwnd, &wpid);
            if (wpid == pid) {
                if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, showCmd);
                done = true;
            }
        }

        // 路径 2：句柄失效（跨会话/句柄被复用），按进程 ID + 标题子串匹配
        if (!done && !title.empty()) {
            RecoverFindCtx fctx{ pid, &title, false };
            EnumWindows(RecoverFindWindowProc, reinterpret_cast<LPARAM>(&fctx));
            done = fctx.found;
        }

        if (done) recovered++;
    }
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
    if (nCode == HC_ACTION && s_instance) {
        // 中键按下：触发老板键并吞掉事件
        if (wParam == WM_MBUTTONDOWN && s_instance->m_middleBtnEnabled) {
            PostMessageW(s_instance->m_mouseNotifyWnd, WM_BOSS_MOUSE_TOGGLE, 0, 0);
            return 1;
        }
        // 中键释放：吞掉事件
        if (wParam == WM_MBUTTONUP && s_instance->m_middleBtnEnabled) {
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
    }
    return CallNextHookEx(s_instance ? s_instance->m_mouseHook : nullptr, nCode, wParam, lParam);
}
