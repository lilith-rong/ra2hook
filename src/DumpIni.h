// DumpIni.h — INI / CSF 导出。
//
// 注意：注释里不要以反斜杠结尾。行尾反斜杠是续行符，会把下一行一起吞进注释
// （曾因此让 RunCsf 声明消失，编译器报 "is not a member"）。路径一律写正斜杠。
#pragma once

namespace DumpIni {

    // 导出内存中合并后的 INI 对象到 ra2hook/dump/ini/（标准 md 命名）
    void RunIni();

    // 原样拷贝 CSF 到 ra2hook/dump/csf/
    void RunCsf();

}  // namespace DumpIni
