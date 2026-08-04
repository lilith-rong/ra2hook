// Hooks.RulesInject.cpp — 核心 hook 骨架（阶段 1 探针 + 阶段 2 注入的落点）。
//
// 注入点 0x679A15 = Phobos 命名的 RulesData_LoadBeforeTypeData。
// 寄存器约定（已核实，见 DEVELOPMENT.md §4.2 / §9）：
//     ECX      = RulesClass*
//     [esp+4]  = CCINIClass*   ← 我们要写入的目标
//     补丁长度   = 0x6
//     return 0 = 继续原流程，不改控制流
//
// 阶段 1 只做一件事：探针（§4.4）。回答"此点能否看到 Ares [#include] 的内容"。
// 在证实之前，不要写任何真实注入逻辑——那是阶段 2 的事。

#include <Syringe.h>           // DEFINE_HOOK / EXPORT_FUNC / REGISTERS
#include <Helpers/Macro.h>     // GET / GET_STACK
#include <CCINIClass.h>
#include <RulesClass.h>

#include "Logger.h"

namespace RulesInject {

    static bool s_done = false;   // 幂等：读档 / 重开局可能重入（§4.7）

    // §4.4 探针：读一个应当由 Ares [#include] 引入的键，把结果写进日志。
    //   读到 "1"  → 假设成立，0x679A15 在 include 之后，设计不变。
    //   读到空    → 此点早于 include 处理，必须换点（候选：ReadCCFile 返回处）。
    static void Probe(CCINIClass* pINI) {
        char buf[8] = {};
        // ReadString @ 0x528A10（已核实）。签名：section, key, default, buf, size。
        pINI->ReadString("RA2HookProbe", "FromAresInclude", "", buf, sizeof(buf));
        Log::Info("probe @0x679A15: [RA2HookProbe]FromAresInclude=[%s]", buf);
    }

    static void Apply(CCINIClass* pINI) {
        if (s_done) return;
        s_done = true;

        // TODO(阶段1)：接入 Config，读取 injection.enabled / injection.probe。
        //             现在硬走探针，先把链路跑通。
        Probe(pINI);

        // TODO(阶段2)：待探针证实后，在此逐文件 MergeFile()。
        //   for (auto const& path : cfg.files)
        //       total += MergeFile(pINI, path);   // 逐键 WriteString @0x528660，后写胜出
        //   if (!cfg.dumpMergedRules.empty())
        //       DumpSnapshot(pINI, cfg.dumpMergedRules);
    }

}  // namespace RulesInject

// DEFINE_HOOK 展开为 declhook + EXPORT_FUNC，把 hook 写进 DLL 导出表；
// Syringe 启动时读取导出表完成 patch。寄存器保存/现场恢复/被覆盖指令重放
// 全部由 Syringe 负责——这就是"无需自写 trampoline"的原因（§4.8）。
DEFINE_HOOK(0x679A15, RA2Hook_RulesInject, 0x6)
{
    GET_STACK(CCINIClass*, pINI, 0x4);
    RulesInject::Apply(pINI);
    return 0;
}
