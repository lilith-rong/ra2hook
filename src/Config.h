// Config.h — 运行时配置。
//
// 读 RA2 根目录下的 ra2hook.ini（不用 JSON：引擎自带 INI 解析器，
// 而我们处在无异常、静态 CRT 环境，少一个解析器少一堆麻烦）。
//
// 配置缺失一律走安全默认值：dump 全关。理由见 DEVELOPMENT.md §5.2——
// 任何失败都不得阻止游戏启动。
#pragma once

namespace Config {

    struct DumpSettings {
        bool enabled     = false;   // 总开关，默认关（dump 会拖慢启动、占磁盘）
        bool ini         = false;
        bool csf         = false;
        bool vxl         = false;
        bool shp         = false;
        bool sortByOwner = true;    // VXL/SHP 是否按 art 注册名归类到子文件夹

        // 导出 INI 时剔除首部 [#include] 段。默认开：include 内容已合并进本文件，
        // 留着会让这份 dump 被 Ares 再次展开一遍（重复合并 / 文件缺失报错）。
        bool stripInclude = true;
    };

    struct InjectSettings {
        bool enabled = false;
    };

    struct Settings {
        DumpSettings   dump;
        InjectSettings inject;
        int            logLevel = 3;   // 对应 Log::Level::Info
    };

    // 载入 ra2hook.ini。多次调用只生效一次。
    void Load();

    const Settings& Get();

}  // namespace Config
