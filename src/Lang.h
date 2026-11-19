#pragma once
#include <Windows.h>
#include <string>
#include <vector>

// ============================================================
// Lang：界面语言与中英文翻译
// 说明：项目内的界面文本以中文原文创建，英文模式下通过本模块的
//       词典翻译。切换回中文时界面会按中文原文重建或重新翻译，
//       因此词典只需维护「中文 → 英文」单向映射。
// ============================================================
enum class AppLanguage {
    Chinese = 0,
    English = 1
};

AppLanguage GetAppLanguage();
void SetAppLanguage(AppLanguage lang);

// 取当前语言下的文本；英文模式且命中词典时返回英文，否则返回原文。
// 返回的指针在进程生命周期内有效（词典为静态存储）。
const wchar_t* Tr(const wchar_t* zh);
std::wstring TrStr(const std::wstring& zh);

// 翻译拼接型消息：先整体精确匹配，再按较长短语优先做片段替换。
// 用于热键冲突、绑定成功等由多段字符串拼接出来的提示。
std::wstring TranslateMessageText(const std::wstring& text);

// 遍历窗口树，把文本精确命中词典的控件翻译为目标语言。
// 覆盖普通控件、选项卡项、列表列头/列表项、下拉框选项；仅英文模式生效。
void ApplyLanguageToWindowTree(HWND root);

// 词典自检（供自动化测试）：返回所有重复的中文键。
// 构建词典时重复键会被静默忽略（保留第一条），导致后续翻译不生效，
// 因此新增/修改词条后必须保证该列表为空（R8-A1）
std::vector<std::wstring> FindDuplicateDictionaryKeys();
