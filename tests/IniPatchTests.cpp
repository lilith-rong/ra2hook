#include "IniPatch.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace IniPatch;
void Require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
#define CHECK(x) Require((x), std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #x)

struct MemoryReader {
    struct Entry { Source source; bool readable = true; };
    std::map<std::string, Entry> files;
    std::vector<std::pair<std::string, std::string>> calls;
    void Add(const std::string& path, std::string text) { files[path] = { { path, std::move(text) }, true }; }
    void Alias(const std::string& request, const std::string& resolved, const std::string& text)
    { files[request] = { { resolved, text }, true }; }
    static bool Read(const std::string& request, const std::string& parent,
                     Source& source, std::string& error, void* context)
    {
        auto& self = *static_cast<MemoryReader*>(context);
        self.calls.emplace_back(request, parent);
        auto found = self.files.end();
        const bool rooted = !request.empty() && (request[0] == '/' || request[0] == '\\' ||
                            (request.size() > 1 && request[1] == ':'));
        const auto slash = parent.find_last_of("/\\");
        if (!rooted && slash != std::string::npos)
            found = self.files.find(parent.substr(0, slash + 1) + request);
        if (found == self.files.end()) found = self.files.find(request);
        if (found == self.files.end() || !found->second.readable) {
            error = "unreadable memory file";
            return false;
        }
        // Intentionally do not canonicalize: production must detect aliases.
        source = found->second.source;
        return true;
    }
};

Plan Stale()
{
    Plan plan;
    plan.allowRemoval = true;
    plan.files = 42;
    plan.commands.push_back({ Operation::Remove, "Old", "Key", {}, "old.ini", 99 });
    return plan;
}
Plan Build(MemoryReader& reader, const std::vector<std::string>& roots, bool remove = true)
{
    Plan plan = Stale();
    Error error { "old", 99, "old" };
    const bool ok = BuildPlan(roots, MemoryReader::Read, &reader, plan, error, remove);
    Require(ok, error.file + ":" + std::to_string(error.line) + ": " + error.message);
    CHECK(error.file.empty() && error.message.empty() && error.line == 0);
    CHECK(plan.allowRemoval == remove);
    return plan;
}
Error Fails(MemoryReader& reader, const std::vector<std::string>& roots,
            const std::string& diagnostic, bool remove = true)
{
    Plan plan = Stale();
    Error error { "old", 99, "old" };
    CHECK(!BuildPlan(roots, MemoryReader::Read, &reader, plan, error, remove));
    CHECK(plan.commands.empty() && plan.files == 0 && !plan.allowRemoval);
    Require(error.message.find(diagnostic) != std::string::npos,
            "expected '" + diagnostic + "', got '" + error.message + "'");
    return error;
}
void Location(const Error& e, const std::string& file, std::size_t line)
{ CHECK(e.file == file && e.line == line); }
void CommandAt(const Plan& plan, std::size_t i, Operation op, const std::string& section,
               const std::string& key, const std::string& value, const std::string& file,
               std::size_t line)
{
    CHECK(i < plan.commands.size());
    const auto& c = plan.commands[i];
    CHECK(c.operation == op && c.section == section && c.key == key && c.value == value);
    CHECK(c.file == file && c.line == line);
}
std::string Repeat(const std::string& text, std::size_t count)
{
    std::string result;
    result.reserve(text.size() * count);
    while (count--) result += text;
    return result;
}

void EmptyAndRootOrder()
{
    MemoryReader r;
    CHECK(Build(r, {}).commands.empty() && r.calls.empty());
    r.Add("empty.ini", ""); r.Add("comments.ini", "; comment\r\n# comment\n[Empty]\n");
    auto p = Build(r, { "empty.ini", "comments.ini" });
    CHECK(p.files == 2 && p.commands.empty());
    r.Add("z.ini", "[Z]\nValue=first\n"); r.Add("a.ini", "[Z]\n-=Value\n");
    p = Build(r, { "z.ini", "a.ini", "z.ini" });
    CHECK(p.files == 3 && p.commands.size() == 3);
    CommandAt(p, 0, Operation::Set, "Z", "Value", "first", "z.ini", 2);
    CommandAt(p, 1, Operation::Remove, "Z", "Value", "", "a.ini", 2);
    CommandAt(p, 2, Operation::Set, "Z", "Value", "first", "z.ini", 2);
}
void MixedLineOrder()
{
    MemoryReader r;
    r.Add("unit.ini", "[Tank]\nStrength=500\n-=Strength\nStrength=800\n"
          "+=One\n+=Two\n-=Armor\n-=Armor\n[Tank]\nEmpty=\n");
    auto p = Build(r, { "unit.ini" });
    CHECK(p.commands.size() == 8);
    CommandAt(p, 0, Operation::Set, "Tank", "Strength", "500", "unit.ini", 2);
    CommandAt(p, 1, Operation::Remove, "Tank", "Strength", "", "unit.ini", 3);
    CommandAt(p, 2, Operation::Set, "Tank", "Strength", "800", "unit.ini", 4);
    CommandAt(p, 3, Operation::Append, "Tank", "", "One", "unit.ini", 5);
    CommandAt(p, 4, Operation::Append, "Tank", "", "Two", "unit.ini", 6);
    CommandAt(p, 5, Operation::Remove, "Tank", "Armor", "", "unit.ini", 7);
    CommandAt(p, 6, Operation::Remove, "Tank", "Armor", "", "unit.ini", 8);
    CommandAt(p, 7, Operation::Set, "Tank", "Empty", "", "unit.ini", 10);
}
void IncludesInPlace()
{
    MemoryReader r;
    r.Add("dir/root.ini", "[Tank]\nValue=before\n[#include]\n+=first.ini\n"
          "named=second.ini\n0=first.ini\n[Tank]\nValue=after\n");
    r.Add("dir/first.ini", "[Tank]\n-=Value\n[#include]\n+=grand.ini\n[Child]\nKey=tail\n");
    r.Add("dir/grand.ini", "[Tank]\nValue=grand\n");
    r.Add("dir/second.ini", "[Tank]\nValue=second\n");
    auto p = Build(r, { "dir/root.ini" });
    CHECK(p.files == 6 && p.commands.size() == 9);
    CommandAt(p, 0, Operation::Set, "Tank", "Value", "before", "dir/root.ini", 2);
    CommandAt(p, 1, Operation::Remove, "Tank", "Value", "", "dir/first.ini", 2);
    CommandAt(p, 2, Operation::Set, "Tank", "Value", "grand", "dir/grand.ini", 2);
    CommandAt(p, 3, Operation::Set, "Child", "Key", "tail", "dir/first.ini", 6);
    CommandAt(p, 4, Operation::Set, "Tank", "Value", "second", "dir/second.ini", 2);
    CommandAt(p, 8, Operation::Set, "Tank", "Value", "after", "dir/root.ini", 8);
    const std::vector<std::pair<std::string, std::string>> calls {
        { "dir/root.ini", "" }, { "first.ini", "dir/root.ini" }, { "grand.ini", "dir/first.ini" },
        { "second.ini", "dir/root.ini" }, { "first.ini", "dir/root.ini" }, { "grand.ini", "dir/first.ini" }
    };
    CHECK(r.calls == calls);
    r.Add("orphan.ini", "Key=no-section\n");
    r.Add("parent.ini", "[Tank]\nA=1\n[#include]\n+=orphan.ini\n");
    Location(Fails(r, { "parent.ini" }, "outside a section"), "orphan.ini", 1);
}
void QuotesAndResolution()
{
    MemoryReader r;
    r.Alias("entry.ini", "resolved/entry.ini", "[#include]\n+ = \"local ; # name.ini\" ; tail\n"
            "named='engine # ; name.ini' #tail\n2=plain.ini;tail\n3=C:\\mix\\extra.ini #tail\n"
            "[Target]\n-=Armor;tail\n");
    r.Add("resolved/local ; # name.ini", "[Local]\nKey=1\n");
    r.Add("local ; # name.ini", "bad text");
    r.Add("engine # ; name.ini", "[Engine]\nKey=2\n");
    r.Add("resolved/plain.ini", "[Plain]\nKey=3\n");
    r.Add("C:\\mix\\extra.ini", "[Absolute]\nKey=4\n");
    const auto p = Build(r, { "entry.ini" });
    CHECK(p.files == 5 && p.commands.size() == 5);
    CommandAt(p, 0, Operation::Set, "Local", "Key", "1", "resolved/local ; # name.ini", 2);
    CommandAt(p, 4, Operation::Remove, "Target", "Armor", "", "resolved/entry.ini", 7);
}
void ValuesAndSpecialOperators()
{
    MemoryReader r;
    r.Add("values.ini", "[Unit With Space]\n $Inherits = BASE \n Text = \"a=b;#c\" ; tail \n"
          "Empty=\nFoo+=literal\n--=ordinary\n+=Item ; comment\n");
    auto p = Build(r, { "values.ini" }, false);
    CHECK(p.commands.size() == 6);
    CHECK(p.commands[0].key == "$Inherits" && p.commands[0].value == "BASE");
    CHECK(p.commands[1].value == "\"a=b;#c\" ; tail");
    CHECK(p.commands[2].operation == Operation::Set && p.commands[2].value.empty());
    CHECK(p.commands[3].key == "Foo+" && p.commands[3].operation == Operation::Set);
    CHECK(p.commands[4].key == "--" && p.commands[4].operation == Operation::Set);
    CHECK(p.commands[5].operation == Operation::Append && p.commands[5].value == "Item");
}
void EncodingAndComments()
{
    const std::string uSpace = "\xE3\x80\x80", gSpace = "\xA1\xA1";
    for (const std::string comment : { ";", "#", "\xEF\xBC\x9B", "\xEF\xBC\x83", "\xA3\xBB", "\xA3\xA3" }) {
        MemoryReader r;
        r.Add("encoding.ini", std::string("\xEF\xBB\xBF") + uSpace + comment + " comment\r\n" +
              gSpace + "[Tank]" + uSpace + comment + "header\r\n" +
              uSpace + "-" + gSpace + "=" + uSpace + "Armor" + gSpace + comment + "tail\r" +
              "+=Item" + comment + "tail\nValue=100\n");
        auto p = Build(r, { "encoding.ini" });
        CHECK(p.commands.size() == 3);
        CommandAt(p, 0, Operation::Remove, "Tank", "Armor", "", "encoding.ini", 3);
        CHECK(p.commands[1].value == "Item" && p.commands[2].value == "100");
    }
    MemoryReader r;
    const std::string section = "\xD5\xBD\xB3\xB5", key = "\xBB\xA4\xBC\xD7";
    r.Add("ansi.ini", "[" + section + "]\r\n-=" + key);
    CommandAt(Build(r, { "ansi.ini" }), 0, Operation::Remove, section, key, "", "ansi.ini", 2);
}
void NamesAndPaths()
{
    CHECK(EqualName("Prerequisite", "prEREQUISITE"));
    CHECK(!EqualName("Prerequisite", "Prerequisite.X"));
    CHECK(!EqualName("\xC0", "\xE0"));
    const std::vector<std::pair<std::string, std::string>> paths {
        { "", "" }, { ".", "" }, { "./a.ini", "a.ini" }, { "a//./b/../c.ini", "a/c.ini" },
        { "../a/../../b.ini", "../../b.ini" }, { "/../../a.ini", "/a.ini" },
        { "C:\\Mods\\.\\child\\..\\Root.ini", "C:/Mods/Root.ini" },
        { "C:mods/../Root.ini", "C:Root.ini" },
        { "\\\\server\\share\\.\\sub\\..\\Root.ini", "//server/share/Root.ini" }
    };
    for (const auto& pair : paths) CHECK(NormalizePath(pair.first) == pair.second);
    CHECK(IsRemovalTarget("Tank", "Prerequisite.X"));
    CHECK(!IsRemovalTarget("Tank*", "Armor") && !IsRemovalTarget("Tank", "Armor,Strength"));
    CHECK(!IsRemovalTarget("#include", "Path"));
}
struct Bad { std::string text, diagnostic; std::size_t line; };
void BadCases(const std::vector<Bad>& cases)
{
    for (const auto& c : cases) {
        MemoryReader r; r.Add("bad.ini", c.text);
        Location(Fails(r, { "bad.ini" }, c.diagnostic), "bad.ini", c.line);
    }
}
void InvalidCommands()
{
    BadCases({
        { "[Tank]\n-=\n", "one exact, nonempty", 2 },
        { "[Tank]\n-= ;comment\n", "one exact, nonempty", 2 },
        { "[Tank]\n+= ;comment\n", "empty +=", 2 },
        { "[Tank]\n=bad\n", "invalid property", 2 },
        { "[Tank]\n-=*\n", "one exact, nonempty", 2 },
        { "[Tank]\n-=Prerequisite.*\n", "one exact, nonempty", 2 },
        { "[Tank]\n-=Arm?r\n", "one exact, nonempty", 2 },
        { "[Tank]\n-=Armor,Strength\n", "one exact, nonempty", 2 },
        { "[Tank]\n-=Armor Strength\n", "one exact, nonempty", 2 },
        { "[Tank]\n-=Armor\tStrength\n", "one exact, nonempty", 2 },
        { "[Tank]\n-=Armor\xE3\x80\x80Strength\n", "one exact, nonempty", 2 },
        { "[Tank]\n-=Armor\xA1\xA1Strength\n", "one exact, nonempty", 2 },
        { "[Tank]\n-=\"Armor\"\n", "one exact, nonempty", 2 },
        { "[Tank]\n-=Armor=no\n", "one exact, nonempty", 2 },
        { "[Tank]\n-=Armor/Strength\n", "one exact, nonempty", 2 },
        { "[Tank]\n-=Armor\\Strength\n", "one exact, nonempty", 2 },
        { "[Tank]\n-=[Armor]\n", "one exact, nonempty", 2 },
        { "[Tank]\n-=Arm\x01or\n", "one exact, nonempty", 2 },
        { "[Tank]\n-=Arm\x7For\n", "one exact, nonempty", 2 },
        { "[Tank*]\n-=Armor\n", "one exact, nonempty", 2 },
        { "[Tank,Other]\n-=Armor\n", "one exact, nonempty", 2 }
    });
}
void InvalidHeaders()
{
    BadCases({
        { "-=Armor\n", "outside a section", 1 }, { "stray\n[Tank]\nA=1", "outside a section", 1 },
        { "[Tank]\nstray\n", "expected key=value", 2 },
        { "[Tank\n", "invalid section header", 1 }, { "[Tank] tail\n", "unexpected text", 1 },
        { "[Tank]]\n", "unexpected text", 1 }, { "[]\n", "invalid section name", 1 },
        { "[ \t ]\n", "invalid section name", 1 }, { "[[Tank]\n", "invalid section name", 1 },
        { std::string("\xFF\xFE") + "[Tank]\n", "UTF-16", 1 },
        { std::string("\xFE\xFF") + "[Tank]\n", "UTF-16", 1 },
        { std::string("[Tank]\nValue=a") + '\0' + "b\n", "NUL byte", 2 },
        { std::string(";comment") + '\0', "NUL byte", 1 },
        { "\n\xEF\xBB\xBF[Tank]\n", "outside a section", 2 },
        { "\r\n;comment\r\n[Tank]\r\n-=\r\n", "one exact, nonempty", 4 }
    });
}
void InvalidIncludes()
{
    BadCases({
        { "[#include]\n-=child.ini\n", "invalid include assignment", 2 },
        { "[#include]\n=child.ini\n", "invalid include assignment", 2 },
        { "[#include]\n*=child.ini\n", "invalid include assignment", 2 },
        { "[#include]\n+=\n", "invalid or overlong", 2 },
        { "[#include]\n+=#comment\n", "invalid or overlong", 2 },
        { "[#include]\n+=\"\"\n", "invalid or overlong", 2 },
        { "[#include]\n+='';comment\n", "invalid or overlong", 2 },
        { "[#include]\n+=\"child.ini\n", "unclosed include path quote", 2 },
        { "[#include]\n+='child.ini\"\n", "unclosed include path quote", 2 },
        { "[#include]\n+=\"child.ini\" junk\n", "unexpected text after include", 2 },
        { "[#include]\n+='a.ini','b.ini'\n", "unexpected text after include", 2 },
        { "[#include]\n+=*.ini\n", "invalid or overlong", 2 },
        { "[#include]\n+=child?.ini\n", "invalid or overlong", 2 },
        { "[#include]\n+=child|other.ini\n", "invalid or overlong", 2 },
        { "[#include]\n+=<child.ini>\n", "invalid or overlong", 2 },
        { "[#include]\n+=child=other.ini\n", "invalid or overlong", 2 },
        { "[#include]\n+='child\tname.ini'\n", "invalid or overlong", 2 },
        { "[#include]\n+='child\x01name.ini'\n", "control character", 2 },
        { "[#include]\n+='child\x7Fname.ini'\n", "control character", 2 }
    });
}
void RegistryAndTargetPolicy()
{
    for (std::string name : { "InfantryTypes", "VehicleTypes", "AircraftTypes", "BuildingTypes",
          "TerrainTypes", "SmudgeTypes", "OverlayTypes", "Animations", "VoxelAnims", "Warheads",
          "Particles", "ParticleSystems", "WeaponTypes", "Projectiles", "Projectile", "SuperWeaponTypes",
          "Countries", "Sides", "AITriggerTypes", "AITriggerTypesEnable", "TeamTypes", "TaskForces",
          "ScriptTypes", "TriggerTypes", "Triggers", "Tags", "Colors", "ColorAdd" }) {
        for (int variation = 0; variation < 2; ++variation) {
            MemoryReader r;
            r.Add("registry.ini", "[" + name + "]\n0=ONE\n+=TWO\n");
            CHECK(Build(r, { "registry.ini" }).commands.size() == 2);
            r.Add("registry.ini", "[" + name + "]\n0=ONE\n-=0\n");
            Location(Fails(r, { "registry.ini" }, "registry sections"), "registry.ini", 3);
            for (char& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        }
    }
    MemoryReader r;
    r.Add("unit.ini", "[Tank]\nA=1\n-=Armor\nA=2\n");
    Fails(r, { "unit.ini" }, "only supported in inject/rules", false);
    Plan p; Error e;
    CHECK(!BuildPlan({ "unit.ini" }, MemoryReader::Read, &r, p, e)); // Safe API default.
    CHECK(p.commands.empty());
    r.Add("near.ini", "[VehicleTypes.X]\n-=Foo\n[MyWarheads]\n-=Bar\n");
    CHECK(Build(r, { "near.ini" }).commands.size() == 2);
}
void ReadFailuresAndAtomicPlan()
{
    MemoryReader r;
    r.Add("good.ini", "[Tank]\nA=1\n-=Armor\n+=Thing\n");
    r.Add("bad.ini", "[Tank]\n-=\n");
    r.Add("bad-body.ini", "[#include]\n+=good.ini\n[Tank]\n-=\n");
    r.Add("child.ini", "[Tank]\nA=1\n[#include]\n+=bad.ini\n");
    r.Add("unreadable.ini", ""); r.files.at("unreadable.ini").readable = false;
    for (const auto& path : { "bad.ini", "missing.ini", "unreadable.ini", "child.ini", "bad-body.ini" }) {
        Fails(r, { "good.ini", path }, "");
        CHECK(Build(r, { "good.ini" }).commands.size() == 3); // Recovery after failure.
    }
    r.calls.clear();
    Location(Fails(r, { "bad-body.ini" }, "one exact"), "bad-body.ini", 4);
    CHECK(r.calls.size() == 2); // Child read first, but no partial plan exposed.
    r.Alias("empty-path.ini", "", "[Tank]\nA=1\n");
    Location(Fails(r, { "empty-path.ini" }, "invalid resolved path"), "empty-path.ini", 0);
}
void CyclesAndDepth()
{
    for (const std::string alias : { "dir/root.ini", "dir/./root.ini", "dir/child/../root.ini", "DIR\\child\\..\\ROOT.INI" }) {
        MemoryReader r;
        r.Add("dir/root.ini", "[Tank]\nA=1\n[#include]\n+=alias.ini\n");
        r.Alias("dir/alias.ini", alias, "");
        Location(Fails(r, { "dir/root.ini" }, "include cycle"), "dir/root.ini", 4);
    }
    MemoryReader r;
    for (std::size_t i = 0; i < kMaxDepth; ++i) {
        std::string text = "[Depth]\nKey=" + std::to_string(i) + "\n";
        if (i + 1 < kMaxDepth) text += "[#include]\n+=d" + std::to_string(i + 1) + ".ini\n";
        r.Add("d" + std::to_string(i) + ".ini", text);
    }
    CHECK(Build(r, { "d0.ini" }).files == kMaxDepth);
    const std::string last = "d" + std::to_string(kMaxDepth - 1) + ".ini";
    r.Add(last, "[#include]\n+=too-deep.ini\n");
    r.Add("too-deep.ini", ""); r.calls.clear();
    Location(Fails(r, { "d0.ini" }, "include depth"), last, 2);
    CHECK(r.calls.size() == kMaxDepth);
}
void PathRootAndTokenLimits()
{
    MemoryReader r; Plan p = Stale(); Error e;
    CHECK(!BuildPlan({}, nullptr, nullptr, p, e));
    CHECK(p.commands.empty() && p.files == 0);
    r.Add("root.ini", "[Tank]\nA=1\n");
    CHECK(Build(r, std::vector<std::string>(256, "root.ini")).files == 256);
    Fails(r, std::vector<std::string>(257, "root.ini"), "too many root files");
    Fails(r, { "" }, "empty or overlong");
    const std::string path(kMaxPath - 1, 'a'), tooLong(kMaxPath, 'a');
    r.Add(path, "[Tank]\nA=1\n");
    CHECK(Build(r, { path }).files == 1);
    Fails(r, { tooLong }, "empty or overlong");
    r.Add("include.ini", "[#include]\n+=" + tooLong + "\n");
    Fails(r, { "include.ini" }, "invalid or overlong");
    r.Alias("alias.ini", tooLong, ""); Fails(r, { "alias.ini" }, "invalid resolved path");
    const std::string token(kMaxToken - 1, 'T'), longToken(kMaxToken, 'T');
    r.Add("token.ini", "[" + token + "]\n" + token + "=value\n-=" + token + "\n");
    CHECK(Build(r, { "token.ini" }).commands.size() == 2);
    r.Add("token.ini", "[" + longToken + "]\n"); Fails(r, { "token.ini" }, "invalid section");
    r.Add("token.ini", "[Tank]\n" + longToken + "=value\n"); Fails(r, { "token.ini" }, "invalid property");
    r.Add("token.ini", "[Tank]\n-=" + longToken + "\n"); Fails(r, { "token.ini" }, "one exact");
    r.Add("token.ini", "[#include]\n" + longToken + "=root.ini\n"); Fails(r, { "token.ini" }, "invalid include assignment");
}
void FileAndCommandLimits()
{
    MemoryReader r;
    r.Add("leaf.ini", "");
    const auto includes = Repeat("+=leaf.ini\n", kMaxFiles - 1);
    r.Add("root.ini", "[#include]\n" + includes);
    CHECK(Build(r, { "root.ini" }).files == kMaxFiles);
    r.Add("root.ini", "[#include]\n" + includes + "+=leaf.ini\n+=leaf.ini\n");
    r.calls.clear();
    Location(Fails(r, { "root.ini" }, "loaded file limit"), "root.ini", kMaxFiles + 1);
    CHECK(r.calls.size() == kMaxFiles); // In-place expansion hits file cap first.
    const auto body = "[Tank]\n" + Repeat("Value=1\n", kMaxCommands - 2) + "-=Value\n+=Thing\n";
    r.Add("commands.ini", body);
    CHECK(Build(r, { "commands.ini" }).commands.size() == kMaxCommands);
    r.Add("extra.ini", "[Tank]\n-=Armor\n");
    Fails(r, { "commands.ini", "extra.ini" }, "patch command limit");
    r.Add("commands.ini", body + "[#include]\n+=extra.ini\n");
    Location(Fails(r, { "commands.ini" }, "patch command limit"), "extra.ini", 2);
}
void ByteLimits()
{
    MemoryReader r;
    const std::string full = ";" + std::string(kMaxFileBytes - 1, 'x');
    r.Add("bytes.ini", full);
    CHECK(Build(r, { "bytes.ini" }).files == 1);
    r.Add("bytes.ini", full + 'x'); Fails(r, { "bytes.ini" }, "input byte limit");
    r.Add("bytes.ini", full);
    const auto count = kMaxTotalBytes / kMaxFileBytes;
    std::vector<std::string> roots(count, "bytes.ini");
    CHECK(Build(r, roots).files == count);
    r.Add("one.ini", ";"); roots.push_back("one.ini"); Fails(r, roots, "input byte limit");
    const std::string text = "[#include]\n+=bytes.ini\n";
    r.Add("include.ini", text);
    r.Add("remainder.ini", ";" + std::string(kMaxFileBytes - text.size() - 1, 'x'));
    roots.assign(count - 2, "bytes.ini"); roots.push_back("remainder.ini"); roots.push_back("include.ini");
    CHECK(Build(r, roots).files == count + 1);
    r.Add("remainder.ini", ";" + std::string(kMaxFileBytes - text.size(), 'x'));
    Location(Fails(r, roots, "input byte limit"), "bytes.ini", 0);
}
} // namespace

int main()
{
    const std::vector<std::pair<const char*, void (*)()>> tests {
        { "empty input and caller root order", EmptyAndRootOrder },
        { "mixed repeated set/append/remove line order", MixedLineOrder },
        { "in-place recursive includes and isolated section state", IncludesInPlace },
        { "relative/fallback lookup and quoted paths", QuotesAndResolution },
        { "ordinary values and exact special operators", ValuesAndSpecialOperators },
        { "UTF8 BOM, ANSI/GBK and fullwidth comments", EncodingAndComments },
        { "exact names and path normalization", NamesAndPaths },
        { "invalid removal and append commands", InvalidCommands },
        { "invalid headers, text, encoding and line diagnostics", InvalidHeaders },
        { "invalid include commands and paths", InvalidIncludes },
        { "registry writes allowed, removal policy enforced", RegistryAndTargetPolicy },
        { "all-roots preflight, stale plan reset and recovery", ReadFailuresAndAtomicPlan },
        { "canonical cycles and depth boundary", CyclesAndDepth },
        { "reader, root, path and token limits", PathRootAndTokenLimits },
        { "shared file and mixed command limits", FileAndCommandLimits },
        { "per-file and aggregate byte limits", ByteLimits }
    };
    std::size_t failures = 0;
    for (const auto& test : tests) {
        try { test.second(); std::cout << "[PASS] " << test.first << '\n'; }
        catch (const std::exception& e) { ++failures; std::cerr << "[FAIL] " << test.first << ": " << e.what() << '\n'; }
        catch (...) { ++failures; std::cerr << "[FAIL] " << test.first << ": unknown exception\n"; }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " test groups passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
