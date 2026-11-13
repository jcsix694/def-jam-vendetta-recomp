using System.Text.Json.Nodes;

namespace VendettaSettings.Services;

public enum ProblemKind { NotInstalled, Unreadable, NeedMissing, Clash, NoSuchWrestler, NoSuchArena, MoveMissing, AnimationMissing }

/// <summary>One problem that stops a project from starting: the line shown (word for word the game's), the mod at fault
/// (its folder, or the name project.json gives) and, for a need, the need's name.</summary>
public sealed class ModProblem
{
    public required ProblemKind Kind { get; init; }
    public required string Text { get; init; }
    public required string Mod { get; init; }
    public string Other { get; init; } = "";
}

/// <summary>The game's check at start (vendetta_modcheck.cpp and the lines vendetta_mods.cpp, vendetta_wrestlers.cpp,
/// vendetta_moves.cpp and vendetta_arenas.cpp mark), done the same way here, so the project shows the same problems and PLAY
/// waits until they are fixed: a mod on but not installed, a mod.json that can't be read, a need missing, two mods
/// replacing the same attire, a wrestler to replace who isn't in the game, an arena to replace that isn't in the game, two
/// mods replacing the same arena, a moveset naming a move the project doesn't have, a move's animation missing.</summary>
public static class ProjectCheck
{
    /// <summary>A mod's arena folder the game takes (vendetta_arenas.cpp findModArenas): its arena.json's "from the game"
    /// naming a game arena's model, or a model.arena in the game's format ("DJARENA1") made from one of the 12 arenas.</summary>
    private static bool UsableArena(string dir)
    {
        try
        {
            if (!Directory.Exists(dir))
                return false;
            var json = Path.Combine(dir, "arena.json");
            if (File.Exists(json) && JsonNode.Parse(File.ReadAllText(json))?["from the game"]?["model"] is JsonValue m &&
                m.TryGetValue<string>(out var model) && GameArenas.Find(model) >= 0)
                return true;
            var file = Path.Combine(dir, "model.arena");
            if (!File.Exists(file))
                return false;
            using var f = File.OpenRead(file);
            var head = new byte[0x40];
            if (f.Read(head, 0, head.Length) != head.Length || System.Text.Encoding.ASCII.GetString(head, 0, 8) != "DJARENA1")
                return false;
            return BitConverter.ToUInt32(head, 0x0C) < 12;
        }
        catch (Exception)
        {
            return false;
        }
    }

    public static List<ModProblem> Problems(GameFolder folder, string project) => Problems(folder, project, null, null);

    /// <summary>As if <paramref name="staged"/> were installed (on) in its folder's place (the Install dialog's "what would
    /// this version break").</summary>
    public static List<ModProblem> Problems(GameFolder folder, string project, string? stagedFolderName, string? staged)
    {
        var problems = new List<ModProblem>();
        void Add(ProblemKind kind, string text, string mod, string other = "")
        {
            if (!problems.Any(p => p.Text == text))
                problems.Add(new ModProblem { Kind = kind, Text = text, Mod = mod, Other = other });
        }
        if (Projects.List(folder).FirstOrDefault(p => p.Name == project) is not { } proj)
            return problems;
        var on = proj.Mods.ToList();
        if (stagedFolderName != null && !on.Any(n => n.Equals(stagedFolderName, StringComparison.OrdinalIgnoreCase)))
            on.Add(stagedFolderName);

        // every folder in the project's mods\ (on or off), the staged one in its place
        var dirs = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var root = ProjectMods.RootOf(folder, project);
        if (Directory.Exists(root))
            foreach (var dir in Directory.GetDirectories(root))
                if (!Path.GetFileName(dir).StartsWith('.'))
                    dirs[Path.GetFileName(dir)] = dir;
        if (stagedFolderName != null && staged != null)
            dirs[stagedFolderName] = staged;
        var all = new List<ModInfo>();
        var unreadable = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var (name, dir) in dirs)
        {
            if (!File.Exists(Path.Combine(dir, "mod.json")))
                continue;
            if (ProjectMods.Read(dir, name) is { } mod)
                all.Add(mod);
            else
                unreadable.Add(name);
        }

        // the mods on, in the project's order
        var onMods = new List<ModInfo>();
        foreach (var want in on)
        {
            if (all.FirstOrDefault(m => m.FolderName.Equals(want, StringComparison.OrdinalIgnoreCase) ||
                                        m.Name.Equals(want, StringComparison.OrdinalIgnoreCase)) is { } mod)
            {
                if (!onMods.Contains(mod))
                    onMods.Add(mod);
            }
            else if (unreadable.Contains(want))
                Add(ProblemKind.Unreadable, $"{want}'s mod.json can't be read.", want);
            else
                Add(ProblemKind.NotInstalled, $"{want} is on but isn't installed in this project.", want);
        }

        // needs: a mod whose need isn't on is left out (as the game does)
        var usable = new List<ModInfo>();
        foreach (var mod in onMods)
        {
            var ok = true;
            foreach (var need in mod.Needs)
                if (!onMods.Any(o => o != mod && o.Is(need)))
                {
                    var needName = all.FirstOrDefault(a => a.Is(need))?.Name ?? need;
                    Add(ProblemKind.NeedMissing, $"{mod.Name} needs {needName}.", mod.FolderName, needName);
                    ok = false;
                }
            if (ok)
                usable.Add(mod);
        }

        // what the recomp itself brings in a mod's form (game\standard\mod.json, read by the game before any mod, in every
        // project): its wrestlers are replaced and named as the game's own, its moves and animations are there for every
        // mod. Its own problems are never the project's.
        var standardDir = Path.Combine(folder.Data, "game", "standard");
        var standard = File.Exists(Path.Combine(standardDir, "mod.json")) ? ProjectMods.Read(standardDir, "standard") : null;
        var named = standard == null ? usable : usable.Prepend(standard).ToList();

        // a project with its own list (project.json "roster" / "new slots", "stages" / "new places"): the list says who
        // is where, and the game checks it at start; the mods' own places are only where each was made to go
        var (rosterListed, stagesListed) = OwnLists(Path.Combine(Path.GetDirectoryName(root) ?? "", "project.json"));

        // replacements: the game's wrestler, and one mod per attire (the first in the load order)
        var taken = new Dictionary<(string, int), string>();
        foreach (var mod in usable.Where(_ => !rosterListed))
            foreach (var w in mod.Wrestlers.Where(w => !w.IsNew))
            {
                if (!Directory.Exists(Path.Combine(mod.Folder, w.Folder)))
                    continue; // the game leaves it out with a note
                if ((GameRoster.Find(w.Place) ?? StandardWrestler(standard, w.Place)) is not { } game)
                {
                    Add(ProblemKind.NoSuchWrestler, $"{mod.Name} replaces {w.Place}, who isn't in the game.", mod.FolderName, w.Folder);
                    continue;
                }
                foreach (var n in AttireSpots(Path.Combine(mod.Folder, w.Folder)).Where(n => n <= game.Attires))
                {
                    if (taken.TryGetValue((game.Full, n), out var was))
                        Add(ProblemKind.Clash, $"{was} and {mod.Name} both replace {w.Place}{(n > 1 ? "'s attire " + n : "")}.", mod.FolderName, w.Folder);
                    else
                        taken[(game.Full, n)] = mod.Name;
                }
            }

        // the arenas: each one replaced is one of the game's 12, by one mod (in the game's order: the name, then a mod
        // earlier in the load order whose arena the game took, then this mod's own arena)
        var arenaTaken = new Dictionary<int, string>();
        foreach (var mod in usable.Where(_ => !stagesListed))
            foreach (var (arena, arenaFolder) in mod.ArenaReplacements)
            {
                var venue = GameArenas.Find(arena);
                if (venue < 0)
                {
                    Add(ProblemKind.NoSuchArena, $"{mod.Name} replaces the arena \"{arena}\", which isn't in the game.", mod.FolderName, arenaFolder);
                    continue;
                }
                if (arenaTaken.TryGetValue(venue, out var was))
                {
                    Add(ProblemKind.Clash, $"{was} and {mod.Name} both replace {GameArenas.Names[venue]}.", mod.FolderName, arenaFolder);
                    continue;
                }
                if (arenaFolder.Length == 0 || !UsableArena(Path.Combine(mod.Folder, arenaFolder)))
                    continue; // the game leaves it out with a note (the game's arena stays)
                arenaTaken[venue] = mod.Name;
            }

        // the new moves by name (their name on screen and their folder's, each also as "<id>/<name>"); their animations
        var moves = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var mod in named)
            foreach (var item in mod.Moves)
            {
                var moveDir = Path.Combine(mod.Folder, item);
                JsonObject? move = null;
                try { move = JsonNode.Parse(File.ReadAllText(Path.Combine(moveDir, "move.json")))?.AsObject(); } catch { }
                if (move == null)
                    continue; // the game leaves it out with a note
                var folderName = Path.GetFileName(item.TrimEnd('/', '\\'));
                var name = move["name"] is JsonValue n && n.TryGetValue<string>(out var s) && s.Length > 0 ? s : folderName;
                foreach (var key in new[] { name, folderName })
                {
                    moves.Add(key);
                    moves.Add(mod.Id + "/" + key);
                }
                if (mod == standard || move["animations"] is not JsonObject animations)
                    continue;
                foreach (var (_, value) in animations)
                {
                    if (value is not JsonValue v || !v.TryGetValue<string>(out var file))
                        continue;
                    var slash = file.IndexOf('/');
                    var prefix = slash > 0 ? file[..slash] : "";
                    var other = prefix.Length > 0 ? named.FirstOrDefault(u => u.Id.Equals(prefix, StringComparison.OrdinalIgnoreCase)) : null;
                    var path = other != null ? Path.Combine(other.Folder, file[(slash + 1)..]) : Path.Combine(moveDir, file);
                    if (File.Exists(path))
                        continue;
                    var what = $"{mod.Name}'s move {name}";
                    if (other != null && other != mod)
                        Add(ProblemKind.AnimationMissing, $"{what} needs an animation {other.Name} no longer has.", mod.FolderName);
                    else if (other == null && prefix.Length > 0 && (all.Any(a => a.Id.Equals(prefix, StringComparison.OrdinalIgnoreCase)) || prefix.Contains('.')))
                        Add(ProblemKind.AnimationMissing, $"{what} needs {all.FirstOrDefault(a => a.Id.Equals(prefix, StringComparison.OrdinalIgnoreCase))?.Name ?? prefix}.", mod.FolderName);
                    else
                        Add(ProblemKind.AnimationMissing, $"{what} is missing one of its animations.", mod.FolderName);
                }
            }
        // the new animations a slot may name (their file's name, plain and as "<id>/<name>")
        var animationNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var mod in named)
            foreach (var a in mod.Animations)
            {
                var stem = Path.GetFileNameWithoutExtension(a);
                animationNames.Add(stem);
                animationNames.Add(mod.Id + "/" + stem);
            }

        // a moveset file's slots (key null), or the slots under `key` in it (a wrestler.json's "moveset")
        void CheckSlots(ModInfo mod, string wrestler, string file, string? key = null)
        {
            JsonObject? moveset = null;
            try
            {
                var whole = JsonNode.Parse(File.ReadAllText(file))?.AsObject();
                moveset = key == null ? whole : whole?[key]?.AsObject();
            }
            catch { }
            if (moveset == null)
            {
                Add(ProblemKind.MoveMissing, $"{wrestler}'s moveset in {mod.Name} can't be read.", mod.FolderName);
                return;
            }
            foreach (var (_, value) in moveset)
            {
                var node = value is JsonObject o ? o["move"] : value;
                if (node is not JsonValue v || !v.TryGetValue<string>(out var name))
                    continue;
                if (name.Equals("none", StringComparison.OrdinalIgnoreCase) || IsGameMove(name) || moves.Contains(name) || animationNames.Contains(name))
                    continue;
                var slash = name.IndexOf('/');
                Add(ProblemKind.MoveMissing, $"{mod.Name} gives {wrestler} the move {(slash < 0 ? name : name[(slash + 1)..])}, which isn't in this project.", mod.FolderName);
            }
        }

        // the wrestlers' own movesets (the "moveset" in the folder's wrestler.json and in each attire's)
        var newWrestlerNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var mod in named)
            foreach (var w in mod.Wrestlers)
            {
                var dir = Path.Combine(mod.Folder, w.Folder);
                if (w.IsNew)
                {
                    newWrestlerNames.Add(Path.GetFileName(w.Folder.TrimEnd('/', '\\')));
                    newWrestlerNames.Add(w.Name);
                }
                if (mod == standard)
                    continue;
                foreach (var file in new[] { Path.Combine(dir, "wrestler.json") }.Concat(Enumerable.Range(1, 3).Select(n => Path.Combine(dir, "attires", n.ToString(), "wrestler.json"))))
                    if (File.Exists(file) && HasKey(file, "moveset"))
                        CheckSlots(mod, w.Name, file, "moveset");
            }
        // the mods' moveset changes for any wrestler
        foreach (var mod in usable)
            foreach (var (wrestler, file) in mod.MovesetChanges)
            {
                if (GameRoster.Find(wrestler) == null && !newWrestlerNames.Contains(wrestler))
                {
                    Add(ProblemKind.NoSuchWrestler, $"{mod.Name} changes {wrestler}'s moves, but there's no wrestler called that.", mod.FolderName);
                    continue;
                }
                var path = Path.Combine(mod.Folder, file);
                if (File.Exists(path))
                    CheckSlots(mod, wrestler, path);
            }
        return problems;
    }

    // One of the recomp's own wrestlers (the standard content's) by its folder's name, its name or its full name, any
    // case, as a game wrestler: its attires are its attire folders (1 without any)
    private static GameRoster.Wrestler? StandardWrestler(ModInfo? standard, string name)
    {
        var want = name.Trim();
        foreach (var w in standard?.Wrestlers.Where(w => w.IsNew) ?? [])
        {
            var dir = Path.Combine(standard!.Folder, w.Folder);
            var full = w.Name;
            try
            {
                if (JsonNode.Parse(File.ReadAllText(Path.Combine(dir, "wrestler.json")))?["profile"]?["full name"] is JsonValue v &&
                    v.TryGetValue<string>(out var f) && f.Length > 0)
                    full = f;
            }
            catch
            {
                // its name stands for its full name
            }
            if (new[] { w.Name, full, Path.GetFileName(w.Folder.TrimEnd('/', '\\')) }.Any(n => n.Equals(want, StringComparison.OrdinalIgnoreCase)))
                return new GameRoster.Wrestler(w.Name, full.ToUpperInvariant(), w.Name.ToUpperInvariant(), AttireSpots(dir).Count);
        }
        return null;
    }

    // A JSON file whose top level has `key` (a file that can't be read: no)
    private static bool HasKey(string file, string key)
    {
        try { return JsonNode.Parse(File.ReadAllText(file)) is JsonObject o && o.ContainsKey(key); }
        catch { return false; }
    }

    // "game move <number> <name>" (as the game reads it)
    private static bool IsGameMove(string value)
    {
        const string want = "game move ";
        if (value.Length <= want.Length || !value.StartsWith(want, StringComparison.OrdinalIgnoreCase))
            return false;
        var digits = new string(value[want.Length..].TakeWhile(char.IsAsciiDigit).ToArray());
        return digits.Length > 0 && int.TryParse(digits, out var n) && n < 999;
    }

    /// <summary>The attires a replacement folder brings: attires\1..9 that hold an attire's files, else the folder itself
    /// is attire 1 (as the game reads it).</summary>
    public static List<int> AttireSpots(string dir)
    {
        string[] files = { "wrestler.json", "render.png", "icon.png", "logo.png", "model.wrs", "model.glb", "model.gltf" };
        var spots = new List<int>();
        var any = false;
        for (var n = 1; n <= 9; n++)
        {
            var attire = Path.Combine(dir, "attires", n.ToString());
            if (!Directory.Exists(attire))
                continue;
            any = true;
            if (files.Any(f => File.Exists(Path.Combine(attire, f))) || Directory.Exists(Path.Combine(attire, "textures")))
                spots.Add(n);
        }
        if (!any)
            spots.Add(1);
        return spots;
    }

    /// <summary>Whether a project's own file lists its Choose Fighter slots, and its stages.</summary>
    private static (bool Roster, bool Stages) OwnLists(string projectJson)
    {
        try
        {
            using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(projectJson),
                new System.Text.Json.JsonDocumentOptions { CommentHandling = System.Text.Json.JsonCommentHandling.Skip, AllowTrailingCommas = true });
            var top = doc.RootElement;
            if (top.ValueKind != System.Text.Json.JsonValueKind.Object)
                return (false, false);
            return (top.TryGetProperty("roster", out _) || top.TryGetProperty("new slots", out _),
                    top.TryGetProperty("stages", out _) || top.TryGetProperty("new places", out _));
        }
        catch (Exception)
        {
            return (false, false);
        }
    }
}
