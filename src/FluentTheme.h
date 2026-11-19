#pragma once
#include <Windows.h>

// ============================================================
// FluentTheme：Fluent Design（浅色）界面主题层
// 目标：让原生 Win32 控件呈现 Fluent 视觉 —— Segoe UI 排版、
//       白色内容面、圆角卡片分组、1px 细边框、强调色主按钮。
//
// 兼容性（Windows 7 SP1 / 10 / 11）：
//   - 全部绘制使用 GDI（系统自带），不引入任何第三方依赖
//   - Win11 专属的窗口圆角等效果通过动态加载 API 实现，
//     系统不支持时调用失败即静默忽略，不影响其他版本
//   - 不使用 Win8+ 专属 API（如 GetDpiForWindow），DPI 缩放复用 Utils.h
// ============================================================
namespace Fluent {

// ---- 外观主题 ----
// System：跟随 Windows 的浅色/深色设置，并采用系统主题色（Win7 回退浅色+默认蓝）
enum class ThemeMode { System = 0, Light = 1, Dark = 2 };

// 按钮外观：次要（白底细边框）/ 主要（强调色实底）/ 危险（红字红框）
enum class ButtonKind { Secondary, Primary, Danger };

// 设置外观主题；切换后需由调用方重绘窗口（见 Main.cpp 的外观菜单）
void SetThemeMode(ThemeMode mode);
ThemeMode GetThemeMode();
bool IsDarkTheme();
// 超链接文字色（跟随浅色/深色与高对比度）
COLORREF LinkColor();

// ---- 字体（Segoe UI；中文由系统字体链接自动回退到微软雅黑）----
HFONT UiFont();          // 正文 14px
HFONT UiFontSemibold();  // 正文半粗（分组标题）
HFONT SmallFont();       // 辅助文字 12px
// DPI 变化后重建主题字体（旧字体可能仍被控件引用，常驻到进程结束）
void RebuildFontsForDpi();
// 按指定缩放创建主题字体：用于子对话框跨 DPI 后重建控件字体，
// 调用方负责释放返回的 HFONT
HFONT CreateFontAtScale(int logicalHeight, int weight, float scale);

// 主题持有的画刷（调用方不得释放）
HBRUSH SurfaceBrush();
HBRUSH PageBrush();

// ---- 高对比度支持 ----
// 开启高对比度时全部控件回退为系统原生外观与系统配色
bool IsHighContrast();
// 高对比度状态变化后刷新缓存（WM_SETTINGCHANGE 中调用）
void RefreshHighContrastCache();
// 主题网格线画笔（缓存，调用方不得 DeleteObject）
HPEN GridPen();
COLORREF TextColor();          // 正文色
COLORREF TextDisabledColor();  // 禁用文字色
COLORREF SurfaceColor();       // 内容底色（绘制背景/挖空图形用）
COLORREF DisabledBackgroundColor(); // 禁用控件底色（列表行、按钮等）
COLORREF GridLineColor();      // 列表网格线（深色下接近背景的深灰）

// ---- 控件装饰（幂等，可重复调用）----
void DecorateButton(HWND button, ButtonKind kind = ButtonKind::Secondary);
void DecorateGroupBox(HWND groupBox);
void DecorateToggle(HWND toggle);   // 复选框 / 单选框
void DecorateInput(HWND control);   // 单行/多行编辑框、列表框、组合框
void DecorateCombo(HWND combo);     // 下拉框显示区（下拉列表沿用系统绘制）
void DecorateListView(HWND listView);
void DecorateTab(HWND tab);
// 遍历窗口内全部子控件，按类型自动装饰（按钮/分组框/输入类控件/列表）
void DecorateDialog(HWND root);

// Windows 11：为顶层窗口启用 Fluent 圆角；其他系统为空操作
void ApplyWindowChrome(HWND topLevel);

// 圆角矩形（radius 为圆角半径）：填充 + 1px 描边
void FillRoundRect(HDC hdc, const RECT& rc, int radius, COLORREF fill, COLORREF border);

// 主题化系统消息框：深色模式下用 CBT 钩子把 #32770 背景/文字切换为深色，
// 高对比度或浅色模式直接走系统 MessageBoxW；返回值与 MessageBox 一致
int ShowMessageBox(HWND owner, const wchar_t* text, const wchar_t* caption, UINT type);

// 同上，但不做任何语言翻译（原样显示）。
// 用于内容中含用户数据/命令的确认框：整段翻译会把用户字段里的中文短语
// 替换成英文，导致确认框显示内容与实际执行内容不一致
int ShowMessageBoxRaw(HWND owner, const wchar_t* text, const wchar_t* caption, UINT type);

}  // namespace Fluent
