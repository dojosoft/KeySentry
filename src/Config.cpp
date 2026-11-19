#include "Config.h"
#include <winuser.h>
#include <algorithm>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

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
        // 提取可执行文件名用于匹配（统一使用 ProcessUtils 的路径拆分）
        const std::wstring exeName = ProcessUtils::PathFileName(fullPath);
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

// 带进程路径缓存的匹配：仅 Process 模式走缓存，避免每个窗口都 OpenProcess 查询路径。
// Window 模式优先复用 cachedTitle，同一次窗口枚举内只读一次标题
bool BoundWindowInfo::Matches(HWND hwnd, ProcessUtils::PathCache& cache, bool requireVisible,
                              const std::wstring* cachedTitle) const {
    if (!IsWindow(hwnd)) return false;
    if (requireVisible && !IsWindowVisible(hwnd)) return false;

    if (matchMode != MatchMode::Process) {
        if (title.empty()) return false;
        const std::wstring& hwndTitle = cachedTitle ? *cachedTitle
                                                    : WindowUtils::GetTextSafe(hwnd);
        return !hwndTitle.empty() && hwndTitle.find(title) != std::wstring::npos;
    }

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
        // 手改 INI 常带尾随空格（如 "65, 66 "），先 Trim 再完整解析，
        // 避免合法值被静默丢弃
        const size_t first = token.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos) continue;
        const size_t last = token.find_last_not_of(L" \t\r\n");
        token = token.substr(first, last - first + 1);
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
// 按键映射解析（V3 四元组 / V2 三元组 / 旧版成对格式）
// ============================================================

// 三元组集合（忽略互换组），用于判断 V2 与 V3 是否描述同一份映射。
// V2 无法表达"修饰键作源"的条目（ParseRemapV2 会丢弃），比较时两侧都必须排除，
// 否则 CapsLock ⇄ Ctrl 这类互换每次重启都会被误判为"被旧版本改过"而丢掉组号
static std::vector<std::tuple<int, unsigned, int>>
RemapTriples(const std::vector<KeyRemapEntry>& entries) {
    std::vector<std::tuple<int, unsigned, int>> out;
    out.reserve(entries.size());
    for (const auto& r : entries) {
        if (ConfigUtils::IsModifierKey(r.sourceVK)) continue;
        out.emplace_back(r.sourceVK, r.mods, r.targetVK);
    }
    std::sort(out.begin(), out.end());
    return out;
}

static bool SameRemapTriples(const std::vector<KeyRemapEntry>& a,
                             const std::vector<KeyRemapEntry>& b) {
    return RemapTriples(a) == RemapTriples(b);
}

// 从普通映射中识别互为反向的按键对，恢复为互换组：
// 旧版本不认识 MappingsV3，修改映射后只更新 MappingsV2，组号会丢失
static void DetectSwapPairs(std::vector<KeyRemapEntry>& entries) {
    int nextGroup = 1;
    for (const auto& e : entries)
        if (e.swapGroup >= nextGroup) nextGroup = e.swapGroup + 1;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].swapGroup != 0 || entries[i].mods != 0) continue;
        for (size_t j = i + 1; j < entries.size(); ++j) {
            if (entries[j].swapGroup != 0 || entries[j].mods != 0) continue;
            if (entries[i].sourceVK == entries[j].targetVK &&
                entries[i].targetVK == entries[j].sourceVK) {
                entries[i].swapGroup = entries[j].swapGroup = nextGroup++;
                break;
            }
        }
    }
}

// 校验互换组：必须恰好两条、无修饰键且互为反向；
// 否则把组号清零降级为普通映射，避免手改 INI 后"一行代表三条记录"或误删无关条目
static void NormalizeSwapGroups(std::vector<KeyRemapEntry>& entries) {
    // 按组号一次分组，避免 "组数 × 条目数" 的嵌套扫描
    std::unordered_map<int, std::vector<size_t>> groups;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].swapGroup > 0) groups[entries[i].swapGroup].push_back(i);
    }
    for (auto& group : groups) {
        std::vector<size_t>& indices = group.second;
        bool valid = (indices.size() == 2);
        if (valid) {
            const KeyRemapEntry& a = entries[indices[0]];
            const KeyRemapEntry& b = entries[indices[1]];
            valid = a.mods == 0 && b.mods == 0 &&
                    a.sourceVK == b.targetVK && a.targetVK == b.sourceVK;
        }
        if (!valid) {
            for (size_t i : indices) entries[i].swapGroup = 0;
        }
    }
}

// MappingsV3：四元组 {源键, 修饰键, 目标键, 互换组}
static std::vector<KeyRemapEntry> ParseRemapV3(const std::vector<int>& ints) {
    std::vector<KeyRemapEntry> entries;
    std::unordered_set<int> seenSources;
    for (size_t i = 0; i + 3 < ints.size(); i += 4) {
        if ((int)entries.size() >= MAX_KEY_REMAPS) break;
        int src = ints[i];
        UINT mods = ConfigUtils::SanitizeMods(ints[i + 1]);
        int tgt = ints[i + 2];
        int swapGroup = ints[i + 3];
        if (!ConfigUtils::IsValidVK(src) || !ConfigUtils::IsValidVK(tgt)) continue;
        if (swapGroup < 0) swapGroup = 0;
        // 普通映射禁止修饰键作源键；互换组允许（如 Ctrl↔Alt）
        if (swapGroup == 0 && ConfigUtils::IsModifierKey(src)) continue;
        if (mods == 0 && src == tgt) continue;
        if (!seenSources.insert(src).second) continue;
        entries.push_back({ src, mods, tgt, swapGroup });
    }
    NormalizeSwapGroups(entries);
    return entries;
}

// MappingsV2：三元组 {源键, 修饰键, 目标键}（无互换组信息）
static std::vector<KeyRemapEntry> ParseRemapV2(const std::vector<int>& ints) {
    std::vector<KeyRemapEntry> entries;
    std::unordered_set<int> seenSources;
    for (size_t i = 0; i + 2 < ints.size(); i += 3) {
        if ((int)entries.size() >= MAX_KEY_REMAPS) break;
        int src = ints[i];
        UINT mods = ConfigUtils::SanitizeMods(ints[i + 1]);
        int tgt = ints[i + 2];
        // 丢弃非法键码：保留会让源键被吞掉却模拟不出目标键，等于该键彻底失效
        if (!ConfigUtils::IsValidVK(src) || !ConfigUtils::IsValidVK(tgt)) continue;
        // 修饰键作源键会让整个 Ctrl/Shift/Alt/Win 失效，直接丢弃
        if (ConfigUtils::IsModifierKey(src)) continue;
        if (mods == 0 && src == tgt) continue;
        if (!seenSources.insert(src).second) continue;
        entries.push_back({ src, mods, tgt });
    }
    return entries;
}

// 旧版 Mappings：成对 {源键, 目标键}，等价于 mods = 0
static std::vector<KeyRemapEntry> ParseRemapLegacy(const std::vector<int>& ints) {
    std::vector<KeyRemapEntry> entries;
    std::unordered_set<int> seenSources;
    for (size_t i = 0; i + 1 < ints.size(); i += 2) {
        if ((int)entries.size() >= MAX_KEY_REMAPS) break;
        int src = ints[i];
        int tgt = ints[i + 1];
        if (!ConfigUtils::IsValidVK(src) || !ConfigUtils::IsValidVK(tgt)) continue;
        if (ConfigUtils::IsModifierKey(src)) continue;
        if (src == tgt) continue;
        if (!seenSources.insert(src).second) continue;
        entries.push_back({ src, 0, tgt });
    }
    return entries;
}

// 校验并规范化从 INI 读入的窗口绑定：Trim 字段、丢弃空标题/空进程名、
// 截断超长标题并限制条目数量，避免畸形条目匹配到大量无关窗口
static void AppendValidatedBinding(std::vector<BoundWindowInfo>& list, const std::wstring& raw) {
    if (raw.empty() || (int)list.size() >= MAX_BOUND_WINDOWS) return;
    BoundWindowInfo info = BoundWindowInfo::Deserialize(raw);
    auto trim = [](std::wstring& s) {
        const size_t first = s.find_first_not_of(L" \t\r\n");
        const size_t last = s.find_last_not_of(L" \t\r\n");
        if (first == std::wstring::npos) { s.clear(); return; }
        s = s.substr(first, last - first + 1);
    };
    trim(info.title);
    trim(info.processName);
    trim(info.processPath);
    info.ClampFields();
    if (info.matchMode == MatchMode::Process) {
        if (info.processName.empty() && info.processPath.empty()) return;
        info.title.clear();
    } else {
        if (info.title.empty()) return;
    }
    for (const auto& existing : list) {
        if (existing.matchMode == info.matchMode &&
            _wcsicmp(existing.title.c_str(), info.title.c_str()) == 0 &&
            _wcsicmp(existing.processName.c_str(), info.processName.c_str()) == 0 &&
            _wcsicmp(existing.processPath.c_str(), info.processPath.c_str()) == 0) {
            return;
        }
    }
    list.push_back(std::move(info));
}

// ============================================================
// 配置分段加载辅助
// 说明：以下函数按原 Load 的读取顺序逐段拆分，键名、默认值与
//       取值收敛规则完全保持原样，仅用于降低单函数长度（R13-A13）。
// ============================================================

// [General] 通用设置/外观/资源管理器/选项卡状态。
// 返回 ConfigVersion：后续 [BossKey] 的默认热键迁移依赖该版本号
static int LoadGeneralSection(AppConfig& cfg, const std::wstring& iniPath) {
    const wchar_t* path = iniPath.c_str();
    const int configVersion = GetPrivateProfileIntW(L"General", L"ConfigVersion", 0, path);
    cfg.autoStart = GetPrivateProfileIntW(L"General", L"AutoStart", 0, path) != 0;
    cfg.showLockKeyOverlay = GetPrivateProfileIntW(L"General", L"ShowOverlay", 1, path) != 0;
    cfg.showMuteOverlay = GetPrivateProfileIntW(L"General", L"ShowMuteOverlay", 0, path) != 0;
    cfg.overlayPrimaryOnly = GetPrivateProfileIntW(L"General", L"OverlayPrimaryOnly", 0, path) != 0;
    // 浮窗外观：越界值（手改 INI）统一收敛到默认，避免绘制/下拉框出现非法状态。
    // 旧版本（v1.40 及更早）的 INI 不含这些键，将按默认值读取（深色/底部/圆角/低透明/适中），
    // 不影响其余字段；新版写入的这些键在旧版本中会被当作未知键忽略
    cfg.overlayPosition = GetPrivateProfileIntW(L"General", L"OverlayPosition", 0, path);
    if (cfg.overlayPosition < 0 || cfg.overlayPosition > 4) cfg.overlayPosition = 0;
    cfg.overlayTheme = GetPrivateProfileIntW(L"General", L"OverlayTheme", 0, path);
    if (cfg.overlayTheme < 0 || cfg.overlayTheme > 1) cfg.overlayTheme = 0;
    cfg.overlayCorner = GetPrivateProfileIntW(L"General", L"OverlayCorner", 1, path);
    if (cfg.overlayCorner < 0 || cfg.overlayCorner > 1) cfg.overlayCorner = 1;
    cfg.overlayOpacity = GetPrivateProfileIntW(L"General", L"OverlayOpacity", 2, path);
    if (cfg.overlayOpacity < 0 || cfg.overlayOpacity > 3) cfg.overlayOpacity = 2;
    cfg.overlayDelay = GetPrivateProfileIntW(L"General", L"OverlayDelay", 1, path);
    if (cfg.overlayDelay < 0 || cfg.overlayDelay > 3) cfg.overlayDelay = 1;
    cfg.forceNumLockOn = GetPrivateProfileIntW(L"General", L"ForceNumLockOn", 0, path) != 0;
    cfg.disableStartupNotification = GetPrivateProfileIntW(L"General", L"DisableStartupNotification", 0, path) != 0;
    cfg.appTheme = GetPrivateProfileIntW(L"General", L"AppTheme", 0, path);
    if (cfg.appTheme < 0 || cfg.appTheme > 2) cfg.appTheme = 0;
    cfg.appLanguage = GetPrivateProfileIntW(L"General", L"AppLanguage", 0, path);
    if (cfg.appLanguage < 0 || cfg.appLanguage > 1) cfg.appLanguage = 0;
    // 资源管理器选项：合法值 0=保持原状 / 1=开启隐藏 / 2=关闭隐藏。
    // 越界值（手改 INI 等）一律归零：否则下拉框无选中项，CB_GETCURSEL 回写 -1，
    // 会让 ApplyExplorerSettings 的"三项均为 0 则跳过"判断失效，多一次注册表打开与广播
    auto clampExplorerOpt = [](int v) { return (v >= 0 && v <= 2) ? v : 0; };
    cfg.explorerHideHidden = clampExplorerOpt(GetPrivateProfileIntW(L"General", L"ExplorerHideHidden", 0, path));
    cfg.explorerHideExt = clampExplorerOpt(GetPrivateProfileIntW(L"General", L"ExplorerHideExt", 0, path));
    cfg.explorerHideOS = clampExplorerOpt(GetPrivateProfileIntW(L"General", L"ExplorerHideOS", 0, path));
    cfg.tabState = GetPrivateProfileIntW(L"General", L"TabState", 0, path);
    if (cfg.tabState < 0 || cfg.tabState > 5) cfg.tabState = 0;
    return configVersion;
}

// [Keys] 按键禁用设置
static void LoadKeysSection(AppConfig& cfg, const std::wstring& iniPath) {
    const wchar_t* path = iniPath.c_str();
    cfg.disableSpecifiedKeysEnabled = GetPrivateProfileIntW(L"Keys", L"DisableSpecifiedKeysEnabled", 0, path) != 0;
    cfg.disableNumLock = GetPrivateProfileIntW(L"Keys", L"DisableNumLock", 0, path) != 0;
    auto keysStr = IniUtils::Read(iniPath, L"Keys", L"DisabledKeyCodes");
    cfg.disabledKeyCodes.clear();  // 重复载入时避免沿用上一次的禁用项
    for (int vk : ParseCSVInts(keysStr)) {
        // 与重映射/自定义热键保持一致的合法性过滤：非法键码永不匹配真实按键，
        // 留在列表里只会显示成 "0x0" 这类无意义条目
        if (!ConfigUtils::IsValidVK(vk)) continue;
        // 手改 INI 或误捕获写入的 Ctrl/Shift/Alt 会让全部组合键失效，直接丢弃
        if (ConfigUtils::IsDangerousDisabledKey(vk)) continue;
        // 去重：重复键码在列表里显示多行，却只对应同一个按键
        if (std::find(cfg.disabledKeyCodes.begin(), cfg.disabledKeyCodes.end(), vk) != cfg.disabledKeyCodes.end())
            continue;
        if ((int)cfg.disabledKeyCodes.size() >= MAX_DISABLED_KEYS) break;
        cfg.disabledKeyCodes.push_back(vk);
    }
}

// [KeyRemap] 按键重映射：V3/V2/旧版格式的兼容与合并
static void LoadKeyRemapSection(AppConfig& cfg, const std::wstring& iniPath) {
    const wchar_t* path = iniPath.c_str();
    cfg.keyRemapEnabled = GetPrivateProfileIntW(L"KeyRemap", L"Enabled", 0, path) != 0;
    cfg.keyRemappings.clear();   // 重复载入时避免沿用上一次的映射项

    const auto v3Entries = ParseRemapV3(
        ParseCSVInts(IniUtils::Read(iniPath, L"KeyRemap", L"MappingsV3")));
    const auto v2Entries = ParseRemapV2(
        ParseCSVInts(IniUtils::Read(iniPath, L"KeyRemap", L"MappingsV2")));
    const auto legacyEntries = ParseRemapLegacy(
        ParseCSVInts(IniUtils::Read(iniPath, L"KeyRemap", L"Mappings")));

    if (!v3Entries.empty() && !v2Entries.empty()) {
        // 两种格式同时存在：若三元组不一致，说明配置被旧版本修改过
        // （旧版本只更新 MappingsV2）。此时以 V2 为准并重新识别互换对，
        // 避免用户的修改被过期的 V3 覆盖
        if (!SameRemapTriples(v3Entries, v2Entries)) {
            std::vector<KeyRemapEntry> reconciled = v2Entries;
            // V2 无法表达"修饰键作源"的条目：旧版本改写 V2 后，
            // CapsLock ⇄ Ctrl 这类互换会只剩一个方向，必须从 V3 补回反向条目。
            // 补回时清空组号（V2 条目本身也不带组号），交给下面的
            // DetectSwapPairs 与另一个方向重新配对，避免留下"半组"记录
            for (const auto& e : v3Entries) {
                if (!ConfigUtils::IsModifierKey(e.sourceVK)) continue;
                bool exists = false;
                for (const auto& r : reconciled) {
                    if (r.sourceVK == e.sourceVK) { exists = true; break; }
                }
                if (exists) continue;
                KeyRemapEntry merged = e;
                merged.swapGroup = 0;
                reconciled.push_back(merged);
            }
            DetectSwapPairs(reconciled);
            cfg.keyRemappings = std::move(reconciled);
        } else {
            cfg.keyRemappings = v3Entries;   // 一致时保留 V3 的互换组信息
        }
    } else if (!v3Entries.empty()) {
        cfg.keyRemappings = v3Entries;
    } else if (!v2Entries.empty()) {
        std::vector<KeyRemapEntry> reconciled = v2Entries;
        DetectSwapPairs(reconciled);
        cfg.keyRemappings = std::move(reconciled);
    } else {
        cfg.keyRemappings = legacyEntries;
    }
    // 映射条目上限：超出部分丢弃，避免钩子回调对每次按键做超长线性扫描
    if ((int)cfg.keyRemappings.size() > MAX_KEY_REMAPS) {
        cfg.keyRemappings.resize(MAX_KEY_REMAPS);
    }
}

// [CustomHotkeys] 自定义热键：条目解析、字段收敛与去重
static void LoadCustomHotkeysSection(AppConfig& cfg, const std::wstring& iniPath) {
    const wchar_t* path = iniPath.c_str();
    cfg.customHotkeysEnabled = GetPrivateProfileIntW(L"CustomHotkeys", L"Enabled", 0, path) != 0;
    auto customStr = IniUtils::Read(iniPath, L"CustomHotkeys", L"Entries");
    auto parts = ParseEntryList(customStr);
    cfg.customHotkeys.clear();
    for (const auto& p : parts) {
        if (p.empty()) continue;
        CustomHotkey hk = CustomHotkey::Deserialize(p);
        // 丢弃手改 INI 产生的非法条目：无修饰键、无主键或修饰键作主键时
        // 注册会失败/劫持普通按键。规则必须与设置页保持一致。
        hk.mod = ConfigUtils::SanitizeMods((int)hk.mod);
        if (hk.mod == 0 || !ConfigUtils::IsValidVK((int)hk.vk) ||
            ConfigUtils::IsModifierKey((int)hk.vk)) continue;
        // 文本字段收敛到设置页允许的上限：手改 INI 可写入数 MB 字段，
        // 不截断会让冲突提示/列表构造出现超大消息（R10-M3）
        auto clampText = [](std::wstring& s, size_t limit) {
            if (s.size() > limit) s.resize(limit);
        };
        clampText(hk.command, MAX_CUSTOM_COMMAND_CHARS);
        clampText(hk.parameters, MAX_CUSTOM_PARAMS_CHARS);
        clampText(hk.workDir, MAX_CUSTOM_WORKDIR_CHARS);
        clampText(hk.description, MAX_CUSTOM_DESC_CHARS);
        // 模拟按键模式下目标键非法时动作必然失败，同样丢弃。
        // 命令型热键不保留模拟目标字段：历史/手改配置可能残留 targetMod，
        // 继续序列化会写出 12 字段并增加新旧格式判别歧义。
        if (!hk.simulateKey) {
            hk.targetVK = 0;
            hk.targetMod = 0;
        } else {
            if (!ConfigUtils::IsValidVK((int)hk.targetVK)) continue;
            // 模拟组合键的修饰键位收敛；修饰键不能作为组合的主键
            hk.targetMod = ConfigUtils::SanitizeMods((int)hk.targetMod);
            if (hk.targetMod != 0 && ConfigUtils::IsModifierKey((int)hk.targetVK)) hk.targetMod = 0;
        }
        // 丢弃重复组合（mod+vk 相同只保留第一条），避免注册失败连环弹窗
        bool duplicated = false;
        for (const auto& existing : cfg.customHotkeys) {
            if (existing.mod == hk.mod && existing.vk == hk.vk) { duplicated = true; break; }
        }
        if (duplicated) continue;
        cfg.customHotkeys.push_back(hk);
        // 超出注册上限的条目直接丢弃（保存时即收敛为合法配置）
        if ((int)cfg.customHotkeys.size() >= MAX_CUSTOM_HOTKEYS) break;
    }
}

// [BossKey] 老板键设置。configVersion 用于判断是否需要补上默认关闭热键
static void LoadBossKeySection(AppConfig& cfg, const std::wstring& iniPath, int configVersion) {
    const wchar_t* path = iniPath.c_str();
    cfg.bossKeyEnabled = GetPrivateProfileIntW(L"BossKey", L"Enabled", 0, path) != 0;
    // 手改 INI 的非法修饰键/虚拟键码会让 RegisterHotKey 永远失败，载入时统一收敛
    cfg.bossKeyMod = ConfigUtils::SanitizeMods(GetPrivateProfileIntW(L"BossKey", L"Modifiers", MOD_WIN, path));
    {
        int vkVal = GetPrivateProfileIntW(L"BossKey", L"VKCode", 0xC0, path);
        cfg.bossKeyVK = ConfigUtils::IsValidVK(vkVal) ? (UINT)vkVal : 0xC0;
    }
    cfg.bossKeyCloseMod = ConfigUtils::SanitizeMods(
        GetPrivateProfileIntW(L"BossKey", L"CloseModifiers", MOD_WIN, path));
    {
        int vkVal = GetPrivateProfileIntW(L"BossKey", L"CloseVKCode", VK_ESCAPE, path);
        cfg.bossKeyCloseVK = (vkVal == 0) ? 0
                             : (ConfigUtils::IsValidVK(vkVal) ? (UINT)vkVal : 0);
    }
    // v5 起恢复“一键关闭程序热键”默认值 Win+Esc：v4 曾因与系统退出放大镜
    // 冲突而默认清空，升级后若用户从未设置（任一侧为 0）则补上默认组合；
    // 用户已自定义的组合保持不变
    if (configVersion < 5 && (cfg.bossKeyCloseMod == 0 || cfg.bossKeyCloseVK == 0)) {
        cfg.bossKeyCloseMod = MOD_WIN;
        cfg.bossKeyCloseVK = VK_ESCAPE;
    }
    cfg.bossKeyMute = GetPrivateProfileIntW(L"BossKey", L"Mute", 0, path) != 0;
    auto bossWinsStr = IniUtils::Read(iniPath, L"BossKey", L"Windows");
    {
        auto parts = ParseEntryList(bossWinsStr);
        cfg.bossKeyWindows.clear();
        for (const auto& p : parts) {
            AppendValidatedBinding(cfg.bossKeyWindows, p);
        }
    }
    cfg.bossKeyHideCurrent = GetPrivateProfileIntW(L"BossKey", L"HideCurrent", 0, path) != 0;
    cfg.bossKeySendPause = GetPrivateProfileIntW(L"BossKey", L"SendPause", 0, path) != 0;
    cfg.bossKeyMiddleButton = GetPrivateProfileIntW(L"BossKey", L"MiddleButton", 0, path) != 0;
    cfg.bossKeySideButton1 = GetPrivateProfileIntW(L"BossKey", L"SideButton1", 0, path) != 0;
    cfg.bossKeySideButton2 = GetPrivateProfileIntW(L"BossKey", L"SideButton2", 0, path) != 0;
    cfg.bossKeyCornerTL = GetPrivateProfileIntW(L"BossKey", L"CornerTL", 0, path) != 0;
    cfg.bossKeyCornerTR = GetPrivateProfileIntW(L"BossKey", L"CornerTR", 0, path) != 0;
    cfg.bossKeyCornerBL = GetPrivateProfileIntW(L"BossKey", L"CornerBL", 0, path) != 0;
    cfg.bossKeyCornerBR = GetPrivateProfileIntW(L"BossKey", L"CornerBR", 0, path) != 0;
    cfg.bossKeyAutoHide = GetPrivateProfileIntW(L"BossKey", L"AutoHide", 0, path) != 0;
    cfg.bossKeyAutoHideTime = GetPrivateProfileIntW(L"BossKey", L"AutoHideTime", 5, path);
    if (cfg.bossKeyAutoHideTime < 1) cfg.bossKeyAutoHideTime = 1;
    if (cfg.bossKeyAutoHideTime > MAX_AUTO_HIDE_MINUTES) cfg.bossKeyAutoHideTime = MAX_AUTO_HIDE_MINUTES;
    cfg.bossKeyCloseOnExit = GetPrivateProfileIntW(L"BossKey", L"CloseOnExit", 0, path) != 0;
}

// [WinDGuard] 窗口保护：新格式（;;/|）与旧版纯标题格式的兼容解析
static void LoadWinDGuardSection(AppConfig& cfg, const std::wstring& iniPath) {
    const wchar_t* path = iniPath.c_str();
    cfg.winDGuardEnabled = GetPrivateProfileIntW(L"WinDGuard", L"Enabled", 0, path) != 0;
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
            // 必须整段比较：旧格式多条目用 | 分隔纯标题，标题可能以数字开头
            // （如 "1Password|SomeApp"），前缀式解析会把它误判成新格式
            if (firstField == L"0" || firstField == L"1") {
                isNewFormat = true;
            }
        }
    }

    if (isNewFormat) {
        auto parts = ParseEntryList(windWinsStr);
        cfg.winDGuardWindows.clear();
        for (const auto& p : parts) {
            AppendValidatedBinding(cfg.winDGuardWindows, p);
        }
    } else {
        auto parts = ParsePipes(windWinsStr);
        cfg.winDGuardWindows.clear();
        for (const auto& p : parts) {
            if ((int)cfg.winDGuardWindows.size() >= MAX_BOUND_WINDOWS) break;
            // 旧格式是纯标题：不能走管道字段解析，单独 Trim 后使用
            const size_t first = p.find_first_not_of(L" \t\r\n");
            const size_t last = p.find_last_not_of(L" \t\r\n");
            if (first == std::wstring::npos) continue;
            std::wstring title = p.substr(first, last - first + 1);
            if (title.empty()) continue;
            if (title.size() > (size_t)MAX_BINDING_TITLE) title.resize((size_t)MAX_BINDING_TITLE);
            BoundWindowInfo info;
            info.title = std::move(title);
            info.matchMode = MatchMode::Window;
            cfg.winDGuardWindows.push_back(std::move(info));
        }
    }
}

// ============================================================
// AppConfig::Load - 从 INI 文件加载所有配置项
// ============================================================
void AppConfig::Load(const std::wstring& iniPath) {
    // 先探测配置文件是否可读：文件存在但打不开（权限/占用）时，
    // 下面所有读取都会静默退回默认值，必须记住这个状态，禁止后续保存覆盖原文件
    loadFailed = false;
    loadFailedEncoding = false;
    {
        const DWORD attrs = GetFileAttributesW(iniPath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            HANDLE hCheck = CreateFileW(iniPath.c_str(), GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hCheck == INVALID_HANDLE_VALUE) {
                loadFailed = true;
            } else {
                // 校验 UTF-16LE BOM：手改成 UTF-8/ANSI 的文件会被 profile API 按
                // ANSI 解析成乱码，随后保存又以 UTF-16 固化，必须拒绝加载并保留原文件。
                // 空文件视为“无配置”，不判失败
                WORD bom = 0;
                DWORD bomRead = 0;
                if (ReadFile(hCheck, &bom, sizeof(bom), &bomRead, nullptr) &&
                    bomRead == sizeof(bom) && bom != 0xFEFF) {
                    loadFailed = true;
                    // 单独标记编码问题：常见于用户用记事本保存为 UTF-8/ANSI，
                    // 启动提示需要据此给出"另存为 UTF-16 LE"的可操作指引（R11-M3）
                    loadFailedEncoding = true;
                }
                CloseHandle(hCheck);
            }
        }
    }
    // --- 加载策略说明 ---
    // 采取「尽力读取」：无论 INI 由哪个版本写入，都按当前已知字段逐个读取，
    // 未知或已废弃的字段自然被忽略，不会因版本号更高而整体放弃配置。
    // 保存时再按最新配置规范重写（见 Save），实现配置格式的自动收敛。
    if (loadFailed) {
        // 读取失败（编码不符或文件不可读）时不能继续解析：profile API 会把
        // UTF-8/ANSI 文件按 ANSI 读取，其中 ASCII 字段仍会生效，与启动提示
        // “本次以默认设置运行”矛盾，乱码字段还可能被注册为热键（R13-M1）。
        // 重置为默认值保证行为与提示一致；保留失败标志以继续阻止保存覆盖原文件
        const bool encodingFailure = loadFailedEncoding;
        *this = AppConfig{};
        loadFailed = true;
        loadFailedEncoding = encodingFailure;
        return;
    }

    // --- 分段读取：顺序与键名同原实现，仅做代码组织（R13-A13） ---
    const int configVersion = LoadGeneralSection(*this, iniPath);

    // --- 按键禁用设置 ---
    LoadKeysSection(*this, iniPath);

    // --- 按键重映射 ---
    LoadKeyRemapSection(*this, iniPath);

    // --- 自定义热键 ---
    LoadCustomHotkeysSection(*this, iniPath);

    // --- 老板键设置 ---
    LoadBossKeySection(*this, iniPath, configVersion);

    // --- WinDGuard 窗口保护 ---
    LoadWinDGuardSection(*this, iniPath);
}

// ============================================================
// 配置分段保存辅助
// 说明：Save 的各个 put 调用集中到 SaveConfigSections，写出顺序与
//       原实现一致；长字段序列化结果一并返回，供保存后的回读校验使用。
// ============================================================
namespace {
    // 写入器：统一记录 profile API 的写失败（该 API 的失败不一定通过
    // 返回值暴露，调用方必须自行汇总）
    struct IniWriter {
        const std::wstring& path;
        bool ok = true;

        void Put(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
            if (!IniUtils::Write(path, section, key, value)) ok = false;
        }
    };

    // 需要回读校验的长字段：profile API 对单值长度有限制，超限时可能静默截断
    struct SerializedConfigFields {
        std::wstring disabledKeysCsv;
        std::wstring remapV3Csv;
        std::wstring customEntries;
        std::wstring bossWindowsEntries;
        std::wstring windWindowsEntries;
    };

    SerializedConfigFields SaveConfigSections(IniWriter& w, const AppConfig& cfg) {
        SerializedConfigFields out;

        w.Put(L"General", L"ConfigVersion", std::to_wstring(CONFIG_VERSION));
        w.Put(L"General", L"AutoStart", IniUtils::BoolToStr(cfg.autoStart));
        w.Put(L"General", L"ShowOverlay", IniUtils::BoolToStr(cfg.showLockKeyOverlay));
        w.Put(L"General", L"ShowMuteOverlay", IniUtils::BoolToStr(cfg.showMuteOverlay));
        w.Put(L"General", L"OverlayPrimaryOnly", IniUtils::BoolToStr(cfg.overlayPrimaryOnly));
        w.Put(L"General", L"OverlayPosition", std::to_wstring(cfg.overlayPosition));
        w.Put(L"General", L"OverlayTheme", std::to_wstring(cfg.overlayTheme));
        w.Put(L"General", L"OverlayCorner", std::to_wstring(cfg.overlayCorner));
        w.Put(L"General", L"OverlayOpacity", std::to_wstring(cfg.overlayOpacity));
        w.Put(L"General", L"OverlayDelay", std::to_wstring(cfg.overlayDelay));
        w.Put(L"General", L"ForceNumLockOn", IniUtils::BoolToStr(cfg.forceNumLockOn));
        w.Put(L"General", L"DisableStartupNotification", IniUtils::BoolToStr(cfg.disableStartupNotification));
        w.Put(L"General", L"AppTheme", std::to_wstring(cfg.appTheme));
        w.Put(L"General", L"AppLanguage", std::to_wstring(cfg.appLanguage));
        w.Put(L"General", L"ExplorerHideHidden", std::to_wstring(cfg.explorerHideHidden));
        w.Put(L"General", L"ExplorerHideExt", std::to_wstring(cfg.explorerHideExt));
        w.Put(L"General", L"ExplorerHideOS", std::to_wstring(cfg.explorerHideOS));
        w.Put(L"General", L"TabState", std::to_wstring(cfg.tabState));

        w.Put(L"Keys", L"DisableSpecifiedKeysEnabled", IniUtils::BoolToStr(cfg.disableSpecifiedKeysEnabled));
        w.Put(L"Keys", L"DisableNumLock", IniUtils::BoolToStr(cfg.disableNumLock));
        out.disabledKeysCsv = IntsToCSV(cfg.disabledKeyCodes);
        w.Put(L"Keys", L"DisabledKeyCodes", out.disabledKeysCsv);

        // 按键重映射：V3 四元组 {源键, 修饰键, 目标键, 互换组}；
        // 同时写 V2 三元组与旧版成对格式，保证旧版本仍能读取（互换会退化为两条独立映射）
        w.Put(L"KeyRemap", L"Enabled", IniUtils::BoolToStr(cfg.keyRemapEnabled));
        std::vector<int> remapV3Flat;
        std::vector<int> remapV2Flat;
        std::vector<int> remapLegacyFlat;
        for (const auto& r : cfg.keyRemappings) {
            remapV3Flat.push_back(r.sourceVK);
            remapV3Flat.push_back((int)r.mods);
            remapV3Flat.push_back(r.targetVK);
            remapV3Flat.push_back(r.swapGroup);
            remapV2Flat.push_back(r.sourceVK);
            remapV2Flat.push_back((int)r.mods);
            remapV2Flat.push_back(r.targetVK);
            // 旧版格式只有成对按键，且旧版本可能不认识"修饰键作源"的互换：
            // 写入 Ctrl→X 会让旧版本拦截整个 Ctrl 键，故这类条目只保留在 V3 中
            if (r.mods == 0 && !ConfigUtils::IsModifierKey(r.sourceVK)) {
                remapLegacyFlat.push_back(r.sourceVK);
                remapLegacyFlat.push_back(r.targetVK);
            }
        }
        out.remapV3Csv = IntsToCSV(remapV3Flat);
        const std::wstring remapV2Csv = IntsToCSV(remapV2Flat);
        const std::wstring remapLegacyCsv = IntsToCSV(remapLegacyFlat);
        w.Put(L"KeyRemap", L"MappingsV3", out.remapV3Csv);
        w.Put(L"KeyRemap", L"MappingsV2", remapV2Csv);
        w.Put(L"KeyRemap", L"Mappings", remapLegacyCsv);

        // 自定义热键：始终按当前序列化规范整体重写，
        // 旧版本遗留的废弃字段（sound/opacity/autoStart）在此被丢弃
        w.Put(L"CustomHotkeys", L"Enabled", IniUtils::BoolToStr(cfg.customHotkeysEnabled));
        {
            std::vector<std::wstring> serialized;
            for (const auto& hk : cfg.customHotkeys) serialized.push_back(hk.Serialize());
            out.customEntries = EntryListToString(serialized);
            w.Put(L"CustomHotkeys", L"Entries", out.customEntries);
        }

        // 老板键设置
        w.Put(L"BossKey", L"Enabled", IniUtils::BoolToStr(cfg.bossKeyEnabled));
        w.Put(L"BossKey", L"Modifiers", std::to_wstring(cfg.bossKeyMod));
        w.Put(L"BossKey", L"VKCode", std::to_wstring(cfg.bossKeyVK));
        w.Put(L"BossKey", L"CloseModifiers", std::to_wstring(cfg.bossKeyCloseMod));
        w.Put(L"BossKey", L"CloseVKCode", std::to_wstring(cfg.bossKeyCloseVK));
        w.Put(L"BossKey", L"Mute", IniUtils::BoolToStr(cfg.bossKeyMute));
        {
            std::vector<std::wstring> serialized;
            for (const auto& bw : cfg.bossKeyWindows) serialized.push_back(bw.Serialize());
            out.bossWindowsEntries = EntryListToString(serialized);
            w.Put(L"BossKey", L"Windows", out.bossWindowsEntries);
        }
        w.Put(L"BossKey", L"HideCurrent", IniUtils::BoolToStr(cfg.bossKeyHideCurrent));
        w.Put(L"BossKey", L"SendPause", IniUtils::BoolToStr(cfg.bossKeySendPause));
        w.Put(L"BossKey", L"MiddleButton", IniUtils::BoolToStr(cfg.bossKeyMiddleButton));
        w.Put(L"BossKey", L"SideButton1", IniUtils::BoolToStr(cfg.bossKeySideButton1));
        w.Put(L"BossKey", L"SideButton2", IniUtils::BoolToStr(cfg.bossKeySideButton2));
        w.Put(L"BossKey", L"CornerTL", IniUtils::BoolToStr(cfg.bossKeyCornerTL));
        w.Put(L"BossKey", L"CornerTR", IniUtils::BoolToStr(cfg.bossKeyCornerTR));
        w.Put(L"BossKey", L"CornerBL", IniUtils::BoolToStr(cfg.bossKeyCornerBL));
        w.Put(L"BossKey", L"CornerBR", IniUtils::BoolToStr(cfg.bossKeyCornerBR));
        w.Put(L"BossKey", L"AutoHide", IniUtils::BoolToStr(cfg.bossKeyAutoHide));
        w.Put(L"BossKey", L"AutoHideTime", std::to_wstring(cfg.bossKeyAutoHideTime));
        w.Put(L"BossKey", L"CloseOnExit", IniUtils::BoolToStr(cfg.bossKeyCloseOnExit));

        // WinDGuard 设置
        w.Put(L"WinDGuard", L"Enabled", IniUtils::BoolToStr(cfg.winDGuardEnabled));
        {
            std::vector<std::wstring> serialized;
            for (const auto& ww : cfg.winDGuardWindows) serialized.push_back(ww.Serialize());
            out.windWindowsEntries = EntryListToString(serialized);
            w.Put(L"WinDGuard", L"Windows", out.windWindowsEntries);
        }
        return out;
    }
}

// ============================================================
// AppConfig::Save - 保存所有配置项到 INI 文件
// ============================================================
bool AppConfig::Save(const std::wstring& iniPath) const {
    // 启动时配置文件存在但无法读取：当前内存里是默认值，直接保存会把用户配置冲掉。
    // 返回失败由调用方提示，用户修复文件权限后重启即可正常保存
    if (loadFailed) return false;
    // 绑定列表与自定义热键同属 INI 单值：超过可靠读写上限时直接失败，
    // 避免写出一个随后无法完整读回的配置（调用方会提示保存失败）
    size_t bossChars = 0;
    for (const auto& w : bossKeyWindows) bossChars += w.Serialize().size() + 2;
    size_t windChars = 0;
    for (const auto& w : winDGuardWindows) windChars += w.Serialize().size() + 2;
    if (bossChars > MAX_BOUND_WINDOWS_CHARS || windChars > MAX_BOUND_WINDOWS_CHARS) {
        return false;
    }
    // 自定义热键与绑定同属 INI 单值：非 UI 路径也必须执行同一预算，
    // 否则超长内容会在回读校验阶段整体失败，或被截断后无法完整读回。
    size_t customChars = 0;
    for (const auto& hk : customHotkeys) customChars += hk.Serialize().size() + 2;
    if (customChars > MAX_CUSTOM_ENTRIES_CHARS) return false;
    // 原子写入：先写临时文件再替换目标文件，避免写盘途中崩溃/断电留下截断的配置。
    // 以全新的临时文件为写入对象，也自然丢弃旧版本遗留的废弃字段。
    // 临时文件名由系统生成（唯一、不可预测）：固定的 iniPath+".tmp" 会被
    // 同用户进程预置重解析点/占位文件，把提权实例的写入重定向（R8-M12）
    std::wstring tmpPath;
    {
        wchar_t tempName[MAX_PATH] = {};
        std::wstring iniDir;
        const size_t slash = iniPath.find_last_of(L"\\/");
        if (slash != std::wstring::npos) iniDir = iniPath.substr(0, slash);
        if (!iniDir.empty() && GetTempFileNameW(iniDir.c_str(), L"ksc", 0, tempName) != 0) {
            tmpPath = tempName;
        } else {
            tmpPath = iniPath + L".tmp";
            DeleteFileW(tmpPath.c_str());
        }
    }
    // 先写入 UTF-16LE BOM：profile API 只有检测到 Unicode BOM 才会以 Unicode
    // 写文件，否则新建文件按当前 ANSI 代码页落盘，emoji/其他语言字符会丢失。
    // BOM 写入失败必须中止：否则后续内容会按 ANSI 落盘而 Save 仍返回成功
    {
        HANDLE hTmp = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (hTmp == INVALID_HANDLE_VALUE) {
            // GetTempFileNameW 已创建占位文件：打开失败时删除，避免残留空文件（R13-A3）
            DeleteFileW(tmpPath.c_str());
            return false;
        }
        const WORD bom = 0xFEFF;
        DWORD written = 0;
        const BOOL bomOk = WriteFile(hTmp, &bom, sizeof(bom), &written, nullptr);
        CloseHandle(hTmp);
        if (!bomOk || written != sizeof(bom)) {
            DeleteFileW(tmpPath.c_str());
            return false;
        }
    }

    // profile API 的写失败不返回错误码给调用方时，必须自行记录并回读校验
    IniWriter writer{ tmpPath };
    const SerializedConfigFields fields = SaveConfigSections(writer, *this);
    // 写入完成标记：作为整个保存流程的最后一项写入
    writer.Put(L"General", L"SaveComplete", L"1");
    // 刷新临时文件的 INI 缓存到磁盘后再原子替换目标文件。
    // 注意：读取过该文件后，这个全 NULL 的刷新调用可能返回失败（缓存已被回读），
    // 不能作为失败判据；写入完整性由下面的回读校验确认
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, tmpPath.c_str());
    // 回读校验：profile API 的写失败（磁盘满/只读/单值过长）不返回错误，
    // 不校验会把残缺的临时文件替换到真实配置上
    const int saveComplete = GetPrivateProfileIntW(L"General", L"SaveComplete", 0, tmpPath.c_str());
    if (saveComplete != 1) {
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    // 任一次写入返回失败都视为整体失败：不能把缺字段的临时文件替换上去
    if (!writer.ok) {
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    // 关键长字段回读校验：profile API 对单值长度有限制，超限时可能静默截断，
    // 只检查 SaveComplete 会让“部分字段丢失”的配置仍被判定保存成功
    auto trimTrailing = [](std::wstring s) {
        while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
        return s;
    };
    auto fieldIntact = [&](const wchar_t* section, const wchar_t* key, const std::wstring& expected) {
        return trimTrailing(IniUtils::Read(tmpPath, section, key)) == trimTrailing(expected);
    };
    if (!fieldIntact(L"Keys", L"DisabledKeyCodes", fields.disabledKeysCsv) ||
        !fieldIntact(L"KeyRemap", L"MappingsV3", fields.remapV3Csv) ||
        !fieldIntact(L"CustomHotkeys", L"Entries", fields.customEntries) ||
        !fieldIntact(L"BossKey", L"Windows", fields.bossWindowsEntries) ||
        !fieldIntact(L"WinDGuard", L"Windows", fields.windWindowsEntries)) {
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    // BOM 校验：确认临时文件确实以 UTF-16LE 落盘（否则中文/emoji 已被写坏）
    {
        HANDLE hCheck = CreateFileW(tmpPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hCheck == INVALID_HANDLE_VALUE) {
            DeleteFileW(tmpPath.c_str());
            return false;
        }
        WORD checkBom = 0;
        DWORD readBytes = 0;
        const BOOL readOk = ReadFile(hCheck, &checkBom, sizeof(checkBom), &readBytes, nullptr);
        CloseHandle(hCheck);
        if (!readOk || readBytes != sizeof(checkBom) || checkBom != 0xFEFF) {
            DeleteFileW(tmpPath.c_str());
            return false;
        }
    }
    const BOOL moved = MoveFileExW(tmpPath.c_str(), iniPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!moved) {
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
