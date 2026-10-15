using System.IO.Compression;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace VendettaSettings.Services;

/// <summary>An installed mod: a folder in the library <c>mods\</c> with a mod.json.</summary>
public sealed class ModInfo
{
    public required string Folder { get; init; }
    public required string FolderName { get; init; }
    public required string Name { get; init; }
    public string Author { get; init; } = "";
    public string Version { get; init; } = "";
    public string Description { get; init; } = "";
    public required string Category { get; init; }
    public required IReadOnlyList<string> Needs { get; init; }
    /// <summary>What it changes, in a few words ("1 new move · 44 movesets").</summary>
    public required string Changes { get; init; }

    /// <summary>Does this name (as a project's list or a "needs" gives it: the folder or the mod's name) mean this mod?</summary>
    public bool Is(string name) =>
        name.Equals(FolderName, StringComparison.OrdinalIgnoreCase) || name.Equals(Name, StringComparison.OrdinalIgnoreCase);
}

/// <summary>A mod picked to install (a zip or a folder), read but not installed yet.</summary>
public sealed class ModPackage
{
    public required string Source { get; init; }
    public required ModInfo Mod { get; init; }
    /// <summary>The installed mod it replaces (the same folder or the same name), or null.</summary>
    public ModInfo? Replaces { get; init; }
    /// <summary>Each of its needs and whether the library has it.</summary>
    public required IReadOnlyList<(string Name, bool Installed)> Needs { get; init; }
}

/// <summary>The mods library (<c>mods\</c>): every installed mod once; projects tick them (project.json's "mods").
/// A mod's category is its mod.json's "category" (the mod tool writes it), else worked out from what it holds.</summary>
public static class ModLibrary
{
    /// <summary>The categories in the order the Mods page shows them (any other a mod names comes after).</summary>
    public static readonly string[] KnownCategories = { "Moves", "Movesets", "Animations", "Wrestlers", "Arenas", "Music", "Other" };

    private static readonly JsonSerializerOptions Indented = new() { WriteIndented = true };

    public static string RootOf(GameFolder folder) => Path.Combine(folder.Data, "mods");

    /// <summary>Every installed mod (a folder with a readable mod.json), by name.</summary>
    public static List<ModInfo> List(GameFolder folder)
    {
        var found = new List<ModInfo>();
        var root = RootOf(folder);
        if (!Directory.Exists(root))
            return found;
        foreach (var dir in Directory.GetDirectories(root))
            if (Read(dir) is { } mod)
                found.Add(mod);
        found.Sort((a, b) => string.Compare(a.Name, b.Name, StringComparison.CurrentCultureIgnoreCase));
        return found;
    }

    private static ModInfo? Read(string dir)
    {
        try
        {
            var path = Path.Combine(dir, "mod.json");
            if (!File.Exists(path))
                return null;
            return FromJson(JsonNode.Parse(File.ReadAllText(path))?.AsObject(), dir, Path.GetFileName(dir));
        }
        catch
        {
            return null; // not JSON: the game says so when it starts
        }
    }

    private static ModInfo? FromJson(JsonObject? json, string dir, string folderName)
    {
        if (json == null)
            return null;
        string Text(string key) => json[key] is JsonValue v ? (v.TryGetValue<string>(out var s) ? s : v.ToJsonString()) : "";
        var needs = new List<string>();
        if (json["needs"] is JsonArray list)
            foreach (var item in list)
                if (item is JsonValue value && value.TryGetValue<string>(out var text) && text.Length > 0)
                    needs.Add(text);
        var name = Text("name");
        return new ModInfo
        {
            Folder = dir,
            FolderName = folderName,
            Name = name.Length > 0 ? name : folderName,
            Author = Text("author"),
            Version = Text("version"),
            Description = Text("description"),
            Category = CategoryOf(json),
            Needs = needs,
            Changes = ChangesOf(json),
        };
    }

    private static int Count(JsonNode? node) => node switch
    {
        JsonArray a => a.Count,
        JsonObject o => o.Count,
        _ => 0,
    };

    /// <summary>"category" as the mod gives it, else: new moves = Moves, wrestlers' slots only = Movesets, animations only =
    /// Animations, anything else = Other.</summary>
    private static string CategoryOf(JsonObject json)
    {
        if (json["category"] is JsonValue v && v.TryGetValue<string>(out var given) && given.Trim().Length > 0)
        {
            given = given.Trim();
            return KnownCategories.FirstOrDefault(c => c.Equals(given, StringComparison.OrdinalIgnoreCase)) ?? given;
        }
        var newer = json["new"] as JsonObject;
        var replace = json["replace"] as JsonObject;
        if (Count(newer?["moves"]) > 0)
            return "Moves";
        var animations = Count(newer?["animations"]) + Count(replace?["animations"]);
        var others = (newer?.Count(kv => kv.Key != "moves" && kv.Key != "animations" && Count(kv.Value) > 0) ?? 0) +
                     (replace?.Count(kv => kv.Key != "animations" && Count(kv.Value) > 0) ?? 0);
        var wrestlers = Count(json["wrestlers"]);
        if (others == 0 && animations == 0 && wrestlers > 0)
            return "Movesets";
        if (others == 0 && animations > 0 && wrestlers == 0)
            return "Animations";
        return animations > 0 && wrestlers > 0 && others == 0 ? "Movesets" : "Other";
    }

    private static string ChangesOf(JsonObject json)
    {
        var parts = new List<string>();
        static string Plural(int n, string one, string many) => n == 1 ? $"1 {one}" : $"{n} {many}";
        if (json["new"] is JsonObject newer)
            foreach (var (key, value) in newer)
            {
                var n = Count(value);
                if (n == 0) continue;
                parts.Add(key switch
                {
                    "moves" => Plural(n, "new move", "new moves"),
                    "animations" => Plural(n, "new animation", "new animations"),
                    _ => $"{n} new {key}",
                });
            }
        if (json["replace"] is JsonObject replace)
            foreach (var (key, value) in replace)
            {
                var n = Count(value);
                if (n == 0) continue;
                parts.Add(key switch
                {
                    "animations" => Plural(n, "animation replaced", "animations replaced"),
                    _ => $"{n} {key} replaced",
                });
            }
        if (Count(json["wrestlers"]) is var w and > 0)
            parts.Add(Plural(w, "wrestler's moves", "wrestlers' moves"));
        return parts.Count > 0 ? string.Join(" · ", parts) : "nothing yet";
    }

    /// <summary>The projects that tick this mod.</summary>
    public static List<string> UsedBy(GameFolder folder, ModInfo mod) =>
        Projects.List(folder).Where(p => p.Mods.Any(mod.Is)).Select(p => p.Name).ToList();

    // --- installing ---

    /// <summary>Reads a mod to install: a .zip (mod.json at its top, or in one folder) or a folder holding mod.json.
    /// Returns it, or why it can't be installed.</summary>
    public static (ModPackage? Package, string? Error) Open(GameFolder folder, string source)
    {
        try
        {
            ModInfo? mod;
            if (Directory.Exists(source))
            {
                if (!File.Exists(Path.Combine(source, "mod.json")))
                    return (null, "That folder has no mod.json, so it isn't a mod.");
                mod = FromJson(JsonNode.Parse(File.ReadAllText(Path.Combine(source, "mod.json")))?.AsObject(), source,
                    Path.GetFileName(source.TrimEnd('\\', '/')));
            }
            else if (source.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) && File.Exists(source))
            {
                using var zip = ZipFile.OpenRead(source);
                var (entry, top) = FindManifest(zip);
                if (entry == null)
                    return (null, zip.Entries.Any(e => e.FullName.Replace('\\', '/').TrimStart('/').Equals("project.json", StringComparison.OrdinalIgnoreCase))
                        ? "That's a shared project, not a mod: import it on the Play page."
                        : "That zip has no mod.json, so it isn't a mod.");
                using var reader = new StreamReader(entry.Open());
                mod = FromJson(JsonNode.Parse(reader.ReadToEnd())?.AsObject(), source,
                    top.Length > 0 ? top : Path.GetFileNameWithoutExtension(source));
            }
            else
                return (null, "A mod is a .zip file or a folder.");
            if (mod == null)
                return (null, "Its mod.json can't be read.");
            if (mod.FolderName.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || mod.FolderName is "." or "..")
                return (null, $"\"{mod.FolderName}\" can't be a folder's name.");
            var library = List(folder);
            var replaces = library.FirstOrDefault(m => m.FolderName.Equals(mod.FolderName, StringComparison.OrdinalIgnoreCase))
                           ?? library.FirstOrDefault(m => m.Name.Equals(mod.Name, StringComparison.OrdinalIgnoreCase));
            if (replaces != null && Path.GetFullPath(replaces.Folder).Equals(Path.GetFullPath(source), StringComparison.OrdinalIgnoreCase))
                return (null, "That's the installed mod itself.");
            var needs = mod.Needs.Select(n => (n, library.Any(m => m.Is(n)))).ToList();
            return (new ModPackage { Source = source, Mod = mod, Replaces = replaces, Needs = needs }, null);
        }
        catch (Exception ex)
        {
            return (null, $"It can't be read: {ex.Message}");
        }
    }

    /// <summary>mod.json at the zip's top, else in one folder (the shallowest); the folder's name ("" at the top).</summary>
    internal static (ZipArchiveEntry? Entry, string Top) FindManifest(ZipArchive zip)
    {
        ZipArchiveEntry? best = null;
        var bestDepth = int.MaxValue;
        foreach (var entry in zip.Entries)
        {
            var parts = entry.FullName.Replace('\\', '/').Split('/', StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length == 0 || !parts[^1].Equals("mod.json", StringComparison.OrdinalIgnoreCase) || parts.Length > 2)
                continue;
            if (parts.Length < bestDepth)
            {
                best = entry;
                bestDepth = parts.Length;
            }
        }
        if (best == null)
            return (null, "");
        var path = best.FullName.Replace('\\', '/').Split('/', StringSplitOptions.RemoveEmptyEntries);
        return (best, path.Length == 2 ? path[0] : "");
    }

    /// <summary>Installs it into the library (an older copy replaced, under that copy's folder name, so the projects that
    /// tick it keep it). Returns the installed folder's name, or why not.</summary>
    public static (string? FolderName, string? Error) Install(GameFolder folder, ModPackage package)
    {
        var root = RootOf(folder);
        var name = package.Replaces?.FolderName ?? package.Mod.FolderName;
        var target = Path.Combine(root, name);
        var temp = Path.Combine(root, ".installing");
        try
        {
            Directory.CreateDirectory(root);
            if (Directory.Exists(temp))
                Directory.Delete(temp, recursive: true);
            if (Directory.Exists(package.Source))
                CopyDir(package.Source, temp);
            else
            {
                using (var zip = ZipFile.OpenRead(package.Source))
                    Unzip(package.Source, FindManifest(zip).Top, temp);
            }
            if (!File.Exists(Path.Combine(temp, "mod.json")))
                throw new InvalidOperationException("no mod.json after unpacking");
            if (package.Replaces != null && Directory.Exists(package.Replaces.Folder))
                Directory.Delete(package.Replaces.Folder, recursive: true);
            if (Directory.Exists(target))
                Directory.Delete(target, recursive: true);
            Directory.Move(temp, target);
            return (name, null);
        }
        catch (Exception ex)
        {
            try { if (Directory.Exists(temp)) Directory.Delete(temp, recursive: true); } catch { }
            return (null, $"Couldn't install it: {ex.Message}");
        }
    }

    /// <summary>The zip's files into the folder, from the folder <paramref name="top"/> down ("" = all; the files above or
    /// beside it are left out). A name that climbs out of the folder ("..\") is never written.</summary>
    internal static void Unzip(string zipPath, string top, string to)
    {
        using var zip = ZipFile.OpenRead(zipPath);
        var prefix = top.Length > 0 ? top + "/" : "";
        var full = Path.GetFullPath(to) + Path.DirectorySeparatorChar;
        Directory.CreateDirectory(to);
        foreach (var entry in zip.Entries)
        {
            var name = entry.FullName.Replace('\\', '/');
            if (!name.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                continue;
            var relative = name[prefix.Length..];
            if (relative.Length == 0)
                continue;
            var path = Path.GetFullPath(Path.Combine(to, relative));
            if (!path.StartsWith(full, StringComparison.OrdinalIgnoreCase))
                continue; // a name that climbs out of the folder ("..\"): never written
            if (name.EndsWith('/'))
            {
                Directory.CreateDirectory(path);
                continue;
            }
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            entry.ExtractToFile(path, overwrite: true);
        }
    }

    private static void CopyDir(string from, string to)
    {
        Directory.CreateDirectory(to);
        foreach (var file in Directory.GetFiles(from))
            File.Copy(file, Path.Combine(to, Path.GetFileName(file)), overwrite: true);
        foreach (var dir in Directory.GetDirectories(from))
            CopyDir(dir, Path.Combine(to, Path.GetFileName(dir)));
    }

    /// <summary>Takes the mod out of the library and out of every project that ticks it (their saves stay).</summary>
    public static string? Remove(GameFolder folder, ModInfo mod)
    {
        try
        {
            foreach (var p in Projects.List(folder).Where(p => p.Mods.Any(mod.Is)))
                Projects.SetMods(folder, p.Name, p.Mods.Where(m => !mod.Is(m)).ToList());
            Directory.Delete(mod.Folder, recursive: true);
            return null;
        }
        catch (Exception ex)
        {
            return $"Couldn't remove it: {ex.Message}";
        }
    }
}
