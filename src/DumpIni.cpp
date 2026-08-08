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
#include <cstdlib>   // malloc / free
#include <cstring>   // strstr / _stricmp

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

    // 从已写出的 INI 文件里剔除首部的 [#include] 段。
    //
    // 为什么在文本层做而不用 INIClass::Clear：Clear(s1,s2) 的语义未文档化，
    // 而它作用于**真实的 rules 对象**，用错会破坏游戏数据。写盘后改文本零风险。
    //
    // 为什么要剔除：dump 出来的 rules 里 include 内容已全部合并进来，[#include]
    // 段只是历史记录。若直接拿这份文件当 mod 的 rules 用，Ares 会再加载一遍那
    // 几百个文件（内容重复合并，文件缺失则报错）。剔除后 dump 才是自包含快照。
    static bool StripIncludeSection(const char* fullPath)
    {
        std::FILE* f = std::fopen(fullPath, "rb");
        if (!f) return false;

        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size <= 0) { std::fclose(f); return false; }

        char* buf = static_cast<char*>(std::malloc(static_cast<size_t>(size) + 1));
        if (!buf) { std::fclose(f); return false; }
        const size_t got = std::fread(buf, 1, static_cast<size_t>(size), f);
        std::fclose(f);
        buf[got] = '\0';

        // 定位 [#include] 段：从它开始，到下一个以 '[' 开头的行为止
        const char* incl = std::strstr(buf, "[#include]");
        if (!incl) { std::free(buf); return true; }   // 没有就无需处理

        const char* p = incl + 10;                     // 跳过 "[#include]"
        const char* nextSection = nullptr;
        while (*p) {
            if (*p == '\n' && *(p + 1) == '[') { nextSection = p + 1; break; }
            ++p;
        }

        std::FILE* out = std::fopen(fullPath, "wb");
        if (!out) { std::free(buf); return false; }

        // 保留 [#include] 之前的内容
        if (incl > buf)
            std::fwrite(buf, 1, static_cast<size_t>(incl - buf), out);
        // 保留下一个段落之后的全部内容
        if (nextSection)
            std::fwrite(nextSection, 1, got - static_cast<size_t>(nextSection - buf), out);

        std::fclose(out);
        std::free(buf);
        return true;
    }

    // 把内存中的 INI 对象写到 dump/ini/<outName>
    static bool WriteMemoryIni(CCINIClass* pINI, const char* outName)
    {
        if (!pINI) {
            Log::Warn("DumpIni: %s 的 INI 对象为空，跳过", outName);
            return false;
        }

        // 空对象不产出空文件——否则会让人误以为该配置本身是空的。
        // 实测 INI_UIMD 在此时机段落数为 0（未加载或 MO 未使用）。
        const int sections = CountSections(pINI);
        if (sections <= 0) {
            Log::Info("DumpIni: %-16s 跳过（段落数 %d，该 INI 实例此时未加载）",
                      outName, sections);
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

        bool stripped = false;
        if (Config::Get().dump.stripInclude)
            stripped = StripIncludeSection(full);

        Log::Info("DumpIni: %-16s <- %d 个段落%s",
                  outName, sections, stripped ? "（已剔除 [#include]）" : "");
        return true;
    }

    // uimd：ifWriteMemoryIni 因对象段数为 0 而没产出文件（返回 false），
    // 引擎读取 uimd.ini 走独立散装读取、不进 INI_UIMD 全局对象。这时在根目录
    // 原样拷贝引擎正在用的散装 uimd.ini，保证 dump 结果可复用。
    static void DumpLooseFallbackIfNeeded(const char* fileName)
    {
        char full[260] = {};
        std::snprintf(full, sizeof(full), "%s\\ini\\%s", DumpIO::kDumpRoot, fileName);
        if (DumpIO::EnsureDirForFile(full)) {
            if (std::FILE* f = std::fopen(full, "rb")) {   // 内存 dump 已产出
                std::fclose(f);
                return;
            }
        }

        char rel[260] = {};
        std::snprintf(rel, sizeof(rel), "ini\\%s", fileName);
        const int n = DumpIO::CopyEngineFile(fileName, rel);
        if (n > 0)
            Log::Info("DumpIni: %-16s <- 内存对象为空，散装文件原样拷贝（%d 字节）",
                      fileName, n);
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

        // INI_UIMD 的对象是空的，散装 uimd.ini 才有真实内容——补一颗。
        DumpLooseFallbackIfNeeded("uimd.ini");
    }

    // 尝试导出一个 CSF 文件；若已处理过同名文件则跳过。
    // 返回导出的字节数（已存在时重复导出没意义）。
    static int TryCsf(const char* name, char (&seen)[64][32], int& seenCount)
    {
        for (int i = 0; i < seenCount; ++i)
            if (_stricmp(seen[i], name) == 0)
                return 0;
        if (seenCount < 64)
            std::snprintf(seen[seenCount++], 32, "%s", name);

        char rel[260] = {};
        std::snprintf(rel, sizeof(rel), "csf\\%s", name);
        return DumpIO::CopyEngineFile(name, rel);
    }

    void RunCsf()
    {
        Log::Info(" [CSF] 探测引擎实际加载的 CSF 名（ra2/ra2md + stringtable 数值）");

        // CSF 没有"内存中合并后的对象"可导，只能按文件名逐个拷。
        //
        // 不硬编码编号清单（过去 MO 新加的 stringtable 编号全靠手抄，容易漏）。
        // 引擎只会按固定的几种名字形式解析 CSF：
        //   基本语言文件  ra2.csf / ra2md.csf（散装或 mix 内都可能）
        //   扩展字符串表 stringtableNNNN.csf（两位/三位数字编号）
        // 据此全部走 CCFileClass 探测 —— 它同时覆盖散装与 mix 内文件，
        // 无需再扫描根目录（曾经的「路 1」对引擎可加载的文件是冗余的）。

        // 已经处理过的名字，避免两个探测段重复导出同一个文件。
        char seen[64][32] = {};
        int  seenCount = 0;

        int ok = 0, bytes = 0;

        // ── 路 1：基本语言文件 ────────────────────────────────────────────
        // ra2/ra2md 是硬名，不匹配后面的 stringtable 编号模式，需单独探测。
        static const char* kBase[] = { "ra2.csf", "ra2md.csf" };
        for (const char* name : kBase) {
            const int n = TryCsf(name, seen, seenCount);
            if (n > 0) { ok += 1; bytes += n; Log::Info("DumpCsf: %-24s %9d 字节", name, n); }
        }

        // ── 路 2：stringtableNNNN 数值试探（NN 两位与三位都试）────────────
        // 两位：00..99；三位：100..599（RA2+MO 实际用到的编号都远小于 600）。
        // 引擎对不存在的文件返回 -1 即自然跳过，试探本身零成本。
        for (int n = 0; n < 600; ++n) {
            char name[32] = {};
            if (n < 100)
                std::snprintf(name, sizeof(name), "stringtable%02d.csf", n);
            else
                std::snprintf(name, sizeof(name), "stringtable%03d.csf", n);
            const int r = TryCsf(name, seen, seenCount);
            if (r > 0) { ok += 1; bytes += r; Log::Info("DumpCsf: %-24s %9d 字节", name, r); }
        }

        Log::Info(" [CSF] 共导出 %d 个文件（%d 字节）", ok, bytes);
    }

}  // namespace DumpIni
