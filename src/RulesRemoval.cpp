#include <CCINIClass.h>
#include <CCFileClass.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "RulesRemoval.h"
#include "IniRemoval.h"
#include "IniOverlay.h"
#include "GamePaths.h"
#include "Logger.h"

namespace RulesRemoval {
namespace {
    bool ReadSource(const std::string& request, const std::string& containingFile,
                    IniRemoval::Source& source, std::string& error, void*)
    {
        std::vector<std::string> candidates;
        const auto add = [&](const std::string& path) {
            const std::string normalized = IniRemoval::NormalizePath(path);
            for (const auto& candidate : candidates) {
                if (IniRemoval::EqualName(candidate, normalized)) return;
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
            if (path.size() >= IniRemoval::kMaxPath) {
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
            if (size < 0 || static_cast<std::size_t>(size) > IniRemoval::kMaxFileBytes) {
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
            source.path = IniRemoval::NormalizePath(path);
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
} // namespace

bool Apply(CCINIClass* rules)
{
    if (!rules) return false;
    char directory[IniOverlay::kPathMax] = {};
    if (!GamePaths::Build(directory, sizeof(directory), "ra2hook\\inject\\remove\\rules")) {
        Log::Warn("remove: cannot resolve remove/rules; no keys removed");
        return false;
    }
    char files[IniOverlay::kMaxFiles][IniOverlay::kPathMax] = {};
    const int count = IniOverlay::ScanDirectory(directory, "*.ini", files);
    if (count < 0) {
        Log::Warn("remove: cannot completely scan %s; no keys removed", directory);
        return false;
    }
    if (!count) {
        Log::Info("remove: %s is absent/empty; no keys removed", directory);
        return true;
    }
    std::vector<std::string> roots;
    for (int i = 0; i < count; ++i) roots.emplace_back(files[i]);

    IniRemoval::Plan plan;
    IniRemoval::Error error;
    if (!IniRemoval::BuildPlan(roots, ReadSource, nullptr, plan, error)) {
        Log::Warn("remove: rejected entire layer at %s:%zu: %s; no keys removed; set writes retained",
                  error.file.c_str(), error.line, error.message.c_str());
        return false;
    }

    // No engine state has been changed before this point. Never pass a remove
    // manifest to IniOverlay::Copy/WriteString (all its keys would be "-").
    std::size_t removed = 0;
    std::size_t skipped = 0;
    for (const auto& command : plan.commands) {
        auto* section = FindSection(rules, command.section.c_str());
        auto* entry = FindEntry(section, command.key.c_str());
        if (!entry) {
            ++skipped;
            Log::Debug("remove: skip absent [%s]%s (%s:%zu)",
                       command.section.c_str(), command.key.c_str(),
                       command.file.c_str(), command.line);
            continue;
        }

        // Native INIClass::Clear @0x5257C0, verified by disassembly against
        // gamemd MD5 56d582a1d6f3c144d3adc867d7a4d91b:
        //   null section => clear everything; null key => clear whole section.
        //   BOTH non-null => remove EntryIndex item + destroy/unlink that entry.
        // It also resets CurrentSectionName/CurrentSection (offsets 4/8).
        // Use the names stored in the lists, not the request's spelling: native
        // indices use the stored-name CRC. Never use ReadString/GetKeyCount to
        // test existence: missing sections can fall back to CurrentSection.
        // Key and section are nonempty here and the section survives key erase.
        rules->Clear(section->Name, entry->Key);
        if (FindEntry(FindSection(rules, command.section.c_str()), command.key.c_str())) {
            Log::Warn("remove: native erase failed for [%s]%s (%s:%zu); stopped after %zu removals",
                      command.section.c_str(), command.key.c_str(),
                      command.file.c_str(), command.line, removed);
            return false;
        }
        ++removed;
        Log::Info("remove: erased [%s]%s (%s:%zu)", command.section.c_str(),
                  command.key.c_str(), command.file.c_str(), command.line);
    }
    Log::Info("remove: rules complete (%zu files, %zu requests, %zu removed, %zu absent/duplicate); defaults untouched",
              plan.files, plan.commands.size(), removed, skipped);
    return true;
}
} // namespace RulesRemoval
