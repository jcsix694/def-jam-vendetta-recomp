using System.Text.Json;
using System.Text.Json.Nodes;

namespace VendettaSettings.Services;

/// <summary>A project: <c>projects\&lt;name&gt;\project.json</c> (the mods ticked in it, in load order, from the library
/// <c>mods\</c>), an optional picture beside it and its own memory cards (<c>saves\mc0</c>, <c>saves\mc1</c>). The game
/// plays one when started with <c>--project "&lt;name&gt;"</c>; <c>--project ""</c> is the original game (no mods, the
/// saves in <c>game\mc0</c>).</summary>
public sealed class Project
{
    public required string Name { get; init; }
    public required string Folder { get; init; }
    public required IReadOnlyList<string> Mods { get; init; }
    /// <summary>The picture's file, or null.</summary>
    public string? Picture { get; init; }
    public string SavesPath => Path.Combine(Folder, "saves", "mc0");
    public bool HasSave => Directory.Exists(Path.Combine(SavesPath, Projects.SaveFolder));
}

public static class Projects
{
    /// <summary>The game's folder on a memory card.</summary>
    public const string SaveFolder = "BASLUS-20639GAMEDATA";
    public const int MaxNameLength = 60;
    private static readonly string[] PictureTypes = { ".png", ".jpg", ".jpeg" };
    private static readonly JsonSerializerOptions Indented = new() { WriteIndented = true };

    public static string RootOf(GameFolder folder) => Path.Combine(folder.Data, "projects");
    public static string FolderOf(GameFolder folder, string name) => Path.Combine(RootOf(folder), name);

    /// <summary>Every project (a folder with a readable project.json), by name.</summary>
    public static List<Project> List(GameFolder folder)
    {
        var found = new List<Project>();
        var root = RootOf(folder);
        if (!Directory.Exists(root))
            return found;
        foreach (var dir in Directory.GetDirectories(root))
        {
            if (Read(dir) is { } project)
                found.Add(project);
        }
        found.Sort((a, b) => string.Compare(a.Name, b.Name, StringComparison.CurrentCultureIgnoreCase));
        return found;
    }

    private static Project? Read(string dir)
    {
        try
        {
            var json = JsonNode.Parse(File.ReadAllText(Path.Combine(dir, "project.json")))?.AsObject();
            if (json == null)
                return null;
            var mods = new List<string>();
            if (json["mods"] is JsonArray list)
                foreach (var item in list)
                    if (item is JsonValue value && value.TryGetValue<string>(out var text) && text.Length > 0)
                        mods.Add(text);
            string? picture = null;
            if (json["picture"] is JsonValue p && p.TryGetValue<string>(out var file) && file.Length > 0 &&
                Path.GetFileName(file) == file && File.Exists(Path.Combine(dir, file)))
                picture = Path.Combine(dir, file);
            return new Project { Name = Path.GetFileName(dir), Folder = dir, Mods = mods, Picture = picture };
        }
        catch
        {
            return null; // no project.json, or not JSON
        }
    }

    /// <summary>What's wrong with a new name (it is the folder's name), or null.</summary>
    public static string? CheckName(GameFolder folder, string name, string? renaming = null)
    {
        if (name.Length == 0)
            return "Give it a name.";
        if (name != name.Trim() || name.EndsWith('.'))
            return "A name can't start or end with a space, or end with a dot.";
        if (name.Length > MaxNameLength)
            return $"At most {MaxNameLength} letters.";
        if (name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
            return "A name can't hold any of \\ / : * ? \" < > |";
        var reserved = new[] { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "LPT1", "LPT2", "LPT3" };
        if (reserved.Contains(name.Split('.')[0].ToUpperInvariant()))
            return "Windows keeps that name for itself.";
        if (name.Equals("Original game", StringComparison.OrdinalIgnoreCase))
            return "That's the original game's name.";
        var same = renaming != null && renaming.Equals(name, StringComparison.OrdinalIgnoreCase);
        if (!same && Directory.Exists(FolderOf(folder, name)))
            return "A project has that name already.";
        return null;
    }

    /// <summary>A new project with no mods ticked. <paramref name="copySaveFrom"/>: null = an empty memory card, "" = the
    /// original game's save, else that project's. Returns what went wrong, or null.</summary>
    public static string? Create(GameFolder folder, string name, string? copySaveFrom, string? picture)
    {
        if (CheckName(folder, name) is { } bad)
            return bad;
        var dir = FolderOf(folder, name);
        try
        {
            Directory.CreateDirectory(Path.Combine(dir, "saves", "mc0"));
            Directory.CreateDirectory(Path.Combine(dir, "saves", "mc1"));
            var json = new JsonObject
            {
                ["format"] = 1,
                ["name"] = name,
                ["author"] = "",
                ["description"] = "",
                ["mods"] = new JsonArray(),
            };
            if (picture != null)
                json["picture"] = CopyPicture(picture, dir);
            File.WriteAllText(Path.Combine(dir, "project.json"), json.ToJsonString(Indented) + "\n");
            if (copySaveFrom != null)
            {
                var from = copySaveFrom.Length == 0 ? Path.Combine(folder.Data, "game") : Path.Combine(FolderOf(folder, copySaveFrom), "saves");
                CopyDir(Path.Combine(from, "mc0"), Path.Combine(dir, "saves", "mc0"));
                CopyDir(Path.Combine(from, "mc1"), Path.Combine(dir, "saves", "mc1"));
            }
            return null;
        }
        catch (Exception ex)
        {
            return $"Couldn't make the project: {ex.Message}";
        }
    }

    /// <summary>A copy of a project: its ticked mods, picture and saves (the mods themselves stay in the library).</summary>
    public static string? Copy(GameFolder folder, string from, string name)
    {
        if (CheckName(folder, name) is { } bad)
            return bad;
        try
        {
            var dir = FolderOf(folder, name);
            CopyDir(FolderOf(folder, from), dir);
            SetName(dir, name);
            return null;
        }
        catch (Exception ex)
        {
            return $"Couldn't copy the project: {ex.Message}";
        }
    }

    public static string? Rename(GameFolder folder, string from, string name)
    {
        if (CheckName(folder, name, from) is { } bad)
            return bad;
        if (from == name)
            return null;
        try
        {
            var dir = FolderOf(folder, name);
            if (from.Equals(name, StringComparison.OrdinalIgnoreCase))
            {
                // only the case changes: Windows needs a step through another name
                var step = dir + ".renaming";
                Directory.Move(FolderOf(folder, from), step);
                Directory.Move(step, dir);
            }
            else
                Directory.Move(FolderOf(folder, from), dir);
            SetName(dir, name);
            return null;
        }
        catch (Exception ex)
        {
            return $"Couldn't rename the project: {ex.Message}";
        }
    }

    /// <summary>The project and its saves; the mods stay in the library.</summary>
    public static string? Delete(GameFolder folder, string name)
    {
        try
        {
            Directory.Delete(FolderOf(folder, name), recursive: true);
            return null;
        }
        catch (Exception ex)
        {
            return $"Couldn't delete the project: {ex.Message}";
        }
    }

    /// <summary>project.json's "mods": the ticked mods in load order (the other keys kept as they are).</summary>
    public static void SetMods(GameFolder folder, string name, IReadOnlyList<string> mods)
    {
        var path = Path.Combine(FolderOf(folder, name), "project.json");
        var json = JsonNode.Parse(File.ReadAllText(path))?.AsObject() ?? new JsonObject();
        var list = new JsonArray();
        foreach (var m in mods)
            list.Add(m);
        json["mods"] = list;
        var temp = path + ".new";
        File.WriteAllText(temp, json.ToJsonString(Indented) + "\n");
        File.Move(temp, path, overwrite: true);
    }

    public static bool IsPicture(string path) => PictureTypes.Contains(Path.GetExtension(path).ToLowerInvariant());

    /// <summary>The picture copied in as picture.png / .jpg; returns its file name.</summary>
    private static string CopyPicture(string picture, string dir)
    {
        var file = "picture" + Path.GetExtension(picture).ToLowerInvariant();
        File.Copy(picture, Path.Combine(dir, file), overwrite: true);
        return file;
    }

    // project.json's "name" follows the folder's (the other keys are kept as they are)
    private static void SetName(string dir, string name)
    {
        var path = Path.Combine(dir, "project.json");
        var json = JsonNode.Parse(File.ReadAllText(path))?.AsObject() ?? new JsonObject();
        json["name"] = name;
        File.WriteAllText(path, json.ToJsonString(Indented) + "\n");
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

    /// <summary>The installed mods' names, lower case: each folder in mods\ and the "name" its mod.json gives.</summary>
    public static HashSet<string> InstalledMods(GameFolder folder)
    {
        var names = new HashSet<string>();
        var root = Path.Combine(folder.Data, "mods");
        if (!Directory.Exists(root))
            return names;
        foreach (var dir in Directory.GetDirectories(root))
        {
            var manifest = Path.Combine(dir, "mod.json");
            if (!File.Exists(manifest))
                continue;
            names.Add(Path.GetFileName(dir).ToLowerInvariant());
            try
            {
                if (JsonNode.Parse(File.ReadAllText(manifest))?["name"] is JsonValue v && v.TryGetValue<string>(out var n))
                    names.Add(n.ToLowerInvariant());
            }
            catch
            {
                // a mod.json the game can't read either: it says so when it starts
            }
        }
        return names;
    }
}
