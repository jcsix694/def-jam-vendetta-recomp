using System.IO.Compression;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace VendettaSettings.Services;

/// <summary>A project about to be exported: its ticked mods found in the library, the ones that aren't.</summary>
public sealed class ExportPlan
{
    public required Project Project { get; init; }
    public required IReadOnlyList<ModInfo> Mods { get; init; }
    /// <summary>Ticked in the project but not in the library: left out.</summary>
    public required IReadOnlyList<string> Missing { get; init; }
    /// <summary>The mods' files, before packing.</summary>
    public required long ModBytes { get; init; }
}

/// <summary>What happens to one of a shared project's mods on import.</summary>
public enum ImportFate { New, Same, Differs }

public sealed class ImportMod
{
    /// <summary>Theirs, unpacked (its Folder is in the temporary folder).</summary>
    public required ModPackage Package { get; init; }
    public required ImportFate Fate { get; init; }
    public ModInfo Theirs => Package.Mod;
    public ModInfo? Mine => Package.Replaces;
    /// <summary>For <see cref="ImportFate.Differs"/>: install theirs over mine (false = keep mine).</summary>
    public bool UseTheirs { get; set; } = true;
}

/// <summary>A shared project read from its zip and unpacked into a temporary folder, not imported yet.</summary>
public sealed class ImportPlan
{
    public required string Source { get; init; }
    public required string TempFolder { get; init; }
    /// <summary>The name it was shared under (made safe for a folder).</summary>
    public required string Name { get; init; }
    /// <summary>Its ticked mods in load order: each one it carries.</summary>
    public required IReadOnlyList<ImportMod> Mods { get; init; }
    /// <summary>Ticked but not in the zip: those already in the library are used, the others are listed here.</summary>
    public required IReadOnlyList<string> Missing { get; init; }
    public required bool HasSaves { get; init; }
}

/// <summary>Sharing a project: one zip holding <c>project.json</c> (its load order naming the mods' folders), the
/// picture, a copy of each ticked mod (<c>mods\&lt;folder&gt;\</c>) and, if asked, its memory cards (<c>saves\mc0</c>,
/// <c>saves\mc1</c>). Importing puts the mods in the library (one the library has already, the same files, is used as it
/// is; another version is replaced or kept, the player's choice) and makes the project.</summary>
public static class ProjectShare
{
    public const string ZipSuffix = " (Def Jam Vendetta project)";
    // names written as they are (an apostrophe stays one, not \u0027): people read project.json by hand
    private static readonly JsonSerializerOptions Indented = new() { WriteIndented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping };

    public static string DefaultFileName(string project) => project + ZipSuffix + ".zip";

    // --- export ---

    public static ExportPlan PlanExport(GameFolder folder, Project project)
    {
        var library = ModLibrary.List(folder);
        var mods = new List<ModInfo>();
        var missing = new List<string>();
        foreach (var name in project.Mods)
        {
            if (library.FirstOrDefault(m => m.Is(name)) is { } mod)
            {
                if (!mods.Contains(mod))
                    mods.Add(mod);
            }
            else
                missing.Add(name);
        }
        return new ExportPlan { Project = project, Mods = mods, Missing = missing, ModBytes = mods.Sum(m => SizeOf(m.Folder)) };
    }

    public static long SizeOf(string dir) =>
        Directory.Exists(dir) ? Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories).Sum(f => new FileInfo(f).Length) : 0;

    public static string Size(long bytes) => bytes switch
    {
        < 1024 * 1024 => $"{Math.Max(1, (bytes + 1023) / 1024)} KB",
        < 1024L * 1024 * 1024 => $"{bytes / (1024.0 * 1024):0.#} MB",
        _ => $"{bytes / (1024.0 * 1024 * 1024):0.##} GB",
    };

    /// <summary>Writes the zip (a .part file first, so a half-written one never takes the name). Returns why not, or null.</summary>
    public static string? Export(ExportPlan plan, bool withSaves, string zipPath)
    {
        var temp = zipPath + ".part";
        try
        {
            if (File.Exists(temp))
                File.Delete(temp);
            using (var zip = ZipFile.Open(temp, ZipArchiveMode.Create))
            {
                var dir = plan.Project.Folder;
                var json = JsonNode.Parse(File.ReadAllText(Path.Combine(dir, "project.json")))?.AsObject() ?? new JsonObject();
                json["name"] = plan.Project.Name;
                var list = new JsonArray();
                foreach (var m in plan.Mods)
                    list.Add(m.FolderName);
                json["mods"] = list;
                if (plan.Project.Picture is { } picture)
                {
                    json["picture"] = Path.GetFileName(picture);
                    zip.CreateEntryFromFile(picture, Path.GetFileName(picture), CompressionLevel.Optimal);
                }
                else
                    json.Remove("picture");
                using (var writer = new StreamWriter(zip.CreateEntry("project.json", CompressionLevel.Optimal).Open()))
                    writer.Write(json.ToJsonString(Indented) + "\n");
                foreach (var m in plan.Mods)
                    AddDir(zip, m.Folder, "mods/" + m.FolderName + "/");
                if (withSaves)
                {
                    AddDir(zip, Path.Combine(dir, "saves", "mc0"), "saves/mc0/");
                    AddDir(zip, Path.Combine(dir, "saves", "mc1"), "saves/mc1/");
                }
            }
            File.Move(temp, zipPath, overwrite: true);
            return null;
        }
        catch (Exception ex)
        {
            try { if (File.Exists(temp)) File.Delete(temp); } catch { }
            return $"Couldn't export it: {ex.Message}";
        }
    }

    private static void AddDir(ZipArchive zip, string dir, string prefix)
    {
        if (!Directory.Exists(dir))
            return;
        foreach (var file in Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories))
            zip.CreateEntryFromFile(file, prefix + Path.GetRelativePath(dir, file).Replace('\\', '/'), CompressionLevel.Optimal);
    }

    // --- import ---

    /// <summary>Reads a shared project's zip: unpacked into a temporary folder (<see cref="Discard"/> it when done), each
    /// mod compared with the library. Returns it, or why it can't be imported.</summary>
    public static (ImportPlan? Plan, string? Error) OpenImport(GameFolder folder, string source)
    {
        if (!source.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) || !File.Exists(source))
            return (null, "A shared project is a .zip file.");
        var temp = Path.Combine(Path.GetTempPath(), "Vendetta import " + Guid.NewGuid().ToString("N")[..8]);
        try
        {
            string top;
            using (var zip = ZipFile.OpenRead(source))
            {
                var found = FindProject(zip);
                if (found == null)
                    return (null, ModLibrary.FindManifest(zip).Entry != null
                        ? "That's a mod, not a project: install it on the Mods page."
                        : "That zip has no project.json, so it isn't a shared project.");
                top = found;
            }
            ModLibrary.Unzip(source, top, temp);
            var json = JsonNode.Parse(File.ReadAllText(Path.Combine(temp, "project.json")))?.AsObject();
            if (json == null)
                return Fail(temp, "Its project.json can't be read.");
            var listed = new List<string>();
            if (json["mods"] is JsonArray list)
                foreach (var item in list)
                    if (item is JsonValue value && value.TryGetValue<string>(out var text) && text.Length > 0 && !listed.Contains(text, StringComparer.OrdinalIgnoreCase))
                        listed.Add(text);

            // its mods, in the project's order; one it ticks but doesn't carry is fine when the library has it
            var carried = new List<ModPackage>();
            var modsDir = Path.Combine(temp, "mods");
            if (Directory.Exists(modsDir))
                foreach (var dir in Directory.GetDirectories(modsDir))
                    if (ModLibrary.Open(folder, dir).Package is { } package)
                        carried.Add(package);
            var library = ModLibrary.List(folder);
            var mods = new List<ImportMod>();
            var missing = new List<string>();
            foreach (var name in listed)
            {
                if (carried.FirstOrDefault(p => p.Mod.Is(name)) is { } package)
                {
                    if (mods.Any(m => m.Package == package))
                        continue;
                    var fate = package.Replaces == null ? ImportFate.New
                        : SameFiles(package.Mod.Folder, package.Replaces.Folder) ? ImportFate.Same : ImportFate.Differs;
                    mods.Add(new ImportMod { Package = package, Fate = fate });
                }
                else if (!library.Any(m => m.Is(name)))
                    missing.Add(name);
            }

            var shared = json["name"] is JsonValue n && n.TryGetValue<string>(out var given) ? given : "";
            if (shared.Trim().Length == 0)
            {
                shared = Path.GetFileNameWithoutExtension(source);
                if (shared.EndsWith(ZipSuffix, StringComparison.OrdinalIgnoreCase))
                    shared = shared[..^ZipSuffix.Length];
            }
            var saves = Path.Combine(temp, "saves");
            return (new ImportPlan
            {
                Source = source,
                TempFolder = temp,
                Name = SafeName(shared),
                Mods = mods,
                Missing = missing,
                HasSaves = Directory.Exists(saves) && Directory.EnumerateFiles(saves, "*", SearchOption.AllDirectories).Any(),
            }, null);
        }
        catch (Exception ex)
        {
            return Fail(temp, $"It can't be read: {ex.Message}");
        }
    }

    private static (ImportPlan?, string?) Fail(string temp, string error)
    {
        TryDelete(temp);
        return (null, error);
    }

    /// <summary>project.json at the zip's top, else in one folder; that folder ("" at the top), or null.</summary>
    private static string? FindProject(ZipArchive zip)
    {
        string? best = null;
        foreach (var entry in zip.Entries)
        {
            var parts = entry.FullName.Replace('\\', '/').Split('/', StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length is 0 or > 2 || !parts[^1].Equals("project.json", StringComparison.OrdinalIgnoreCase))
                continue;
            if (parts.Length == 1)
                return "";
            best ??= parts[0];
        }
        return best;
    }

    /// <summary>The shared name made fit for a folder (the letters Windows refuses taken out).</summary>
    private static string SafeName(string name)
    {
        var safe = new string(name.Where(c => !Path.GetInvalidFileNameChars().Contains(c)).ToArray()).Trim().TrimEnd('.').Trim();
        if (safe.Length > Projects.MaxNameLength)
            safe = safe[..Projects.MaxNameLength].Trim();
        return safe.Length > 0 ? safe : "Shared project";
    }

    /// <summary>The two folders hold the same files with the same bytes.</summary>
    public static bool SameFiles(string a, string b)
    {
        static List<string> Files(string dir) => Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories)
            .Select(f => Path.GetRelativePath(dir, f).ToLowerInvariant()).Order(StringComparer.Ordinal).ToList();
        var left = Files(a);
        if (!left.SequenceEqual(Files(b)))
            return false;
        foreach (var file in left)
        {
            var x = Path.Combine(a, file);
            var y = Path.Combine(b, file);
            if (new FileInfo(x).Length != new FileInfo(y).Length)
                return false;
            using var fx = File.OpenRead(x);
            using var fy = File.OpenRead(y);
            var bx = new byte[81920];
            var by = new byte[81920];
            int read;
            while ((read = fx.Read(bx, 0, bx.Length)) > 0)
            {
                fy.ReadExactly(by, 0, read);
                if (!bx.AsSpan(0, read).SequenceEqual(by.AsSpan(0, read)))
                    return false;
            }
        }
        return true;
    }

    /// <summary>Imports it as <paramref name="name"/>: the mods into the library as chosen, then the project (its load
    /// order naming each mod's folder in the library, its picture, its saves if <paramref name="useSaves"/>, else an
    /// empty memory card). Returns why not, or null.</summary>
    public static string? Import(GameFolder folder, ImportPlan plan, string name, bool useSaves)
    {
        if (Projects.CheckName(folder, name) is { } bad)
            return bad;
        try
        {
            var json = JsonNode.Parse(File.ReadAllText(Path.Combine(plan.TempFolder, "project.json")))?.AsObject() ?? new JsonObject();
            // where each mod is in the library afterwards, by the name the project ticks it with
            var placed = new List<(ModInfo Theirs, string Folder)>();
            foreach (var m in plan.Mods)
            {
                if (m.Fate == ImportFate.New || (m.Fate == ImportFate.Differs && m.UseTheirs))
                {
                    var (installed, error) = ModLibrary.Install(folder, m.Package);
                    if (error != null)
                        return $"{m.Theirs.Name}: {error}";
                    placed.Add((m.Theirs, installed!));
                }
                else
                    placed.Add((m.Theirs, m.Mine!.FolderName));
            }
            var list = new JsonArray();
            if (json["mods"] is JsonArray ticked)
                foreach (var item in ticked)
                    if (item is JsonValue value && value.TryGetValue<string>(out var text) && text.Length > 0)
                    {
                        var at = placed.FirstOrDefault(p => p.Theirs.Is(text)).Folder ?? text;
                        if (!list.Any(x => x?.GetValue<string>().Equals(at, StringComparison.OrdinalIgnoreCase) == true))
                            list.Add(at);
                    }
            json["format"] ??= 1;
            json["name"] = name;
            json["mods"] = list;

            var dir = Projects.FolderOf(folder, name);
            Directory.CreateDirectory(Path.Combine(dir, "saves", "mc0"));
            Directory.CreateDirectory(Path.Combine(dir, "saves", "mc1"));
            if (json["picture"] is JsonValue p && p.TryGetValue<string>(out var picture) && Path.GetFileName(picture) == picture &&
                Projects.IsPicture(picture) && File.Exists(Path.Combine(plan.TempFolder, picture)))
                File.Copy(Path.Combine(plan.TempFolder, picture), Path.Combine(dir, picture), overwrite: true);
            else
                json.Remove("picture");
            File.WriteAllText(Path.Combine(dir, "project.json"), json.ToJsonString(Indented) + "\n");
            if (useSaves)
                CopyDir(Path.Combine(plan.TempFolder, "saves"), Path.Combine(dir, "saves"));
            return null;
        }
        catch (Exception ex)
        {
            return $"Couldn't import it: {ex.Message}";
        }
    }

    /// <summary>The temporary folder removed (after an import, or when it is cancelled).</summary>
    public static void Discard(ImportPlan? plan)
    {
        if (plan != null)
            TryDelete(plan.TempFolder);
    }

    private static void TryDelete(string dir)
    {
        try { if (Directory.Exists(dir)) Directory.Delete(dir, recursive: true); } catch { }
    }

    private static void CopyDir(string from, string to)
    {
        if (!Directory.Exists(from))
            return;
        Directory.CreateDirectory(to);
        foreach (var file in Directory.GetFiles(from))
            File.Copy(file, Path.Combine(to, Path.GetFileName(file)), overwrite: true);
        foreach (var dir in Directory.GetDirectories(from))
            CopyDir(dir, Path.Combine(to, Path.GetFileName(dir)));
    }
}
