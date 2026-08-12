// Paths owned by ra2hook, anchored to the running game's executable directory.
#pragma once

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

namespace GamePaths {

    inline bool ExecutableDirectory(wchar_t* output, size_t capacity)
    {
        if (!output || capacity == 0 || capacity > MAXDWORD) return false;

        const DWORD length = GetModuleFileNameW(
            nullptr, output, static_cast<DWORD>(capacity));
        if (length == 0 || length >= capacity) return false;

        wchar_t* slash = std::wcsrchr(output, L'\\');
        wchar_t* alternate = std::wcsrchr(output, L'/');
        if (!slash || (alternate && alternate > slash)) slash = alternate;
        if (!slash) return false;
        *slash = L'\0';
        return true;
    }

    inline bool IsAbsolute(const char* path)
    {
        if (!path || !path[0]) return false;
        const bool driveRoot =
            (((path[0] >= 'A' && path[0] <= 'Z') ||
              (path[0] >= 'a' && path[0] <= 'z')) &&
             path[1] == ':' && (path[2] == '\\' || path[2] == '/'));
        const bool networkRoot =
            ((path[0] == '\\' || path[0] == '/') &&
             (path[1] == '\\' || path[1] == '/'));
        return driveRoot || networkRoot;
    }

    inline bool Build(char* output, size_t capacity, const char* relative)
    {
        if (!output || capacity == 0 || !relative || !relative[0]) return false;

        char executable[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, executable,
                                                static_cast<DWORD>(sizeof(executable)));
        if (length == 0 || length >= sizeof(executable)) return false;

        char* slash = std::strrchr(executable, '\\');
        char* alternate = std::strrchr(executable, '/');
        if (!slash || (alternate && alternate > slash)) slash = alternate;
        if (!slash) return false;
        *slash = '\0';

        const int written = std::snprintf(output, capacity, "%s\\%s",
                                          executable, relative);
        return written > 0 && static_cast<size_t>(written) < capacity;
    }

    inline bool Resolve(char* output, size_t capacity, const char* path)
    {
        if (!output || capacity == 0 || !path || !path[0]) return false;
        if (!IsAbsolute(path)) return Build(output, capacity, path);

        const int written = std::snprintf(output, capacity, "%s", path);
        return written > 0 && static_cast<size_t>(written) < capacity;
    }

    inline bool BuildRuntimePipeName(char* output, size_t capacity)
    {
        wchar_t directory[MAX_PATH] = {};
        if (!ExecutableDirectory(directory, sizeof(directory) / sizeof(directory[0])))
            return false;

        unsigned int hash = 2166136261u;
        for (const wchar_t* cursor = directory; *cursor; ++cursor) {
            unsigned int value = static_cast<unsigned int>(*cursor);
            if (value >= L'a' && value <= L'z') value -= L'a' - L'A';
            hash ^= value & 0xFFu;
            hash *= 16777619u;
            hash ^= (value >> 8) & 0xFFu;
            hash *= 16777619u;
        }

        const int written = std::snprintf(
            output, capacity, "\\\\.\\pipe\\ra2hook-runtime-v1-%08X", hash);
        return written > 0 && static_cast<size_t>(written) < capacity;
    }

    inline void EnsureDataDirectory()
    {
        char directory[MAX_PATH] = {};
        if (Build(directory, sizeof(directory), "ra2hook")) {
            CreateDirectoryA(directory, nullptr);
        }
    }

}  // namespace GamePaths
