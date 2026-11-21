#pragma once
#include <string>
#include <vector>
#include <memory>
#include <atomic>
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
    // 注册窗口类（幂等）：类已存在视为成功，避免第二次创建窗口时注册失败。
    // 各窗口模块共用，统一处理 ERROR_CLASS_ALREADY_EXISTS。
    inline bool RegisterClassOnce(const WNDCLASSW& wc) {
        if (RegisterClassW(&wc)) return true;
        return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

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
                // 目标窗口的返回值不可全信：异常窗口可能返回超过缓冲区的长度，
                // 直接 resize 会抛出 bad_alloc，这里钳制到实际请求的字符数
                if (copied > len) copied = len;
                text.resize((size_t)copied);
                return text;
            }
        }
        return buf;
    }

    // 将窗口尺寸与位置夹紧到当前工作区并居中。
    // 高 DPI 下次级对话框可能超过屏幕高度，导致标题栏为负、底部按钮移出工作区
    inline void ClampWindowToWorkArea(int* x, int* y, int* w, int* h) {
        if (!w || !h) return;
        RECT work = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int workW = work.right - work.left;
        const int workH = work.bottom - work.top;
        if (workW <= 0 || workH <= 0) return;
        if (*w > workW) *w = workW;
        if (*h > workH) *h = workH;
        if (x) *x = work.left + (workW - *w) / 2;
        if (y) *y = work.top + (workH - *h) / 2;
    }

    // 将窗口居中到指定工作区并夹紧尺寸；工作区无效时回退主屏工作区
    inline void CenterWindowInWorkArea(const RECT& work, int* x, int* y, int* w, int* h) {
        if (!w || !h) return;
        const int workW = work.right - work.left;
        const int workH = work.bottom - work.top;
        if (workW <= 0 || workH <= 0) {
            ClampWindowToWorkArea(x, y, w, h);
            return;
        }
        if (*w > workW) *w = workW;
        if (*h > workH) *h = workH;
        if (x) *x = work.left + (workW - *w) / 2;
        if (y) *y = work.top + (workH - *h) / 2;
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
    // 取路径所在目录（不含结尾分隔符）；无法解析时返回空
    inline std::wstring PathDirectory(const std::wstring& path) {
        const size_t pos = path.find_last_of(L"\\/");
        return (pos == std::wstring::npos) ? std::wstring() : path.substr(0, pos);
    }

    // 取路径中的文件名（含扩展名）
    inline std::wstring PathFileName(const std::wstring& path) {
        const size_t pos = path.find_last_of(L"\\/");
        return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
    }

    // 判断文件名（可传完整路径）是否为本产品的任一构建产物。
    // 单实例判定、自启项归属清理等场景共用同一份名单：新增构建名时
    // 只需改这里，避免某处漏改导致同名/异名实例互相覆盖状态（R13-A13）
    inline bool IsProductExecutableName(const std::wstring& nameOrPath) {
        const std::wstring name = PathFileName(nameOrPath);
        return _wcsicmp(name.c_str(), L"KeySentry.exe") == 0 ||
               _wcsicmp(name.c_str(), L"KeySentry-x86.exe") == 0 ||
               _wcsicmp(name.c_str(), L"KeySentry-x64.exe") == 0;
    }

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

    // 查询进程令牌的完整性级别（最后一个子权威，即 RID）。
    // 用于提权运行时的操作边界：配置文件/恢复文件位于用户可写目录，
    // 不能被用来驱动高完整性窗口/进程操作（R8-H3）
    inline bool GetProcessIntegrityRid(DWORD pid, DWORD* rid) {
        if (!rid) return false;
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProc) return false;
        HANDLE token = nullptr;
        bool ok = false;
        if (OpenProcessToken(hProc, TOKEN_QUERY, &token)) {
            DWORD size = 0;
            GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
            if (size > 0) {
                std::vector<BYTE> buf(size);
                if (GetTokenInformation(token, TokenIntegrityLevel, buf.data(), size, &size)) {
                    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buf.data());
                    if (label->Label.Sid) {
                        const DWORD count = *GetSidSubAuthorityCount(label->Label.Sid);
                        if (count > 0) {
                            *rid = *GetSidSubAuthority(label->Label.Sid, count - 1);
                            ok = true;
                        }
                    }
                }
            }
            CloseHandle(token);
        }
        CloseHandle(hProc);
        return ok;
    }

    // 当前进程完整性 RID 的缓存值：自身令牌在进程生命周期内不变，
    // 而 IsProcessIntegrityAllowed 会在每次窗口枚举中对每个窗口调用一次，
    // 每次都 OpenProcessToken(自身) 纯属浪费（一次枚举可能上百次）。
    // 用函数内静态量缓存结果；查询失败不缓存（可能受临时资源限制），
    // 由下一次调用重试，避免把瞬时失败固化为“永久不允许”
    inline bool GetSelfIntegrityRid(DWORD* rid) {
        if (!rid) return false;
        static DWORD cached = 0;
        static bool cachedValid = false;
        if (!cachedValid) {
            if (!GetProcessIntegrityRid(GetCurrentProcessId(), &cached)) return false;
            cachedValid = true;
        }
        *rid = cached;
        return true;
    }

    // 目标进程的完整性级别是否不高于当前进程。
    // 无法查询时按“不允许”处理：高影响操作宁可跳过，也不对无法确认的目标执行
    inline bool IsProcessIntegrityAllowed(DWORD pid) {
        DWORD selfRid = 0;
        if (!GetSelfIntegrityRid(&selfRid)) return false;
        DWORD targetRid = 0;
        if (!GetProcessIntegrityRid(pid, &targetRid)) return false;
        return targetRid <= selfRid;
    }

    // 当前进程是否已提权。令牌提升状态在进程生命周期内不变，可安全缓存；
    // 用于在提权会话中按需启用完整性边界检查，避免普通会话为每个窗口增加开销
    inline bool IsCurrentProcessElevated() {
        static const bool elevated = []() -> bool {
            HANDLE token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
            TOKEN_ELEVATION info = {};
            DWORD returned = 0;
            const BOOL ok = GetTokenInformation(token, TokenElevation, &info,
                                                sizeof(info), &returned);
            CloseHandle(token);
            return ok && info.TokenIsElevated != 0;
        }();
        return elevated;
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

    // 文件是否存在（非目录）
    inline bool FileExists(const std::wstring& path) {
        if (path.empty()) return false;
        const DWORD attrs = GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
    }

    // 文件存在且非空（0 字节配置/恢复记录没有实际内容）
    inline bool FileExistsNonEmpty(const std::wstring& path) {
        if (path.empty()) return false;
        WIN32_FILE_ATTRIBUTE_DATA data = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
        return data.nFileSizeHigh != 0 || data.nFileSizeLow != 0;
    }

    inline bool FileWriteTime(const std::wstring& path, FILETIME* out) {
        WIN32_FILE_ATTRIBUTE_DATA data = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
        if (out) *out = data.ftLastWriteTime;
        return true;
    }

    // 原子写入：先写同目录临时文件，落盘成功后替换目标；失败清理临时文件。
    // 配置/恢复记录/锁文件共用，避免各自实现漏掉 Flush 或短写检查。
    // 临时文件名使用系统生成的唯一名（先由系统创建，调用方无法预知），并以
    // FILE_FLAG_OPEN_REPARSE_POINT 打开：固定的 ".tmp" + CREATE_ALWAYS 会被
    // 同用户进程预置的重解析点/占位文件劫持，把提权进程的写入重定向（R8-M12）
    // flushBuffers=false 供崩溃恢复类文件使用：进程崩溃不会丢失 OS 缓存，
    // 跳过同步刷盘可避免在 UI 线程（承载低级钩子）上出现秒级停顿（R9-A9）
    inline bool AtomicWriteFile(const std::wstring& path, const void* data, size_t bytes,
                                bool flushBuffers = true) {
        if (path.empty()) return false;

        std::wstring dir;
        const size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos) dir = path.substr(0, slash);

        wchar_t tempName[MAX_PATH] = {};
        std::wstring tmpPath;
        // 记录临时文件是否由系统创建：回退路径 path+".tmp" 可能是目录中
        // 已存在的其他文件，失败的清理不能误删（R14-A4）
        bool tempCreatedBySystem = false;
        if (!dir.empty() && GetTempFileNameW(dir.c_str(), L"ksn", 0, tempName) != 0) {
            tmpPath = tempName;  // 系统已创建唯一空文件
            tempCreatedBySystem = true;
        } else {
            // 目录不可用/路径过长：退回固定名，但仍使用 OPEN_REPARSE_POINT，
            // 避免沿预置符号链接写入
            tmpPath = path + L".tmp";
        }

        HANDLE hFile = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                   nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            // GetTempFileNameW 已创建了占位文件：打开失败时必须删除，
            // 否则反复保存失败会在目录中累积空临时文件（R13-A3）
            if (tempCreatedBySystem) DeleteFileW(tmpPath.c_str());
            return false;
        }

        // DWORD 无法表示超过 4GB 的长度；截断后仍按 written == toWrite
        // 判定成功会静默写出不完整内容（R14-A4）
#ifdef _WIN64
        if (bytes > (size_t)0xFFFFFFFFull) {
            CloseHandle(hFile);
            if (tempCreatedBySystem) DeleteFileW(tmpPath.c_str());
            return false;
        }
#endif
        bool ok = true;
        if (bytes > 0) {
            const DWORD toWrite = (DWORD)bytes;
            DWORD written = 0;
            ok = WriteFile(hFile, data, toWrite, &written, nullptr) &&
                 written == toWrite;
        }
        if (flushBuffers) ok = FlushFileBuffers(hFile) != FALSE;
        CloseHandle(hFile);
        if (!ok) {
            DeleteFileW(tmpPath.c_str());
            return false;
        }
        if (!MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(tmpPath.c_str());
            return false;
        }
        return true;
    }

    // 崩溃恢复/锁记录文件的候选位置：exe 同目录优先，%APPDATA%\KeySentry 兜底。
    // 读取阶段必须同时检查两处；写入阶段按顺序回退，避免目录变只读后
    // 恢复记录静默失效（R15-A02）
    inline std::vector<std::wstring> RecoveryFileCandidates(const wchar_t* fileName) {
        std::vector<std::wstring> paths;
        if (!fileName || !*fileName) return paths;
        const std::wstring dir = GetModuleDir();
        if (!dir.empty()) paths.push_back(dir + fileName);

        const std::wstring appData = GetAppDataDir();
        if (!appData.empty()) {
            const std::wstring ad = appData + L"\\KeySentry";
            CreateDirectoryW(ad.c_str(), nullptr);
            paths.push_back(ad + L"\\" + fileName);
        }
        return paths;
    }

    // 主路径不存在但回退路径存在时优先使用回退路径（不复制）：
    // 用于恢复/锁文件，避免旧位置的崩溃记录被静默忽略
    inline std::wstring PreferExistingFile(const std::wstring& primary,
                                            const std::wstring& fallback) {
        if (FileExistsNonEmpty(primary)) return primary;
        if (FileExistsNonEmpty(fallback)) return fallback;
        return primary;
    }

    // 两个位置都存在时选择修改时间较新的文件：
    // 程序移动/权限变化后可能同时留下新旧记录，不能盲目优先主路径
    inline std::wstring PreferNewerFile(const std::wstring& primary,
                                         const std::wstring& fallback) {
        const bool hasPrimary = FileExistsNonEmpty(primary);
        const bool hasFallback = FileExistsNonEmpty(fallback);
        if (hasPrimary && !hasFallback) return primary;
        if (!hasPrimary && hasFallback) return fallback;
        if (!hasPrimary && !hasFallback) return primary;
        FILETIME tp = {}, tf = {};
        if (FileWriteTime(primary, &tp) && FileWriteTime(fallback, &tf)) {
            return (CompareFileTime(&tp, &tf) >= 0) ? primary : fallback;
        }
        return primary;
    }

    // 主路径不存在但旧路径存在时复制到主路径（可写位置）：
    // 用于配置文件，保证程序移动/权限变化后仍能读写原有设置
    inline std::wstring MigrateOrPreferFile(const std::wstring& primary,
                                             const std::wstring& legacy) {
        if (FileExistsNonEmpty(primary)) return primary;
        if (FileExistsNonEmpty(legacy)) {
            // 0 字节主文件会被 CopyFileW(bFailIfExists=TRUE) 视为“已存在”
            // 而复制失败，导致每次启动都回退旧位置。先删除这个空壳再复制。
            if (FileExists(primary)) DeleteFileW(primary.c_str());
            if (CopyFileW(legacy.c_str(), primary.c_str(), TRUE)) {
                // CopyFile 会保留只读属性；目标只读会让后续原子替换失败
                const DWORD attrs = GetFileAttributesW(primary.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
                    SetFileAttributesW(primary.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
                }
                return primary;
            }
            return legacy;  // 复制失败：至少按只读方式加载旧配置
        }
        return primary;
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
// 按键捕获期间为 true。低级鼠标钩子在专用线程读取该标志，必须用原子类型
extern std::atomic<bool> g_suppressHotkeys;

struct AppConfig;
// 预检查热键冲突（在设置对话框中应用前调用）。
// occupancyOnly=true 时只检测是否被其他程序占用（打开设置时提醒用）
extern std::wstring PreCheckHotkeyConflicts(const AppConfig& newConfig,
                                            bool occupancyOnly = false);
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
        if (ret <= 0) {
            if (ret == 0) {
                // 退出消息：模态窗口可能仍然存活，先销毁再转发 WM_QUIT，
                // 避免调用方随后释放字体等资源时窗口仍在引用它们
                if (IsWindow(dlg)) DestroyWindow(dlg);
                PostQuitMessage((int)msg.wParam);
            } else {
                // 消息循环出错：与退出路径一样销毁窗口，避免调用方返回后
                // 留下一个无人管理的模态窗口（调用方通常不会再次销毁）
                if (IsWindow(dlg)) DestroyWindow(dlg);
            }
            break;
        }
        if (!IsWindow(dlg)) break;
        // 热键消息转发给主窗口处理，避免在模态对话框中被吞掉；
        // 按键捕获期间丢弃 WM_HOTKEY，避免触发热键动作打断捕获
        if (msg.message == WM_HOTKEY) {
            if (!g_suppressHotkeys && g_mainWnd && IsWindow(g_mainWnd))
                SendMessageW(g_mainWnd, msg.message, msg.wParam, msg.lParam);
            continue;
        }
        // 键盘导航：Tab 切换焦点、Enter 触发默认按钮、Esc 取消
        if (IsDialogMessageW(dlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// ============================================================
// IniUtils: INI 配置文件读写工具
// ============================================================
namespace IniUtils {
    // 从 INI 文件读取字符串值。
    // profile API 在缓冲不足时返回 size-1 而不报错，固定 64KB 会静默截断
    // 超长值（如包含大量自定义热键的 Entries），导致保存回读校验永远失败。
    // 这里从小缓冲开始，按“返回长度达到上限”判定截断并翻倍重试。
    static constexpr DWORD READ_BUFFER_INITIAL = 4096;
    static constexpr DWORD READ_BUFFER_MAX = 4 * 1024 * 1024;

    // 按实际文件大小估算读取上限：INI 文本每字符至少占 1 字节，
    // 因此“文件字节数 + 2”足以容纳任意 ANSI/UTF-16 单值。
    // 小文件不再分配 4MB 缓冲，也避免读取超过文件实际内容的长值。
    inline DWORD ComputeReadLimit(const std::wstring& path) {
        WIN32_FILE_ATTRIBUTE_DATA data = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
            return READ_BUFFER_INITIAL;
        }
        ULONGLONG need = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        need += 2;  // 结尾 NUL
        if (need < 2) need = 2;
        if (need > READ_BUFFER_MAX) need = READ_BUFFER_MAX;
        return (DWORD)need;
    }

    inline std::wstring Read(const std::wstring& path, const wchar_t* section,
                              const wchar_t* key, const wchar_t* def = L"") {
        const DWORD limit = ComputeReadLimit(path);
        DWORD size = (limit < READ_BUFFER_INITIAL) ? limit : READ_BUFFER_INITIAL;
        if (size < 2) size = 2;
        for (;;) {
            auto buf = std::make_unique<wchar_t[]>(size);
            const DWORD copied = GetPrivateProfileStringW(section, key, def, buf.get(),
                                                          size, path.c_str());
            if (copied < size - 1 || size >= limit) {
                return std::wstring(buf.get(), copied);
            }
            // 每次增长不超过估算上限，避免小文件按 4MB 峰值分配
            size = (size > limit / 2) ? limit : size * 2;
        }
    }

    // 向 INI 文件写入字符串值；返回是否成功。
    // 调用方必须检查返回值：磁盘满、只读、单值超长时 profile API 会失败，
    // 忽略返回值会让残缺配置被当成“保存成功”换上。
    inline bool Write(const std::wstring& path, const wchar_t* section,
                       const wchar_t* key, const std::wstring& val) {
        return WritePrivateProfileStringW(section, key, val.c_str(), path.c_str()) != FALSE;
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

// 线程级缩放覆盖：用于按目标窗口/显示器 DPI 临时绘制（Per-Monitor DPI）。
// 0 表示未覆盖，回退到进程级 UiScaleStorage。
inline float& UiScaleOverrideStorage() {
    static thread_local float scale = 0.0f;
    return scale;
}

class ScopedUiScale {
public:
    explicit ScopedUiScale(float scale) : m_old(UiScaleOverrideStorage()) {
        UiScaleOverrideStorage() = (scale > 0.0f) ? scale : 0.0f;
    }
    ~ScopedUiScale() { UiScaleOverrideStorage() = m_old; }
    ScopedUiScale(const ScopedUiScale&) = delete;
    ScopedUiScale& operator=(const ScopedUiScale&) = delete;
private:
    float m_old;
};

// 按 DPI 与工作区计算缩放系数：以最大的设置窗口 800x680 为基准，
// 避免缩放后窗口超出屏幕；低 DPI 尽量不缩小，但极小工作区（如 1280x720）
// 必须允许小于 1.0，否则窗口被工作区夹紧后底部按钮会移出客户区（R13-M3）。
// 下限 0.75 保证文字仍可读，上限 3 倍。
inline float ComputeUiScale(UINT dpi, const RECT* workArea = nullptr) {
    if (dpi == 0) dpi = 96;
    float scale = (float)dpi / 96.0f;

    RECT work = { 0, 0, 0, 0 };
    if (workArea) {
        work = *workArea;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    }
    if (work.right > work.left && work.bottom > work.top) {
        float fitW = (float)(work.right - work.left - 40) / 800.0f;
        float fitH = (float)(work.bottom - work.top - 60) / 680.0f;
        float fit = (fitW < fitH) ? fitW : fitH;
        if (scale > fit) scale = fit;
    }

    if (scale < 0.75f) scale = 0.75f;
    if (scale > 3.0f) scale = 3.0f;
    return scale;
}

inline void InitUiScale() {
    HDC hdc = GetDC(nullptr);
    int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
    if (hdc) ReleaseDC(nullptr, hdc);
    UiScaleStorage() = ComputeUiScale((UINT)dpi);
}

inline void SetUiScale(float scale) {
    if (scale < 0.75f) scale = 0.75f;
    if (scale > 3.0f) scale = 3.0f;
    UiScaleStorage() = scale;
}

inline float GetUiScale() {
    const float overrideScale = UiScaleOverrideStorage();
    if (overrideScale > 0.0f) return overrideScale;
    float s = UiScaleStorage();
    return s > 0.0f ? s : 1.0f;
}

// 将逻辑像素换算为当前 DPI 下的物理像素（四舍五入）
inline int ScaleUi(int v) {
    return (int)(v * GetUiScale() + (v >= 0 ? 0.5f : -0.5f));
}

// ============================================================
// DPI 查询：优先 Win10 GetDpiForWindow，其次 Win8.1 GetDpiForMonitor，
// 最后回退系统 DPI。全部动态加载，Win7 SP1 上自动走回退分支。
// ============================================================
namespace DpiUtils {
    inline HMODULE LoadSystemModule(const wchar_t* name) {
        wchar_t dir[MAX_PATH] = {};
        const UINT len = GetSystemDirectoryW(dir, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::wstring full = std::wstring(dir) + L"\\" + name;
            return LoadLibraryW(full.c_str());
        }
        return LoadLibraryW(name);
    }

    inline UINT GetSystemDpi() {
        HDC hdc = GetDC(nullptr);
        int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
        if (hdc) ReleaseDC(nullptr, hdc);
        return dpi > 0 ? (UINT)dpi : 96u;
    }

    inline UINT GetDpiForMonitorSafe(HMONITOR monitor) {
        if (monitor) {
            typedef HRESULT(WINAPI* PFN_GetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);
            static PFN_GetDpiForMonitor getDpi = []() -> PFN_GetDpiForMonitor {
                HMODULE shcore = LoadSystemModule(L"shcore.dll");
                if (!shcore) return nullptr;
                return reinterpret_cast<PFN_GetDpiForMonitor>(
                    GetProcAddress(shcore, "GetDpiForMonitor"));
            }();
            if (getDpi) {
                UINT dpiX = 0, dpiY = 0;
                // MDT_EFFECTIVE_DPI = 0
                if (SUCCEEDED(getDpi(monitor, 0, &dpiX, &dpiY)) && dpiY > 0) {
                    return dpiY;
                }
            }
        }
        return GetSystemDpi();
    }

    inline UINT GetDpiForWindowSafe(HWND hwnd) {
        if (hwnd) {
            typedef UINT(WINAPI* PFN_GetDpiForWindow)(HWND);
            static PFN_GetDpiForWindow getDpi = []() -> PFN_GetDpiForWindow {
                HMODULE user32 = GetModuleHandleW(L"user32.dll");
                if (!user32) return nullptr;
                return reinterpret_cast<PFN_GetDpiForWindow>(
                    GetProcAddress(user32, "GetDpiForWindow"));
            }();
            if (getDpi) {
                const UINT dpi = getDpi(hwnd);
                if (dpi > 0) return dpi;
            }
            HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            if (monitor) return GetDpiForMonitorSafe(monitor);
        }
        return GetSystemDpi();
    }

    // 获取目标窗口/显示器所在工作区
    inline bool GetWorkAreaForWindow(HWND hwnd, RECT* out) {
        if (!out) return false;
        HMONITOR monitor = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
        if (!monitor) {
            POINT pt = { 0, 0 };
            GetCursorPos(&pt);
            monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
        }
        if (!monitor) return false;
        MONITORINFO mi = { sizeof(mi) };
        if (!GetMonitorInfoW(monitor, &mi)) return false;
        *out = mi.rcWork;
        return true;
    }

    inline float ScaleForDpi(UINT dpi, const RECT* workArea = nullptr) {
        return ComputeUiScale(dpi, workArea);
    }
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
inline HFONT CreateUiFontAtScale(int height, int weight, float scale) {
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -(int)(height * scale + 0.5f);
    lf.lfWeight = weight;
    return CreateFontIndirectW(&lf);
}

inline HFONT CreateUiFont(int height, int weight = FW_NORMAL) {
    return CreateUiFontAtScale(height, weight, GetUiScale());
}
