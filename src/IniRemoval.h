#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Engine-independent, strict parser for remove manifests. No INI writes occur
// here: repeated -= lines are commands, not duplicate keys named "-".
namespace IniRemoval {

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

    struct Command {
        std::string section;
        std::string key;
        std::string file;
        std::size_t line = 0;
    };

    struct Plan {
        std::vector<Command> commands;
        std::size_t files = 0;
    };

    struct Error {
        std::string file;
        std::size_t line = 0;
        std::string message;
    };

    // The reader tries relative to the containing file first, then the game
    // directory/engine filesystem. On failure it supplies a diagnostic.
    using Reader = bool (*)(const std::string& request,
                            const std::string& containingFile,
                            Source& source, std::string& error, void* context);

    bool EqualName(const std::string& left, const std::string& right);
    std::string NormalizePath(const std::string& path);

    // roots must already be sorted by the caller. On ANY failure output is
    // empty. Only a successfully built plan may be applied to a live INI.
    bool BuildPlan(const std::vector<std::string>& roots, Reader reader,
                   void* context, Plan& output, Error& error);

} // namespace IniRemoval
