#pragma once

#include "windows.h"
#include "IniPatch.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

class GenericNode {
public:
    virtual ~GenericNode() = default;
    GenericNode* next = nullptr;
    bool IsValid() const { return true; }
    GenericNode* Next() const { return next; }
};
class GenericList {
public:
    GenericNode* first = nullptr;
    GenericNode* First() const { return first; }
};

class INIClass {
public:
    struct INIEntry : GenericNode {
        std::string keyStorage;
        std::string valueStorage;
        char* Key;
        char* Value;
        INIEntry(std::string key, std::string value)
            : keyStorage(std::move(key)), valueStorage(std::move(value)),
              Key(keyStorage.data()), Value(valueStorage.data()) {}
    };
    struct INISection {
        std::string nameStorage;
        char* Name;
        GenericList Entries;
        INISection* next = nullptr;
        std::vector<std::unique_ptr<INIEntry>> owned;
        explicit INISection(std::string name)
            : nameStorage(std::move(name)), Name(nameStorage.data()) {}
        bool IsValid() const { return true; }
        INISection* Next() const { return next; }
    };
    struct SectionList {
        INISection* first = nullptr;
        INISection* First() const { return first; }
    } Sections;
    std::vector<std::unique_ptr<INISection>> owned;
    std::vector<std::pair<std::string, std::string>> clearCalls;
    std::vector<std::string> mutations;
    bool failWrite = false;
    bool wrongCaseWrite = false;
    INISection* CurrentSection = nullptr;
    bool unsafeClear = false;
    bool failClear = false;

    INISection* Section(const std::string& name) const
    {
        for (const auto& section : owned)
            if (IniPatch::EqualName(section->Name, name)) return section.get();
        return nullptr;
    }
    INIEntry* Entry(const std::string& section, const std::string& key) const
    {
        const auto* target = Section(section);
        if (target) {
            for (const auto& entry : target->owned)
                if (IniPatch::EqualName(entry->Key, key)) return entry.get();
        }
        return nullptr;
    }
    void Add(const std::string& sectionName, const std::string& key, const std::string& value)
    {
        auto* section = Section(sectionName);
        if (!section) {
            auto item = std::make_unique<INISection>(sectionName);
            section = item.get();
            if (!owned.empty()) owned.back()->next = section;
            else Sections.first = section;
            owned.push_back(std::move(item));
        }
        auto entry = std::make_unique<INIEntry>(key, value);
        if (!section->owned.empty()) section->owned.back()->next = entry.get();
        else section->Entries.first = entry.get();
        section->owned.push_back(std::move(entry));
    }
    bool WriteString(const char* sectionName, const char* key, const char* value)
    {
        if (!sectionName || !key || !value || !sectionName[0] || !key[0]) return false;
        mutations.push_back(std::string("S:") + sectionName + ":" + key + "=" + value);
        if (failWrite) return false;
        auto* section = Section(sectionName);
        auto* entry = Entry(sectionName, key);
        // Production must preserve stored names to avoid case-sensitive native
        // CRC lookup accidentally creating a second spelling of the same key.
        if ((section && std::strcmp(section->Name, sectionName)) ||
            (entry && std::strcmp(entry->Key, key))) {
            wrongCaseWrite = true;
            return false;
        }
        if (entry) {
            entry->valueStorage = value;
            entry->Value = entry->valueStorage.data();
        } else Add(sectionName, key, value);
        return true;
    }
    void Clear(const char* sectionName, char* key)
    {
        if (!sectionName || !sectionName[0] || !key || !key[0]) {
            unsafeClear = true;
            return;
        }
        clearCalls.emplace_back(sectionName, key);
        mutations.push_back(std::string("R:") + sectionName + ":" + key);
        CurrentSection = nullptr;
        if (failClear) return;
        auto* section = Section(sectionName);
        // Model stored-spelling CRC lookup: a differently cased argument must
        // fail, so these tests detect passing request spelling to native Clear.
        if (!section || std::strcmp(sectionName, section->Name)) return;
        for (std::size_t i = 0; i < section->owned.size(); ++i) {
            if (std::strcmp(section->owned[i]->Key, key)) continue;
            auto* next = section->owned[i]->next;
            if (i) section->owned[i - 1]->next = next;
            else section->Entries.first = next;
            section->owned.erase(section->owned.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
};
class CCINIClass : public INIClass {};
