// ArtMap.cpp — 单位 → 素材映射与 VXL/SHP 导出。
//
// 设计依据全部来自对真实 dump 的核对，见 ArtMap.h 顶部注释。
//
// 本轮范围（直接引用）：
//   主体 VXL + HVA、炮塔/炮管（文件名约定）、SHP 主体、图标 PCX/SHP
// 暂不含间接引用（ActiveAnim → [Animations] 段 → SHP），留待下一轮。

#include <CCINIClass.h>
#include <CCFileClass.h>

#include <cstdio>
#include <cstring>

#include "ArtMap.h"
#include "Config.h"
#include "DumpIO.h"
#include "Logger.h"

namespace ArtMap {

    namespace {

        struct Stats {
            int units        = 0;   // 扫过的单位数
            int noArtSection = 0;   // 其中没有 art 段的（只能按 ID 试探）
            int filesFound   = 0;   // 实际存在并导出的文件数
            int filesMiss    = 0;   // 试探未命中的候选数
            int bytes        = 0;
        };

        // NewTheater 的地图类型前缀。文件名第 2 个字母会被替换成这些。
        // 例："GAPOWR" 在雪地是 "GSPOWR"。实测 art 里 NewTheater=yes 有 2315 处。
        constexpr char kTheaterLetters[] = { 'A', 'T', 'U', 'S', 'L', 'D', 'N' };

        // SHP 主体未命中时的诊断计数（跑通后可连同诊断代码一并删除）
        constexpr int kShpDiagMax = 12;
        int s_shpDiag = 0;

        // ── 为什么这里不用引擎的 ReadString / GetKeyCount ──────────────────
        //
        // 引擎的 INI 读取内部缓存了「当前段」。读一个**不存在的段**时它不报错，
        // 而是回退到上一次成功读取的段（Phobos 的 ReadString 包装里那个
        // useCurrentSection 参数即为此机制）。
        //
        // 实测证据：[VehicleTypes] 的第 0 项是 AMCV，它有 art 段且
        // CameoPCX=mcvicon.pcx。其后 2627 个无 art 段的单位全部读到了这同一个
        // 值 —— 2628 个单位目录里 2627 个都只有 mcvicon.pcx。
        //
        // 关键：GetKeyCount 同样受此机制影响，对不存在的段会返回「当前段」的
        // 键数（>0），所以用它做段存在性检查是无效的 —— 检查恒为真。这正是
        // 上一版修复失败的原因。
        //
        // 因此下面全部改为**直接遍历 INIClass 的链表结构**，不经过任何引擎
        // 读取函数，也就不存在隐藏状态。Phobos 判段存在用的是 GetSection() 的
        // 返回指针，同理。
        //
        // 注意 GenericList 末尾是哨兵节点，终止条件必须用 IsValid()。

        INIClass::INISection* FindSection(CCINIClass* pINI, const char* section)
        {
            if (!pINI || !section || !section[0]) return nullptr;
            for (auto* s = pINI->Sections.First(); s && s->IsValid(); s = s->Next()) {
                if (s->Name && _stricmp(s->Name, section) == 0)
                    return s;
            }
            return nullptr;
        }

        // 在已定位的段里找键，返回其值（未找到返回 nullptr）
        const char* FindValue(INIClass::INISection* sec, const char* key)
        {
            if (!sec || !key) return nullptr;
            // Entries 声明为 List<INIEntry*>，First() 的返回类型对不上实际布局，
            // 故走 GenericList 层自行转型。
            for (auto* n = sec->Entries.GenericList::First(); n && n->IsValid(); n = n->Next()) {
                auto* e = static_cast<INIClass::INIEntry*>(n);
                if (e->Key && _stricmp(e->Key, key) == 0)
                    return e->Value;
            }
            return nullptr;
        }

        bool IsTrue(INIClass::INISection* sec, const char* key)
        {
            const char* v = FindValue(sec, key);
            if (!v || !v[0]) return false;
            return v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T' || v[0] == '1';
        }

        // 读一个字符串键；段或键不存在返回 false，且 out 保持为空串。
        bool ReadKey(INIClass::INISection* sec, const char* key,
                     char* out, int outSize)
        {
            out[0] = '\0';
            const char* v = FindValue(sec, key);
            if (!v || !v[0]) return false;
            std::snprintf(out, static_cast<size_t>(outSize), "%s", v);
            return out[0] != '\0';
        }

        // 去重：避免对同一 (单位, 文件) 组合重复 Exists()/写盘。
        //
        // 按单位归类时，同一文件被不同单位引用**应当**各写一份，所以 key 含
        // ownerDir。但这样 key 总数 ≈ 单位数 × 候选数，量级几万——上一版用
        // 4096 条的线性数组，既会中途静默失效，每次查找还要线性扫描。
        // 改为哈希集合：固定桶数、开放寻址，命中/插入都是常数时间。
        constexpr int kSeenBuckets = 1 << 16;      // 65536，足够容纳几万条
        unsigned int s_seenHash[kSeenBuckets] = {};
        int          s_seenCount = 0;

        unsigned int HashKey(const char* ownerDir, const char* fileName)
        {
            // FNV-1a，大小写无关（引擎文件名不区分大小写）
            unsigned int h = 2166136261u;
            for (const char* p = ownerDir ? ownerDir : ""; *p; ++p) {
                char c = (*p >= 'a' && *p <= 'z') ? static_cast<char>(*p - 32) : *p;
                h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
            }
            h = (h ^ '|') * 16777619u;
            for (const char* p = fileName; *p; ++p) {
                char c = (*p >= 'a' && *p <= 'z') ? static_cast<char>(*p - 32) : *p;
                h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
            }
            return h ? h : 1u;   // 0 用作空槽标记
        }

        bool AlreadySeen(const char* fileName, const char* ownerDir)
        {
            const unsigned int h = HashKey(ownerDir, fileName);
            unsigned int idx = h & (kSeenBuckets - 1);
            for (int probe = 0; probe < kSeenBuckets; ++probe) {
                if (s_seenHash[idx] == 0) {          // 空槽 -> 未见过，占用它
                    s_seenHash[idx] = h;
                    ++s_seenCount;
                    return false;
                }
                if (s_seenHash[idx] == h)            // 见过
                    return true;
                idx = (idx + 1) & (kSeenBuckets - 1);
            }
            return false;   // 表满（实际不会发生）
        }

        // 尝试导出一个候选文件。存在则写盘，返回字节数；不存在返回 0。
        //   ownerDir 为空表示平铺（不按单位归类）
        int TryDump(const char* fileName, const char* subDir,
                    const char* ownerDir, Stats& st)
        {
            if (AlreadySeen(fileName, ownerDir))
                return 0;

            CCFileClass file(fileName);
            if (!file.Exists()) {
                ++st.filesMiss;
                return 0;
            }

            char rel[320] = {};
            if (ownerDir && ownerDir[0])
                std::snprintf(rel, sizeof(rel), "%s/%s/%s", subDir, ownerDir, fileName);
            else
                std::snprintf(rel, sizeof(rel), "%s/%s", subDir, fileName);

            const int n = DumpIO::CopyEngineFile(fileName, rel);
            if (n > 0) {
                ++st.filesFound;
                st.bytes += n;
                Log::Debug("  dump %s -> %s (%d)", fileName, rel, n);
                return n;
            }
            return 0;
        }

        // 对一个 base 名尝试若干扩展名与 NewTheater 变体
        void TryDumpVariants(const char* baseName, const char* ext,
                             const char* subDir, const char* ownerDir,
                             bool newTheater, Stats& st)
        {
            char name[320] = {};

            std::snprintf(name, sizeof(name), "%s%s", baseName, ext);
            TryDump(name, subDir, ownerDir, st);

            if (!newTheater || std::strlen(baseName) < 2)
                return;

            // NewTheater：替换第 2 个字母，逐个地图类型试
            for (char letter : kTheaterLetters) {
                std::snprintf(name, sizeof(name), "%s%s", baseName, ext);
                name[1] = letter;
                TryDump(name, subDir, ownerDir, st);
            }
        }

        // 处理一个单位：解析它的 art 段，导出全部直接引用的素材
        void ProcessUnit(CCINIClass* pRules, CCINIClass* pArt,
                         const char* unitId, bool sortByOwner, Stats& st)
        {
            // 1) rules 里的 Image= 决定 art 段名；缺省用 ID 本身
            char artName[128] = {};
            auto* rulesSec = FindSection(pRules, unitId);
            if (!ReadKey(rulesSec, "Image", artName, sizeof(artName)))
                std::snprintf(artName, sizeof(artName), "%s", unitId);

            // 2) art 段里可能再有一层 Image= 重定向
            auto* artSec = FindSection(pArt, artName);
            char image[128] = {};
            if (ReadKey(artSec, "Image", image, sizeof(image))) {
                std::snprintf(artName, sizeof(artName), "%s", image);
                artSec = FindSection(pArt, artName);   // 重定向后重新定位
            }

            // 无 art 段的单位：其素材仍可能以 ID 命名存在于 mix 中，
            // 故仍按 ID 试探主体文件，但不读任何 art 键（也无从可读）。
            const bool hasArt = (artSec != nullptr);

            ++st.units;
            if (!hasArt)
                ++st.noArtSection;

            // 归类目录用单位 ID（而非 art 名）——便于按注册名查找
            const char* ownerDir = sortByOwner ? unitId : nullptr;

            const bool isVoxel    = IsTrue(artSec, "Voxel");
            const bool newTheater = IsTrue(artSec, "NewTheater");
            const auto& cfg = Config::Get();

            if (isVoxel && cfg.dump.vxl) {
                // VXL 不做 NewTheater 变体试探：实测 Voxel=yes 有 952 段、
                // NewTheater=yes 有 2243 段，但两者同时出现的只有 2 段。
                // 对 VXL 试 7 个地图变体等于上万次无用的文件查找。
                TryDumpVariants(artName, ".VXL", "vxl", ownerDir, false, st);
                TryDumpVariants(artName, ".HVA", "vxl", ownerDir, false, st);

                // 炮塔/炮管：文件名约定派生（实测 art 段里没有 TurretAnim/BarrelAnim 键）
                char sub[160] = {};
                static const char* kSuffixes[] = { "TUR", "BARL", "WTUR", "WBARL" };
                for (const char* sfx : kSuffixes) {
                    std::snprintf(sub, sizeof(sub), "%s%s", artName, sfx);
                    TryDumpVariants(sub, ".VXL", "vxl", ownerDir, false, st);
                    TryDumpVariants(sub, ".HVA", "vxl", ownerDir, false, st);
                }
            }

            // 无 art 段时 isVoxel 必为 false，不能因此就当作 SHP 单位：
            // 上一版正是在这里让 2687 个无 art 段的单位全部走进 SHP 分支，
            // 又通过段回退读到同一个 CameoPCX，于是每个目录里都是 mcvicon.pcx。
            // 现在：无 art 段者只按 ID 试探两种主体文件，不读任何 art 键。
            if (!hasArt) {
                if (cfg.dump.vxl) {
                    TryDumpVariants(artName, ".VXL", "vxl", ownerDir, false, st);
                    TryDumpVariants(artName, ".HVA", "vxl", ownerDir, false, st);
                }
                if (cfg.dump.shp)
                    TryDumpVariants(artName, ".SHP", "shp", ownerDir, false, st);
                return;
            }

            if (!isVoxel && cfg.dump.shp) {
                // SHP 才需要 NewTheater 变体（建筑居多）
                TryDumpVariants(artName, ".SHP", "shp", ownerDir, newTheater, st);

                // 诊断：只有图标被导出、主体 SHP 全部未命中时，需要知道究竟试了
                // 什么名字。对前若干个非 Voxel 单位打印实测结果（Info 级，便于
                // 用户直接贴日志）。跑通后可删。
                if (s_shpDiag < kShpDiagMax) {
                    ++s_shpDiag;
                    char n1[160] = {}, n2[160] = {};
                    std::snprintf(n1, sizeof(n1), "%s.SHP", artName);
                    std::snprintf(n2, sizeof(n2), "%s.shp", artName);
                    CCFileClass f1(n1), f2(n2);
                    Log::Info("  [shp诊断] %s -> art=%s NewTheater=%d "
                              "%s:%d %s:%d",
                              unitId, artName, newTheater ? 1 : 0,
                              n1, f1.Exists() ? 1 : 0,
                              n2, f2.Exists() ? 1 : 0);
                }
            }

            // 图标：CameoPCX / AltCameoPCX（仅当 art 段存在时才有意义）
            //
            // 图标与主体同目录：VXL 单位的图标跟去 vxl/<ID>/，SHP 单位留在
            // shp/<ID>/，这样一个单位的全部素材集中在一处。
            const char* cameoDir  = isVoxel ? "vxl" : "shp";
            const bool  wantCameo = isVoxel ? cfg.dump.vxl : cfg.dump.shp;
            if (wantCameo) {
                char cameo[128] = {};
                if (ReadKey(artSec, "CameoPCX", cameo, sizeof(cameo)))
                    TryDump(cameo, cameoDir, ownerDir, st);
                if (ReadKey(artSec, "AltCameoPCX", cameo, sizeof(cameo)))
                    TryDump(cameo, cameoDir, ownerDir, st);

                // SHP 形式的图标（部分 mod 用 Cameo= 指向 SHP 而非 PCX）
                if (ReadKey(artSec, "Cameo", cameo, sizeof(cameo)))
                    TryDumpVariants(cameo, ".SHP", cameoDir, ownerDir, false, st);
                if (ReadKey(artSec, "AltCameo", cameo, sizeof(cameo)))
                    TryDumpVariants(cameo, ".SHP", cameoDir, ownerDir, false, st);
            }
        }

        // OnlyUnits 过滤：配置里给了逗号分隔的 ID 列表时，只处理这些单位。
        // 留空表示不过滤（全部导出）。比较不区分大小写。
        bool UnitAllowed(const char* unitId)
        {
            const char* list = Config::Get().dump.onlyUnits;
            if (!list || !list[0]) return true;      // 未配置 -> 全部

            const size_t idLen = std::strlen(unitId);
            const char* p = list;
            while (*p) {
                while (*p == ',' || *p == ' ' || *p == '\t') ++p;
                const char* start = p;
                while (*p && *p != ',') ++p;
                const char* end = p;
                while (end > start && (end[-1] == ' ' || end[-1] == '\t')) --end;

                const size_t len = static_cast<size_t>(end - start);
                if (len == idLen && _strnicmp(start, unitId, len) == 0)
                    return true;
            }
            return false;
        }

        // 遍历 rules 里一个类型列表段（如 [VehicleTypes]）。
        // 同样直接遍历链表：GetKeyCount/GetKeyName/ReadString 都受段回退影响。
        void ProcessTypeList(CCINIClass* pRules, CCINIClass* pArt,
                             const char* listSection, bool sortByOwner, Stats& st)
        {
            auto* listSec = FindSection(pRules, listSection);
            if (!listSec) {
                Log::Info("  [%s] 段不存在，跳过", listSection);
                return;
            }

            const Stats before = st;
            int count = 0;

            // 列表段的形式是 0=AMCV / 1=AHMV / ...，值即单位 ID
            for (auto* n = listSec->Entries.GenericList::First(); n && n->IsValid(); n = n->Next()) {
                auto* e = static_cast<INIClass::INIEntry*>(n);
                if (!e->Value || !e->Value[0]) continue;
                ++count;

                char id[128] = {};
                std::snprintf(id, sizeof(id), "%s", e->Value);
                if (!UnitAllowed(id)) continue;
                ProcessUnit(pRules, pArt, id, sortByOwner, st);
            }

            Log::Info("  [%-14s] %4d 项 -> 导出 %d 个文件",
                      listSection, count, st.filesFound - before.filesFound);
        }

    }  // namespace

    void Run()
    {
        const auto& cfg = Config::Get();
        if (!cfg.dump.vxl && !cfg.dump.shp) {
            Log::Info(" [VXL/SHP] 两项均未开启，跳过");
            return;
        }

        CCINIClass* pRules = CCINIClass::INI_Rules;
        CCINIClass* pArt   = &CCINIClass::INI_Art;
        if (!pRules || !pArt) {
            Log::Warn(" [VXL/SHP] rules/art INI 不可用，跳过");
            return;
        }

        Log::Info(" [VXL/SHP] 开始（vxl=%d shp=%d 按单位归类=%d）",
                  cfg.dump.vxl ? 1 : 0, cfg.dump.shp ? 1 : 0,
                  cfg.dump.sortByOwner ? 1 : 0);

        Stats st;
        static const char* kLists[] = {
            "VehicleTypes", "InfantryTypes", "AircraftTypes", "BuildingTypes",
        };
        for (const char* list : kLists)
            ProcessTypeList(pRules, pArt, list, cfg.dump.sortByOwner, st);

        Log::Info(" [VXL/SHP] 完成：%d 个单位（其中 %d 个无 art 段），"
                  "导出 %d 个文件（%d 字节），未命中候选 %d",
                  st.units, st.noArtSection, st.filesFound, st.bytes, st.filesMiss);
    }

}  // namespace ArtMap
