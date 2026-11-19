#pragma once
#include <Windows.h>
#include <algorithm>
#include <cstddef>
#include <vector>

// ============================================================
// 键盘钩子的纯逻辑辅助
// 这些函数不接触钩子句柄、线程或全局状态，便于单元测试覆盖
// “映射释放/模拟抬起”这类容易产生卡键的边界（R15-A01）。
// ============================================================

// 修饰键位（MOD_*） -> 实际虚拟键码，与注入时使用的键一致
inline int HookModifierVK(UINT mod) {
    switch (mod) {
    case MOD_CONTROL: return VK_CONTROL;
    case MOD_SHIFT:   return VK_SHIFT;
    case MOD_ALT:     return VK_MENU;
    case MOD_WIN:     return VK_LWIN;
    default:          return 0;
    }
}

// 释放修饰键的顺序：按注入的逆序，避免组合状态错乱
inline const UINT* HookModifierReleaseOrder() {
    static const UINT order[] = { MOD_WIN, MOD_ALT, MOD_SHIFT, MOD_CONTROL };
    return order;
}

// 构造“释放全部仍按下映射”的按键序列：
//   - 多个源键映射到同一目标键时只释放一次；
//   - 目标键按出现顺序排列，随后按固定逆序释放注入过的修饰键。
// TRemap 需提供 sourceVK/targetVK/injectedMods 字段。
template <typename TRemap>
inline std::vector<int> BuildRemapReleaseKeys(const std::vector<TRemap>& active) {
    std::vector<int> targets;
    UINT injectedMods = 0;
    for (const auto& remap : active) {
        if (std::find(targets.begin(), targets.end(), remap.targetVK) == targets.end()) {
            targets.push_back(remap.targetVK);
        }
        injectedMods |= remap.injectedMods;
    }

    std::vector<int> keys = targets;
    const UINT* order = HookModifierReleaseOrder();
    for (int i = 0; i < 4; ++i) {
        if (injectedMods & order[i]) {
            const int vk = HookModifierVK(order[i]);
            if (vk != 0) keys.push_back(vk);
        }
    }
    return keys;
}

// 模拟输入旁路中处理某个源键抬起时的释放计划
struct SimulatedRemapReleasePlan {
    bool handled = false;      // 该源键是否属于活动映射
    std::size_t index = 0;     // 对应活动记录下标
    std::vector<int> keys;     // 需要补发 keyup 的虚拟键码
};

// 构造单个源键抬起时的释放序列：
//   - 目标键仍被其他源键按住时不释放目标键；
//   - 只释放本条记录注入、且其他仍按住记录不需要的修饰键。
template <typename TRemap>
inline SimulatedRemapReleasePlan BuildSimulatedRemapReleasePlan(
        const std::vector<TRemap>& active, int sourceVK) {
    SimulatedRemapReleasePlan plan;
    const auto it = std::find_if(active.begin(), active.end(),
                                 [sourceVK](const TRemap& remap) {
                                     return remap.sourceVK == sourceVK;
                                 });
    if (it == active.end()) return plan;

    plan.handled = true;
    plan.index = static_cast<std::size_t>(it - active.begin());

    bool targetStillHeld = false;
    UINT otherMods = 0;
    for (const auto& remap : active) {
        if (remap.sourceVK == sourceVK) continue;
        if (remap.targetVK == it->targetVK) targetStillHeld = true;
        otherMods |= remap.injectedMods;
    }

    if (!targetStillHeld) plan.keys.push_back(it->targetVK);
    const UINT releaseMods = it->injectedMods & ~otherMods;
    const UINT* order = HookModifierReleaseOrder();
    for (int i = 0; i < 4; ++i) {
        if (releaseMods & order[i]) {
            const int vk = HookModifierVK(order[i]);
            if (vk != 0) plan.keys.push_back(vk);
        }
    }
    return plan;
}
