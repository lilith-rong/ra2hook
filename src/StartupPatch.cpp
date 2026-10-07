#include <CCINIClass.h>
#include <CCFileClass.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "StartupPatch.h"
#include "IniPatch.h"
#include "IniOverlay.h"
#include "GamePaths.h"
#include "Logger.h"

namespace StartupPatch {
namespace {
    bool ReadSource(const std::string& request, const std::string& containingFile,
                    IniPatch::Source& source, std::string& error, void*)
    {
        std::vector<std::string> candidates;
        const auto add = [&](const std::string& path) {
            const std::string normalized = IniPatch::NormalizePath(path);
            for (const auto& candidate : candidates) {
                if (IniPatch::EqualName(candidate, normalized)) return;
            }
            candidates.push_back(normalized);
        };
        // Also recognize drive-relative/root-relative forms so they are never
        // silently reinterpreted as paths beneath an including file.
        const bool rooted = !request.empty() &&
            (request[0] == '/' || request[0] == '\\' ||
             (request.size() > 1 && request[1] == ':'));
        if (rooted) add(request);
        else {
            const auto slash = containingFile.find_last_of("/\\");
            if (slash != std::string::npos)
                add(containingFile.substr(0, slash + 1) + request);
            char gamePath[IniOverlay::kPathMax] = {};
            if (!GamePaths::Build(gamePath, sizeof(gamePath), request.c_str())) {
                error = "cannot resolve path against game executable directory";
                return false;
            }
            add(gamePath);
            // Bare names may resolve inside already registered MIX archives.
            add(request);
        }

        for (auto path : candidates) {
            if (path.size() >= IniPatch::kMaxPath) {
                error = "resolved path exceeds 259 bytes: " + path;
                return false;
            }
            for (char& c : path) if (c == '/') c = '\\';
            CCFileClass file(path.c_str());
            if (!file.Exists()) continue;
            if (!file.Open(FileAccessMode::Read)) {
                error = "cannot open " + path;
                return false;
            }
            const int size = file.GetFileSize();
            if (size < 0 || static_cast<std::size_t>(size) > IniPatch::kMaxFileBytes) {
                file.Close();
                error = "invalid or oversized file: " + path;
                return false;
            }
            source.text.resize(static_cast<std::size_t>(size));
            const int read = size ? file.ReadBytes(source.text.data(), size) : 0;
            file.Close();
            if (read != size) {
                error = "short read: " + path;
                return false;
            }
            source.path = IniPatch::NormalizePath(path);
            return true;
        }
        error = "file not found (relative, game directory and engine/MIX lookup)";
        return false;
    }

    INIClass::INISection* FindSection(INIClass* ini, const char* name)
    {
        for (auto* section = ini->Sections.First();
             section && section->IsValid(); section = section->Next()) {
            if (section->Name && !_stricmp(section->Name, name)) return section;
        }
        return nullptr;
    }

    INIClass::INIEntry* FindEntry(INIClass::INISection* section, const char* key)
    {
        if (!section) return nullptr;
        for (auto* node = section->Entries.GenericList::First();
             node && node->IsValid(); node = node->Next()) {
            auto* entry = static_cast<INIClass::INIEntry*>(node);
            if (entry->Key && !_stricmp(entry->Key, key)) return entry;
        }
        return nullptr;
    }
    // Keep the established RA2Hook_N namespace, separate from Ares/Phobos.
    // Scan the live object at this instruction: explicit preceding assignments
    // to RA2Hook_N must also be respected, and repeated += must never collapse.
    bool WriteAppend(CCINIClass* target, const std::string& section, const std::string& value)
    {
        constexpr const char* prefix = "RA2Hook_";
        constexpr std::size_t prefixLength = 8;
        unsigned int next = 0;
        for (auto* item = target->Sections.First();
             item && item->IsValid(); item = item->Next()) {
            for (auto* node = item->Entries.GenericList::First();
                 node && node->IsValid(); node = node->Next()) {
                auto* entry = static_cast<INIClass::INIEntry*>(node);
                if (!entry->Key || _strnicmp(entry->Key, prefix, prefixLength) ||
                    !entry->Key[prefixLength]) continue;
                unsigned int index = 0;
                bool valid = true;
                for (const char* p = entry->Key + prefixLength; *p; ++p) {
                    if (*p < '0' || *p > '9') { valid = false; break; }
                    const unsigned int digit = static_cast<unsigned int>(*p - '0');
                    if (index > (0xFFFFFFFFu - digit) / 10u) { valid = false; break; }
                    index = index * 10u + digit;
                }
                if (!valid) continue;
                if (index == 0xFFFFFFFFu) return false;
                if (index >= next) next = index + 1;
            }
        }
        if (next == 0xFFFFFFFFu) return false;
        char key[64] = {};
        std::snprintf(key, sizeof(key), "%s%u", prefix, next);
        return target->WriteString(section.c_str(), key, value.c_str());
    }
} // namespace

bool Prepare(const char* directory, bool allowRemoval, IniPatch::Plan& plan)
{
    plan = {};
    if (!directory || !directory[0]) {
        Log::Warn("patch: invalid directory; no changes applied");
        return false;
    }
    char files[IniOverlay::kMaxFiles][IniOverlay::kPathMax] = {};
    const int count = IniOverlay::ScanDirectory(directory, "*.ini", files);
    if (count < 0) {
        Log::Warn("patch: cannot completely scan %s; no changes applied", directory);
        return false;
    }
    std::vector<std::string> roots;
    for (int i = 0; i < count; ++i) roots.emplace_back(files[i]);

    IniPatch::Error error;
    if (!IniPatch::BuildPlan(roots, ReadSource, nullptr, plan, error, allowRemoval)) {
        Log::Warn("patch: rejected entire target %s at %s:%zu: %s; no changes applied",
                  directory, error.file.c_str(), error.line, error.message.c_str());
        return false;
    }
    Log::Info("patch: prepared %s (%zu files, %zu ordered instructions)",
              directory, plan.files, plan.commands.size());
    return true;
}

bool Apply(CCINIClass* target, const IniPatch::Plan& plan, Stats& stats)
{
    stats = {};
    if (!target) return false;
    // Plans normally come only from Prepare. Still reject unsafe manually
    // constructed plans in full before the first write, especially null-key
    // Clear (whole section) and registry deletions. Never accept include data.
    for (const auto& command : plan.commands) {
        bool valid = !command.section.empty() &&
                     !IniPatch::EqualName(command.section, "#include");
        switch (command.operation) {
        case IniPatch::Operation::Set:
            valid = valid && !command.key.empty() &&
                    command.key != "+" && command.key != "-";
            break;
        case IniPatch::Operation::Append:
            valid = valid && !command.value.empty();
            break;
        case IniPatch::Operation::Remove:
            valid = valid && plan.allowRemoval &&
                    IniPatch::IsRemovalTarget(command.section, command.key);
            break;
        default:
            valid = false;
            break;
        }
        if (!valid) {
            Log::Warn("patch: invalid plan at %s:%zu; no changes applied",
                      command.file.c_str(), command.line);
            return false;
        }
    }

    for (const auto& command : plan.commands) {
        auto* section = FindSection(target, command.section.c_str());
        auto* entry = FindEntry(section, command.key.c_str());
        // Copy stored spelling before WriteString, which may replace an entry.
        const std::string sectionName = section ? section->Name : command.section;
        const std::string keyName = entry ? entry->Key : command.key;
        bool success = true;
        switch (command.operation) {
        case IniPatch::Operation::Set:
            success = target->WriteString(sectionName.c_str(), keyName.c_str(),
                                          command.value.c_str());
            if (success) ++stats.writes;
            break;
        case IniPatch::Operation::Append:
            success = WriteAppend(target, sectionName, command.value);
            if (success) ++stats.appends;
            break;
        case IniPatch::Operation::Remove:
            if (!entry) {
                ++stats.absent;
                Log::Debug("patch: skip absent [%s]%s (%s:%zu)",
                           command.section.c_str(), command.key.c_str(),
                           command.file.c_str(), command.line);
                break;
            }
            // Native Clear @0x5257C0 verified against gamemd MD5
            // 56d582a1d6f3c144d3adc867d7a4d91b: null section clears everything;
            // null key clears the section. BOTH non-null delete just the entry
            // and maintain native indices and CurrentSection caches.
            // Use stored spelling (native CRC), never fallback-prone ReadString.
            target->Clear(section->Name, entry->Key);
            success = !FindEntry(FindSection(target, command.section.c_str()),
                                 command.key.c_str());
            if (success) {
                ++stats.removals;
                Log::Info("patch: erased [%s]%s (%s:%zu)",
                          command.section.c_str(), command.key.c_str(),
                          command.file.c_str(), command.line);
            }
            break;
        default: // Rejected in the preflight above.
            success = false;
            break;
        }
        if (!success) {
            Log::Warn("patch: native instruction failed at %s:%zu [%s]%s; stopped "
                      "after %zu writes, %zu appends, %zu removals (no rollback)",
                      command.file.c_str(), command.line, command.section.c_str(),
                      command.key.c_str(), stats.writes, stats.appends, stats.removals);
            return false;
        }
    }
    Log::Info("patch: applied %zu writes, %zu appends, %zu removals, %zu absent; defaults untouched",
              stats.writes, stats.appends, stats.removals, stats.absent);
    return true;
}

bool ApplyDirectory(CCINIClass* target, const char* directory,
                    bool allowRemoval, Stats& stats)
{
    stats = {};
    if (!target) return false;
    IniPatch::Plan plan;
    return Prepare(directory, allowRemoval, plan) && Apply(target, plan, stats);
}
} // namespace StartupPatch
