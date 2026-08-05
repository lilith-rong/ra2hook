// Hooks.DumpProbe.cpp — 验证 dump 路线的致命前提（验证完即可删）。
//
// 【要验证什么】
// 整个 dump 方案建立在一个推断上：CCFileClass 会透明穿透 mix 包并完成解密，
// 因此 ReadWholeFile() 拿到的是明文原始字节。这个推断没有被证实过——若为假，
// dump 出来的将是加密垃圾，后续 art 映射等工作全部白做。
//
// 【怎么验证】
// 用 CCFileClass 读几个不同来源的文件，打印大小 + 文件头前 16 字节。
// 各格式的头部特征（明文时应当看到）：
//   .shp  前 2 字节为 0x0000，随后是 width/height/frames（小端 short）
//   .vxl  以 ASCII "Voxel Animation" 开头
//   .hva  头 16 字节是文件名字符串
//   .csf  以 ASCII " FSC" (即 'FSC ' 反序) 开头
//   .ini  纯文本，应能看到 '[' 或字母
// 若看到高熵随机字节且不匹配以上任何特征，说明拿到的是密文，方案需改道。

#include <Syringe.h>
#include <Helpers/Macro.h>
#include <CCFileClass.h>
#include <CCINIClass.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

#include "Config.h"
#include "DumpIO.h"
#include "Logger.h"

namespace DumpProbe {

    static void HexDumpHead(const char* name)
    {
        CCFileClass file(name);
        if (!file.Exists()) {
            Log::Info("  %-20s 不存在", name);
            return;
        }

        const int size = file.GetFileSize();
        void* data = file.ReadWholeFile();
        if (!data) {
            Log::Info("  %-20s size=%d 但读取失败", name, size);
            return;
        }

        const unsigned char* p = static_cast<const unsigned char*>(data);
        const int n = (size < 16) ? size : 16;

        char hex[64] = {};
        char asc[24] = {};
        int ho = 0;
        for (int i = 0; i < n; ++i) {
            ho += std::snprintf(hex + ho, sizeof(hex) - ho, "%02X ", p[i]);
            asc[i] = (p[i] >= 32 && p[i] < 127) ? static_cast<char>(p[i]) : '.';
        }
        asc[n] = '\0';

        Log::Info("  %-20s size=%-9d head=%s |%s|", name, size, hex, asc);

        YRMemory::Deallocate(data);
    }

    void Run()
    {
        Log::Info("---- dump probe: CCFileClass 能否读到明文 ----");

        // 1) 散装文件对照组（必然明文，用于确认探针本身正确）
        Log::Info(" [散装文件对照]");
        HexDumpHead("rulesmd.ini");
        HexDumpHead("ra2hook.ini");

        // 2) 标准 mix 内的素材（YR 原版素材，通常在 conquer.mix / cache.mix）
        Log::Info(" [标准 mix 内素材]");
        HexDumpHead("E1.SHP");
        HexDumpHead("HTNK.VXL");
        HexDumpHead("HTNK.HVA");

        // 3) MO 的文件（据用户反馈其 mix 有特殊加密，这几项是关键）
        Log::Info(" [MO / 加密 mix 内]");
        HexDumpHead("rulesmo.ini");
        HexDumpHead("artmo.ini");
        HexDumpHead("ra2md.csf");
        HexDumpHead("stringtable00.csf");

        Log::Info("---- end dump probe ----");

        // 4) 实际写盘测试：确认目录创建与写入链路可用
        const int written = DumpIO::CopyEngineFile("rulesmd.ini", "ini/_probe_rulesmd.ini");
        Log::Info("写盘测试 ra2hook\\dump\\ini\\_probe_rulesmd.ini -> %d 字节", written);
    }

}  // namespace DumpProbe

// 挂在 rules 全部加载完成之后（0x668F6A，Phobos 命名 InitializeAfterAllLoaded）。
// 此时所有 mix 已挂载，文件系统完全就绪。
DEFINE_HOOK(0x668F6A, RA2Hook_DumpProbe, 0x5)
{
    static bool once = false;
    if (once) return 0;
    once = true;

    Config::Load();

    // 探针受总开关控制：ra2hook.ini 里 [Dump] Enabled=yes 才跑
    if (Config::Get().dump.enabled)
        DumpProbe::Run();
    else
        Log::Info("dump probe: 已跳过（ra2hook.ini 中 [Dump] Enabled 未开启）");

    return 0;
}
