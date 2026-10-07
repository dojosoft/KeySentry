#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <Windows.h>
#include <objbase.h>
#include <Psapi.h>

#pragma comment(lib, "Psapi.lib")

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
public:
    explicit ModalGuard(HWND parent) : m_parent(parent) { if (IsWindow(parent)) EnableWindow(parent, FALSE); }
    ~ModalGuard() { if (IsWindow(m_parent)) { EnableWindow(m_parent, TRUE); SetForegroundWindow(m_parent); } }
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

// 创建 UI 字体，基于系统默认字体调整高度和粗细
inline HFONT CreateUiFont(int height, int weight = FW_NORMAL) {
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -height;
    lf.lfWeight = weight;
    return CreateFontIndirectW(&lf);
}
