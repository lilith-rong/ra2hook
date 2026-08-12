// Hooks.DumpProbe.cpp — dump 总入口。
//
// 挂在 0x668F6A（Phobos 命名 InitializeAfterAllLoaded）：rules 全部加载完成、
// 所有 mix 已挂载、文件系统完全就绪。dump 在此一次性执行。
//
// 【前提已验证】CCFileClass 对加密 mix 返回明文：
//   rulesmo.ini -> EF BB BF ";;;;;;;;;;Me"（UTF-8 BOM + 文本）
//   artmo.ini   -> "[#include]"
//   ra2md.csf   -> " FSC" 魔数
//   HTNK.VXL    -> "Voxel Animation"
// 故 dump 只需原样搬字节，不需要实现任何格式编码器。
//
// 【已知事实】E1.SHP 不存在而 HTNK.VXL 存在 —— MO 重命名了素材，
// 因此 VXL/SHP 的文件名必须从 art 的 Image= 等键读取，不能用 TypeClass ID 猜。
// 这是后续 art 映射阶段的工作。

#include <Syringe.h>
#include <Helpers/Macro.h>

#include "Config.h"
#include "ArtMap.h"
#include "DumpIni.h"
#include "DumpIO.h"
#include "Logger.h"
#include "Runtime.h"

DEFINE_HOOK(0x668F6A, RA2Hook_DumpEntry, 0x5)
{
    Runtime::Initialize();

    static bool once = false;
    if (once) return 0;
    once = true;

    Config::Load();
    const auto& cfg = Config::Get();

    if (!cfg.dump.enabled) {
        Log::Info("dump: 已跳过（配置中的 [Dump] Enabled 未开启）");
        return 0;
    }

    Log::Info("==== ra2hook dump 开始 ====");

    if (cfg.dump.ini)
        DumpIni::RunIni();
    if (cfg.dump.csf)
        DumpIni::RunCsf();
    if (cfg.dump.vxl || cfg.dump.shp)
        ArtMap::Run();

    Log::Info("==== ra2hook dump 结束，输出至 %s ====", DumpIO::kDumpRoot);
    return 0;
}
