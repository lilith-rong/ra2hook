#pragma once

#include "IniRemoval.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace FakeFiles {
    struct File {
        std::string text;
        bool openable = true;
        bool shortRead = false;
        int reportedSize = -1;
    };
    inline std::map<std::string, File> files;
    inline std::vector<std::string> attempts;
    inline std::vector<std::string> roots;
    inline std::string scannedDirectory;
    inline bool scanFails = false;
    inline std::size_t closes = 0;
    inline std::string Identity(const std::string& path)
    {
        auto result = IniRemoval::NormalizePath(path);
        for (char& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        return result;
    }
    inline void Add(const std::string& path, const std::string& text)
    {
        files[Identity(path)] = { text };
    }
    inline void Reset()
    {
        files.clear(); attempts.clear(); roots.clear(); scannedDirectory.clear();
        scanFails = false; closes = 0;
    }
}

enum class FileAccessMode { Read };
class CCFileClass {
    std::string path_;
public:
    explicit CCFileClass(const char* path) : path_(FakeFiles::Identity(path)) {}
    bool Exists()
    {
        FakeFiles::attempts.push_back(path_);
        return FakeFiles::files.find(path_) != FakeFiles::files.end();
    }
    bool Open(FileAccessMode) { return FakeFiles::files.at(path_).openable; }
    int GetFileSize()
    {
        const auto& file = FakeFiles::files.at(path_);
        return file.reportedSize >= 0 ? file.reportedSize : static_cast<int>(file.text.size());
    }
    int ReadBytes(void* buffer, int size)
    {
        const auto& file = FakeFiles::files.at(path_);
        int count = std::min(size, static_cast<int>(file.text.size()));
        if (file.shortRead && count) --count;
        if (count) std::memcpy(buffer, file.text.data(), static_cast<std::size_t>(count));
        return count;
    }
    void Close() { ++FakeFiles::closes; }
};
