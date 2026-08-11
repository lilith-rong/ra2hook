#pragma once

#include <cstddef>

class CCINIClass;
class INIClass;

namespace IniOverlay {

    constexpr int kMaxFiles = 256;
    constexpr int kPathMax = 260;

    struct MergeStats {
        int files = 0;
        int sections = 0;
        int keys = 0;
        int errors = 0;
        char firstError[256] = {};
    };

    int CountSections(INIClass* pINI);

    // Returns the number of sorted matches, 0 for a valid empty directory,
    // and -1 for scan failure or when the file limit would truncate results.
    int ScanDirectory(const char* dir, const char* wildcard,
                      char files[][kPathMax]);

    // Merges a file and its private [#include] tree. The current file body is
    // merged first, followed by include entries in source order.
    int MergeFile(CCINIClass* pTarget, const char* path, MergeStats* stats,
                  const char* logTag);

    // An empty directory is a valid empty overlay. Parse/read/include failures
    // are reported through stats and make the return value false.
    bool MergeDirectory(CCINIClass* pTarget, const char* dir,
                        MergeStats* stats, const char* logTag);

    void Copy(CCINIClass* pTarget, INIClass* pSource,
              bool copyIncludeSection = false);

}  // namespace IniOverlay
