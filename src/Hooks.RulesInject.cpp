// Hooks.RulesInject.cpp — INI 注入（inject 方向）。
//
// 注入点 0x679A15 = Phobos 命名的 RulesData_LoadBeforeTypeData。
// 寄存器约定（已核实，DEVELOPMENT.md §4.2 / §9）：
//     ECX      = RulesClass*
//     [esp+4]  = CCINIClass*   ← 注入目标
//     补丁长度   = 0x6
//     return 0 = 继续原流程
//
// 【已验证】此点 pINI == INI_Rules；[General]=425、[VehicleTypes]=1011
// （rules 已完整装载，非"太早"）；WriteString 可写且写入能存活到类型解析之后。
//
// 【尚未验证】写入是否被引擎真正采纳（即改 [E1]Strength 后 TypeClass 是否变）。
// 该验证与本轮 dump 探针一起进行，见下方 effect test。

#include <Syringe.h>
#include <Helpers/Macro.h>
#include <CCINIClass.h>
#include <RulesClass.h>
#include <InfantryTypeClass.h>

#include <cstdio>

#include "Config.h"
#include "Logger.h"

namespace RulesInject {

    // 生效性测试值。E1（美国大兵）vanilla Strength = 100。
    constexpr int kTestStrength = 543;

    static bool s_done = false;   // 幂等：读档 / 重开局可能重入

    static void Apply(CCINIClass* pINI)
    {
        if (s_done) return;
        s_done = true;

        Config::Load();

        Log::Info("---- rules inject @0x679A15 ----");
        Log::Info("pINI=%p INI_Rules=%p same=%s  [General]=%d",
                  (void*)pINI, (void*)CCINIClass::INI_Rules,
                  (pINI == CCINIClass::INI_Rules) ? "YES" : "NO",
                  pINI->GetKeyCount("General"));

        // 生效性验证：写一个引擎真的会解析的键，稍后在 0x679CAF 读 TypeClass 对照。
        char before[16] = {};
        pINI->ReadString("E1", "Strength", "<unset>", before, sizeof(before));

        char val[16] = {};
        std::snprintf(val, sizeof(val), "%d", kTestStrength);
        const bool ok = pINI->WriteString("E1", "Strength", val);

        Log::Info("[effect test] [E1]Strength: before=[%s] write_ok=%d (期望解析后为 %d)",
                  before, ok ? 1 : 0, kTestStrength);

        // TODO(阶段2)：Config 里 inject.enabled 为真时，遍历 ra2hook\inject\*.ini
        //              逐键 WriteString 并进 pINI（后写胜出）。

        Log::Info("---- end rules inject ----");
    }

}  // namespace RulesInject

DEFINE_HOOK(0x679A15, RA2Hook_RulesInject, 0x6)
{
    GET_STACK(CCINIClass*, pINI, 0x4);
    RulesInject::Apply(pINI);
    return 0;
}

// 类型解析之后读回 TypeClass：这才是"注入是否被引擎采纳"的答案。
DEFINE_HOOK(0x679CAF, RA2Hook_InjectVerify, 0x5)
{
    static bool once = false;
    if (once) return 0;
    once = true;

    InfantryTypeClass* pE1 = InfantryTypeClass::Find("E1");
    const int parsed = pE1 ? pE1->Strength : -1;

    Log::Info("[effect test] 解析后 E1.Strength=%d -> %s",
              parsed,
              !pE1 ? "E1 未找到"
                   : (parsed == RulesInject::kTestStrength ? "注入已生效 APPLIED"
                                                           : "未采纳 (vanilla?)"));
    return 0;
}
