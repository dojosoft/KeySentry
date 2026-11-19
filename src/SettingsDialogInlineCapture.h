#pragma once
#include <Windows.h>

// 内联热键捕获：老板键/关闭键/自定义热键编辑共用的吞键钩子。
// wParam=虚拟键码，lParam=修饰键组合(MOD_*)。
inline constexpr UINT WM_INLINE_CAPTURE_KEY = WM_USER + 302;

// 判断虚拟键码是否为纯修饰键（捕获时需继续等待主键）
bool InlineCapIsModifier(int vk);

// 开始/结束内联捕获；Begin 返回 false 表示钩子安装失败
bool BeginInlineCapture(HWND hwnd);
void EndInlineCapture(HWND owner);

// 当前是否有内联捕获钩子、其所属窗口
bool IsInlineCaptureActive();
HWND InlineCaptureOwner();
