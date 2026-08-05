// Config.cpp — 用引擎自己的 CCINIClass 解析我们的配置文件。
//
// 好处：不必自带 INI 解析器（无异常 + 静态 CRT 环境下能省不少事），
// 且行为与游戏一致。CCFileClass 会透明处理路径。

#include <CCINIClass.h>
#include <CCFileClass.h>

#include "Config.h"
#include "Logger.h"

namespace Config {

    static Settings s_settings;
    static bool     s_loaded = false;

    static const char* kFileName = "ra2hook.ini";

    void Load()
    {
        if (s_loaded) return;
        s_loaded = true;

        CCFileClass file(kFileName);
        if (!file.Exists()) {
            Log::Info("Config: %s 不存在，全部功能保持默认（dump 关闭）", kFileName);
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

        s_settings.inject.enabled = ini.ReadBool("Inject", "Enabled", s_settings.inject.enabled);
        s_settings.logLevel       = ini.ReadInteger("Log", "Level", s_settings.logLevel);

        Log::g_level = static_cast<Log::Level>(s_settings.logLevel);

        Log::Info("Config: dump=%d (ini=%d csf=%d vxl=%d shp=%d sort=%d) inject=%d",
                  d.enabled ? 1 : 0, d.ini ? 1 : 0, d.csf ? 1 : 0,
                  d.vxl ? 1 : 0, d.shp ? 1 : 0, d.sortByOwner ? 1 : 0,
                  s_settings.inject.enabled ? 1 : 0);
    }

    const Settings& Get()
    {
        return s_settings;
    }

}  // namespace Config
