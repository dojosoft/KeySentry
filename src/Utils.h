#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <Windows.h>
#include <CommCtrl.h>
#include <objbase.h>
#include <Psapi.h>

#pragma comment(lib, "Psapi.lib")

// ============================================================
// 窗口工具命名空间
// ============================================================
namespace WindowUtils {
    // 安全获取窗口标题
    // 说明：GetWindowTextW 对其它进程的窗口会同步发送 WM_GETTEXT，
    //       目标进程无响应时会阻塞调用线程。老板键运行在承载
    //       WH_MOUSE_LL 鼠标钩子的 UI 线程上，一旦阻塞会导致全系统
    //       鼠标输入停滞，故改用带超时的 SendMessageTimeoutW。
    //   注意：这里不能加 SMTO_BLOCK —— 该标志要求"等待期间不处理任何其它
    //       消息"，而低级鼠标钩子正是靠给本线程投递消息来派发的，加了它
    //       反而会让鼠标在整个等待期间完全无响应。只用 SMTO_ABORTIFHUNG，
    //       等待期间线程仍可继续派发钩子回调。
    inline std::wstring GetTextSafe(HWND hwnd, UINT timeoutMs = 50) {
        if (!IsWindow(hwnd)) return L"";
        // 先用栈缓冲尝试；返回值达到缓冲上限说明可能被截断，再按实际长度重读一次。
        // 固定 512 字符会让超长标题被截断，导致按标题绑定/恢复文件精确匹配失败
        const int kInitialChars = 512;
        wchar_t buf[kInitialChars] = {};
        DWORD_PTR result = 0;
        LRESULT ok = SendMessageTimeoutW(hwnd, WM_GETTEXT, (WPARAM)kInitialChars, (LPARAM)buf,
                                         SMTO_ABORTIFHUNG, timeoutMs, &result);
        if (ok == 0) {
            // 超时或失败：仅对同进程窗口回退到 GetWindowTextW（不会跨进程阻塞）
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == GetCurrentProcessId()) {
                std::wstring text(kInitialChars, L'\0');
                int len = GetWindowTextW(hwnd, &text[0], kInitialChars);
                text.resize(len > 0 ? (size_t)len : 0);
                return text;
            }
            return L"";
        }

        if ((size_t)result < (size_t)kInitialChars - 1) return buf;

        // 可能被截断：查询实际长度后重读（上限 32K，防止异常窗口返回极大值）
        DWORD_PTR len = 0;
        if (SendMessageTimeoutW(hwnd, WM_GETTEXTLENGTH, 0, 0,
                                SMTO_ABORTIFHUNG, timeoutMs, &len) != 0 &&
            len > 0 && len + 1 <= 32768) {
            std::wstring text((size_t)len + 1, L'\0');
            DWORD_PTR copied = 0;
            if (SendMessageTimeoutW(hwnd, WM_GETTEXT, (WPARAM)(len + 1), (LPARAM)&text[0],
                                    SMTO_ABORTIFHUNG, timeoutMs, &copied) != 0) {
                text.resize((size_t)copied);
                return text;
            }
        }
        return buf;
    }

    // 是否为桌面/任务栏等外壳窗口：这些窗口不允许被隐藏、关闭或修改样式，
    // 否则会破坏桌面与任务栏的显示层级
    inline bool IsShellWindow(HWND hwnd) {
        wchar_t cls[128] = {};
        if (GetClassNameW(hwnd, cls, 128) == 0) return false;
        static const wchar_t* kShellClasses[] = {
            L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"NotifyIconOverflowWindow",
            L"Progman", L"WorkerW", L"TaskListOverflowWnd"
        };
        for (auto name : kShellClasses) {
            if (_wcsicmp(cls, name) == 0) return true;
        }
        return false;
    }
}

// ============================================================
// 进程工具命名空间：提供进程路径和进程名查询功能
// ============================================================
namespace ProcessUtils {
    // 根据进程ID获取进程完整路径，优先使用 QueryFullProcessImageNameW，失败则回退到 GetModuleFileNameExW
    // 路径可能超过 MAX_PATH（Win10+ 长路径），故采用两段式查询
    inline std::wstring GetProcessPath(DWORD pid) {
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProc) return L"";
        std::wstring result;

        // 第一段：按 MAX_PATH 缓冲查询；若路径更长，API 会在 size 中返回所需长度
        wchar_t path[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, path, &size)) {
            result.assign(path, size);
        } else if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && size > MAX_PATH) {
            // 第二段：按所需长度重新分配并查询
            std::wstring big(size, L'\0');
            DWORD size2 = (DWORD)big.size();
            if (QueryFullProcessImageNameW(hProc, 0, &big[0], &size2)) {
                result.assign(big.c_str(), size2);
            }
        }

        // 回退：GetModuleFileNameExW（缓冲同样放大，避免长路径再次失败）
        if (result.empty()) {
            std::wstring big(1024, L'\0');
            DWORD len = GetModuleFileNameExW(hProc, nullptr, &big[0], (DWORD)big.size());
            if (len > 0 && len < big.size()) result.assign(big.c_str(), len);
        }

        CloseHandle(hProc);
        return result;
    }

    // 根据进程ID获取进程可执行文件名（不含路径）
    inline std::wstring GetProcessName(DWORD pid) {
        std::wstring fullPath = GetProcessPath(pid);
        if (fullPath.empty()) return L"";
        size_t pos = fullPath.find_last_of(L"\\/");
        return (pos != std::wstring::npos) ? fullPath.substr(pos + 1) : fullPath;
    }

    // 获取当前进程 exe 的完整路径（动态缓冲，支持超过 MAX_PATH 的长路径）
    inline std::wstring GetModulePath() {
        std::wstring buf(MAX_PATH, L'\0');
        for (int i = 0; i < 4; ++i) {
            DWORD len = GetModuleFileNameW(nullptr, &buf[0], (DWORD)buf.size());
            if (len == 0) return L"";
            if (len < buf.size()) {
                buf.resize(len);
                return buf;
            }
            // 被截断：扩大缓冲后重试（旧系统不返回所需长度，只能翻倍）
            buf.resize(buf.size() * 2);
        }
        return L"";
    }

    // 获取当前进程 exe 所在目录（以反斜杠结尾）
    inline std::wstring GetModuleDir() {
        std::wstring path = GetModulePath();
        size_t pos = path.find_last_of(L"\\/");
        if (pos == std::wstring::npos) return L"";
        return path.substr(0, pos + 1);
    }

    // 获取当前用户 %APPDATA% 目录（动态缓冲，路径可超过 MAX_PATH）
    inline std::wstring GetAppDataDir() {
        DWORD size = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
        if (size == 0) return L"";
        std::wstring buf((size_t)size, L'\0');
        DWORD len = GetEnvironmentVariableW(L"APPDATA", &buf[0], size);
        if (len == 0 || len >= size) return L"";
        buf.resize(len);
        return buf;
    }

    // 进程路径信息（含可执行文件名与完整路径）
    struct ProcPathInfo {
        std::wstring name;      // 可执行文件名（不含路径）
        std::wstring path;      // 进程完整路径
        bool resolved = false;  // 是否成功解析到路径
    };

    // 进程路径缓存：单次窗口枚举期间缓存 pid->路径，避免每个窗口重复 OpenProcess
    class PathCache {
    public:
        // 获取指定进程的路径信息（首次查询后缓存，后续直接命中）
        const ProcPathInfo& Get(DWORD pid) {
            auto it = m_map.find(pid);
            if (it != m_map.end()) return it->second;
            ProcPathInfo info;
            info.path = GetProcessPath(pid);
            if (!info.path.empty()) {
                size_t pos = info.path.find_last_of(L"\\/");
                info.name = (pos != std::wstring::npos) ? info.path.substr(pos + 1) : info.path;
                info.resolved = true;
            }
            return m_map.emplace(pid, std::move(info)).first->second;
        }
    private:
        std::unordered_map<DWORD, ProcPathInfo> m_map;
    };
}

// ============================================================
// GdiObjectGuard: GDI 对象的 RAII 包装，自动删除 GDI 对象
// ============================================================
class GdiObjectGuard {
    HGDIOBJ m_obj;
public:
    explicit GdiObjectGuard(HGDIOBJ obj = nullptr) : m_obj(obj) {}
    ~GdiObjectGuard() { if (m_obj) DeleteObject(m_obj); }
    GdiObjectGuard(const GdiObjectGuard&) = delete;
    GdiObjectGuard& operator=(const GdiObjectGuard&) = delete;
    HGDIOBJ get() const { return m_obj; }
    operator HGDIOBJ() const { return m_obj; }
    HGDIOBJ release() { HGDIOBJ o = m_obj; m_obj = nullptr; return o; }
};

// ============================================================
// CoInitGuard: COM 初始化的 RAII 包装，构造时初始化，析构时反初始化
// ============================================================
class CoInitGuard {
    bool m_needUninit;
public:
    CoInitGuard() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        m_needUninit = SUCCEEDED(hr);
    }
    ~CoInitGuard() { if (m_needUninit) CoUninitialize(); }
    CoInitGuard(const CoInitGuard&) = delete;
    CoInitGuard& operator=(const CoInitGuard&) = delete;
};

// ============================================================
// ModalGuard: 模态对话框的 RAII 包装，构造时禁用父窗口，析构时恢复
// ============================================================
class ModalGuard {
    HWND m_parent;
    bool m_wasEnabled;   // 进入前的启用状态，析构时按原样还原
public:
    explicit ModalGuard(HWND parent)
        : m_parent(parent),
          m_wasEnabled(IsWindow(parent) != FALSE && IsWindowEnabled(parent) != FALSE) {
        if (IsWindow(m_parent)) EnableWindow(m_parent, FALSE);
    }
    ~ModalGuard() {
        // 只还原"进入前本就处于启用状态"的窗口：若父窗口事先已被禁用（嵌套模态），
        // 无条件 EnableWindow 会误启用外层正需要保持禁用的窗口
        if (IsWindow(m_parent) && m_wasEnabled) {
            EnableWindow(m_parent, TRUE);
            SetForegroundWindow(m_parent);
        }
    }
    ModalGuard(const ModalGuard&) = delete;
    ModalGuard& operator=(const ModalGuard&) = delete;
};

// 主窗口句柄，全局共享
extern HWND g_mainWnd;
// 按键捕获期间为 true：各处模态消息循环据此丢弃 WM_HOTKEY，
// 避免捕获操作本身触发热键动作（如老板键隐藏窗口）打断捕获
extern bool g_suppressHotkeys;

struct AppConfig;
// 预检查热键冲突（在设置对话框中应用前调用）
extern std::wstring PreCheckHotkeyConflicts(const AppConfig& newConfig);
// 弹出托盘气泡通知（warning 为 true 时用警告图标）；实现见 Main.cpp
extern void ShowTrayNotification(const std::wstring& text, bool warning);

// ============================================================
// RunModalLoop: 模态消息循环，用于模态对话框
// 特殊处理 WM_HOTKEY 消息，转发给主窗口处理
// ============================================================
inline int RunModalLoop(HWND dlg) {
    MSG msg;
    while (IsWindow(dlg)) {
        BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
        if (ret <= 0) { if (ret == 0) PostQuitMessage((int)msg.wParam); break; }
        if (!IsWindow(dlg)) break;
        // 热键消息转发给主窗口处理，避免在模态对话框中被吞掉；
        // 按键捕获期间丢弃 WM_HOTKEY，避免触发热键动作打断捕获
        if (msg.message == WM_HOTKEY) {
            if (!g_suppressHotkeys && g_mainWnd && IsWindow(g_mainWnd))
                SendMessageW(g_mainWnd, msg.message, msg.wParam, msg.lParam);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// ============================================================
// IniUtils: INI 配置文件读写工具
// ============================================================
namespace IniUtils {
    static constexpr DWORD BUFFER_SIZE = 65536;

    // 从 INI 文件读取字符串值
    inline std::wstring Read(const std::wstring& path, const wchar_t* section,
                              const wchar_t* key, const wchar_t* def = L"") {
        auto buf = std::make_unique<wchar_t[]>(BUFFER_SIZE);
        GetPrivateProfileStringW(section, key, def, buf.get(), BUFFER_SIZE, path.c_str());
        return buf.get();
    }

    // 向 INI 文件写入字符串值
    inline void Write(const std::wstring& path, const wchar_t* section,
                       const wchar_t* key, const std::wstring& val) {
        WritePrivateProfileStringW(section, key, val.c_str(), path.c_str());
    }

    // 布尔值转字符串（"1" / "0"）
    inline std::wstring BoolToStr(bool v) { return std::to_wstring(v ? 1 : 0); }
}

// 关闭进程的超时时间（毫秒），超时后强制终止
static constexpr DWORD PROCESS_CLOSE_TIMEOUT_MS = 3000;

// 判断指定虚拟键码是否需要 KEYEVENTF_EXTENDEDKEY 标志
// 包括导航键、NumLock、右 Ctrl/Alt、Win 键等
inline bool NeedsExtendedKeyFlag(UINT vk) {
    if (vk >= 0x21 && vk <= 0x2E) return true;
    if (vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END ||
        vk == VK_PRIOR || vk == VK_NEXT || vk == VK_NUMLOCK ||
        vk == VK_RCONTROL || vk == VK_RMENU || vk == VK_DIVIDE ||
        (vk >= 0x5B && vk <= 0x5D)) return true;
    return false;
}

// ============================================================
// 高 DPI 适配：进程声明为系统 DPI 感知后，固定像素布局在高 DPI 屏幕上会偏小。
// 统一用该缩放系数换算字号与控件坐标，保证 100%/125%/150%/200% 下观感一致。
// ============================================================
inline float& UiScaleStorage() {
    static float scale = 0.0f;
    return scale;
}

// 计算界面缩放系数：以系统 DPI 为基准，并限制在可用工作区内
// （以最大的设置窗口 800x680 为基准，避免缩放后窗口超出屏幕）。
inline void InitUiScale() {
    HDC hdc = GetDC(nullptr);
    int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
    if (hdc) ReleaseDC(nullptr, hdc);
    if (dpi <= 0) dpi = 96;

    float scale = (float)dpi / 96.0f;

    RECT work = { 0, 0, 0, 0 };
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        float fitW = (float)(work.right - work.left - 40) / 800.0f;
        float fitH = (float)(work.bottom - work.top - 60) / 680.0f;
        float fit = (fitW < fitH) ? fitW : fitH;
        if (scale > fit) scale = fit;
    }

    // 低 DPI 下不缩小；极端缩放设置上限，防止布局溢出
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 3.0f) scale = 3.0f;
    UiScaleStorage() = scale;
}

inline float GetUiScale() {
    float s = UiScaleStorage();
    return s > 0.0f ? s : 1.0f;
}

// 将逻辑像素换算为当前 DPI 下的物理像素（四舍五入）
inline int ScaleUi(int v) {
    return (int)(v * GetUiScale() + (v >= 0 ? 0.5f : -0.5f));
}

// 按缩放系数重设子控件矩形；ListView 同步缩放列宽。
// 仅用于布局完成后一次性缩放，运行期反复调用会重复放大。
// skipIds 中的控件（已按物理坐标创建）会被跳过，典型用于选项卡与底部按钮。
inline void ScaleChildWindows(HWND parent, float scale,
                              const LONG_PTR* skipIds = nullptr, int skipCount = 0) {
    if (!parent || scale <= 0.0f || scale == 1.0f) return;

    HWND child = GetWindow(parent, GW_CHILD);
    while (child) {
        HWND next = GetWindow(child, GW_HWNDNEXT);

        bool skip = false;
        if (skipIds) {
            LONG_PTR id = GetWindowLongPtrW(child, GWLP_ID);
            for (int i = 0; i < skipCount; i++) {
                if (id == skipIds[i]) { skip = true; break; }
            }
        }

        if (!skip) {
            RECT rc = { 0, 0, 0, 0 };
            if (GetWindowRect(child, &rc)) {
                MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&rc), 2);
                int x = (int)(rc.left * scale + (rc.left >= 0 ? 0.5f : -0.5f));
                int y = (int)(rc.top * scale + (rc.top >= 0 ? 0.5f : -0.5f));
                int w = (int)((rc.right - rc.left) * scale + 0.5f);
                int h = (int)((rc.bottom - rc.top) * scale + 0.5f);
                SetWindowPos(child, nullptr, x, y, w, h,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }

            wchar_t cls[64] = {};
            if (GetClassNameW(child, cls, 64) && wcscmp(cls, WC_LISTVIEWW) == 0) {
                int cols = (int)Header_GetItemCount((HWND)SendMessageW(child, LVM_GETHEADER, 0, 0));
                for (int i = 0; i < cols; i++) {
                    int cw = (int)SendMessageW(child, LVM_GETCOLUMNWIDTH, i, 0);
                    if (cw > 0) {
                        SendMessageW(child, LVM_SETCOLUMNWIDTH, i,
                                     (LPARAM)((int)(cw * scale + 0.5f)));
                    }
                }
            }
        }

        child = next;
    }
}

// 创建 UI 字体，基于系统默认字体调整高度和粗细
inline HFONT CreateUiFont(int height, int weight = FW_NORMAL) {
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -ScaleUi(height);
    lf.lfWeight = weight;
    return CreateFontIndirectW(&lf);
}
