#pragma once

// ============================================================
// 版本号单一来源：RC 版本资源、清单与关于对话框均从此处取值。
// 修改版本时只需更新本文件；CMake 会解析这些宏并生成
// res/KeySentry.manifest.in -> ${CMAKE_BINARY_DIR}/KeySentry.manifest。
// ============================================================
#define KS_VERSION_MAJOR 3
#define KS_VERSION_MINOR 0
#define KS_VERSION_PATCH 1
#define KS_VERSION_BUILD 1121

// RC 的 VALUE 字符串使用窄字符串宏
#define KS_VERSION_STRING_A "3.0.1.1121"
// C++ 界面使用宽字符串
#define KS_VERSION_STRING_W L"3.0.1.1121"
