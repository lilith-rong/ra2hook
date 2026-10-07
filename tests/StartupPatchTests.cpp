#include "StartupPatch.h"
#include "IniOverlay.h"
#include "Logger.h"
#include <CCINIClass.h>
#include <CCFileClass.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Compile the real StartupPatch.cpp and IniPatch.cpp. Only external engine/
// file APIs and directory enumeration are faked, not the adapter's execution.
namespace IniOverlay {
int ScanDirectory(const char* directory, const char*, char files[][kPathMax])
{
    FakeFiles::scannedDirectory = directory;
    if (FakeFiles::scanFails || FakeFiles::roots.size() > kMaxFiles) return -1;
    auto roots = FakeFiles::roots;
    std::sort(roots.begin(), roots.end(), [](const auto& a, const auto& b) {
        return FakeFiles::Identity(a) < FakeFiles::Identity(b);
    });
    for (std::size_t i = 0; i < roots.size(); ++i) {
        if (roots[i].size() >= kPathMax) return -1;
        std::snprintf(files[i], kPathMax, "%s", roots[i].c_str());
    }
    return static_cast<int>(roots.size());
}
}
namespace {
const std::string directory = "C:/game/ra2hook/inject/rules";
const std::string root = directory + "/10-main.ini";
void Require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
#define CHECK(x) Require((x), std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #x)
void Manifest(const std::string& text)
{
    FakeFiles::Reset(); FakeFiles::roots.push_back(root); FakeFiles::Add(root, text);
}
bool Run(CCINIClass& target, bool allowRemoval = true)
{
    StartupPatch::Stats stats;
    return StartupPatch::ApplyDirectory(&target, directory.c_str(), allowRemoval, stats);
}
void ExactDeletion()
{
    const std::string text = "[htnk]\n-=prerequisite\n-=factoryowners\n-=Empty\n"
                             "-=PREREQUISITE\n[Missing]\n-=Strength\n";
    Manifest(text);
    CCINIClass target;
    target.Add("HtNk", "Prerequisite", "explicit"); target.Add("HtNk", "FactoryOwners", "YuriCountry");
    target.Add("HtNk", "Empty", ""); target.Add("HtNk", "Prerequisite.X", "keep");
    target.Add("HtNk", "Strength", "999"); target.Add("Other", "Prerequisite", "untouched");
    target.CurrentSection = target.Section("HtNk");
    StartupPatch::Stats stats;
    CHECK(StartupPatch::ApplyDirectory(&target, directory.c_str(), true, stats));
    CHECK(!target.Entry("HTNK", "Prerequisite") && !target.Entry("HTNK", "FactoryOwners"));
    CHECK(!target.Entry("HTNK", "Empty") && target.Entry("HTNK", "Prerequisite.X"));
    CHECK(target.Entry("HTNK", "Strength")->valueStorage == "999");
    CHECK(target.Entry("Other", "Prerequisite") && !target.Section("Missing"));
    CHECK(stats.removals == 3 && stats.absent == 2 && stats.writes == 0);
    CHECK(target.clearCalls[0] == std::make_pair(std::string("HtNk"), std::string("Prerequisite")));
    CHECK(!target.unsafeClear && target.CurrentSection == nullptr);
    CHECK(FakeFiles::files.at(FakeFiles::Identity(root)).text == text);
}
void OrderedSetRemoveSet()
{
    Manifest("[tank]\nstrength=600\n-=Strength\nSTRENGTH=800\n-=Strength\nStrength=900\nEmpty=\n");
    CCINIClass target; target.Add("Tank", "Strength", "500");
    CHECK(Run(target));
    CHECK(target.Entry("Tank", "Strength")->valueStorage == "900");
    CHECK(target.Entry("Tank", "Empty") && target.Entry("Tank", "Empty")->valueStorage.empty());
    CHECK(!target.wrongCaseWrite && !target.unsafeClear);
    const std::vector<std::string> expected {
        "S:Tank:Strength=600", "R:Tank:Strength", "S:Tank:STRENGTH=800",
        "R:Tank:STRENGTH", "S:Tank:Strength=900", "S:Tank:Empty="
    };
    CHECK(target.mutations == expected);
    Manifest("[Tank]\nStrength=700\n-=Strength\n");
    CHECK(Run(target) && !target.Entry("Tank", "Strength"));
    CHECK(!target.Entry("Tank", "-") && !target.Entry("Tank", "+"));
}
void SortedRootsAndIncludeOrder()
{
    Manifest("[Tank]\nValue=before\n[#include]\n+=child.ini\n[Tank]\nValue=parent-tail\n");
    FakeFiles::Add(directory + "/child.ini", "[Tank]\n-=Value\nValue=child\n");
    const std::string later = directory + "/99-LATER.ini";
    FakeFiles::roots.insert(FakeFiles::roots.begin(), later); // Enumeration intentionally reversed.
    FakeFiles::Add(later, "[Tank]\n-=Value\nValue=last-root\n");
    CCINIClass target;
    CHECK(Run(target));
    CHECK(target.Entry("Tank", "Value")->valueStorage == "last-root");
    const std::vector<std::string> expected {
        "S:Tank:Value=before", "R:Tank:Value", "S:Tank:Value=child",
        "S:Tank:Value=parent-tail", "R:Tank:Value", "S:Tank:Value=last-root"
    };
    CHECK(target.mutations == expected);
}
void AppendIdentityAndCollisions()
{
    Manifest("[VehicleTypes]\nRA2Hook_10=EXPLICIT\n+=ONE\n+=TWO\nRA2Hook_11=REPLACED\n"
             "[Unit]\n+=X\n+=X\n-=RA2Hook_13\n");
    CCINIClass target;
    target.Add("Other", "ra2hook_2", "existing");
    target.Add("Other", "RA2Hook_not_a_number", "untouched");
    CHECK(Run(target));
    CHECK(target.Entry("VehicleTypes", "RA2Hook_10")->valueStorage == "EXPLICIT");
    CHECK(target.Entry("VehicleTypes", "RA2Hook_11")->valueStorage == "REPLACED");
    CHECK(target.Entry("VehicleTypes", "RA2Hook_12")->valueStorage == "TWO");
    CHECK(!target.Entry("Unit", "RA2Hook_13") && target.Entry("Unit", "RA2Hook_14"));
    CHECK(!target.Entry("Unit", "+") && !target.Entry("VehicleTypes", "+"));
    CHECK(target.Entry("Other", "ra2hook_2")->valueStorage == "existing");
    Manifest("[List]\n+=NEW\n");
    CCINIClass empty; CHECK(Run(empty, false));
    CHECK(empty.Entry("List", "RA2Hook_0")->valueStorage == "NEW");
    CCINIClass full; full.Add("Other", "RA2Hook_4294967295", "keep");
    CHECK(!Run(full, false) && full.mutations.empty());
}
void EmptyAndAbsentSections()
{
    Manifest("[A]\n-=Only\n-=Only\n[Missing]\n-=Only\n");
    CCINIClass target; target.Add("A", "Only", "value");
    target.CurrentSection = target.Section("A");
    CHECK(Run(target));
    CHECK(target.Section("A") && !target.Section("A")->Entries.First());
    CHECK(!target.Section("Missing") && target.clearCalls.size() == 1 && !target.unsafeClear);
}
void PreflightNoPartialChanges()
{
    for (const std::string invalid : { "[A]\n-=\n", "[A]\n+=\n", "[A]\n-=*\n",
          "[VehicleTypes]\n-=1\n", "[A\n-=Second\n", "[#include]\n-=bad.ini\n" }) {
        Manifest("[A]\nFirst=changed\n-=Second\n+=APPENDED\n[#include]\n+=bad.ini\n");
        FakeFiles::Add(directory + "/bad.ini", invalid);
        CCINIClass target; target.Add("A", "First", "original"); target.Add("A", "Second", "keep");
        CHECK(!Run(target));
        CHECK(target.mutations.empty() && target.clearCalls.empty());
        CHECK(target.Entry("A", "First")->valueStorage == "original");
        CHECK(target.Entry("A", "Second") && !target.Entry("A", "RA2Hook_0"));
    }
    Manifest("[A]\nFirst=changed\n-=Second\n");
    const auto later = directory + "/99-bad.ini";
    FakeFiles::roots.push_back(later); FakeFiles::Add(later, "[A]\n-=\n");
    CCINIClass target; target.Add("A", "Second", "keep");
    CHECK(!Run(target) && target.mutations.empty() && target.Entry("A", "Second"));
}
void NonRulesAndRegistryPolicy()
{
    Manifest("[A]\nValue=changed\n-=Other\n");
    CCINIClass target; target.Add("A", "Value", "original");
    CHECK(!Run(target, false) && target.mutations.empty());
    CHECK(target.Entry("A", "Value")->valueStorage == "original");
    Manifest("[VehicleTypes]\n+=MYTANK\n[MYTANK]\nStrength=800\n-=Prerequisite\n");
    CHECK(Run(target));
    CHECK(target.Entry("VehicleTypes", "RA2Hook_0")->valueStorage == "MYTANK");
    CHECK(target.Entry("MYTANK", "Strength")->valueStorage == "800");
}
void ReadFailures()
{
    for (int mode = 0; mode < 4; ++mode) {
        Manifest("[A]\nFirst=changed\n-=Second\n[#include]\n+=bad.ini\n");
        const auto child = directory + "/bad.ini";
        if (mode) {
            FakeFiles::Add(child, "[A]\nValue=child\n");
            auto& f = FakeFiles::files.at(FakeFiles::Identity(child));
            if (mode == 1) f.openable = false;
            if (mode == 2) f.shortRead = true;
            if (mode == 3) f.reportedSize = static_cast<int>(IniPatch::kMaxFileBytes + 1);
            FakeFiles::Add("bad.ini", "[A]\nValue=wrong-fallback\n");
        }
        CCINIClass target; target.Add("A", "First", "original");
        CHECK(!Run(target) && target.mutations.empty());
        CHECK(target.Entry("A", "First")->valueStorage == "original");
    }
}
void ResolutionAndMix()
{
    Manifest("[#include]\n+=local.ini\n+=game.ini\n+=mix.ini\n[A]\n-=Root\n");
    FakeFiles::Add(directory + "/local.ini", "[A]\n-=Local\n");
    FakeFiles::Add("C:/game/local.ini", "[A]\n-=MustKeep\n");
    FakeFiles::Add("local.ini", "[A]\n-=MustKeep\n");
    FakeFiles::Add("C:/game/game.ini", "[A]\n-=Game\n");
    FakeFiles::Add("game.ini", "[A]\n-=MustKeep\n");
    FakeFiles::Add("mix.ini", "[#include]\n+=nested.ini\n[A]\n-=Mix\n");
    FakeFiles::Add("nested.ini", "[A]\n-=Nested\n");
    CCINIClass target;
    for (const auto* key : { "Root", "Local", "Game", "Mix", "Nested", "MustKeep" }) target.Add("A", key, "value");
    CHECK(Run(target) && target.clearCalls.size() == 5 && target.Entry("A", "MustKeep"));
    const std::vector<std::string> expected { "Local", "Game", "Nested", "Mix", "Root" };
    for (std::size_t i = 0; i < expected.size(); ++i) CHECK(target.clearCalls[i].second == expected[i]);
}
void CyclesAndScanFailures()
{
    Manifest("[A]\nFirst=changed\n[#include]\n+=.\\..\\rules\\10-MAIN.ini\n");
    CCINIClass target; target.Add("A", "First", "keep");
    CHECK(!Run(target) && target.mutations.empty());
    FakeFiles::Reset();
    CHECK(Run(target) && FakeFiles::scannedDirectory == directory);
    FakeFiles::scanFails = true; CHECK(!Run(target) && target.mutations.empty());
    StartupPatch::Stats stats;
    CHECK(!StartupPatch::ApplyDirectory(nullptr, directory.c_str(), true, stats));
    IniPatch::Plan p;
    CHECK(!StartupPatch::Prepare(nullptr, true, p));
    CHECK(!StartupPatch::Prepare("", true, p));
}
void PreparedSoundHasNoSourceReads()
{
    Manifest("[Sound]\nVolume=1\nVolume=2\n+=SAMPLE\n");
    IniPatch::Plan p;
    CHECK(StartupPatch::Prepare(directory.c_str(), false, p));
    CHECK(p.commands.size() == 3 && !p.allowRemoval);
    const auto attempts = FakeFiles::attempts;
    FakeFiles::files.clear(); // Sources unavailable after the safe early hook.
    CCINIClass sound; StartupPatch::Stats stats;
    CHECK(StartupPatch::Apply(&sound, p, stats));
    CHECK(FakeFiles::attempts == attempts);
    CHECK(sound.Entry("Sound", "Volume")->valueStorage == "2");
    CHECK(stats.writes == 2 && stats.appends == 1 && stats.removals == 0);
    FakeFiles::scanFails = true;
    CHECK(!StartupPatch::Prepare(directory.c_str(), false, p) && p.commands.empty());
}
void NativeFailureAndUnsafePlans()
{
    Manifest("[A]\nValue=written\n-=First\nTail=must-not-run\n");
    CCINIClass target; target.Add("A", "First", "keep"); target.failClear = true;
    CHECK(!Run(target));
    CHECK(target.Entry("A", "Value")->valueStorage == "written"); // No rollback promised.
    CHECK(target.Entry("A", "First") && !target.Entry("A", "Tail"));
    CHECK(target.clearCalls.size() == 1);
    Manifest("[A]\n-=First\nValue=failed\nTail=must-not-run\n");
    CCINIClass writeFailure; writeFailure.Add("A", "First", "old"); writeFailure.failWrite = true;
    CHECK(!Run(writeFailure));
    CHECK(!writeFailure.Entry("A", "First") && !writeFailure.Entry("A", "Tail"));
    for (const auto* key : { "", "*", "0" }) {
        IniPatch::Plan unsafe;
        unsafe.allowRemoval = true;
        unsafe.commands.push_back({ IniPatch::Operation::Set, "A", "Value", "must-not-write", "x", 1 });
        unsafe.commands.push_back({ IniPatch::Operation::Remove, "VehicleTypes", key, {}, "x", 2 });
        CCINIClass untouched; StartupPatch::Stats stats;
        CHECK(!StartupPatch::Apply(&untouched, unsafe, stats));
        CHECK(untouched.mutations.empty() && !untouched.unsafeClear);
    }
}
} // namespace
int main()
{
    Log::g_level = Log::Level::Off; // Tests must not create log files/directories.
    const std::vector<std::pair<const char*, void(*)()>> tests {
        { "exact explicit deletion, case, empty value and source preservation", ExactDeletion },
        { "set/remove/set line order and stored-case writes", OrderedSetRemoveSet },
        { "sorted roots, in-place include and later overrides", SortedRootsAndIncludeOrder },
        { "repeated append identities and explicit-key collisions", AppendIdentityAndCollisions },
        { "empty and missing sections are not deleted/created", EmptyAndAbsentSections },
        { "whole-target preflight prevents ALL partial mutations", PreflightNoPartialChanges },
        { "rules-only removal and registry additions", NonRulesAndRegistryPolicy },
        { "missing/open/short/oversize source failures", ReadFailures },
        { "relative/game/MIX resolution and instruction order", ResolutionAndMix },
        { "canonical cycles and scan/null errors", CyclesAndScanFailures },
        { "sound preparation then source-free memory application", PreparedSoundHasNoSourceReads },
        { "native failures stop without rollback; unsafe plans rejected", NativeFailureAndUnsafePlans }
    };
    std::size_t failures = 0;
    for (const auto& t : tests) {
        try { t.second(); std::cout << "[PASS] " << t.first << '\n'; }
        catch (const std::exception& e) { ++failures; std::cerr << "[FAIL] " << t.first << ": " << e.what() << '\n'; }
        catch (...) { ++failures; std::cerr << "[FAIL] " << t.first << ": unknown exception\n"; }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " adapter groups passed\n";
    return failures ? 1 : 0;
}
