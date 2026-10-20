using Avalonia;
using Avalonia.Headless;
using Avalonia.Threading;
using VendettaSettings.Services;
using VendettaSettings.ViewModels;
using VendettaSettings.Views;

namespace VendettaSettings;

/// <summary>Dev only: draws the window on one page to a PNG (1120 × 700 unless asked), headless (no screen, no DPI). Settings are read
/// but never written.</summary>
internal static class Shot
{
    public static void Render(string page, string path, int width, int height)
    {
        AppBuilder.Configure<App>()
            .UseSkia()
            .UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false })
            .WithInterFont()
            .SetupWithoutStarting();

        var vm = new MainViewModel(GameFolder.Find(), readOnly: true);
        vm.ShowPage(page);
        var window = new MainWindow { DataContext = vm, Width = width, Height = height };
        window.Show();
        for (int i = 0; i < 6; i++)
        {
            Dispatcher.UIThread.RunJobs();
            window.CaptureRenderedFrame();
        }
        window.CaptureRenderedFrame()?.Save(path);
        Console.WriteLine($"shot {page} -> {path}");
    }

    /// <summary>Dev only (--selftest-projects, with VENDETTA_ROOT = a scratch copy of the folder): the Play page's buttons
    /// as a player presses them, with what's on disk printed after each.</summary>
    public static void SelfTestProjects()
    {
        if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("VENDETTA_ROOT")))
        {
            Console.WriteLine("--selftest-projects makes and deletes projects: set VENDETTA_ROOT to a scratch copy of the folder first");
            return;
        }
        var folder = GameFolder.Find();
        var p = new ProjectsViewModel(folder, readOnly: false, () => false, text => Console.WriteLine("  says: " + text));
        void Show(string what)
        {
            Console.WriteLine($"{what}: picked '{p.Key}', settings [play] project = '{GameSettings.LoadProject(folder.SettingsPath)}'");
            foreach (var c in p.Choices)
                Console.WriteLine($"  {(c == p.Selected ? ">" : " ")} {c.Name} | {c.Line} | {c.SaveLine} | mods: {string.Join(", ", c.Mods)}");
        }
        void Try(string name)
        {
            p.DialogName = name;
            p.ConfirmCommand.Execute(null);
            Console.WriteLine($"  '{name}': {(p.DialogError.Length > 0 ? "refused: " + p.DialogError : "done")}");
        }
        Show("start");
        p.NewCommand.Execute(null);
        Console.WriteLine($"new project, the name offered: '{p.DialogName}'");
        foreach (var bad in new[] { "", " lead", "a/b", "dot.", "Original game", "con", new string('x', 61) })
            Try(bad);
        Try("Test A");
        Show("after New (empty card)");
        p.NewCommand.Execute(null);
        p.CopySave = true;
        p.SaveSource = p.SaveSources.First(s => s.Key == "");
        Try("test a");
        Try("Test B");
        Show("after New (the original game's save copied)");
        Console.WriteLine("  Test B's card: " + string.Join(", ", Directory.GetFileSystemEntries(Path.Combine(Projects.FolderOf(folder, "Test B"), "saves", "mc0")).Select(Path.GetFileName)));
        p.CopyCommand.Execute(null);
        Console.WriteLine($"copy, the name offered: '{p.DialogName}'");
        Try(p.DialogName);
        Show("after Copy");
        p.RenameCommand.Execute(null);
        Try("Test A");
        Try("Test C");
        p.RenameCommand.Execute(null);
        Try("test c");
        Show("after Rename (and a rename of the case alone)");
        Console.WriteLine("  project.json: " + File.ReadAllText(Path.Combine(Projects.FolderOf(folder, "test c"), "project.json")).Replace("\n", " "));
        p.Selected = p.Choices.First(c => c.Key == "Test A");
        p.DeleteCommand.Execute(null);
        p.ConfirmCommand.Execute(null);
        Show("after Delete of Test A");
        Console.WriteLine($"play arguments for test c: {folder.PlayArguments("test c")}");
        Console.WriteLine($"play arguments for the original game: {folder.PlayArguments("")}");
    }

    /// <summary>Dev only (--selftest-mods &lt;a mod's zip or folder&gt; [&lt;another&gt; ...], with VENDETTA_ROOT = a scratch copy
    /// of the folder holding a project): the Mods page as a player uses it on the first project (install into it, where the
    /// wrestlers go, on / off with needs, the problems and their fixes, an update that breaks something, remove), with the
    /// project's list and problems printed after each step.</summary>
    public static void SelfTestMods(string[] mods)
    {
        if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("VENDETTA_ROOT")))
        {
            Console.WriteLine("--selftest-mods installs and removes mods: set VENDETTA_ROOT to a scratch copy of the folder first");
            return;
        }
        Console.OutputEncoding = System.Text.Encoding.UTF8;
        var folder = GameFolder.Find();
        var project = Projects.List(folder).FirstOrDefault()?.Name ?? "";
        var m = new ModsViewModel(folder, readOnly: false, text => Console.WriteLine("  says: " + text), () => { });
        m.Open(project);
        void Show(string what)
        {
            Console.WriteLine($"{what}: {m.ListHeading}");
            foreach (var g in m.Groups)
                Console.WriteLine($"  {g.Title} {g.Counts}: " + string.Join(", ", g.Rows.Select(r => (r.IsOn ? "[x] " : "[ ] ") + r.Name +
                    (r.HasPlaces ? $" ({r.PlacesText})" : "") + (r.HasProblem ? " !" : ""))));
            Console.WriteLine($"  {project}'s project.json mods: " + string.Join(" | ", Projects.List(folder).First(p => p.Name == project).Mods));
            if (m.HasNote)
                Console.WriteLine($"  note: {m.Note}{(m.AskOff ? "  [Turn both off] [Keep it on]" : "")}");
            if (m.HasProblems)
            {
                Console.WriteLine($"  {m.ProblemsHeading}:");
                foreach (var p in m.Problems)
                    Console.WriteLine($"    {p.Text}  " + string.Join(" ", p.Fixes.Select(f => $"[{f.Label}]")));
            }
        }
        ModRow? Row(string name) => m.Groups.SelectMany(g => g.Rows).FirstOrDefault(r => r.Name == name);
        Show("start");
        foreach (var path in mods)
        {
            m.Pick(path);
            if (!m.IsInstall)
                continue;
            Console.WriteLine($"install dialog: {m.InstallTitle} | version {m.InstallVersion} | in it {m.InstallContents}" +
                              (m.InstallHasWrestlers ? $" | {m.InstallPlacesHeading}: " + (m.InstallPlacesInline
                                  ? string.Join(", ", m.PlaceRows.Select(r => $"{r.Name} = {r.Selected?.Label}"))
                                  : m.InstallPlacesSummary) : "") +
                              (m.InstallHasNeeds ? " | needs " + string.Join(", ", m.InstallNeedLines) : "") +
                              (m.InstallNeedsMissing ? $" | warning: {m.InstallMissingText}" : "") +
                              (m.InstallReplaces ? $" | {m.InstallReplacesText}" : "") +
                              (m.InstallBreaksSomething ? " | breaks: " + string.Join(" / ", m.InstallBreaks) : "") +
                              $" | button {m.InstallConfirmText}");
            m.ConfirmDialogCommand.Execute(null);
            Show($"installed {Path.GetFileName(path)}");
        }
        // where a pack's wrestlers go: the window, a taken spot, a change, Done
        if (m.Groups.SelectMany(g => g.Rows).FirstOrDefault(r => r.Mod.Wrestlers.Count > 1) is { } pack)
        {
            pack.Pick.Execute(null);
            Console.WriteLine($"picked {m.PickedHeading}: {m.PickedPlacesHeading}: " + string.Join(", ", m.PickedPlaces.Select(p => $"{p.Name} {p.Place}")));
            m.ChangePlacesCommand.Execute(null);
            Console.WriteLine($"where dialog: {m.WhereTitle} | {m.WhereSummary}");
            foreach (var r in m.PlaceRows)
                Console.WriteLine($"  {r.Name} = {r.Selected?.Label}; greyed: " + string.Join(", ", r.Options.Where(o => !o.IsEnabled).Select(o => o.Label)));
            var first = m.PlaceRows[0];
            first.Selected = first.Options[0];
            Console.WriteLine($"  {first.Name} made a new wrestler: {m.WhereSummary}");
            m.WhereSearch = first.Name[..2];
            Console.WriteLine($"  searched '{m.WhereSearch}': " + string.Join(", ", m.WhereRows.Select(r => r.Name)));
            m.ConfirmDialogCommand.Execute(null);
            Show("places changed");
        }
        // on / off with needs, and the problems' fixes
        foreach (var row in m.Groups.SelectMany(g => g.Rows).Where(r => r.Mod.Needs.Count > 0).ToList())
        {
            var needed = m.Groups.SelectMany(g => g.Rows).FirstOrDefault(r => row.Mod.Needs.Any(r.Mod.Is));
            if (needed == null)
                continue;
            needed.Toggle.Execute(null);
            Show($"turned off {needed.Name} (needed by {row.Name})");
            m.KeepOnCommand.Execute(null);
            Row(needed.Name)!.Toggle.Execute(null);
            m.OffBothCommand.Execute(null);
            Show("both turned off");
            Row(row.Name)!.Toggle.Execute(null);
            Show($"turned {row.Name} on (its need comes with it)");
            break;
        }
        while (m.Problems.FirstOrDefault(p => p.Fixes.Any(f => f.IsCommand)) is { } problem)
        {
            var fix = problem.Fixes.First(f => f.IsCommand);
            Console.WriteLine($"fix: {problem.Text} -> [{fix.Label}]");
            fix.Run!.Execute(null);
            if (m.IsWhere)
            {
                Console.WriteLine($"  where dialog: {m.WhereTitle}");
                var clash = m.PlaceRows.FirstOrDefault(r => r.Selected is { IsEnabled: false }) ?? m.PlaceRows[0];
                clash.Selected = clash.Options[0];
                m.ConfirmDialogCommand.Execute(null);
            }
            Show("after the fix");
        }
        if (m.Groups.SelectMany(g => g.Rows).FirstOrDefault() is { } last)
        {
            last.Pick.Execute(null);
            m.AskRemoveCommand.Execute(null);
            Console.WriteLine($"remove dialog: {m.RemoveTitle} | {m.RemoveText}" + (m.RemoveHasWarning ? $" | {m.RemoveWarning}" : ""));
            m.ConfirmDialogCommand.Execute(null);
            Show("removed");
        }
        Console.WriteLine("  the project's mods folder: " + string.Join(", ", Directory.GetDirectories(ProjectMods.RootOf(folder, project)).Select(Path.GetFileName)));
    }

    /// <summary>Dev only (--selftest-share &lt;project&gt;, with VENDETTA_ROOT = a scratch copy of the folder holding that
    /// project and its mods): export it, import it back as a new project (its own copies), and the zips that must be
    /// refused.</summary>
    public static void SelfTestShare(string name)
    {
        if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("VENDETTA_ROOT")))
        {
            Console.WriteLine("--selftest-share exports and imports projects: set VENDETTA_ROOT to a scratch copy of the folder first");
            return;
        }
        Console.OutputEncoding = System.Text.Encoding.UTF8;
        var folder = GameFolder.Find();
        var p = new ProjectsViewModel(folder, readOnly: false, () => false, text => Console.WriteLine("  says: " + text));
        var outDir = Path.Combine(folder.Data, "shared");
        Directory.CreateDirectory(outDir);
        void Pick(string key) => p.Selected = p.Choices.First(c => c.Key == key);
        void ShowProject(string key)
        {
            var dir = Projects.FolderOf(folder, key);
            Console.WriteLine($"  {key}\\project.json: " + File.ReadAllText(Path.Combine(dir, "project.json")).ReplaceLineEndings(" ").Replace("  ", ""));
            Console.WriteLine($"  {key}\\mods: " + string.Join(", ", ProjectMods.List(folder, key).Select(m => $"{m.FolderName} (v{(m.Version.Length > 0 ? m.Version : "-")}{(m.Wrestlers.Count > 0 ? ", " + m.PlacesSummary : "")})")));
            Console.WriteLine($"  {key}\\saves: " + string.Join(", ", Directory.EnumerateFiles(Path.Combine(dir, "saves"), "*", SearchOption.AllDirectories).Select(f => Path.GetRelativePath(dir, f))));
            var choice = p.Choices.FirstOrDefault(c => c.Key == key);
            Console.WriteLine($"  on the Play page: {choice?.Line}{choice?.ProblemsText}");
        }
        void ShowImport()
        {
            Console.WriteLine($"import dialog: {p.DialogTitle} | from {Path.GetFileName(p.ImportFrom)} | name '{p.DialogName}' | {p.ImportNameHint}");
            Console.WriteLine($"  {p.ImportModsHeading}: " + string.Join(", ", p.ImportMods.Select(r => $"{r.Name} {r.Version} ({r.Line})")));
            if (p.ImportHasMissing) Console.WriteLine($"  missing: {p.ImportMissingText}");
            Console.WriteLine($"  saves came with it: {p.ImportHasSaves}, use them: {p.ImportUseSaves}");
        }
        string Export(string key, bool saves)
        {
            Pick(key);
            p.ExportCommand.Execute(null);
            Console.WriteLine($"export dialog: {p.DialogTitle} | {p.ExportModsTitle}{p.ExportModsSize} | {p.ExportModsList} | saves possible {p.ExportCanSaves}" +
                              (p.ExportHasMissing ? $" | warning: {p.ExportMissingText}" : "") + $" | default {Path.GetFileName(p.ExportPath)}");
            p.SetExportPath(Path.Combine(outDir, ProjectShare.DefaultFileName(key)));
            p.ExportSaves = saves;
            p.ConfirmCommand.Execute(null);
            using var zip = System.IO.Compression.ZipFile.OpenRead(p.ExportPath);
            Console.WriteLine($"  zip ({new FileInfo(p.ExportPath).Length} bytes): " + string.Join(", ", zip.Entries.Select(e => e.FullName).Where(n => !n.Contains("/moves/") && !n.Contains("/wrestlers/"))) +
                              $" + {zip.Entries.Count(e => e.FullName.Contains("/moves/") || e.FullName.Contains("/wrestlers/"))} move / wrestler files");
            return p.ExportPath;
        }

        // a save in the project, so it can go with it
        var saveDir = Path.Combine(Projects.FolderOf(folder, name), "saves", "mc0", Projects.SaveFolder);
        Directory.CreateDirectory(saveDir);
        File.WriteAllText(Path.Combine(saveDir, "test save.bin"), "a pretend save");
        p.Reload(name);
        ShowProject(name);

        Console.WriteLine("\n1. export with its saves");
        var zipPath = Export(name, saves: true);

        Console.WriteLine("\n2. import it back: the name is taken, so it gets a new one; its own copies of the mods");
        p.PickImport(zipPath);
        ShowImport();
        var temp = Directory.GetDirectories(Path.GetTempPath(), "Vendetta import *").OrderByDescending(Directory.GetCreationTimeUtc).First();
        p.ConfirmCommand.Execute(null);
        Console.WriteLine($"  picked now: '{p.Key}'; temporary folder gone: {!Directory.Exists(temp)}");
        ShowProject(p.Key);
        var copy = p.Key;

        Console.WriteLine("\n3. the copy changed (a mod's version): the first project keeps its own");
        if (ProjectMods.List(folder, copy).FirstOrDefault() is { } mod)
        {
            var manifest = Path.Combine(mod.Folder, "mod.json");
            var changed = System.Text.Json.Nodes.JsonNode.Parse(File.ReadAllText(manifest))!.AsObject();
            changed["version"] = "3";
            File.WriteAllText(manifest, changed.ToJsonString());
            p.Reload(copy);
            ShowProject(copy);
            ShowProject(name);
        }

        Console.WriteLine("\n4. import without the saves, under another name");
        p.PickImport(zipPath);
        p.ImportUseSaves = false;
        p.DialogName = "Shared " + name;
        p.ConfirmCommand.Execute(null);
        ShowProject(p.Key);

        Console.WriteLine("\n5. cancelled: the temporary folder goes");
        p.PickImport(zipPath);
        temp = Directory.GetDirectories(Path.GetTempPath(), "Vendetta import *").OrderByDescending(Directory.GetCreationTimeUtc).First();
        p.CancelCommand.Execute(null);
        Console.WriteLine($"  temporary folder gone: {!Directory.Exists(temp)}");

        Console.WriteLine("\n6. zips that are refused or mistaken");
        var modZip = Path.Combine(outDir, "a mod.zip");
        if (File.Exists(modZip))
            File.Delete(modZip);
        System.IO.Compression.ZipFile.CreateFromDirectory(ProjectMods.List(folder, name).First().Folder, modZip);
        Console.Write("  a mod's zip on Import:"); p.PickImport(modZip);
        Console.WriteLine("  the shared project's zip on Install a mod: " + ProjectMods.Open(folder, name, zipPath).Error);
        var junk = Path.Combine(outDir, "not a project.zip");
        using (var z = System.IO.Compression.ZipFile.Open(junk, System.IO.Compression.ZipArchiveMode.Create))
            z.CreateEntry("readme.txt");
        Console.Write("  a zip with neither:"); p.PickImport(junk);
        var evil = Path.Combine(outDir, "climbs out.zip");
        using (var z = System.IO.Compression.ZipFile.Open(evil, System.IO.Compression.ZipArchiveMode.Create))
        {
            using (var w = new StreamWriter(z.CreateEntry("project.json").Open()))
                w.Write("{\"format\":1,\"name\":\"Bad/Name:\",\"mods\":[\"Nobody's mod\"]}");
            using (var w = new StreamWriter(z.CreateEntry("../../climbed out.txt").Open()))
                w.Write("never written");
        }
        p.PickImport(evil);
        ShowImport();
        var climbed = Directory.GetFiles(Path.GetTempPath(), "climbed out.txt").Length + Directory.GetFiles(Path.GetDirectoryName(Path.GetTempPath().TrimEnd('\\'))!, "climbed out.txt").Length;
        Console.WriteLine($"  'climbed out.txt' written outside its folder: {climbed > 0}");
        p.ConfirmCommand.Execute(null);
        ShowProject(p.Key);
    }

    /// <summary>Dev only (--selftest, with VENDETTA_ROOT = a scratch copy of the folder): changes settings the way the
    /// pages do, prints settings.toml after each, and with "play" starts the game as PLAY does.</summary>
    public static void SelfTest(bool play)
    {
        // it changes and saves settings: only ever on a scratch copy, never the real settings.toml
        if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("VENDETTA_ROOT")))
        {
            Console.WriteLine("--selftest changes and saves settings: set VENDETTA_ROOT to a scratch copy of the folder first");
            return;
        }
        var folder = GameFolder.Find();
        Console.WriteLine($"folder {folder.Root}; game {folder.HasGame}; exe {folder.GameExe ?? "none"}");
        if (play)
        {
            Console.WriteLine("play: " + (folder.Play(GameSettings.LoadProject(folder.SettingsPath), out var game) ?? $"started (pid {game?.Id})"));
            return;
        }
        var vm = new MainViewModel(folder, readOnly: false);
        var s = vm.Settings;
        Console.WriteLine($"loaded: mode {s.ModeIndex}, screen {s.Screen?.Label}, refresh {s.Refresh?.Label}, scale {s.ScaleIndex}");
        Console.WriteLine("refresh choices in fullscreen: " + string.Join(" | ", s.RefreshChoices.Select(c => c.Label)));
        Console.WriteLine("resolution choices: " + string.Join(" | ", s.ResolutionChoices.Take(6).Select(c => c.Label)));
        s.ModeIndex = 2;
        s.Refresh = s.RefreshChoices.FirstOrDefault(c => c.Value == 60) ?? s.Refresh;
        s.ScaleIndex = 2;
        s.FilterIndex = 2;
        s.Volume = 55;
        s.Reverb = false;
        s.ShapeIndex = 1;
        s.Multitap = false;
        s.Vibration3 = false;
        s.UnlockAll = true;
        // match speed: the No Mercy preset, then Custom from it, a typed speed, one out of range, Custom off again
        s.MatchSpeedIndex = 1;
        s.CustomSpeed = true;
        Console.WriteLine($"custom from WWF No Mercy: {s.CustomMatchSpeed} '{s.CustomSpeedText}'");
        s.CustomSpeedText = "1.1";
        Console.WriteLine($"typed 1.1: {s.CustomMatchSpeed} '{s.CustomSpeedText}'");
        s.TidySpeedText();
        Console.WriteLine($"box left: {s.CustomMatchSpeed} '{s.CustomSpeedText}'");
        s.CustomSpeedText = "3";
        s.TidySpeedText();
        Console.WriteLine($"typed 3: {s.CustomMatchSpeed} '{s.CustomSpeedText}'");
        s.CustomMatchSpeed = 0.86249;
        Console.WriteLine($"slider at 0.86249: {s.CustomMatchSpeed} '{s.CustomSpeedText}'");
        s.CustomSpeed = false;
        s.CustomSpeed = true;
        Console.WriteLine($"custom off and on again: {s.CustomMatchSpeed} '{s.CustomSpeedText}'");
        Console.WriteLine($"status: {s.Status}");
        Console.WriteLine($"summary: {s.SummaryWindow} | {s.SummaryPicture} | {s.SummaryPlayers}");
        Console.WriteLine("--- settings.toml now:");
        Console.Write(File.ReadAllText(folder.SettingsPath));
    }

    /// <summary>Dev only (--selftest-update, with VENDETTA_ROOT = a test install and VENDETTA_UPDATE_URL = a test
    /// server): an update's steps without the window: check, download and check it, put it in place, rebuild (the
    /// install's tools\setup_game.py), then finish, or undo it all when the rebuild fails.</summary>
    public static async Task<int> SelfTestUpdate()
    {
        var folder = GameFolder.Find();
        Console.WriteLine($"folder {folder.Root}; can update {Updater.CanUpdate(folder)}; this version {Updater.CurrentText}");
        try
        {
            var release = await Updater.CheckAsync();
            Console.WriteLine(release == null
                ? "no release"
                : $"latest {release.Version} ({release.ZipSize} bytes, sha256 {release.Sha256 ?? "none"})");
            if (release == null || release.Version <= Updater.Current)
                return 0;
            var zip = Updater.ZipPath(folder);
            await Updater.DownloadAsync(release, zip, (_, _) => { }, CancellationToken.None);
            Console.WriteLine("downloaded and checked");
            var restart = Updater.Apply(folder, zip, release, _ => { });
            var journal = Updater.LoadJournal(folder)!;
            foreach (var e in journal.Entries)
                Console.WriteLine($"  {e.Kind} {e.Path}");
            Console.WriteLine($"in place (journal: {journal.State}); this program replaced: {restart}");
            using var rebuild = System.Diagnostics.Process.Start(folder.SetupStart(null))!;
            var output = await rebuild.StandardOutput.ReadToEndAsync();
            await rebuild.WaitForExitAsync();
            Console.Write(output);
            if (rebuild.ExitCode == 0)
            {
                Updater.Finish(folder);
                Console.WriteLine("rebuilt: the update is finished");
            }
            else
            {
                var back = Updater.RollBack(folder, journal);
                Updater.DeleteJournal(folder);
                Console.WriteLine($"the rebuild failed: undone (this program put back: {back})");
            }
            return 0;
        }
        catch (UpdateException ex)
        {
            Console.WriteLine($"stopped ({ex.Kind}): {ex.Message}");
            return 1;
        }
    }
}
