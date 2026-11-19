// ===== 语言层最小回归测试 =====
// 覆盖：词典无重复键、中文直通、英文精确翻译、拼接消息片段替换。
// 词典重复键会被 std::map::emplace 静默忽略，导致后续翻译不生效（R8-A1）。

#include "../src/Lang.h"

#include <cstdio>
#include <string>

static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

static void TestNoDuplicateKeys() {
    const std::vector<std::wstring> duplicates = FindDuplicateDictionaryKeys();
    for (const auto& key : duplicates) {
        std::printf("重复的词典键: %ls\n", key.c_str());
    }
    CHECK(duplicates.empty());
}

static void TestChinesePassthrough() {
    SetAppLanguage(AppLanguage::Chinese);
    CHECK(std::wstring(Tr(L"确认")) == L"确认");
    CHECK(TrStr(L"未知文本：中文直通") == L"未知文本：中文直通");
    CHECK(TranslateMessageText(L"确认") == L"确认");
}

static void TestEnglishTranslation() {
    SetAppLanguage(AppLanguage::English);
    // 精确命中词典
    CHECK(std::wstring(Tr(L"确认")) == L"Confirm");
    // 未命中词典时回退原文，不能返回空指针/空串
    CHECK(std::wstring(Tr(L"词典中不存在的文本")) == L"词典中不存在的文本");
    // 拼接型消息：固定片段应被替换
    const std::wstring translated = TranslateMessageText(L"确认");
    CHECK(translated == L"Confirm");
    SetAppLanguage(AppLanguage::Chinese);
}

int wmain() {
    TestNoDuplicateKeys();
    TestChinesePassthrough();
    TestEnglishTranslation();
    if (g_failures == 0) std::printf("LangTests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
