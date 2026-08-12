using System.IO;
using System.Text;
using RA2Hook.RuntimeUI.Models;

namespace RA2Hook.RuntimeUI.Services;

public static class IniDocument
{
    private static readonly HashSet<string> RegistrySections = new(StringComparer.OrdinalIgnoreCase)
    {
        "Maximums", "InfantryTypes", "VehicleTypes", "AircraftTypes", "BuildingTypes",
        "TerrainTypes", "SmudgeTypes", "OverlayTypes", "Animations", "VoxelAnims",
        "Warheads", "Particles", "ParticleSystems", "SuperWeaponTypes", "Countries",
        "Sides", "AITriggerTypes", "TeamTypes", "TaskForces", "ScriptTypes",
        "TriggerTypes", "Tags", "Colors", "ColorAdd"
    };

    private static readonly HashSet<string> RuntimeRuleSections = new(StringComparer.OrdinalIgnoreCase)
    {
        "General", "CombatDamage", "AudioVisual", "CrateRules", "Radiation",
        "ElevationModel", "WallModel", "SpecialWeapons", "Powerups",
        "LandCharacteristics", "IQ", "JumpjetControls", "AdvancedCommandBar",
        "MultiplayerDialogSettings", "Easy", "Normal", "Difficult", "Difficulty", "AI"
    };

    private static readonly string[] UnsafeKeys =
    {
        "Image", "AlphaImage", "Cameo", "AltCameo", "Voxel", "Foundation",
        "Locomotor", "MovementZone", "SpeedType", "Theater", "NewTheater",
        "AlternateArcticArt", "Palette", "CustomPalette", "SHP", "VXL", "HVA",
        "TurretAnim", "BarrelAnim", "PBarrelLength", "PBarrelThickness"
    };

    public static IReadOnlyList<IniEntryRow> Parse(string text)
    {
        var rows = new List<IniEntryRow>();
        var section = string.Empty;
        using var reader = new StringReader(text);
        while (reader.ReadLine() is { } line)
        {
            var trimmed = line.Trim();
            if (trimmed.Length == 0 || trimmed[0] is ';' or '#') continue;
            if (trimmed[0] == '[' && trimmed.EndsWith(']'))
            {
                section = trimmed[1..^1].Trim();
                continue;
            }
            var equal = trimmed.IndexOf('=');
            if (equal <= 0 || section.Length == 0) continue;
            var key = trimmed[..equal].Trim();
            var value = trimmed[(equal + 1)..].Trim();
            rows.Add(new IniEntryRow(section, key, value, Classify(section, key)));
        }
        return rows;
    }

    public static async Task WriteAtomicAsync(string path, string text)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temporary = path + ".tmp." + Guid.NewGuid().ToString("N");
        try
        {
            await File.WriteAllTextAsync(temporary, text, new UTF8Encoding(true));
            File.Move(temporary, path, true);
        }
        finally
        {
            if (File.Exists(temporary)) File.Delete(temporary);
        }
    }

    public static string ResolveRuntimeDirectory(string gameRoot)
    {
        var primaryConfig = Path.Combine(gameRoot, "ra2hook", "ra2hook.ini");
        var legacyConfig = Path.Combine(gameRoot, "ra2hook.ini");
        var configPath = File.Exists(primaryConfig) ? primaryConfig : legacyConfig;
        var configured = ReadRuntimeDirectory(configPath);
        return Path.GetFullPath(Path.Combine(gameRoot,
            string.IsNullOrWhiteSpace(configured) ? Path.Combine("ra2hook", "runtime") : configured));
    }

    private static string? ReadRuntimeDirectory(string configPath)
    {
        if (!File.Exists(configPath)) return null;
        var section = string.Empty;
        foreach (var line in File.ReadLines(configPath))
        {
            var trimmed = line.Trim();
            if (trimmed.StartsWith('[') && trimmed.EndsWith(']'))
            {
                section = trimmed[1..^1];
                continue;
            }
            if (!section.Equals("Runtime", StringComparison.OrdinalIgnoreCase)) continue;
            var equal = trimmed.IndexOf('=');
            if (equal > 0 && trimmed[..equal].Trim().Equals("Directory",
                    StringComparison.OrdinalIgnoreCase))
                return trimmed[(equal + 1)..].Trim();
        }
        return null;
    }

    private static string Classify(string section, string key)
    {
        if (RegistrySections.Contains(section)) return "RestartRequired";
        if (UnsafeKeys.Any(value => key.Equals(value, StringComparison.OrdinalIgnoreCase) ||
                                    key.StartsWith(value + ".", StringComparison.OrdinalIgnoreCase)))
            return "RestartRequired";
        if (RuntimeRuleSections.Contains(section)) return "ControlledReload";
        return "Pending DLL classification";
    }
}
