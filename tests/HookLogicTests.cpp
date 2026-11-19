// ===== 键盘钩子纯逻辑回归测试 =====
// 覆盖映射释放与模拟抬起中最容易造成卡键的边界：
// 目标键去重、修饰键释放顺序、其他源键仍按住目标键、修饰键被共享（R15-A01）。

#include "../src/HookLogic.h"

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

namespace {

struct TestRemap {
    int sourceVK;
    int targetVK;
    UINT injectedMods;
};

bool KeysEqual(const std::vector<int>& keys, std::initializer_list<int> expected) {
    return keys.size() == expected.size() &&
           std::equal(keys.begin(), keys.end(), expected.begin());
}

void TestModifierVK() {
    CHECK(HookModifierVK(MOD_CONTROL) == VK_CONTROL);
    CHECK(HookModifierVK(MOD_SHIFT) == VK_SHIFT);
    CHECK(HookModifierVK(MOD_ALT) == VK_MENU);
    CHECK(HookModifierVK(MOD_WIN) == VK_LWIN);
    CHECK(HookModifierVK(0) == 0);
}

void TestReleaseKeysEmpty() {
    const std::vector<TestRemap> active;
    CHECK(BuildRemapReleaseKeys(active).empty());
}

void TestReleaseKeysDedupeTarget() {
    // 两个源键映射到同一目标键：目标键只释放一次；
    // 修饰键按 WIN/ALT/SHIFT/CTRL 的逆序排列
    const std::vector<TestRemap> active = {
        { 'A', 'B', MOD_CONTROL },
        { 'C', 'B', MOD_SHIFT },
    };
    CHECK(KeysEqual(BuildRemapReleaseKeys(active),
                    { 'B', VK_SHIFT, VK_CONTROL }));
}

void TestReleaseKeysMultipleTargets() {
    const std::vector<TestRemap> active = {
        { 'A', 'B', MOD_CONTROL },
        { 'C', 'D', MOD_CONTROL },
    };
    CHECK(KeysEqual(BuildRemapReleaseKeys(active),
                    { 'B', 'D', VK_CONTROL }));
}

void TestReleaseKeysModifierOrder() {
    const std::vector<TestRemap> active = {
        { 'A', 'B', MOD_CONTROL | MOD_SHIFT | MOD_ALT | MOD_WIN },
    };
    CHECK(KeysEqual(BuildRemapReleaseKeys(active),
                    { 'B', VK_LWIN, VK_MENU, VK_SHIFT, VK_CONTROL }));
}

void TestSimulatedNotHandled() {
    const std::vector<TestRemap> active = {
        { 'A', 'B', MOD_CONTROL },
    };
    const SimulatedRemapReleasePlan plan =
        BuildSimulatedRemapReleasePlan(active, 'X');
    CHECK(!plan.handled);
    CHECK(plan.keys.empty());
}

void TestSimulatedTargetStillHeld() {
    // 另一个源键仍按住同一目标键：不释放目标键，只释放本条独有的修饰键
    const std::vector<TestRemap> active = {
        { 'A', 'B', MOD_CONTROL | MOD_SHIFT },
        { 'C', 'B', 0 },
    };
    const SimulatedRemapReleasePlan plan =
        BuildSimulatedRemapReleasePlan(active, 'A');
    CHECK(plan.handled);
    CHECK(plan.index == 0);
    CHECK(KeysEqual(plan.keys, { VK_SHIFT, VK_CONTROL }));
}

void TestSimulatedSharedModifier() {
    // 两条记录都注入了 Ctrl：释放 A 时不能释放 Ctrl，否则 C 的组合会失效
    const std::vector<TestRemap> active = {
        { 'A', 'B', MOD_CONTROL },
        { 'C', 'D', MOD_CONTROL },
    };
    const SimulatedRemapReleasePlan planA =
        BuildSimulatedRemapReleasePlan(active, 'A');
    CHECK(planA.handled);
    CHECK(KeysEqual(planA.keys, { 'B' }));

    const SimulatedRemapReleasePlan planC =
        BuildSimulatedRemapReleasePlan(active, 'C');
    CHECK(planC.handled);
    CHECK(planC.index == 1);
    CHECK(KeysEqual(planC.keys, { 'D' }));
}

void TestSimulatedUniqueModifier() {
    // 目标键无人共享、修饰键也无人共享：按逆序释放目标键与 Ctrl
    const std::vector<TestRemap> active = {
        { 'A', 'B', MOD_CONTROL },
    };
    const SimulatedRemapReleasePlan plan =
        BuildSimulatedRemapReleasePlan(active, 'A');
    CHECK(plan.handled);
    CHECK(KeysEqual(plan.keys, { 'B', VK_CONTROL }));
}

}  // namespace

int wmain() {
    TestModifierVK();
    TestReleaseKeysEmpty();
    TestReleaseKeysDedupeTarget();
    TestReleaseKeysMultipleTargets();
    TestReleaseKeysModifierOrder();
    TestSimulatedNotHandled();
    TestSimulatedTargetStillHeld();
    TestSimulatedSharedModifier();
    TestSimulatedUniqueModifier();
    if (g_failures == 0) std::printf("HookLogicTests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
