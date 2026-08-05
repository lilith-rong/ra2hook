// DumpIO.h — dump 的磁盘写入与目录管理。
#pragma once

namespace DumpIO {

    // dump 根目录（相对 RA2 根目录）。目录结构见 DEVELOPMENT.md。
    //   ra2hook/dump/ini|csf|vxl|shp/
    //   ra2hook/inject/
    extern const char* kDumpRoot;

    // 递归建目录（等价 mkdir -p）。已存在视为成功。
    bool EnsureDir(const char* path);

    // 用 CCFileClass 读引擎能看到的文件（透明穿透 mix、含解密），
    // 原样写到 dump 目录下的 relPath。返回写出的字节数，失败返回 -1。
    //   srcName  引擎内的文件名，如 "HTNK.VXL"
    //   relPath  相对 kDumpRoot 的目标路径，如 "vxl/HTNK/HTNK.VXL"
    int CopyEngineFile(const char* srcName, const char* relPath);

    // 直接写一段内存到 dump 目录下的 relPath。
    bool WriteBuffer(const char* relPath, const void* data, int size);

}  // namespace DumpIO
