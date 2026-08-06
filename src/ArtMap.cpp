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

        // 段是否存在。这个检查是必须的，不是防御性冗余：
        //
        // 引擎的 INI 读取内部缓存了「当前段」，读一个**不存在的段**时会回退到
        // 上一次成功读取的段（Phobos 的 ReadString 包装里那个 useCurrentSection
        // 参数即为此机制）。实测后果：2687 个无 art 段的单位全部读到了同一个
        // CameoPCX=mcvicon.pcx（该值在 artmd.ini 里仅出现 3 次），于是每个单位
        // 目录里都被写进同一个 mcvicon.pcx。
        bool SectionExists(CCINIClass* pINI, const char* section)
        {
            return section && section[0] && pINI->GetKeyCount(section) > 0;
        }

        bool IsTrue(CCINIClass* pINI, const char* section, const char* key)
        {
            if (!SectionExists(pINI, section)) return false;
            return pINI->ReadBool(section, key, false);
        }

        // 读一个字符串键；段或键不存在返回 false。
        // 段存在性检查不可省略，原因见 SectionExists。
        bool ReadKey(CCINIClass* pINI, const char* section, const char* key,
                     char* out, int outSize)
        {
            out[0] = '\0';
            if (!SectionExists(pINI, section))
                return false;
            pINI->ReadString(section, key, "", out, static_cast<size_t>(outSize));
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
            if (!ReadKey(pRules, unitId, "Image", artName, sizeof(artName)))
                std::snprintf(artName, sizeof(artName), "%s", unitId);

            // 2) art 段里可能再有一层 Image= 重定向
            char image[128] = {};
            if (ReadKey(pArt, artName, "Image", image, sizeof(image)))
                std::snprintf(artName, sizeof(artName), "%s", image);

            // art 段不存在就直接退出：这类单位（实测 2687 个中的大多数）没有
            // 自己的 art 定义，继续往下读任何键都会因引擎的段回退机制拿到
            // 上一个单位的值。它们的素材仍可能以 ID 命名存在于 mix 中，
            // 故仍按 ID 试探主体文件，但不再读任何 art 键。
            const bool hasArt = SectionExists(pArt, artName);

            ++st.units;
            if (!hasArt)
                ++st.noArtSection;

            // 归类目录用单位 ID（而非 art 名）——便于按注册名查找
            const char* ownerDir = sortByOwner ? unitId : nullptr;

            const bool isVoxel   = IsTrue(pArt, artName, "Voxel");
            const bool newTheater = IsTrue(pArt, artName, "NewTheater");
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
            }

            // 图标：CameoPCX / AltCameoPCX。这两个键读取前已由 ReadKey 做段检查。
            if (cfg.dump.shp) {
                char cameo[128] = {};
                if (ReadKey(pArt, artName, "CameoPCX", cameo, sizeof(cameo)))
                    TryDump(cameo, "shp", ownerDir, st);
                if (ReadKey(pArt, artName, "AltCameoPCX", cameo, sizeof(cameo)))
                    TryDump(cameo, "shp", ownerDir, st);
            }
        }

        // 遍历 rules 里一个类型列表段（如 [VehicleTypes]）
        void ProcessTypeList(CCINIClass* pRules, CCINIClass* pArt,
                             const char* listSection, bool sortByOwner, Stats& st)
        {
            const int count = pRules->GetKeyCount(listSection);
            if (count <= 0) {
                Log::Info("  [%s] 无条目，跳过", listSection);
                return;
            }

            const Stats before = st;
            for (int i = 0; i < count; ++i) {
                const char* key = pRules->GetKeyName(listSection, i);
                if (!key) continue;

                char id[128] = {};
                pRules->ReadString(listSection, key, "", id, sizeof(id));
                if (id[0] == '\0') continue;

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
