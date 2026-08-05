// Hooks.RulesInject.cpp — 核心 hook + 生效性验证探针。
//
// 注入点 0x679A15 = Phobos 命名的 RulesData_LoadBeforeTypeData。
// 寄存器约定（已核实，DEVELOPMENT.md §4.2 / §9）：
//     ECX      = RulesClass*
//     [esp+4]  = CCINIClass*   ← 注入目标
//     补丁长度   = 0x6
//     return 0 = 继续原流程
//
// 【上一轮结论】0x679A15 处：pINI == INI_Rules，[General]=425、[VehicleTypes]=1011
// （rules 已完整装载，不是"太早"），WriteString 回读成功且写入能存活到 0x679CAF /
// 0x668F6A。FromAresInclude 读不到的原因已确认为探针 ini 从未创建，与 hook 时机无关。
//
// 【本轮目的】验证真正关键的一点：注入能否改变引擎最终解析出的 TypeClass。
// 手法：此处写 [E1]Strength=REAL_TEST_VALUE，然后在 Hooks.Probe.Late.cpp 里
// 于类型解析之后读 InfantryTypeClass::Find("E1")->Strength。读到该值即证明注入生效。
// 这个验证不依赖任何 [#include] 配置，也不需要肉眼看游戏。

#include <Syringe.h>
#include <Helpers/Macro.h>
#include <CCINIClass.h>
#include <RulesClass.h>

#include "Logger.h"
#include "ProbeShared.h"

namespace RulesInject {

    static bool s_done = false;   // 幂等：读档 / 重开局可能重入（§4.7）

    static void Diagnose(CCINIClass* pINI) {
        Log::Info("---- ra2hook diagnose @0x679A15 ----");

        CCINIClass* pRules = CCINIClass::INI_Rules;
        Log::Info("pINI=%p  INI_Rules=%p  same=%s",
                  (void*)pINI, (void*)pRules, (pINI == pRules) ? "YES" : "NO");

        // rules 装载深浅（上轮已证实为满，保留作回归对照）
        Log::Info("keycount [General]=%d  [E1]=%d",
                  pINI->GetKeyCount("General"), pINI->GetKeyCount("E1"));

        // ---- Ares [#include] 可见性 ----
        // 需要在 RA2 根目录建 ra2hook_probe.ini 并在 rulesmd.ini 的 [#include] 里引用。
        // 未配置时这里读到 <MISSING> 属正常，不影响下面的生效性验证。
        char buf[32] = {};
        pINI->ReadString("RA2HookProbe", "FromAresInclude", "<MISSING>", buf, sizeof(buf));
        Log::Info("[include test] FromAresInclude=[%s]  ([RA2HookProbe] keys=%d)",
                  buf, pINI->GetKeyCount("RA2HookProbe"));

        // ---- 生效性验证：写一个引擎真的会解析的键 ----
        // 先记录注入前的原值，便于对照（E1 香蒲步兵 vanilla Strength = 100）。
        char before[16] = {};
        pINI->ReadString("E1", "Strength", "<unset>", before, sizeof(before));

        char val[16] = {};
        std::snprintf(val, sizeof(val), "%d", Probe::kTestStrength);
        bool ok = pINI->WriteString("E1", "Strength", val);

        char after[16] = {};
        pINI->ReadString("E1", "Strength", "<unset>", after, sizeof(after));

        Log::Info("[effect test] wrote [E1]Strength: before=[%s] write_ok=%d after=[%s] (expect %d)",
                  before, ok ? 1 : 0, after, Probe::kTestStrength);

        Log::Info("---- end diagnose ----");
    }

    static void Apply(CCINIClass* pINI) {
        if (s_done) return;
        s_done = true;

        Diagnose(pINI);

        // TODO(阶段2)：生效性确认后，在此逐文件 MergeFile()。
    }

}  // namespace RulesInject

DEFINE_HOOK(0x679A15, RA2Hook_RulesInject, 0x6)
{
    GET_STACK(CCINIClass*, pINI, 0x4);
    RulesInject::Apply(pINI);
    return 0;
}
