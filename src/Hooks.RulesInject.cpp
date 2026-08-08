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
//
// 【本轮实现】把外部 ini 逐键并进 pINI，语义"后写胜出"：
//   来源（二选一）：
//     1. [Inject] Files= 显式列表（逗号分隔，顺序即注入顺序，后者覆盖前者）
//     2. 未配置则扫描 ra2hook/inject/*.ini（按文件名排序，确定性顺序）
//   读源文件与写目标都直接用 INIClass 链表遍历，绕开引擎 CurrentSection
//   回退陷阱（详见 ArtMap.cpp 顶部注释——GetKeyCount/ReadString 对不存在的
//   段会回退到上一次成功段，拿它做存在性检查是无效的）。
//   写入用 INIClass::WriteString（0x528660）：键存在则覆盖，不存在则新建，
//   与引擎自身写入方式一致，下游读取者无法区分。

#include <Syringe.h>
#include <Helpers/Macro.h>
#include <CCINIClass.h>
#include <CCFileClass.h>
#include <RulesClass.h>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "Config.h"
#include "Logger.h"

namespace RulesInject {

    static bool s_done = false;   // 幂等：读档 / 重开局可能重入

    static const char* kInjectDir = "ra2hook\\inject";
    static const int   kMaxFiles  = 128;   // 一个注入目录最多接受的文件数
    static const int   kPathMax   = 260;

    // 把一个外部 ini 文件逐键并进 pTarget。返回写入的键数；文件不存在返回 -1。
    static int MergeFile(CCINIClass* pTarget, const char* path)
    {
        CCFileClass file(path);
        if (!file.Exists()) {
            Log::Warn("inject: 文件不存在 %s，跳过", path);
            return -1;
        }

        CCINIClass src;
        src.ReadCCFile(&file);   // 0x4741F0 —— 与 Config.cpp 同款读法

        int keys = 0, sections = 0;
        for (auto* s = src.Sections.First(); s && s->IsValid(); s = s->Next()) {
            if (!s->Name || !s->Name[0]) continue;
            ++sections;
            for (auto* n = s->Entries.GenericList::First(); n && n->IsValid(); n = n->Next()) {
                auto* e = static_cast<INIClass::INIEntry*>(n);
                if (!e->Key || !e->Key[0]) continue;
                pTarget->WriteString(s->Name, e->Key, e->Value ? e->Value : "");
                ++keys;
            }
        }

        Log::Info("inject: 并进 %s（%d 段 %d 键）", path, sections, keys);
        return keys;
    }

    // 扫描注入目录里的 *.ini，按文件名排序，返回找到的数量。
    // 超上限的忽略并告警。返回 0 = 目录不存在或没有 ini。
    static int ScanInjectDir(char files[][kPathMax])
    {
        char pattern[kPathMax] = {};
        std::snprintf(pattern, sizeof(pattern), "%s\\*%s", kInjectDir, "*.ini");

        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE)
            return 0;

        int n = 0;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (n < kMaxFiles)
                std::snprintf(files[n], kPathMax, "%s\\%s", kInjectDir, fd.cFileName);
            ++n;
        } while (FindNextFileA(h, &fd));
        FindClose(h);

        if (n > kMaxFiles)
            Log::Warn("inject: 目录里有 %d 个 ini，超过上限 %d，多余的被忽略", n, kMaxFiles);

        // 确定性顺序：按文件名（路径）升序。表示顺序即注入顺序，越靠后越优先。
        std::qsort(files, (n < kMaxFiles ? n : kMaxFiles), kPathMax,
                   [](const void* a, const void* b) {
                       return _stricmp(static_cast<const char*>(a),
                                       static_cast<const char*>(b));
                   });

        return n < kMaxFiles ? n : kMaxFiles;
    }

    // 把 Files= 列表里的每一项都注入。列表项以逗号/空格/制表符分隔。
    static void InjectList(CCINIClass* pINI, const char* list, int& total, int& missing)
    {
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
            const int k = MergeFile(pINI, path);
            if (k < 0) ++missing; else total += k;
        }
    }

    // 合并全部来源：显式 Files= 列表优先；否则扫注入目录。
    static void MergeAll(CCINIClass* pINI)
    {
        const auto& cfg = Config::Get().inject;

        int total = 0, missing = 0;

        if (cfg.files[0]) {
            InjectList(pINI, cfg.files, total, missing);
        } else {
            char found[kMaxFiles][kPathMax];
            const int n = ScanInjectDir(found);
            if (n == 0) {
                Log::Info("inject: 目录 %s 下没有可注入的 ini（或目录不存在），本轮空操作",
                          kInjectDir);
                return;
            }
            for (int i = 0; i < n; ++i) {
                const int k = MergeFile(pINI, found[i]);
                if (k < 0) ++missing; else total += k;
            }
        }

        Log::Info("inject: 完成，共 %d 键，%d 个文件缺失", total, missing);
    }

    static void Apply(CCINIClass* pINI)
    {
        if (s_done) return;
        s_done = true;

        Config::Load();
        if (!Config::Get().inject.enabled) {
            Log::Debug("inject: 已跳过（[Inject] Enabled 未开启）");
            return;
        }

        Log::Info("inject @0x679A15: pINI=%p [General]=%d Keys",
                  (void*)pINI, pINI->GetKeyCount("General"));

        MergeAll(pINI);
    }

}  // namespace RulesInject

DEFINE_HOOK(0x679A15, RA2Hook_RulesInject, 0x6)
{
    GET_STACK(CCINIClass*, pINI, 0x4);
    RulesInject::Apply(pINI);
    return 0;
}