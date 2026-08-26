#pragma once
#include <string>
#include <vector>
#include <memory>
#include <Windows.h>
#include <objbase.h>
#include <Psapi.h>

#pragma comment(lib, "Psapi.lib")

// ============================================================
// 进程工具命名空间：提供进程路径和进程名查询功能
// ============================================================
namespace ProcessUtils {
    // 根据进程ID获取进程完整路径，优先使用 QueryFullProcessImageNameW，失败则回退到 GetModuleFileNameExW
    inline std::wstring GetProcessPath(DWORD pid) {
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProc) return L"";
        wchar_t path[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        std::wstring result;
        if (QueryFullProcessImageNameW(hProc, 0, path, &size)) {
            result = path;
        } else {
            if (GetModuleFileNameExW(hProc, nullptr, path, MAX_PATH)) {
                result = path;
            }
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
}

// ============================================================
// HandleGuard: Windows HANDLE 的 RAII 包装，自动关闭句柄
// ============================================================
class HandleGuard {
    HANDLE m_h;
public:
    explicit HandleGuard(HANDLE h = nullptr) : m_h(h) {}
    ~HandleGuard() { if (m_h && m_h != INVALID_HANDLE_VALUE) CloseHandle(m_h); }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    HandleGuard(HandleGuard&& o) noexcept : m_h(o.m_h) { o.m_h = nullptr; }
    HandleGuard& operator=(HandleGuard&& o) noexcept { if (this != &o) { if (m_h) CloseHandle(m_h); m_h = o.m_h; o.m_h = nullptr; } return *this; }
    HANDLE get() const { return m_h; }
    operator bool() const { return m_h != nullptr && m_h != INVALID_HANDLE_VALUE; }
};

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
public:
    explicit ModalGuard(HWND parent) : m_parent(parent) { if (IsWindow(parent)) EnableWindow(parent, FALSE); }
    ~ModalGuard() { if (IsWindow(m_parent)) { EnableWindow(m_parent, TRUE); SetForegroundWindow(m_parent); } }
    ModalGuard(const ModalGuard&) = delete;
    ModalGuard& operator=(const ModalGuard&) = delete;
};

// 主窗口句柄，全局共享
extern HWND g_mainWnd;

struct AppConfig;
// 预检查热键冲突（在设置对话框中应用前调用）
extern std::wstring PreCheckHotkeyConflicts(const AppConfig& newConfig);

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
        // 热键消息转发给主窗口处理，避免在模态对话框中被吞掉
        if (msg.message == WM_HOTKEY) { if (g_mainWnd && IsWindow(g_mainWnd)) SendMessageW(g_mainWnd, msg.message, msg.wParam, msg.lParam); continue; }
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

// ============================================================
// DpiUtils: DPI 缩放工具，支持高 DPI 显示
// ============================================================
namespace DpiUtils {
    // 获取指定窗口的 DPI 缩放比例（相对于 96 DPI）
    inline float GetDpiScale(HWND hwnd) {
        // 优先使用 Win10+ 的 GetDpiForWindow API
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (hUser32) {
            typedef UINT(WINAPI* GetDpiForWindow_t)(HWND);
            auto pGetDpiForWindow = reinterpret_cast<GetDpiForWindow_t>(
                GetProcAddress(hUser32, "GetDpiForWindow"));
            if (pGetDpiForWindow && hwnd) {
                UINT dpi = pGetDpiForWindow(hwnd);
                if (dpi > 0) return dpi / 96.0f;
            }
        }
        // 回退方案：使用 DC 的 LOGPIXELSY
        HDC hdc = GetDC(hwnd);
        float scale = GetDeviceCaps(hdc, LOGPIXELSY) / 96.0f;
        ReleaseDC(hwnd, hdc);
        return scale;
    }

    // 按缩放比例换算像素值
    inline int Scale(int px, float scale) { return (int)(px * scale + 0.5f); }
}

// ============================================================
// WindowUtils: 窗口相关工具
// ============================================================
namespace WindowUtils {
    // 获取当前按下的修饰键组合（Ctrl/Shift/Alt/Win）
    inline UINT GetCurrentModifiers() {
        UINT mod = 0;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mod |= MOD_CONTROL;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mod |= MOD_SHIFT;
        if (GetAsyncKeyState(VK_MENU) & 0x8000) mod |= MOD_ALT;
        if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) mod |= MOD_WIN;
        return mod;
    }
}

// 关闭进程的超时时间（毫秒），超时后强制终止
static constexpr DWORD PROCESS_CLOSE_TIMEOUT_MS = 3000;
// 窗口标题缓冲区大小
static constexpr int WINDOW_TITLE_BUFFER_SIZE = 512;

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

// 创建 UI 字体，基于系统默认字体调整高度和粗细
inline HFONT CreateUiFont(int height, int weight = FW_NORMAL) {
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -height;
    lf.lfWeight = weight;
    return CreateFontIndirectW(&lf);
}
