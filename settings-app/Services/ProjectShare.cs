using System.IO.Compression;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace VendettaSettings.Services;

/// <summary>A project about to be exported: the mods installed in it, the ones its list names that aren't there.</summary>
public sealed class ExportPlan
{
    public required Project Project { get; init; }
    public required IReadOnlyList<ModInfo> Mods { get; init; }
    /// <summary>On in the project but not installed in it: nothing to put in.</summary>
    public required IReadOnlyList<string> Missing { get; init; }
    /// <summary>The mods' files, before packing.</summary>
    public required long ModBytes { get; init; }
}

/// <summary>One of a shared project's mods (its copy in the temporary folder).</summary>
public sealed class ImportMod
{
    public required ModInfo Mod { get; init; }
}

/// <summary>A shared project read from its zip and unpacked into a temporary folder, not imported yet.</summary>
public sealed class ImportPlan
{
    public required string Source { get; init; }
    public required string TempFolder { get; init; }
    /// <summary>The name it was shared under (made safe for a folder).</summary>
    public required string Name { get; init; }
    /// <summary>The mods it carries.</summary>
    public required IReadOnlyList<ImportMod> Mods { get; init; }
    /// <summary>On in it but not in the zip.</summary>
    public required IReadOnlyList<string> Missing { get; init; }
    public required bool HasSaves { get; init; }
}

/// <summary>Sharing a project: one zip holding <c>project.json</c> (the mods that are on, in load order), the picture,
/// its mods (<c>mods\&lt;folder&gt;\</c>, each the project's own copy, with where its wrestlers go) and, if asked, its
/// memory cards (<c>saves\mc0</c>, <c>saves\mc1</c>). Importing makes a new project with those copies: nothing in the
/// other projects changes.</summary>
public static class ProjectShare
{
    public const string ZipSuffix = " (Def Jam Vendetta project)";
    // names written as they are (an apostrophe stays one, not '): people read project.json by hand
    private static readonly JsonSerializerOptions Indented = new() { WriteIndented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping };

    public static string DefaultFileName(string project) => project + ZipSuffix + ".zip";

    // --- export ---

    public static ExportPlan PlanExport(GameFolder folder, Project project)
    {
        var mods = ProjectMods.List(folder, project.Name);
        var missing = project.Mods.Where(n => !mods.Any(m => m.Is(n))).ToList();
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

    /// <summary>Reads a shared project's zip: unpacked into a temporary folder (<see cref="Discard"/> it when done).
    /// Returns it, or why it can't be imported.</summary>
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
                    return (null, ProjectMods.FindManifest(zip).Entry != null
                        ? "That's a mod, not a project: install it on the Mods page."
                        : "That zip has no project.json, so it isn't a shared project.");
                top = found;
            }
            ProjectMods.Unzip(source, top, temp);
            var json = JsonNode.Parse(File.ReadAllText(Path.Combine(temp, "project.json")))?.AsObject();
            if (json == null)
                return Fail(temp, "Its project.json can't be read.");
            var listed = new List<string>();
            if (json["mods"] is JsonArray list)
                foreach (var item in list)
                    if (item is JsonValue value && value.TryGetValue<string>(out var text) && text.Length > 0 && !listed.Contains(text, StringComparer.OrdinalIgnoreCase))
                        listed.Add(text);
            var carried = new List<ImportMod>();
            var modsDir = Path.Combine(temp, "mods");
            if (Directory.Exists(modsDir))
                foreach (var dir in Directory.GetDirectories(modsDir))
                    if (ProjectMods.Read(dir) is { } mod)
                        carried.Add(new ImportMod { Mod = mod });
            var missing = listed.Where(n => !carried.Any(c => c.Mod.Is(n))).ToList();

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
                Mods = carried,
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

    /// <summary>Imports it as a new project called <paramref name="name"/>: its mods (their copies), its list, its picture,
    /// its saves if <paramref name="useSaves"/> (else an empty memory card). Returns why not, or null.</summary>
    public static string? Import(GameFolder folder, ImportPlan plan, string name, bool useSaves)
    {
        if (Projects.CheckName(folder, name) is { } bad)
            return bad;
        var dir = Projects.FolderOf(folder, name);
        try
        {
            var json = JsonNode.Parse(File.ReadAllText(Path.Combine(plan.TempFolder, "project.json")))?.AsObject() ?? new JsonObject();
            json["format"] ??= 1;
            json["name"] = name;
            Directory.CreateDirectory(Path.Combine(dir, "saves", "mc0"));
            Directory.CreateDirectory(Path.Combine(dir, "saves", "mc1"));
            foreach (var m in plan.Mods)
                ProjectMods.CopyDir(m.Mod.Folder, Path.Combine(dir, "mods", m.Mod.FolderName));
            if (json["picture"] is JsonValue p && p.TryGetValue<string>(out var picture) && Path.GetFileName(picture) == picture &&
                Projects.IsPicture(picture) && File.Exists(Path.Combine(plan.TempFolder, picture)))
                File.Copy(Path.Combine(plan.TempFolder, picture), Path.Combine(dir, picture), overwrite: true);
            else
                json.Remove("picture");
            File.WriteAllText(Path.Combine(dir, "project.json"), json.ToJsonString(Indented) + "\n");
            if (useSaves)
                ProjectMods.CopyDir(Path.Combine(plan.TempFolder, "saves"), Path.Combine(dir, "saves"));
            return null;
        }
        catch (Exception ex)
        {
            TryDelete(dir);
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
}
