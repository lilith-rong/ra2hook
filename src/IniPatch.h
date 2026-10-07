#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Startup-only ordered patch plan. Unlike an INI dictionary, this preserves
// repeated assignments, += and -=, and expands includes at their source line.
// Runtime keeps its existing IniOverlay parser and does not consume this plan.
namespace IniPatch {

    constexpr std::size_t kMaxFileBytes = 8 * 1024 * 1024;
    constexpr std::size_t kMaxTotalBytes = 64 * 1024 * 1024;
    constexpr std::size_t kMaxFiles = 4096;
    constexpr std::size_t kMaxCommands = 65536;
    constexpr std::size_t kMaxDepth = 32;
    constexpr std::size_t kMaxPath = 260;
    constexpr std::size_t kMaxToken = 512;

    struct Source {
        std::string path; // Resolved disk or engine/MIX name, used for includes.
        std::string text;
    };

    enum class Operation { Set, Append, Remove };

    struct Command {
        Operation operation = Operation::Set;
        std::string section;
        std::string key;   // Empty for Append: the executor allocates a fresh key.
        std::string value; // Empty for Remove; empty Set values are legal.
        std::string file;
        std::size_t line = 0;
    };

    struct Plan {
        std::vector<Command> commands;
        std::size_t files = 0;
        bool allowRemoval = false;
    };

    struct Error {
        std::string file;
        std::size_t line = 0;
        std::string message;
    };

    using Reader = bool (*)(const std::string& request,
                            const std::string& containingFile,
                            Source& source, std::string& error, void* context);

    bool EqualName(const std::string& left, const std::string& right);
    std::string NormalizePath(const std::string& path);
    bool IsRemovalTarget(const std::string& section, const std::string& key);

    // roots are supplied in sorted order. Includes execute exactly where they
    // occur, then the caller resumes its own section/line. ANY read/parse failure
    // leaves output empty. No engine mutation takes place while building a plan.
    // Removal is opt-in and must only be enabled for startup INI_Rules.
    bool BuildPlan(const std::vector<std::string>& roots, Reader reader,
                   void* context, Plan& output, Error& error,
                   bool allowRemoval = false);

} // namespace IniPatch
