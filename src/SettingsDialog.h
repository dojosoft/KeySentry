#pragma once
#include <Windows.h>
#include <CommCtrl.h>
#include "Config.h"

// ===== 设置对话框类 =====
// 管理键客 KeySentry 的所有设置界面，包括常规、按键禁用、按键映射、
// 热键管理、窗口隐藏、窗口置顶六个选项卡页面的创建、交互和配置保存。
class SettingsDialog {
public:
    // 显示设置对话框的模态窗口入口
    // parent: 父窗口句柄，config: 应用配置的引用
    static INT_PTR Show(HWND parent, AppConfig& config);

    // 切换到指定索引的选项卡页面，销毁旧页面控件并创建新页面
    static void SwitchTab(HWND hwnd, int tabIndex, AppConfig& config);

    // 保存当前选项卡页面上的控件状态到配置对象中
    static void SaveCurrentTabState(HWND hwnd, AppConfig& config);

    // 应用设置：将工作副本写入实际配置，并检查热键冲突
    static bool ApplySettings(HWND hwnd, AppConfig& config);

    // 将修饰键和虚拟键码格式化为可读的热键字符串，如 "Ctrl+Alt+F1"
    static std::wstring FormatHotKey(UINT mod, UINT vk);

    // 设置应用回调函数，在配置被应用后通知外部模块
    static void SetApplyCallback(void (*callback)());
    static void (*s_applyCallback)();

    // 设置按键捕获模式回调，进入/退出捕获模式时通知钩子模块
    static void SetCaptureModeCallback(void (*callback)(bool));
    static void (*s_captureModeCallback)(bool);

    // 添加一个禁用按键（弹出按键捕获对话框）
    static void AddDisabledKey(HWND hwnd, AppConfig& config);
    // 从禁用按键列表中删除选中项
    static void RemoveDisabledKey(HWND hwnd, AppConfig& config);

    // 添加一个窗口隐藏绑定窗口（弹出绑定对话框）
    static void AddBossWindow(HWND hwnd, AppConfig& config);
    // 从窗口隐藏绑定列表中删除选中项
    static void RemoveBossWindow(HWND hwnd, AppConfig& config);

    // 添加一个窗口置顶绑定窗口（弹出绑定对话框）
    static void AddWindWindow(HWND hwnd, AppConfig& config);
    // 从窗口置顶绑定列表中删除选中项
    static void RemoveWindWindow(HWND hwnd, AppConfig& config);

    // 从窗口列表中选择一个窗口并填入编辑框（预留接口）
    static void SelectWindow(HWND hwnd, int listBoxId, int editId);

    // 开始捕获窗口隐藏热键（用户按下组合键后自动设置）
    static void StartHotKeyCapture(HWND hwnd);
    // 开始捕获一键关闭程序热键
    static void StartCloseHotKeyCapture(HWND hwnd);

    // 将窗口隐藏热键重置为默认值（Win+`）
    static void ResetHotKey(HWND hwnd);
    // 将一键关闭程序热键重置为默认值（Win+Esc）
    static void ResetCloseHotKey(HWND hwnd);

    // 刷新禁用按键列表视图的内容
    static void RefreshDisabledKeyList(HWND hwnd, AppConfig& config);
    // 刷新窗口隐藏绑定列表视图的内容
    static void RefreshBossWindowList(HWND hwnd, AppConfig& config);
    // 刷新所有窗口列表（用于绑定对话框中的可选窗口）
    static void RefreshAllWindowsList(HWND hwnd, AppConfig& config);
    // 刷新窗口置顶绑定列表视图的内容
    static void RefreshWindWindowList(HWND hwnd, AppConfig& config);

    // 根据启用状态更新禁用按键相关控件的可用性
    static void UpdateDisableKeysState(HWND hwnd, bool enabled);

    // 添加一条按键映射规则（先捕获源按键，再从键盘选择器选目标按键）
    static void AddRemapEntry(HWND hwnd, AppConfig& config);
    // 从按键映射列表中删除选中项
    static void RemoveRemapEntry(HWND hwnd, AppConfig& config);
    // 刷新按键映射列表视图的内容
    static void RefreshRemapList(HWND hwnd, AppConfig& config);
    // 根据启用状态更新按键映射相关控件的可用性
    static void UpdateRemapState(HWND hwnd, bool enabled);

    // 显示虚拟键盘选择器对话框，返回选中的虚拟键码，0 表示取消
    static int ShowKeyboardPicker(HWND parent);

    // 扫描系统中已被注册的全局热键，结果存入 scannedHotkeys
    static void ScanHotkeys(HWND hwnd, AppConfig& config);
    // 探测选中热键的归属程序（通过模拟按键触发后检测前台窗口变化）
    static void ProbeHotkey(HWND hwnd, AppConfig& config);
    // 将选中的已占用热键添加到禁用列表
    static void DisableHotkey(HWND hwnd, AppConfig& config);
    // 将选中的已禁用热键从禁用列表中移除（恢复启用）
    static void EnableHotkey(HWND hwnd, AppConfig& config);
    // 刷新系统热键列表视图的内容（支持过滤和搜索）
    static void RefreshHotkeyList(HWND hwnd, AppConfig& config);
    // 刷新已禁用热键列表（委托给 RefreshHotkeyList）
    static void RefreshDisabledHotkeyList(HWND hwnd, AppConfig& config);

    // 从所有窗口列表添加绑定（预留接口）
    static void AddBindingFromAll(HWND hwnd, AppConfig& config);
    // 移除绑定到所有窗口（委托给 RemoveBossWindow）
    static void RemoveBindingToAll(HWND hwnd, AppConfig& config);
    // 添加进程绑定（预留接口）
    static void AddProcessBinding(HWND hwnd, AppConfig& config);
    // 添加窗口绑定（预留接口）
    static void AddWindowBinding(HWND hwnd, AppConfig& config);
    // 显示添加绑定的模态对话框，支持按进程名/窗口名/手动输入三种方式
    static void ShowBindingDialog(HWND hwnd, AppConfig& config, std::vector<BoundWindowInfo>& boundWindows, const wchar_t* className);

    // 新增自定义热键（弹出热键编辑对话框）
    static void AddCustomHotkey(HWND hwnd, AppConfig& config);
    // 编辑选中的自定义热键
    static void EditCustomHotkey(HWND hwnd, AppConfig& config);
    // 删除选中的自定义热键
    static void DeleteCustomHotkey(HWND hwnd, AppConfig& config);
    // 刷新自定义热键列表视图的内容
    static void RefreshCustomHotkeyList(HWND hwnd, AppConfig& config);
    // 显示自定义热键编辑对话框，editIndex=-1 表示新增，否则为编辑模式
    static void ShowHotkeyDialog(HWND hwnd, AppConfig& config, int editIndex = -1);

    // 扫描到的热键信息结构体
    struct ScannedHotkey {
        UINT mod;                   // 修饰键标志（MOD_ALT | MOD_CONTROL 等）
        UINT vk;                    // 虚拟键码
        std::wstring ownerName;     // 归属程序名称（探测后填充）
    };

    // 对话框运行时数据结构体，存储在窗口的 GWLP_USERDATA 中
    struct DialogData {
        AppConfig* config;          // 指向原始配置的指针（用于取消时恢复）
        AppConfig workingCopy;      // 当前工作副本（用户修改尚未应用的配置）
        AppConfig originalConfig;   // 打开对话框时的原始配置快照
        int currentTab;             // 当前显示的选项卡索引（-1 表示未选中）
        bool capturingHotKey;       // 是否正在捕获窗口隐藏热键
        UINT captureMod;            // 捕获到的窗口隐藏热键修饰键
        UINT captureVK;             // 捕获到的窗口隐藏热键虚拟键码
        bool capturingCloseHotKey;  // 是否正在捕获一键关闭程序热键
        UINT captureCloseMod;       // 捕获到的一键关闭热键修饰键
        UINT captureCloseVK;        // 捕获到的一键关闭热键虚拟键码
        std::vector<ScannedHotkey> scannedHotkeys;  // 扫描到的系统热键列表
        std::vector<BoundWindowInfo> allWindowsCache; // 所有可选窗口的缓存
    };
};
