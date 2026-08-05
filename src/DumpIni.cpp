// DumpIni.cpp — 导出引擎内存中的 INI 与 CSF。
//
// 两种导出方式，用途不同：
//
// 1) INI —— 导出**内存中合并后的对象**（WriteCCFile）。
//    这是关键：MO 的 art/rules 都用 Ares [#include] 拆成了多个文件
//    （artmo.ini 首行就是 [#include]），单独拷贝源文件只能得到一个清单。
//    内存对象里是所有 include、所有插件合并后的最终状态。
//    按用户要求用标准 md 名输出（rulesmd.ini 等）——含义是"最终生效的 rules"，
//    而非"原版那个 rulesmd.ini"。
//
// 2) CSF —— 原样字节拷贝（CopyEngineFile）。
//    CSF 是二进制字符串表，不经过 CCINIClass，引擎也没有公开的写出 API。
//    好在 CCFileClass 已能拿到明文（探针确认 " FSC" 魔数），直接搬字节即可。

#include <CCINIClass.h>
#include <CCFileClass.h>

#include <cstdio>

#include "Config.h"
#include "DumpIO.h"
#include "DumpIni.h"
#include "Logger.h"

namespace DumpIni {

    // 统计一个 INI 对象里的段落数，用于日志核对导出是否合理。
    // 注意：GenericList 末尾是哨兵节点，终止条件必须用 IsValid()，
    // 用 != nullptr 会越过尾哨兵读到非法内存。
    static int CountSections(INIClass* pINI)
    {
        if (!pINI) return -1;
        int n = 0;
        for (auto* s = pINI->Sections.First(); s && s->IsValid(); s = s->Next())
            ++n;
        return n;
    }

    // 把内存中的 INI 对象写到 dump\ini\<outName>
    static bool WriteMemoryIni(CCINIClass* pINI, const char* outName)
    {
        if (!pINI) {
            Log::Warn("DumpIni: %s 的 INI 对象为空，跳过", outName);
            return false;
        }

        char rel[260] = {};
        std::snprintf(rel, sizeof(rel), "ini\\%s", outName);

        char full[260] = {};
        std::snprintf(full, sizeof(full), "%s\\%s", DumpIO::kDumpRoot, rel);
        for (char* p = full; *p; ++p)
            if (*p == '/') *p = '\\';

        if (!DumpIO::EnsureDirForFile(full))
            return false;

        // RawFileClass 直接写磁盘（不走 mix 查找）。
        // Open(Write) 会按需创建，不必先 CreateFile。
        RawFileClass file(full);
        if (!file.Open(FileAccessMode::Write)) {
            Log::Warn("DumpIni: 无法写入 %s", full);
            return false;
        }

        pINI->WriteCCFile(&file, false);   // 0x474430
        file.Close();

        Log::Info("DumpIni: %-16s <- %d 个段落", outName, CountSections(pINI));
        return true;
    }

    void RunIni()
    {
        Log::Info(" [INI] 导出内存中合并后的对象（标准 md 命名）");

        // 注意 YRpp 里这几个的形式不同：
        //   INI_Rules 是 CCINIClass*（指针）
        //   INI_AI / INI_Art / INI_UIMD / INI_RA2MD 是 CCINIClass（对象，需取址）
        WriteMemoryIni(CCINIClass::INI_Rules,   "rulesmd.ini");
        WriteMemoryIni(&CCINIClass::INI_Art,    "artmd.ini");
        WriteMemoryIni(&CCINIClass::INI_AI,     "aimd.ini");
        WriteMemoryIni(&CCINIClass::INI_UIMD,   "uimd.ini");
        WriteMemoryIni(&CCINIClass::INI_RA2MD,  "ra2md.ini");
    }

    void RunCsf()
    {
        Log::Info(" [CSF] 原样字节拷贝");

        // CSF 没有"内存中合并后的对象"可导，只能按文件名逐个拷。
        // 覆盖 YR 原版 + MO 已知用到的 stringtable 编号；不存在的会被静默跳过。
        static const char* kNames[] = {
            "ra2md.csf",
            "ra2.csf",
            "stringtable00.csf", "stringtable01.csf", "stringtable02.csf",
            "stringtable03.csf", "stringtable04.csf", "stringtable05.csf",
            "stringtable06.csf", "stringtable07.csf", "stringtable08.csf",
            "stringtable09.csf", "stringtable10.csf", "stringtable11.csf",
            "stringtable12.csf", "stringtable13.csf", "stringtable14.csf",
        };

        int ok = 0;
        for (const char* name : kNames) {
            char rel[260] = {};
            std::snprintf(rel, sizeof(rel), "csf\\%s", name);
            const int n = DumpIO::CopyEngineFile(name, rel);
            if (n > 0) {
                Log::Info("DumpCsf: %-20s %d 字节", name, n);
                ++ok;
            }
        }
        Log::Info(" [CSF] 共导出 %d 个文件", ok);
    }

}  // namespace DumpIni
