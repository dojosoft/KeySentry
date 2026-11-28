// ===== Lang.cpp - 界面语言与中英文词典 =====
// 界面文本统一以中文原文创建，英文模式下通过词典翻译；
// 切回中文时由调用方按中文原文重建界面，因此这里只维护中文 → 英文。

#include "Lang.h"
#include <CommCtrl.h>
#include <algorithm>
#include <map>
#include <vector>

namespace {

struct Entry {
    const wchar_t* zh;
    const wchar_t* en;
};

// 词典条目：覆盖设置页、子对话框、托盘菜单、关于窗口、通知与消息框。
// 不含调试输出与带内嵌 \0 的文件对话框过滤器。
const Entry kEntries[] = {
    { L" 个", L" item(s)" },
    { L"%s: 锁", L"%s: Lock" },
    { L"（Alt+字母 可能与菜单快捷键冲突）", L"(Alt+letters may conflict with menu shortcuts)" },
    { L"（Ctrl+数字 可能与输入法冲突）", L"(Ctrl+digits may conflict with IME shortcuts)" },
    { L"(Fn键无法捕获)", L"(Fn key cannot be captured)" },
    { L"(Fn键无法捕获；修饰键也可互换)", L"(Fn key cannot be captured; modifier keys can also be swapped)" },
    { L"（建议序列化总长度不超过 30000 字符）", L"(Recommended total serialized length: no more than 30000 characters)" },
    { L"（每组绑定建议不超过 30000 字符）", L"(Recommended length per binding group: no more than 30000 characters)" },
    { L"以下热键注册失败，可能被其他程序占用：\n",
      L"The following hotkeys failed to register and may be in use by another program:\n" },
    { L"检测到以下热键被其他程序占用：\n\n",
      L"The following hotkeys are in use by another program:\n\n" },
    { L"热键被占用", L"Hotkey in use" },
    { L"窗口隐藏热键: ", L"Window hide hotkey: " },
    { L"关闭程序热键: ", L"Close-program hotkey: " },
    { L"自定义热键: ", L"Custom hotkey: " },
    { L"(未命名热键)", L"(Unnamed hotkey)" },
    { L"（无效）", L"(Invalid)" },
    { L"（序列化总长度不能超过 30000 字符）", L"(Serialized total length must not exceed 30000 characters)" },
    { L"\n\n本次互换包含修饰键，所有依赖该修饰键的组合键", L"\n\nThis swap includes a modifier key. All shortcuts that depend on this modifier" },
    { L"\n\n本次设置仅在当前运行期间生效，重启后可能丢失。\n", L"\n\nThis setting only applies to the current session and may be lost after restart.\n" },
    { L"\n\n部分设置可能无法在下次启动时恢复。\n", L"\n\nSome settings may not be restored on the next launch.\n" },
    { L"\n\n是否继续？", L"\n\nContinue?" },
    { L"\n\n是否用当前设置覆盖外部修改？\n", L"\n\nOverwrite the external changes with the current settings?\n" },
    { L"\n是否仍要应用设置？", L"\nApply the settings anyway?" },
    { L"注意：将以管理员权限运行",
      L"Note: It will run with administrator privileges." },
    { L"注意：当前已以管理员身份运行，子进程将以管理员权限运行。",
      L"Note: KeySentry is already elevated; the child process will run as administrator." },
    { L"」将被映射为组合键「", L"\" will be mapped to the shortcut \"" },
    { L"」将不再输出原字符，", L"\" will no longer output the original character, " },
    { L"」将输出「", L"\" will output \"" },
    { L"」与「", L"\" and \"" },
    { L"© 2026 Marvin 翁敏峰", L"© 2026 Marvin" },
    { L"……等共 ", L"... total " },
    { L"• 按下「", L"• Press \"" },
    { L"1. 该功能主要用于对指定窗口或进程保持置顶激活状态，并禁止被最小化，实现对返回桌面等操作快捷键（如 Win+D）的免疫。\n",
      L"1. Keeps the specified window or process pinned and prevents it from being minimized, so shortcuts such as Win+D cannot send it to the desktop.\n" },
    { L"按键映射为组合键后，单独按下原始按键将失效；发生按键冲突时请删除对应映射项。",
      L"Once a key is mapped to a shortcut, pressing the original key alone no longer works. If a key conflict occurs, delete the conflicting mapping." },
    { L"对指定窗口保持置顶，禁止窗口最小化。出现窗口异常时，点击「修复窗口层级」取消所有窗口置顶。",
      L"Keeps the selected window on top and prevents minimization. If a window behaves abnormally, click \"Fix Window Z-Order\" to clear topmost from all windows." },
    { L"1.通过热键、鼠标动作、闲置延时等指令快速隐藏窗口，实现老板键的功能，保护隐私；\n",
      L"1. Quickly hides windows via hotkeys, mouse actions, or idle timeout to protect privacy.\n" },
    { L"2. 若出现窗口异常置顶的情况，可点击\u201C修复窗口堆叠\u201D按钮，强制取消所有窗口的置顶状态，恢复正常窗口层级。",
      L"2. If windows become stuck on top, click \"Fix Window Z-Order\" to clear all topmost states and restore the normal window order." },
    { L"2.支持窗口标题匹配 / 进程名匹配两种绑定方式，绑定进程将自动隐藏同一进程的任何窗口。",
      L"2. Supports window-title and process-name matching. Binding a process hides any window of that process." },
    { L"Alt (右)", L"Alt (Right)" },
    { L"Alt (左)", L"Alt (Left)" },
    { L"Ctrl (右)", L"Ctrl (Right)" },
    { L"Ctrl (左)", L"Ctrl (Left)" },
    { L"Shift (右)", L"Shift (Right)" },
    { L"Shift (左)", L"Shift (Left)" },
    { L"Win (右)", L"Win (Right)" },
    { L"Win (左)", L"Win (Left)" },
    { L"安全提示", L"Security Notice" },
    { L"按窗口名绑定", L"Bind by window title" },
    { L"按窗口名绑定：仅匹配包含该标题的窗口", L"Bind by title: windows containing this title" },
    { L"按键代码", L"Key code" },
    { L"按键互换", L"Swap keys" },
    { L"按键屏蔽", L"Key Blocking" },
    { L"按键名称", L"Key name" },
    { L"按键映射", L"Key Remapping" },
    { L"按键映射 - 源按键", L"Key Remapping - Source key" },
    { L"按键映射数量已达上限（256 条），请先删除不需要的规则。",
      L"The key remapping limit (256 entries) has been reached. Delete unused rules first." },
    { L"按键映射数量已达上限（256 条），无法再添加互换组。",
      L"The key remapping limit (256 entries) has been reached. Cannot add another swap group." },
    { L"按进程名绑定", L"Bind by process name" },
    { L"按进程名绑定：匹配该进程的全部窗口（含后续新建窗口）",
      L"Bind by process: all windows of it, including new ones" },
    { L"帮助", L"Help" },
    { L"绑定数量已达上限（64 个），请先删除不需要的绑定",
      L"The binding limit (64) has been reached. Delete unused bindings first." },
    { L"保持 NumLock 开启状态", L"Keep NumLock on" },
    { L"保持显示", L"Keep visible" },
    { L"保持原状", L"Leave unchanged" },
    { L"保存", L"Save" },
    { L"崩溃恢复记录写入失败：若程序异常退出，被隐藏的窗口可能无法自动恢复。",
      L"Failed to write the crash-recovery record. If the program exits unexpectedly, hidden windows may not be restored automatically." },
    { L"被隐藏窗口过多，恢复记录已截断；部分窗口在程序异常退出后可能无法自动恢复。",
      L"Too many windows are hidden. The recovery record was truncated; some windows may not be restored automatically after an abnormal exit." },
    { L"边框", L"Border" },
    { L"编辑菜单", L"Edit menu" },
    { L"编辑选中项", L"Edit selected item" },
    { L"编辑自定义热键", L"Edit custom hotkey" },
    { L"标题/进程名", L"Title / process name" },
    { L"捕获快捷键", L"Capture shortcut" },
    { L"不透明", L"Opaque" },
    { L"部分绑定进程因目标无响应未被收集，可能仍在运行。",
      L"Some bound processes were not collected because the target did not respond and may still be running." },
    { L"部分窗口因目标无响应未被处理，请再次触发老板键或稍后重试。",
      L"Some windows were not processed because the target did not respond. Trigger the boss key again or try later." },
    { L"部分窗口因目标无响应未能读取标题，窗口守护可能未覆盖全部窗口。",
      L"Some window titles could not be read because the target did not respond. Window guard may not cover every window." },
    { L"部分窗口因目标无响应未能读取标题，列表中可能缺少这些窗口。",
      L"Some window titles could not be read because the target did not respond. They may be missing from the list." },
    { L"部分目标窗口以管理员权限运行，置顶/禁止最小化保护无法生效。",
      L"Some target windows run with administrator privileges, so pinning and minimize protection cannot take effect." },
    { L"部分文件夹选项设置写入失败（可能被组策略或权限限制），这些设置未生效。",
      L"Some folder option settings could not be written (possibly restricted by Group Policy or permissions) and did not take effect." },
    { L"部分已生效的热键在冲突检测后未能重新注册，\n请重启程序或重新应用设置。",
      L"Some active hotkeys could not be re-registered after the conflict check.\nRestart the program or apply the settings again." },
    { L"参数：", L"Arguments:" },
    { L"插入链接", L"Insert link" },
    { L"查看菜单", L"View menu" },
    { L"查找", L"Find" },
    { L"查找下一个", L"Find next" },
    { L"常规", L"General" },
    { L"撤销", L"Undo" },
    { L"程序：", L"Program:" },
    { L"触发动作:", L"Action:" },
    { L"窗口", L"Window" },
    { L"窗口绑定内容总长过长，无法安全写入配置文件。\n"
      L"请减少绑定数量或缩短窗口标题/进程路径后重试。\n"
      L"（每组绑定建议不超过 30000 字符）",
      L"The total window binding content is too long to write safely to the configuration file.\n"
      L"Reduce the number of bindings or shorten window titles/process paths and try again.\n"
      L"(Recommended length per binding group: no more than 30000 characters)" },
    { L"窗口绑定内容总长过长，无法安全写入配置文件。\n",
      L"The total window binding content is too long to write safely to the configuration file.\n" },
    { L"窗口标题至少需要 2 个字符，避免匹配过多窗口",
      L"The window title must contain at least 2 characters to avoid matching too many windows." },
    { L"窗口截图", L"Window screenshot" },
    { L"窗口类注册失败", L"Failed to register window class" },
    { L"窗口守护记录无法写入（目录只读或磁盘已满），",
      L"Could not write the window guard record (directory is read-only or disk is full), " },
    { L"窗口隐藏", L"Window Hiding" },
    { L"窗口隐藏被其他程序占用: ", L"Window hiding hotkey is occupied by another program: " },
    { L"窗口隐藏可能与常用快捷键冲突: ", L"Window hiding may conflict with common shortcuts: " },
    { L"窗口隐藏热键注册失败，可能被其他程序占用。\n仍可通过鼠标按键或屏幕角落触发。",
      L"Failed to register the window hiding hotkey; it may be occupied by another program.\nYou can still trigger it with mouse buttons or screen corners." },
    { L"窗口隐藏与一键关闭热键冲突: ", L"Window hiding conflicts with the close-program hotkey: " },
    { L"窗口置顶", L"Window Pinning" },
    { L"窗口置顶守护正在启用，无法执行修复。\n请先在“窗口置顶”页关闭守护并应用，再运行本修复。",
      L"Window pinning guard is enabled, so the repair cannot run.\nDisable the guard on the \"Window Pinning\" page and apply, then run the repair again." },
    { L"窗口状态:", L"Window state:" },
    { L"此功能用于修复 Win+D 桌面保护引发的窗口 Z 轴错乱及异常置顶问题。\n请确认是否运行修复？",
      L"This repairs window Z-order corruption and abnormal topmost states caused by Win+D desktop protection.\nRun the repair now?" },
    { L"错误", L"Error" },
    { L"打开", L"Open" },
    { L"打开文件/目录", L"Open file/folder" },
    { L"打开: ", L"Open: " },
    { L"打开失败: ", L"Failed to open: " },
    { L"打印", L"Print" },
    { L"单键「", L"Single key \"" },
    { L"单键失效提醒", L"Single-key behavior notice" },
    { L"当前自定义热键的总内容过长，无法安全写入配置文件。\n",
      L"The total custom hotkey content is too long to write safely to the configuration file.\n" },
    { L"地址栏", L"Address bar" },
    { L"低透明", L"Low transparency" },
    { L"底部", L"Bottom" },
    { L"顶部", L"Top" },
    { L"都会按互换后的按键处理。", L" will be handled as the swapped key." },
    { L"而是等同于按下「", L" but is treated as pressing \"" },
    { L"反向切换标签", L"Switch tabs backward" },
    { L"方向", L"Direction" },
    { L"分钟", L"minutes" },
    { L"否则所有依赖该键的组合键都会失效。", L" Otherwise every shortcut that depends on this key will stop working." },
    { L"复制", L"Copy" },
    { L"该按键已在屏蔽列表中", L"This key is already in the blocked list." },
    { L"该窗口没有标题，无法按窗口名绑定", L"This window has no title and cannot be bound by window name." },
    { L"该窗口已在绑定列表中", L"This window is already in the binding list." },
    { L"该窗口已在列表中", L"This window is already in the list." },
    { L"该互换在列表中显示为一行，删除时两个方向一并移除。\n",
      L"This swap appears as one row; deleting it removes both directions.\n" },
    { L"该进程已在绑定列表中", L"This process is already in the binding list." },
    { L"该目标按键已被其他按键映射为源按键，这会导致循环映射问题。\n请先删除相关映射后再试。",
      L"The target key is already used as a source key by another mapping, which would create a circular mapping.\nDelete the related mapping first." },
    { L"该热键无法编辑", L"This hotkey cannot be edited." },
    { L"该热键无法删除", L"This hotkey cannot be deleted." },
    { L"该热键已存在，请使用其他组合", L"This hotkey already exists. Use a different combination." },
    { L"该源按键已存在映射规则", L"A mapping already exists for this source key." },
    { L"高透明", L"High transparency" },
    { L"跟随系统", L"Follow system" },
    { L"工作目录:", L"Working directory:" },
    { L"工作目录：", L"Working directory: " },
    { L"功能说明：\n", L"Description:\n" },
    { L"功能说明：\n按键屏蔽功能允许用户阻止特定按键的输入，避免误操作。",
      L"Description:\nKey blocking prevents specific keys from being input, avoiding accidental operation." },
    { L"功能说明：\n按键映射支持单键或组合键（如 C → Ctrl+C）。映射为组合键后，单独按源键不再输出原字符。",
      L"Description:\nKey remapping supports single keys or shortcuts (e.g. C → Ctrl+C). Once mapped to a shortcut, pressing the source key alone no longer outputs its original character." },
    { L"关", L"Off" },
    { L"关闭", L"Close" },
    { L"关闭标签", L"Close tab" },
    { L"关闭窗口", L"Close window" },
    { L"关闭热键注册失败，可能被其他程序占用",
      L"Failed to register the close-program hotkey; it may be occupied by another program." },
    { L"关于", L"About" },
    { L"关于 键客", L"About KeySentry" },
    { L"后退", L"Back" },
    { L"互换 ⇄", L"Swap ⇄" },
    { L"缓慢", L"Slow" },
    { L"换行/发送", L"New line / Send" },
    { L"恢复标签", L"Restore tab" },
    { L"恢复隐藏窗口", L"Restore hidden windows" },
    { L"加粗", L"Bold" },
    { L"检测到上次异常退出，已恢复 %d 个被隐藏的窗口。",
      L"Detected an abnormal exit last time. Restored %d hidden windows." },
    { L"检测到以管理员身份运行：已为全部自定义热键强制启用执行前确认，防止用户可写配置被直接篡改后静默提权执行。",
      L"Running as administrator: \"Confirm before running\" has been enabled for all custom hotkeys to prevent silently elevated execution from a user-writable configuration." },
    { L"剪切", L"Cut" },
    { L"键客", L"KeySentry" },
    { L"键客 KeySentry", L"KeySentry" },
    { L"键客 KeySentry —— 重新掌控你的键盘", L"KeySentry — Take back control of your keyboard" },
    { L"键客 KeySentry 已启动，如需设置请点击托盘图标。", L"KeySentry is running. Click the tray icon to open settings." },
    { L"键客 设置", L"KeySentry Settings" },
    { L"键盘钩子安装失败，部分功能可能不可用。\n请检查是否有安全软件拦截。",
      L"Failed to install the keyboard hook; some features may be unavailable.\nCheck whether security software is blocking it." },
    { L"键盘钩子安装失败，无法捕获按键。\n请稍后重试或检查安全软件拦截。",
      L"Failed to install the keyboard hook; keys cannot be captured.\nTry again later or check security software." },
    { L"键盘热键", L"Keyboard hotkey" },
    { L"将互换「", L"Swap \"" },
    { L"仅在主屏显示", L"Show on primary monitor only" },
    { L"进程", L"Process" },
    { L"屏蔽按键数量已达上限（256 个），请先删除不需要的条目。",
      L"The blocked-key limit (256) has been reached. Delete unused entries first." },
    { L"静音", L"Mute" },
    { L"居中", L"Center" },
    { L"开", L"On" },
    { L"开机自动启动", L"Start with Windows" },
    { L"开机自启设置失败：无法获取程序路径。", L"Failed to configure auto-start: cannot get the program path." },
    { L"开机自启设置失败：注册表写入被拒绝。", L"Failed to configure auto-start: registry write was denied." },
    { L"开始菜单", L"Start menu" },
    { L"快速", L"Fast" },
    { L"历史", L"History" },
    { L"两个按键不能相同，无法互换。", L"The two keys must be different to swap." },
    { L"浏览", L"Browse" },
    { L"每次软件启动时，会按此设置全局重置文件夹选项，实现开机初始化设置。",
      L"At each program launch, folder options are reset globally according to this setting." },
    { L"描述", L"Description" },
    { L"描述:", L"Description:" },
    { L"描述：", L"Description: " },
    { L"命令", L"Command" },
    { L"命令/程序:", L"Command / program:" },
    { L"模拟按键", L"Simulate key" },
    { L"模拟按键:", L"Simulate key:" },
    { L"模拟按键: ", L"Simulate key: " },
    { L"模拟按键发送失败：目标窗口可能以管理员权限运行，或输入被安全软件拦截。",
      L"Failed to send the simulated key: the target window may be running as administrator, or input is blocked by security software." },
    { L"默认", L"Default" },
    { L"配置保存失败", L"Failed to save configuration" },
    { L"配置冲突", L"Configuration conflict" },
    { L"配置文件读取失败，本次以默认设置运行；为避免覆盖原文件，本次不会自动保存。",
      L"Failed to read the configuration file. Default settings will be used, and the file will not be saved automatically to avoid overwriting it." },
    { L"配置文件已被其他程序修改：\n", L"The configuration file was modified by another program:\n" },
    { L"配置无法写入配置文件：\n", L"The configuration could not be written to the configuration file:\n" },
    { L"匹配模式", L"Match mode" },
    { L"其他窗口正在捕获热键，请先完成或取消该捕获。",
      L"Another window is capturing a hotkey. Finish or cancel that capture first." },
    { L"其他选项", L"Other options" },
    { L"其中一个按键已存在于现有映射规则中。\n", L"One of the keys already exists in an existing mapping rule.\n" },
    { L"启动后开启 NumLock", L"Turn on NumLock after launch" },
    { L"启动时不显示通知", L"Do not show a notification at startup" },
    { L"启动与退出", L"Startup and exit" },
    { L"启用按键屏蔽", L"Enable key blocking" },
    { L"启用按键映射", L"Enable key remapping" },
    { L"启用窗口隐藏", L"Enable window hiding" },
    { L"启用窗口置顶", L"Enable window pinning" },
    { L"启用自定义热键", L"Enable custom hotkeys" },
    { L"前进", L"Forward" },
    { L"浅色", L"Light" },
    { L"切换标签", L"Switch tabs" },
    { L"切换窗口", L"Switch window" },
    { L"清除", L"Clear" },
    { L"请按下快捷键...", L"Press a shortcut..." },
    { L"请按下需要屏蔽的按键...", L"Press the key to block..." },
    { L"请按下要互换的第二个按键...", L"Press the second key to swap..." },
    { L"请按下要互换的第一个按键...", L"Press the first key to swap..." },
    { L"请按下要映射的源按键...", L"Press the source key to remap..." },
    { L"请点击\"选择按键\"按钮设置要模拟的按键", L"Click \"Select key\" to set the key to simulate." },
    { L"请检查该目录是否只读、磁盘是否已满。", L"Check whether the directory is read-only or the disk is full." },
    { L"请减少绑定数量或缩短窗口标题/进程路径后重试。\n",
      L"Reduce the number of bindings or shorten the window title/process path and try again.\n" },
    { L"请设置热键", L"Set a hotkey" },
    { L"请使用 修饰键+按键 的组合...", L"Use a modifier + key combination..." },
    { L"请使用 修饰键+按键...", L"Use a modifier + key..." },
    { L"请输入命令或程序路径", L"Enter a command or program path." },
    { L"请输入至少 2 个字符的窗口标题", L"Enter a window title with at least 2 characters." },
    { L"请缩短命令/参数，或减少自定义热键数量后重试。\n",
      L"Shorten the command/arguments or reduce the number of custom hotkeys and try again.\n" },
    { L"请缩短命令/参数/描述，或减少热键数量。\n",
      L"Shorten the command/arguments/description or reduce the number of hotkeys.\n" },
    { L"请先处理当前热键确认框，再打开设置。", L"Handle the current hotkey confirmation first, then open settings." },
    { L"请先从窗口列表中选择一个窗口", L"Select a window from the list first." },
    { L"请先关闭设置窗口，再退出程序。", L"Close the settings window before exiting." },
    { L"请先删除相关规则，或选择其他按键。", L"Delete the related rule first or choose another key." },
    { L"请先选择要编辑的热键", L"Select a hotkey to edit first." },
    { L"请先选择要删除的热键", L"Select a hotkey to delete first." },
    { L"取消", L"Cancel" },
    { L"取消静音", L"Unmute" },
    { L"全屏", L"Full screen" },
    { L"全选", L"Select all" },
    { L"确定", L"OK" },
    { L"确定要执行以下操作？\n\n", L"Run the following action?\n\n" },
    { L"确定要执行以下操作？", L"Run the following action?" },
    { L"请核对命令、参数与工作目录，确认无误后再执行。",
      L"Review the command, arguments, and working directory before running." },
    { L"确认", L"Confirm" },
    { L"确认后执行", L"Confirm before running" },
    { L"执行", L"Run" },
    { L"热键", L"Hotkey" },
    { L"热键冲突", L"Hotkey conflict" },
    { L"热键管理", L"Hotkeys" },
    { L"任务管理器", L"Task Manager" },
    { L"如需恢复原按键输入，请在本页删除该映射。\r\n\r\n",
      L"To restore the original key input, delete this mapping on this page.\r\n\r\n" },
    { L"软件启动时设置", L"Startup settings" },
    { L"若该键已被它们注册为热键，对应功能仍可能被触发。",
      L"If they already registered this key as a hotkey, their feature may still be triggered." },
    { L"删除选中项", L"Delete selected" },
    { L"设置", L"Settings" },
    { L"设置无法写入配置文件：\n", L"Settings could not be written to the configuration file:\n" },
    { L"深色", L"Dark" },
    { L"是否继续添加？", L"Continue adding?" },
    { L"适中", L"Medium" },
    { L"收藏夹", L"Favorites" },
    { L"手动添加窗口标题 (标题包含该文字的所有窗口都会匹配):",
      L"Manual title (all windows containing it will match):" },
    { L"输入法切换", L"Switch input method" },
    { L"属性", L"Properties" },
    { L"鼠标侧键1", L"Mouse side button 1" },
    { L"鼠标侧键2", L"Mouse side button 2" },
    { L"鼠标钩子安装失败，鼠标侧键触发老板键可能不可用。\n请检查是否有安全软件拦截。",
      L"Failed to install the mouse hook; triggering the boss key with mouse side buttons may not work.\nCheck whether security software is blocking it." },
    { L"鼠标隐藏", L"Mouse hiding" },
    { L"鼠标中键", L"Middle mouse button" },
    { L"刷新", L"Refresh" },
    { L"说明：软件启动时及点击「应用」后，会按此全局重置文件夹选项；若被组策略或权限限制，写入失败会提示。",
      L"Note: Folder options are reset at startup and after Apply; write failures are reported." },
    { L"搜索", L"Search" },
    { L"缩进", L"Indent" },
    { L"请确认", L"Please confirm" },
    { L"是否强制结束这些程序？", L"Force-end these programs?" },
    { L"以下程序未在超时内退出，强制结束可能导致未保存的数据丢失：\n\n",
      L"The following programs did not exit in time. Force-ending them may cause unsaved data to be lost:\n\n" },
    { L"提示", L"Notice" },
    { L"提示窗口创建失败", L"Failed to create the notification window" },
    { L"添加", L"Add" },
    { L"添加按键互换", L"Add key swap" },
    { L"添加按键互换 - 第二个按键", L"Add key swap - second key" },
    { L"添加按键互换 - 第一个按键", L"Add key swap - first key" },
    { L"添加按键映射", L"Add key mapping" },
    { L"添加绑定", L"Add binding" },
    { L"添加绑定窗口/进程", L"Add window/process binding" },
    { L"添加绑定方式:", L"Add binding method:" },
    { L"添加成功", L"Added" },
    { L"添加屏蔽按键", L"Add blocked key" },
    { L"添加书签", L"Add bookmark" },
    { L"添加自定义热键", L"Add custom hotkey" },
    { L"添加热键执行命令/程序、打开文件或目录，或通过热键模拟按键和组合键。",
      L"Add a hotkey to run commands/programs, open files or folders, or simulate keys and shortcuts." },
    { L"透明度", L"Opacity" },
    { L"退出", L"Exit" },
    { L"退出键客时关闭已隐藏程序", L"Close hidden programs when KeySentry exits" },
    { L"外观", L"Appearance" },
    { L"未发现由键客造成异常的窗口", L"No windows were found to be corrupted by KeySentry" },
    { L"未设置", L"Not set" },
    { L"位置", L"Position" },
    { L"文件菜单", L"File menu" },
    { L"文件/目录:", L"File / folder:" },
    { L"文件/目录：", L"File / folder: " },
    { L"无法安装键盘钩子，热键捕获已取消。\n请检查是否有安全软件拦截。",
      L"Cannot install the keyboard hook; hotkey capture was cancelled.\nCheck whether security software is blocking it." },
    { L"无法打开资源管理器设置注册表项，文件夹选项设置未生效。",
      L"Cannot open the Explorer settings registry key; folder option changes did not take effect." },
    { L"无法获取该窗口的进程名称", L"Cannot get the process name of this window." },
    { L"无法识别该按键名称。\n请点击\"捕获快捷键\"或\"选择按键\"按钮重新设置主键。",
      L"Unrecognized key name.\nClick \"Capture shortcut\" or \"Select key\" to set the main key again." },
    { L"系统菜单", L"System menu" },
    { L"系统静音状态未能恢复（音频设备暂不可用），将在下次启动时重试解除。",
      L"The system mute state could not be restored (audio device temporarily unavailable). It will be retried at the next launch." },
    { L"下划线", L"Underline" },
    { L"下载", L"Download" },
    { L"先勾选修饰键，再点击主键", L"Select modifiers first, then click the main key." },
    { L"闲置自动隐藏", L"Idle auto-hide" },
    { L"显示", L"Show" },
    { L"启用静音状态提示", L"Enable mute status notifications" },
    { L"启用锁定键状态提示 (Caps/Num/Scroll Lock)", L"Enable lock key status notifications (Caps/Num/Scroll Lock)" },
    { L"现有窗口列表:", L"Existing windows:" },
    { L"新标签", L"New tab" },
    { L"新建", L"New" },
    { L"新建文件夹", L"New folder" },
    { L"新增自定义热键", L"Add custom hotkey" },
    { L"修复窗口堆叠", L"Fix window Z-order" },
    { L"修复完成", L"Repair complete" },
    { L"修饰键（Ctrl/Shift/Alt）不能加入屏蔽列表，\n",
      L"Modifier keys (Ctrl/Shift/Alt) cannot be added to the blocked list.\n" },
    { L"修饰键（Ctrl/Shift/Alt/Win）不能作为映射源键，\n否则会导致该修饰键整体失效。",
      L"Modifier keys (Ctrl/Shift/Alt/Win) cannot be used as mapping source keys,\notherwise the entire modifier would stop working." },
    { L"修饰键:", L"Modifiers:" },
    { L"修饰键不能作为组合键的主键。\n请先勾选下方的 Ctrl/Shift/Alt/Win，再点击要使用的主键。",
      L"A modifier cannot be the main key of a shortcut.\nSelect Ctrl/Shift/Alt/Win below, then click the main key." },
    { L"选择“否”将保留外部修改，本次运行中的设置不会写入。",
      L"Choosing \"No\" keeps the external changes; settings from this session will not be written." },
    { L"选择按键", L"Select key" },
    { L"选择工作目录", L"Select working directory" },
    { L"选择目标按键或组合键", L"Select target key or shortcut" },
    { L"选择映射按键", L"Select mapping key" },
    { L"选择文件...", L"Choose file..." },
    { L"选择文件夹...", L"Choose folder..." },
    { L"选择要打开的文件夹", L"Choose a folder to open" },
    { L"请选择或输入要打开的文件或目录", L"Choose or enter a file or folder to open." },
    { L"延时", L"Delay" },
    { L"颜色", L"Color" },
    { L"关闭程序热键:", L"Close-program hotkey:" },
    { L"一键关闭热键被其他程序占用: ", L"Close-program hotkey is occupied by another program: " },
    { L"一键关闭热键可能与常用快捷键冲突: ", L"Close-program hotkey may conflict with common shortcuts: " },
    // 常规页「状态提示」与窗口隐藏页「启用窗口隐藏」的浮窗说明
    { L"按下 Caps Lock / Num Lock / Scroll Lock 或静音键时，在屏幕上显示当前状态浮层。",
      L"Shows an on-screen status overlay when Caps Lock, Num Lock, Scroll Lock, or the mute key is pressed." },
    { L"快速隐藏/恢复指定窗口，保护隐私（老板键）。",
      L"Quickly hide or restore selected windows to protect privacy (boss key)." },
    { L"恢复默认", L"Restore defaults" },
    { L"一款掌控你的键盘和窗口的轻量工具。", L"A lightweight keyboard and window utility." },
    { L"已绑定的窗口/进程", L"Bound windows / processes" },
    { L"已添加窗口绑定: ", L"Window binding added: " },
    { L"已添加进程绑定: ", L"Process binding added: " },
    { L"已修复 %d 个窗口的置顶/最小化按钮状态", L"Fixed topmost/minimize button state for %d windows." },
    { L"以管理员运行", L"Run as administrator" },
    { L"以下热键存在冲突或被占用：\n\n", L"The following hotkeys conflict or are occupied:\n\n" },
    { L"（仅显示前 20 条冲突）", L"(Only the first 20 conflicts are shown)" },
    { L"以下自定义热键注册失败，可能与其他热键或程序冲突：\n",
      L"Failed to register the following custom hotkeys; they may conflict with other hotkeys or programs:\n" },
    { L"异常退出后窗口样式可能无法自动恢复。", L"Window styles may not be restored automatically after an abnormal exit." },
    { L"隐藏", L"Hide" },
    { L"隐藏/显示窗口热键:", L"Hide/show window hotkey:" },
    { L"隐藏窗口", L"Hide window" },
    { L"隐藏当前窗口", L"Hide the current window" },
    { L"隐藏时静音", L"Mute while hiding" },
    { L"隐藏受保护的操作系统文件", L"Hide protected operating system files" },
    { L"隐藏文件和文件夹", L"Hide files and folders" },
    { L"隐藏已知文件类型扩展名", L"Hide known file type extensions" },
    { L"应用", L"Apply" },
    { L"映射 →", L"Maps to →" },
    { L"映射目标", L"Mapping target" },
    { L"永久删除", L"Delete permanently" },
    { L"右侧", L"Right" },
    { L"右键菜单", L"Context menu" },
    { L"右上角隐藏", L"Hide at top-right corner" },
    { L"右下角隐藏", L"Hide at bottom-right corner" },
    { L"原始按键", L"Source key" },
    { L"圆角", L"Rounded" },
    { L"源按键和目标按键不能相同", L"The source and target keys must be different." },
    { L"允许用户阻止特定按键的输入，实现按键的屏蔽，避免误操作。",
      L"Allows you to block specific keys to prevent accidental input." },
    { L"运行参数:", L"Arguments:" },
    { L"运行命令", L"Run command" },
    { L"暂停媒体", L"Pause media" },
    { L"粘贴", L"Paste" },
    { L"正常", L"Normal" },
    { L"执行失败: ", L"Execution failed: " },
    { L"该程序要求管理员权限，无法以标准权限启动。\n请编辑该热键并勾选「以管理员运行」。",
      L"This program requires administrator privileges and cannot be started with standard permissions.\nEdit the hotkey and check \"Run as administrator\"." },
    { L"直角", L"Square" },
    { L"重命名", L"Rename" },
    { L"重做", L"Redo" },
    { L"主窗口创建失败", L"Failed to create the main window" },
    { L"主键:", L"Main key:" },
    { L"主键不能是修饰键（Ctrl/Shift/Alt/Win），请设置一个普通按键。",
      L"The main key cannot be a modifier (Ctrl/Shift/Alt/Win). Set a regular key." },
    { L"主题已切换，但配置保存失败，重启后可能恢复原主题。",
      L"The theme changed, but saving the configuration failed; the previous theme may return after restart." },
    { L"注意：互换靠模拟按键实现，输出按键仍会经过其他程序和系统，",
      L"Note: Swapping is implemented by simulating key presses; output still passes through other programs and the system, " },
    { L"注意：启用后单独按下「", L"Note: After enabling, pressing \"" },
    { L"状态提示", L"Status notifications" },
    { L"自定义热键被其他程序占用: ", L"Custom hotkey is occupied by another program: " },
    { L"自定义热键的总内容过长，无法安全写入配置文件。\n"
      L"请缩短命令/参数，或减少自定义热键数量后重试。\n"
      L"（建议序列化总长度不超过 30000 字符）",
      L"The total custom hotkey content is too long to write safely to the configuration file.\n"
      L"Shorten the command/arguments or reduce the number of custom hotkeys and try again.\n"
      L"(Recommended total serialized length: no more than 30000 characters)" },
    { L"自定义热键的总内容过长，无法安全写入配置文件。\n",
      L"The total custom hotkey content is too long to write safely to the configuration file.\n" },
    { L"自定义热键互相冲突: ", L"Custom hotkeys conflict with each other: " },
    { L"自定义热键可能与常用快捷键冲突: ", L"Custom hotkeys may conflict with common shortcuts: " },
    { L"自定义热键数量已达上限（100 个），无法继续添加。",
      L"The custom hotkey limit (100) has been reached; no more can be added." },
    { L"自定义热键需要至少一个修饰键（Ctrl/Shift/Alt/Win），否则会与正常输入冲突。",
      L"A custom hotkey needs at least one modifier (Ctrl/Shift/Alt/Win); otherwise it will conflict with normal input." },
    { L"自定义热键与窗口隐藏冲突: ", L"Custom hotkey conflicts with window hiding: " },
    { L"自定义热键与一键关闭热键冲突: ", L"Custom hotkey conflicts with the close-program hotkey: " },
    { L"最大化", L"Maximize" },
    { L"最小化", L"Minimize" },
    { L"左侧", L"Left" },
    { L"左上角隐藏", L"Hide at top-left corner" },
    { L"左下角隐藏", L"Hide at bottom-left corner" },
    // 动态消息的片段（含数字占位，整句无法精确匹配时按片段替换）
    { L"检测到上次异常退出，已恢复 ", L"Detected an abnormal exit last time. Restored " },
    { L" 个被隐藏的窗口。", L" hidden windows." },
    { L"已修复 ", L"Fixed " },
    { L" 个窗口的置顶/最小化按钮状态", L" windows' topmost/minimize button state." },
    { L"锁", L"Lock" },
    { L"语言已切换，但配置保存失败，重启后可能恢复原语言。",
      L"The language changed, but saving the configuration failed; the previous language may return after restart." },
    { L"外观或语言设置保存失败，重启后可能恢复原设置。",
      L"Failed to save the appearance or language setting; the previous setting may return after restart." },
    // 动态拼接消息中的收尾引号片段（单独「」不翻译，避免误伤正文）
    { L"」：\n\n", L"\":\n\n" },
    { L"」：", L"\":" },
    { L"」。\r\n\r\n", L"\".\r\n\r\n" },
    { L"」。\r\n", L"\".\r\n" },
    { L"」\n", L"\"\n" },
    { L"设置隐藏/显示窗口的全局热键，默认 Win+`。",
      L"Set the global hotkey for hiding/showing windows. The default is Win+`." },
    { L"鼠标隐藏：按下鼠标中键/侧键，或把鼠标移到屏幕对应角落，即可快速隐藏或恢复窗口。",
      L"Mouse hiding: press the middle/side mouse button, or move the cursor to the corresponding screen corner, to hide or restore windows." },
    { L"隐藏时静音：隐藏窗口时同时静音；\n隐藏当前窗口：同时隐藏当前活动窗口；\n暂停媒体：隐藏前发送媒体暂停命令；\n闲置自动隐藏：无操作达到设定分钟数后自动隐藏；\n退出键客时关闭已隐藏程序：退出时结束被隐藏的程序。",
      L"Mute while hiding: mute the system while hiding windows;\nHide the current window: also hide the currently active window;\nPause media: send a media pause command before hiding;\nAuto-hide when idle: hide after the configured number of minutes without input;\nClose hidden programs when KeySentry exits: end hidden programs on exit." },
    { L"按下鼠标中键隐藏/恢复窗口。", L"Press the middle mouse button to hide/restore windows." },
    { L"按下鼠标侧键1隐藏/恢复窗口。", L"Press mouse side button 1 to hide/restore windows." },
    { L"按下鼠标侧键2隐藏/恢复窗口。", L"Press mouse side button 2 to hide/restore windows." },
    { L"鼠标移动到屏幕该角落时自动隐藏窗口。", L"Hide windows automatically when the cursor moves to this screen corner." },
    { L"隐藏窗口时，把当前活动窗口也一并隐藏。", L"Also hide the currently active window when hiding." },
    { L"隐藏窗口时把系统静音，恢复时还原。", L"Mute the system while windows are hidden and restore it when they return." },
    { L"隐藏窗口前发送媒体暂停命令。", L"Send a media pause command before hiding windows." },
    { L"退出键客时关闭仍处于隐藏状态的程序；同一进程的其他窗口也会一并关闭。",
      L"Close hidden programs when KeySentry exits; other windows of the same process are closed too." },
    { L"外观：", L"Appearance: " },
    { L"语言", L"Language" },
    { L"语言：", L"Language: " },
    { L"（点击切换）", L" (click to switch)" },
    { L"（点击切换，立即生效并保存）", L" (click to switch; applies and saves immediately)" },
    { L"配置文件不可写：本次偏好仅在当前会话生效，不会保存。",
      L"The configuration file is not writable. This preference applies to the current session only." },
    // 提权模式下模拟按键热键的执行前确认（R8-H2）
    { L"触发热键：", L"Trigger hotkey: " },
    { L"注意：模拟按键会发送到当前前台窗口。", L"Note: Simulated keys are sent to the current foreground window." },
};

using Dict = std::map<std::wstring, std::wstring>;

const Dict& GetDict() {
    static const Dict dict = []() {
        Dict m;
        for (const auto& e : kEntries) {
            // 重复键会静默保留第一条（后续翻译不生效），由 FindDuplicateDictionaryKeys
            // 的自动化测试守护（R8-A1）
            m.emplace(e.zh, e.en);
        }
        return m;
    }();
    return dict;
}

using PhraseList = std::vector<std::pair<std::wstring, std::wstring>>;

// 片段替换表：仅取长度 >= 3 的条目，按长度降序，避免短词抢先替换
// 破坏更长的固定短语（如「窗口隐藏与一键关闭热键冲突: 」）。
const PhraseList& GetPhrases() {
    static const PhraseList phrases = []() {
        PhraseList v;
        for (const auto& e : kEntries) {
            std::wstring key(e.zh);
            // 常规片段按 3 字以上过滤；以「」开头的收尾引号片段允许 2 字，
            // 否则动态拼接消息会残留中文引号
            if (key.size() >= 3 || key == L" 个" ||
                (key.size() >= 2 && key[0] == L'」')) {
                v.emplace_back(key, std::wstring(e.en));
            }
        }
        std::sort(v.begin(), v.end(),
                  [](const std::pair<std::wstring, std::wstring>& a,
                     const std::pair<std::wstring, std::wstring>& b) {
                      return a.first.size() > b.first.size();
                  });
        return v;
    }();
    return phrases;
}

AppLanguage g_language = AppLanguage::Chinese;

// 把文本中的中文短语替换为英文；用于拼接型消息
std::wstring ReplacePhrases(const std::wstring& text) {
    std::wstring out = text;
    for (const auto& p : GetPhrases()) {
        size_t pos = 0;
        while ((pos = out.find(p.first, pos)) != std::wstring::npos) {
            out.replace(pos, p.first.size(), p.second);
            pos += p.second.size();
        }
    }
    return out;
}

void TranslateWindowText(HWND wnd) {
    if (!wnd) return;
    wchar_t buf[1024] = L"";
    const int len = GetWindowTextW(wnd, buf, (int)(sizeof(buf) / sizeof(buf[0])));
    if (len <= 0) return;
    const std::wstring zh(buf, (size_t)len);
    if (GetAppLanguage() != AppLanguage::English) return;
    const Dict& dict = GetDict();
    auto it = dict.find(zh);
    if (it != dict.end()) {
        SetWindowTextW(wnd, it->second.c_str());
    }
}

void TranslateTabItems(HWND wnd) {
    const int count = TabCtrl_GetItemCount(wnd);
    const Dict& dict = GetDict();
    for (int i = 0; i < count; ++i) {
        wchar_t buf[256] = L"";
        TCITEMW item = {};
        item.mask = TCIF_TEXT;
        item.pszText = buf;
        item.cchTextMax = (int)(sizeof(buf) / sizeof(buf[0]));
        if (!TabCtrl_GetItem(wnd, i, &item)) continue;
        auto it = dict.find(buf);
        if (it == dict.end()) continue;
        item.pszText = const_cast<LPWSTR>(it->second.c_str());
        // cchTextMax 表示缓冲区字符数，需要包含结尾 NUL；
        // 传 size() 在部分 comctl32 实现上会截掉最后一个字符（R13-A9）
        item.cchTextMax = (int)it->second.size() + 1;
        TabCtrl_SetItem(wnd, i, &item);
    }
}

void TranslateListColumns(HWND wnd) {
    const Dict& dict = GetDict();
    HWND header = (HWND)SendMessageW(wnd, LVM_GETHEADER, 0, 0);
    if (!header) return;
    const int count = (int)SendMessageW(header, HDM_GETITEMCOUNT, 0, 0);
    for (int i = 0; i < count; ++i) {
        wchar_t buf[256] = L"";
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT;
        col.pszText = buf;
        col.cchTextMax = (int)(sizeof(buf) / sizeof(buf[0]));
        if (!ListView_GetColumn(wnd, i, &col)) continue;
        auto it = dict.find(buf);
        if (it == dict.end()) continue;
        col.pszText = const_cast<LPWSTR>(it->second.c_str());
        col.cchTextMax = (int)it->second.size() + 1;  // 含结尾 NUL（R13-A9）
        ListView_SetColumn(wnd, i, &col);
    }
}

void TranslateComboItems(HWND wnd) {
    const int count = (int)SendMessageW(wnd, CB_GETCOUNT, 0, 0);
    if (count == CB_ERR || count <= 0) return;
    const Dict& dict = GetDict();
    const int sel = (int)SendMessageW(wnd, CB_GETCURSEL, 0, 0);
    for (int i = 0; i < count; ++i) {
        // 按实际长度分配缓冲：CB_GETLBTEXT 不接收缓冲区大小，
        // 固定 256 字符的栈缓冲在条目超长时会被越界写入（R12-M3）
        const LRESULT len = SendMessageW(wnd, CB_GETLBTEXTLEN, (WPARAM)i, 0);
        if (len == CB_ERR || len < 0) continue;
        std::wstring buf((size_t)len + 1, L'\0');
        const LRESULT copied = SendMessageW(wnd, CB_GETLBTEXT, (WPARAM)i, (LPARAM)&buf[0]);
        if (copied == CB_ERR) continue;
        buf.resize((size_t)copied);
        auto it = dict.find(buf);
        if (it == dict.end()) continue;
        SendMessageW(wnd, CB_DELETESTRING, (WPARAM)i, 0);
        SendMessageW(wnd, CB_INSERTSTRING, (WPARAM)i, (LPARAM)it->second.c_str());
    }
    if (sel != CB_ERR) {
        SendMessageW(wnd, CB_SETCURSEL, (WPARAM)sel, 0);
    }
}

BOOL CALLBACK TranslateChildProc(HWND child, LPARAM) {
    wchar_t cls[64] = L"";
    GetClassNameW(child, cls, (int)(sizeof(cls) / sizeof(cls[0])));
    // 编辑框内容是用户数据，不参与界面翻译
    if (_wcsicmp(cls, L"Edit") == 0) return TRUE;

    if (_wcsicmp(cls, L"ComboBox") == 0) {
        TranslateComboItems(child);
        return TRUE;
    }
    if (_wcsicmp(cls, L"SysListView32") == 0) {
        // 只翻译列头：列表单元格里既有固定值（插入时已经过 Tr）也有用户数据
        // （热键描述、命令、窗口标题）。整表按词典翻译会篡改用户数据（R8-M3）
        TranslateListColumns(child);
        return TRUE;
    }
    if (_wcsicmp(cls, L"SysTabControl32") == 0) {
        TranslateTabItems(child);
        return TRUE;
    }
    TranslateWindowText(child);
    return TRUE;
}

}  // namespace

// 词典重复键自检：同一条目出现两次以上时返回该中文键（R8-A1）
std::vector<std::wstring> FindDuplicateDictionaryKeys() {
    std::map<std::wstring, int> counts;
    std::vector<std::wstring> duplicates;
    for (const auto& e : kEntries) {
        if (++counts[e.zh] == 2) duplicates.emplace_back(e.zh);
    }
    return duplicates;
}

AppLanguage GetAppLanguage() {
    return g_language;
}

void SetAppLanguage(AppLanguage lang) {
    g_language = (lang == AppLanguage::English) ? AppLanguage::English
                                                : AppLanguage::Chinese;
}

const wchar_t* Tr(const wchar_t* zh) {
    if (!zh || g_language != AppLanguage::English) return zh;
    const Dict& dict = GetDict();
    auto it = dict.find(zh);
    return (it != dict.end()) ? it->second.c_str() : zh;
}

std::wstring TrStr(const std::wstring& zh) {
    if (g_language != AppLanguage::English) return zh;
    const Dict& dict = GetDict();
    auto it = dict.find(zh);
    return (it != dict.end()) ? it->second : zh;
}

std::wstring TranslateMessageText(const std::wstring& text) {
    if (g_language != AppLanguage::English) return text;
    const Dict& dict = GetDict();
    auto it = dict.find(text);
    if (it != dict.end()) return it->second;
    // 拼接型消息：按较长短语优先替换
    return ReplacePhrases(text);
}

void ApplyLanguageToWindowTree(HWND root) {
    if (!root || g_language != AppLanguage::English) return;
    TranslateWindowText(root);
    EnumChildWindows(root, TranslateChildProc, 0);
}
