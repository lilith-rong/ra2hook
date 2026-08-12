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
//     - sound  使用两个独立挂点。0x52C6C4 在打开 SOUNDMD.INI 前预读配置、注册
//              MIX，并把 enabled/sound 合并到持久内存对象；0x7510F6 在
//              sub_7510D0 内取得 ECX=SOUNDMD 对象，只把预备内容复制进去。
//              旧 0x52C796（relative call）、0x52C78F（ESP-relative lea）和
//              0x7510D0（修改 ESP）均已实测会在 SyringeIH 下崩溃，不能使用。
//   InjectTarget 用「目标对象段数>0」作守卫：若某个对象尚未装载（或装载失败
//   后段数为 0）则跳过，避免写进无效对象。
//   注入目标只由 enabled/<target> 目录决定，不存在跨目标的全局文件列表。
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
#include <AircraftTypeClass.h>
#include <AnimTypeClass.h>
#include <BulletTypeClass.h>
#include <BuildingTypeClass.h>
#include <HouseTypeClass.h>
#include <InfantryTypeClass.h>
#include <OverlayTypeClass.h>
#include <ParticleSystemTypeClass.h>
#include <ParticleTypeClass.h>
#include <SmudgeTypeClass.h>
#include <SuperWeaponTypeClass.h>
#include <TerrainTypeClass.h>
#include <UnitTypeClass.h>
#include <VoxelAnimTypeClass.h>
#include <WarheadTypeClass.h>
#include <WeaponTypeClass.h>
#include <Memory.h>

#include <windows.h>

#include <cstdio>

#include "Config.h"
#include "IniOverlay.h"
#include "Logger.h"

namespace RulesInject {

    static bool s_done = false;    // 主钩子幂等（0x679A1B）：读档 / 重开局可能重入
    static bool s_ai_done = false;    // ai 钩子（0x52D37D）幂等
    static bool s_uimd_done = false;  // uimd 钩子（0x53531A）幂等
    static bool s_sound_prepare_done = false; // 0x52C6C4：文件 I/O 阶段幂等
    static bool s_sound_apply_done = false;   // 0x7510F6：内存应用阶段幂等
    static bool s_mix_done = false;   // mix 注册幂等；sound/ai/uimd 可能早于主钩子
    static CCINIClass* s_sound_overlay = nullptr; // 引擎分配，进程结束前有意保留

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
        const int keys = IniOverlay::MergeFile(&staging, path, &stats,
                                               "inject", true);
        if (keys < 0 || stats.errors > 0) {
            Log::Warn("inject: rejected %s: %s", path,
                      stats.firstError[0] ? stats.firstError : "read/parse failure");
            return -1;
        }

        if (stats.warnings > 0) {
            Log::Warn("inject: %s encountered %d recoverable issue(s): %s",
                      path, stats.warnings,
                      stats.firstWarning[0] ? stats.firstWarning : "ignored INI issue");
        }

        if (stats.appends > 0) {
            Log::Info("inject: %s preserved %d += append item(s)",
                      path, stats.appends);
        }

        IniOverlay::Copy(pTarget, &staging, false, true);
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

    static unsigned int HashList(CCINIClass* pINI, const char* sectionName)
    {
        if (!pINI || !sectionName || !sectionName[0]) return 0;

        auto* section = pINI->GetSection(sectionName);
        if (!section) return 0;

        // Hash complete entry strings in list order. ReadString would require
        // a fixed buffer and could miss a change beyond the truncated prefix.
        unsigned int hash = 2166136261u;
        int count = 0;
        const auto append = [&](const char* text) {
            if (!text) return;
            for (const unsigned char* p =
                     reinterpret_cast<const unsigned char*>(text); *p; ++p) {
                hash ^= static_cast<unsigned int>(*p);
                hash *= 16777619u;
            }
        };

        for (auto* node = section->Entries.GenericList::First();
             node && node->IsValid(); node = node->Next()) {
            auto* entry = static_cast<INIClass::INIEntry*>(node);
            if (!entry->Key) continue;
            ++count;
            append(entry->Key);
            hash ^= 0xFFu;
            hash *= 16777619u;
            append(entry->Value);
            hash ^= 0x00u;
            hash *= 16777619u;
        }
        return hash ^ static_cast<unsigned int>(count);
    }

    struct RulesListSnapshot {
        unsigned int countries;
        unsigned int overlayTypes;
        unsigned int superWeaponTypes;
        unsigned int warheads;
        unsigned int smudgeTypes;
        unsigned int terrainTypes;
        unsigned int buildingTypes;
        unsigned int vehicleTypes;
        unsigned int aircraftTypes;
        unsigned int infantryTypes;
        unsigned int animations;
        unsigned int voxelAnims;
        unsigned int particles;
        unsigned int particleSystems;
        unsigned int weaponTypes;
        unsigned int projectiles;
        unsigned int projectileAlias;
    };

    static RulesListSnapshot CaptureRulesListSnapshot(CCINIClass* pINI)
    {
        return {
            HashList(pINI, "Countries"),
            HashList(pINI, "OverlayTypes"),
            HashList(pINI, "SuperWeaponTypes"),
            HashList(pINI, "Warheads"),
            HashList(pINI, "SmudgeTypes"),
            HashList(pINI, "TerrainTypes"),
            HashList(pINI, "BuildingTypes"),
            HashList(pINI, "VehicleTypes"),
            HashList(pINI, "AircraftTypes"),
            HashList(pINI, "InfantryTypes"),
            HashList(pINI, "Animations"),
            HashList(pINI, "VoxelAnims"),
            HashList(pINI, "Particles"),
            HashList(pINI, "ParticleSystems"),
            HashList(pINI, "WeaponTypes"),
            HashList(pINI, "Projectiles"),
            HashList(pINI, "Projectile")
        };
    }

    enum class RulesTypeKind {
        Countries,
        OverlayTypes,
        SuperWeaponTypes,
        Warheads,
        SmudgeTypes,
        TerrainTypes,
        BuildingTypes,
        VehicleTypes,
        AircraftTypes,
        InfantryTypes,
        Animations,
        VoxelAnims,
        Particles,
        ParticleSystems,
        WeaponTypes,
        Projectiles,
        Count
    };

    static constexpr int kRulesTypeKindCount =
        static_cast<int>(RulesTypeKind::Count);
    static constexpr int kMaxTrackedTypeIds = 1024;

    struct TrackedRuleType {
        RulesTypeKind kind;
        char id[64];
    };

    struct RulesTypeDiagnostics {
        bool pending;
        int beforeCounts[kRulesTypeKindCount];
        bool changed[kRulesTypeKindCount];
        int candidates[kRulesTypeKindCount];
        int stored;
        TrackedRuleType types[kMaxTrackedTypeIds];
    };

    static RulesTypeDiagnostics s_typeDiagnostics = {};

    static int TypeKindIndex(RulesTypeKind kind)
    {
        return static_cast<int>(kind);
    }

    static const char* TypeKindSection(RulesTypeKind kind)
    {
        switch (kind) {
        case RulesTypeKind::Countries: return "Countries";
        case RulesTypeKind::OverlayTypes: return "OverlayTypes";
        case RulesTypeKind::SuperWeaponTypes: return "SuperWeaponTypes";
        case RulesTypeKind::Warheads: return "Warheads";
        case RulesTypeKind::SmudgeTypes: return "SmudgeTypes";
        case RulesTypeKind::TerrainTypes: return "TerrainTypes";
        case RulesTypeKind::BuildingTypes: return "BuildingTypes";
        case RulesTypeKind::VehicleTypes: return "VehicleTypes";
        case RulesTypeKind::AircraftTypes: return "AircraftTypes";
        case RulesTypeKind::InfantryTypes: return "InfantryTypes";
        case RulesTypeKind::Animations: return "Animations";
        case RulesTypeKind::VoxelAnims: return "VoxelAnims";
        case RulesTypeKind::Particles: return "Particles";
        case RulesTypeKind::ParticleSystems: return "ParticleSystems";
        case RulesTypeKind::WeaponTypes: return "WeaponTypes";
        case RulesTypeKind::Projectiles: return "Projectiles";
        default: return "unknown";
        }
    }

    static int CurrentTypeCount(RulesTypeKind kind)
    {
        switch (kind) {
        case RulesTypeKind::Countries: return HouseTypeClass::Array.Count;
        case RulesTypeKind::OverlayTypes: return OverlayTypeClass::Array.Count;
        case RulesTypeKind::SuperWeaponTypes:
            return SuperWeaponTypeClass::Array.Count;
        case RulesTypeKind::Warheads: return WarheadTypeClass::Array.Count;
        case RulesTypeKind::SmudgeTypes: return SmudgeTypeClass::Array.Count;
        case RulesTypeKind::TerrainTypes: return TerrainTypeClass::Array.Count;
        case RulesTypeKind::BuildingTypes: return BuildingTypeClass::Array.Count;
        case RulesTypeKind::VehicleTypes: return UnitTypeClass::Array.Count;
        case RulesTypeKind::AircraftTypes: return AircraftTypeClass::Array.Count;
        case RulesTypeKind::InfantryTypes: return InfantryTypeClass::Array.Count;
        case RulesTypeKind::Animations: return AnimTypeClass::Array.Count;
        case RulesTypeKind::VoxelAnims: return VoxelAnimTypeClass::Array.Count;
        case RulesTypeKind::Particles: return ParticleTypeClass::Array.Count;
        case RulesTypeKind::ParticleSystems:
            return ParticleSystemTypeClass::Array.Count;
        case RulesTypeKind::WeaponTypes: return WeaponTypeClass::Array.Count;
        case RulesTypeKind::Projectiles: return BulletTypeClass::Array.Count;
        default: return -1;
        }
    }

    static bool FindTrackedType(const TrackedRuleType& tracked)
    {
        switch (tracked.kind) {
        case RulesTypeKind::Countries:
            return HouseTypeClass::Find(tracked.id) != nullptr;
        case RulesTypeKind::OverlayTypes:
            return OverlayTypeClass::Find(tracked.id) != nullptr;
        case RulesTypeKind::SuperWeaponTypes:
            return SuperWeaponTypeClass::Find(tracked.id) != nullptr;
        case RulesTypeKind::Warheads:
            return WarheadTypeClass::Find(tracked.id) != nullptr;
        case RulesTypeKind::SmudgeTypes:
            return SmudgeTypeClass::Find(tracked.id) != nullptr;
        case RulesTypeKind::TerrainTypes:
            return TerrainTypeClass::Find(tracked.id) != nullptr;
        case RulesTypeKind::BuildingTypes:
            return BuildingTypeClass::Find(tracked.id) != nullptr;
        case RulesTypeKind::VehicleTypes:
            return UnitTypeClass::Find(tracked.id) != nullptr;
        case RulesTypeKind::AircraftTypes:
            return AircraftTypeClass::Find(tracked.id) != nullptr;
        case RulesTypeKind::InfantryTypes:
            return InfantryTypeClass::Find(tracked.id) != nullptr;
        case RulesTypeKind::Animations:
            return AnimTypeClass::Find(tracked.id) != nullptr;
        case RulesTypeKind::VoxelAnims:
            return VoxelAnimTypeClass::Find(tracked.id) != nullptr;
        case RulesTypeKind::Particles:
            return ParticleTypeClass::Find(tracked.id) != nullptr;
        case RulesTypeKind::ParticleSystems:
            return ParticleSystemTypeClass::Find(tracked.id) != nullptr;
        case RulesTypeKind::WeaponTypes:
            return WeaponTypeClass::Find(tracked.id) != nullptr;
        case RulesTypeKind::Projectiles:
            return BulletTypeClass::Find(tracked.id) != nullptr;
        default:
            return false;
        }
    }

    static void BeginTypeDiagnostics()
    {
        s_typeDiagnostics = {};
        s_typeDiagnostics.pending = true;
        for (int i = 0; i < kRulesTypeKindCount; ++i) {
            s_typeDiagnostics.beforeCounts[i] =
                CurrentTypeCount(static_cast<RulesTypeKind>(i));
        }
    }

    template <typename T>
    static void TrackNewListEntries(CCINIClass* pINI, const char* sectionName,
                                    RulesTypeKind kind)
    {
        if (!pINI || !sectionName) return;

        const int kindIndex = TypeKindIndex(kind);
        s_typeDiagnostics.changed[kindIndex] = true;

        const int count = pINI->GetKeyCount(sectionName);
        char id[128] = {};
        for (int i = 0; i < count; ++i) {
            const char* key = pINI->GetKeyName(sectionName, i);
            if (!key || !key[0]) continue;

            id[0] = '\0';
            if (pINI->ReadString(sectionName, key, "", id, sizeof(id)) <= 0 ||
                !id[0] || T::Find(id)) {
                continue;
            }

            bool duplicate = false;
            for (int j = 0; j < s_typeDiagnostics.stored; ++j) {
                const TrackedRuleType& existing = s_typeDiagnostics.types[j];
                if (existing.kind == kind && !_stricmp(existing.id, id)) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            ++s_typeDiagnostics.candidates[kindIndex];
            if (s_typeDiagnostics.stored >= kMaxTrackedTypeIds) continue;

            TrackedRuleType& tracked =
                s_typeDiagnostics.types[s_typeDiagnostics.stored++];
            tracked.kind = kind;
            std::snprintf(tracked.id, sizeof(tracked.id), "%s", id);
        }
    }

    static void ReportTypeDiagnostics()
    {
        if (!s_typeDiagnostics.pending) return;
        s_typeDiagnostics.pending = false;

        int changedSections = 0;
        int totalCandidates = 0;
        int totalFound = 0;
        int totalMissing = 0;
        int totalUntracked = 0;
        int successSamples = 0;
        int failureSamples = 0;

        for (int i = 0; i < kRulesTypeKindCount; ++i) {
            if (!s_typeDiagnostics.changed[i]) continue;
            ++changedSections;

            const RulesTypeKind kind = static_cast<RulesTypeKind>(i);
            int stored = 0;
            int found = 0;
            for (int j = 0; j < s_typeDiagnostics.stored; ++j) {
                const TrackedRuleType& tracked = s_typeDiagnostics.types[j];
                if (tracked.kind != kind) continue;
                ++stored;
                if (FindTrackedType(tracked)) ++found;
            }

            const int missing = stored - found;
            const int untracked = s_typeDiagnostics.candidates[i] - stored;
            totalCandidates += s_typeDiagnostics.candidates[i];
            totalFound += found;
            totalMissing += missing;
            totalUntracked += untracked;
            Log::Info("inject post @0x668EF5: [%s] array %d->%d, new candidates=%d, verified=%d/%d, missing=%d, untracked=%d",
                      TypeKindSection(kind),
                      s_typeDiagnostics.beforeCounts[i], CurrentTypeCount(kind),
                      s_typeDiagnostics.candidates[i], found, stored, missing,
                      untracked);
        }

        for (int i = 0; i < s_typeDiagnostics.stored; ++i) {
            const TrackedRuleType& tracked = s_typeDiagnostics.types[i];
            if (FindTrackedType(tracked)) {
                if (successSamples < 12) {
                    Log::Info("inject post: registered [%s] %s",
                              TypeKindSection(tracked.kind), tracked.id);
                    ++successSamples;
                }
            } else if (failureSamples < 12) {
                Log::Warn("inject post: missing [%s] %s after LoadTypesFromINI",
                          TypeKindSection(tracked.kind), tracked.id);
                ++failureSamples;
            }
        }

        if (changedSections == 0) {
            Log::Info("inject post @0x668EF5: LoadTypesFromINI 完成，rules 类型列表无变化");
            return;
        }

        Log::Info("inject post @0x668EF5: LoadTypesFromINI 完成，变化列表 %d，新增候选 %d，已跟踪 %d，找到 %d，缺失 %d，未跟踪 %d",
                  changedSections, totalCandidates, s_typeDiagnostics.stored,
                  totalFound, totalMissing, totalUntracked);
        if (totalUntracked > 0) {
            Log::Warn("inject post @0x668EF5: 新增候选超过 %d 个，仅对已跟踪项核验；不能据此判定全部注册成功",
                      kMaxTrackedTypeIds);
        }
    }

    struct RulesGlobalSnapshot {
        unsigned int maximums;
        unsigned int general;
        unsigned int powerups;
        unsigned int landCharacteristics;
        unsigned int iq;
        unsigned int jumpjetControls;
        unsigned int multiplayerDialogSettings;
        unsigned int ai;
    };

    static RulesGlobalSnapshot CaptureRulesGlobalSnapshot(CCINIClass* pINI)
    {
        return {
            HashList(pINI, "Maximums"),
            HashList(pINI, "General"),
            HashList(pINI, "Powerups"),
            HashList(pINI, "LandCharacteristics"),
            HashList(pINI, "IQ"),
            HashList(pINI, "JumpjetControls"),
            HashList(pINI, "MultiplayerDialogSettings"),
            HashList(pINI, "AI")
        };
    }

    // Init/Read_File consumes these sections before it reaches 0x679A1B. The
    // overlay therefore needs the corresponding native readers, otherwise the
    // CCINIClass changes but RulesClass keeps its old cached values. Sections
    // naturally read after LoadTypesFromINI (difficulty, damage, audiovisual,
    // crates, radiation, elevation/wall, special weapons and command bar) are
    // deliberately left to the original flow. Sides, Colors/ColorAdd and other
    // structural registries are not replayed because they mutate global arrays.
    static void ReloadInjectedGlobalRules(
        CCINIClass* pINI, const RulesGlobalSnapshot& before)
    {
        RulesClass* rules = RulesClass::Instance;
        if (!rules || !pINI) {
            Log::Warn("inject: 无法重读 rules 全局段（RulesClass 或 INI 为空）");
            return;
        }

        int changed = 0;
        int failed = 0;
        const auto reload = [&](const char* section, unsigned int oldHash,
                                bool (RulesClass::*reader)(CCINIClass*)) {
            if (HashList(pINI, section) == oldHash) return;
            ++changed;
            if (!(rules->*reader)(pINI)) {
                ++failed;
                Log::Warn("inject: 重读 rules 全局段 [%s] 失败", section);
            } else {
                Log::Info("inject: 重读 rules 全局段 [%s]", section);
            }
        };

        reload("Maximums", before.maximums, &RulesClass::Read_Maximums);
        reload("JumpjetControls", before.jumpjetControls,
               &RulesClass::Read_JumpjetControls);
        reload("MultiplayerDialogSettings", before.multiplayerDialogSettings,
               &RulesClass::Read_MultiplayerDialogSettings);
        reload("AI", before.ai, &RulesClass::Read_AI);
        reload("Powerups", before.powerups, &RulesClass::Read_Powerups);
        reload("LandCharacteristics", before.landCharacteristics,
               &RulesClass::Read_LandCharacteristics);
        reload("IQ", before.iq, &RulesClass::Read_IQ);
        reload("General", before.general, &RulesClass::Read_General);

        Log::Info("inject: rules 全局段重读完成（变化 %d，失败 %d）",
                  changed, failed);
    }

    // RulesClass has no Read_WeaponTypes/Read_Projectiles helpers. Those two
    // lists are consumed by WeaponTypeClass/BulletTypeClass references while
    // LoadTypesFromINI is running, so create their entries explicitly before
    // the original virtual LoadFromINI loops start.
    template <typename T>
    static int RegisterFindOrAllocateList(CCINIClass* pINI,
                                           const char* sectionName,
                                           unsigned int beforeHash)
    {
        if (!pINI || !sectionName || !sectionName[0]) return 0;
        if (HashList(pINI, sectionName) == beforeHash) return 0;

        const int count = pINI->GetKeyCount(sectionName);
        int created = 0;
        int missing = 0;
        char id[128] = {};
        for (int i = 0; i < count; ++i) {
            const char* key = pINI->GetKeyName(sectionName, i);
            if (!key || !key[0]) continue;

            id[0] = '\0';
            if (pINI->ReadString(sectionName, key, "", id, sizeof(id)) <= 0 ||
                !id[0]) {
                continue;
            }

            T* before = T::Find(id);
            T* value = T::FindOrAllocate(id);
            if (!value) {
                ++missing;
            } else if (!before) {
                ++created;
            }
        }

        if (count > 0) {
            Log::Info("inject: [%s] scanned %d key(s), allocated %d new type(s)%s",
                      sectionName, count, created,
                      missing > 0 ? ", some entries failed" : "");
        }
        return created;
    }

    // Read_File registers the names in these list sections before it calls
    // LoadTypesFromINI. The post-Ares hook runs after that step, so a private
    // overlay can contain the new definition but the engine still cannot find
    // the new type. Re-run only the list registration readers here; the normal
    // LoadTypesFromINI flow below then loads the definitions and fields.
    static void RegisterInjectedTypes(CCINIClass* pINI,
                                      const RulesListSnapshot& before)
    {
        RulesClass* rules = RulesClass::Instance;
        if (!rules || !pINI) {
            Log::Warn("inject: 无法补注册 rules 类型（RulesClass 或 INI 为空）");
            return;
        }

        int registeredLists = 0;
        const auto changed = [&](const char* name, unsigned int oldHash) {
            return HashList(pINI, name) != oldHash;
        };

        if (changed("Countries", before.countries)) {
            TrackNewListEntries<HouseTypeClass>(
                pINI, "Countries", RulesTypeKind::Countries);
            registeredLists += rules->Read_Countries(pINI) ? 1 : 0;
        }
        if (changed("OverlayTypes", before.overlayTypes)) {
            TrackNewListEntries<OverlayTypeClass>(
                pINI, "OverlayTypes", RulesTypeKind::OverlayTypes);
            registeredLists += rules->Read_OverlayTypes(pINI) ? 1 : 0;
        }
        if (changed("SuperWeaponTypes", before.superWeaponTypes)) {
            TrackNewListEntries<SuperWeaponTypeClass>(
                pINI, "SuperWeaponTypes", RulesTypeKind::SuperWeaponTypes);
            registeredLists += rules->Read_SuperWeaponTypes(pINI) ? 1 : 0;
        }
        if (changed("Warheads", before.warheads)) {
            TrackNewListEntries<WarheadTypeClass>(
                pINI, "Warheads", RulesTypeKind::Warheads);
            registeredLists += rules->Read_Warheads(pINI) ? 1 : 0;
        }
        if (changed("SmudgeTypes", before.smudgeTypes)) {
            TrackNewListEntries<SmudgeTypeClass>(
                pINI, "SmudgeTypes", RulesTypeKind::SmudgeTypes);
            registeredLists += rules->Read_SmudgeTypes(pINI) ? 1 : 0;
        }
        if (changed("TerrainTypes", before.terrainTypes)) {
            TrackNewListEntries<TerrainTypeClass>(
                pINI, "TerrainTypes", RulesTypeKind::TerrainTypes);
            registeredLists += rules->Read_TerrainTypes(pINI) ? 1 : 0;
        }
        if (changed("BuildingTypes", before.buildingTypes)) {
            TrackNewListEntries<BuildingTypeClass>(
                pINI, "BuildingTypes", RulesTypeKind::BuildingTypes);
            registeredLists += rules->Read_BuildingTypes(pINI) ? 1 : 0;
        }
        if (changed("VehicleTypes", before.vehicleTypes)) {
            TrackNewListEntries<UnitTypeClass>(
                pINI, "VehicleTypes", RulesTypeKind::VehicleTypes);
            registeredLists += rules->Read_VehicleTypes(pINI) ? 1 : 0;
        }
        if (changed("AircraftTypes", before.aircraftTypes)) {
            TrackNewListEntries<AircraftTypeClass>(
                pINI, "AircraftTypes", RulesTypeKind::AircraftTypes);
            registeredLists += rules->Read_AircraftTypes(pINI) ? 1 : 0;
        }
        if (changed("InfantryTypes", before.infantryTypes)) {
            TrackNewListEntries<InfantryTypeClass>(
                pINI, "InfantryTypes", RulesTypeKind::InfantryTypes);
            registeredLists += rules->Read_InfantryTypes(pINI) ? 1 : 0;
        }
        if (changed("Animations", before.animations)) {
            TrackNewListEntries<AnimTypeClass>(
                pINI, "Animations", RulesTypeKind::Animations);
            registeredLists += rules->Read_Animations(pINI) ? 1 : 0;
        }
        if (changed("VoxelAnims", before.voxelAnims)) {
            TrackNewListEntries<VoxelAnimTypeClass>(
                pINI, "VoxelAnims", RulesTypeKind::VoxelAnims);
            registeredLists += rules->Read_VoxelAnims(pINI) ? 1 : 0;
        }
        if (changed("Particles", before.particles)) {
            TrackNewListEntries<ParticleTypeClass>(
                pINI, "Particles", RulesTypeKind::Particles);
            registeredLists += rules->Read_Particles(pINI) ? 1 : 0;
        }
        if (changed("ParticleSystems", before.particleSystems)) {
            TrackNewListEntries<ParticleSystemTypeClass>(
                pINI, "ParticleSystems", RulesTypeKind::ParticleSystems);
            registeredLists += rules->Read_ParticleSystems(pINI) ? 1 : 0;
        }

        if (changed("WeaponTypes", before.weaponTypes)) {
            TrackNewListEntries<WeaponTypeClass>(
                pINI, "WeaponTypes", RulesTypeKind::WeaponTypes);
        }
        if (changed("Projectiles", before.projectiles)) {
            TrackNewListEntries<BulletTypeClass>(
                pINI, "Projectiles", RulesTypeKind::Projectiles);
        }
        // Some community INIs use singular [Projectile] as an append-list
        // alias. Keep the section intact, but register its IDs as BulletTypes.
        if (changed("Projectile", before.projectileAlias)) {
            TrackNewListEntries<BulletTypeClass>(
                pINI, "Projectile", RulesTypeKind::Projectiles);
        }

        const int weaponTypes = RegisterFindOrAllocateList<WeaponTypeClass>(
            pINI, "WeaponTypes", before.weaponTypes);
        int projectileTypes = RegisterFindOrAllocateList<BulletTypeClass>(
            pINI, "Projectiles", before.projectiles);
        projectileTypes += RegisterFindOrAllocateList<BulletTypeClass>(
            pINI, "Projectile", before.projectileAlias);

        Log::Info("inject: 已补注册 %d 个 rules 类型列表，额外分配 WeaponTypes=%d Projectiles=%d，等待 LoadTypesFromINI 加载定义",
                  registeredLists, weaponTypes, projectileTypes);
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

        int total = 0;
        const RulesGlobalSnapshot beforeGlobals =
            CaptureRulesGlobalSnapshot(pINI);
        const RulesListSnapshot beforeLists = CaptureRulesListSnapshot(pINI);
        BeginTypeDiagnostics();
        // 前 kReadyAtMain 项 = rules/ra2md/art（0x679A1B 时已就绪）。
        for (int i = 0; i < kReadyAtMain; ++i)
            total += InjectTarget(kTargets[i]);

        RegisterInjectedTypes(pINI, beforeLists);
        // Match the native Read_File dependency order: list registries exist
        // before global sections that may resolve references to those types.
        ReloadInjectedGlobalRules(pINI, beforeGlobals);
        Log::Info("inject: 目标目录注入完成（rules/ra2md/art），共 %d 键", total);
    }

    // ai 钩子 0x52D37D：AIMD.INI 已于 0x52d378 读进 INI_AI（装载完成），
    // 早于 AI 类型数据读取（sub_686B20 0x687984 一带），注入内容可被采纳。
    static void ApplyAi()
    {
        if (s_ai_done) return;
        s_ai_done = true;

        Config::Load();
        if (!Config::Get().inject.enabled) return;

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

        if (Config::Get().inject.mix)
            InjectMix();

        Log::Info("inject @0x53531A: UIMD.INI 装载完成，注入 INI_UIMD");
        InjectTarget(kTargets[4]);   // uimd
    }

    // 0x52C6C4 位于打开 SOUNDMD.INI 之前。配置读取、MIX 注册、目录扫描和
    // include 展开必须全部在这里完成；在 sub_7510D0 内做这些操作会重入
    // Ares 的 INI 解析链。
    static void PrepareSound()
    {
        if (s_sound_prepare_done) return;
        s_sound_prepare_done = true;

        Config::Load();
        if (!Config::Get().inject.enabled) return;

        if (Config::Get().inject.mix)
            InjectMix();

        char dir[kPathMax] = {};
        std::snprintf(dir, sizeof(dir), "%s\\enabled\\sound", kInjectRoot);

        CCINIClass staging;
        IniOverlay::MergeStats stats;
        if (!IniOverlay::MergeDirectory(&staging, dir, &stats, "inject.sound")) {
            Log::Warn("inject prepare @0x52C6C4: SOUNDMD 覆盖层构建失败，整层跳过：%s",
                      stats.firstError[0] ? stats.firstError : "read/parse failure");
            return;
        }

        const int sections = CountSections(&staging);
        if (sections <= 0) {
            Log::Info("inject prepare @0x52C6C4: 目录 %s 无可应用内容", dir);
            return;
        }

        s_sound_overlay = GameCreate<CCINIClass>();
        IniOverlay::Copy(s_sound_overlay, &staging);
        Log::Info("inject prepare @0x52C6C4: SOUNDMD 覆盖层已就绪（%d 文件，%d 段，%d 键）",
                  stats.files, sections, stats.keys);
    }

    // 0x7510F6 位于 sub_7510D0 内，0x7510F4 已执行 mov ecx,edi，因此 ECX
    // 是完成原始 SOUNDMD.INI 装载的局部 CCINIClass。这里只做内存复制，随后
    // 原函数从 0x751114 开始读取 [Defaults]，之后再处理 [SoundList]。
    static void ApplyPreparedSound(CCINIClass* pSoundIni)
    {
        if (s_sound_apply_done) return;
        s_sound_apply_done = true;

        if (!s_sound_prepare_done) {
            Log::Warn("inject apply @0x7510F6: SOUNDMD 覆盖层尚未预备，跳过");
            return;
        }
        if (!s_sound_overlay) return;

        if (!pSoundIni) {
            Log::Warn("inject apply @0x7510F6: SOUNDMD CCINIClass 为 null，跳过");
            return;
        }
        const DWORD vtable = *reinterpret_cast<const DWORD*>(pSoundIni);
        if (vtable != 0x7E1AF4u) {
            Log::Warn("inject apply @0x7510F6: SOUNDMD 对象 vtable=%08X（期望 007E1AF4），跳过",
                      static_cast<unsigned int>(vtable));
            return;
        }

        const int before = CountSections(pSoundIni);
        IniOverlay::Copy(pSoundIni, s_sound_overlay, false, true);
        Log::Info("inject apply @0x7510F6: SOUNDMD 覆盖已应用到 %p（段数 %d->%d）",
                  static_cast<void*>(pSoundIni), before, CountSections(pSoundIni));
    }

}  // namespace RulesInject

DEFINE_HOOK(0x679A1B, RA2Hook_RulesInject_PostAresPhobos, 0x5)
{
    GET(CCINIClass*, pINI, ESI);
    RulesInject::Apply(pINI);
    return 0;
}

// RulesClass::Read_File 在 0x668EF0 调用 LoadTypesFromINI。0x668EF5 是返回后
// 的第一条完整指令（push "Easy"），ESI 仍为 rules CCINIClass*。当前实机的
// Ares/Phobos .syhks00 表均未占用 0x668EF5，也未在 0x668ED0..0x668F20 起钩。
DEFINE_HOOK(0x668EF5, RA2Hook_RulesInject_PostLoadTypesDiagnostic, 0x5)
{
    RulesInject::ReportTypeDiagnostics();
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

// 原始指令：mov eax,dword ptr [88730Ch]。此时尚未打开 SOUNDMD.INI，适合
// 完成所有可能重入 INI 解析器的预备工作。
DEFINE_HOOK(0x52C6C4, RA2Hook_SoundOverlay_Prepare, 0x5)
{
    RulesInject::PrepareSound();
    return 0;
}

// 原始指令：mov dword ptr [B1D3A4h],eax；0x7510F4 已设置 ECX=EDI，EDI 是
// SOUNDMD CCINIClass。该点不覆盖 relative call、ESP-relative 指令或栈调整。
DEFINE_HOOK(0x7510F6, RA2Hook_SoundOverlay_Apply, 0x5)
{
    GET(CCINIClass*, pSoundIni, ECX);
    RulesInject::ApplyPreparedSound(pSoundIni);
    return 0;
}
