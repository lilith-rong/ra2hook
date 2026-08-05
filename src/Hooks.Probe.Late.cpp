// Hooks.Probe.Late.cpp — 对照观测点（仅诊断，验证完即可删）。
//
// 目的：与 0x679A15 形成对照。若探针键在这些更晚的点能读到、而在 0x679A15 读不到，
// 就证明 0x679A15 太早（原因 A）；若所有点都读不到，则是探针 ini 本身没被 include
// 成功（原因 B）——两者应对相反，必须区分。
//
// 这两个地址与寄存器约定同样取自 Phobos 的 hook（已核实，DEVELOPMENT §9）：
//   0x679CAF  RulesData_LoadAfterTypeData   CCINIClass* 在 ESI
//   0x668F6A  Rules Read_File / InitializeAfterAllLoaded（全部加载完成）
//
// 注意 0x668F6A 处 Phobos 挂了两个 handler，说明同址串联可行；我们只读不写。

#include <Syringe.h>
#include <Helpers/Macro.h>
#include <CCINIClass.h>
#include <RulesClass.h>

#include "Logger.h"

namespace LateProbe {

    static void Report(const char* where, CCINIClass* pINI) {
        if (!pINI) { Log::Info("[late %s] pINI=null", where); return; }

        int nProbe = pINI->GetKeyCount("RA2HookProbe");
        int nGeneral = pINI->GetKeyCount("General");

        char buf[32] = {};
        pINI->ReadString("RA2HookProbe", "FromAresInclude", "<MISSING>", buf, sizeof(buf));

        Log::Info("[late %s] pINI=%p rules=%p same=%s  [General]=%d [RA2HookProbe]=%d  FromAresInclude=[%s]",
                  where, (void*)pINI, (void*)CCINIClass::INI_Rules,
                  (pINI == CCINIClass::INI_Rules) ? "YES" : "NO",
                  nGeneral, nProbe, buf);
    }

}  // namespace LateProbe

// 类型数据解析之后。CCINIClass* 在 ESI（照 Phobos 的取法）。
DEFINE_HOOK(0x679CAF, RA2Hook_LateProbe_AfterTypeData, 0x5)
{
    GET(CCINIClass*, pINI, ESI);
    static bool once = false;
    if (!once) { once = true; LateProbe::Report("0x679CAF AfterTypeData", pINI); }
    return 0;
}

// 全部加载完成。此处 Phobos 不取 pINI，我们直接查全局 rules 实例。
DEFINE_HOOK(0x668F6A, RA2Hook_LateProbe_AllLoaded, 0x5)
{
    static bool once = false;
    if (!once) { once = true; LateProbe::Report("0x668F6A AllLoaded(INI_Rules)", CCINIClass::INI_Rules); }
    return 0;
}
