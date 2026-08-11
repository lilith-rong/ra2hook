// Hooks.RulesInject.cpp — INI 注入（inject 方向）。
//
// 注入点 0x679A1B = RulesData_LoadBeforeTypeData 入口之后的后置窗口。
// 寄存器约定（已核实，DEVELOPMENT.md §4.2 / §9）：
//     ESI      = CCINIClass*   ← 0x679A15 的原始指令已经完成参数取值
//     补丁长度   = 0x5
//     return 0 = 继续原流程
//
// 【已实测验证】
//   pINI == INI_Rules，[General]=425（rules 已完整装载，非"太早"）
//   写入 [E1]Strength=543 → 类型解析后 InfantryTypeClass::Find("E1")->Strength
//   读到 543（原值 175，MO 自己改过）。即注入被引擎真实采纳。
//
// 【本轮实现】多目标注入：
//   每个引擎 INI 对象（rules/ra2md/art/ai/uimd/sound）对应一个注入目录：
//     ra2hook/inject/enabled/<target>/*.ini
//   目标表见 kTargets。装载时机均已核实（IDA，gamemd.exe 0x400000）：
//     - rules  挂点 0x679A1B：在 0x679A15 的 Ares/Phobos 处理之后、类型读取之前
//     - ra2md  与 rules 同一窗口（引擎先读 ra2md.ini 再读 rulesmd.ini）
//     - art    INI_Art 在 0x679A1B 前已装载（ARTMD.INI 于 sub_52CD70 0x52d053
//               ReadCCFile），art 读取循环（0x679a66）在注入点后 → 同挂钩点
//     - ai     AIMD.INI 于 sub_52CD70 0x52d378 读进 INI_AI，晚于 0x679A1B
//               → 独立挂点 0x52D37D（装载完成后立即注入，早于 AI 读取）
//     - uimd   UIMD.INI 于 sub_534FA0 0x535311 读进 INI_UIMD
//               → 独立挂点 0x53531A（装载完成后、数据读取 0x53533d 之前）
//     - sound  SOUNDMD.INI 于 sub_52BA60 0x52C763 读进**栈上局部** CCINIClass
//              （v72，非全局）。0x52C796 call sub_7510D0 时 ECX = &v72，
//              独立挂点 0x52C796（5 字节 call 整条覆盖，返回 0 时框架恢复
//              原 call，让引擎继续解析 [SoundList]/[Defaults]）。
//   InjectTarget 用「目标对象段数>0」作守卫：若某个对象尚未装载（或装载失败
//   后段数为 0）则跳过，避免写进无效对象。
//   显式 Files= 列表（旧版行为）仍保留：全部并进 rules。
//   inject 文件内的 [#include] 由 ra2hook 自己展开，独立于 Ares/Phobos：
//     - 不修改原 rules/art 的 [#include] 段
//     - 不把 inject 文件自己的 [#include] 段写入引擎目标对象
//     - 先合并当前文件，再按 include 键顺序深度优先合并被引用文件
//     - 文件内容通过 CCFileClass 原始读取后由本文件解析，不调用
//       CCINIClass::ReadCCFile，避免 Ares 的 ReadCCFile hook 自动展开一次。
//
// 【mix 装载】ra2hook/inject/mix/*.mix 在首次目标注入前全部 new MixFileClass 注册进引擎
//   Mixes 列表（MixFileClass.h:53 全局 MIXes，构造 0x5B3C20）。
//   SHP/VXL/PCX 这类资源由引擎按文件名惰性查找，mix 注册后即可引用。
//   注意：mix 内容是给"文件查找"用的，与 INI 对象无作用。

#include <Syringe.h>
#include <Helpers/Macro.h>
#include <CCINIClass.h>
#include <CCFileClass.h>
#include <MixFileClass.h>
#include <RulesClass.h>
#include <Memory.h>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "Config.h"
#include "IniOverlay.h"
#include "Logger.h"

namespace RulesInject {

    static bool s_done = false;    // 主钩子幂等（0x679A1B）：读档 / 重开局可能重入
    static bool s_ai_done = false;    // ai 钩子（0x52D37D）幂等
    static bool s_uimd_done = false;  // uimd 钩子（0x53531A）幂等
    static bool s_sound_done = false; // sound 钩子（0x52C796）幂等
    static bool s_mix_done = false;   // mix 注册幂等；ai/uimd/sound 可能早于主钩子

    static const char* kInjectRoot = "ra2hook\\inject";
    static const char* kMixDir     = "ra2hook\\inject\\mix";
    static const int   kMaxFiles   = IniOverlay::kMaxFiles;
    static const int   kPathMax    = IniOverlay::kPathMax;

    // 每个目标 = { 目录名, 引擎对象名（日志）, 取对象指针，是否已挂载 }
    // 注意 INI_Rules 是 CCINIClass*（指针），其余是 CCINIClass（对象，需取址）。
    static struct Target {
        const char* dir;                // enabled/ 下的子目录
        const char* label;              // 日志名
        CCINIClass* (*get)(void);      // 取目标对象
        bool        mapped;             // 已确认的注入时机
    } const kTargets[] = {
        { "rules", "rulesmd.ini", []{ return CCINIClass::INI_Rules; },  true },
        { "ra2md", "ra2md.ini",   []{ return &CCINIClass::INI_RA2MD; }, true },
        { "art",   "artmd.ini",   []{ return &CCINIClass::INI_Art; },   true },
        { "ai",    "aimd.ini",    []{ return &CCINIClass::INI_AI; },    true },
        { "uimd",  "uimd.ini",    []{ return &CCINIClass::INI_UIMD; },  true },
    };
    static const int kReadyAtMain = 3;   // 0x679A1B 时已就绪的目标数（rules/ra2md/art）

    static int CountSections(INIClass* pINI)
    {
        return IniOverlay::CountSections(pINI);
    }

    static int MergeFile(CCINIClass* pTarget, const char* path)
    {
        if (!pTarget || !path || !path[0]) return -1;

        CCINIClass staging;
        IniOverlay::MergeStats stats;
        const int keys = IniOverlay::MergeFile(&staging, path, &stats, "inject");
        if (keys < 0 || stats.errors > 0) {
            Log::Warn("inject: rejected %s: %s", path,
                      stats.firstError[0] ? stats.firstError : "read/parse failure");
            return -1;
        }

        IniOverlay::Copy(pTarget, &staging);
        return keys;
    }

    static bool FileExistsInEngineFS(const char* path)
    {
        CCFileClass file(path);
        if (!file.Exists()) {
            return false;
        }
        return true;
    }

    // 扫描目录里的通配文件，按文件名排序，返回数量。
    static int ScanDir(const char* dir, const char* wildcard,
                       char files[][kPathMax])
    {
        return IniOverlay::ScanDirectory(dir, wildcard, files);
    }

    // 显式 Files= 列表：逗号分隔的路径，全部并进 rules。
    static void InjectExplicit(CCINIClass* pINI)
    {
        const char* list = Config::Get().inject.files;
        if (!list || !list[0]) return;

        const char* p = list;
        while (*p) {
            while (*p == ',' || *p == ' ' || *p == '\t') ++p;
            if (!*p) break;

            const char* start = p;
            while (*p && *p != ',') ++p;
            const char* end = p;
            while (end > start && (end[-1] == ' ' || end[-1] == '\t')) --end;

            const size_t len = static_cast<size_t>(end - start);
            if (len == 0) continue;

            char path[kPathMax] = {};
            std::snprintf(path, sizeof(path), "%.*s", (int)len, start);
            MergeFile(pINI, path);
        }
    }

    // 对单个目标：扫描 enabled/<dir>，把目标对象注入。
    // 守卫：目标对象必须已装载（段数>0）才注入，否则写进未装载/未就绪的
    // 对象是无效的（引擎可能在装载时重建/覆盖该对象）。
    // 各目标装载时机：
    //   - rules/ra2md/art：0x679A1B 时已就绪（见 kTargets 注释），由主钩子注入
    //   - ai：AIMD.INI 读完（0x52d378）后，独立钩子 0x52D37D 注入
    //   - uimd：UIMD.INI 读完（0x535311）后，独立钩子 0x53531A 注入
    static int InjectTarget(const Target& t)
    {
        char dir[kPathMax] = {};
        std::snprintf(dir, sizeof(dir), "%s\\enabled\\%s", kInjectRoot, t.dir);

        char found[kMaxFiles][kPathMax] = {};
        const int n = ScanDir(dir, "*.ini", found);
        if (n < 0) {
            Log::Warn("inject: 无法完整扫描目录 %s，跳过", dir);
            return 0;
        }
        if (n == 0) {
            Log::Info("inject: 目录 %s 无 ini（空），跳过", dir);
            return 0;
        }

        if (!t.mapped) {
            Log::Info("inject: %s 目录 %s 有 %d 个 ini，但挂点未定，跳过",
                      t.label, dir, n);
            return 0;
        }

        CCINIClass* obj = t.get();
        if (!obj) {
            Log::Warn("inject: %s 目标对象缺失，跳过", t.label);
            return 0;
        }
        const int objSec = CountSections(obj);

        if (objSec <= 0) {
            // 已挂载的目标对象必须已装载（段数>0）才注；否则写了也没意义，
            // 引擎后续读取时可能把这个对象重建/覆盖掉。
            Log::Warn("inject: %s 目标对象当前段数 %d（尚未装载），跳过注入",
                      t.label, objSec);
            return 0;
        }

        int total = 0;
        for (int i = 0; i < n; ++i) {
            const int k = MergeFile(obj, found[i]);
            if (k < 0) continue;
            total += k;
        }

        Log::Info("inject: %s <- %s（%d 文件，%d 键）",
                  t.label, dir, n, total);
        Log::Info("inject: %s 注入后段数 %d->%d",
                  t.label, objSec, obj ? CountSections(obj) : -1);
        return total;
    }

    // 把 ra2hook/inject/mix/ 下全部 .mix 注册进引擎文件系统。
    // ai/uimd/sound 的挂点可能早于 rules 主挂点，因此不能只在主钩子注册。
    static void InjectMix()
    {
        if (s_mix_done) return;
        s_mix_done = true;

        char found[kMaxFiles][kPathMax] = {};
        const int n = ScanDir(kMixDir, "*.mix", found);
        if (n < 0) {
            Log::Warn("inject: 无法完整扫描目录 %s，跳过 mix 装载", kMixDir);
            return;
        }
        if (n == 0) {
            Log::Info("inject: 目录 %s 无 .mix，跳过 mix 装载", kMixDir);
            return;
        }

        for (int i = 0; i < n; ++i) {
            // MixFileClass 构造函数会把自身链入全局 Mixes 列表（0x5B3C20）。
            // 用 GameCreate 走引擎分配器,避免 DLL 池与游戏池混用导致释放错乱
            // （Memory.h 顶部注释:引擎类必须配引擎的 operator new）。
            if (!FileExistsInEngineFS(found[i])) {
                Log::Warn("inject: mix 文件不存在 %s，跳过", found[i]);
                continue;
            }
            MixFileClass* pMix = GameCreate<MixFileClass>(found[i]);
            if (pMix) {
                Log::Info("inject: 注册 mix %s", found[i]);
            } else {
                Log::Warn("inject: 注册 mix 失败 %s", found[i]);
            }
        }
    }

    // 主钩子 0x679A1B：rules/ra2md/art 在此刻已装载就绪，逐个注入。
    // ai（0x52D37D）/ uimd（0x53531A）在同一装载流程更晚处由独立钩子注入，
    // 不在此处理。
    static void Apply(CCINIClass* pINI)
    {
        if (s_done) return;
        s_done = true;

        Config::Load();
        if (!Config::Get().inject.enabled) {
            Log::Debug("inject: 已跳过（[Inject] Enabled 未开启）");
            return;
        }

        if (!pINI) {
            Log::Warn("inject @0x679A1B: pINI=null，跳过 rules/ra2md/art 注入");
            return;
        }

        char probe[64] = {};
        const bool hasProbe = pINI->Exists("RA2HookProbe", "FromAresInclude");
        if (hasProbe)
            pINI->ReadString("RA2HookProbe", "FromAresInclude", "",
                             probe, sizeof(probe));
        Log::Info("probe @0x679A1B: [RA2HookProbe]FromAresInclude=[%s]",
                  probe);

        Log::Info("inject @0x679A1B: pINI=%p [General]=%d Keys",
                  (void*)pINI, pINI->GetKeyCount("General"));

        if (Config::Get().inject.mix)
            InjectMix();

        // 显式 Files= 列表（旧版语义）优先，否则按目标目录注入。
        if (Config::Get().inject.files[0]) {
            InjectExplicit(CCINIClass::INI_Rules);
        } else {
            int total = 0;
            // 前 kReadyAtMain 项 = rules/ra2md/art（0x679A1B 时已就绪）。
            for (int i = 0; i < kReadyAtMain; ++i)
                total += InjectTarget(kTargets[i]);
            Log::Info("inject: 目标目录注入完成（rules/ra2md/art），共 %d 键", total);
        }
    }

    // ai 钩子 0x52D37D：AIMD.INI 已于 0x52d378 读进 INI_AI（装载完成），
    // 早于 AI 类型数据读取（sub_686B20 0x687984 一带），注入内容可被采纳。
    static void ApplyAi()
    {
        if (s_ai_done) return;
        s_ai_done = true;

        Config::Load();
        if (!Config::Get().inject.enabled) return;
        if (Config::Get().inject.files[0]) return;   // 显式列表模式只注 rules

        if (Config::Get().inject.mix)
            InjectMix();

        Log::Info("inject @0x52D37D: AIMD.INI 装载完成，注入 INI_AI");
        InjectTarget(kTargets[3]);   // ai
    }

    // uimd 钩子 0x53531A：UIMD.INI 已于 0x535311 读进 INI_UIMD（装载完成），
    // 早于 sub_674650 读取（0x53533d），注入内容可被采纳。
    static void ApplyUimd()
    {
        if (s_uimd_done) return;
        s_uimd_done = true;

        Config::Load();
        if (!Config::Get().inject.enabled) return;
        if (Config::Get().inject.files[0]) return;   // 显式列表模式只注 rules

        if (Config::Get().inject.mix)
            InjectMix();

        Log::Info("inject @0x53531A: UIMD.INI 装载完成，注入 INI_UIMD");
        InjectTarget(kTargets[4]);   // uimd
    }

    // sound 钩子 0x52C796：SOUNDMD.INI 已于 0x52C763 读进栈上局部 CCINIClass
    // （sub_52BA60 内 v72），随后 0x52C796 call sub_7510D0 解析 [Defaults]/
    // [SoundList]。此处 ECX = &v72（0x52C78F lea ecx,[esp+..var_2E14] 刚设置，
    // 未被打乱），直接注入该对象即可在引擎解析前写入定制键。
    // 与全局对象不同，v72 是栈临时对象，无法放进 kTargets（get 取不到），
    // 故单独实现、由钩子把指针传进来。
    static void ApplySound(CCINIClass* pSoundIni)
    {
        if (s_sound_done) return;
        s_sound_done = true;

        Config::Load();
        if (!Config::Get().inject.enabled) return;
        if (Config::Get().inject.files[0]) return;   // 显式列表模式只注 rules

        if (Config::Get().inject.mix)
            InjectMix();

        if (!pSoundIni) {
            Log::Warn("inject @0x52C796: 局部 SOUNDMD CCINIClass 为 null，跳过");
            return;
        }
        if (CountSections(pSoundIni) <= 0) {
            Log::Warn("inject @0x52C796: 局部 SOUNDMD 对象段数 <=0，跳过");
            return;
        }

        Log::Info("inject @0x52C796: SOUNDMD.INI 装载完成，注入局部 CCINIClass %p",
                  (void*)pSoundIni);

        char dir[kPathMax] = {};
        std::snprintf(dir, sizeof(dir), "%s\\enabled\\sound", kInjectRoot);

        char found[kMaxFiles][kPathMax] = {};
        const int n = ScanDir(dir, "*.ini", found);
        if (n < 0) {
            Log::Warn("inject: 无法完整扫描目录 %s，跳过", dir);
            return;
        }
        if (n == 0) {
            Log::Info("inject: 目录 %s 无 ini（空），跳过", dir);
            return;
        }

        int total = 0;
        for (int i = 0; i < n; ++i) {
            const int k = MergeFile(pSoundIni, found[i]);
            if (k < 0) continue;
            total += k;
        }

        Log::Info("inject: soundmd <- %s（%d 文件，%d 键）", dir, n, total);
    }

}  // namespace RulesInject

DEFINE_HOOK(0x679A1B, RA2Hook_RulesInject_PostAresPhobos, 0x5)
{
    GET(CCINIClass*, pINI, ESI);
    RulesInject::Apply(pINI);
    return 0;
}

// AIMD.INI 读入 INI_AI 之后（sub_52CD70, 0x52d378 call sub_4741F0 的下一条），
// 7 字节 lea 由钩子框架保存/恢复。
DEFINE_HOOK(0x52D37D, RA2Hook_AimdInject, 0x7)
{
    RulesInject::ApplyAi();
    return 0;
}

// UIMD.INI 读入 INI_UIMD 之后（sub_534FA0, 0x535311 call sub_4741F0 的下一条），
// 5 字节 mov 由钩子框架保存/恢复。
DEFINE_HOOK(0x53531A, RA2Hook_UimdInject, 0x5)
{
    RulesInject::ApplyUimd();
    return 0;
}

// SOUNDMD.INI 读入栈上局部 CCINIClass 后（sub_52BA60, 0x52c763 call
// sub_4741F0 成功分支），0x52C796 call sub_7510D0 之前。此刻 ECX = &v72
// （0x52C78F lea ecx,[esp+..var_2E14] 刚设置）。挂钩点恰好是 5 字节 call，
// 钩子框架整体保存/恢复，返回 0 后原 call 照常执行（引擎解析 [SoundList]）。
DEFINE_HOOK(0x52C796, RA2Hook_SoundInject, 0x5)
{
    GET(CCINIClass*, pSoundIni, ECX);
    RulesInject::ApplySound(pSoundIni);
    return 0;
}
