// Hooks.RulesInject.cpp — INI 注入（inject 方向）。
//
// 注入点 0x679A15 = Phobos 命名的 RulesData_LoadBeforeTypeData。
// 寄存器约定（已核实，DEVELOPMENT.md §4.2 / §9）：
//     ECX      = RulesClass*
//     [esp+4]  = CCINIClass*   ← 注入目标
//     补丁长度   = 0x6
//     return 0 = 继续原流程
//
// 【已实测验证】
//   pINI == INI_Rules，[General]=425（rules 已完整装载，非"太早"）
//   写入 [E1]Strength=543 → 类型解析后 InfantryTypeClass::Find("E1")->Strength
//   读到 543（原值 175，MO 自己改过）。即注入被引擎真实采纳。
// 生效性测试代码已移除——留着会污染每局游戏。

#include <Syringe.h>
#include <Helpers/Macro.h>
#include <CCINIClass.h>
#include <RulesClass.h>

#include "Config.h"
#include "Logger.h"

namespace RulesInject {

    static bool s_done = false;   // 幂等：读档 / 重开局可能重入

    static void Apply(CCINIClass* pINI)
    {
        if (s_done) return;
        s_done = true;

        Config::Load();
        if (!Config::Get().inject.enabled) {
            Log::Debug("inject: 已跳过（[Inject] Enabled 未开启）");
            return;
        }

        Log::Info("inject @0x679A15: pINI=%p [General]=%d",
                  (void*)pINI, pINI->GetKeyCount("General"));

        // TODO(下一步)：遍历 ra2hook\inject\*.ini，逐键 WriteString 并进 pINI。
        //   语义：后写胜出（与 INI 直觉一致，也与"在 Ares include 之后加载"的需求一致）
        Log::Info("inject: 注入逻辑尚未实现，本轮为空操作");
    }

}  // namespace RulesInject

DEFINE_HOOK(0x679A15, RA2Hook_RulesInject, 0x6)
{
    GET_STACK(CCINIClass*, pINI, 0x4);
    RulesInject::Apply(pINI);
    return 0;
}
