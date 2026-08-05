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
#include <string.h>   // _stricmp

#include "ArtMap.h"
#include "Config.h"
#include "DumpIO.h"
#include "Logger.h"

namespace ArtMap {

    namespace {

        struct Stats {
            int units      = 0;   // 扫过的单位数
            int filesFound = 0;   // 实际存在并导出的文件数
            int filesMiss  = 0;   // 试探未命中的候选数
            int bytes      = 0;
        };

        // NewTheater 的地图类型前缀。文件名第 2 个字母会被替换成这些。
        // 例："GAPOWR" 在雪地是 "GSPOWR"。实测 art 里 NewTheater=yes 有 2315 处。
        constexpr char kTheaterLetters[] = { 'A', 'T', 'U', 'S', 'L', 'D', 'N' };

        bool IsTrue(CCINIClass* pINI, const char* section, const char* key)
        {
            return pINI->ReadBool(section, key, false);
        }

        // 读一个字符串键；不存在返回 false
        bool ReadKey(CCINIClass* pINI, const char* section, const char* key,
                     char* out, int outSize)
        {
            out[0] = '\0';
            pINI->ReadString(section, key, "", out, static_cast<size_t>(outSize));
            return out[0] != '\0';
        }

        // 去掉扩展名（"giicon.pcx" -> "giicon"）
        void StripExt(char* s)
        {
            char* dot = std::strrchr(s, '.');
            if (dot) *dot = '\0';
        }

        // 已试探过的候选文件名（含未命中），避免重复的 Exists() 与重复写盘。
        // 多个单位常共用同一个 Image=，不去重会做大量无用功。
        // 注意：按单位归类时，同一文件被不同单位引用**应当**各写一份，
        // 故 key 里带上 ownerDir。
        constexpr int kSeenCap = 4096;
        char  s_seen[kSeenCap][96];
        int   s_seenCount = 0;

        bool AlreadySeen(const char* fileName, const char* ownerDir)
        {
            char key[96] = {};
            std::snprintf(key, sizeof(key), "%s|%s", ownerDir ? ownerDir : "", fileName);
            for (int i = 0; i < s_seenCount; ++i)
                if (_stricmp(s_seen[i], key) == 0)
                    return true;
            if (s_seenCount < kSeenCap) {
                std::snprintf(s_seen[s_seenCount], sizeof(s_seen[0]), "%s", key);
                ++s_seenCount;
            }
            return false;
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

            if (pArt->GetKeyCount(artName) <= 0 && pRules->GetKeyCount(unitId) <= 0)
                return;

            ++st.units;

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

            if (!isVoxel && cfg.dump.shp) {
                // SHP 才需要 NewTheater 变体（建筑居多）
                TryDumpVariants(artName, ".SHP", "shp", ownerDir, newTheater, st);
            }

            // 图标：CameoPCX / AltCameoPCX 是显式键，另有 SHP 形式的 Cameo
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

        Log::Info(" [VXL/SHP] 完成：%d 个单位，导出 %d 个文件（%d 字节），未命中候选 %d",
                  st.units, st.filesFound, st.bytes, st.filesMiss);
    }

}  // namespace ArtMap
