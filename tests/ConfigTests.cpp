// ===== 配置层最小回归测试 =====
// 覆盖：UTF-16 往返、特殊字符转义、绑定校验/去重/上限、超大绑定保存拦截。
// 不依赖 UI 与钩子，可直接在 CI/本地通过 ctest 运行。

#include "../src/Config.h"
#include "../src/Utils.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

static std::wstring TempPath(const wchar_t* name) {
    wchar_t dir[MAX_PATH] = {};
    const DWORD len = GetTempPathW(MAX_PATH, dir);
    // 文件名带进程 ID：x86/x64 或并行 CI 同时运行时不会互相覆盖临时配置
    const std::wstring unique =
        std::wstring(L"KeySentryTest_") + std::to_wstring(GetCurrentProcessId()) + L"_" + name;
    if (len == 0 || len >= MAX_PATH) return std::wstring(L".\\") + unique;
    return std::wstring(dir) + unique;
}

static void RemoveTestFile(const std::wstring& path) {
    DeleteFileW(path.c_str());
    DeleteFileW((path + L".tmp").c_str());
}

static std::wstring MakeLongPath(wchar_t fill, int length) {
    std::wstring s = L"C:\\";
    s.append((size_t)length, fill);
    return s;
}

static void TestRoundTrip() {
    const std::wstring path = TempPath(L"keysentry_config_roundtrip.ini");
    RemoveTestFile(path);

    AppConfig in;
    in.autoStart = true;
    in.showLockKeyOverlay = false;
    in.overlayPosition = 2;
    in.overlayTheme = 1;
    in.overlayOpacity = 3;
    in.overlayDelay = 0;
    in.forceNumLockOn = true;
    in.disableStartupNotification = true;
    in.appTheme = 2;
    in.explorerHideHidden = 1;
    in.explorerHideExt = 2;
    in.explorerHideOS = 1;
    in.disableSpecifiedKeysEnabled = true;
    in.disabledKeyCodes = { 0x14, 0x2D, 0x5B };
    in.keyRemapEnabled = true;
    in.keyRemappings = { { 0x41, MOD_CONTROL, 0x42, 0 }, { 0x43, 0, 0x44, 0 } };
    in.customHotkeysEnabled = true;
    {
        CustomHotkey hk;
        hk.mod = MOD_CONTROL | MOD_ALT;
        hk.vk = 'K';
        hk.command = L"C:\\Program Files\\A;B=C|D\\app.exe";
        hk.parameters = L"--flag=\"x|y;z\" --path=C:\\tmp\\a=b";
        hk.workDir = L"C:\\dir with space ";
        hk.description = L"中文 😀 描述";
        hk.windowState = HotkeyWindowState::Maximized;
        hk.confirmBeforeRun = true;
        hk.runAsAdmin = true;
        in.customHotkeys.push_back(hk);
    }
    in.bossKeyEnabled = true;
    in.bossKeyMod = MOD_CONTROL | MOD_ALT;
    in.bossKeyVK = 'J';
    in.bossKeyCloseMod = MOD_CONTROL | MOD_SHIFT;
    in.bossKeyCloseVK = 'Q';
    in.bossKeyMute = true;
    in.bossKeyAutoHide = true;
    in.bossKeyAutoHideTime = 7;
    in.bossKeyCloseOnExit = true;
    {
        BoundWindowInfo w;
        w.matchMode = MatchMode::Process;
        w.processName = L"notepad.exe";
        w.processPath = L"C:\\Windows\\notepad.exe";
        w.pid = 1234;
        in.bossKeyWindows.push_back(w);
    }
    {
        BoundWindowInfo w;
        w.matchMode = MatchMode::Window;
        w.title = L"文档 | A;B=C";
        in.winDGuardWindows.push_back(w);
    }
    in.tabState = 4;

    CHECK(in.Save(path));

    // UTF-16LE BOM：保证中文/emoji 不会按 ANSI 落盘
    FILE* fp = nullptr;
    CHECK(_wfopen_s(&fp, path.c_str(), L"rb") == 0 && fp != nullptr);
    unsigned char bom[2] = {};
    if (fp) {
        CHECK(fread(bom, 1, 2, fp) == 2);
        fclose(fp);
    }
    CHECK(bom[0] == 0xFF && bom[1] == 0xFE);

    AppConfig out;
    out.Load(path);
    CHECK(!out.LoadFailed());
    CHECK(out.autoStart == true);
    CHECK(out.showLockKeyOverlay == false);
    CHECK(out.overlayPosition == 2);
    CHECK(out.overlayTheme == 1);
    CHECK(out.overlayOpacity == 3);
    CHECK(out.overlayDelay == 0);
    CHECK(out.forceNumLockOn == true);
    CHECK(out.disableStartupNotification == true);
    CHECK(out.appTheme == 2);
    CHECK(out.explorerHideHidden == 1);
    CHECK(out.explorerHideExt == 2);
    CHECK(out.explorerHideOS == 1);
    CHECK(out.disabledKeyCodes.size() == 3);
    CHECK(out.keyRemappings.size() == 2);
    CHECK(out.customHotkeys.size() == 1);
    if (out.customHotkeys.size() == 1) {
        const CustomHotkey& hk = out.customHotkeys[0];
        CHECK(hk.command == L"C:\\Program Files\\A;B=C|D\\app.exe");
        CHECK(hk.parameters == L"--flag=\"x|y;z\" --path=C:\\tmp\\a=b");
        CHECK(hk.workDir == L"C:\\dir with space ");
        CHECK(hk.description == L"中文 😀 描述");
        CHECK(hk.windowState == HotkeyWindowState::Maximized);
        CHECK(hk.confirmBeforeRun);
        CHECK(hk.runAsAdmin);
    }
    CHECK(out.bossKeyEnabled == true);
    CHECK(out.bossKeyMod == (MOD_CONTROL | MOD_ALT));
    CHECK(out.bossKeyVK == 'J');
    CHECK(out.bossKeyCloseMod == (MOD_CONTROL | MOD_SHIFT));
    CHECK(out.bossKeyCloseVK == 'Q');
    CHECK(out.bossKeyMute == true);
    CHECK(out.bossKeyAutoHide == true);
    CHECK(out.bossKeyAutoHideTime == 7);
    CHECK(out.bossKeyCloseOnExit == true);
    CHECK(out.bossKeyWindows.size() == 1);
    if (out.bossKeyWindows.size() == 1) {
        CHECK(out.bossKeyWindows[0].matchMode == MatchMode::Process);
        CHECK(out.bossKeyWindows[0].processName == L"notepad.exe");
        CHECK(out.bossKeyWindows[0].processPath == L"C:\\Windows\\notepad.exe");
    }
    CHECK(out.winDGuardWindows.size() == 1);
    if (out.winDGuardWindows.size() == 1) {
        CHECK(out.winDGuardWindows[0].matchMode == MatchMode::Window);
        CHECK(out.winDGuardWindows[0].title == L"文档 | A;B=C");
    }
    CHECK(out.tabState == 4);

    RemoveTestFile(path);
}

static void TestBindingLimitAndDedupe() {
    const std::wstring path = TempPath(L"keysentry_config_limit.ini");
    RemoveTestFile(path);

    AppConfig in;
    // 70 条不同绑定：加载时应收敛到 MAX_BOUND_WINDOWS
    for (int i = 0; i < 70; ++i) {
        BoundWindowInfo w;
        w.matchMode = MatchMode::Process;
        w.processName = L"proc" + std::to_wstring(i) + L".exe";
        in.bossKeyWindows.push_back(w);
    }
    // 重复项：同进程名/路径只保留一条
    in.bossKeyWindows.push_back(in.bossKeyWindows[0]);
    CHECK(in.Save(path));

    AppConfig out;
    out.Load(path);
    CHECK(out.bossKeyWindows.size() == (size_t)MAX_BOUND_WINDOWS);

    RemoveTestFile(path);
}

static void TestFieldClampAndOversizeSave() {
    const std::wstring path = TempPath(L"keysentry_config_oversize.ini");
    RemoveTestFile(path);

    AppConfig in;
    BoundWindowInfo w;
    w.matchMode = MatchMode::Process;
    w.processName.assign((size_t)MAX_BINDING_NAME + 20, L'N');
    w.processPath = MakeLongPath(L'P', MAX_BINDING_PATH + 50);
    in.bossKeyWindows.push_back(w);
    CHECK(in.Save(path));

    AppConfig out;
    out.Load(path);
    CHECK(out.bossKeyWindows.size() == 1);
    if (out.bossKeyWindows.size() == 1) {
        CHECK(out.bossKeyWindows[0].processName.size() <= (size_t)MAX_BINDING_NAME);
        CHECK(out.bossKeyWindows[0].processPath.size() <= (size_t)MAX_BINDING_PATH);
    }
    RemoveTestFile(path);

    // 64 条长路径绑定：序列化总长超过 30000，Save 必须拒绝且不产生文件
    AppConfig oversized;
    for (int i = 0; i < MAX_BOUND_WINDOWS; ++i) {
        BoundWindowInfo b;
        b.matchMode = MatchMode::Process;
        b.processName = L"proc" + std::to_wstring(i) + L".exe";
        b.processPath = MakeLongPath(L'Q', MAX_BINDING_PATH);
        oversized.bossKeyWindows.push_back(b);
    }
    const std::wstring oversizePath = TempPath(L"keysentry_config_guard.ini");
    RemoveTestFile(oversizePath);
    CHECK(!oversized.Save(oversizePath));
    CHECK(GetFileAttributesW(oversizePath.c_str()) == INVALID_FILE_ATTRIBUTES);
    RemoveTestFile(oversizePath);
}

static void TestMalformedBindingRejected() {
    const std::wstring path = TempPath(L"keysentry_config_malformed.ini");
    RemoveTestFile(path);

    // 空标题的 Window 模式绑定、空进程信息的 Process 模式绑定都应在加载时丢弃
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
        const WORD bom = 0xFEFF;
        DWORD written = 0;
        WriteFile(h, &bom, sizeof(bom), &written, nullptr);
        CloseHandle(h);
    }
    IniUtils::Write(path, L"BossKey", L"Windows", L"0|||0|;;1|||0|");
    IniUtils::Write(path, L"WinDGuard", L"Windows", L"0|||0|");

    AppConfig out;
    out.Load(path);
    CHECK(out.bossKeyWindows.empty());
    CHECK(out.winDGuardWindows.empty());

    RemoveTestFile(path);
}

static int CountPipes(const std::wstring& s) {
    int count = 0;
    for (wchar_t c : s) if (c == L'|') ++count;
    return count;
}

static void TestHotkeyFormatCompatibility() {
    // 模拟组合键：必须写 12 字段并完整往返
    CustomHotkey sim;
    sim.mod = MOD_CONTROL;
    sim.vk = 'K';
    sim.simulateKey = true;
    sim.targetVK = 'A';
    sim.targetMod = MOD_SHIFT;
    const std::wstring simText = sim.Serialize();
    CHECK(CountPipes(simText) == 11);
    const CustomHotkey simOut = CustomHotkey::Deserialize(simText);
    CHECK(simOut.simulateKey);
    CHECK(simOut.targetVK == 'A');
    CHECK(simOut.targetMod == MOD_SHIFT);

    // 命令型热键即使残留 targetMod/targetVK，也只写 11 字段
    CustomHotkey cmd;
    cmd.mod = MOD_CONTROL;
    cmd.vk = 'J';
    cmd.command = L"notepad.exe";
    cmd.targetVK = 'B';
    cmd.targetMod = MOD_SHIFT;
    const std::wstring cmdText = cmd.Serialize();
    CHECK(CountPipes(cmdText) == 10);
    const CustomHotkey cmdOut = CustomHotkey::Deserialize(cmdText);
    CHECK(!cmdOut.simulateKey);
    CHECK(cmdOut.targetVK == 0);
    CHECK(cmdOut.targetMod == 0);

    // “确认后执行”取消勾选后必须原样往返，不能被解析回 true
    CustomHotkey noConfirm;
    noConfirm.mod = MOD_CONTROL;
    noConfirm.vk = 'H';
    noConfirm.command = L"notepad.exe";
    noConfirm.confirmBeforeRun = false;
    const CustomHotkey noConfirmOut = CustomHotkey::Deserialize(noConfirm.Serialize());
    CHECK(!noConfirmOut.confirmBeforeRun);
    CHECK(!noConfirmOut.runAsAdmin);

    // 打开文件/目录：仍写 11 字段，第 10 字段为 2，不能与模拟按键混淆
    CustomHotkey open;
    open.mod = MOD_ALT;
    open.vk = 'O';
    open.command = L"D:\\Docs";
    open.openPath = true;
    const std::wstring openText = open.Serialize();
    CHECK(CountPipes(openText) == 10);
    const CustomHotkey openOut = CustomHotkey::Deserialize(openText);
    CHECK(openOut.openPath);
    CHECK(!openOut.simulateKey);
    CHECK(openOut.command == L"D:\\Docs");
    CHECK(openOut.targetVK == 0);
    CHECK(openOut.targetMod == 0);
}

static void TestEscapingAndPidRoundTrip() {
    CustomHotkey hk;
    hk.mod = MOD_CONTROL;
    hk.vk = 'K';
    hk.command = L"  \"C:\\a;b=c|d\"  ";
    hk.parameters = L"line1\r\nline2\t\"q\"";
    hk.description = L"=;|\\";
    const CustomHotkey out = CustomHotkey::Deserialize(hk.Serialize());
    CHECK(out.command == hk.command);
    CHECK(out.parameters == hk.parameters);
    CHECK(out.description == hk.description);

    BoundWindowInfo w;
    w.matchMode = MatchMode::Window;
    w.title = L" 标题|a;b=c\\d ";
    w.processName = L" p|q ";
    w.processPath = L"C:\\x;y=z";
    w.pid = 0xFFFFFFF0u;  // 大于 INT_MAX，验证 DWORD 解析
    const BoundWindowInfo wOut = BoundWindowInfo::Deserialize(w.Serialize());
    CHECK(wOut.title == w.title);
    CHECK(wOut.processName == w.processName);
    CHECK(wOut.processPath == w.processPath);
    CHECK(wOut.pid == w.pid);
}

static void TestCsvWhitespaceTolerance() {
    const std::wstring path = TempPath(L"keysentry_config_csvspace.ini");
    RemoveTestFile(path);

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
        const WORD bom = 0xFEFF;
        DWORD written = 0;
        WriteFile(h, &bom, sizeof(bom), &written, nullptr);
        CloseHandle(h);
    }
    // 尾随空格曾被完整解析要求拒绝，导致第二个键被静默丢弃
    IniUtils::Write(path, L"Keys", L"DisabledKeyCodes", L"65, 66 ");

    AppConfig out;
    out.Load(path);
    CHECK(out.disabledKeyCodes.size() == 2);
    RemoveTestFile(path);
}

static void TestZeroBytePrimaryMigration() {
    const std::wstring primary = TempPath(L"keysentry_migrate_primary.ini");
    const std::wstring legacy = TempPath(L"keysentry_migrate_legacy.ini");
    RemoveTestFile(primary);
    RemoveTestFile(legacy);

    AppConfig seed;
    seed.autoStart = true;
    CHECK(seed.Save(legacy));

    // 0 字节主文件：迁移必须删除它并成功复制旧配置，而不是永远回退
    HANDLE h = CreateFileW(primary.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    const std::wstring chosen = ProcessUtils::MigrateOrPreferFile(primary, legacy);
    CHECK(chosen == primary);
    WIN32_FILE_ATTRIBUTE_DATA attrs = {};
    CHECK(GetFileAttributesExW(primary.c_str(), GetFileExInfoStandard, &attrs));
    CHECK(attrs.nFileSizeLow > 0);

    RemoveTestFile(primary);
    RemoveTestFile(legacy);
}

static void TestModifierMainKeyRejected() {
    const std::wstring path = TempPath(L"keysentry_config_modifierkey.ini");
    RemoveTestFile(path);

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
        const WORD bom = 0xFEFF;
        DWORD written = 0;
        WriteFile(h, &bom, sizeof(bom), &written, nullptr);
        CloseHandle(h);
    }
    IniUtils::Write(path, L"CustomHotkeys", L"Enabled", L"1");
    // mod=Ctrl，主键=VK_CONTROL（修饰键）：加载时必须丢弃
    IniUtils::Write(path, L"CustomHotkeys", L"Entries",
                    L"2|17|notepad.exe||||0|0|0|0|0");

    AppConfig out;
    out.Load(path);
    CHECK(out.customHotkeys.empty());
    RemoveTestFile(path);
}

static void TestLoadFailureIsNonDestructive() {
    const std::wstring path = TempPath(L"keysentry_config_invalid.ini");
    RemoveTestFile(path);

    // 非 UTF-16 BOM 的文件必须拒绝加载，且 Save 不得覆盖。
    // 内容特意包含可被 profile API 按 ANSI 解析的 ASCII 字段：
    // 若失败后仍继续读取，这些值会生效并与“以默认设置运行”的提示矛盾（R13-M1）
    const char payload[] =
        "[General]\r\n"
        "ConfigVersion=5\r\n"
        "AutoStart=1\r\n"
        "[Keys]\r\n"
        "DisableSpecifiedKeysEnabled=1\r\n"
        "[BossKey]\r\n"
        "VKCode=65\r\n"
        "CloseVKCode=66\r\n";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, payload, (DWORD)(sizeof(payload) - 1), &written, nullptr);
        CloseHandle(h);
    }

    AppConfig cfg;
    cfg.Load(path);
    CHECK(cfg.LoadFailed());
    CHECK(cfg.LoadFailedEncoding());
    // 编码失败必须整体回退默认值，而不是“尽力读取”后部分生效
    CHECK(!cfg.autoStart);
    CHECK(!cfg.disableSpecifiedKeysEnabled);
    CHECK(cfg.bossKeyVK == 0xC0);
    CHECK(cfg.bossKeyCloseVK == VK_ESCAPE);
    CHECK(cfg.disabledKeyCodes.empty());
    CHECK(!cfg.Save(path));

    WIN32_FILE_ATTRIBUTE_DATA attrs = {};
    CHECK(GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attrs));
    CHECK(attrs.nFileSizeLow == sizeof(payload) - 1);
    RemoveTestFile(path);
}

static void TestProductExecutableName() {
    // 单实例判定与自启项归属共用同一份产品 exe 名单（R13-A13）
    CHECK(ProcessUtils::IsProductExecutableName(L"KeySentry.exe"));
    CHECK(ProcessUtils::IsProductExecutableName(L"keysentry-x64.EXE"));
    CHECK(ProcessUtils::IsProductExecutableName(L"KeySentry-x86.exe"));
    CHECK(ProcessUtils::IsProductExecutableName(L"C:\\Program Files\\KeySentry\\KeySentry-x64.exe"));
    // 仅包含相同子串或扩展名不同的第三方程序不能被误判
    CHECK(!ProcessUtils::IsProductExecutableName(L"NotKeySentry.exe"));
    CHECK(!ProcessUtils::IsProductExecutableName(L"KeySentry.exe.bak"));
    // 目录归属不在这里判断：函数只负责文件名名单，目录一致性由单实例逻辑单独校验
    CHECK(ProcessUtils::IsProductExecutableName(L"C:\\Other\\KeySentry-x64.exe"));
}

static void TestDpiScaling() {
    RECT work = { 0, 0, 3840, 2160 };
    CHECK(ComputeUiScale(96, &work) >= 1.0f);
    CHECK(ComputeUiScale(144, &work) > 1.4f && ComputeUiScale(144, &work) < 1.6f);
    CHECK(ComputeUiScale(192, &work) > 1.9f && ComputeUiScale(192, &work) < 2.1f);
    // 小屏夹紧：125% 不应让 800x680 的设置窗口超出 1024x768 工作区
    RECT smallWork = { 0, 0, 1024, 768 };
    CHECK(ComputeUiScale(144, &smallWork) <= 1.25f + 0.01f);
    CHECK(DpiUtils::GetSystemDpi() >= 96u);
    CHECK(DpiUtils::GetDpiForWindowSafe(nullptr) >= 96u);
}

static void TestLanguageAndCloseHotkeyDefaults() {
    // 默认值：一键关闭程序热键为 Win+Esc（R8-A3）
    AppConfig defaults;
    CHECK(defaults.bossKeyCloseMod == MOD_WIN);
    CHECK(defaults.bossKeyCloseVK == VK_ESCAPE);
    CHECK(defaults.appLanguage == 0);

    const std::wstring path = TempPath(L"keysentry_lang.ini");
    RemoveTestFile(path);

    AppConfig in;
    in.appLanguage = 1;
    in.appTheme = 2;
    in.bossKeyCloseMod = MOD_WIN;
    in.bossKeyCloseVK = 'Q';
    CHECK(in.Save(path));

    // 保存时必须写入当前配置版本号，便于后续迁移判断
    CHECK(GetPrivateProfileIntW(L"General", L"ConfigVersion", 0, path.c_str()) == CONFIG_VERSION);

    AppConfig out;
    out.Load(path);
    CHECK(!out.LoadFailed());
    CHECK(out.appLanguage == 1);
    CHECK(out.appTheme == 2);
    CHECK(out.bossKeyCloseMod == MOD_WIN);
    CHECK(out.bossKeyCloseVK == 'Q');
    RemoveTestFile(path);
}

int main() {
    std::printf("KeySentry config tests\n");
    TestLanguageAndCloseHotkeyDefaults();
    TestRoundTrip();
    TestBindingLimitAndDedupe();
    TestFieldClampAndOversizeSave();
    TestMalformedBindingRejected();
    TestHotkeyFormatCompatibility();
    TestEscapingAndPidRoundTrip();
    TestCsvWhitespaceTolerance();
    TestZeroBytePrimaryMigration();
    TestModifierMainKeyRejected();
    TestLoadFailureIsNonDestructive();
    TestProductExecutableName();
    TestDpiScaling();
    if (g_failures == 0) {
        std::printf("PASS\n");
        return 0;
    }
    std::printf("FAILED: %d\n", g_failures);
    return 1;
}
