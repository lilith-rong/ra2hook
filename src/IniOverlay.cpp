#include <CCINIClass.h>
#include <CCFileClass.h>

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>

#include "IniOverlay.h"
#include "Logger.h"

namespace IniOverlay {
namespace {

    constexpr int kMaxIniBytes = 8 * 1024 * 1024;
    constexpr int kIniTokenMax = 512;
    constexpr int kMaxIncludeDepth = 32;

    const char* Tag(const char* tag)
    {
        return tag && tag[0] ? tag : "ini";
    }

    void RecordError(MergeStats* stats, const char* format, ...)
    {
        if (!stats) return;
        ++stats->errors;
        if (stats->firstError[0]) return;

        va_list args;
        va_start(args, format);
        std::vsnprintf(stats->firstError, sizeof(stats->firstError), format, args);
        va_end(args);
    }

    bool IsIncludeSection(const char* name)
    {
        return name && _stricmp(name, "#include") == 0;
    }

    bool IsRootedPath(const char* path)
    {
        if (!path || !path[0]) return false;
        return path[0] == '\\' || path[0] == '/' ||
               (path[0] && path[1] == ':');
    }

    void NormalizeSlashes(char* path)
    {
        if (!path) return;
        for (char* p = path; *p; ++p) {
            if (*p == '/') *p = '\\';
        }
    }

    bool CopyTrimmedSpan(const char* begin, const char* end,
                         char* dst, size_t dstSize, bool stripQuotes)
    {
        if (!dst || dstSize == 0) return false;
        dst[0] = '\0';
        if (!begin || !end || end < begin) return false;

        while (begin < end && (*begin == ' ' || *begin == '\t')) ++begin;
        while (end > begin && (end[-1] == ' ' || end[-1] == '\t' ||
                               end[-1] == '\r' || end[-1] == '\n')) {
            --end;
        }

        if (stripQuotes && end > begin + 1 &&
            ((*begin == '"' && end[-1] == '"') ||
             (*begin == '\'' && end[-1] == '\''))) {
            ++begin;
            --end;
        }

        const size_t len = static_cast<size_t>(end - begin);
        if (len >= dstSize) return false;
        std::memcpy(dst, begin, len);
        dst[len] = '\0';
        return true;
    }

    char* DuplicateTrimmedSpan(const char* begin, const char* end)
    {
        if (!begin || !end || end < begin) return nullptr;
        while (begin < end && (*begin == ' ' || *begin == '\t')) ++begin;
        while (end > begin && (end[-1] == ' ' || end[-1] == '\t' ||
                               end[-1] == '\r' || end[-1] == '\n')) {
            --end;
        }

        const size_t len = static_cast<size_t>(end - begin);
        char* value = static_cast<char*>(std::malloc(len + 1));
        if (!value) return nullptr;
        std::memcpy(value, begin, len);
        value[len] = '\0';
        return value;
    }

    void ParentDirOf(const char* path, char* out, size_t outSize)
    {
        if (!out || outSize == 0) return;
        out[0] = '\0';
        if (!path || !path[0]) return;

        const char* last = nullptr;
        for (const char* p = path; *p; ++p) {
            if (*p == '\\' || *p == '/') last = p;
        }
        if (!last) return;

        const size_t len = static_cast<size_t>(last - path);
        std::snprintf(out, outSize, "%.*s",
                      static_cast<int>(len < outSize ? len : outSize - 1), path);
        NormalizeSlashes(out);
    }

    struct IncludeContext {
        char stack[kMaxIncludeDepth][kPathMax] = {};
        int depth = 0;
        MergeStats* stats = nullptr;
        const char* logTag = nullptr;
    };

    bool IsInIncludeStack(const IncludeContext& ctx, const char* path)
    {
        for (int i = 0; i < ctx.depth; ++i) {
            if (_stricmp(ctx.stack[i], path) == 0) return true;
        }
        return false;
    }

    bool ReadIniResolved(const char* request, const char* baseDir,
                         char** outData, int* outSize, char* usedPath,
                         IncludeContext& ctx)
    {
        char name[kPathMax] = {};
        if (!request || !outData || !outSize || !usedPath) return false;
        const char* requestEnd = request + std::strlen(request);
        if (!CopyTrimmedSpan(request, requestEnd, name, sizeof(name), true)) {
            RecordError(ctx.stats, "path too long: %s", request);
            return false;
        }
        NormalizeSlashes(name);
        if (!name[0]) {
            RecordError(ctx.stats, "empty include path");
            return false;
        }

        char candidates[2][kPathMax] = {};
        int count = 0;
        if (baseDir && baseDir[0] && !IsRootedPath(name)) {
            const int written = std::snprintf(candidates[count], kPathMax,
                                              "%s\\%s", baseDir, name);
            if (written > 0 && written < kPathMax) {
                NormalizeSlashes(candidates[count]);
                ++count;
            }
        }

        std::snprintf(candidates[count], kPathMax, "%s", name);
        NormalizeSlashes(candidates[count]);
        ++count;

        for (int i = 0; i < count; ++i) {
            CCFileClass file(candidates[i]);
            if (!file.Exists()) continue;
            if (!file.Open(FileAccessMode::Read)) {
                Log::Warn("%s: cannot open %s", Tag(ctx.logTag), candidates[i]);
                continue;
            }

            const int size = file.GetFileSize();
            if (size < 0 || size > kMaxIniBytes) {
                Log::Warn("%s: invalid file size %s (%d)",
                          Tag(ctx.logTag), candidates[i], size);
                file.Close();
                continue;
            }

            char* data = static_cast<char*>(std::malloc(static_cast<size_t>(size) + 1));
            if (!data) {
                file.Close();
                RecordError(ctx.stats, "out of memory reading %s", candidates[i]);
                return false;
            }

            const int read = size > 0 ? file.ReadBytes(data, size) : 0;
            file.Close();
            if (read != size) {
                std::free(data);
                Log::Warn("%s: short read %s (%d/%d)",
                          Tag(ctx.logTag), candidates[i], read, size);
                continue;
            }

            data[size] = '\0';
            *outData = data;
            *outSize = size;
            std::snprintf(usedPath, kPathMax, "%s", candidates[i]);
            return true;
        }

        RecordError(ctx.stats, "file not found: %s", name);
        return false;
    }

    int ParseRawIni(CCINIClass& out, const char* data, int size,
                    const char* path, IncludeContext& ctx)
    {
        if (!data || size < 0) return 0;

        char section[kIniTokenMax] = {};
        int sectionCount = 0;
        int lineNumber = 0;
        const char* p = data;
        const char* end = data + size;
        if (size >= 3 && static_cast<unsigned char>(p[0]) == 0xEF &&
            static_cast<unsigned char>(p[1]) == 0xBB &&
            static_cast<unsigned char>(p[2]) == 0xBF) {
            p += 3;
        }

        while (p < end) {
            ++lineNumber;
            const char* lineEnd = p;
            while (lineEnd < end && *lineEnd != '\r' && *lineEnd != '\n') ++lineEnd;

            const char* begin = p;
            while (begin < lineEnd && (*begin == ' ' || *begin == '\t')) ++begin;
            const char* trimmedEnd = lineEnd;
            while (trimmedEnd > begin &&
                   (trimmedEnd[-1] == ' ' || trimmedEnd[-1] == '\t')) {
                --trimmedEnd;
            }

            if (begin < trimmedEnd && *begin != ';' && *begin != '#') {
                if (*begin == '[' && trimmedEnd[-1] == ']') {
                    if (CopyTrimmedSpan(begin + 1, trimmedEnd - 1,
                                        section, sizeof(section), false) && section[0]) {
                        ++sectionCount;
                    } else {
                        section[0] = '\0';
                        RecordError(ctx.stats, "%s:%d invalid section", path, lineNumber);
                    }
                } else {
                    const char* equal = begin;
                    while (equal < trimmedEnd && *equal != '=') ++equal;
                    if (equal >= trimmedEnd || !section[0]) {
                        RecordError(ctx.stats, "%s:%d key outside section or missing '='",
                                    path, lineNumber);
                    } else {
                        char key[kIniTokenMax] = {};
                        if (!CopyTrimmedSpan(begin, equal, key, sizeof(key), false) || !key[0]) {
                            RecordError(ctx.stats, "%s:%d invalid key", path, lineNumber);
                        } else {
                            char* value = DuplicateTrimmedSpan(equal + 1, trimmedEnd);
                            if (!value) {
                                RecordError(ctx.stats, "%s:%d out of memory", path, lineNumber);
                            } else {
                                out.WriteString(section, key, value);
                                std::free(value);
                            }
                        }
                    }
                }
            }

            while (lineEnd < end && (*lineEnd == '\r' || *lineEnd == '\n')) ++lineEnd;
            p = lineEnd;
        }
        return sectionCount;
    }

    int MergeFileRecursive(CCINIClass* pTarget, const char* path,
                           const char* baseDir, IncludeContext& ctx);

    // Do not read [#include] back from CCINIClass: duplicate keys such as
    // "+=foo.ini" are collapsed by WriteString. Scan the raw text so each
    // include entry keeps its source order and duplicate-key semantics.
    int MergeIncludesRaw(CCINIClass* pTarget, const char* data, int size,
                         const char* currentPath, IncludeContext& ctx)
    {
        char baseDir[kPathMax] = {};
        ParentDirOf(currentPath, baseDir, sizeof(baseDir));
        if (!data || size < 0) return 0;

        int total = 0;
        int includes = 0;
        char section[kIniTokenMax] = {};
        const char* p = data;
        const char* end = data + size;
        if (size >= 3 && static_cast<unsigned char>(p[0]) == 0xEF &&
            static_cast<unsigned char>(p[1]) == 0xBB &&
            static_cast<unsigned char>(p[2]) == 0xBF) {
            p += 3;
        }

        while (p < end) {
            const char* lineEnd = p;
            while (lineEnd < end && *lineEnd != '\r' && *lineEnd != '\n') ++lineEnd;

            const char* begin = p;
            while (begin < lineEnd && (*begin == ' ' || *begin == '\t')) ++begin;
            const char* trimmedEnd = lineEnd;
            while (trimmedEnd > begin &&
                   (trimmedEnd[-1] == ' ' || trimmedEnd[-1] == '\t')) {
                --trimmedEnd;
            }

            if (begin < trimmedEnd && *begin != ';' && *begin != '#') {
                if (*begin == '[' && trimmedEnd[-1] == ']') {
                    CopyTrimmedSpan(begin + 1, trimmedEnd - 1,
                                    section, sizeof(section), false);
                } else if (IsIncludeSection(section)) {
                    const char* equal = begin;
                    while (equal < trimmedEnd && *equal != '=') ++equal;
                    if (equal < trimmedEnd) {
                        char includePath[kPathMax] = {};
                        if (CopyTrimmedSpan(equal + 1, trimmedEnd,
                                            includePath, sizeof(includePath), true) &&
                            includePath[0]) {
                            ++includes;
                            const int keys = MergeFileRecursive(pTarget, includePath,
                                                                baseDir, ctx);
                            if (keys > 0) total += keys;
                        }
                    }
                }
            }

            while (lineEnd < end && (*lineEnd == '\r' || *lineEnd == '\n')) ++lineEnd;
            p = lineEnd;
        }

        if (includes > 0) {
            Log::Info("%s: %s expanded %d include(s), %d key(s)",
                      Tag(ctx.logTag), currentPath, includes, total);
        }
        return total;
    }

    int MergeBody(CCINIClass* pTarget, CCINIClass& src,
                  const char* path, IncludeContext& ctx)
    {
        int keys = 0;
        int sections = 0;
        for (auto* section = src.Sections.First();
             section && section->IsValid(); section = section->Next()) {
            if (!section->Name || !section->Name[0] ||
                IsIncludeSection(section->Name)) continue;
            ++sections;
            for (auto* node = section->Entries.GenericList::First();
                 node && node->IsValid(); node = node->Next()) {
                auto* entry = static_cast<INIClass::INIEntry*>(node);
                if (!entry->Key || !entry->Key[0]) continue;
                pTarget->WriteString(section->Name, entry->Key,
                                     entry->Value ? entry->Value : "");
                ++keys;
            }
        }

        if (ctx.stats) {
            ctx.stats->sections += sections;
            ctx.stats->keys += keys;
        }
        Log::Info("%s: merged %s (%d section(s), %d key(s))",
                  Tag(ctx.logTag), path, sections, keys);
        return keys;
    }

    int MergeFileRecursive(CCINIClass* pTarget, const char* path,
                           const char* baseDir, IncludeContext& ctx)
    {
        if (!pTarget || !path || !path[0]) return -1;
        if (ctx.depth >= kMaxIncludeDepth) {
            RecordError(ctx.stats, "include depth exceeds %d: %s",
                        kMaxIncludeDepth, path);
            return -1;
        }

        char* rawData = nullptr;
        int rawSize = 0;
        char usedPath[kPathMax] = {};
        if (!ReadIniResolved(path, baseDir, &rawData, &rawSize, usedPath, ctx)) {
            Log::Warn("%s: skipped missing/unreadable file %s", Tag(ctx.logTag), path);
            return -1;
        }

        if (IsInIncludeStack(ctx, usedPath)) {
            RecordError(ctx.stats, "include cycle: %s", usedPath);
            Log::Warn("%s: include cycle %s", Tag(ctx.logTag), usedPath);
            std::free(rawData);
            return -1;
        }

        std::snprintf(ctx.stack[ctx.depth++], kPathMax, "%s", usedPath);
        if (ctx.stats) ++ctx.stats->files;

        CCINIClass source;
        ParseRawIni(source, rawData, rawSize, usedPath, ctx);
        int keys = MergeBody(pTarget, source, usedPath, ctx);
        const int includeKeys = MergeIncludesRaw(pTarget, rawData, rawSize,
                                                  usedPath, ctx);
        std::free(rawData);
        --ctx.depth;
        return keys + (includeKeys > 0 ? includeKeys : 0);
    }

}  // namespace

int CountSections(INIClass* pINI)
{
    if (!pINI) return -1;
    int count = 0;
    for (auto* section = pINI->Sections.First();
         section && section->IsValid(); section = section->Next()) {
        ++count;
    }
    return count;
}

int ScanDirectory(const char* dir, const char* wildcard,
                  char files[][kPathMax])
{
    if (!dir || !dir[0] || !wildcard || !wildcard[0] || !files) return -1;
    char pattern[kPathMax] = {};
    const int written = std::snprintf(pattern, sizeof(pattern),
                                      "%s\\%s", dir, wildcard);
    if (written <= 0 || written >= static_cast<int>(sizeof(pattern))) {
        Log::Warn("ini: scan pattern is too long: %s\\%s", dir, wildcard);
        return -1;
    }

    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) return 0;
        Log::Warn("ini: cannot scan %s (error %lu)", pattern, error);
        return -1;
    }

    int count = 0;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (count < kMaxFiles) {
            std::snprintf(files[count], kPathMax, "%s\\%s", dir, data.cFileName);
        }
        ++count;
    } while (FindNextFileA(find, &data));
    FindClose(find);

    if (count > kMaxFiles) {
        Log::Warn("ini: directory %s has %d files; refusing limit %d truncation",
                  dir, count, kMaxFiles);
        return -1;
    }

    std::qsort(files, count, kPathMax,
               [](const void* left, const void* right) {
                   return _stricmp(static_cast<const char*>(left),
                                   static_cast<const char*>(right));
               });
    return count;
}

int MergeFile(CCINIClass* pTarget, const char* path, MergeStats* stats,
              const char* logTag)
{
    IncludeContext context;
    context.stats = stats;
    context.logTag = logTag;
    return MergeFileRecursive(pTarget, path, nullptr, context);
}

bool MergeDirectory(CCINIClass* pTarget, const char* dir,
                    MergeStats* stats, const char* logTag)
{
    if (!pTarget || !dir || !dir[0]) {
        RecordError(stats, "invalid overlay directory");
        return false;
    }

    char files[kMaxFiles][kPathMax] = {};
    const int count = ScanDirectory(dir, "*.ini", files);
    if (count < 0) {
        RecordError(stats, "cannot scan overlay directory: %s", dir);
        return false;
    }
    for (int i = 0; i < count; ++i) {
        MergeFile(pTarget, files[i], stats, logTag);
    }
    return !stats || stats->errors == 0;
}

void Copy(CCINIClass* pTarget, INIClass* pSource, bool copyIncludeSection)
{
    if (!pTarget || !pSource) return;
    for (auto* section = pSource->Sections.First();
         section && section->IsValid(); section = section->Next()) {
        if (!section->Name || !section->Name[0]) continue;
        if (!copyIncludeSection && IsIncludeSection(section->Name)) continue;
        for (auto* node = section->Entries.GenericList::First();
             node && node->IsValid(); node = node->Next()) {
            auto* entry = static_cast<INIClass::INIEntry*>(node);
            if (!entry->Key || !entry->Key[0]) continue;
            pTarget->WriteString(section->Name, entry->Key,
                                 entry->Value ? entry->Value : "");
        }
    }
}

}  // namespace IniOverlay
