// ===== 自定义热键编辑对话框 =====
// 支持运行命令、模拟按键、打开文件/目录三种触发动作，主键可通过内联捕获或虚拟键盘选择。
// 从 SettingsDialog.cpp 拆出，行为保持不变。

#include "SettingsDialog.h"
#include "SettingsDialogSubDialogs.h"
#include "SettingsDialogInlineCapture.h"
#include "SettingsDialogPages.h"
#include "Utils.h"
#include "FluentTheme.h"
#include "Lang.h"
#include <ShlObj.h>
#include <Commdlg.h>

static const wchar_t* ADD_HOTKEY_CLASS = L"KeySentryAddHotkeyDlg";

// 触发动作：运行命令 / 模拟按键 / 打开文件或目录
enum class HotkeyAction { Command = 0, SimulateKey = 1, OpenPath = 2 };

// 自定义热键对话框数据
struct AddHotkeyData {
    AppConfig* workingCopy;     // 工作副本配置
    bool capturing;             // 是否正在捕获热键
    UINT captureMod;            // 捕获到的修饰键
    UINT captureVK;             // 捕获到的虚拟键码
    bool confirmed;             // 用户是否确认
    bool isEdit;                // 是否为编辑模式
    int editIndex;              // 编辑模式下对应的配置索引
    HotkeyAction action = HotkeyAction::Command;  // 当前触发动作
    UINT simVK = 0;             // 模拟按键模式下选中的被模拟键
    UINT simMods = 0;           // 模拟按键模式下目标组合的修饰键（0 表示单键）
};

// ===== 切换自定义热键对话框的触发动作模式 =====
// 运行命令模式：命令/程序行显示可用，其余命令相关选项启用
// 模拟按键模式：命令行位置切换为按键选择行，参数/工作目录/窗口状态等选项置灰
// 打开文件/目录：命令行位置显示目标路径，只保留路径输入与浏览
static void UpdateAddHotkeyMode(HWND hwnd, HotkeyAction action) {
    auto* ahd = reinterpret_cast<AddHotkeyData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (ahd) ahd->action = action;

    const bool simMode = (action == HotkeyAction::SimulateKey);
    const bool openMode = (action == HotkeyAction::OpenPath);

    // 动态标签："命令/程序:" / "模拟按键:" / "文件/目录:"
    HWND lbl = GetDlgItem(hwnd, IDC_LBL_HK_CMDLABEL);
    if (lbl) {
        SetWindowTextW(lbl, Tr(simMode ? L"模拟按键:"
                                      : (openMode ? L"文件/目录:" : L"命令/程序:")));
    }

    // 命令行控件与模拟行控件同位置互斥显示
    ShowWindow(GetDlgItem(hwnd, IDC_EDT_HK_CMD), simMode ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(hwnd, IDC_BTN_HK_BROWSE), simMode ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(hwnd, IDC_EDT_HK_SIMKEY), simMode ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, IDC_BTN_HK_PICKKEY), simMode ? SW_SHOW : SW_HIDE);

    // 仅命令模式有意义的选项：模拟按键与打开路径模式下置灰
    const int cmdOnlyCtrls[] = {
        IDC_EDT_HK_PARAMS, IDC_EDT_HK_WORKDIR, IDC_BTN_HK_WORKDIR_BROWSE,
        IDC_CBO_HK_WINSTATE, IDC_CHK_HK_CONFIRM, IDC_CHK_HK_ADMIN
    };
    for (int id : cmdOnlyCtrls) {
        HWND h = GetDlgItem(hwnd, id);
        if (h) EnableWindow(h, action == HotkeyAction::Command);
    }
    // 非命令模式保存时会丢弃确认/管理员/窗口状态；同步清空勾选与下拉，
    // 避免切回命令模式后仍显示为已勾选但实际不会生效
    if (action != HotkeyAction::Command) {
        if (HWND chk = GetDlgItem(hwnd, IDC_CHK_HK_CONFIRM))
            SendMessageW(chk, BM_SETCHECK, BST_UNCHECKED, 0);
        if (HWND chk = GetDlgItem(hwnd, IDC_CHK_HK_ADMIN))
            SendMessageW(chk, BM_SETCHECK, BST_UNCHECKED, 0);
        if (HWND cbo = GetDlgItem(hwnd, IDC_CBO_HK_WINSTATE))
            SendMessageW(cbo, CB_SETCURSEL, 0, 0);
    }
}

// ===== 结束自定义热键对话框的捕获态 =====
// 捕获期间打开"浏览程序/浏览目录/选择按键"等模态界面时必须先调用：
// 吞键钩子会吞掉全部物理按键，这些模态窗口将完全无法接收键盘输入，
// 且其模态循环不会处理 WM_INLINE_CAPTURE_KEY，捕获也无法自行结束
static void StopAddHotkeyCapture(HWND hwnd, AddHotkeyData* ahd) {
    if (!ahd->capturing) return;
    ahd->capturing = false;
    EndInlineCapture(hwnd);
    SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_HK_CAPTURE), Tr(L"捕获快捷键"));
}

// ===== 自定义热键对话框窗口过程 =====
static LRESULT CALLBACK AddHotkeyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) { EraseWindowBackground(hwnd, wp); return 1; }
    // DPI 处理不依赖 USERDATA，必须在空指针提前返回之前处理
    if (msg == WM_NCDESTROY) {
        CleanupSubDialogDpiState(hwnd);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (msg == WM_DPICHANGED) {
        HandleSubDialogDpiChanged(hwnd, wp, lp, 14, FW_NORMAL);
        return 0;
    }
    auto* ahd = reinterpret_cast<AddHotkeyData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!ahd) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            // 捕获期间窗口失焦：立即结束捕获并卸载吞键钩子。
            // 吞键钩子会吞掉全系统按键，不取消会让键盘在捕获窗口之外完全失灵（R8-H4）
            StopAddHotkeyCapture(hwnd, ahd);
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            // 捕获态下确定：先卸载吞键钩子，避免后续提示框无法用键盘关闭
            StopAddHotkeyCapture(hwnd, ahd);
            // 确定按钮：验证输入并保存自定义热键
            HWND edtKey = GetDlgItem(hwnd, IDC_EDT_HK_KEY);
            const std::wstring keyText = ReadWindowText(edtKey);
            if (keyText.empty()) {
                Fluent::ShowMessageBox(hwnd, L"请设置热键", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }

            // 主键取值：优先采用文本框当前内容（支持手动修改）。
            // 仅当文本框内容与"捕获到的键名"完全一致时，才允许回退到捕获值——
            // VKToName 借助 GetKeyNameTextW 能产出 "Numpad 0"、"Divide" 这类
            // NameToVK 无法反解的本地化名称，若此时静默回退，会注册出与界面显示不符的键。
            // 因此解析失败且文本框已被改动时直接拒绝，而不是猜一个值
            UINT parsedVK = (UINT)AppConfig::NameToVK(keyText.c_str());
            if (parsedVK != 0) {
                ahd->captureVK = parsedVK;
            } else if (ahd->captureVK == 0 ||
                       keyText != AppConfig::VKToName((int)ahd->captureVK)) {
                Fluent::ShowMessageBox(hwnd, L"无法识别该按键名称。\n请点击\"捕获快捷键\"或\"选择按键\"按钮重新设置主键。",
                             L"提示", MB_OK | MB_ICONWARNING);
                return 0;
            }
            // 主键不能是修饰键：RegisterHotKey 必然失败，且会让用户以为热键已生效
            if (ahd->captureVK != 0 && ConfigUtils::IsModifierKey((int)ahd->captureVK)) {
                Fluent::ShowMessageBox(hwnd, L"主键不能是修饰键（Ctrl/Shift/Alt/Win），请设置一个普通按键。",
                             L"提示", MB_OK | MB_ICONWARNING);
                return 0;
            }

            // 检查是否至少选择了一个修饰键
            UINT hotkeyMod = 0;
            if (SendMessageW(GetDlgItem(hwnd, IDC_CHK_HK_CTRL), BM_GETCHECK, 0, 0) == BST_CHECKED) hotkeyMod |= MOD_CONTROL;
            if (SendMessageW(GetDlgItem(hwnd, IDC_CHK_HK_SHIFT), BM_GETCHECK, 0, 0) == BST_CHECKED) hotkeyMod |= MOD_SHIFT;
            if (SendMessageW(GetDlgItem(hwnd, IDC_CHK_HK_ALT), BM_GETCHECK, 0, 0) == BST_CHECKED) hotkeyMod |= MOD_ALT;
            if (SendMessageW(GetDlgItem(hwnd, IDC_CHK_HK_WIN), BM_GETCHECK, 0, 0) == BST_CHECKED) hotkeyMod |= MOD_WIN;

            if (hotkeyMod == 0) {
                Fluent::ShowMessageBox(hwnd, L"自定义热键需要至少一个修饰键（Ctrl/Shift/Alt/Win），否则会与正常输入冲突。",
                             L"提示", MB_OK | MB_ICONWARNING);
                return 0;
            }

            HWND edtCmd = GetDlgItem(hwnd, IDC_EDT_HK_CMD);
            std::wstring cmdText = ReadWindowText(edtCmd);
            if (ahd->action == HotkeyAction::OpenPath) {
                // 路径首尾空白来自复制粘贴，去掉后才能被 ShellExecute 正确识别
                const size_t first = cmdText.find_first_not_of(L" \t\r\n");
                const size_t last = cmdText.find_last_not_of(L" \t\r\n");
                cmdText = (first == std::wstring::npos) ? L""
                                                        : cmdText.substr(first, last - first + 1);
                if (cmdText.empty()) {
                    Fluent::ShowMessageBox(hwnd, L"请选择或输入要打开的文件或目录", L"提示",
                                 MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
            } else if (ahd->action == HotkeyAction::Command && cmdText.empty()) {
                Fluent::ShowMessageBox(hwnd, L"请输入命令或程序路径", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }

            // 模拟按键模式：必须已选择被模拟的按键
            if (ahd->action == HotkeyAction::SimulateKey && ahd->simVK == 0) {
                Fluent::ShowMessageBox(hwnd, L"请点击\"选择按键\"按钮设置要模拟的按键", L"提示", MB_OK | MB_ICONWARNING);
                return 0;
            }

            // 构建自定义热键对象
            // 修饰键完全以复选框勾选状态为准（从 0 构建），
            // 用户取消勾选修饰键后保存才能生效（此前 |= 只加不减）
            CustomHotkey hk;
            hk.mod = hotkeyMod;
            hk.vk = ahd->captureVK;

            const bool simMode = (ahd->action == HotkeyAction::SimulateKey);
            const bool openMode = (ahd->action == HotkeyAction::OpenPath);
            const bool cmdMode = (ahd->action == HotkeyAction::Command);
            hk.simulateKey = simMode;
            hk.openPath = openMode;
            hk.targetVK = simMode ? ahd->simVK : 0;
            hk.targetMod = simMode ? ahd->simMods : 0;

            hk.command = simMode ? L"" : cmdText;

            // 用动态读取替代固定 512/256 栈缓冲：编辑框没有长度限制，
            // 超长粘贴会被静默截断，保存后无法恢复
            hk.parameters = cmdMode ? ReadWindowText(GetDlgItem(hwnd, IDC_EDT_HK_PARAMS)) : L"";

            std::wstring workDirText = ReadWindowText(GetDlgItem(hwnd, IDC_EDT_HK_WORKDIR));
            hk.workDir = cmdMode ? workDirText : L"";

            hk.description = ReadWindowText(GetDlgItem(hwnd, IDC_EDT_HK_DESC));

            // 录入阶段就按“当前条目 + 其余条目”预估序列化总长：
            // 否则用户要等到“应用”才被整体拒绝，且无法定位是哪一条过长
            {
                size_t total = hk.Serialize().size() + 2;
                for (size_t i = 0; i < ahd->workingCopy->customHotkeys.size(); ++i) {
                    if (ahd->isEdit && (int)i == ahd->editIndex) continue;
                    total += ahd->workingCopy->customHotkeys[i].Serialize().size() + 2;
                }
                if (total > MAX_CUSTOM_ENTRIES_CHARS) {
                    Fluent::ShowMessageBox(hwnd,
                        L"当前自定义热键的总内容过长，无法安全写入配置文件。\n"
                        L"请缩短命令/参数/描述，或减少热键数量。\n"
                        L"（序列化总长度不能超过 30000 字符）",
                        L"提示", MB_OK | MB_ICONWARNING);
                    return 0;
                }
            }

            HWND cboWinState = GetDlgItem(hwnd, IDC_CBO_HK_WINSTATE);
            int wsSel = (int)SendMessageW(cboWinState, CB_GETCURSEL, 0, 0);
            hk.windowState = cmdMode ? ConfigUtils::ClampWindowState(wsSel) : HotkeyWindowState::Normal;

            hk.confirmBeforeRun = cmdMode && SendMessageW(GetDlgItem(hwnd, IDC_CHK_HK_CONFIRM), BM_GETCHECK, 0, 0) == BST_CHECKED;
            hk.runAsAdmin = cmdMode && SendMessageW(GetDlgItem(hwnd, IDC_CHK_HK_ADMIN), BM_GETCHECK, 0, 0) == BST_CHECKED;

            // 检查热键是否已存在（编辑模式下跳过自身）
            for (size_t i = 0; i < ahd->workingCopy->customHotkeys.size(); i++) {
                if (ahd->workingCopy->customHotkeys[i].mod == hk.mod &&
                    ahd->workingCopy->customHotkeys[i].vk == hk.vk) {
                    if (ahd->isEdit && (int)i == ahd->editIndex) continue;
                    Fluent::ShowMessageBox(hwnd, L"该热键已存在，请使用其他组合", L"提示", MB_OK | MB_ICONWARNING);
                    return 0;
                }
            }

            // 编辑模式：替换原有热键；新增模式：追加到列表
            if (ahd->isEdit && ahd->editIndex >= 0 && ahd->editIndex < (int)ahd->workingCopy->customHotkeys.size()) {
                ahd->workingCopy->customHotkeys[ahd->editIndex] = hk;
            } else {
                // 上限校验：超过注册上限的热键会被静默忽略，提前提示避免用户困惑
                if ((int)ahd->workingCopy->customHotkeys.size() >= MAX_CUSTOM_HOTKEYS) {
                    Fluent::ShowMessageBox(hwnd, L"自定义热键数量已达上限（100 个），无法继续添加。",
                                 L"提示", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                ahd->workingCopy->customHotkeys.push_back(hk);
            }
            ahd->confirmed = true;
            DestroyWindow(hwnd);
            return 0;
        }
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        case IDC_BTN_HK_CAPTURE:
            // 开始捕获热键：安装吞键钩子，避免按键被全局钩子吞掉或触发系统快捷键
            // 钩子安装失败时不进入捕获态，否则界面提示"请按下快捷键"但永远收不到按键
            if (BeginInlineCapture(hwnd)) {
                ahd->capturing = true;
                SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_HK_CAPTURE), Tr(L"请按下快捷键..."));
            }
            SetFocus(hwnd);
            return 0;
        case IDC_CBO_HK_ACTION:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                const int sel = (int)SendMessageW(GetDlgItem(hwnd, IDC_CBO_HK_ACTION),
                                                  CB_GETCURSEL, 0, 0);
                const HotkeyAction action = (sel == 1) ? HotkeyAction::SimulateKey
                    : (sel == 2 ? HotkeyAction::OpenPath : HotkeyAction::Command);
                UpdateAddHotkeyMode(hwnd, action);
                // 首次切入模拟模式且尚未选键时，直接弹出键盘选择界面
                if (action == HotkeyAction::SimulateKey && ahd->simVK == 0) {
                    // 键盘选择界面需要接收键盘输入，先退出捕获态
                    StopAddHotkeyCapture(hwnd, ahd);
                    UINT pickedMods = 0;
                    int picked = SettingsDialog::ShowKeyboardPicker(hwnd, &pickedMods, true);
                    if (picked > 0) {
                        ahd->simVK = (UINT)picked;
                        ahd->simMods = pickedMods;
                        SetWindowTextW(GetDlgItem(hwnd, IDC_EDT_HK_SIMKEY),
                                       SettingsDialog::FormatHotKey(pickedMods, (UINT)picked).c_str());
                    }
                }
            }
            return 0;
        case IDC_BTN_HK_PICKKEY: {
            // 重新选择被模拟的按键或组合键（与按键映射一致的虚拟键盘界面）
            StopAddHotkeyCapture(hwnd, ahd);
            UINT pickedMods = 0;
            int picked = SettingsDialog::ShowKeyboardPicker(hwnd, &pickedMods, true);
            if (picked > 0) {
                ahd->simVK = (UINT)picked;
                ahd->simMods = pickedMods;
                SetWindowTextW(GetDlgItem(hwnd, IDC_EDT_HK_SIMKEY),
                               SettingsDialog::FormatHotKey(pickedMods, (UINT)picked).c_str());
            }
            return 0;
        }
        case IDC_BTN_HK_BROWSE: {
            // 浏览选择目标：对话框需要键盘输入，先退出捕获态
            StopAddHotkeyCapture(hwnd, ahd);

            // 打开文件/目录：目标可能不是可执行文件，用菜单让用户选择文件或文件夹
            if (ahd->action == HotkeyAction::OpenPath) {
                HMENU menu = CreatePopupMenu();
                if (!menu) return 0;
                AppendMenuW(menu, MF_STRING, 1, Tr(L"选择文件..."));
                AppendMenuW(menu, MF_STRING, 2, Tr(L"选择文件夹..."));
                // 以按钮下沿作为菜单起点：键盘激活按钮时鼠标可能不在按钮上
                RECT btnRect = {};
                GetWindowRect(GetDlgItem(hwnd, IDC_BTN_HK_BROWSE), &btnRect);
                POINT pt = { btnRect.left, btnRect.bottom };
                SetForegroundWindow(hwnd);
                const int picked = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                                       pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(menu);
                if (picked == 1) {
                    OPENFILENAMEW ofn = {};
                    std::wstring szFile(32768, L'\0');
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFile = &szFile[0];
                    ofn.nMaxFile = (DWORD)szFile.size();
                    ofn.lpstrFilter = (GetAppLanguage() == AppLanguage::English)
                        ? L"All files\0*.*\0"
                        : L"所有文件\0*.*\0";
                    ofn.nFilterIndex = 1;
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                    if (GetOpenFileNameW(&ofn)) {
                        SetWindowTextW(GetDlgItem(hwnd, IDC_EDT_HK_CMD), szFile.c_str());
                    }
                } else if (picked == 2) {
                    std::vector<wchar_t> dir(32768, L'\0');
                    BROWSEINFOW bi = {};
                    bi.hwndOwner = hwnd;
                    bi.lpszTitle = Tr(L"选择要打开的文件夹");
                    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
                    if (pidl) {
                        if (SHGetPathFromIDListW(pidl, dir.data())) {
                            SetWindowTextW(GetDlgItem(hwnd, IDC_EDT_HK_CMD), dir.data());
                        }
                        CoTaskMemFree(pidl);
                    }
                }
                return 0;
            }

            OPENFILENAMEW ofn = {};
            // 使用长路径上限缓冲（32768），避免路径超过 MAX_PATH 时被截断
            std::wstring szFile(32768, L'\0');
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFile = &szFile[0];
            ofn.nMaxFile = (DWORD)szFile.size();
            // 文件对话框过滤器含内嵌 \0，不能走普通词典，按语言直接选择
            ofn.lpstrFilter = (GetAppLanguage() == AppLanguage::English)
                ? L"Executable files\0*.exe;*.bat;*.cmd;*.ps1\0All files\0*.*\0"
                : L"可执行文件\0*.exe;*.bat;*.cmd;*.ps1\0所有文件\0*.*\0";
            ofn.nFilterIndex = 1;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                SetWindowTextW(GetDlgItem(hwnd, IDC_EDT_HK_CMD), szFile.c_str());
            }
            return 0;
        }
        case IDC_BTN_HK_WORKDIR_BROWSE: {
            // 浏览选择工作目录：目录选择框需要键盘输入，先退出捕获态
            StopAddHotkeyCapture(hwnd, ahd);
            // 与上面的文件浏览一致使用长路径缓冲：MAX_PATH 下超过 260 字符的
            // 目录会让 SHGetPathFromIDListW 失败，工作目录被静默丢弃
            std::vector<wchar_t> dir(32768, L'\0');
            BROWSEINFOW bi = {};
            bi.hwndOwner = hwnd;
            bi.lpszTitle = Tr(L"选择工作目录");  // 直接传给系统 API，必须在这里翻译
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                // 转换失败时不能清空已有工作目录，仅在成功取得路径时写回
                if (SHGetPathFromIDListW(pidl, dir.data())) {
                    SetWindowTextW(GetDlgItem(hwnd, IDC_EDT_HK_WORKDIR), dir.data());
                }
                CoTaskMemFree(pidl);
            }
            return 0;
        }
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case DM_GETDEFID:
        // 非对话框窗口：声明默认按钮，Enter 可触发「确定」
        return MAKELRESULT(IDOK, DC_HASDEFID);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, IsWindowEnabled((HWND)lp) ? Fluent::TextColor() : Fluent::TextDisabledColor());
        return (LRESULT)Fluent::SurfaceBrush();
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, Fluent::SurfaceColor());
        SetTextColor(hdc, IsWindowEnabled((HWND)lp) ? Fluent::TextColor() : Fluent::TextDisabledColor());
        return (LRESULT)Fluent::SurfaceBrush();
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ===== 显示自定义热键编辑对话框 =====
// editIndex=-1 为新增模式，否则为编辑模式
void SettingsDialog::ShowHotkeyDialog(HWND hwnd, AppConfig& config, int editIndex) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = AddHotkeyWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = ADD_HOTKEY_CLASS;
        // 背景由 WM_ERASEBKGND 用主题画刷绘制（不随系统浅色/深色固定为白色）
        wc.hbrBackground = nullptr;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // 注册失败时不得标记成功：否则窗口类始终未注册，
        // CreateWindowExW 会持续返回空且不再重试（ERROR_CLASS_ALREADY_EXISTS 视为成功）
        if (!WindowUtils::RegisterClassOnce(wc)) return;
        registered = true;
    }

    bool isEdit = editIndex >= 0;
    AddHotkeyData ahd;
    ahd.workingCopy = &config;
    ahd.capturing = false;
    ahd.captureMod = 0;
    ahd.captureVK = 0;
    ahd.confirmed = false;
    ahd.isEdit = isEdit;
    ahd.editIndex = editIndex;

    if (isEdit) {
        const auto& existing = config.customHotkeys[editIndex];
        ahd.captureMod = existing.mod;
        ahd.captureVK = existing.vk;
        ahd.action = existing.openPath
            ? HotkeyAction::OpenPath
            : (existing.simulateKey ? HotkeyAction::SimulateKey : HotkeyAction::Command);
        ahd.simVK = existing.targetVK;
        ahd.simMods = existing.targetMod;
    }

    const int kLogicalHeight = 440;
    RECT rc = { 0, 0, ScaleUi(480), ScaleUi(kLogicalHeight) };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int winX = 0, winY = 0;
    WindowUtils::ClampWindowToWorkArea(&winX, &winY, &winW, &winH);

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, ADD_HOTKEY_CLASS,
                                isEdit ? L"编辑自定义热键" : L"新增自定义热键",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                                winX, winY,
                                winW, winH,
                                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    // 窗口创建失败时直接返回，避免以空句柄创建子控件产生孤儿窗口
    if (!dlg) return;
    SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&ahd));

    HFONT font = GetDlgFont();
    int y = 14;
    int cx = 14;
    int cw = 440;
    int lineH = 22;
    int btnH = 32;
    // 标签列留足英文宽度（"Working directory:" 约 123px、"Command / program:" 约 143px），
    // 中文标签多余空间不影响布局（R8-M5）
    int labelW = 150;
    int editX = cx + labelW + 5;

    auto dlgLabel = [&](const wchar_t* text, int px, int py, int w, int h = 0) {
        HWND h2 = CreateWindowExW(0, L"STATIC", text,
                                   WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                   px, py, w, h > 0 ? h : lineH, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(h2, font);
    };

    auto dlgEdit = [&](int id, int px, int py, int w) {
        HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                                   px, py, w, lineH,
                                   dlg, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(h, font);
    };

    auto dlgChk = [&](int id, const wchar_t* text, int px, int py, int w) {
        HWND h = CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                   px, py, w, lineH,
                                   dlg, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(h, font);
    };

    auto dlgButton = [&](int id, const wchar_t* text, int px, int py, int w, int h2) {
        HWND h = CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   px, py, w, h2,
                                   dlg, (HMENU)(LONG_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(h, font);
    };

    dlgLabel(L"修饰键:", cx, y, labelW, btnH);
    dlgChk(IDC_CHK_HK_CTRL, L"Ctrl", editX, y, 55);
    dlgChk(IDC_CHK_HK_SHIFT, L"Shift", editX + 60, y, 60);
    dlgChk(IDC_CHK_HK_ALT, L"Alt", editX + 125, y, 50);
    dlgChk(IDC_CHK_HK_WIN, L"Win", editX + 180, y, 50);

    if (isEdit) {
        const auto& existing = config.customHotkeys[editIndex];
        SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_CTRL), BM_SETCHECK, (existing.mod & MOD_CONTROL) ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_SHIFT), BM_SETCHECK, (existing.mod & MOD_SHIFT) ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_ALT), BM_SETCHECK, (existing.mod & MOD_ALT) ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_WIN), BM_SETCHECK, (existing.mod & MOD_WIN) ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    y += btnH + 6;

    dlgLabel(L"主键:", cx, y, labelW, btnH);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                     editX, y, 100, btnH,
                     dlg, (HMENU)(LONG_PTR)IDC_EDT_HK_KEY, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(GetDlgItem(dlg, IDC_EDT_HK_KEY), font);
    SendMessageW(GetDlgItem(dlg, IDC_EDT_HK_KEY), EM_SETLIMITTEXT, 64, 0);
    dlgButton(IDC_BTN_HK_CAPTURE, L"捕获快捷键", editX + 108, y, 115, btnH);

    if (isEdit) {
        SetWindowTextW(GetDlgItem(dlg, IDC_EDT_HK_KEY), AppConfig::VKToName(config.customHotkeys[editIndex].vk).c_str());
    }
    y += btnH + 6;

    // 触发动作：运行命令（默认）/ 模拟按键 / 打开文件或目录
    // 三项互斥，用下拉框选择，避免单选按钮在英文界面下挤在同一行
    dlgLabel(L"触发动作:", cx, y, labelW, btnH);
    HWND cboAction = CreateWindowExW(0, L"COMBOBOX", L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                      CBS_DROPDOWNLIST | WS_VSCROLL,
                                      editX, y, 200, 200,
                                      dlg, (HMENU)(LONG_PTR)IDC_CBO_HK_ACTION,
                                      GetModuleHandleW(nullptr), nullptr);
    SetControlFont(cboAction, font);
    SendMessageW(cboAction, CB_ADDSTRING, 0, (LPARAM)L"运行命令");
    SendMessageW(cboAction, CB_ADDSTRING, 0, (LPARAM)L"模拟按键");
    SendMessageW(cboAction, CB_ADDSTRING, 0, (LPARAM)L"打开文件/目录");
    SendMessageW(cboAction, CB_SETCURSEL, (WPARAM)ahd.action, 0);
    y += btnH + 6;

    // 命令/程序行 与 模拟按键选择行同位置，按触发动作模式互斥显示
    {
        HWND lblCmd = CreateWindowExW(0, L"STATIC", L"命令/程序:",
                                       WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                       cx, y, labelW, btnH,
                                       dlg, (HMENU)(LONG_PTR)IDC_LBL_HK_CMDLABEL, GetModuleHandleW(nullptr), nullptr);
        SetControlFont(lblCmd, font);
    }
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                     editX, y, cw - labelW - 5 - 80, btnH,
                     dlg, (HMENU)(LONG_PTR)IDC_EDT_HK_CMD, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(GetDlgItem(dlg, IDC_EDT_HK_CMD), font);
    // 限制输入长度：超长命令会让 INI 单值超过 profile API 上限，保存时整条丢失。
    // 保留较宽的字段上限以兼容已有长命令；是否超出 30000 总预算
    // 在 IDOK 时按“当前条目 + 其余条目”即时校验并提示。
    SendMessageW(GetDlgItem(dlg, IDC_EDT_HK_CMD), EM_SETLIMITTEXT, 32768, 0);
    dlgButton(IDC_BTN_HK_BROWSE, L"浏览", cx + cw - 75, y, 75, btnH);

    // 模拟按键行：只读编辑框显示选中按键 + 选择按键按钮（初始不显示，由模式切换控制）
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                     WS_CHILD | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_READONLY,
                     editX, y, 150, btnH,
                     dlg, (HMENU)(LONG_PTR)IDC_EDT_HK_SIMKEY, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(GetDlgItem(dlg, IDC_EDT_HK_SIMKEY), font);
    dlgButton(IDC_BTN_HK_PICKKEY, L"选择按键", editX + 158, y, 115, btnH);

    if (isEdit) {
        const auto& existing = config.customHotkeys[editIndex];
        SetWindowTextW(GetDlgItem(dlg, IDC_EDT_HK_CMD), existing.command.c_str());
        if (existing.simulateKey && existing.targetVK != 0) {
            SetWindowTextW(GetDlgItem(dlg, IDC_EDT_HK_SIMKEY),
                           FormatHotKey(existing.targetMod, existing.targetVK).c_str());
        }
    }
    // 按当前模式（新增默认命令模式，编辑恢复原模式）应用控件显示/启用状态
    UpdateAddHotkeyMode(dlg, ahd.action);
    y += btnH + 6;

    dlgLabel(L"运行参数:", cx, y, labelW, btnH);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                     editX, y, cw - labelW - 5, btnH,
                     dlg, (HMENU)(LONG_PTR)IDC_EDT_HK_PARAMS, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(GetDlgItem(dlg, IDC_EDT_HK_PARAMS), font);
    SendMessageW(GetDlgItem(dlg, IDC_EDT_HK_PARAMS), EM_SETLIMITTEXT, 8192, 0);

    if (isEdit) {
        SetWindowTextW(GetDlgItem(dlg, IDC_EDT_HK_PARAMS), config.customHotkeys[editIndex].parameters.c_str());
    }
    y += btnH + 6;

    dlgLabel(L"工作目录:", cx, y, labelW, btnH);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                     editX, y, cw - labelW - 5 - 80, btnH,
                     dlg, (HMENU)(LONG_PTR)IDC_EDT_HK_WORKDIR, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(GetDlgItem(dlg, IDC_EDT_HK_WORKDIR), font);
    SendMessageW(GetDlgItem(dlg, IDC_EDT_HK_WORKDIR), EM_SETLIMITTEXT, 1024, 0);
    dlgButton(IDC_BTN_HK_WORKDIR_BROWSE, L"浏览", cx + cw - 75, y, 75, btnH);

    if (isEdit) {
        SetWindowTextW(GetDlgItem(dlg, IDC_EDT_HK_WORKDIR), config.customHotkeys[editIndex].workDir.c_str());
    }
    y += btnH + 6;

    dlgLabel(L"描述:", cx, y, labelW, btnH);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                     editX, y, cw - labelW - 5, btnH,
                     dlg, (HMENU)(LONG_PTR)IDC_EDT_HK_DESC, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(GetDlgItem(dlg, IDC_EDT_HK_DESC), font);
    SendMessageW(GetDlgItem(dlg, IDC_EDT_HK_DESC), EM_SETLIMITTEXT, 512, 0);

    if (isEdit) {
        SetWindowTextW(GetDlgItem(dlg, IDC_EDT_HK_DESC), config.customHotkeys[editIndex].description.c_str());
    }
    y += btnH + 6;

    dlgLabel(L"窗口状态:", cx, y, labelW, btnH);
    HWND cboWinState = CreateWindowExW(0, L"COMBOBOX", L"",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                        editX, y, 100, 200,
                                        dlg, (HMENU)IDC_CBO_HK_WINSTATE, GetModuleHandleW(nullptr), nullptr);
    SetControlFont(cboWinState, font);
    SendMessageW(cboWinState, CB_ADDSTRING, 0, (LPARAM)L"正常");
    SendMessageW(cboWinState, CB_ADDSTRING, 0, (LPARAM)L"最小化");
    SendMessageW(cboWinState, CB_ADDSTRING, 0, (LPARAM)L"最大化");
    SendMessageW(cboWinState, CB_ADDSTRING, 0, (LPARAM)L"隐藏");
    SendMessageW(cboWinState, CB_SETCURSEL, isEdit ? (int)config.customHotkeys[editIndex].windowState : 0, 0);

    // 确认/管理员选项单独占一行：英文「Confirm before running」约 155px，
    // 与窗口状态下拉框挤在同一行会被截断或超出对话框边界（R8-M5）
    y += btnH + 6;
    dlgChk(IDC_CHK_HK_CONFIRM, L"确认后执行", cx, y, 165);
    dlgChk(IDC_CHK_HK_ADMIN, L"以管理员运行", cx + 170, y, 220);

    if (isEdit) {
        SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_CONFIRM), BM_SETCHECK, config.customHotkeys[editIndex].confirmBeforeRun ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_ADMIN), BM_SETCHECK, config.customHotkeys[editIndex].runAsAdmin ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    // 按钮固定在窗口底部（距底边 14px），与上方窗口高度常量保持一致
    const int bottomBtnY = kLogicalHeight - 30 - 14;
    dlgButton(IDOK, L"确定", cx + cw - 180, bottomBtnY, 80, 30);
    dlgButton(IDCANCEL, L"取消", cx + cw - 90, bottomBtnY, 80, 30);

    // 控件按逻辑坐标创建，统一放大到当前 DPI 的物理尺寸
    ScaleChildWindows(dlg, GetUiScale());
    Fluent::ApplyWindowChrome(dlg);
    Fluent::DecorateDialog(dlg);
    ApplyLanguageToWindowTree(dlg);

    {
        ModalGuard guard(hwnd);
        ShowWindow(dlg, SW_SHOW);
        UpdateWindow(dlg);

        MSG msg;
        while (IsWindow(dlg)) {
            BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
            if (ret <= 0) { if (ret == 0) PostQuitMessage((int)msg.wParam); break; }
            // 捕获的按键由吞键钩子以 WM_INLINE_CAPTURE_KEY 投递（lParam=修饰键组合）
            if (msg.message == WM_INLINE_CAPTURE_KEY && ahd.capturing) {
                UINT mod = (UINT)msg.lParam;
                int vk = (int)msg.wParam;
                // 纯修饰键：继续等待主键
                if (InlineCapIsModifier(vk)) continue;
                if (vk == VK_ESCAPE && mod == 0) {
                    // 仅无修饰键的 Esc 取消捕获；Ctrl+Esc/Alt+Esc/Win+Esc 应作为
                    // 组合录入（与老板键/一键关闭键的捕获语义一致，R8-M1）
                    ahd.capturing = false;
                    EndInlineCapture(dlg);
                    SetWindowTextW(GetDlgItem(dlg, IDC_BTN_HK_CAPTURE), Tr(L"捕获快捷键"));
                    continue;
                }
                if (mod == 0) {
                    // 拒绝无修饰键的裸键：保存时本也会被拒，这里即时提示避免误以为已捕获
                    SetWindowTextW(GetDlgItem(dlg, IDC_BTN_HK_CAPTURE), Tr(L"请使用 修饰键+按键..."));
                    continue;
                }
                ahd.captureMod = mod;
                ahd.captureVK = (UINT)vk;
                ahd.capturing = false;
                // 卸载吞键钩子：恢复键盘钩子对按键的拦截
                EndInlineCapture(dlg);
                SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_CTRL), BM_SETCHECK, (mod & MOD_CONTROL) ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_SHIFT), BM_SETCHECK, (mod & MOD_SHIFT) ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_ALT), BM_SETCHECK, (mod & MOD_ALT) ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(GetDlgItem(dlg, IDC_CHK_HK_WIN), BM_SETCHECK, (mod & MOD_WIN) ? BST_CHECKED : BST_UNCHECKED, 0);
                SetWindowTextW(GetDlgItem(dlg, IDC_EDT_HK_KEY), AppConfig::VKToName(vk).c_str());
                SetWindowTextW(GetDlgItem(dlg, IDC_BTN_HK_CAPTURE), Tr(L"捕获快捷键"));
                continue;
            }
            // 捕获热键期间丢弃 WM_HOTKEY：按下的组合若正好是已注册热键，
            // 转发会触发热键动作（如老板键隐藏窗口）打断捕获
            if (msg.message == WM_HOTKEY) {
                if (!ahd.capturing && g_mainWnd && IsWindow(g_mainWnd))
                    SendMessageW(g_mainWnd, msg.message, msg.wParam, msg.lParam);
                continue;
            }
            // 交给对话框管理器：Tab 切换焦点、Esc 自动转成 IDCANCEL，便于纯键盘操作
            if (IsDialogMessageW(dlg, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // 兜底：捕获过程中对话框被关闭时卸载吞键钩子，避免按键被长期吞掉；
        // EndInlineCapture 内部按归属判断，不会误拆其他对话框正在使用的钩子
        if (ahd.capturing || InlineCaptureOwner() == dlg) {
            ahd.capturing = false;
            EndInlineCapture(dlg);
        }

        // 兜底：异常退出路径下销毁窗口，避免 GWLP_USERDATA 指向已失效的栈对象 ahd
        if (IsWindow(dlg)) DestroyWindow(dlg);
    }
}
