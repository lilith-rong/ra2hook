using System.IO;

namespace RA2Hook.RuntimeUI.Models;

public sealed record RuntimePatchFile(string FullName, string Name, bool IsEnabled)
{
    public string DisplayName => IsEnabled ? Name : $"{Name}（停用）";

    public static bool IsPatchPath(string path) =>
        path.EndsWith(".ini", StringComparison.OrdinalIgnoreCase) ||
        path.EndsWith(".ini.disabled", StringComparison.OrdinalIgnoreCase);

    public static RuntimePatchFile FromPath(string path)
    {
        var fileName = Path.GetFileName(path);
        var enabled = fileName.EndsWith(".ini", StringComparison.OrdinalIgnoreCase);
        var suffixLength = enabled ? ".ini".Length : ".ini.disabled".Length;
        return new RuntimePatchFile(path, fileName[..^suffixLength], enabled);
    }

    public static string BuildPath(string directory, string name, bool enabled) =>
        Path.Combine(directory, name + (enabled ? ".ini" : ".ini.disabled"));
}

public sealed record RuntimeStatus(
    bool Initialized,
    bool Enabled,
    bool AutoApply,
    bool CurrentlyInGame,
    bool SinglePlayer,
    bool Replay,
    bool Applied,
    int Generation,
    int AppliedKeys,
    int RejectedKeys,
    int PendingCommands,
    string Mode,
    string Directory,
    string Message);

public sealed record RuntimeItem(
    string Section,
    string Key,
    string OldValue,
    string NewValue,
    string Safety,
    string Result);

public sealed record IniEntryRow(string Section, string Key, string Value, string Safety);
