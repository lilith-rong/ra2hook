// Config.h — 运行时配置。
//
// 优先读 <game>\ra2hook\ra2hook.ini，兼容回退 <game>\ra2hook.ini。
// 路径基于当前游戏 EXE，而不是可能被启动器改变的工作目录。
//
// 配置缺失一律走安全默认值：dump/inject/runtime 全关。理由见 DEVELOPMENT.md §5.2——
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

        // 只导出指定单位的 VXL/SHP（逗号分隔的 ID，如 "HTNK,MTNK,GI"）。
        // 留空 = 全部。仅影响 VXL/SHP，不影响 INI/CSF。
        // 长度上限够放几十个 ID；超长会被截断并告警。
        char onlyUnits[512] = {};
    };

    struct InjectSettings {
        bool enabled = false;

        // 注入目标只由 enabled 下的子目录决定：
        //   ra2hook/inject/enabled/rules/*.ini   -> INI_Rules
        //   ra2hook/inject/enabled/ra2md/*.ini   -> INI_RA2MD
        //   ra2hook/inject/enabled/art|ai|uimd  -> 目标已注册（挂点见 Hooks.RulesInject.cpp）
        //   ra2hook/inject/enabled/sound/*.ini -> SOUNDMD 两阶段覆盖（0x52C6C4/0x7510F6）
        // 每个目录内按文件名（不区分大小写）排序，后写覆盖前写。

        // 是否把 ra2hook/inject/mix/*.mix 全部注册进引擎文件系统。
        // mix 内可放自定义 SHP/VXL/PCX —— 这类资源只被惰性引用，注入时机宽松。
        // 文件名无约定，目录下所有 *.mix 都会被 new MixFileClass 装载。
        bool mix = false;
    };

    struct RuntimeSettings {
        // IPC is always available once ra2hook has initialized, but game data
        // writes remain disabled unless this switch is enabled.
        bool enabled = false;
        bool autoApply = true;
        int debounceMs = 500;
        char directory[260] = "ra2hook\\runtime";
    };

    struct Settings {
        DumpSettings   dump;
        InjectSettings inject;
        RuntimeSettings runtime;
        int            logLevel = 3;   // 对应 Log::Level::Info
    };

    // 载入配置。多次调用只生效一次。
    void Load();

    const Settings& Get();

}  // namespace Config
