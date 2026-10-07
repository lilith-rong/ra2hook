#pragma once

// Minimal Win32 path API used by the real GamePaths/Logger headers in adapter
// tests. This is not a model of the native game memory layout.
#include <cstring>
#include <cwchar>

using DWORD = unsigned long;
using HANDLE = void*;
constexpr DWORD MAXDWORD = 0xFFFFFFFFul;
constexpr int MAX_PATH = 260;

inline DWORD GetModuleFileNameA(HANDLE, char* output, DWORD capacity)
{
    constexpr const char* path = "C:\\game\\gamemd.exe";
    const auto length = std::strlen(path);
    if (capacity <= length) return capacity;
    std::memcpy(output, path, length + 1);
    return static_cast<DWORD>(length);
}
inline DWORD GetModuleFileNameW(HANDLE, wchar_t* output, DWORD capacity)
{
    constexpr const wchar_t* path = L"C:\\game\\gamemd.exe";
    const auto length = std::wcslen(path);
    if (capacity <= length) return capacity;
    std::wmemcpy(output, path, length + 1);
    return static_cast<DWORD>(length);
}
inline int CreateDirectoryA(const char*, void*) { return 1; }

#ifndef _WIN32
inline int _stricmp(const char* left, const char* right)
{
    for (;;) {
        const auto fold = [](unsigned char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + 32) : c;
        };
        const auto a = fold(static_cast<unsigned char>(*left++));
        const auto b = fold(static_cast<unsigned char>(*right++));
        if (a != b || !a || !b) return static_cast<int>(a) - static_cast<int>(b);
    }
}
inline int _strnicmp(const char* left, const char* right, std::size_t count)
{
    while (count--) {
        const auto fold = [](unsigned char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + 32) : c;
        };
        const auto a = fold(static_cast<unsigned char>(*left++));
        const auto b = fold(static_cast<unsigned char>(*right++));
        if (a != b || !a || !b) return static_cast<int>(a) - static_cast<int>(b);
    }
    return 0;
}
#endif
