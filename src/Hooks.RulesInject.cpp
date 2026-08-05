// Hooks.RulesInject.cpp — 核心 hook + 诊断探针。
//
// 注入点 0x679A15 = Phobos 命名的 RulesData_LoadBeforeTypeData。
// 寄存器约定（已核实，见 DEVELOPMENT.md §4.2 / §9）：
//     ECX      = RulesClass*
//     [esp+4]  = CCINIClass*   ← 我们要写入的目标
//     补丁长度   = 0x6
//     return 0 = 继续原流程，不改控制流
//
// 【本轮目的】第一次探针在 0x679A15 读到 FromAresInclude=[] （空）。
// 空值有两类完全不同的原因，应对相反，必须先区分：
//   (A) hook 太早——此时 include 内容尚未并入 CCINIClass
//   (B) 探针键根本不存在——ini 没被 include 成功 / 路径错 / pINI 不是 rules
// 下面的诊断一次运行同时回答这两个问题，避免慢循环里反复猜。

#include <Syringe.h>           // DEFINE_HOOK / EXPORT_FUNC / REGISTERS
#include <Helpers/Macro.h>     // GET / GET_STACK
#include <CCINIClass.h>
#include <RulesClass.h>

#include "Logger.h"

namespace RulesInject {

    static bool s_done = false;   // 幂等：读档 / 重开局可能重入（§4.7）

    // 判定一个 section 是否存在且非空：GetKeyCount > 0。
    static int SectionKeyCount(CCINIClass* pINI, const char* section) {
        if (!pINI) return -1;
        return pINI->GetKeyCount(section);   // 0x526960
    }

    static void Diagnose(CCINIClass* pINI) {
        Log::Info("---- ra2hook diagnose @0x679A15 ----");

        // 1) pINI 是否就是全局 rules 实例？若不是，说明本 hook 拿到的 INI 不是
        //    rules，那么"读不到 rules 里的键"完全正常，问题在选点而非 include。
        CCINIClass* pRules = CCINIClass::INI_Rules;   // 0x887048
        Log::Info("pINI=%p  INI_Rules=%p  same=%s",
                  (void*)pINI, (void*)pRules, (pINI == pRules) ? "YES" : "NO");

        // 2) pINI 里到底有没有东西？拿几个 vanilla rules 必然存在的 section 探深浅。
        //    若这些都为 0，说明此刻 CCINIClass 尚未装载 rules 内容（hook 太早）。
        Log::Info("keycount [General]=%d  [InfantryTypes]=%d  [VehicleTypes]=%d  [E1]=%d",
                  SectionKeyCount(pINI, "General"),
                  SectionKeyCount(pINI, "InfantryTypes"),
                  SectionKeyCount(pINI, "VehicleTypes"),
                  SectionKeyCount(pINI, "E1"));

        // 3) 探针 section 本身在不在（区分"键不存在"与"值为空"）。
        Log::Info("keycount [RA2HookProbe]=%d", SectionKeyCount(pINI, "RA2HookProbe"));

        char buf[32] = {};
        int len = pINI->ReadString("RA2HookProbe", "FromAresInclude", "<MISSING>", buf, sizeof(buf));
        Log::Info("ReadString FromAresInclude -> len=%d value=[%s]", len, buf);

        // 4) 若探针 section 存在，把它的键全列出来——可看出是键名写错还是值空。
        int n = SectionKeyCount(pINI, "RA2HookProbe");
        for (int i = 0; i < n && i < 8; ++i) {
            const char* k = pINI->GetKeyName("RA2HookProbe", i);   // 0x526CC0
            Log::Info("  [RA2HookProbe] key[%d]=%s", i, k ? k : "(null)");
        }

        // 5) 同时查全局 rules 实例（若与 pINI 不同）。这能回答：
        //    "内容其实已经在 rules 里了，只是没在我拿到的这个 pINI 里"。
        if (pRules && pRules != pINI) {
            Log::Info("-- via INI_Rules --");
            Log::Info("keycount [General]=%d  [RA2HookProbe]=%d",
                      SectionKeyCount(pRules, "General"),
                      SectionKeyCount(pRules, "RA2HookProbe"));
            char b2[32] = {};
            pRules->ReadString("RA2HookProbe", "FromAresInclude", "<MISSING>", b2, sizeof(b2));
            Log::Info("INI_Rules FromAresInclude=[%s]", b2);
        }

        // 6) 写入连通性自测：往 pINI 写一个键再读回。
        //    读回成功 = WriteString 在此阶段可用（阶段2 的前提，§6 待验证第 2 项）。
        pINI->WriteString("RA2HookProbe", "WriteBackTest", "42");     // 0x528660
        char b3[16] = {};
        pINI->ReadString("RA2HookProbe", "WriteBackTest", "<FAIL>", b3, sizeof(b3));
        Log::Info("WriteString roundtrip -> [%s]  (expect 42)", b3);

        Log::Info("---- end diagnose ----");
    }

    static void Apply(CCINIClass* pINI) {
        if (s_done) return;
        s_done = true;

        Diagnose(pINI);

        // TODO(阶段2)：诊断结论明确后，在此逐文件 MergeFile()。
    }

}  // namespace RulesInject

// DEFINE_HOOK 展开为 declhook + EXPORT_FUNC，把 hook 写进 DLL 导出表；
// Syringe 启动时读取 .syhks00 段完成 patch。寄存器保存/现场恢复/被覆盖指令重放
// 全部由 Syringe 负责（§4.8）。
DEFINE_HOOK(0x679A15, RA2Hook_RulesInject, 0x6)
{
    GET_STACK(CCINIClass*, pINI, 0x4);
    RulesInject::Apply(pINI);
    return 0;
}
