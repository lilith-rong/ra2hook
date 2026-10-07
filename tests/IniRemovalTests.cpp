#include "IniRemoval.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using IniRemoval::Command;
using IniRemoval::Error;
using IniRemoval::Plan;
using IniRemoval::Source;

void Require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

#define CHECK(condition) \
    Require((condition), std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
        ": " #condition)

std::string Describe(const Error& error)
{
    return error.file + ":" + std::to_string(error.line) + ": " + error.message;
}

// No disk, locale, current-directory or platform path API is involved. Relative
// lookup is tried before the fallback name, just like the Reader contract.
// Deliberately do NOT use NormalizePath here: cycle tests must exercise the
// production canonicalizer, not accidentally canonicalize in the test double.
struct MemoryReader {
    struct Entry {
        Source source;
        std::string failure;
        bool readable = true;
    };
    using Call = std::pair<std::string, std::string>; // request, containing file
    std::map<std::string, Entry> entries;
    std::vector<Call> calls;

    void Add(const std::string& path, std::string text)
    {
        AddResolved(path, path, std::move(text));
    }

    void AddResolved(const std::string& request, std::string resolved, std::string text)
    {
        entries[request] = { { std::move(resolved), std::move(text) }, {}, true };
    }

    void FailRead(const std::string& path, const std::string& diagnostic)
    {
        entries[path] = { { path, {} }, diagnostic, false };
    }

    static bool Read(const std::string& request, const std::string& parent,
        Source& source, std::string& error, void* context)
    {
        auto& self = *static_cast<MemoryReader*>(context);
        self.calls.emplace_back(request, parent);
        source = {};
        error.clear();
        auto found = self.entries.end();
        const bool absolute = !request.empty() &&
            (request.front() == '/' || request.front() == '\\' ||
             (request.size() > 1 && request[1] == ':'));
        const auto slash = parent.find_last_of("/\\");
        if (!absolute && slash != std::string::npos) {
            found = self.entries.find(parent.substr(0, slash + 1) + request);
        }
        if (found == self.entries.end()) found = self.entries.find(request);
        if (found == self.entries.end()) {
            error = "not found in memory";
            return false;
        }
        if (!found->second.readable) {
            error = found->second.failure;
            return false;
        }
        source = found->second.source;
        return true;
    }
};

Plan StalePlan()
{
    return { { { "StaleSection", "StaleKey", "previous.ini", 99 } }, 42 };
}

Plan Build(MemoryReader& reader, const std::vector<std::string>& roots,
    const std::string& label)
{
    Plan plan = StalePlan();
    Error error { "stale.ini", 999, "stale diagnostic" };
    const bool ok = IniRemoval::BuildPlan(roots, MemoryReader::Read, &reader, plan, error);
    Require(ok, label + ": expected success, got " + Describe(error));
    Require(error.file.empty() && error.line == 0 && error.message.empty(),
        label + ": success did not clear the previous error");
    return plan;
}

Error BuildFails(MemoryReader& reader, const std::vector<std::string>& roots,
    const std::string& label, const std::string& diagnostic)
{
    Plan plan = StalePlan();
    Error error { "stale.ini", 999, "stale diagnostic" };
    const bool ok = IniRemoval::BuildPlan(roots, MemoryReader::Read, &reader, plan, error);
    Require(!ok, label + ": invalid manifest unexpectedly accepted");
    Require(plan.commands.empty() && plan.files == 0,
        label + ": failure exposed a partial or stale plan");
    Require(!error.message.empty() && error.message != "stale diagnostic",
        label + ": failure did not provide a fresh diagnostic");
    Require(error.message.find(diagnostic) != std::string::npos,
        label + ": expected diagnostic containing '" + diagnostic + "', got " + Describe(error));
    return error;
}

void CheckLocation(const Error& error, const std::string& file, std::size_t line)
{
    Require(error.file == file && error.line == line,
        "expected error at " + file + ":" + std::to_string(line) + ", got " + Describe(error));
}

void CheckCommands(const Plan& plan, const std::vector<Command>& expected)
{
    Require(plan.commands.size() == expected.size(),
        "expected " + std::to_string(expected.size()) + " commands, got " +
        std::to_string(plan.commands.size()));
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& actual = plan.commands[i];
        const auto& want = expected[i];
        Require(actual.section == want.section && actual.key == want.key &&
            actual.file == want.file && actual.line == want.line,
            "command " + std::to_string(i) + ": expected [" + want.section + "] -= " +
            want.key + " at " + want.file + ":" + std::to_string(want.line) +
            ", got [" + actual.section + "] -= " + actual.key + " at " + actual.file +
            ":" + std::to_string(actual.line));
    }
}

void EmptyInputAndRootOrder()
{
    MemoryReader reader;
    const auto empty = Build(reader, {}, "no roots");
    CHECK(empty.commands.empty() && empty.files == 0 && reader.calls.empty());
    reader.Add("empty.ini", "");
    reader.Add("comments.ini", "; comment\r\n# comment\n[Empty]\n");
    const auto comments = Build(reader, { "empty.ini", "comments.ini" }, "no commands");
    CHECK(comments.commands.empty() && comments.files == 2);

    reader.Add("z.ini", "[Z]\n-=LastNameFirst\n");
    reader.Add("a.ini", "[A]\n-=FirstNameLast\n");
    const auto ordered = Build(reader, { "z.ini", "a.ini", "z.ini" }, "caller root order");
    CheckCommands(ordered, {
        { "Z", "LastNameFirst", "z.ini", 2 },
        { "A", "FirstNameLast", "a.ini", 2 },
        { "Z", "LastNameFirst", "z.ini", 2 }
    });
    CHECK(ordered.files == 3);
}

void RepeatedKeysAndSections()
{
    MemoryReader reader;
    reader.Add("root.ini",
        "[Tank]\n-=Prerequisite\n-=Armor\n-=Prerequisite\n"
        "[Building]\n-=Prerequisite.X\n[Tank]\n-=Prerequisite\n"
        "[tAnK]\n-=prEREQUISITE\n");
    const auto plan = Build(reader, { "root.ini" }, "repeated -= keys");
    CheckCommands(plan, {
        { "Tank", "Prerequisite", "root.ini", 2 },
        { "Tank", "Armor", "root.ini", 3 },
        { "Tank", "Prerequisite", "root.ini", 4 },
        { "Building", "Prerequisite.X", "root.ini", 6 },
        { "Tank", "Prerequisite", "root.ini", 8 },
        { "tAnK", "prEREQUISITE", "root.ini", 10 }
    });
    CHECK(plan.files == 1);
}

void NameEqualityAndPathNormalization()
{
    CHECK(IniRemoval::EqualName("Prerequisite", "pREREQUISITE"));
    CHECK(IniRemoval::EqualName("#INCLUDE", "#include"));
    CHECK(IniRemoval::EqualName("", ""));
    CHECK(!IniRemoval::EqualName("Prerequisite", "Prerequisite.X"));
    CHECK(!IniRemoval::EqualName("X", "XX"));
    CHECK(!IniRemoval::EqualName("Armor", "Armoq"));
    CHECK(!IniRemoval::EqualName("Name", "Name "));
    CHECK(IniRemoval::EqualName("\xD5\xBD\xB3\xB5" "A", "\xD5\xBD\xB3\xB5" "a"));
    CHECK(!IniRemoval::EqualName("\xC0", "\xE0")); // byte-stable, not locale folding
    const std::vector<std::pair<std::string, std::string>> paths {
        { "", "" }, { ".", "" }, { "./a.ini", "a.ini" },
        { "a//./b/../c.ini", "a/c.ini" },
        { "../a/../../b.ini", "../../b.ini" },
        { "a/../../b.ini", "../b.ini" },
        { "/../../a.ini", "/a.ini" },
        { "C:\\Mods\\.\\child\\..\\Root.ini", "C:/Mods/Root.ini" },
        { "C:mods/../Root.ini", "C:Root.ini" },
        { "\\\\server\\share\\.\\sub\\..\\Root.ini", "//server/share/Root.ini" },
        { "/a/../", "/" }
    };
    for (const auto& [input, expected] : paths) {
        const auto actual = IniRemoval::NormalizePath(input);
        Require(actual == expected, "NormalizePath('" + input + "'): expected '" +
            expected + "', got '" + actual + "'");
    }
}

void IncludeOrderingAndRepetition()
{
    MemoryReader reader;
    reader.Add("dir/root.ini",
        "[#InClUdE]\n+=first.ini\nnamed=second.ini\n0=first.ini\n"
        "[Root]\n-=RootFirst\n[#include]\nextra=third.ini\n[Root]\n-=RootLast\n");
    reader.Add("dir/first.ini", "[First]\n-=One\n[#include]\n+=grand.ini\n");
    reader.Add("dir/grand.ini", "[Grand]\n-=Two\n");
    reader.Add("dir/second.ini", "[Second]\n-=Three\n");
    reader.Add("dir/third.ini", "[Third]\n-=Four\n");
    const auto plan = Build(reader, { "dir/root.ini" }, "body-first/depth-first includes");
    CheckCommands(plan, {
        { "Root", "RootFirst", "dir/root.ini", 6 },
        { "Root", "RootLast", "dir/root.ini", 10 },
        { "First", "One", "dir/first.ini", 2 },
        { "Grand", "Two", "dir/grand.ini", 2 },
        { "Second", "Three", "dir/second.ini", 2 },
        { "First", "One", "dir/first.ini", 2 },
        { "Grand", "Two", "dir/grand.ini", 2 },
        { "Third", "Four", "dir/third.ini", 2 }
    });
    CHECK(plan.files == 7);
    const std::vector<MemoryReader::Call> calls {
        { "dir/root.ini", "" }, { "first.ini", "dir/root.ini" },
        { "grand.ini", "dir/first.ini" }, { "second.ini", "dir/root.ini" },
        { "first.ini", "dir/root.ini" }, { "grand.ini", "dir/first.ini" },
        { "third.ini", "dir/root.ini" }
    };
    CHECK(reader.calls == calls);
}

void IncludeResolutionAndQuotes()
{
    MemoryReader reader;
    reader.AddResolved("entry.ini", "resolved/entry.ini",
        "[#include] ; trailing header comment\n"
        "+ = \"local ; # name.ini\" ; outside quote\n"
        "named = 'engine # ; name.ini' # outside quote\n"
        "2 = plain.ini;comment\n"
        "3 = C:\\mix\\extra.ini # absolute path\n"
        "[Target] # comment\n-=Armor;comment\n-=Strength #comment\n");
    reader.Add("resolved/local ; # name.ini", "[Local]\n-=Key\n");
    reader.Add("local ; # name.ini", "[WrongFallback]\nFoo=no\n");
    reader.Add("engine # ; name.ini", "[Engine]\n-=Key\n");
    reader.Add("resolved/plain.ini", "[Plain]\n-=Key\n");
    reader.Add("C:\\mix\\extra.ini", "[Absolute]\n-=Key\n");
    const auto plan = Build(reader, { "entry.ini" }, "quoted comments and Reader context");
    CheckCommands(plan, {
        { "Target", "Armor", "resolved/entry.ini", 7 },
        { "Target", "Strength", "resolved/entry.ini", 8 },
        { "Local", "Key", "resolved/local ; # name.ini", 2 },
        { "Engine", "Key", "engine # ; name.ini", 2 },
        { "Plain", "Key", "resolved/plain.ini", 2 },
        { "Absolute", "Key", "C:\\mix\\extra.ini", 2 }
    });
    CHECK(plan.files == 5);
    CHECK(reader.calls[1].second == "resolved/entry.ini");
}

void EncodingsWhitespaceCommentsAndNewlines()
{
    // Byte escapes keep the fixtures identical in GCC and MSVC source encodings.
    const std::string utf8Space = "\xE3\x80\x80";
    const std::string gbkSpace = "\xA1\xA1";
    const std::vector<std::string> comments {
        ";", "#", "\xEF\xBC\x9B", "\xEF\xBC\x83", "\xA3\xBB", "\xA3\xA3"
    };
    for (std::size_t i = 0; i < comments.size(); ++i) {
        const auto& comment = comments[i];
        const std::string quotedName = "quoted" + comment + "name.ini";
        MemoryReader reader;
        reader.Add("encoding.ini", std::string("\xEF\xBB\xBF") +
            utf8Space + comment + " \xD6\xD0\xCE\xC4 ANSI/GBK comment\r\n" +
            gbkSpace + "[" + utf8Space + "Tank" + gbkSpace + "]" + utf8Space + comment + "header\r\n" +
            utf8Space + "-" + gbkSpace + "=" + utf8Space + "Armor" + gbkSpace + comment + "tail\r\n" +
            gbkSpace + "- = Strength" + utf8Space + "\r" +
            "[#include]\n+ = \"" + quotedName + "\"" + gbkSpace + comment + "quoted tail\n" +
            "0 = plain.ini" + comment + "unquoted tail\n");
        reader.Add(quotedName, "[Quoted]\n-=Key\n");
        reader.Add("plain.ini", "[Plain]\n-=Key\n");
        const auto plan = Build(reader, { "encoding.ini" }, "comment marker " + std::to_string(i));
        CheckCommands(plan, {
            { "Tank", "Armor", "encoding.ini", 3 },
            { "Tank", "Strength", "encoding.ini", 4 },
            { "Quoted", "Key", quotedName, 2 },
            { "Plain", "Key", "plain.ini", 2 }
        });
        CHECK(plan.files == 3);
    }
    MemoryReader reader;
    const std::string gbkSection = "\xD5\xBD\xB3\xB5";
    const std::string gbkProperty = "\xBB\xA4\xBC\xD7";
    const std::string ansiProperty = "Cl" "\xE9";
    reader.Add("ansi.ini", "[" + gbkSection + "]\r\n-= " + gbkProperty +
        "\r\n-= " + ansiProperty); // no final newline
    CheckCommands(Build(reader, { "ansi.ini" }, "ANSI/GBK exact identifier bytes"), {
        { gbkSection, gbkProperty, "ansi.ini", 2 },
        { gbkSection, ansiProperty, "ansi.ini", 3 }
    });
}

struct BadManifest {
    std::string label;
    std::string text;
    std::string diagnostic;
    std::size_t line;
};

void RejectManifests(const std::vector<BadManifest>& cases)
{
    for (const auto& test : cases) {
        MemoryReader reader;
        reader.Add("bad.ini", test.text);
        const auto error = BuildFails(reader, { "bad.ini" }, test.label, test.diagnostic);
        CheckLocation(error, "bad.ini", test.line);
    }
}

void InvalidBodyCommands()
{
    RejectManifests({
        { "append in body", "[Tank]\n+=Armor\n", "only -=property", 2 },
        { "assignment in body", "[Tank]\nFoo=no\n", "only -=property", 2 },
        { "restore assignment", "[Tank]\nArmor=old\n", "only -=property", 2 },
        { "wrong operator", "[Tank]\n--=Armor\n", "only -=property", 2 },
        { "empty key", "[Tank]\n-=\n", "one exact, nonempty", 2 },
        { "comment-only key", "[Tank]\n-= ; comment\n", "one exact, nonempty", 2 },
        { "wildcard key", "[Tank]\n-=*\n", "one exact, nonempty", 2 },
        { "wildcard suffix", "[Tank]\n-=Prerequisite.*\n", "one exact, nonempty", 2 },
        { "question wildcard", "[Tank]\n-=Arm?r\n", "one exact, nonempty", 2 },
        { "comma list", "[Tank]\n-=Armor,Strength\n", "one exact, nonempty", 2 },
        { "space list", "[Tank]\n-=Armor Strength\n", "one exact, nonempty", 2 },
        { "tab list", "[Tank]\n-=Armor\tStrength\n", "one exact, nonempty", 2 },
        { "UTF8 fullwidth space list", "[Tank]\n-=Armor\xE3\x80\x80Strength\n", "one exact, nonempty", 2 },
        { "GBK fullwidth space list", "[Tank]\n-=Armor\xA1\xA1Strength\n", "one exact, nonempty", 2 },
        { "quoted key", "[Tank]\n-=\"Armor\"\n", "one exact, nonempty", 2 },
        { "single quoted key", "[Tank]\n-='Armor'\n", "one exact, nonempty", 2 },
        { "unclosed body quote", "[Tank]\n-=\"Armor\n", "one exact, nonempty", 2 },
        { "assignment as key", "[Tank]\n-=Armor=no\n", "one exact, nonempty", 2 },
        { "path as key", "[Tank]\n-=Armor/Strength\n", "one exact, nonempty", 2 },
        { "backslash as key", "[Tank]\n-=Armor\\Strength\n", "one exact, nonempty", 2 },
        { "bracket as key", "[Tank]\n-=[Armor]\n", "one exact, nonempty", 2 },
        { "control byte", "[Tank]\n-=Arm\x01or\n", "one exact, nonempty", 2 },
        { "DEL byte", "[Tank]\n-=Arm\x7For\n", "one exact, nonempty", 2 }
    });
}

void InvalidHeadersAndText()
{
    RejectManifests({
        { "outside section", "-=Armor\n", "outside a section", 1 },
        { "stray opening text", "stray text\n[Tank]\n-=Armor\n", "outside a section", 1 },
        { "stray body text", "[Tank]\nstray text\n", "expected -=property", 2 },
        { "missing equals", "[Tank]\n-Armor\n", "expected -=property", 2 },
        { "unclosed header", "[Tank\n-=Armor\n", "invalid section header", 1 },
        { "header suffix", "[Tank] trailing\n", "unexpected text after section", 1 },
        { "extra bracket", "[Tank]]\n", "unexpected text after section", 1 },
        { "empty header", "[]\n", "invalid or wildcard section", 1 },
        { "blank header", "[ \t ]\n", "invalid or wildcard section", 1 },
        { "wildcard header", "[*]\n", "invalid or wildcard section", 1 },
        { "question header", "[Tank?]\n", "invalid or wildcard section", 1 },
        { "list header", "[Tank,Building]\n", "invalid or wildcard section", 1 },
        { "space header", "[Tank Building]\n", "invalid or wildcard section", 1 },
        { "quoted header", "[\"Tank\"]\n", "invalid or wildcard section", 1 },
        { "nested header", "[[Tank]\n", "invalid or wildcard section", 1 },
        { "comment in header", "[Tank;comment]\n", "invalid or wildcard section", 1 },
        { "UTF16 LE", std::string("\xFF\xFE") + "[Tank]\n", "UTF-16", 1 },
        { "UTF16 BE", std::string("\xFE\xFF") + "[Tank]\n", "UTF-16", 1 },
        { "NUL in body", std::string("[Tank]\n-=Ar") + '\0' + "mor\n", "NUL byte", 2 },
        { "NUL in comment", std::string("; comment") + '\0' + "\n", "NUL byte", 1 },
        { "late BOM", "\n\xEF\xBB\xBF[Tank]\n", "outside a section", 2 },
        { "CRLF diagnostic line", "\r\n;comment\r\n[Tank]\r\nFoo=no\r\n", "only -=property", 4 }
    });
}

void InvalidIncludeSyntax()
{
    RejectManifests({
        { "delete in includes", "[#include]\n-=child.ini\n", "invalid include assignment", 2 },
        { "empty include assignment", "[#include]\n=child.ini\n", "invalid include assignment", 2 },
        { "wildcard include assignment", "[#include]\n*=child.ini\n", "invalid include assignment", 2 },
        { "empty include path", "[#include]\n+=\n", "invalid or overlong include path", 2 },
        { "comment-only include path", "[#include]\n+=#comment\n", "invalid or overlong include path", 2 },
        { "empty quoted include", "[#include]\n+=\"\"\n", "invalid or overlong include path", 2 },
        { "empty single quoted include", "[#include]\n+='';comment\n", "invalid or overlong include path", 2 },
        { "unclosed double quote", "[#include]\n+=\"child.ini\n", "unclosed include path quote", 2 },
        { "unclosed single quote", "[#include]\n+='child.ini\n", "unclosed include path quote", 2 },
        { "quote mismatch", "[#include]\n+='child.ini\"\n", "unclosed include path quote", 2 },
        { "quoted include suffix", "[#include]\n+=\"child.ini\" junk\n", "unexpected text after include path", 2 },
        { "two paths after quote", "[#include]\n+='a.ini','b.ini'\n", "unexpected text after include path", 2 },
        { "include glob", "[#include]\n+=*.ini\n", "invalid or overlong include path", 2 },
        { "include question glob", "[#include]\n+=child?.ini\n", "invalid or overlong include path", 2 },
        { "include pipe", "[#include]\n+=child|other.ini\n", "invalid or overlong include path", 2 },
        { "include angle bracket", "[#include]\n+=<child.ini>\n", "invalid or overlong include path", 2 },
        { "include equals", "[#include]\n+=child=other.ini\n", "invalid or overlong include path", 2 },
        { "include tab", "[#include]\n+='child\tname.ini'\n", "invalid or overlong include path", 2 },
        { "include control byte", "[#include]\n+='child\x01name.ini'\n", "control character", 2 },
        { "include DEL byte", "[#include]\n+='child\x7Fname.ini'\n", "control character", 2 }
    });
}

void ForbiddenRegistries()
{
    const std::vector<std::string> registries {
        "InfantryTypes", "VehicleTypes", "AircraftTypes", "BuildingTypes",
        "TerrainTypes", "SmudgeTypes", "OverlayTypes", "Animations", "VoxelAnims",
        "Warheads", "Particles", "ParticleSystems", "WeaponTypes", "Projectiles",
        "Projectile", "SuperWeaponTypes", "Countries", "Sides", "AITriggerTypes",
        "AITriggerTypesEnable", "TeamTypes", "TaskForces", "ScriptTypes",
        "TriggerTypes", "Triggers", "Tags", "Colors", "ColorAdd"
    };
    for (auto registry : registries) {
        // Check canonical and mixed case, including a header with no commands.
        for (int variation = 0; variation < 2; ++variation) {
            MemoryReader reader;
            reader.Add("registry.ini", "[" + registry + "]\n" +
                (variation == 0 ? "-=0\n" : ""));
            CheckLocation(BuildFails(reader, { "registry.ini" }, "forbidden " + registry,
                "registry sections cannot be removed"), "registry.ini", 1);
            for (char& c : registry) {
                if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
                else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
            }
        }
    }
    MemoryReader reader;
    reader.Add("near.ini", "[VehicleTypes.X]\n-=Foo\n[MyWarheads]\n-=Bar\n");
    CHECK(Build(reader, { "near.ini" }, "registry names must match exactly").commands.size() == 2);
}

void MissingAndFailedReads()
{
    MemoryReader reader;
    CheckLocation(BuildFails(reader, { "missing.ini" }, "missing root", "not found in memory"),
        "missing.ini", 0);
    reader.Add("dir/root.ini", "[Tank]\n-=Armor\n[#include]\n+=missing.ini\n");
    CheckLocation(BuildFails(reader, { "dir/root.ini" }, "missing include", "cannot read missing.ini"),
        "dir/root.ini", 4);
    reader.FailRead("missing.ini", "simulated I/O failure");
    CheckLocation(BuildFails(reader, { "dir/root.ini" }, "failed child Reader", "simulated I/O failure"),
        "dir/root.ini", 4);
    reader.FailRead("broken.ini", "simulated root failure");
    CheckLocation(BuildFails(reader, { "broken.ini" }, "failed root Reader", "simulated root failure"),
        "broken.ini", 0);
    reader.AddResolved("empty-path.ini", "", "[Tank]\n-=Armor\n");
    CheckLocation(BuildFails(reader, { "empty-path.ini" }, "empty resolved path", "invalid resolved path"),
        "empty-path.ini", 0);
}

void CanonicalCycles()
{
    const std::vector<std::string> aliases {
        "dir/root.ini", "dir/./root.ini", "dir/child/../root.ini",
        "DIR\\child\\..\\.\\ROOT.INI"
    };
    for (const auto& alias : aliases) {
        MemoryReader reader;
        reader.Add("dir/root.ini", "[Root]\n-=Armor\n[#include]\n+=alias.ini\n");
        reader.AddResolved("dir/alias.ini", alias, "[NeverParsed]\n-=Key\n");
        CheckLocation(BuildFails(reader, { "dir/root.ini" }, "canonical cycle " + alias,
            "include cycle"), "dir/root.ini", 4);
        CHECK(reader.calls.size() == 2);
    }
    MemoryReader reader;
    reader.Add("dir/root.ini", "[#include]\n+=child.ini\n");
    reader.Add("dir/child.ini", "[Child]\n-=Key\n[#include]\n+=back.ini\n");
    reader.AddResolved("dir/back.ini", "dir/nested/.././ROOT.INI", "");
    CheckLocation(BuildFails(reader, { "dir/root.ini" }, "cycle through descendant", "include cycle"),
        "dir/child.ini", 4);
    CHECK(reader.calls.size() == 3);
}

void DepthBoundary()
{
    MemoryReader reader;
    for (std::size_t i = 0; i < IniRemoval::kMaxDepth; ++i) {
        std::string body = "[Depth]\n-=K" + std::to_string(i) + "\n";
        if (i + 1 < IniRemoval::kMaxDepth)
            body += "[#include]\n+=d" + std::to_string(i + 1) + ".ini\n";
        reader.Add("d" + std::to_string(i) + ".ini", std::move(body));
    }
    const auto plan = Build(reader, { "d0.ini" }, "maximum include depth");
    CHECK(plan.files == IniRemoval::kMaxDepth);
    CHECK(plan.commands.size() == IniRemoval::kMaxDepth);
    for (std::size_t i = 0; i < plan.commands.size(); ++i)
        CHECK(plan.commands[i].key == "K" + std::to_string(i));

    const auto last = "d" + std::to_string(IniRemoval::kMaxDepth - 1) + ".ini";
    reader.Add(last, "[Depth]\n-=Last\n[#include]\n+=too-deep.ini\n");
    reader.Add("too-deep.ini", "");
    reader.calls.clear();
    CheckLocation(BuildFails(reader, { "d0.ini" }, "depth overflow", "include depth"), last, 4);
    CHECK(reader.calls.size() == IniRemoval::kMaxDepth); // fail before another read
}

void RootReaderAndPathLimits()
{
    MemoryReader reader;
    Plan plan = StalePlan();
    Error error { "stale", 1, "stale" };
    CHECK(!IniRemoval::BuildPlan({}, nullptr, nullptr, plan, error));
    CHECK(plan.commands.empty() && plan.files == 0);
    CHECK(error.file.empty() && error.line == 0 && error.message.find("invalid reader") != std::string::npos);

    reader.Add("root.ini", "[Tank]\n-=Armor\n");
    // Root cap is currently part of BuildPlan's contract but not an API constant.
    std::vector<std::string> roots(256, "root.ini");
    CHECK(Build(reader, roots, "256 roots").files == roots.size());
    roots.push_back("root.ini");
    reader.calls.clear();
    CheckLocation(BuildFails(reader, roots, "257 roots", "too many root files"), "", 0);
    CHECK(reader.calls.empty());

    CheckLocation(BuildFails(reader, { "" }, "empty root path", "empty or overlong include path"), "", 0);
    const std::string longest(IniRemoval::kMaxPath - 1, 'a');
    const std::string tooLong(IniRemoval::kMaxPath, 'a');
    reader.Add(longest, "[Tank]\n-=Armor\n");
    CHECK(Build(reader, { longest }, "maximum valid root path").files == 1);
    reader.calls.clear();
    CheckLocation(BuildFails(reader, { tooLong }, "overlong root path", "empty or overlong include path"), tooLong, 0);
    CHECK(reader.calls.empty());
    reader.Add("include.ini", "[#include]\n+=" + longest + "\n");
    CHECK(Build(reader, { "include.ini" }, "maximum valid include path").files == 2);
    reader.Add("include.ini", "[#include]\n+=" + tooLong + "\n");
    CheckLocation(BuildFails(reader, { "include.ini" }, "overlong include path", "invalid or overlong include path"),
        "include.ini", 2);
    reader.AddResolved("alias.ini", tooLong, "");
    CheckLocation(BuildFails(reader, { "alias.ini" }, "overlong resolved path", "invalid resolved path"), "alias.ini", 0);
}

void TokenLimits()
{
    MemoryReader reader;
    const std::string valid(IniRemoval::kMaxToken - 1, 'T');
    const std::string invalid(IniRemoval::kMaxToken, 'T');
    reader.Add("token.ini", "[" + valid + "]\n-=" + valid + "\n");
    CheckCommands(Build(reader, { "token.ini" }, "maximum valid token sizes"), {
        { valid, valid, "token.ini", 2 }
    });
    reader.Add("token.ini", "[" + invalid + "]\n-=Armor\n");
    CheckLocation(BuildFails(reader, { "token.ini" }, "overlong section", "invalid or wildcard section"), "token.ini", 1);
    reader.Add("token.ini", "[Tank]\n-=" + invalid + "\n");
    CheckLocation(BuildFails(reader, { "token.ini" }, "overlong key", "one exact, nonempty"), "token.ini", 2);
    reader.Add("token.ini", "[#include]\n" + valid + "=child.ini\n");
    reader.Add("child.ini", "");
    CHECK(Build(reader, { "token.ini" }, "maximum include assignment token").files == 2);
    reader.Add("token.ini", "[#include]\n" + invalid + "=child.ini\n");
    CheckLocation(BuildFails(reader, { "token.ini" }, "overlong include assignment", "invalid include assignment"), "token.ini", 2);
}

std::string Repeated(const std::string& text, std::size_t count)
{
    std::string result;
    result.reserve(text.size() * count);
    for (std::size_t i = 0; i < count; ++i) result += text;
    return result;
}

void FileAndIncludeEntryLimits()
{
    MemoryReader reader;
    const auto includes = Repeated("+=leaf.ini\n", IniRemoval::kMaxFiles - 1);
    reader.Add("root.ini", "[#include]\n" + includes);
    reader.Add("leaf.ini", "");
    const auto plan = Build(reader, { "root.ini" }, "maximum loaded file count");
    CHECK(plan.files == IniRemoval::kMaxFiles && plan.commands.empty());
    CHECK(reader.calls.size() == IniRemoval::kMaxFiles);

    // Exactly kMaxFiles include entries parse, but the root also consumes a file.
    reader.Add("root.ini", "[#include]\n" + includes + "+=leaf.ini\n");
    reader.calls.clear();
    CheckLocation(BuildFails(reader, { "root.ini" }, "loaded file overflow", "loaded file limit"),
        "root.ini", IniRemoval::kMaxFiles + 1);
    CHECK(reader.calls.size() == IniRemoval::kMaxFiles);

    reader.Add("root.ini", "[#include]\n" + includes + "+=leaf.ini\n+=leaf.ini\n");
    reader.calls.clear();
    CheckLocation(BuildFails(reader, { "root.ini" }, "include entry overflow", "include entry limit"),
        "root.ini", IniRemoval::kMaxFiles + 2);
    CHECK(reader.calls.size() == 1); // body parsing completes before children load
}

void CommandLimits()
{
    MemoryReader reader;
    const auto body = "[Tank]\n" + Repeated("-=Armor\n", IniRemoval::kMaxCommands);
    reader.Add("commands.ini", body);
    const auto plan = Build(reader, { "commands.ini" }, "maximum command count");
    CHECK(plan.files == 1 && plan.commands.size() == IniRemoval::kMaxCommands);
    CHECK(plan.commands.front().line == 2 && plan.commands.back().line == IniRemoval::kMaxCommands + 1);
    reader.Add("commands.ini", body + "-=Strength\n");
    CheckLocation(BuildFails(reader, { "commands.ini" }, "command overflow", "delete command limit"),
        "commands.ini", IniRemoval::kMaxCommands + 2);

    reader.Add("commands.ini", body + "[#include]\n+=extra.ini\n");
    reader.Add("extra.ini", "[Tank]\n-=Strength\n");
    CheckLocation(BuildFails(reader, { "commands.ini" }, "command cap shared with child", "delete command limit"),
        "extra.ini", 2);
    reader.Add("commands.ini", body);
    CheckLocation(BuildFails(reader, { "commands.ini", "extra.ini" }, "command cap shared with later root",
        "delete command limit"), "extra.ini", 2);
}

void ByteLimits()
{
    MemoryReader reader;
    // A single comment is cheap to parse and allows exact byte-boundary tests
    // without millions of lines or retaining 64 MiB of distinct fixture strings.
    const auto full = ";" + std::string(IniRemoval::kMaxFileBytes - 1, 'x');
    reader.Add("bytes.ini", full);
    const auto one = Build(reader, { "bytes.ini" }, "maximum file byte count");
    CHECK(one.files == 1 && one.commands.empty());
    reader.Add("bytes.ini", full + 'x');
    CheckLocation(BuildFails(reader, { "bytes.ini" }, "per-file byte overflow", "input byte limit"), "bytes.ini", 0);
    reader.Add("bytes.ini", full);

    CHECK(IniRemoval::kMaxTotalBytes % IniRemoval::kMaxFileBytes == 0);
    const std::size_t fullFiles = IniRemoval::kMaxTotalBytes / IniRemoval::kMaxFileBytes;
    std::vector<std::string> roots(fullFiles, "bytes.ini");
    const auto maximum = Build(reader, roots, "maximum aggregate byte count with repeated reads");
    CHECK(maximum.files == fullFiles && maximum.commands.empty());
    reader.Add("one-byte.ini", ";");
    roots.push_back("one-byte.ini");
    CheckLocation(BuildFails(reader, roots, "total byte overflow by one", "input byte limit"), "one-byte.ini", 0);

    // Includes and roots must draw from the same aggregate byte budget.
    const std::string rootText = "[#include]\n+=bytes.ini\n";
    reader.Add("include.ini", rootText);
    reader.Add("remainder.ini", ";" + std::string(IniRemoval::kMaxFileBytes - rootText.size() - 1, 'x'));
    roots.assign(fullFiles - 2, "bytes.ini");
    roots.push_back("remainder.ini");
    roots.push_back("include.ini");
    CHECK(Build(reader, roots, "exact byte budget across roots and child").files == fullFiles + 1);
    reader.Add("remainder.ini", ";" + std::string(IniRemoval::kMaxFileBytes - rootText.size(), 'x'));
    CheckLocation(BuildFails(reader, roots, "aggregate byte overflow in child", "input byte limit"), "bytes.ini", 0);
}

// This is deliberately a target simulation, NOT an engine/default restoration
// test. A successful plan only erases exact, case-insensitive property names.
// find_if avoids map::operator[], which would create missing sections/keys.
using Properties = std::map<std::string, std::string>;
using Target = std::map<std::string, Properties>;

void ApplySuccessfulPlan(Target& target, const Plan& plan)
{
    for (const auto& command : plan.commands) {
        const auto section = std::find_if(target.begin(), target.end(), [&](const auto& item) {
            return IniRemoval::EqualName(item.first, command.section);
        });
        if (section == target.end()) continue;
        const auto key = std::find_if(section->second.begin(), section->second.end(), [&](const auto& item) {
            return IniRemoval::EqualName(item.first, command.key);
        });
        if (key != section->second.end()) section->second.erase(key);
    }
}

bool BuildThenApply(MemoryReader& reader, const std::vector<std::string>& roots,
    Target& target, Plan& plan, Error& error, std::size_t& applicationCount)
{
    if (!IniRemoval::BuildPlan(roots, MemoryReader::Read, &reader, plan, error)) return false;
    ++applicationCount;
    ApplySuccessfulPlan(target, plan);
    return true;
}

void TransactionalFailuresAndStalePlans()
{
    const Target original {
        { "Tank", { { "Armor", "heavy" }, { "Strength", "500" }, { "Keep", "unchanged" } } },
        { "Other", { { "Key", "value" } } }, { "Empty", {} }
    };
    const std::vector<std::pair<std::string, std::vector<std::string>>> scenarios {
        { "invalid later root", { "good.ini", "bad.ini" } },
        { "missing later root", { "good.ini", "missing.ini" } },
        { "failed later root read", { "good.ini", "unreadable.ini" } },
        { "invalid later child", { "children.ini" } },
        { "missing later child", { "missing-child.ini" } },
        { "failed later child read", { "failed-child.ini" } },
        { "invalid grandchild", { "nested.ini" } },
        { "invalid body after include", { "bad-body.ini" } }
    };
    for (const auto& [label, roots] : scenarios) {
        MemoryReader reader;
        reader.Add("good.ini", "[Tank]\n-=Armor\n");
        reader.Add("child.ini", "[Tank]\n-=Strength\n");
        reader.Add("bad.ini", "[Tank]\n-=Keep\nFoo=no\n");
        reader.FailRead("unreadable.ini", "simulated failure");
        reader.Add("children.ini", "[Tank]\n-=Armor\n[#include]\n+=child.ini\n+=bad.ini\n");
        reader.Add("missing-child.ini", "[Tank]\n-=Armor\n[#include]\n+=child.ini\n+=missing.ini\n");
        reader.Add("failed-child.ini", "[Tank]\n-=Armor\n[#include]\n+=child.ini\n+=unreadable.ini\n");
        reader.Add("nested.ini", "[Tank]\n-=Armor\n[#include]\n+=children.ini\n");
        reader.Add("bad-body.ini", "[#include]\n+=child.ini\n[Tank]\n-=Armor\nFoo=no\n");

        // Use an actual previous success, not just a synthetic stale plan.
        Plan plan = Build(reader, { "good.ini" }, label + " previous successful plan");
        CHECK(!plan.commands.empty() && plan.files == 1);
        reader.calls.clear();
        Target target = original;
        Error error;
        std::size_t applicationCount = 0;
        Require(!BuildThenApply(reader, roots, target, plan, error, applicationCount),
            label + ": invalid batch unexpectedly succeeded");
        Require(target == original, label + ": target was partially modified");
        Require(applicationCount == 0, label + ": application ran before complete validation");
        Require(plan.commands.empty() && plan.files == 0, label + ": stale/partial plan survived failure");
        Require(!error.message.empty(), label + ": no diagnostic");
        if (label == "invalid body after include") CHECK(reader.calls.size() == 1);

        // Failure state must not poison a later success using the same outputs.
        CHECK(BuildThenApply(reader, { "good.ini" }, target, plan, error, applicationCount));
        CHECK(applicationCount == 1 && plan.files == 1 && plan.commands.size() == 1);
        CHECK(error.message.empty() && error.file.empty() && error.line == 0);
        Target expected = original;
        expected.at("Tank").erase("Armor");
        CHECK(target == expected);
    }
}

void ExactIdempotentDeletionWithoutRestoration()
{
    MemoryReader reader;
    reader.Add("delete.ini",
        "[tAnK]\n-=pREREQUISITE\n-=Prerequisite\n-=AbsentProperty\n"
        "-=Armor\n[AbsentSection]\n-=AbsentProperty\n[Empty]\n-=Missing\n"
        "[LastProperty]\n-=Only\n");
    Target target {
        { "Tank", { { "Prerequisite", "BARRACKS" }, { "Prerequisite.X", "FACTORY" },
                    { "Armor", "heavy" }, { "Keep", "yes" }, { "AlreadyEmpty", "" } } },
        { "Other", { { "Armor", "light" }, { "Keep", "untouched" } } },
        { "Empty", {} }, { "LastProperty", { { "Only", "last" } } }
    };
    Target expected = target;
    expected.at("Tank").erase("Prerequisite");
    expected.at("Tank").erase("Armor");
    expected.at("LastProperty").erase("Only");
    Plan plan;
    Error error;
    std::size_t applicationCount = 0;
    CHECK(BuildThenApply(reader, { "delete.ini" }, target, plan, error, applicationCount));
    CHECK(applicationCount == 1 && plan.commands.size() == 7 && plan.files == 1);
    CHECK(target == expected);
    CHECK(target.at("Tank").at("Prerequisite.X") == "FACTORY");
    CHECK(target.at("Tank").count("Prerequisite") == 0 && target.at("Tank").count("Armor") == 0);
    CHECK(target.count("AbsentSection") == 0);
    CHECK(target.at("Empty").empty() && target.at("LastProperty").empty());
    ApplySuccessfulPlan(target, plan);
    CHECK(target == expected); // duplicate/absent deletes are idempotent no-ops

    reader.Add("qualified.ini", "[Tank]\n-=Prerequisite.X\n");
    Target qualified { { "Tank", { { "Prerequisite", "base" }, { "Prerequisite.X", "extension" } } } };
    const Target qualifiedExpected { { "Tank", { { "Prerequisite", "base" } } } };
    ApplySuccessfulPlan(qualified, Build(reader, { "qualified.ini" }, "qualified key is exact too"));
    CHECK(qualified == qualifiedExpected);
}

} // namespace

int main()
{
    const std::vector<std::pair<const char*, void (*)()>> tests {
        { "empty input and caller root order", EmptyInputAndRootOrder },
        { "repeated -= keys and multiple sections", RepeatedKeysAndSections },
        { "case-insensitive exact names and canonical paths", NameEqualityAndPathNormalization },
        { "body-first/depth-first and repeated includes", IncludeOrderingAndRepetition },
        { "relative/fallback Reader context and quoted comments", IncludeResolutionAndQuotes },
        { "UTF8 BOM, ANSI/GBK, fullwidth whitespace/comments and CRLF", EncodingsWhitespaceCommentsAndNewlines },
        { "invalid body assignments, empty/wildcard/list properties", InvalidBodyCommands },
        { "bad headers, stray text, encodings and line diagnostics", InvalidHeadersAndText },
        { "invalid include syntax and unclosed quotes", InvalidIncludeSyntax },
        { "forbidden registry sections", ForbiddenRegistries },
        { "missing includes and Reader failures", MissingAndFailedReads },
        { "canonical dot/dot-dot and case/slash cycles", CanonicalCycles },
        { "depth boundary", DepthBoundary },
        { "root count, Reader and path limits", RootReaderAndPathLimits },
        { "section/property/include token limits", TokenLimits },
        { "file count and include entry limits", FileAndIncludeEntryLimits },
        { "command count limits across files", CommandLimits },
        { "file and aggregate byte limits", ByteLimits },
        { "transactional failures, stale plans and recovery", TransactionalFailuresAndStalePlans },
        { "exact idempotent deletions without assignment/restoration", ExactIdempotentDeletionWithoutRestoration }
    };
    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << exception.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": unknown exception\n";
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " test groups passed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
