namespace RA2Hook.RuntimeUI.Models;

public sealed record DumpUnitInfo(
    string Id,
    string Category,
    string CategoryKey,
    string RegistrySection,
    string RegistrationKey,
    string ArtSection,
    bool HasRules,
    bool HasArt,
    int MaterialFiles)
{
    public string DisplayName => $"{Id}  ·  {Category}";
    public string Availability =>
        $"rules {(HasRules ? "有" : "无")}  /  art {(HasArt ? "有" : "无")}  /  素材 {MaterialFiles}";
}

public sealed record DumpExtractionPreview(
    string OutputDirectory,
    IReadOnlyList<string> RulesSections,
    IReadOnlyList<string> ArtSections,
    int MaterialFiles);

public sealed record DumpExtractionResult(
    string OutputDirectory,
    int RulesSections,
    int ArtSections,
    int MaterialFiles);
