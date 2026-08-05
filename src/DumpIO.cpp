// DumpIO.cpp
//
// 核心手法：用引擎自己的 CCFileClass 读文件。它会透明穿透 mix 包并处理解密，
// 所以 ReadWholeFile() 拿到的是**已解密的原始文件字节**——原样写盘即为合法
// 的 .vxl/.shp/.ini/.csf，我们不需要理解也不需要重新编码任何格式。
//
// 内存归属：ReadWholeFile 从游戏内存池分配，必须用 YRMemory::Deallocate 释放，
// 不能用 free/delete。

#include <CCFileClass.h>
#include <Memory.h>

#include <windows.h>
#include <cstdio>
#include <cstring>

#include "DumpIO.h"
#include "Logger.h"

namespace DumpIO {

    const char* kDumpRoot = "ra2hook\\dump";

    bool EnsureDir(const char* path)
    {
        // 逐级创建。CreateDirectoryA 对已存在目录返回 ALREADY_EXISTS，视为成功。
        char buf[MAX_PATH] = {};
        std::strncpy(buf, path, sizeof(buf) - 1);

        for (char* p = buf; *p; ++p) {
            if (*p == '\\' || *p == '/') {
                char saved = *p;
                *p = '\0';
                if (buf[0] && !(std::strlen(buf) == 2 && buf[1] == ':')) {
                    if (!CreateDirectoryA(buf, nullptr) &&
                        GetLastError() != ERROR_ALREADY_EXISTS) {
                        Log::Warn("EnsureDir: 建目录失败 %s (err=%lu)", buf, GetLastError());
                        return false;
                    }
                }
                *p = saved;
            }
        }
        if (!CreateDirectoryA(buf, nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            Log::Warn("EnsureDir: 建目录失败 %s (err=%lu)", buf, GetLastError());
            return false;
        }
        return true;
    }

    // 从 "vxl/HTNK/HTNK.VXL" 得到 "ra2hook\dump\vxl\HTNK"（供建目录用）
    static void BuildFullPath(const char* relPath, char* out, int outSize)
    {
        std::snprintf(out, outSize, "%s\\%s", kDumpRoot, relPath);
        for (char* p = out; *p; ++p)
            if (*p == '/') *p = '\\';
    }

    static bool EnsureParentDir(const char* fullPath)
    {
        char dir[MAX_PATH] = {};
        std::strncpy(dir, fullPath, sizeof(dir) - 1);
        char* lastSep = std::strrchr(dir, '\\');
        if (!lastSep) return true;
        *lastSep = '\0';
        return EnsureDir(dir);
    }

    bool WriteBuffer(const char* relPath, const void* data, int size)
    {
        if (!data || size <= 0) return false;

        char full[MAX_PATH] = {};
        BuildFullPath(relPath, full, sizeof(full));
        if (!EnsureParentDir(full)) return false;

        std::FILE* f = std::fopen(full, "wb");
        if (!f) {
            Log::Warn("WriteBuffer: 打不开 %s", full);
            return false;
        }
        size_t written = std::fwrite(data, 1, static_cast<size_t>(size), f);
        std::fclose(f);

        if (static_cast<int>(written) != size) {
            Log::Warn("WriteBuffer: %s 写入不全 %zu/%d", full, written, size);
            return false;
        }
        return true;
    }

    int CopyEngineFile(const char* srcName, const char* relPath)
    {
        CCFileClass file(srcName);
        if (!file.Exists()) {
            Log::Debug("CopyEngineFile: 引擎中不存在 %s", srcName);
            return -1;
        }

        const int size = file.GetFileSize();
        if (size <= 0) {
            Log::Debug("CopyEngineFile: %s 大小为 %d", srcName, size);
            return -1;
        }

        // ReadWholeFile：穿透 mix + 解密后的原始字节
        void* data = file.ReadWholeFile();
        if (!data) {
            Log::Warn("CopyEngineFile: 读取失败 %s", srcName);
            return -1;
        }

        const bool ok = WriteBuffer(relPath, data, size);
        YRMemory::Deallocate(data);   // 必须还给游戏内存池

        return ok ? size : -1;
    }

}  // namespace DumpIO
