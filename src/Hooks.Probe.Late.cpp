// Hooks.Probe.Late.cpp — 类型解析之后的观测点（仅诊断，验证完即可删）。
//
// 决定性验证：Hooks.RulesInject.cpp 在 0x679A15 往 CCINIClass 写了
// [E1]Strength=Probe::kTestStrength。这里在引擎把 INI 解析成 TypeClass 之后，
// 直接读 InfantryTypeClass::Find("E1")->Strength：
//
//   读到 kTestStrength(543) → 注入真实生效，整个方案成立，可进阶段 2
//   读到 100（vanilla 原值） → INI 写入未被引擎采纳，此点对"改数值"无效，需换策略
//
// 地址与寄存器约定取自 Phobos 的 hook（已核实，DEVELOPMENT §9）：
//   0x679CAF  RulesData_LoadAfterTypeData    CCINIClass* 在 ESI
//   0x668F6A  InitializeAfterAllLoaded（全部加载完成）

#include <Syringe.h>
#include <Helpers/Macro.h>
#include <CCINIClass.h>
#include <RulesClass.h>
#include <InfantryTypeClass.h>

#include "Logger.h"
#include "ProbeShared.h"

namespace LateProbe {

    static void Report(const char* where, CCINIClass* pINI) {
        // INI 层：我们写进去的值还在不在
        char sIni[16] = {};
        if (pINI)
            pINI->ReadString("E1", "Strength", "<unset>", sIni, sizeof(sIni));

        // TypeClass 层：引擎解析后的实际值 —— 这才是"是否生效"的答案
        InfantryTypeClass* pE1 = InfantryTypeClass::Find("E1");
        int parsed = pE1 ? pE1->Strength : -1;

        const bool applied = (parsed == Probe::kTestStrength);

        Log::Info("[late %s] E1: ini=[%s] TypeClass.Strength=%d -> %s",
                  where, pINI ? sIni : "(no ini)", parsed,
                  pE1 ? (applied ? "INJECTION APPLIED" : "not applied (vanilla?)")
                      : "E1 TypeClass not found");

        // include 可见性一并复查
        if (pINI) {
            char b[32] = {};
            pINI->ReadString("RA2HookProbe", "FromAresInclude", "<MISSING>", b, sizeof(b));
            Log::Info("[late %s] FromAresInclude=[%s]", where, b);
        }
    }

}  // namespace LateProbe

DEFINE_HOOK(0x679CAF, RA2Hook_LateProbe_AfterTypeData, 0x5)
{
    GET(CCINIClass*, pINI, ESI);
    static bool once = false;
    if (!once) { once = true; LateProbe::Report("0x679CAF AfterTypeData", pINI); }
    return 0;
}

DEFINE_HOOK(0x668F6A, RA2Hook_LateProbe_AllLoaded, 0x5)
{
    static bool once = false;
    if (!once) { once = true; LateProbe::Report("0x668F6A AllLoaded", CCINIClass::INI_Rules); }
    return 0;
}
