namespace RA2Hook.RuntimeUI.Models;

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
