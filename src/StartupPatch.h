#pragma once

#include "IniPatch.h"

class CCINIClass;

// Engine adapter for startup patches only. Prepare does file I/O and validates
// every file; an invalid/missing file is dropped alone and logged. Apply performs only ordered in-memory mutations (sound
// deliberately calls these from two separate, already established hooks).
namespace StartupPatch {
    struct Stats {
        std::size_t writes = 0;
        std::size_t appends = 0;
        std::size_t removals = 0;
        std::size_t absent = 0;
        std::size_t skipped = 0; // Files dropped during Prepare (see log).
    };

    bool Prepare(const char* directory, bool allowRemoval, IniPatch::Plan& plan);
    bool Apply(CCINIClass* target, const IniPatch::Plan& plan, Stats& stats);
    bool ApplyDirectory(CCINIClass* target, const char* directory,
                        bool allowRemoval, Stats& stats);
} // namespace StartupPatch
