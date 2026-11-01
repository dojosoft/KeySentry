#include "Config.h"
#include <winuser.h>
#include <algorithm>
#include <sstream>

// ============================================================
// BoundWindowInfo::Matches
// 窗口/进程匹配相关实现
//   - Process 模式：按进程名或完整路径匹配（不区分大小写）
//   - Window 模式：按窗口标题子串匹配
// ============================================================
// 判断进程可执行名/完整路径是否匹配（进程名或完整路径任一相同即视为匹配）
bool BoundWindowInfo::MatchesProcessInfo(const std::wstring& exeName, const std::wstring& fullPath) const {
    if (!processName.empty() && _wcsicmp(exeName.c_str(), processName.c_str()) == 0) return true;
    if (!processPath.empty() && _wcsicmp(fullPath.c_str(), processPath.c_str()) == 0) return true;
    return false;
}

bool BoundWindowInfo::Matches(HWND hwnd, bool requireVisible) const {
    if (!IsWindow(hwnd)) return false;
    if (requireVisible && !IsWindowVisible(hwnd)) return false;

    DWORD hwndPid = 0;
    GetWindowThreadProcessId(hwnd, &hwndPid);

    if (matchMode == MatchMode::Process) {
        if (hwndPid == 0) return false;
        std::wstring fullPath = ProcessUtils::GetProcessPath(hwndPid);
        if (fullPath.empty()) return false;
        // 提取可执行文件名用于匹配
        size_t pos = fullPath.find_last_of(L"\\/");
        std::wstring exeName = (pos != std::wstring::npos) ? fullPath.substr(pos + 1) : fullPath;
        return MatchesProcessInfo(exeName, fullPath);
    }

    // Window 模式：标题子串匹配
    if (!title.empty()) {
        std::wstring hwndTitle = WindowUtils::GetTextSafe(hwnd);
        if (!hwndTitle.empty() && hwndTitle.find(title) != std::wstring::npos) {
            return true;
        }
    }

    return false;
}

// 带进程路径缓存的匹配：仅 Process 模式走缓存，避免每个窗口都 OpenProcess 查询路径
bool BoundWindowInfo::Matches(HWND hwnd, ProcessUtils::PathCache& cache, bool requireVisible) const {
    if (matchMode != MatchMode::Process) return Matches(hwnd, requireVisible);
    if (!IsWindow(hwnd)) return false;
    if (requireVisible && !IsWindowVisible(hwnd)) return false;
    DWORD hwndPid = 0;
    GetWindowThreadProcessId(hwnd, &hwndPid);
    if (hwndPid == 0) return false;
    const auto& info = cache.Get(hwndPid);
    if (!info.resolved) return false;
    return MatchesProcessInfo(info.name, info.path);
}

// ============================================================
// 配置文件解析辅助函数
// ============================================================

// 将逗号分隔的整数字符串解析为整数向量
// 要求整段完整解析：std::stoi 会接受 "12abc" 这类前缀匹配（pos=2 仍算成功），
// 那样手改 INI 写入的错误值会被静默当作合法值，这里一律丢弃
static std::vector<int> ParseCSVInts(const std::wstring& csv) {
    std::vector<int> result;
    std::wstringstream ss(csv);
    std::wstring token;
    while (std::getline(ss, token, L',')) {
        if (token.empty()) continue;
        try {
            size_t pos = 0;
            int val = std::stoi(token, &pos);
            if (pos == token.size()) result.push_back(val);
        } catch (...) {}
    }
    return result;
}

// 将整数向量转换为逗号分隔的字符串
static std::wstring IntsToCSV(const std::vector<int>& vals) {
    std::wstring result;
    for (size_t i = 0; i < vals.size(); ++i) {
        if (i > 0) result += L",";
        result += std::to_wstring(vals[i]);
    }
    return result;
}

// 将 ";;" 分隔的字符串列表解析为字符串向量（用于列表项序列化）
static std::vector<std::wstring> ParseEntryList(const std::wstring& str) {
    std::vector<std::wstring> result;
    size_t start = 0;
    while (start < str.size()) {
        size_t pos = str.find(L";;", start);
        if (pos == std::wstring::npos) {
            std::wstring token = str.substr(start);
            if (!token.empty()) result.push_back(token);
            break;
        }
        std::wstring token = str.substr(start, pos - start);
        if (!token.empty()) result.push_back(token);
        start = pos + 2;
    }
    return result;
}

// 将字符串向量用 ";;" 连接为一个字符串
static std::wstring EntryListToString(const std::vector<std::wstring>& vals) {
    std::wstring result;
    for (size_t i = 0; i < vals.size(); ++i) {
        if (i > 0) result += L";;";
        result += vals[i];
    }
    return result;
}

// 将管道分隔的字符串解析为字符串向量（用于旧版 WinDGuard 配置兼容）
static std::vector<std::wstring> ParsePipes(const std::wstring& csv) {
    std::vector<std::wstring> result;
    std::wstringstream ss(csv);
    std::wstring token;
    while (std::getline(ss, token, L'|')) {
        if (!token.empty()) result.push_back(token);
    }
    return result;
}

// ============================================================
// AppConfig::Load - 从 INI 文件加载所有配置项
// ============================================================
void AppConfig::Load(const std::wstring& iniPath) {
    // --- 加载策略说明 ---
    // 采取「尽力读取」：无论 INI 由哪个版本写入，都按当前已知字段逐个读取，
    // 未知或已废弃的字段自然被忽略，不会因版本号更高而整体放弃配置。
    // 保存时再按最新配置规范重写（见 Save），实现配置格式的自动收敛。

    // --- 通用设置 ---
    autoStart = GetPrivateProfileIntW(L"General", L"AutoStart", 0, iniPath.c_str()) != 0;
    showLockKeyOverlay = GetPrivateProfileIntW(L"General", L"ShowOverlay", 1, iniPath.c_str()) != 0;
    showMuteOverlay = GetPrivateProfileIntW(L"General", L"ShowMuteOverlay", 0, iniPath.c_str()) != 0;
    overlayPrimaryOnly = GetPrivateProfileIntW(L"General", L"OverlayPrimaryOnly", 0, iniPath.c_str()) != 0;
    // 浮窗外观：越界值（手改 INI）统一收敛到默认，避免绘制/下拉框出现非法状态。
    // 旧版本（v1.40 及更早）的 INI 不含这些键，将按默认值读取（深色/底部/圆角/低透明/适中），
    // 不影响其余字段；新版写入的这些键在旧版本中会被当作未知键忽略
    overlayPosition = GetPrivateProfileIntW(L"General", L"OverlayPosition", 0, iniPath.c_str());
    if (overlayPosition < 0 || overlayPosition > 4) overlayPosition = 0;
    overlayTheme = GetPrivateProfileIntW(L"General", L"OverlayTheme", 0, iniPath.c_str());
    if (overlayTheme < 0 || overlayTheme > 1) overlayTheme = 0;
    overlayCorner = GetPrivateProfileIntW(L"General", L"OverlayCorner", 1, iniPath.c_str());
    if (overlayCorner < 0 || overlayCorner > 1) overlayCorner = 1;
    overlayOpacity = GetPrivateProfileIntW(L"General", L"OverlayOpacity", 2, iniPath.c_str());
    if (overlayOpacity < 0 || overlayOpacity > 3) overlayOpacity = 2;
    overlayDelay = GetPrivateProfileIntW(L"General", L"OverlayDelay", 1, iniPath.c_str());
    if (overlayDelay < 0 || overlayDelay > 3) overlayDelay = 1;
    forceNumLockOn = GetPrivateProfileIntW(L"General", L"ForceNumLockOn", 0, iniPath.c_str()) != 0;
    disableStartupNotification = GetPrivateProfileIntW(L"General", L"DisableStartupNotification", 0, iniPath.c_str()) != 0;
    // 资源管理器选项：合法值 0=保持原状 / 1=开启隐藏 / 2=关闭隐藏。
    // 越界值（手改 INI 等）一律归零：否则下拉框无选中项，CB_GETCURSEL 回写 -1，
    // 会让 ApplyExplorerSettings 的"三项均为 0 则跳过"判断失效，多一次注册表打开与广播
    auto clampExplorerOpt = [](int v) { return (v >= 0 && v <= 2) ? v : 0; };
    explorerHideHidden = clampExplorerOpt(GetPrivateProfileIntW(L"General", L"ExplorerHideHidden", 0, iniPath.c_str()));
    explorerHideExt = clampExplorerOpt(GetPrivateProfileIntW(L"General", L"ExplorerHideExt", 0, iniPath.c_str()));
    explorerHideOS = clampExplorerOpt(GetPrivateProfileIntW(L"General", L"ExplorerHideOS", 0, iniPath.c_str()));
    tabState = GetPrivateProfileIntW(L"General", L"TabState", 0, iniPath.c_str());
    if (tabState < 0 || tabState > 5) tabState = 0;

    // --- 按键禁用设置 ---
    disableSpecifiedKeysEnabled = GetPrivateProfileIntW(L"Keys", L"DisableSpecifiedKeysEnabled", 0, iniPath.c_str()) != 0;
    disableNumLock = GetPrivateProfileIntW(L"Keys", L"DisableNumLock", 0, iniPath.c_str()) != 0;
    forceInsertMode = GetPrivateProfileIntW(L"Keys", L"ForceInsertMode", 0, iniPath.c_str()) != 0;
    auto keysStr = IniUtils::Read(iniPath, L"Keys", L"DisabledKeyCodes");
    disabledKeyCodes.clear();  // 重复载入时避免沿用上一次的禁用项
    for (int vk : ParseCSVInts(keysStr)) {
        // 与重映射/自定义热键保持一致的合法性过滤：非法键码永不匹配真实按键，
        // 留在列表里只会显示成 "0x0" 这类无意义条目
        if (ConfigUtils::IsValidVK(vk)) disabledKeyCodes.push_back(vk);
    }

    // --- 按键重映射 ---
    keyRemapEnabled = GetPrivateProfileIntW(L"KeyRemap", L"Enabled", 0, iniPath.c_str()) != 0;
    keyRemappings.clear();   // 重复载入时避免沿用上一次的映射项

    // 新格式 MappingsV2：三元组 {源键, 修饰键, 目标键}；旧格式 Mappings 仍按成对读取
    auto remapV2Str = IniUtils::Read(iniPath, L"KeyRemap", L"MappingsV2");
    auto remapV2Ints = ParseCSVInts(remapV2Str);
    if (!remapV2Ints.empty()) {
        for (size_t i = 0; i + 2 < remapV2Ints.size(); i += 3) {
            int src = remapV2Ints[i];
            UINT mods = ConfigUtils::SanitizeMods(remapV2Ints[i + 1]);
            int tgt = remapV2Ints[i + 2];
            // 丢弃非法键码：保留会让源键被吞掉却模拟不出目标键，等于该键彻底失效
            if (!ConfigUtils::IsValidVK(src) || !ConfigUtils::IsValidVK(tgt)) continue;
            // 修饰键作源键会让整个 Ctrl/Shift/Alt/Win 失效，直接丢弃
            if (ConfigUtils::IsModifierKey(src)) continue;
            if (mods == 0 && src == tgt) continue;
            // 同一源键只保留第一条，避免重复映射造成行为不确定
            bool exists = false;
            for (const auto& r : keyRemappings) {
                if (r.sourceVK == src) { exists = true; break; }
            }
            if (!exists) keyRemappings.push_back({ src, mods, tgt });
        }
    } else {
        auto remapStr = IniUtils::Read(iniPath, L"KeyRemap", L"Mappings");
        auto remapInts = ParseCSVInts(remapStr);
        // 旧格式以成对形式存储：{源键, 目标键}，等价于 mods = 0
        for (size_t i = 0; i + 1 < remapInts.size(); i += 2) {
            int src = remapInts[i];
            int tgt = remapInts[i + 1];
            if (!ConfigUtils::IsValidVK(src) || !ConfigUtils::IsValidVK(tgt)) continue;
            if (ConfigUtils::IsModifierKey(src)) continue;
            if (src == tgt) continue;
            bool exists = false;
            for (const auto& r : keyRemappings) {
                if (r.sourceVK == src) { exists = true; break; }
            }
            if (!exists) keyRemappings.push_back({ src, 0, tgt });
        }
    }

    // --- 自定义热键 ---
    customHotkeysEnabled = GetPrivateProfileIntW(L"CustomHotkeys", L"Enabled", 0, iniPath.c_str()) != 0;
    {
        auto customStr = IniUtils::Read(iniPath, L"CustomHotkeys", L"Entries");
        auto parts = ParseEntryList(customStr);
        customHotkeys.clear();
        for (const auto& p : parts) {
            if (p.empty()) continue;
            CustomHotkey hk = CustomHotkey::Deserialize(p);
            // 丢弃手改 INI 产生的非法条目：无修饰键或无主键时注册会全局劫持普通按键
            hk.mod = ConfigUtils::SanitizeMods((int)hk.mod);
            if (hk.mod == 0 || !ConfigUtils::IsValidVK((int)hk.vk)) continue;
            // 模拟按键模式下目标键非法时动作必然失败，同样丢弃
            if (hk.simulateKey && !ConfigUtils::IsValidVK((int)hk.targetVK)) continue;
            // 模拟组合键的修饰键位收敛；修饰键不能作为组合的主键
            hk.targetMod = ConfigUtils::SanitizeMods((int)hk.targetMod);
            if (hk.targetMod != 0 && ConfigUtils::IsModifierKey((int)hk.targetVK)) hk.targetMod = 0;
            // 丢弃重复组合（mod+vk 相同只保留第一条），避免注册失败连环弹窗
            bool duplicated = false;
            for (const auto& existing : customHotkeys) {
                if (existing.mod == hk.mod && existing.vk == hk.vk) { duplicated = true; break; }
            }
            if (duplicated) continue;
            customHotkeys.push_back(hk);
            // 超出注册上限的条目直接丢弃（保存时即收敛为合法配置）
            if ((int)customHotkeys.size() >= MAX_CUSTOM_HOTKEYS) break;
        }
    }

    // --- 老板键设置 ---
    bossKeyEnabled = GetPrivateProfileIntW(L"BossKey", L"Enabled", 0, iniPath.c_str()) != 0;
    // 手改 INI 的非法修饰键/虚拟键码会让 RegisterHotKey 永远失败，载入时统一收敛
    bossKeyMod = ConfigUtils::SanitizeMods(GetPrivateProfileIntW(L"BossKey", L"Modifiers", MOD_WIN, iniPath.c_str()));
    {
        int vkVal = GetPrivateProfileIntW(L"BossKey", L"VKCode", 0xC0, iniPath.c_str());
        bossKeyVK = ConfigUtils::IsValidVK(vkVal) ? (UINT)vkVal : 0xC0;
    }
    bossKeyCloseMod = ConfigUtils::SanitizeMods(GetPrivateProfileIntW(L"BossKey", L"CloseModifiers", MOD_WIN, iniPath.c_str()));
    {
        int vkVal = GetPrivateProfileIntW(L"BossKey", L"CloseVKCode", VK_ESCAPE, iniPath.c_str());
        bossKeyCloseVK = ConfigUtils::IsValidVK(vkVal) ? (UINT)vkVal : (UINT)VK_ESCAPE;
    }
    bossKeyMute = GetPrivateProfileIntW(L"BossKey", L"Mute", 0, iniPath.c_str()) != 0;
    auto bossWinsStr = IniUtils::Read(iniPath, L"BossKey", L"Windows");
    {
        auto parts = ParseEntryList(bossWinsStr);
        bossKeyWindows.clear();
        for (const auto& p : parts) {
            if (!p.empty()) bossKeyWindows.push_back(BoundWindowInfo::Deserialize(p));
        }
    }
    bossKeyHideCurrent = GetPrivateProfileIntW(L"BossKey", L"HideCurrent", 0, iniPath.c_str()) != 0;
    bossKeySendPause = GetPrivateProfileIntW(L"BossKey", L"SendPause", 0, iniPath.c_str()) != 0;
    bossKeyMiddleButton = GetPrivateProfileIntW(L"BossKey", L"MiddleButton", 0, iniPath.c_str()) != 0;
    bossKeySideButton1 = GetPrivateProfileIntW(L"BossKey", L"SideButton1", 0, iniPath.c_str()) != 0;
    bossKeySideButton2 = GetPrivateProfileIntW(L"BossKey", L"SideButton2", 0, iniPath.c_str()) != 0;
    bossKeyCornerTL = GetPrivateProfileIntW(L"BossKey", L"CornerTL", 0, iniPath.c_str()) != 0;
    bossKeyCornerTR = GetPrivateProfileIntW(L"BossKey", L"CornerTR", 0, iniPath.c_str()) != 0;
    bossKeyCornerBL = GetPrivateProfileIntW(L"BossKey", L"CornerBL", 0, iniPath.c_str()) != 0;
    bossKeyCornerBR = GetPrivateProfileIntW(L"BossKey", L"CornerBR", 0, iniPath.c_str()) != 0;
    bossKeyAutoHide = GetPrivateProfileIntW(L"BossKey", L"AutoHide", 0, iniPath.c_str()) != 0;
    bossKeyAutoHideTime = GetPrivateProfileIntW(L"BossKey", L"AutoHideTime", 5, iniPath.c_str());
    if (bossKeyAutoHideTime < 1) bossKeyAutoHideTime = 1;
    if (bossKeyAutoHideTime > 120) bossKeyAutoHideTime = 120;
    bossKeyCloseOnExit = GetPrivateProfileIntW(L"BossKey", L"CloseOnExit", 0, iniPath.c_str()) != 0;

    // --- WinDGuard 窗口保护 ---
    winDGuardEnabled = GetPrivateProfileIntW(L"WinDGuard", L"Enabled", 0, iniPath.c_str()) != 0;
    {
        auto windWinsStr = IniUtils::Read(iniPath, L"WinDGuard", L"Windows");
        // 区分新旧格式：
        //   新格式：使用 ;; 作为条目分隔符，| 作为字段分隔符
        //     多条目: "0|Notepad||0|;;1|calc.exe|C:\calc.exe"
        //     单条目: "0|Notepad||0|" (以 matchMode 数字开头)
        //   旧格式：使用 | 作为条目分隔符，每个条目是纯标题
        //     多条目: "Notepad|Calculator"
        //     单条目: "Notepad" (不含 |)
        bool isNewFormat = false;
        if (windWinsStr.find(L";;") != std::wstring::npos) {
            // 包含 ;; 条目分隔符，一定是新格式
            isNewFormat = true;
        } else if (!windWinsStr.empty()) {
            // 无 ;; 时，检查首字段是否为 matchMode 数字（0 或 1）
            // 新格式单条目: "0|Notepad||0|" 或 "1|calc.exe|C:\calc.exe"
            // 旧格式单条目: "Notepad" (无 |)
            // 旧格式多条目: "Notepad|Calculator" (首字段是标题，不是数字)
            size_t firstPipe = windWinsStr.find(L'|');
            if (firstPipe != std::wstring::npos && firstPipe > 0) {
                std::wstring firstField = windWinsStr.substr(0, firstPipe);
                int modeVal = ConfigUtils::SafeParseInt(firstField, -1);
                if (modeVal == 0 || modeVal == 1) {
                    isNewFormat = true;
                }
            }
        }

        if (isNewFormat) {
            auto parts = ParseEntryList(windWinsStr);
            winDGuardWindows.clear();
            for (const auto& p : parts) {
                if (!p.empty()) winDGuardWindows.push_back(BoundWindowInfo::Deserialize(p));
            }
        } else {
            auto parts = ParsePipes(windWinsStr);
            winDGuardWindows.clear();
            for (const auto& p : parts) {
                BoundWindowInfo info;
                info.title = p;
                info.matchMode = MatchMode::Window;
                winDGuardWindows.push_back(info);
            }
        }
    }
}

// ============================================================
// AppConfig::Save - 保存所有配置项到 INI 文件
// ============================================================
bool AppConfig::Save(const std::wstring& iniPath) const {
    // 原子写入：先写临时文件再替换目标文件，避免写盘途中崩溃/断电留下截断的配置。
    // 以全新的临时文件为写入对象，也自然丢弃旧版本遗留的废弃字段
    std::wstring tmpPath = iniPath + L".tmp";
    DeleteFileW(tmpPath.c_str());
    IniUtils::Write(tmpPath, L"General", L"ConfigVersion", std::to_wstring(CONFIG_VERSION));
    IniUtils::Write(tmpPath, L"General", L"AutoStart", IniUtils::BoolToStr(autoStart));
    IniUtils::Write(tmpPath, L"General", L"ShowOverlay", IniUtils::BoolToStr(showLockKeyOverlay));
    IniUtils::Write(tmpPath, L"General", L"ShowMuteOverlay", IniUtils::BoolToStr(showMuteOverlay));
    IniUtils::Write(tmpPath, L"General", L"OverlayPrimaryOnly", IniUtils::BoolToStr(overlayPrimaryOnly));
    IniUtils::Write(tmpPath, L"General", L"OverlayPosition", std::to_wstring(overlayPosition));
    IniUtils::Write(tmpPath, L"General", L"OverlayTheme", std::to_wstring(overlayTheme));
    IniUtils::Write(tmpPath, L"General", L"OverlayCorner", std::to_wstring(overlayCorner));
    IniUtils::Write(tmpPath, L"General", L"OverlayOpacity", std::to_wstring(overlayOpacity));
    IniUtils::Write(tmpPath, L"General", L"OverlayDelay", std::to_wstring(overlayDelay));
    IniUtils::Write(tmpPath, L"General", L"ForceNumLockOn", IniUtils::BoolToStr(forceNumLockOn));
    IniUtils::Write(tmpPath, L"General", L"DisableStartupNotification", IniUtils::BoolToStr(disableStartupNotification));
    IniUtils::Write(tmpPath, L"General", L"ExplorerHideHidden", std::to_wstring(explorerHideHidden));
    IniUtils::Write(tmpPath, L"General", L"ExplorerHideExt", std::to_wstring(explorerHideExt));
    IniUtils::Write(tmpPath, L"General", L"ExplorerHideOS", std::to_wstring(explorerHideOS));
    IniUtils::Write(tmpPath, L"General", L"TabState", std::to_wstring(tabState));

    IniUtils::Write(tmpPath, L"Keys", L"DisableSpecifiedKeysEnabled", IniUtils::BoolToStr(disableSpecifiedKeysEnabled));
    IniUtils::Write(tmpPath, L"Keys", L"DisableNumLock", IniUtils::BoolToStr(disableNumLock));
    IniUtils::Write(tmpPath, L"Keys", L"ForceInsertMode", IniUtils::BoolToStr(forceInsertMode));
    IniUtils::Write(tmpPath, L"Keys", L"DisabledKeyCodes", IntsToCSV(disabledKeyCodes));

    // 按键重映射：写入新格式三元组；同时为旧版本保留单键映射的成对格式
    IniUtils::Write(tmpPath, L"KeyRemap", L"Enabled", IniUtils::BoolToStr(keyRemapEnabled));
    std::vector<int> remapV2Flat;
    std::vector<int> remapLegacyFlat;
    for (const auto& r : keyRemappings) {
        remapV2Flat.push_back(r.sourceVK);
        remapV2Flat.push_back((int)r.mods);
        remapV2Flat.push_back(r.targetVK);
        if (r.mods == 0) {
            remapLegacyFlat.push_back(r.sourceVK);
            remapLegacyFlat.push_back(r.targetVK);
        }
    }
    IniUtils::Write(tmpPath, L"KeyRemap", L"MappingsV2", IntsToCSV(remapV2Flat));
    IniUtils::Write(tmpPath, L"KeyRemap", L"Mappings", IntsToCSV(remapLegacyFlat));

    // 自定义热键：始终按当前序列化规范整体重写，
    // 旧版本遗留的废弃字段（sound/opacity/autoStart）在此被丢弃
    IniUtils::Write(tmpPath, L"CustomHotkeys", L"Enabled", IniUtils::BoolToStr(customHotkeysEnabled));
    {
        std::vector<std::wstring> serialized;
        for (const auto& hk : customHotkeys) serialized.push_back(hk.Serialize());
        IniUtils::Write(tmpPath, L"CustomHotkeys", L"Entries", EntryListToString(serialized));
    }

    // 老板键设置
    IniUtils::Write(tmpPath, L"BossKey", L"Enabled", IniUtils::BoolToStr(bossKeyEnabled));
    IniUtils::Write(tmpPath, L"BossKey", L"Modifiers", std::to_wstring(bossKeyMod));
    IniUtils::Write(tmpPath, L"BossKey", L"VKCode", std::to_wstring(bossKeyVK));
    IniUtils::Write(tmpPath, L"BossKey", L"CloseModifiers", std::to_wstring(bossKeyCloseMod));
    IniUtils::Write(tmpPath, L"BossKey", L"CloseVKCode", std::to_wstring(bossKeyCloseVK));
    IniUtils::Write(tmpPath, L"BossKey", L"Mute", IniUtils::BoolToStr(bossKeyMute));
    {
        std::vector<std::wstring> serialized;
        for (const auto& w : bossKeyWindows) serialized.push_back(w.Serialize());
        IniUtils::Write(tmpPath, L"BossKey", L"Windows", EntryListToString(serialized));
    }
    IniUtils::Write(tmpPath, L"BossKey", L"HideCurrent", IniUtils::BoolToStr(bossKeyHideCurrent));
    IniUtils::Write(tmpPath, L"BossKey", L"SendPause", IniUtils::BoolToStr(bossKeySendPause));
    IniUtils::Write(tmpPath, L"BossKey", L"MiddleButton", IniUtils::BoolToStr(bossKeyMiddleButton));
    IniUtils::Write(tmpPath, L"BossKey", L"SideButton1", IniUtils::BoolToStr(bossKeySideButton1));
    IniUtils::Write(tmpPath, L"BossKey", L"SideButton2", IniUtils::BoolToStr(bossKeySideButton2));
    IniUtils::Write(tmpPath, L"BossKey", L"CornerTL", IniUtils::BoolToStr(bossKeyCornerTL));
    IniUtils::Write(tmpPath, L"BossKey", L"CornerTR", IniUtils::BoolToStr(bossKeyCornerTR));
    IniUtils::Write(tmpPath, L"BossKey", L"CornerBL", IniUtils::BoolToStr(bossKeyCornerBL));
    IniUtils::Write(tmpPath, L"BossKey", L"CornerBR", IniUtils::BoolToStr(bossKeyCornerBR));
    IniUtils::Write(tmpPath, L"BossKey", L"AutoHide", IniUtils::BoolToStr(bossKeyAutoHide));
    IniUtils::Write(tmpPath, L"BossKey", L"AutoHideTime", std::to_wstring(bossKeyAutoHideTime));
    IniUtils::Write(tmpPath, L"BossKey", L"CloseOnExit", IniUtils::BoolToStr(bossKeyCloseOnExit));

    // WinDGuard 设置
    IniUtils::Write(tmpPath, L"WinDGuard", L"Enabled", IniUtils::BoolToStr(winDGuardEnabled));
    {
        std::vector<std::wstring> serialized;
        for (const auto& w : winDGuardWindows) serialized.push_back(w.Serialize());
        IniUtils::Write(tmpPath, L"WinDGuard", L"Windows", EntryListToString(serialized));
    }

    // 刷新临时文件的 INI 缓存到磁盘后再原子替换目标文件；
    // 替换失败时清理临时文件，避免 .tmp 残留
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, tmpPath.c_str());
    if (!MoveFileExW(tmpPath.c_str(), iniPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmpPath.c_str());
        return false;  // 目录只读/磁盘满/权限不足：由调用方提示，避免"已应用"却未落盘
    }
    return true;
}

// ============================================================
// AppConfig::VKToName - 虚拟键码转可读名称
// 优先使用硬编码映射表，最后尝试通过 GetKeyNameTextW 获取
// ============================================================
std::wstring AppConfig::VKToName(int vk) {
    if (vk == VK_NUMLOCK) return L"Num Lock";
    if (vk == VK_CAPITAL) return L"Caps Lock";
    if (vk == VK_SCROLL) return L"Scroll Lock";
    if (vk == VK_INSERT) return L"Insert";
    if (vk == VK_DELETE) return L"Delete";
    if (vk == VK_HOME) return L"Home";
    if (vk == VK_END) return L"End";
    if (vk == VK_PRIOR) return L"Page Up";
    if (vk == VK_NEXT) return L"Page Down";
    if (vk == VK_PRINT) return L"Print";
    if (vk == VK_SNAPSHOT) return L"PrtScn";
    if (vk == VK_PAUSE) return L"Pause";
    if (vk == VK_TAB) return L"Tab";
    if (vk == VK_ESCAPE) return L"Esc";
    if (vk == VK_SPACE) return L"Space";
    if (vk == VK_BACK) return L"Backspace";
    if (vk == VK_RETURN) return L"Enter";
    if (vk == VK_LSHIFT) return L"Shift (左)";
    if (vk == VK_RSHIFT) return L"Shift (右)";
    if (vk == VK_LCONTROL) return L"Ctrl (左)";
    if (vk == VK_RCONTROL) return L"Ctrl (右)";
    if (vk == VK_LMENU) return L"Alt (左)";
    if (vk == VK_RMENU) return L"Alt (右)";
    if (vk == VK_LWIN) return L"Win (左)";
    if (vk == VK_RWIN) return L"Win (右)";
    if (vk == VK_APPS) return L"Menu";
    if (vk == VK_F1) return L"F1";
    if (vk == VK_F2) return L"F2";
    if (vk == VK_F3) return L"F3";
    if (vk == VK_F4) return L"F4";
    if (vk == VK_F5) return L"F5";
    if (vk == VK_F6) return L"F6";
    if (vk == VK_F7) return L"F7";
    if (vk == VK_F8) return L"F8";
    if (vk == VK_F9) return L"F9";
    if (vk == VK_F10) return L"F10";
    if (vk == VK_F11) return L"F11";
    if (vk == VK_F12) return L"F12";
    // 数字键 0-9
    if (vk >= 0x30 && vk <= 0x39) return std::wstring(1, (wchar_t)vk);
    // 字母键 A-Z
    if (vk >= 0x41 && vk <= 0x5A) return std::wstring(1, (wchar_t)vk);
    // 常见 OEM 键
    if (vk == VK_OEM_3) return L"`";
    if (vk == VK_OEM_MINUS) return L"-";
    if (vk == VK_OEM_PLUS) return L"=";
    if (vk == VK_OEM_4) return L"[";
    if (vk == VK_OEM_6) return L"]";
    if (vk == VK_OEM_5) return L"\\";
    if (vk == VK_OEM_1) return L";";
    if (vk == VK_OEM_7) return L"'";
    if (vk == VK_OEM_COMMA) return L",";
    if (vk == VK_OEM_PERIOD) return L".";
    if (vk == VK_OEM_2) return L"/";

    // 回退方案：通过扫描码获取键名
    UINT scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (scanCode == 0) return L"Key 0x" + std::to_wstring(vk);
    // GetKeyNameTextW 的 lParam 形参为 LONG，此处用 LONG 构造避免 64 位下的截断告警
    LONG lParam = (LONG)scanCode << 16;
    if (NeedsExtendedKeyFlag(vk)) lParam |= (1 << 24);
    wchar_t name[64] = {};
    if (GetKeyNameTextW(lParam, name, 64) > 0) return name;

    return L"Key 0x" + std::to_wstring(vk);
}

// ============================================================
// AppConfig::NameToVK - 可读名称转虚拟键码
// 使用硬编码映射表反向查找
// ============================================================
int AppConfig::NameToVK(const std::wstring& name) {
    static const struct { const wchar_t* n; int vk; } map[] = {
        {L"Num Lock", VK_NUMLOCK}, {L"Caps Lock", VK_CAPITAL}, {L"Scroll Lock", VK_SCROLL},
        {L"Insert", VK_INSERT}, {L"Delete", VK_DELETE}, {L"Home", VK_HOME}, {L"End", VK_END},
        {L"Page Up", VK_PRIOR}, {L"Page Down", VK_NEXT}, {L"PrtScn", VK_SNAPSHOT},
        {L"Print", VK_PRINT}, {L"Pause", VK_PAUSE}, {L"Tab", VK_TAB}, {L"Esc", VK_ESCAPE},
        {L"Space", VK_SPACE}, {L"Backspace", VK_BACK}, {L"Enter", VK_RETURN},
        {L"Shift (左)", VK_LSHIFT}, {L"Shift (右)", VK_RSHIFT},
        {L"Ctrl (左)", VK_LCONTROL}, {L"Ctrl (右)", VK_RCONTROL},
        {L"Alt (左)", VK_LMENU}, {L"Alt (右)", VK_RMENU},
        {L"Win (左)", VK_LWIN}, {L"Win (右)", VK_RWIN},
        {L"Menu", VK_APPS},
        {L"F1", VK_F1}, {L"F2", VK_F2}, {L"F3", VK_F3}, {L"F4", VK_F4},
        {L"F5", VK_F5}, {L"F6", VK_F6}, {L"F7", VK_F7}, {L"F8", VK_F8},
        {L"F9", VK_F9}, {L"F10", VK_F10}, {L"F11", VK_F11}, {L"F12", VK_F12},
        {L"`", VK_OEM_3}, {L"-", VK_OEM_MINUS}, {L"=", VK_OEM_PLUS},
        {L"[", VK_OEM_4}, {L"]", VK_OEM_6}, {L"\\", VK_OEM_5},
        {L";", VK_OEM_1}, {L"'", VK_OEM_7}, {L",", VK_OEM_COMMA},
        {L".", VK_OEM_PERIOD}, {L"/", VK_OEM_2},
    };
    for (auto& m : map) {
        if (name == m.n) return m.vk;
    }
    // 单字符：数字或字母
    if (name.length() == 1) {
        wchar_t ch = towupper(name[0]);
        if (ch >= L'0' && ch <= L'9') return (int)ch;
        if (ch >= L'A' && ch <= L'Z') return (int)ch;
    }
    return 0;
}
