using System.Globalization;
using System.IO;
using System.Text;
using RA2Hook.RuntimeUI.Models;

namespace RA2Hook.RuntimeUI.Services;

public sealed class DumpUnitExtractor
{
    private static readonly UnitRegistry[] Registries =
    {
        new("BuildingTypes", "建筑", "building"),
        new("InfantryTypes", "步兵", "infantry"),
        new("VehicleTypes", "坦克", "vehicle"),
        new("AircraftTypes", "飞机", "aircraft")
    };

    private static readonly string[] DependencyRegistries =
    {
        "BuildingTypes", "InfantryTypes", "VehicleTypes", "AircraftTypes",
        "WeaponTypes", "Projectiles", "Projectile", "Warheads",
        "ParticleSystems", "Particles", "Animations", "VoxelAnims",
        "TerrainTypes", "SmudgeTypes", "OverlayTypes", "SuperWeaponTypes"
    };

    private static readonly HashSet<string> RuleReferenceKeys = new(StringComparer.OrdinalIgnoreCase)
    {
        "Primary", "Secondary", "ElitePrimary", "EliteSecondary",
        "DeathWeapon", "EliteDeathWeapon", "OccupyWeapon", "EliteOccupyWeapon",
        "AirburstWeapon", "ShrapnelWeapon", "Projectile", "Warhead",
        "DeploysInto", "UndeploysInto", "PowersUnit", "FreeUnit",
        "SpawnedAircraft", "SpawnedType", "Spawned", "Spawns", "HoldsWhat",
        "NextParticle", "AttachedParticleSystem", "ParticleSystem", "Particle",
        "Particles", "DamageParticleSystems", "DestroyParticleSystems",
        "RefinerySmokeParticleSystem", "NaturalParticleSystem", "SmokeParticleSystem",
        "DebrisTypes", "DebrisAnims", "Trailer", "TrailerAnim", "Explosion",
        "Anim", "AnimList"
    };

    private static readonly HashSet<string> ArtReferenceKeys = new(StringComparer.OrdinalIgnoreCase)
    {
        "Buildup", "ActiveAnim", "ActiveAnimDamaged", "ActiveAnimTwo",
        "ActiveAnimTwoDamaged", "ActiveAnimThree", "ActiveAnimThreeDamaged",
        "ActiveAnimFour", "ActiveAnimFourDamaged", "SpecialAnim",
        "SpecialAnimDamaged", "SpecialAnimTwo", "SpecialAnimTwoDamaged",
        "SpecialAnimThree", "SpecialAnimFour", "SuperAnim", "SuperAnimTwo",
        "SuperAnimThree", "SuperAnimFour", "IdleAnim", "IdleAnimDamaged",
        "TrailerAnim", "ExpireAnim", "TurretAnim", "BarrelAnim", "Bib",
        "Anim", "AnimList", "Explosion", "DebrisAnim"
    };

    private string _dumpRoot = string.Empty;
    private IniSnapshot? _rules;
    private IniSnapshot? _art;
    private readonly Dictionary<string, List<Registration>> _registrations =
        new(StringComparer.OrdinalIgnoreCase);

    public string RulesPath => Path.Combine(_dumpRoot, "ini", "rulesmd.ini");
    public string ArtPath => Path.Combine(_dumpRoot, "ini", "artmd.ini");
    public string DumpRoot => _dumpRoot;

    public async Task<IReadOnlyList<DumpUnitInfo>> LoadAsync(string gameRoot)
    {
        _dumpRoot = Path.Combine(Path.GetFullPath(gameRoot), "ra2hook", "dump");
        if (!File.Exists(RulesPath))
            throw new FileNotFoundException("没有找到 Dump 的 rulesmd.ini", RulesPath);
        if (!File.Exists(ArtPath))
            throw new FileNotFoundException("没有找到 Dump 的 artmd.ini", ArtPath);

        var rulesTask = IniSnapshot.LoadAsync(RulesPath);
        var artTask = IniSnapshot.LoadAsync(ArtPath);
        await Task.WhenAll(rulesTask, artTask);
        _rules = await rulesTask;
        _art = await artTask;
        IndexRegistrations();

        var units = new List<DumpUnitInfo>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var registry in Registries)
        {
            var section = _rules.Find(registry.Section);
            if (section is null) continue;
            foreach (var entry in section.Entries)
            {
                var id = entry.Value.Trim();
                if (id.Length == 0 || !seen.Add(id)) continue;
                var rulesSection = _rules.Find(id);
                var artName = rulesSection?.Value("Image") ?? id;
                var artSection = _art.Find(artName);
                units.Add(new DumpUnitInfo(
                    id, registry.Label, registry.Key, registry.Section, entry.Key,
                    artSection?.Name ?? artName, rulesSection is not null,
                    artSection is not null, CountMaterialFiles(id)));
            }
        }

        return units.OrderBy(unit => unit.Id, StringComparer.OrdinalIgnoreCase).ToArray();
    }

    public DumpExtractionPreview Preview(DumpUnitInfo unit)
    {
        EnsureLoaded();
        var rulesSections = CollectRulesSections(unit.Id);
        var artSections = CollectArtSections(unit.Id, rulesSections);
        return new DumpExtractionPreview(
            OutputDirectory(unit),
            rulesSections.Select(section => section.Name).ToArray(),
            artSections.Select(section => section.Name).ToArray(),
            CountMaterialFiles(unit.Id));
    }

    public async Task<DumpExtractionResult> ExtractAsync(DumpUnitInfo unit, bool overwrite)
    {
        EnsureLoaded();
        var output = OutputDirectory(unit);
        if (!Directory.Exists(output))
            throw new DirectoryNotFoundException($"没有找到该单位的模型目录: {output}");

        var rulesPath = Path.Combine(output, "rules.ini");
        var artPath = Path.Combine(output, "art.ini");
        if (!overwrite && (File.Exists(rulesPath) || File.Exists(artPath)))
            throw new IOException($"模型目录中已有 rules.ini 或 art.ini: {output}");

        var rulesSections = CollectRulesSections(unit.Id);
        var artSections = CollectArtSections(unit.Id, rulesSections);
        var rulesText = RenderRules(unit, rulesSections);
        var artText = RenderIni("RA2Hook unit art extract", unit.Id, artSections);
        await WriteAtomicAsync(rulesPath, rulesText, _rules!.Encoding);
        await WriteAtomicAsync(artPath, artText, _art!.Encoding);

        return new DumpExtractionResult(output, rulesSections.Count, artSections.Count, CountMaterialFiles(unit.Id));
    }

    private List<IniSection> CollectRulesSections(string unitId)
    {
        var result = new List<IniSection>();
        var queued = new Queue<string>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        queued.Enqueue(unitId);

        while (queued.Count > 0)
        {
            var name = queued.Dequeue();
            if (!seen.Add(name)) continue;
            var section = _rules!.Find(name);
            if (section is null) continue;
            result.Add(section);

            foreach (var entry in section.Entries)
            {
                foreach (var reference in SplitReferences(entry.Value))
                    if ((IsRuleReferenceKey(entry.Key) || _registrations.ContainsKey(reference)) &&
                        _rules.Find(reference) is not null && !seen.Contains(reference))
                        queued.Enqueue(reference);
            }
        }
        return result;
    }

    private List<IniSection> CollectArtSections(string unitId, IReadOnlyList<IniSection> rulesSections)
    {
        var result = new List<IniSection>();
        var queued = new Queue<string>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var unitRules = _rules!.Find(unitId);
        queued.Enqueue(unitRules?.Value("Image") ?? unitId);

        foreach (var section in rulesSections)
        {
            queued.Enqueue(section.Name);
            var image = section.Value("Image");
            if (!string.IsNullOrWhiteSpace(image)) queued.Enqueue(image);
            foreach (var entry in section.Entries)
                if (IsArtReferenceKey(entry.Key))
                    foreach (var reference in SplitReferences(entry.Value))
                        queued.Enqueue(reference);
        }

        while (queued.Count > 0)
        {
            var name = queued.Dequeue();
            if (!seen.Add(name)) continue;
            var section = _art!.Find(name);
            if (section is null) continue;
            result.Add(section);

            var redirected = section.Value("Image");
            if (!string.IsNullOrWhiteSpace(redirected) && _art.Find(redirected) is not null)
                queued.Enqueue(redirected);

            foreach (var entry in section.Entries)
            {
                if (!IsArtReferenceKey(entry.Key)) continue;
                foreach (var reference in SplitReferences(entry.Value))
                    queued.Enqueue(reference);
            }
        }
        return result;
    }

    private static bool IsRuleReferenceKey(string key)
    {
        if (RuleReferenceKeys.Contains(key)) return true;
        return key.Contains("Weapon", StringComparison.OrdinalIgnoreCase) ||
               key.Contains("Warhead", StringComparison.OrdinalIgnoreCase) ||
               key.Contains("Projectile", StringComparison.OrdinalIgnoreCase) ||
               key.Contains("Particle", StringComparison.OrdinalIgnoreCase) ||
               key.Contains("Anim", StringComparison.OrdinalIgnoreCase) ||
               key.Contains("Debris", StringComparison.OrdinalIgnoreCase) ||
               key.Contains("Spawn", StringComparison.OrdinalIgnoreCase) ||
               key.Contains("Trail", StringComparison.OrdinalIgnoreCase) ||
               key.Contains("Effect", StringComparison.OrdinalIgnoreCase) ||
               key.Contains("Smoke", StringComparison.OrdinalIgnoreCase);
    }

    private static bool IsArtReferenceKey(string key)
    {
        return ArtReferenceKeys.Contains(key) ||
               key.Contains("Anim", StringComparison.OrdinalIgnoreCase) ||
               key.Contains("Explosion", StringComparison.OrdinalIgnoreCase) ||
               key.Contains("Debris", StringComparison.OrdinalIgnoreCase) ||
               key.Contains("Trailer", StringComparison.OrdinalIgnoreCase);
    }

    private static IEnumerable<string> SplitReferences(string value)
    {
        return value.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Select(part => part.Trim().Trim('"'))
            .Where(part => part.Length > 0 && !part.Equals("none", StringComparison.OrdinalIgnoreCase) &&
                           !part.Equals("<none>", StringComparison.OrdinalIgnoreCase));
    }

    private string RenderRules(DumpUnitInfo unit, IReadOnlyList<IniSection> sections)
    {
        var builder = new StringBuilder();
        builder.AppendLine($"; RA2Hook unit rules extract: {unit.Id}");
        var included = sections.Select(section => section.Name)
            .ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (var registryName in DependencyRegistries)
        {
            var registry = _rules!.Find(registryName);
            if (registry is null) continue;
            var entries = registry.Entries
                .Select(entry => entry.Value.Trim())
                .Where(id => id.Length > 0 && included.Contains(id))
                .ToArray();
            if (entries.Length == 0) continue;
            builder.Append('[').Append(registryName).AppendLine("]");
            var sequence = 1;
            foreach (var id in entries)
            {
                var left = $"{SafeRegistrationPart(id)}_{RegistrationCategory(registryName)}_{sequence++}";
                builder.Append(left).Append('=').AppendLine(id);
            }
            builder.AppendLine();
        }
        AppendSections(builder, sections);
        return builder.ToString();
    }

    private static string RegistrationCategory(string registryName) => registryName switch
    {
        "BuildingTypes" => "Building",
        "InfantryTypes" => "Infantry",
        "VehicleTypes" => "Vehicle",
        "AircraftTypes" => "Aircraft",
        "WeaponTypes" => "Weapon",
        "Projectiles" or "Projectile" => "Projectile",
        "Warheads" => "Warhead",
        "ParticleSystems" => "ParticleSystem",
        "Particles" => "Particle",
        "Animations" => "Animation",
        "VoxelAnims" => "VoxelAnim",
        "TerrainTypes" => "Terrain",
        "SmudgeTypes" => "Smudge",
        "OverlayTypes" => "Overlay",
        "SuperWeaponTypes" => "SuperWeapon",
        _ => registryName
    };

    private static string SafeRegistrationPart(string value)
    {
        var result = new string(value.Select(character =>
            char.IsLetterOrDigit(character) || character == '_' ? character : '_').ToArray());
        return string.IsNullOrWhiteSpace(result) ? "Unit" : result;
    }

    private void IndexRegistrations()
    {
        _registrations.Clear();
        foreach (var registryName in DependencyRegistries)
        {
            var section = _rules!.Find(registryName);
            if (section is null) continue;
            foreach (var entry in section.Entries)
            {
                var id = entry.Value.Trim();
                if (id.Length == 0) continue;
                if (!_registrations.TryGetValue(id, out var registrations))
                {
                    registrations = new List<Registration>();
                    _registrations[id] = registrations;
                }
                registrations.Add(new Registration(registryName, entry.Key));
            }
        }
    }

    private static string RenderIni(string title, string unitId, IReadOnlyList<IniSection> sections)
    {
        var builder = new StringBuilder();
        builder.AppendLine($"; {title}: {unitId}");
        AppendSections(builder, sections);
        return builder.ToString();
    }

    private static void AppendSections(StringBuilder builder, IReadOnlyList<IniSection> sections)
    {
        foreach (var section in sections)
        {
            builder.Append('[').Append(section.Name).AppendLine("]");
            foreach (var line in section.Lines) builder.AppendLine(line);
            if (builder.Length > 0 && builder[^1] != '\n') builder.AppendLine();
            builder.AppendLine();
        }
    }

    private int CountMaterialFiles(string unitId)
    {
        var count = 0;
        foreach (var kind in new[] { "vxl", "shp" })
        {
            var directory = FindUnitMaterialDirectory(kind, unitId);
            if (directory is not null)
                count += Directory.EnumerateFiles(directory, "*", SearchOption.AllDirectories).Count();
        }
        return count;
    }

    private string? FindUnitMaterialDirectory(string kind, string unitId)
    {
        var root = Path.Combine(_dumpRoot, kind);
        if (!Directory.Exists(root)) return null;
        return Directory.EnumerateDirectories(root)
            .FirstOrDefault(path => Path.GetFileName(path).Equals(unitId, StringComparison.OrdinalIgnoreCase));
    }

    private string OutputDirectory(DumpUnitInfo unit)
    {
        var preferredKind = IsVoxel(unit) ? "vxl" : "shp";
        var preferred = FindUnitMaterialDirectory(preferredKind, unit.Id);
        if (preferred is not null) return preferred;
        var fallbackKind = preferredKind.Equals("vxl", StringComparison.OrdinalIgnoreCase) ? "shp" : "vxl";
        return FindUnitMaterialDirectory(fallbackKind, unit.Id) ??
               Path.Combine(_dumpRoot, preferredKind, SafeFileName(unit.Id));
    }

    private bool IsVoxel(DumpUnitInfo unit)
    {
        var art = _art!.Find(unit.ArtSection);
        var redirected = art?.Value("Image");
        if (!string.IsNullOrWhiteSpace(redirected) && _art.Find(redirected) is { } redirectedArt)
            art = redirectedArt;
        var value = art?.Value("Voxel");
        return value is not null && (value.Equals("yes", StringComparison.OrdinalIgnoreCase) ||
                                     value.Equals("true", StringComparison.OrdinalIgnoreCase) ||
                                     value == "1");
    }

    private static string SafeFileName(string value)
    {
        var invalid = Path.GetInvalidFileNameChars();
        var result = new string(value.Select(character => invalid.Contains(character) ? '_' : character).ToArray());
        return string.IsNullOrWhiteSpace(result) || result is "." or ".." ? "unit" : result;
    }

    private static async Task WriteAtomicAsync(string path, string text, Encoding encoding)
    {
        var temporary = path + ".tmp." + Guid.NewGuid().ToString("N");
        try
        {
            await File.WriteAllTextAsync(temporary, text, encoding);
            File.Move(temporary, path, true);
        }
        finally
        {
            if (File.Exists(temporary)) File.Delete(temporary);
        }
    }

    private void EnsureLoaded()
    {
        if (_rules is null || _art is null)
            throw new InvalidOperationException("请先扫描 Dump 数据");
    }

    private sealed record UnitRegistry(string Section, string Label, string Key);
    private sealed record Registration(string Section, string Key);

    private sealed class IniSnapshot
    {
        private readonly Dictionary<string, IniSection> _sections;
        public Encoding Encoding { get; }

        private IniSnapshot(Dictionary<string, IniSection> sections, Encoding encoding)
        {
            _sections = sections;
            Encoding = encoding;
        }

        public IniSection? Find(string name) => _sections.GetValueOrDefault(name.Trim());

        public static async Task<IniSnapshot> LoadAsync(string path)
        {
            var bytes = await File.ReadAllBytesAsync(path);
            var (text, encoding) = Decode(bytes);
            return Parse(text, encoding);
        }

        private static IniSnapshot Parse(string text, Encoding encoding)
        {
            var sections = new Dictionary<string, IniSection>(StringComparer.OrdinalIgnoreCase);
            IniSection? current = null;
            using var reader = new StringReader(text);
            while (reader.ReadLine() is { } line)
            {
                var trimmed = line.Trim();
                if (trimmed.StartsWith('[') && trimmed.EndsWith(']'))
                {
                    var name = trimmed[1..^1].Trim();
                    current = new IniSection(name);
                    sections[name] = current;
                    continue;
                }
                current?.Add(line);
            }
            return new IniSnapshot(sections, encoding);
        }

        private static (string Text, Encoding Encoding) Decode(byte[] bytes)
        {
            if (bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
                return (Encoding.UTF8.GetString(bytes, 3, bytes.Length - 3), new UTF8Encoding(true));
            if (bytes.Length >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE)
                return (Encoding.Unicode.GetString(bytes, 2, bytes.Length - 2), Encoding.Unicode);

            try
            {
                var utf8 = new UTF8Encoding(false, true);
                return (utf8.GetString(bytes), new UTF8Encoding(true));
            }
            catch (DecoderFallbackException)
            {
                Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
                var codePage = CultureInfo.CurrentCulture.TextInfo.ANSICodePage;
                var ansi = Encoding.GetEncoding(codePage);
                return (ansi.GetString(bytes), ansi);
            }
        }
    }

    private sealed class IniSection
    {
        private readonly List<KeyValuePair<string, string>> _entries = new();
        public string Name { get; }
        public List<string> Lines { get; } = new();
        public IReadOnlyList<KeyValuePair<string, string>> Entries => _entries;

        public IniSection(string name) => Name = name;

        public void Add(string line)
        {
            Lines.Add(line);
            var trimmed = line.Trim();
            if (trimmed.Length == 0 || trimmed[0] is ';' or '#') return;
            var equal = trimmed.IndexOf('=');
            if (equal <= 0) return;
            _entries.Add(new KeyValuePair<string, string>(
                trimmed[..equal].Trim(), trimmed[(equal + 1)..].Trim()));
        }

        public string? Value(string key)
        {
            for (var index = _entries.Count - 1; index >= 0; index--)
                if (_entries[index].Key.Equals(key, StringComparison.OrdinalIgnoreCase))
                    return _entries[index].Value;
            return null;
        }
    }
}
