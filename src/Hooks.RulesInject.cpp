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
// 【本轮实现】多目标注入：
//   每个引擎 INI 对象（rules/ra2md/art/ai/uimd）对应一个注入目录：
//     ra2hook/inject/enabled/<target>/*.ini
//   目标表见 kTargets。其中：
//     - rules  挂点 0x679A15，已核实；目录内 ini 全部并进 INI_Rules
//     - ra2md  与 rules 同一窗口（引擎先读 ra2md.ini 再读 rulesmd.ini）——
//               若此对象此时已装载（段数>0 才写，见 InjectTarget 守卫）则并进
//     - art/ai/uimd 挂点未定（对象在此时段可能尚未装载，写进去是无效的），
//               现阶段标记为「待挂点」，只记日志不注入（见 TODO.md）
//   显式 Files= 列表（旧版行为）仍保留：全部并进 rules。
//   inject 文件内的 [#include] 由 ra2hook 自己展开，独立于 Ares/Phobos：
//     - 不修改原 rules/art 的 [#include] 段
//     - 不把 inject 文件自己的 [#include] 段写入引擎目标对象
//     - 先合并当前文件，再按 include 键顺序深度优先合并被引用文件
//
// 【mix 装载】ra2hook/inject/mix/*.mix 全部 new MixFileClass 注册进引擎
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
#include "Logger.h"

namespace RulesInject {

    static bool s_done = false;   // 幂等：读档 / 重开局可能重入

    static const char* kInjectRoot = "ra2hook\\inject";
    static const char* kMixDir     = "ra2hook\\inject\\mix";
    static const int   kMaxFiles   = 256;   // 一个注入目录最多接受的文件数
    static const int   kPathMax    = 260;
    static const int   kMaxIncludeDepth = 32;

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
        { "art",   "artmd.ini",   []{ return &CCINIClass::INI_Art; },   false },
        { "ai",    "aimd.ini",    []{ return &CCINIClass::INI_AI; },    false },
        { "uimd",  "uimd.ini",    []{ return &CCINIClass::INI_UIMD; },  false },
    };
    static const int kTargetNum =
        static_cast<int>(sizeof(kTargets) / sizeof(kTargets[0]));

    // 遍历链表数段落，绕开引擎 CurrentSection 回退陷阱（见 ArtMap.cpp）
    static int CountSections(INIClass* pINI)
    {
        if (!pINI) return -1;
        int n = 0;
        for (auto* s = pINI->Sections.First(); s && s->IsValid(); s = s->Next())
            ++n;
        return n;
    }

    static bool IsIncludeSection(const char* name)
    {
        return name && _stricmp(name, "#include") == 0;
    }

    static bool IsRootedPath(const char* path)
    {
        if (!path || !path[0]) return false;
        return path[0] == '\\' || path[0] == '/' || (path[0] && path[1] == ':');
    }

    static void NormalizeSlashes(char* path)
    {
        if (!path) return;
        for (char* p = path; *p; ++p)
            if (*p == '/') *p = '\\';
    }

    static void TrimIniValue(const char* src, char* dst, size_t dstSize)
    {
        if (!dst || dstSize == 0) return;
        dst[0] = '\0';
        if (!src) return;

        while (*src == ' ' || *src == '\t') ++src;
        const char* end = src + std::strlen(src);
        while (end > src && (end[-1] == ' ' || end[-1] == '\t' ||
                             end[-1] == '\r' || end[-1] == '\n'))
            --end;

        if (end > src + 1 &&
            ((*src == '"' && end[-1] == '"') || (*src == '\'' && end[-1] == '\''))) {
            ++src;
            --end;
        }

        const size_t len = static_cast<size_t>(end - src);
        std::snprintf(dst, dstSize, "%.*s",
                      static_cast<int>(len < dstSize ? len : dstSize - 1), src);
        NormalizeSlashes(dst);
    }

    static void ParentDirOf(const char* path, char* out, size_t outSize)
    {
        if (!out || outSize == 0) return;
        out[0] = '\0';
        if (!path || !path[0]) return;

        const char* last = nullptr;
        for (const char* p = path; *p; ++p) {
            if (*p == '\\' || *p == '/')
                last = p;
        }
        if (!last) return;

        const size_t len = static_cast<size_t>(last - path);
        std::snprintf(out, outSize, "%.*s",
                      static_cast<int>(len < outSize ? len : outSize - 1), path);
        NormalizeSlashes(out);
    }

    struct IncludeContext {
        char stack[kMaxIncludeDepth][kPathMax] = {};
        int  depth = 0;
    };

    static bool IsInIncludeStack(const IncludeContext& ctx, const char* path)
    {
        for (int i = 0; i < ctx.depth; ++i) {
            if (_stricmp(ctx.stack[i], path) == 0)
                return true;
        }
        return false;
    }

    static bool ReadIniResolved(const char* request, const char* baseDir,
                                CCINIClass& out, char* usedPath)
    {
        char name[kPathMax] = {};
        TrimIniValue(request, name, sizeof(name));
        if (!name[0]) return false;

        char candidates[2][kPathMax] = {};
        int n = 0;

        if (baseDir && baseDir[0] && !IsRootedPath(name)) {
            std::snprintf(candidates[n++], kPathMax, "%s\\%s", baseDir, name);
            NormalizeSlashes(candidates[n - 1]);
        }

        std::snprintf(candidates[n++], kPathMax, "%s", name);
        NormalizeSlashes(candidates[n - 1]);

        for (int i = 0; i < n; ++i) {
            CCFileClass file(candidates[i]);
            if (!file.Exists())
                continue;

            out.ReadCCFile(&file);   // 0x4741F0 —— 与 Config.cpp 同款读法
            std::snprintf(usedPath, kPathMax, "%s", candidates[i]);
            return true;
        }

        return false;
    }

    static int MergeFileRecursive(CCINIClass* pTarget, const char* path,
                                  const char* baseDir, IncludeContext& ctx);

    static int MergeIncludes(CCINIClass* pTarget, CCINIClass& src,
                             const char* currentPath, IncludeContext& ctx)
    {
        char baseDir[kPathMax] = {};
        ParentDirOf(currentPath, baseDir, sizeof(baseDir));

        int total = 0;
        int includes = 0;
        for (auto* s = src.Sections.First(); s && s->IsValid(); s = s->Next()) {
            if (!IsIncludeSection(s->Name)) continue;

            for (auto* n = s->Entries.GenericList::First(); n && n->IsValid(); n = n->Next()) {
                auto* e = static_cast<INIClass::INIEntry*>(n);
                if (!e->Key || !e->Key[0] || !e->Value || !e->Value[0]) continue;
                ++includes;
                const int keys = MergeFileRecursive(pTarget, e->Value, baseDir, ctx);
                if (keys > 0)
                    total += keys;
            }
        }

        if (includes > 0)
            Log::Info("inject: %s 展开 [#include] %d 项，共并进 %d 键",
                      currentPath, includes, total);
        return total;
    }

    static int MergeBody(CCINIClass* pTarget, CCINIClass& src,
                         const char* path, int& sectionCount)
    {
        int keys = 0;
        sectionCount = 0;
        for (auto* s = src.Sections.First(); s && s->IsValid(); s = s->Next()) {
            if (!s->Name || !s->Name[0] || IsIncludeSection(s->Name)) continue;
            ++sectionCount;
            for (auto* n = s->Entries.GenericList::First(); n && n->IsValid(); n = n->Next()) {
                auto* e = static_cast<INIClass::INIEntry*>(n);
                if (!e->Key || !e->Key[0]) continue;
                pTarget->WriteString(s->Name, e->Key, e->Value ? e->Value : "");
                ++keys;
            }
        }

        Log::Info("inject: 并进 %s（%d 段 %d 键）", path, sectionCount, keys);
        return keys;
    }

    // 把外部 ini 文件逐键并进 pTarget，并展开该文件自己的 [#include]。
    // 返回写入键数；文件不存在返回 -1。
    static int MergeFileRecursive(CCINIClass* pTarget, const char* path,
                                  const char* baseDir, IncludeContext& ctx)
    {
        if (!pTarget || !path || !path[0]) return -1;
        if (ctx.depth >= kMaxIncludeDepth) {
            Log::Warn("inject: [#include] 深度超过 %d，跳过 %s",
                      kMaxIncludeDepth, path);
            return -1;
        }

        CCINIClass src;
        char usedPath[kPathMax] = {};
        if (!ReadIniResolved(path, baseDir, src, usedPath)) {
            Log::Warn("inject: 文件不存在 %s，跳过", path);
            return -1;
        }

        if (IsInIncludeStack(ctx, usedPath)) {
            Log::Warn("inject: 检测到 [#include] 循环引用 %s，跳过", usedPath);
            return -1;
        }

        std::snprintf(ctx.stack[ctx.depth++], kPathMax, "%s", usedPath);

        int sections = 0;
        int keys = MergeBody(pTarget, src, usedPath, sections);
        const int includeKeys = MergeIncludes(pTarget, src, usedPath, ctx);

        --ctx.depth;
        return keys + (includeKeys > 0 ? includeKeys : 0);
    }

    static int MergeFile(CCINIClass* pTarget, const char* path)
    {
        IncludeContext ctx;
        return MergeFileRecursive(pTarget, path, nullptr, ctx);
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
        char pattern[kPathMax] = {};
        std::snprintf(pattern, sizeof(pattern), "%s\\%s", dir, wildcard);

        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;

        int n = 0;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (n < kMaxFiles)
                std::snprintf(files[n], kPathMax, "%s\\%s", dir, fd.cFileName);
            ++n;
        } while (FindNextFileA(h, &fd));
        FindClose(h);

        if (n > kMaxFiles)
            Log::Warn("inject: 目录里有 %d 个文件，超过上限 %d，多余的忽略",
                      n, kMaxFiles);

        std::qsort(files, (n < kMaxFiles ? n : kMaxFiles), kPathMax,
                   [](const void* a, const void* b) {
                       return _stricmp(static_cast<const char*>(a),
                                       static_cast<const char*>(b));
                   });
        return n < kMaxFiles ? n : kMaxFiles;
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
    // mapped=false（art/ai/uimd）的目标：挂点未定，只在目录内容存在时提示，
    // 不注入——注入到未完成装载的对象上后，引擎后续重建可能把键清掉。
    static int InjectTarget(const Target& t)
    {
        char dir[kPathMax] = {};
        std::snprintf(dir, sizeof(dir), "%s\\enabled\\%s", kInjectRoot, t.dir);

        char found[kMaxFiles][kPathMax] = {};
        const int n = ScanDir(dir, "*.ini", found);
        if (n == 0) {
            Log::Info("inject: 目录 %s 无 ini（空），跳过", dir);
            return 0;
        }

        if (!t.mapped) {
            Log::Info("inject: %s 目录 %s 有 %d 个 ini，但挂点未定（TODO），本次忽略",
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
    static void InjectMix()
    {
        char found[kMaxFiles][kPathMax] = {};
        const int n = ScanDir(kMixDir, "*.mix", found);
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

        if (Config::Get().inject.mix)
            InjectMix();

        // 显式 Files= 列表（旧版语义）优先，否则按目标目录注入。
        if (Config::Get().inject.files[0]) {
            InjectExplicit(CCINIClass::INI_Rules);
        } else {
            int total = 0;
            for (int i = 0; i < kTargetNum; ++i)
                total += InjectTarget(kTargets[i]);
            Log::Info("inject: 目标目录注入完成，共 %d 键", total);
        }
    }

}  // namespace RulesInject

DEFINE_HOOK(0x679A15, RA2Hook_RulesInject, 0x6)
{
    GET_STACK(CCINIClass*, pINI, 0x4);
    RulesInject::Apply(pINI);
    return 0;
}
