// Config.cpp — 用引擎自己的 CCINIClass 解析我们的配置文件。
//
// 好处：不必自带 INI 解析器（无异常 + 静态 CRT 环境下能省不少事），
// 且行为与游戏一致。CCFileClass 会透明处理路径。

#include <CCINIClass.h>
#include <CCFileClass.h>

#include <cstring>   // _stricmp

#include "Config.h"
#include "Logger.h"

namespace Config {

    static Settings s_settings;
    static bool     s_loaded = false;

    static const char* kFileName = "ra2hook.ini";

    // 直接遍历链表的段查找。引擎的 ReadString/GetKeyCount 对**不存在的段**会
    // 回退到上一次成功段（CurrentSection），拿它们做存在性检查恒为真；这里与
    // ArtMap.cpp 保持一致，走链表避免隐藏状态。
    static INIClass::INISection* FindSection(INIClass* pINI, const char* section)
    {
        if (!pINI || !section || !section[0]) return nullptr;
        for (auto* s = pINI->Sections.First(); s && s->IsValid(); s = s->Next()) {
            if (s->Name && _stricmp(s->Name, section) == 0)
                return s;
        }
        return nullptr;
    }

    void Load()
    {
        if (s_loaded) return;
        s_loaded = true;

        CCFileClass file(kFileName);
        if (!file.Exists()) {
            Log::Info("Config: %s 不存在，全部功能保持默认（dump/inject 关闭）", kFileName);
            return;
        }

        // 用引擎的 INI 容器解析我们自己的配置文件
        CCINIClass ini;
        ini.ReadCCFile(&file);

        auto& d = s_settings.dump;
        d.enabled     = ini.ReadBool("Dump", "Enabled",     d.enabled);
        d.ini         = ini.ReadBool("Dump", "INI",         d.ini);
        d.csf         = ini.ReadBool("Dump", "CSF",         d.csf);
        d.vxl         = ini.ReadBool("Dump", "VXL",         d.vxl);
        d.shp         = ini.ReadBool("Dump", "SHP",         d.shp);
        d.sortByOwner   = ini.ReadBool("Dump", "SortByOwner",   d.sortByOwner);
        d.stripInclude  = ini.ReadBool("Dump", "StripInclude",  d.stripInclude);

        // OnlyUnits：逗号分隔的单位 ID 白名单，留空则全部导出。
        // 这里用 ReadString 是安全的——[Dump] 段确实存在（文件存在才走到这），
        // 段回退机制只在读**不存在的段**时才误导。
        ini.ReadString("Dump", "OnlyUnits", "", d.onlyUnits, sizeof(d.onlyUnits));

        // [Inject]：段可能不存在（默认 ra2hook.ini 里就没有）。若不检查段存在性，
        // ReadBool/ReadString 会回退到 [Dump] 段，把 OnlyUnits 之类的值误读进来——
        // Enabled 可能被错误地打开。故先查段，存在才读。
        s_settings.inject.enabled = false;
        s_settings.inject.files[0] = '\0';
        s_settings.inject.mix     = false;
        if (FindSection(&ini, "Inject")) {
            s_settings.inject.enabled = ini.ReadBool("Inject", "Enabled", false);
            ini.ReadString("Inject", "Files", "", s_settings.inject.files,
                           sizeof(s_settings.inject.files));
            s_settings.inject.mix = ini.ReadBool("Inject", "Mix", false);
        } else {
            Log::Debug("Config: [Inject] 段不存在，inject 保持关闭");
        }

        s_settings.logLevel       = ini.ReadInteger("Log", "Level", s_settings.logLevel);

        Log::g_level = static_cast<Log::Level>(s_settings.logLevel);

        Log::Info("Config: dump=%d (ini=%d csf=%d vxl=%d shp=%d sort=%d) inject=%d mix=%d",
                  d.enabled ? 1 : 0, d.ini ? 1 : 0, d.csf ? 1 : 0,
                  d.vxl ? 1 : 0, d.shp ? 1 : 0, d.sortByOwner ? 1 : 0,
                  s_settings.inject.enabled ? 1 : 0,
                  s_settings.inject.mix ? 1 : 0);

        if (d.onlyUnits[0])
            Log::Info("Config: OnlyUnits=[%s]（仅导出这些单位）", d.onlyUnits);
    }

    const Settings& Get()
    {
        return s_settings;
    }

}  // namespace Config
