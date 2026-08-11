#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <cstring>

#include "Logger.h"
#include "Runtime.h"
#include "RuntimeWatcher.h"

namespace RuntimeWatcher {
namespace {

    bool s_started = false;
    char s_directory[260] = {};
    int s_debounceMs = 500;

    unsigned long long DirectoryStamp(const wchar_t* directory)
    {
        wchar_t pattern[300] = {};
        std::swprintf(pattern, sizeof(pattern) / sizeof(pattern[0]),
                      L"%ls\\*.ini", directory);
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileW(pattern, &data);
        if (find == INVALID_HANDLE_VALUE) return 0;

        unsigned long long stamp = 1469598103934665603ull;
        do {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const unsigned long long time =
                (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                 data.ftLastWriteTime.dwLowDateTime;
            const unsigned long long size =
                (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) |
                 data.nFileSizeLow;
            stamp ^= time + size;
            stamp *= 1099511628211ull;
        } while (FindNextFileW(find, &data));
        FindClose(find);
        return stamp;
    }

    void WaitUntilStable(const wchar_t* directory)
    {
        Sleep(static_cast<DWORD>(s_debounceMs));
        for (int attempt = 0; attempt < 5; ++attempt) {
            const unsigned long long before = DirectoryStamp(directory);
            Sleep(100);
            if (before == DirectoryStamp(directory)) return;
        }
    }

    DWORD WINAPI WatchThread(void*)
    {
        wchar_t directory[260] = {};
        if (!MultiByteToWideChar(CP_ACP, 0, s_directory, -1,
                                 directory, static_cast<int>(sizeof(directory) / sizeof(directory[0])))) {
            Log::Error("runtime watcher: invalid directory %s", s_directory);
            return 0;
        }

        HANDLE handle = CreateFileW(directory, FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            Log::Error("runtime watcher: CreateFile failed for %s (%lu)",
                       s_directory, GetLastError());
            return 0;
        }

        BYTE buffer[4096] = {};
        for (;;) {
            DWORD bytes = 0;
            const BOOL ok = ReadDirectoryChangesW(handle, buffer, sizeof(buffer), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                FILE_NOTIFY_CHANGE_SIZE, &bytes, nullptr, nullptr);
            if (!ok) {
                Log::Error("runtime watcher: ReadDirectoryChangesW failed (%lu)",
                           GetLastError());
                break;
            }
            if (bytes == 0) continue;
            WaitUntilStable(directory);
            Runtime::NotifyFilesChanged();
        }

        CloseHandle(handle);
        return 0;
    }

}  // namespace

bool Start(const char* directory, int debounceMs)
{
    if (s_started) return true;
    if (!directory || !directory[0]) return false;
    std::snprintf(s_directory, sizeof(s_directory), "%s", directory);
    s_debounceMs = debounceMs;

    HANDLE thread = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
    if (!thread) {
        Log::Error("runtime watcher: CreateThread failed (%lu)", GetLastError());
        return false;
    }
    CloseHandle(thread);
    s_started = true;
    return true;
}

}  // namespace RuntimeWatcher
