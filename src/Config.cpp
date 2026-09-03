#include "Config.h"
#include <winuser.h>
#include <algorithm>
#include <sstream>

// ============================================================
// BoundWindowInfo::Matches
// 判断指定窗口是否匹配此绑定信息
//   - Process 模式：按进程名或完整路径匹配（不区分大小写）
//   - Window 模式：按窗口标题子串匹配
// ============================================================
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
        if (!processName.empty() && _wcsicmp(exeName.c_str(), processName.c_str()) == 0) return true;
        if (!processPath.empty() && _wcsicmp(fullPath.c_str(), processPath.c_str()) == 0) return true;
        return false;
    }

    // Window 模式：标题子串匹配
    if (!title.empty()) {
        wchar_t hwndTitle[512] = {};
        GetWindowTextW(hwnd, hwndTitle, 512);
        if (wcslen(hwndTitle) > 0 && wcsstr(hwndTitle, title.c_str()) != nullptr) {
            return true;
        }
    }

    return false;
}

// ============================================================
// 配置文件解析辅助函数
// ============================================================

// 将逗号分隔的整数字符串解析为整数向量
static std::vector<int> ParseCSVInts(const std::wstring& csv) {
    std::vector<int> result;
    std::wstringstream ss(csv);
    std::wstring token;
    while (std::getline(ss, token, L',')) {
        if (!token.empty()) {
            try {
                size_t pos = 0;
                int val = std::stoi(token, &pos);
                if (pos > 0) result.push_back(val);
            } catch (...) {}
        }
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
    // --- 通用设置 ---
    autoStart = GetPrivateProfileIntW(L"General", L"AutoStart", 0, iniPath.c_str()) != 0;
    showLockKeyOverlay = GetPrivateProfileIntW(L"General", L"ShowOverlay", 1, iniPath.c_str()) != 0;
    showMuteOverlay = GetPrivateProfileIntW(L"General", L"ShowMuteOverlay", 0, iniPath.c_str()) != 0;
    overlayPrimaryOnly = GetPrivateProfileIntW(L"General", L"OverlayPrimaryOnly", 0, iniPath.c_str()) != 0;
    forceNumLockOn = GetPrivateProfileIntW(L"General", L"ForceNumLockOn", 0, iniPath.c_str()) != 0;
    disableStartupNotification = GetPrivateProfileIntW(L"General", L"DisableStartupNotification", 0, iniPath.c_str()) != 0;
    explorerHideHidden = GetPrivateProfileIntW(L"General", L"ExplorerHideHidden", 0, iniPath.c_str());
    explorerHideExt = GetPrivateProfileIntW(L"General", L"ExplorerHideExt", 0, iniPath.c_str());
    explorerHideOS = GetPrivateProfileIntW(L"General", L"ExplorerHideOS", 0, iniPath.c_str());
    tabState = GetPrivateProfileIntW(L"General", L"TabState", 0, iniPath.c_str());
    if (tabState < 0 || tabState > 5) tabState = 0;

    // --- 按键禁用设置 ---
    disableSpecifiedKeysEnabled = GetPrivateProfileIntW(L"Keys", L"DisableSpecifiedKeysEnabled", 0, iniPath.c_str()) != 0;
    disableNumLock = GetPrivateProfileIntW(L"Keys", L"DisableNumLock", 0, iniPath.c_str()) != 0;
    forceInsertMode = GetPrivateProfileIntW(L"Keys", L"ForceInsertMode", 0, iniPath.c_str()) != 0;
    auto keysStr = IniUtils::Read(iniPath, L"Keys", L"DisabledKeyCodes");
    disabledKeyCodes = ParseCSVInts(keysStr);

    // --- 按键重映射 ---
    keyRemapEnabled = GetPrivateProfileIntW(L"KeyRemap", L"Enabled", 0, iniPath.c_str()) != 0;
    auto remapStr = IniUtils::Read(iniPath, L"KeyRemap", L"Mappings");
    auto remapInts = ParseCSVInts(remapStr);
    // 重映射以成对形式存储：{源键, 目标键}
    for (size_t i = 0; i + 1 < remapInts.size(); i += 2) {
        keyRemappings.push_back({remapInts[i], remapInts[i + 1]});
    }

    // --- 热键屏蔽 ---
    auto hkStr = IniUtils::Read(iniPath, L"HotkeyBlock", L"DisabledHotkeys");
    auto hkInts = ParseCSVInts(hkStr);
    for (size_t i = 0; i + 1 < hkInts.size(); i += 2) {
        disabledHotkeys.push_back({(UINT)hkInts[i], (UINT)hkInts[i + 1]});
    }

    // --- 自定义热键 ---
    customHotkeysEnabled = GetPrivateProfileIntW(L"CustomHotkeys", L"Enabled", 0, iniPath.c_str()) != 0;
    {
        auto customStr = IniUtils::Read(iniPath, L"CustomHotkeys", L"Entries");
        auto parts = ParseEntryList(customStr);
        customHotkeys.clear();
        for (const auto& p : parts) {
            if (!p.empty()) customHotkeys.push_back(CustomHotkey::Deserialize(p));
        }
    }

    // --- 老板键设置 ---
    bossKeyEnabled = GetPrivateProfileIntW(L"BossKey", L"Enabled", 0, iniPath.c_str()) != 0;
    bossKeyMod = (UINT)GetPrivateProfileIntW(L"BossKey", L"Modifiers", MOD_WIN, iniPath.c_str());
    bossKeyVK = (UINT)GetPrivateProfileIntW(L"BossKey", L"VKCode", 0xC0, iniPath.c_str());
    bossKeyCloseMod = (UINT)GetPrivateProfileIntW(L"BossKey", L"CloseModifiers", MOD_WIN, iniPath.c_str());
    bossKeyCloseVK = (UINT)GetPrivateProfileIntW(L"BossKey", L"CloseVKCode", VK_ESCAPE, iniPath.c_str());
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
void AppConfig::Save(const std::wstring& iniPath) const {
    IniUtils::Write(iniPath, L"General", L"ConfigVersion", std::to_wstring(CONFIG_VERSION));
    IniUtils::Write(iniPath, L"General", L"AutoStart", IniUtils::BoolToStr(autoStart));
    IniUtils::Write(iniPath, L"General", L"ShowOverlay", IniUtils::BoolToStr(showLockKeyOverlay));
    IniUtils::Write(iniPath, L"General", L"ShowMuteOverlay", IniUtils::BoolToStr(showMuteOverlay));
    IniUtils::Write(iniPath, L"General", L"OverlayPrimaryOnly", IniUtils::BoolToStr(overlayPrimaryOnly));
    IniUtils::Write(iniPath, L"General", L"ForceNumLockOn", IniUtils::BoolToStr(forceNumLockOn));
    IniUtils::Write(iniPath, L"General", L"DisableStartupNotification", IniUtils::BoolToStr(disableStartupNotification));
    IniUtils::Write(iniPath, L"General", L"ExplorerHideHidden", std::to_wstring(explorerHideHidden));
    IniUtils::Write(iniPath, L"General", L"ExplorerHideExt", std::to_wstring(explorerHideExt));
    IniUtils::Write(iniPath, L"General", L"ExplorerHideOS", std::to_wstring(explorerHideOS));
    IniUtils::Write(iniPath, L"General", L"TabState", std::to_wstring(tabState));

    IniUtils::Write(iniPath, L"Keys", L"DisableSpecifiedKeysEnabled", IniUtils::BoolToStr(disableSpecifiedKeysEnabled));
    IniUtils::Write(iniPath, L"Keys", L"DisableNumLock", IniUtils::BoolToStr(disableNumLock));
    IniUtils::Write(iniPath, L"Keys", L"ForceInsertMode", IniUtils::BoolToStr(forceInsertMode));
    IniUtils::Write(iniPath, L"Keys", L"DisabledKeyCodes", IntsToCSV(disabledKeyCodes));

    // 按键重映射：将成对映射展平为逗号分隔的整数序列
    IniUtils::Write(iniPath, L"KeyRemap", L"Enabled", IniUtils::BoolToStr(keyRemapEnabled));
    std::vector<int> remapFlat;
    for (auto& p : keyRemappings) { remapFlat.push_back(p.first); remapFlat.push_back(p.second); }
    IniUtils::Write(iniPath, L"KeyRemap", L"Mappings", IntsToCSV(remapFlat));

    // 热键屏蔽
    std::vector<int> hkFlat;
    for (auto& p : disabledHotkeys) { hkFlat.push_back((int)p.first); hkFlat.push_back((int)p.second); }
    IniUtils::Write(iniPath, L"HotkeyBlock", L"DisabledHotkeys", IntsToCSV(hkFlat));

    // 自定义热键
    IniUtils::Write(iniPath, L"CustomHotkeys", L"Enabled", IniUtils::BoolToStr(customHotkeysEnabled));
    {
        std::vector<std::wstring> serialized;
        for (const auto& hk : customHotkeys) serialized.push_back(hk.Serialize());
        IniUtils::Write(iniPath, L"CustomHotkeys", L"Entries", EntryListToString(serialized));
    }

    // 老板键设置
    IniUtils::Write(iniPath, L"BossKey", L"Enabled", IniUtils::BoolToStr(bossKeyEnabled));
    IniUtils::Write(iniPath, L"BossKey", L"Modifiers", std::to_wstring(bossKeyMod));
    IniUtils::Write(iniPath, L"BossKey", L"VKCode", std::to_wstring(bossKeyVK));
    IniUtils::Write(iniPath, L"BossKey", L"CloseModifiers", std::to_wstring(bossKeyCloseMod));
    IniUtils::Write(iniPath, L"BossKey", L"CloseVKCode", std::to_wstring(bossKeyCloseVK));
    IniUtils::Write(iniPath, L"BossKey", L"Mute", IniUtils::BoolToStr(bossKeyMute));
    {
        std::vector<std::wstring> serialized;
        for (const auto& w : bossKeyWindows) serialized.push_back(w.Serialize());
        IniUtils::Write(iniPath, L"BossKey", L"Windows", EntryListToString(serialized));
    }
    IniUtils::Write(iniPath, L"BossKey", L"HideCurrent", IniUtils::BoolToStr(bossKeyHideCurrent));
    IniUtils::Write(iniPath, L"BossKey", L"SendPause", IniUtils::BoolToStr(bossKeySendPause));
    IniUtils::Write(iniPath, L"BossKey", L"MiddleButton", IniUtils::BoolToStr(bossKeyMiddleButton));
    IniUtils::Write(iniPath, L"BossKey", L"SideButton1", IniUtils::BoolToStr(bossKeySideButton1));
    IniUtils::Write(iniPath, L"BossKey", L"SideButton2", IniUtils::BoolToStr(bossKeySideButton2));
    IniUtils::Write(iniPath, L"BossKey", L"CornerTL", IniUtils::BoolToStr(bossKeyCornerTL));
    IniUtils::Write(iniPath, L"BossKey", L"CornerTR", IniUtils::BoolToStr(bossKeyCornerTR));
    IniUtils::Write(iniPath, L"BossKey", L"CornerBL", IniUtils::BoolToStr(bossKeyCornerBL));
    IniUtils::Write(iniPath, L"BossKey", L"CornerBR", IniUtils::BoolToStr(bossKeyCornerBR));
    IniUtils::Write(iniPath, L"BossKey", L"AutoHide", IniUtils::BoolToStr(bossKeyAutoHide));
    IniUtils::Write(iniPath, L"BossKey", L"AutoHideTime", std::to_wstring(bossKeyAutoHideTime));
    IniUtils::Write(iniPath, L"BossKey", L"CloseOnExit", IniUtils::BoolToStr(bossKeyCloseOnExit));

    // WinDGuard 设置
    IniUtils::Write(iniPath, L"WinDGuard", L"Enabled", IniUtils::BoolToStr(winDGuardEnabled));
    {
        std::vector<std::wstring> serialized;
        for (const auto& w : winDGuardWindows) serialized.push_back(w.Serialize());
        IniUtils::Write(iniPath, L"WinDGuard", L"Windows", EntryListToString(serialized));
    }

    // 刷新 INI 文件缓存，确保写入磁盘
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, iniPath.c_str());
}

// 判断指定虚拟键码是否被禁用（包括 NumLock 特殊处理）
bool AppConfig::IsKeyDisabled(int vkCode) const {
    if (disableNumLock && vkCode == VK_NUMLOCK) return true;
    return std::find(disabledKeyCodes.begin(), disabledKeyCodes.end(), vkCode) != disabledKeyCodes.end();
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
    LPARAM lParam = (LPARAM)scanCode << 16;
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
