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
    constexpr const char* kAppendKeyPrefix = "__RA2HOOK_APPEND_";
    constexpr const char* kOutputAppendKeyPrefix = "RA2Hook_";

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

    void RecordWarning(MergeStats* stats, const char* format, ...)
    {
        if (!stats) return;
        ++stats->warnings;
        if (stats->firstWarning[0]) return;

        va_list args;
        va_start(args, format);
        std::vsnprintf(stats->firstWarning, sizeof(stats->firstWarning), format, args);
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

    size_t IniWhitespaceWidth(const char* p, const char* end)
    {
        if (!p || p >= end) return 0;
        if (*p == ' ' || *p == '\t') return 1;

        const auto* bytes = reinterpret_cast<const unsigned char*>(p);
        const size_t remaining = static_cast<size_t>(end - p);
        if (remaining >= 3 && bytes[0] == 0xE3 && bytes[1] == 0x80 &&
            bytes[2] == 0x80) {
            return 3;  // UTF-8 U+3000 IDEOGRAPHIC SPACE
        }
        if (remaining >= 2 && bytes[0] == 0xA1 && bytes[1] == 0xA1) {
            return 2;  // GBK full-width space
        }
        return 0;
    }

    bool EndsWithIniWhitespace(const char* begin, const char* end, size_t* width)
    {
        if (!begin || !end || end <= begin || !width) return false;
        if (end[-1] == ' ' || end[-1] == '\t') {
            *width = 1;
            return true;
        }

        const auto* bytes = reinterpret_cast<const unsigned char*>(end);
        if (end - begin >= 3 && bytes[-3] == 0xE3 && bytes[-2] == 0x80 &&
            bytes[-1] == 0x80) {
            *width = 3;
            return true;
        }
        if (end - begin >= 2 && bytes[-2] == 0xA1 && bytes[-1] == 0xA1) {
            *width = 2;
            return true;
        }
        return false;
    }

    bool IsIniCommentMarker(const char* p, const char* end)
    {
        if (!p || p >= end) return false;
        if (*p == ';' || *p == '#') return true;

        const auto* bytes = reinterpret_cast<const unsigned char*>(p);
        const size_t remaining = static_cast<size_t>(end - p);
        return (remaining >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBC &&
                (bytes[2] == 0x9B || bytes[2] == 0x83)) ||
               (remaining >= 2 && bytes[0] == 0xA3 &&
                (bytes[1] == 0xBB || bytes[1] == 0xA3));
    }

    void TrimIniSpan(const char*& begin, const char*& end)
    {
        size_t width = 0;
        while ((width = IniWhitespaceWidth(begin, end)) != 0) begin += width;
        while (EndsWithIniWhitespace(begin, end, &width)) end -= width;
    }

    bool CopyTrimmedSpan(const char* begin, const char* end,
                         char* dst, size_t dstSize, bool stripQuotes)
    {
        if (!dst || dstSize == 0) return false;
        dst[0] = '\0';
        if (!begin || !end || end < begin) return false;

        while (end > begin && (end[-1] == '\r' || end[-1] == '\n')) --end;
        TrimIniSpan(begin, end);

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
        while (end > begin && (end[-1] == '\r' || end[-1] == '\n')) --end;
        TrimIniSpan(begin, end);

        const size_t len = static_cast<size_t>(end - begin);
        char* value = static_cast<char*>(std::malloc(len + 1));
        if (!value) return nullptr;
        std::memcpy(value, begin, len);
        value[len] = '\0';
        return value;
    }

    bool ParseSectionHeader(const char* begin, const char* end,
                            char* section, size_t sectionSize)
    {
        if (!begin || !end || begin >= end || *begin != '[' ||
            !section || sectionSize == 0) {
            return false;
        }

        const char* close = begin + 1;
        while (close < end && *close != ']') ++close;
        if (close >= end) return false;

        const char* trailing = close + 1;
        size_t width = 0;
        while ((width = IniWhitespaceWidth(trailing, end)) != 0) trailing += width;
        if (trailing < end && !IsIniCommentMarker(trailing, end)) return false;

        return CopyTrimmedSpan(begin + 1, close, section, sectionSize, false) &&
               section[0];
    }

    bool IsAppendMarker(const char* key)
    {
        return key && _stricmp(key, "+") == 0;
    }

    bool IsSyntheticAppendKey(const char* key)
    {
        return key && _strnicmp(key, kAppendKeyPrefix,
                                std::strlen(kAppendKeyPrefix)) == 0;
    }

    bool ReadAppendIndex(const char* key, unsigned int* result)
    {
        const size_t prefixLength = std::strlen(kOutputAppendKeyPrefix);
        if (!key || !result || _strnicmp(key, kOutputAppendKeyPrefix,
                                          prefixLength) != 0 ||
            !key[prefixLength]) {
            return false;
        }

        unsigned int value = 0;
        for (const char* p = key + prefixLength; *p; ++p) {
            if (*p < '0' || *p > '9') return false;
            const unsigned int digit = static_cast<unsigned int>(*p - '0');
            if (value > 0xFFFFFFFFu / 10u ||
                (value * 10u) > 0xFFFFFFFFu - digit) {
                return false;
            }
            value = value * 10u + digit;
        }
        *result = value;
        return true;
    }

    unsigned int NextAppendIndex(INIClass* target)
    {
        if (!target) return 0;
        unsigned int next = 0;
        bool found = false;
        for (auto* section = target->Sections.First();
             section && section->IsValid(); section = section->Next()) {
            for (auto* node = section->Entries.GenericList::First();
                 node && node->IsValid(); node = node->Next()) {
                auto* entry = static_cast<INIClass::INIEntry*>(node);
                unsigned int value = 0;
                if (!entry->Key || !ReadAppendIndex(entry->Key, &value)) continue;
                if (!found || value >= next) {
                    if (value == 0xFFFFFFFFu) return 0xFFFFFFFFu;
                    next = value + 1;
                    found = true;
                }
            }
        }
        return found ? next : 0;
    }

    bool WriteAppend(INIClass* target, const char* sectionName, const char* value)
    {
        if (!target || !sectionName || !sectionName[0] || !value) return false;

        // The key is only an identity for INIClass; the type-list loader
        // consumes its value. Keep a private namespace so Ares and Phobos
        // generated keys are neither scanned nor reused by ra2hook.
        const unsigned int index = NextAppendIndex(target);
        if (index == 0xFFFFFFFFu) return false;
        char key[64] = {};
        std::snprintf(key, sizeof(key), "%s%u", kOutputAppendKeyPrefix, index);
        return target->WriteString(sectionName, key, value);
    }

    char* DuplicateAppendValue(const char* begin, const char* end)
    {
        if (!begin || !end || end < begin) return nullptr;

        const char* valueEnd = end;
        for (const char* p = begin; p < end; ++p) {
            if (IsIniCommentMarker(p, end)) {
                valueEnd = p;
                break;
            }
        }
        return DuplicateTrimmedSpan(begin, valueEnd);
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
        int appendId = 0;
        bool preserveAppendKeys = false;
        MergeStats* stats = nullptr;
        const char* logTag = nullptr;
    };

    void RecordResolvableIncludeWarning(IncludeContext& ctx,
                                         const char* format, ...)
    {
        char message[256] = {};
        va_list args;
        va_start(args, format);
        std::vsnprintf(message, sizeof(message), format, args);
        va_end(args);

        // depth is non-zero while resolving a child referenced by [#include].
        // A missing child must not discard valid siblings in the same manifest.
        if (ctx.depth > 0) {
            RecordWarning(ctx.stats, "%s", message);
        } else {
            RecordError(ctx.stats, "%s", message);
        }
    }

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
            RecordResolvableIncludeWarning(ctx, "path too long: %s", request);
            return false;
        }
        NormalizeSlashes(name);
        if (!name[0]) {
            RecordResolvableIncludeWarning(ctx, "empty include path");
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

        RecordResolvableIncludeWarning(ctx, "file not found: %s", name);
        return false;
    }

    bool ParseRawIni(CCINIClass& out, const char* data, int size,
                     const char* path, IncludeContext& ctx)
    {
        if (!data || size < 0) return false;

        char section[kIniTokenMax] = {};
        bool valid = true;
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
            const char* trimmedEnd = lineEnd;
            TrimIniSpan(begin, trimmedEnd);

            if (begin < trimmedEnd && !IsIniCommentMarker(begin, trimmedEnd)) {
                if (*begin == '[') {
                    if (!ParseSectionHeader(begin, trimmedEnd,
                                            section, sizeof(section))) {
                        section[0] = '\0';
                        RecordWarning(ctx.stats, "%s:%d ignored invalid section",
                                      path, lineNumber);
                        Log::Warn("%s: ignored invalid section %s:%d",
                                  Tag(ctx.logTag), path, lineNumber);
                    }
                } else {
                    const char* equal = begin;
                    while (equal < trimmedEnd && *equal != '=') ++equal;
                    if (equal >= trimmedEnd || !section[0]) {
                        RecordWarning(ctx.stats,
                                      "%s:%d ignored text outside section or missing '='",
                                      path, lineNumber);
                        Log::Warn("%s: ignored text outside section or missing '=' %s:%d",
                                  Tag(ctx.logTag), path, lineNumber);
                    } else {
                        char key[kIniTokenMax] = {};
                        if (!CopyTrimmedSpan(begin, equal, key, sizeof(key), false) || !key[0]) {
                            RecordWarning(ctx.stats, "%s:%d ignored invalid key",
                                          path, lineNumber);
                            Log::Warn("%s: ignored invalid key %s:%d",
                                      Tag(ctx.logTag), path, lineNumber);
                        } else {
                            const bool append = IsAppendMarker(key) &&
                                                 !IsIncludeSection(section);
                            char* value = append
                                ? DuplicateAppendValue(equal + 1, trimmedEnd)
                                : DuplicateTrimmedSpan(equal + 1, trimmedEnd);
                            if (!value) {
                                RecordError(ctx.stats, "%s:%d out of memory", path, lineNumber);
                                valid = false;
                            } else {
                                if (append) {
                                    char syntheticKey[64] = {};
                                    std::snprintf(syntheticKey, sizeof(syntheticKey),
                                                  "%s%d", kAppendKeyPrefix,
                                                  ++ctx.appendId);
                                    out.WriteString(section, syntheticKey, value);
                                } else {
                                    out.WriteString(section, key, value);
                                }
                                std::free(value);
                            }
                        }
                    }
                }
            }

            if (lineEnd < end && *lineEnd == '\r') ++lineEnd;
            if (lineEnd < end && *lineEnd == '\n') ++lineEnd;
            p = lineEnd;
        }
        return valid;
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
            const char* trimmedEnd = lineEnd;
            TrimIniSpan(begin, trimmedEnd);

            if (begin < trimmedEnd && !IsIniCommentMarker(begin, trimmedEnd)) {
                if (*begin == '[') {
                    if (!ParseSectionHeader(begin, trimmedEnd,
                                            section, sizeof(section))) {
                        section[0] = '\0';
                    }
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

            if (lineEnd < end && *lineEnd == '\r') ++lineEnd;
            if (lineEnd < end && *lineEnd == '\n') ++lineEnd;
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
                const char* value = entry->Value ? entry->Value : "";
                if (IsSyntheticAppendKey(entry->Key)) {
                    if (ctx.stats) ++ctx.stats->appends;
                    // Keep the marker while building a staging INI. A direct
                    // MergeFile call targets the live object and can append now.
                    if (ctx.preserveAppendKeys) {
                        pTarget->WriteString(section->Name, entry->Key, value);
                    } else if (!WriteAppend(pTarget, section->Name, value)) {
                        continue;
                    }
                } else {
                    pTarget->WriteString(section->Name, entry->Key, value);
                }
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
            RecordWarning(ctx.stats, "include depth exceeds %d: %s",
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
            RecordWarning(ctx.stats, "include cycle: %s", usedPath);
            Log::Warn("%s: include cycle %s", Tag(ctx.logTag), usedPath);
            std::free(rawData);
            return -1;
        }

        std::snprintf(ctx.stack[ctx.depth++], kPathMax, "%s", usedPath);
        if (ctx.stats) ++ctx.stats->files;

        CCINIClass source;
        const bool valid = ParseRawIni(source, rawData, rawSize, usedPath, ctx);
        int keys = valid ? MergeBody(pTarget, source, usedPath, ctx) : 0;
        if (!valid) {
            Log::Warn("%s: skipped malformed file body %s",
                      Tag(ctx.logTag), usedPath);
        }
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
              const char* logTag, bool preserveAppendKeys)
{
    IncludeContext context;
    context.preserveAppendKeys = preserveAppendKeys;
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
    IncludeContext context;
    context.preserveAppendKeys = true;
    context.stats = stats;
    context.logTag = logTag;
    for (int i = 0; i < count; ++i) {
        MergeFileRecursive(pTarget, files[i], nullptr, context);
    }
    return !stats || stats->errors == 0;
}

void Copy(CCINIClass* pTarget, INIClass* pSource, bool copyIncludeSection,
          bool applyAppendEntries)
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
            const char* value = entry->Value ? entry->Value : "";
            if (applyAppendEntries && IsSyntheticAppendKey(entry->Key)) {
                WriteAppend(pTarget, section->Name, value);
            } else {
                pTarget->WriteString(section->Name, entry->Key, value);
            }
        }
    }
}

}  // namespace IniOverlay
