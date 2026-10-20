using System.IO.Compression;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace VendettaSettings.Services;

/// <summary>One of a mod's wrestlers and where it goes in this project: a new wrestler, or in a game wrestler's place.</summary>
public sealed class WrestlerPlace
{
    /// <summary>Its folder, as mod.json names it ("wrestlers/Sting").</summary>
    public required string Folder { get; init; }
    /// <summary>The name shown: its wrestler.json's "profile" "name", else its folder's.</summary>
    public required string Name { get; init; }
    /// <summary>"" = a new wrestler, else the game wrestler it replaces (as mod.json gives it).</summary>
    public required string Place { get; init; }
    public bool IsNew => Place.Length == 0;
    public string PlaceText => IsNew ? "New wrestler" : "Replaces " + (GameRoster.Find(Place)?.Name ?? Place);
}

/// <summary>A mod installed in a project: a folder in <c>projects\&lt;name&gt;\mods\</c> with a mod.json.</summary>
public sealed class ModInfo
{
    public required string Folder { get; init; }
    public required string FolderName { get; init; }
    public required string Name { get; init; }
    /// <summary>mod.json's "id", else made from the folder's name (as the game makes it).</summary>
    public required string Id { get; init; }
    public string Author { get; init; } = "";
    public string Version { get; init; } = "";
    public required string Category { get; init; }
    public required IReadOnlyList<string> Needs { get; init; }
    /// <summary>What it holds, in a few words ("1 wrestler · 2 moves").</summary>
    public required string Contents { get; init; }
    public required IReadOnlyList<WrestlerPlace> Wrestlers { get; init; }
    /// <summary>Its new moves' folders, as mod.json lists them.</summary>
    public required IReadOnlyList<string> Moves { get; init; }
    /// <summary>Its new animations' files, as mod.json lists them.</summary>
    public required IReadOnlyList<string> Animations { get; init; }
    /// <summary>"wrestlers": { "&lt;wrestler&gt;": "&lt;moveset change's file&gt;" }.</summary>
    public required IReadOnlyList<(string Wrestler, string File)> MovesetChanges { get; init; }

    /// <summary>Does this name (as project.json or a "needs" gives it: the folder, the name or the id) mean this mod?</summary>
    public bool Is(string name) =>
        name.Equals(FolderName, StringComparison.OrdinalIgnoreCase) || name.Equals(Name, StringComparison.OrdinalIgnoreCase) ||
        name.Equals(Id, StringComparison.OrdinalIgnoreCase);

    /// <summary>"replaces Briggs", "new wrestler", "8 replace · 2 new"; "" without wrestlers.</summary>
    public string PlacesSummary
    {
        get
        {
            if (Wrestlers.Count == 0)
                return "";
            if (Wrestlers.Count == 1)
                return Wrestlers[0].IsNew ? "new wrestler" : "replaces " + (GameRoster.Find(Wrestlers[0].Place)?.Name ?? Wrestlers[0].Place);
            var replace = Wrestlers.Count(w => !w.IsNew);
            var parts = new List<string>();
            if (replace > 0)
                parts.Add($"{replace} replace");
            if (Wrestlers.Count - replace > 0)
                parts.Add($"{Wrestlers.Count - replace} new");
            return string.Join(" · ", parts);
        }
    }
}

/// <summary>A mod picked to install (a zip or a folder), read and unpacked to look at, not installed yet.</summary>
public sealed class ModPackage
{
    public required string Source { get; init; }
    /// <summary>Its files: the folder itself, or the zip unpacked into a temporary folder (<see cref="ProjectMods.Discard"/>).</summary>
    public required string Staged { get; init; }
    public required bool StagedIsTemp { get; init; }
    public required ModInfo Mod { get; init; }
    /// <summary>The project's copy it replaces (the same id, folder or name), or null.</summary>
    public ModInfo? Replaces { get; init; }
    /// <summary>Each of its needs: its name and whether the project has it.</summary>
    public required IReadOnlyList<(string Name, bool InProject)> Needs { get; init; }
}

/// <summary>A project's mods: each installed into the project, its own copy in <c>projects\&lt;name&gt;\mods\</c> (the
/// same mod in two projects is two copies); project.json's "mods" lists the ones that are on, in load order. Where a mod's
/// wrestlers go is written in this copy's mod.json ("new" / "replace": "wrestlers"), so each project can place them its
/// own way. A mod's category is its mod.json's "category" (the mod tool writes it), else worked out from what it holds.</summary>
public static class ProjectMods
{
    /// <summary>The categories in the order the Mods page shows them (any other a mod names comes after).</summary>
    public static readonly string[] KnownCategories = { "Wrestlers", "Moves", "Movesets", "Animations", "Arenas", "Music", "Other" };

    private static readonly JsonSerializerOptions Indented = new() { WriteIndented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping };

    public static string RootOf(GameFolder folder, string project) => Path.Combine(Projects.FolderOf(folder, project), "mods");

    /// <summary>Every mod in the project's folder (a folder with a readable mod.json), by name.</summary>
    public static List<ModInfo> List(GameFolder folder, string project)
    {
        var found = new List<ModInfo>();
        var root = RootOf(folder, project);
        if (!Directory.Exists(root))
            return found;
        foreach (var dir in Directory.GetDirectories(root))
            if (Read(dir) is { } mod)
                found.Add(mod);
        found.Sort((a, b) => string.Compare(a.Name, b.Name, StringComparison.CurrentCultureIgnoreCase));
        return found;
    }

    /// <summary>A mod's folder read, or null (no mod.json, or one that can't be read).</summary>
    public static ModInfo? Read(string dir, string? folderName = null)
    {
        try
        {
            var path = Path.Combine(dir, "mod.json");
            if (!File.Exists(path))
                return null;
            return FromJson(JsonNode.Parse(File.ReadAllText(path))?.AsObject(), dir, folderName ?? Path.GetFileName(dir.TrimEnd('\\', '/')));
        }
        catch
        {
            return null;
        }
    }

    /// <summary>The id the game gives a mod without one: its folder's name in lower case, every run of other characters one
    /// '-' (vendetta_mods.cpp's idFrom).</summary>
    public static string IdFrom(string folderName)
    {
        var id = new System.Text.StringBuilder();
        foreach (var c in folderName.ToLowerInvariant())
        {
            if (char.IsAsciiLetterOrDigit(c) || c == '.')
                id.Append(c);
            else if (id.Length > 0 && id[^1] != '-')
                id.Append('-');
        }
        return id.ToString().TrimEnd('-');
    }

    private static List<string> Strings(JsonNode? node) =>
        node is JsonArray list
            ? list.OfType<JsonValue>().Select(v => v.TryGetValue<string>(out var s) ? s : "").Where(s => s.Length > 0).ToList()
            : new List<string>();

    private static ModInfo? FromJson(JsonObject? json, string dir, string folderName)
    {
        if (json == null)
            return null;
        string Text(JsonObject o, string key) => o[key] is JsonValue v ? (v.TryGetValue<string>(out var s) ? s : v.ToJsonString()) : "";
        var newer = json["new"] as JsonObject;
        var replace = json["replace"] as JsonObject;
        var wrestlers = new List<WrestlerPlace>();
        foreach (var w in Strings(newer?["wrestlers"]))
            wrestlers.Add(new WrestlerPlace { Folder = w, Name = WrestlerName(dir, w), Place = "" });
        if (replace?["wrestlers"] is JsonObject replaced)
            foreach (var (gameName, value) in replaced)
                if (value is JsonValue v && v.TryGetValue<string>(out var w) && w.Length > 0)
                    wrestlers.Add(new WrestlerPlace { Folder = w, Name = WrestlerName(dir, w), Place = gameName });
        var changes = new List<(string, string)>();
        if (json["wrestlers"] is JsonObject edits)
            foreach (var (wrestler, value) in edits)
                if (value is JsonValue v && v.TryGetValue<string>(out var file))
                    changes.Add((wrestler, file));
        var name = Text(json, "name");
        var id = Text(json, "id");
        var moves = Strings(newer?["moves"]);
        var animations = Strings(newer?["animations"]);
        return new ModInfo
        {
            Folder = dir,
            FolderName = folderName,
            Name = name.Length > 0 ? name : folderName,
            Id = id.Length > 0 ? id.ToLowerInvariant() : IdFrom(folderName),
            Author = Text(json, "author"),
            Version = Text(json, "version"),
            Category = CategoryOf(json, wrestlers.Count),
            Needs = Strings(json["needs"]),
            Contents = ContentsOf(wrestlers.Count, moves.Count, animations.Count, (replace?["animations"] as JsonObject)?.Count ?? 0, changes.Count),
            Wrestlers = wrestlers,
            Moves = moves,
            Animations = animations,
            MovesetChanges = changes,
        };
    }

    /// <summary>A wrestler folder's wrestler.json "profile" "name" (the game shows it), else the folder's own name.</summary>
    private static string WrestlerName(string modDir, string folder)
    {
        try
        {
            var path = Path.Combine(modDir, folder, "wrestler.json");
            if (File.Exists(path) && JsonNode.Parse(File.ReadAllText(path))?["profile"]?["name"] is JsonValue v && v.TryGetValue<string>(out var n) && n.Length > 0)
                return n;
        }
        catch
        {
            // its name from the folder
        }
        return Path.GetFileName(folder.TrimEnd('/', '\\'));
    }

    /// <summary>"category" as the mod gives it, else: wrestlers = Wrestlers, new moves = Moves, moveset changes only =
    /// Movesets, animations only = Animations, anything else = Other.</summary>
    private static string CategoryOf(JsonObject json, int wrestlers)
    {
        if (json["category"] is JsonValue v && v.TryGetValue<string>(out var given) && given.Trim().Length > 0)
        {
            given = given.Trim();
            return KnownCategories.FirstOrDefault(c => c.Equals(given, StringComparison.OrdinalIgnoreCase)) ?? given;
        }
        if (wrestlers > 0)
            return "Wrestlers";
        var newer = json["new"] as JsonObject;
        var replace = json["replace"] as JsonObject;
        if (Strings(newer?["moves"]).Count > 0)
            return "Moves";
        var animations = Strings(newer?["animations"]).Count + ((replace?["animations"] as JsonObject)?.Count ?? 0);
        var changes = (json["wrestlers"] as JsonObject)?.Count ?? 0;
        if (animations == 0 && changes > 0)
            return "Movesets";
        if (animations > 0 && changes == 0)
            return "Animations";
        return animations > 0 ? "Movesets" : "Other";
    }

    private static string ContentsOf(int wrestlers, int moves, int animations, int replaced, int changes)
    {
        static string Plural(int n, string one, string many) => n == 1 ? $"1 {one}" : $"{n} {many}";
        var parts = new List<string>();
        if (wrestlers > 0) parts.Add(Plural(wrestlers, "wrestler", "wrestlers"));
        if (moves > 0) parts.Add(Plural(moves, "move", "moves"));
        if (animations > 0) parts.Add(Plural(animations, "animation", "animations"));
        if (replaced > 0) parts.Add(Plural(replaced, "game animation replaced", "game animations replaced"));
        if (changes > 0) parts.Add(Plural(changes, "moveset changed", "movesets changed"));
        return parts.Count > 0 ? string.Join(" · ", parts) : "nothing yet";
    }

    // --- installing ---

    /// <summary>Reads a mod to install into a project: a .zip (mod.json at its top, or in one folder; unpacked into a
    /// temporary folder) or a folder holding mod.json. Returns it, or why it can't be installed.</summary>
    public static (ModPackage? Package, string? Error) Open(GameFolder folder, string project, string source)
    {
        string? temp = null;
        try
        {
            string staged, folderName;
            if (Directory.Exists(source))
            {
                if (!File.Exists(Path.Combine(source, "mod.json")))
                    return (null, "That folder has no mod.json, so it isn't a mod.");
                staged = source;
                folderName = Path.GetFileName(source.TrimEnd('\\', '/'));
            }
            else if (source.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) && File.Exists(source))
            {
                string top;
                using (var zip = ZipFile.OpenRead(source))
                {
                    var (entry, found) = FindManifest(zip);
                    if (entry == null)
                        return (null, zip.Entries.Any(e => e.FullName.Replace('\\', '/').TrimStart('/').Equals("project.json", StringComparison.OrdinalIgnoreCase))
                            ? "That's a shared project, not a mod: import it on the Play page."
                            : "That zip has no mod.json, so it isn't a mod.");
                    top = found;
                }
                temp = Path.Combine(Path.GetTempPath(), "Vendetta mod " + Guid.NewGuid().ToString("N")[..8]);
                Unzip(source, top, temp);
                staged = temp;
                folderName = top.Length > 0 ? top : Path.GetFileNameWithoutExtension(source);
            }
            else
                return (null, "A mod is a .zip file or a folder.");
            if (Read(staged, folderName) is not { } mod)
                return (Fail(temp), "Its mod.json can't be read.");
            if (mod.FolderName.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || mod.FolderName is "." or "..")
                return (Fail(temp), $"\"{mod.FolderName}\" can't be a folder's name.");
            var mods = List(folder, project);
            var replaces = mods.FirstOrDefault(m => m.Id.Equals(mod.Id, StringComparison.OrdinalIgnoreCase))
                           ?? mods.FirstOrDefault(m => m.FolderName.Equals(mod.FolderName, StringComparison.OrdinalIgnoreCase))
                           ?? mods.FirstOrDefault(m => m.Name.Equals(mod.Name, StringComparison.OrdinalIgnoreCase));
            if (replaces != null && Path.GetFullPath(replaces.Folder).Equals(Path.GetFullPath(staged), StringComparison.OrdinalIgnoreCase))
                return (Fail(temp), "That's the installed mod itself.");
            var needs = mod.Needs.Select(n => (mods.FirstOrDefault(m => m.Is(n))?.Name ?? n, mods.Any(m => m.Is(n)))).ToList();
            return (new ModPackage
            {
                Source = source, Staged = staged, StagedIsTemp = temp != null, Mod = mod, Replaces = replaces, Needs = needs,
            }, null);
        }
        catch (Exception ex)
        {
            Fail(temp);
            return (null, $"It can't be read: {ex.Message}");
        }
    }

    private static ModPackage? Fail(string? temp)
    {
        TryDelete(temp);
        return null;
    }

    /// <summary>A package's temporary folder removed (after installing, or when it is cancelled).</summary>
    public static void Discard(ModPackage? package)
    {
        if (package is { StagedIsTemp: true })
            TryDelete(package.Staged);
    }

    private static void TryDelete(string? dir)
    {
        try { if (dir != null && Directory.Exists(dir)) Directory.Delete(dir, recursive: true); } catch { }
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

    /// <summary>Installs it into the project, on (an older copy replaced in place, keeping its spot in the load order):
    /// its id written in if it has none, its wrestlers placed as <paramref name="places"/> says (by folder; "" = new).
    /// Returns the installed folder's name, or why not.</summary>
    public static (string? FolderName, string? Error) Install(GameFolder folder, string project, ModPackage package,
                                                              IReadOnlyDictionary<string, string> places)
    {
        var root = RootOf(folder, project);
        var name = package.Replaces?.FolderName ?? package.Mod.FolderName;
        var target = Path.Combine(root, name);
        var temp = Path.Combine(root, ".installing");
        try
        {
            Directory.CreateDirectory(root);
            TryDelete(temp);
            CopyDir(package.Staged, temp);
            if (!File.Exists(Path.Combine(temp, "mod.json")))
                throw new InvalidOperationException("no mod.json after copying");
            Write(temp, json =>
            {
                if (json["id"] is not JsonValue v || !v.TryGetValue<string>(out var id) || id.Length == 0)
                    json["id"] = package.Mod.Id;
                SetPlaces(json, package.Mod.Wrestlers, places);
            });
            if (package.Replaces != null && Directory.Exists(package.Replaces.Folder))
                Directory.Delete(package.Replaces.Folder, recursive: true);
            if (Directory.Exists(target))
                Directory.Delete(target, recursive: true);
            Directory.Move(temp, target);
            var on = Projects.List(folder).FirstOrDefault(p => p.Name == project)?.Mods.ToList() ?? new List<string>();
            if (!on.Any(n => n.Equals(name, StringComparison.OrdinalIgnoreCase) || (package.Replaces?.Is(n) ?? false)))
                on.Add(name);
            Projects.SetMods(folder, project, on);
            return (name, null);
        }
        catch (Exception ex)
        {
            TryDelete(temp);
            return (null, $"Couldn't install it: {ex.Message}");
        }
    }

    /// <summary>Where its wrestlers go, written into this project's copy of the mod (by folder; "" = new).</summary>
    public static string? Place(ModInfo mod, IReadOnlyDictionary<string, string> places)
    {
        try
        {
            Write(mod.Folder, json => SetPlaces(json, mod.Wrestlers, places));
            return null;
        }
        catch (Exception ex)
        {
            return $"Couldn't change where they go: {ex.Message}";
        }
    }

    // "new": { "wrestlers": [...] } and "replace": { "wrestlers": { "<game wrestler>": "<folder>" } }, the other keys kept
    private static void SetPlaces(JsonObject json, IReadOnlyList<WrestlerPlace> wrestlers, IReadOnlyDictionary<string, string> places)
    {
        if (wrestlers.Count == 0)
            return;
        var newer = json["new"] as JsonObject ?? new JsonObject();
        var replace = json["replace"] as JsonObject ?? new JsonObject();
        var newList = new JsonArray();
        var replaceList = new JsonObject();
        foreach (var w in wrestlers)
        {
            var place = places.TryGetValue(w.Folder, out var p) ? p : w.Place;
            if (place.Length == 0)
                newList.Add(w.Folder);
            else
                replaceList[place] = w.Folder;
        }
        newer.Remove("wrestlers");
        replace.Remove("wrestlers");
        if (newList.Count > 0)
            newer["wrestlers"] = newList;
        if (replaceList.Count > 0)
            replace["wrestlers"] = replaceList;
        json.Remove("new");
        json.Remove("replace");
        if (newer.Count > 0)
            json["new"] = newer;
        if (replace.Count > 0)
            json["replace"] = replace;
    }

    private static void Write(string modDir, Action<JsonObject> change)
    {
        var path = Path.Combine(modDir, "mod.json");
        var json = JsonNode.Parse(File.ReadAllText(path))?.AsObject() ?? new JsonObject();
        change(json);
        var temp = path + ".new";
        File.WriteAllText(temp, json.ToJsonString(Indented) + "\n");
        File.Move(temp, path, overwrite: true);
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

    internal static void CopyDir(string from, string to)
    {
        if (!Directory.Exists(from))
            return;
        Directory.CreateDirectory(to);
        foreach (var file in Directory.GetFiles(from))
            File.Copy(file, Path.Combine(to, Path.GetFileName(file)), overwrite: true);
        foreach (var dir in Directory.GetDirectories(from))
            CopyDir(dir, Path.Combine(to, Path.GetFileName(dir)));
    }

    /// <summary>Deletes the project's copy (with any changes made to it) and takes it off the project's list.</summary>
    public static string? Remove(GameFolder folder, string project, ModInfo mod)
    {
        try
        {
            if (Projects.List(folder).FirstOrDefault(p => p.Name == project) is { } p)
                Projects.SetMods(folder, project, p.Mods.Where(m => !mod.Is(m)).ToList());
            Directory.Delete(mod.Folder, recursive: true);
            return null;
        }
        catch (Exception ex)
        {
            return $"Couldn't remove it: {ex.Message}";
        }
    }
}
