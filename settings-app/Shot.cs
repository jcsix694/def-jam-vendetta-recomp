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

    /// <summary>Dev only (--selftest-mods &lt;a mod's zip&gt;, with VENDETTA_ROOT = a scratch copy of the folder holding a
    /// project): the Mods page as a player uses it, with the project's list printed after each step.</summary>
    public static void SelfTestMods(string zip)
    {
        if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("VENDETTA_ROOT")))
        {
            Console.WriteLine("--selftest-mods installs and removes mods: set VENDETTA_ROOT to a scratch copy of the folder first");
            return;
        }
        var folder = GameFolder.Find();
        var project = Projects.List(folder).FirstOrDefault()?.Name ?? "";
        var m = new ModsViewModel(folder, readOnly: false, text => Console.WriteLine("  says: " + text), () => { });
        m.Open(project);
        void Show(string what)
        {
            Console.WriteLine($"{what}: {m.LibraryHeading}");
            foreach (var g in m.Groups)
                Console.WriteLine($"  {g.Title} {g.Counts}: " + string.Join(", ", g.Rows.Select(r => (r.IsOn ? "[x] " : "[ ] ") + r.Name)));
            Console.WriteLine($"  {project}'s project.json mods: " + string.Join(" | ", Projects.List(folder).First(p => p.Name == project).Mods));
            if (m.HasNote)
                Console.WriteLine($"  note: {m.Note}{(m.AskUntick ? "  [Untick both] [Keep it ticked]" : "")}");
        }
        ModRow Row(string name) => m.Groups.SelectMany(g => g.Rows).First(r => r.Name == name);
        Show("start");
        foreach (var g in m.Groups.SelectMany(g => g.Rows))
            Console.WriteLine($"  {g.Name}: category {g.Mod.Category}; changes {g.Changes}; needs {string.Join(", ", g.Mod.Needs)}");
        Row("Needs SCM").Toggle.Execute(null);
        Show("ticked Needs SCM");
        Row("Sweet Chin Music test").Toggle.Execute(null);
        Show("unticked Sweet Chin Music (needed)");
        m.KeepTickedCommand.Execute(null);
        Show("kept");
        m.Order[0].Down.Execute(null);
        Show("the first moved down");
        Row("Sweet Chin Music test").Toggle.Execute(null);
        m.UntickBothCommand.Execute(null);
        Show("unticked both");
        m.Search = "readd";
        Show("searched 'readd'");
        m.Search = "";
        m.Category = "Animations";
        Show("category Animations");
        m.Category = "All";
        m.Pick(zip);
        Console.WriteLine($"install dialog: {m.InstallTitle} | version {m.InstallVersion} | {m.InstallCategory} | {m.InstallChanges} | needs {m.InstallNeeds} | replaces: {m.InstallReplacesText} | {m.TickTooText}: {m.TickToo}");
        m.ConfirmDialogCommand.Execute(null);
        Show("installed");
        Console.WriteLine($"  picked: {m.PickedHeading} | {m.PickedDescription} | used by {m.PickedUsedBy}");
        m.AskRemoveCommand.Execute(null);
        Console.WriteLine($"remove dialog: {m.RemoveTitle} | {m.RemoveText} {string.Join(", ", m.RemoveUsers)}");
        m.ConfirmDialogCommand.Execute(null);
        Show("removed");
        Console.WriteLine("  mods folder: " + string.Join(", ", Directory.GetDirectories(ModLibrary.RootOf(folder)).Select(Path.GetFileName)));
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
