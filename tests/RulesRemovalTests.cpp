#include "RulesRemoval.h"
#include "IniOverlay.h"
#include "Logger.h"
#include <CCINIClass.h>
#include <CCFileClass.h>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Only the scan and external engine APIs are faked. RulesRemoval.cpp (including
// its file resolution, parse-before-mutate gate, exact lookups and native Clear
// call site) is compiled unchanged into this executable.
namespace IniOverlay {
int ScanDirectory(const char* directory, const char*, char files[][kPathMax])
{
    FakeFiles::scannedDirectory = directory;
    if (FakeFiles::scanFails) return -1;
    auto roots = FakeFiles::roots;
    std::sort(roots.begin(), roots.end(), [](const auto& left, const auto& right) {
        return FakeFiles::Identity(left) < FakeFiles::Identity(right);
    });
    for (std::size_t i = 0; i < roots.size(); ++i)
        std::snprintf(files[i], kPathMax, "%s", roots[i].c_str());
    return static_cast<int>(roots.size());
}
}

namespace {
const std::string root = "C:/game/ra2hook/inject/remove/rules/10-main.ini";

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void Manifest(const std::string& text)
{
    FakeFiles::Reset();
    FakeFiles::roots.push_back(root);
    FakeFiles::Add(root, text);
}
void ExactDeletion()
{
    Manifest("[htnk]\n-=prerequisite\n-=factoryowners\n-=Empty\n"
             "-=PREREQUISITE\n[Missing]\n-=Strength\n");
    CCINIClass rules;
    rules.Add("HtNk", "Prerequisite", "set-final");
    rules.Add("HtNk", "FactoryOwners", "YuriCountry");
    rules.Add("HtNk", "Empty", "");
    rules.Add("HtNk", "Prerequisite.X", "keep");
    rules.Add("HtNk", "Strength", "999");
    rules.Add("Other", "Prerequisite", "untouched");
    rules.CurrentSection = rules.Section("HtNk");
    const auto originalManifest = FakeFiles::files.at(FakeFiles::Identity(root)).text;
    Require(RulesRemoval::Apply(&rules), "valid plan failed");
    Require(!rules.Entry("HTNK", "Prerequisite"), "explicit value was not removed");
    Require(!rules.Entry("HTNK", "FactoryOwners"), "second -= was lost");
    Require(!rules.Entry("HTNK", "Empty"), "explicit empty value was not removed");
    Require(rules.Entry("HTNK", "Prerequisite.X"), "prefix key was removed");
    Require(rules.Entry("HTNK", "Strength")->valueStorage == "999", "unrelated field changed");
    Require(rules.Entry("Other", "Prerequisite"), "other section was modified");
    Require(!rules.Section("Missing"), "absent section was created");
    Require(rules.clearCalls.size() == 3, "absent/duplicate requests called Clear");
    Require(rules.clearCalls[0] == std::make_pair(std::string("HtNk"), std::string("Prerequisite")),
            "Clear did not receive stored-case names");
    Require(!rules.unsafeClear, "null/empty argument reached Clear");
    Require(rules.CurrentSection == nullptr, "native cache reset was not observed");
    Require(FakeFiles::files.at(FakeFiles::Identity(root)).text == originalManifest, "source modified");
}
void KeepEmptySection()
{
    Manifest("[A]\n-=Only\n[A]\n-=Only\n");
    CCINIClass rules;
    rules.Add("A", "Only", "value");
    Require(RulesRemoval::Apply(&rules), "single-key removal failed");
    Require(rules.Section("A") && !rules.Section("A")->Entries.First(), "empty section deleted");
    Require(rules.clearCalls.size() == 1 && !rules.unsafeClear, "duplicate was not a no-op");
}
void NoPartialDeletion()
{
    const std::vector<std::string> invalidChildren = {
        "[A]\n+=Second\n", "[A]\nSecond=no\n", "[A]\n-=\n", "[A]\n-=*\n",
        "[VehicleTypes]\n-=1\n", "[A\n-=Second\n"
    };
    for (const auto& invalid : invalidChildren) {
        Manifest("[A]\n-=First\n[#include]\n+=bad.ini\n");
        FakeFiles::Add("C:/game/ra2hook/inject/remove/rules/bad.ini", invalid);
        CCINIClass rules;
        rules.Add("A", "First", "set-value");
        rules.Add("A", "Second", "old-value");
        Require(!RulesRemoval::Apply(&rules), "invalid include accepted");
        Require(rules.clearCalls.empty(), "partial deletion before include validation");
        Require(rules.Entry("A", "First")->valueStorage == "set-value", "set writes rolled back/changed");
    }
    Manifest("[A]\n-=First\n");
    const std::string badRoot = "C:/game/ra2hook/inject/remove/rules/99-bad.ini";
    FakeFiles::roots.push_back(badRoot);
    FakeFiles::Add(badRoot, "[A]\nFoo=no\n");
    CCINIClass rules;
    rules.Add("A", "First", "set-value");
    Require(!RulesRemoval::Apply(&rules) && rules.clearCalls.empty(), "later root caused partial deletion");
}
void ReadFailures()
{
    for (int mode = 0; mode < 4; ++mode) {
        Manifest("[A]\n-=First\n[#include]\n+=bad.ini\n");
        const std::string child = "C:/game/ra2hook/inject/remove/rules/bad.ini";
        if (mode) {
            FakeFiles::Add(child, "[A]\n-=Second\n");
            auto& file = FakeFiles::files.at(FakeFiles::Identity(child));
            if (mode == 1) file.openable = false;
            if (mode == 2) file.shortRead = true;
            if (mode == 3) file.reportedSize = static_cast<int>(IniRemoval::kMaxFileBytes + 1);
            // A bad existing local file must not silently fall back to this.
            FakeFiles::Add("bad.ini", "[A]\n-=First\n");
        }
        CCINIClass rules;
        rules.Add("A", "First", "keep");
        Require(!RulesRemoval::Apply(&rules), "read failure accepted");
        Require(rules.clearCalls.empty(), "read failure caused deletion");
    }
}
void ResolutionAndMix()
{
    Manifest("[#include]\n+=local.ini\n+=game.ini\n+=mix.ini\n[A]\n-=Root\n");
    FakeFiles::Add("C:/game/ra2hook/inject/remove/rules/local.ini", "[A]\n-=Local\n");
    FakeFiles::Add("C:/game/local.ini", "[A]\n-=MustKeep\n");
    FakeFiles::Add("local.ini", "[A]\n-=MustKeep\n");
    FakeFiles::Add("C:/game/game.ini", "[A]\n-=Game\n");
    FakeFiles::Add("game.ini", "[A]\n-=MustKeep\n");
    FakeFiles::Add("mix.ini", "[#include]\n+=nested.ini\n[A]\n-=Mix\n");
    FakeFiles::Add("nested.ini", "[A]\n-=Nested\n");
    CCINIClass rules;
    for (const char* key : { "Root", "Local", "Game", "Mix", "Nested", "MustKeep" })
        rules.Add("A", key, "value");
    Require(RulesRemoval::Apply(&rules), "relative/game/MIX include resolution failed");
    Require(rules.clearCalls.size() == 5, "some resolved commands were lost");
    Require(rules.Entry("A", "MustKeep"), "lower-priority include won");
    const std::vector<std::string> expected = { "Root", "Local", "Game", "Mix", "Nested" };
    for (std::size_t i = 0; i < expected.size(); ++i)
        Require(rules.clearCalls[i].second == expected[i], "body/include order wrong");
}
void CyclesAndAbsentTargets()
{
    Manifest("[A]\n-=First\n[#include]\n+=.\\..\\rules\\10-MAIN.ini\n");
    CCINIClass rules;
    rules.Add("A", "First", "keep");
    Require(!RulesRemoval::Apply(&rules) && rules.clearCalls.empty(), "canonical cycle caused deletion");
    Manifest("[Missing]\n-=First\n[A]\n-=Missing\n");
    rules.CurrentSection = rules.Section("A");
    Require(RulesRemoval::Apply(&rules), "missing target is not a valid no-op");
    Require(rules.clearCalls.empty() && rules.Entry("A", "First"), "missing target fell back to current section");
}
void EmptyScanAndNativeFailure()
{
    FakeFiles::Reset();
    CCINIClass rules;
    rules.Add("A", "First", "keep");
    Require(RulesRemoval::Apply(&rules), "absent/empty directory failed");
    Require(FakeFiles::Identity(FakeFiles::scannedDirectory) == "c:/game/ra2hook/inject/remove/rules",
            "directory is not anchored to game EXE");
    FakeFiles::scanFails = true;
    Require(!RulesRemoval::Apply(&rules) && rules.clearCalls.empty(), "scan failure caused writes");
    Require(!RulesRemoval::Apply(nullptr), "null INI accepted");
    Manifest("[A]\n-=First\n-=Second\n");
    rules.Add("A", "Second", "keep");
    rules.failClear = true;
    Require(!RulesRemoval::Apply(&rules), "failed native deletion reported success");
    Require(rules.clearCalls.size() == 1, "continued after native deletion failure");
    Require(rules.Entry("A", "First") && rules.Entry("A", "Second"), "native failure test changed keys");
}
}

int main()
{
    Log::g_level = Log::Level::Off;
    const std::vector<std::pair<const char*, void(*)()>> tests = {
        { "exact deletion, stored spelling, defaults/source untouched", ExactDeletion },
        { "empty sections and duplicate deletion", KeepEmptySection },
        { "whole-layer validation before native mutation", NoPartialDeletion },
        { "missing/open/short/oversize file errors", ReadFailures },
        { "real adapter resolution and MIX fallbacks", ResolutionAndMix },
        { "canonical cycles and missing-section fallback prevention", CyclesAndAbsentTargets },
        { "empty/failed scans and native failure reporting", EmptyScanAndNativeFailure }
    };
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "[PASS] " << name << '\n'; }
        catch (const std::exception& e) {
            std::cerr << "[FAIL] " << name << ": " << e.what() << '\n';
            return 1;
        }
    }
    std::cout << tests.size() << "/" << tests.size() << " adapter test groups passed\n";
}
