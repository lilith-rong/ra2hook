// DumpIni.h — INI / CSF 导出。
#pragma once

namespace DumpIni {

    // 导出内存中合并后的 INI 对象到 ra2hook\dump\ini\（标准 md 命名）
    void RunIni();

    // 原样拷贝 CSF 到 ra2hook\dump\csf\
    void RunCsf();

}  // namespace DumpIni
