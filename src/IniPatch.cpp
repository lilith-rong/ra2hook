#include "IniPatch.h"

#include <string_view>
#include <utility>

namespace IniPatch {
namespace {
    char Fold(char c)
    {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
    }

    std::size_t SpaceWidth(std::string_view text)
    {
        if (text.empty()) return 0;
        if (text[0] == ' ' || text[0] == '\t') return 1;
        if (text.substr(0, 3) == "\xE3\x80\x80") return 3;
        if (text.substr(0, 2) == "\xA1\xA1") return 2;
        return 0;
    }

    std::string_view Trim(std::string_view text)
    {
        while (const auto width = SpaceWidth(text)) text.remove_prefix(width);
        while (!text.empty()) {
            if (text.back() == ' ' || text.back() == '\t') text.remove_suffix(1);
            else if (text.size() >= 3 && text.substr(text.size() - 3) == "\xE3\x80\x80")
                text.remove_suffix(3);
            else if (text.size() >= 2 && text.substr(text.size() - 2) == "\xA1\xA1")
                text.remove_suffix(2);
            else break;
        }
        return text;
    }

    bool IsComment(std::string_view text)
    {
        return !text.empty() && (text[0] == ';' || text[0] == '#' ||
            text.substr(0, 3) == "\xEF\xBC\x9B" ||
            text.substr(0, 3) == "\xEF\xBC\x83" ||
            text.substr(0, 2) == "\xA3\xBB" ||
            text.substr(0, 2) == "\xA3\xA3");
    }

    std::string_view WithoutComment(std::string_view text)
    {
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (IsComment(text.substr(i))) return Trim(text.substr(0, i));
        }
        return Trim(text);
    }

    bool IsIdentifier(std::string_view text)
    {
        if (text.empty() || text.size() >= kMaxToken) return false;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (c <= 0x20 || c == 0x7F || SpaceWidth(text.substr(i)) ||
                IsComment(text.substr(i)) ||
                std::string_view("[]=\"'*?,/\\").find(text[i]) != std::string_view::npos)
                return false;
        }
        return true;
    }

    bool IsRegistry(const std::string& name)
    {
        // Removing list members is not property deletion and cannot unregister
        // objects that Read_File has already allocated before our hook.
        static const char* const registries[] = {
            "InfantryTypes", "VehicleTypes", "AircraftTypes", "BuildingTypes",
            "TerrainTypes", "SmudgeTypes", "OverlayTypes", "Animations",
            "VoxelAnims", "Warheads", "Particles", "ParticleSystems",
            "WeaponTypes", "Projectiles", "Projectile", "SuperWeaponTypes",
            "Countries", "Sides", "AITriggerTypes", "AITriggerTypesEnable",
            "TeamTypes", "TaskForces", "ScriptTypes", "TriggerTypes", "Triggers",
            "Tags", "Colors", "ColorAdd"
        };
        for (const char* registry : registries) {
            if (EqualName(name, registry)) return true;
        }
        return false;
    }

    // Ordinary INI names retain the overlay's broad vocabulary (e.g. $Inherits
    // and names containing spaces). Deletion uses the stricter exact-identifier
    // check above, and is checked per operation rather than per section header.
    bool IsIniName(std::string_view text)
    {
        if (text.empty() || text.size() >= kMaxToken) return false;
        for (const unsigned char c : text) {
            if (c < 0x20 || c == 0x7F || c == '[' || c == ']' || c == '=')
                return false;
        }
        return true;
    }

    class Builder {
    public:
        Builder(Reader reader, void* context, Error& error, bool allowRemoval)
            : reader_(reader), context_(context), error_(error)
        {
            plan.allowRemoval = allowRemoval;
        }

        Plan plan;

        bool Load(const std::string& request, const std::string& parent,
                  std::size_t includeLine)
        {
            const std::string& location = parent.empty() ? request : parent;
            if (request.empty() || request.size() >= kMaxPath)
                return Fail(location, includeLine, "empty or overlong include path");
            if (stack_.size() >= kMaxDepth)
                return Fail(location, includeLine, "include depth exceeds 32: " + request);
            if (plan.files >= kMaxFiles)
                return Fail(location, includeLine, "loaded file limit exceeded");

            Source source;
            std::string readError;
            if (!reader_(request, parent, source, readError, context_))
                return Fail(location, includeLine, "cannot read " + request + ": " + readError);
            if (source.path.empty() || source.path.size() >= kMaxPath)
                return Fail(location, includeLine, "invalid resolved path: " + source.path);
            const std::string identity = NormalizePath(source.path);
            for (const auto& ancestor : stack_) {
                if (EqualName(identity, ancestor))
                    return Fail(location, includeLine, "include cycle: " + source.path);
            }
            if (source.text.size() > kMaxFileBytes ||
                source.text.size() > kMaxTotalBytes - bytes_)
                return Fail(source.path, 0, "input byte limit exceeded");
            bytes_ += source.text.size();
            ++plan.files;
            stack_.push_back(identity);

            // Parse recursively at the include line, not after this file's
            // body. Each Parse call owns its section state and resumes on return.
            if (!Parse(source)) return false;
            stack_.pop_back();
            return true;
        }

    private:
        Reader reader_;
        void* context_;
        Error& error_;
        std::size_t bytes_ = 0;
        std::vector<std::string> stack_;

        bool Fail(const std::string& file, std::size_t line, std::string message)
        {
            error_ = { file, line, std::move(message) };
            return false;
        }

        bool Parse(const Source& source)
        {
            std::size_t includes = 0;
            std::string_view text(source.text);
            if (text.substr(0, 2) == "\xFF\xFE" || text.substr(0, 2) == "\xFE\xFF")
                return Fail(source.path, 1, "UTF-16 is not supported; use UTF-8 or ANSI");
            if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
            std::string section;
            std::size_t line = 0;
            while (!text.empty()) {
                ++line;
                const auto end = text.find_first_of("\r\n");
                const auto raw = text.substr(0, end);
                if (end == std::string_view::npos) text = {};
                else {
                    const char newline = text[end];
                    text.remove_prefix(end + 1);
                    if (newline == '\r' && !text.empty() && text[0] == '\n')
                        text.remove_prefix(1);
                }
                if (raw.find('\0') != std::string_view::npos)
                    return Fail(source.path, line, "NUL byte in patch");
                const auto body = Trim(raw);
                if (body.empty() || IsComment(body)) continue;

                if (body[0] == '[') {
                    const auto close = body.find(']');
                    if (close == std::string_view::npos)
                        return Fail(source.path, line, "invalid section header");
                    const auto suffix = Trim(body.substr(close + 1));
                    if (!suffix.empty() && !IsComment(suffix))
                        return Fail(source.path, line, "unexpected text after section header");
                    section = std::string(Trim(body.substr(1, close - 1)));
                    if (!IsIniName(section))
                        return Fail(source.path, line, "invalid section name");
                    continue;
                }
                if (section.empty())
                    return Fail(source.path, line, "command outside a section");
                const auto equal = body.find('=');
                if (equal == std::string_view::npos)
                    return Fail(source.path, line, "expected key=value, +=value, -=property or include");
                const auto operation = Trim(body.substr(0, equal));
                auto value = Trim(body.substr(equal + 1));
                if (EqualName(section, "#include")) {
                    if (operation == "-" ||
                        (operation != "+" && !IsIdentifier(operation)))
                        return Fail(source.path, line, "invalid include assignment");
                    if (!value.empty() && (value[0] == '\"' || value[0] == '\'')) {
                        const auto close = value.find(value[0], 1);
                        if (close == std::string_view::npos)
                            return Fail(source.path, line, "unclosed include path quote");
                        const auto suffix = Trim(value.substr(close + 1));
                        if (!suffix.empty() && !IsComment(suffix))
                            return Fail(source.path, line, "unexpected text after include path");
                        value = value.substr(1, close - 1);
                    } else value = WithoutComment(value);
                    if (value.empty() || value.size() >= kMaxPath ||
                        value.find_first_of("*?\"'<>|=\t") != std::string_view::npos)
                        return Fail(source.path, line, "invalid or overlong include path");
                    for (const unsigned char c : value) {
                        if (c < 0x20 || c == 0x7F)
                            return Fail(source.path, line, "control character in include path");
                    }
                    if (++includes > kMaxFiles)
                        return Fail(source.path, line, "include entry limit exceeded");
                    if (!Load(std::string(value), source.path, line)) return false;
                } else {
                    Command command;
                    command.section = section;
                    command.file = source.path;
                    command.line = line;
                    if (operation == "-") {
                        if (!plan.allowRemoval)
                            return Fail(source.path, line, "-= is only supported in inject/rules");
                        value = WithoutComment(value);
                        if (!IsIdentifier(section) || !IsIdentifier(value))
                            return Fail(source.path, line, "expected one exact, nonempty property and section name");
                        if (IsRegistry(section))
                            return Fail(source.path, line, "registry sections cannot be removed: " + section);
                        command.operation = Operation::Remove;
                        command.key = std::string(value);
                    } else if (operation == "+") {
                        value = WithoutComment(value);
                        if (value.empty())
                            return Fail(source.path, line, "empty += append value");
                        command.operation = Operation::Append;
                        command.value = std::string(value);
                    } else {
                        if (!IsIniName(operation))
                            return Fail(source.path, line, "invalid property name");
                        command.operation = Operation::Set;
                        command.key = std::string(operation);
                        // Match the previous ordinary overlay assignment:
                        // trim edges, but preserve empty values, quotes, '='
                        // and inline text rather than reinterpreting values.
                        command.value = std::string(value);
                    }
                    if (plan.commands.size() >= kMaxCommands)
                        return Fail(source.path, line, "patch command limit exceeded");
                    plan.commands.push_back(std::move(command));
                }
            }
            return true;
        }
    };
} // namespace

bool EqualName(const std::string& left, const std::string& right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (Fold(left[i]) != Fold(right[i])) return false;
    }
    return true;
}

std::string NormalizePath(const std::string& path)
{
    std::string normalized = path;
    for (char& c : normalized) if (c == '\\') c = '/';
    std::string prefix;
    std::size_t start = 0;
    if (normalized.size() >= 2 && normalized[1] == ':') {
        prefix = normalized.substr(0, 2);
        start = 2;
    }
    const bool rooted = start < normalized.size() && normalized[start] == '/';
    if (rooted) {
        // Preserve UNC prefix so filesystem identity is not confused with a
        // rooted path on the current drive.
        prefix += normalized.substr(start, 2) == "//" ? "//" : "/";
    }
    std::vector<std::string> components;
    while (start < normalized.size()) {
        const auto end = normalized.find('/', start);
        const auto component = normalized.substr(start, end - start);
        if (!component.empty() && component != ".") {
            if (component == ".." && !components.empty() && components.back() != "..")
                components.pop_back();
            else if (component != ".." || !rooted) components.push_back(component);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    for (const auto& component : components) {
        if (!prefix.empty() && prefix.back() != '/' && prefix.back() != ':') prefix += '/';
        prefix += component;
    }
    return prefix;
}

bool IsRemovalTarget(const std::string& section, const std::string& key)
{
    return IsIdentifier(section) && IsIdentifier(key) &&
           !EqualName(section, "#include") && !IsRegistry(section);
}

bool BuildPlan(const std::vector<std::string>& roots, Reader reader,
               void* context, Plan& output, Error& error, bool allowRemoval)
{
    output = {};
    error = {};
    if (!reader || roots.size() > 256) {
        error.message = "invalid reader or too many root files";
        return false;
    }
    Builder builder(reader, context, error, allowRemoval);
    for (const auto& root : roots) {
        if (!builder.Load(root, {}, 0)) return false;
    }
    output = std::move(builder.plan);
    return true;
}
} // namespace IniPatch
