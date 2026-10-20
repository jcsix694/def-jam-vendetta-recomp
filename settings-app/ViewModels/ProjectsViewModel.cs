using System.Collections.ObjectModel;
using Avalonia.Media.Imaging;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using VendettaSettings.Services;

namespace VendettaSettings.ViewModels;

/// <summary>One line of "What to play": the original game (Key "") or a project (Key = its name).</summary>
public sealed class PlayChoice
{
    public required string Key { get; init; }
    public required string Name { get; init; }
    public required string Line { get; init; }
    public Bitmap? Picture { get; init; }
    public bool IsOriginal => Key.Length == 0;
    public bool HasPicture => Picture != null;
    public bool ShowNoPicture => Picture == null && !IsOriginal;
    public required IReadOnlyList<string> Mods { get; init; }
    public required string SaveLine { get; init; }
    public required string SavesPath { get; init; }
    public required string Folder { get; init; }
    /// <summary>What stops it from starting (the game's check, done here too): PLAY waits until they are fixed.</summary>
    public IReadOnlyList<string> Problems { get; init; } = Array.Empty<string>();
    public bool HasProblems => Problems.Count > 0;
    public string ProblemsText => Problems.Count switch { 0 => "", 1 => " · 1 problem", var n => $" · {n} problems" };
}

/// <summary>A save to copy into a new project: the original game's ("") or a project's.</summary>
public sealed record SaveSource(string Label, string Key);

/// <summary>One of a shared project's mods in the Import dialog: it comes with the project, its own copy.</summary>
public sealed class ImportModRow
{
    public required ImportMod Item { get; init; }
    public string Name => Item.Mod.Name;
    public string Version => Item.Mod.Version.Length > 0 ? "v" + Item.Mod.Version : "";
    public string Line => Item.Mod.Contents;
}

/// <summary>The Play page's "What to play": the original game and the projects (projects\&lt;name&gt;), the one picked
/// (remembered in settings.toml's [play] project, so the game plays it when started on its own too), and New / Copy /
/// Rename / Delete / Open folder / a desktop shortcut.</summary>
public partial class ProjectsViewModel : ObservableObject
{
    private readonly GameFolder _folder;
    private readonly bool _readOnly;
    private readonly Func<bool> _gameRunning;
    private readonly Action<string> _say;

    public ObservableCollection<PlayChoice> Choices { get; } = new();

    public ProjectsViewModel(GameFolder folder, bool readOnly, Func<bool> gameRunning, Action<string> say)
    {
        _folder = folder;
        _readOnly = readOnly;
        _gameRunning = gameRunning;
        _say = say;
        Reload(GameSettings.LoadProject(folder.SettingsPath));
    }

    /// <summary>The list read again; <paramref name="pick"/> picked (the original game when it isn't there).</summary>
    public void Reload(string pick)
    {
        _reloading = true;
        Choices.Clear();
        Choices.Add(new PlayChoice
        {
            Key = "",
            Name = "Original game",
            Line = "As it came on the disc · no mods · " + (_folder.HasSave ? "saved" : "no save yet"),
            Mods = Array.Empty<string>(),
            SaveLine = _folder.HasSave ? "Saved · game\\mc0" : "No save yet",
            SavesPath = _folder.SavesPath,
            Folder = _folder.Data,
        });
        foreach (var p in Projects.List(_folder))
        {
            var installed = ProjectMods.List(_folder, p.Name);
            var problems = ProjectCheck.Problems(_folder, p.Name).Select(x => x.Text).ToList();
            var mods = p.Mods.Count switch { 0 => "no mods on", 1 => "1 mod", _ => $"{p.Mods.Count} mods" };
            Choices.Add(new PlayChoice
            {
                Key = p.Name,
                Name = p.Name,
                Line = mods + " · " + (p.HasSave ? "saved" : "no save yet"),
                Picture = LoadPicture(p.Picture),
                Mods = p.Mods.Select((m, i) => $"{i + 1}.  " + (installed.FirstOrDefault(x => x.Is(m))?.Name ?? m + "  (not installed)")).ToList(),
                SaveLine = p.HasSave ? $"Saved · projects\\{p.Name}\\saves" : "No save yet (an empty memory card)",
                SavesPath = p.SavesPath,
                Folder = p.Folder,
                Problems = problems,
            });
        }
        _reloading = false;
        Selected = Choices.FirstOrDefault(c => c.Key.Equals(pick, StringComparison.OrdinalIgnoreCase)) ?? Choices[0];
    }

    private bool _reloading;

    private static Bitmap? LoadPicture(string? path)
    {
        if (path == null)
            return null;
        try
        {
            using var stream = File.OpenRead(path);
            return Bitmap.DecodeToWidth(stream, 128);
        }
        catch
        {
            return null;
        }
    }

    [ObservableProperty] private PlayChoice? _selected;

    partial void OnSelectedChanged(PlayChoice? value)
    {
        if (_reloading || value == null)
            return;
        foreach (var name in new[]
                 {
                     nameof(SelectedName), nameof(Heading), nameof(IsOriginal), nameof(IsProject), nameof(SelectedMods),
                     nameof(HasMods), nameof(NoMods), nameof(SaveLine), nameof(HasProblems), nameof(ProblemCount),
                     nameof(ProblemsHeading), nameof(ProblemsLine),
                 })
            OnPropertyChanged(name);
        if (!_readOnly && GameSettings.LoadProject(_folder.SettingsPath) != value.Key)
        {
            try { GameSettings.SaveProject(_folder.SettingsPath, value.Key); }
            catch (Exception ex) { _say($"Couldn't save the choice: {ex.Message}"); }
        }
        PickedChanged?.Invoke();
    }

    /// <summary>The pick changed (the header's status names it).</summary>
    public event Action? PickedChanged;

    public string Key => Selected?.Key ?? "";
    public string SelectedName => Selected?.Name ?? "Original game";
    public string Heading => Selected is { IsOriginal: false } s ? "PROJECT · " + s.Name.ToUpperInvariant() : "THE ORIGINAL GAME";
    public bool IsOriginal => Selected?.IsOriginal ?? true;
    public bool IsProject => !IsOriginal;
    public IReadOnlyList<string> SelectedMods => Selected?.Mods ?? Array.Empty<string>();
    public bool HasMods => IsProject && SelectedMods.Count > 0;
    public bool NoMods => IsProject && SelectedMods.Count == 0;
    public string SaveLine => Selected?.SaveLine ?? "";
    public bool HasProblems => Selected?.HasProblems ?? false;
    public int ProblemCount => Selected?.Problems.Count ?? 0;
    public string ProblemsHeading => ProblemCount == 1 ? "1 problem to fix before playing" : $"{ProblemCount} problems to fix before playing";
    public string ProblemsLine => string.Join(" · ", Selected?.Problems ?? Array.Empty<string>());
    public string SavesPath => Selected?.SavesPath ?? _folder.SavesPath;

    // --- the buttons ---

    private bool Busy()
    {
        if (!_gameRunning())
            return false;
        _say("Close the game first.");
        return true;
    }

    [RelayCommand]
    private void OpenFolder()
    {
        if (Selected is { } s)
            GameFolder.Open(s.Folder);
    }

    [RelayCommand]
    private void MakeShortcut()
    {
        if (Selected is not { } s)
            return;
        _say(_folder.CreateProjectShortcut(s.Key) ?? $"A shortcut for {s.Name} is on the desktop.");
    }

    // --- the dialog: New project, Copy, Rename, Delete ---

    public enum DialogKind { None, New, Copy, Rename, Delete, Export, Import }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DialogOpen), nameof(IsNameDialog), nameof(IsNew), nameof(IsCopy), nameof(IsRename),
        nameof(IsDelete), nameof(IsExport), nameof(IsImport), nameof(ShowConfirm), nameof(ShowFolderHint),
        nameof(DialogWidth), nameof(DialogTitle), nameof(ConfirmText))]
    private DialogKind _dialog;

    public bool DialogOpen => Dialog != DialogKind.None;
    public bool IsNameDialog => Dialog is DialogKind.New or DialogKind.Copy or DialogKind.Rename or DialogKind.Import;
    public bool IsExport => Dialog == DialogKind.Export;
    public bool IsImport => Dialog == DialogKind.Import;
    public bool ShowConfirm => Dialog != DialogKind.None && Dialog != DialogKind.Delete;
    public bool ShowFolderHint => Dialog is DialogKind.New or DialogKind.Copy or DialogKind.Rename;
    public double DialogWidth => Dialog is DialogKind.Export or DialogKind.Import ? 600 : 520;
    public bool IsNew => Dialog == DialogKind.New;
    public bool IsCopy => Dialog == DialogKind.Copy;
    public bool IsRename => Dialog == DialogKind.Rename;
    public bool IsDelete => Dialog == DialogKind.Delete;

    public string DialogTitle => Dialog switch
    {
        DialogKind.New => "New project",
        DialogKind.Copy => $"Copy {SelectedName}",
        DialogKind.Rename => $"Rename {SelectedName}",
        DialogKind.Delete => $"Delete {SelectedName}?",
        DialogKind.Export => $"Export {SelectedName}",
        DialogKind.Import => _import is { } i ? $"Import {i.Name}?" : "Import",
        _ => "",
    };

    public string ConfirmText => Dialog switch
    {
        DialogKind.New => "Create",
        DialogKind.Copy => "Copy",
        DialogKind.Rename => "Rename",
        DialogKind.Delete => $"Delete {SelectedName}",
        DialogKind.Export => "Export",
        DialogKind.Import => "Import",
        _ => "",
    };

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(FolderHint))]
    private string _dialogName = "";

    partial void OnDialogNameChanged(string value)
    {
        DialogError = "";
        OnPropertyChanged(nameof(ImportNameHint));
        OnPropertyChanged(nameof(ImportNote));
    }

    public string FolderHint => $"Its folder: projects\\{DialogName.Trim()}";

    [ObservableProperty] private string _dialogError = "";

    // New project: the memory card, and a picture
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(EmptyCard))]
    private bool _copySave;

    public bool EmptyCard
    {
        get => !CopySave;
        set => CopySave = !value;
    }

    public ObservableCollection<SaveSource> SaveSources { get; } = new();
    [ObservableProperty] private SaveSource? _saveSource;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasNewPicture), nameof(NoNewPicture))]
    private string? _newPicturePath;

    [ObservableProperty] private Bitmap? _newPicture;

    public bool HasNewPicture => NewPicturePath != null;
    public bool NoNewPicture => NewPicturePath == null;

    public void SetNewPicture(string path)
    {
        if (!Projects.IsPicture(path))
        {
            DialogError = "A picture is a .png or .jpg file.";
            return;
        }
        NewPicture = LoadPicture(path);
        NewPicturePath = NewPicture != null ? path : null;
        DialogError = NewPicture != null ? "" : "That picture couldn't be read.";
    }

    [RelayCommand]
    private void RemovePicture()
    {
        NewPicturePath = null;
        NewPicture = null;
    }

    [RelayCommand]
    private void New()
    {
        if (Busy())
            return;
        SaveSources.Clear();
        foreach (var c in Choices)
            SaveSources.Add(new SaveSource(c.Name, c.Key));
        SaveSource = SaveSources.FirstOrDefault(s => s.Key == Key) ?? SaveSources.FirstOrDefault();
        CopySave = false;
        RemovePicture();
        DialogName = FreeName("New project");
        Dialog = DialogKind.New;
    }

    [RelayCommand]
    private void Copy()
    {
        if (IsOriginal || Busy())
            return;
        DialogName = FreeName(SelectedName + " copy");
        Dialog = DialogKind.Copy;
    }

    [RelayCommand]
    private void Rename()
    {
        if (IsOriginal || Busy())
            return;
        DialogName = SelectedName;
        Dialog = DialogKind.Rename;
    }

    [RelayCommand]
    private void Delete()
    {
        if (IsOriginal || Busy())
            return;
        Dialog = DialogKind.Delete;
    }

    [RelayCommand]
    private void Cancel()
    {
        Dialog = DialogKind.None;
        DialogError = "";
        DropImport();
    }

    [RelayCommand]
    private void Confirm()
    {
        if (_readOnly || Busy())
            return;
        var name = DialogName;
        string? error;
        string pick;
        switch (Dialog)
        {
            case DialogKind.New:
                error = Projects.Create(_folder, name, CopySave ? SaveSource?.Key ?? "" : null, NewPicturePath);
                pick = name;
                break;
            case DialogKind.Copy:
                error = Projects.Copy(_folder, Key, name);
                pick = name;
                break;
            case DialogKind.Rename:
                error = Projects.Rename(_folder, Key, name);
                pick = name;
                break;
            case DialogKind.Delete:
                error = Projects.Delete(_folder, Key);
                pick = "";
                break;
            case DialogKind.Export:
                ConfirmExport();
                return;
            case DialogKind.Import:
                error = _import == null ? "Nothing to import." : ProjectShare.Import(_folder, _import, name, ImportUseSaves && ImportHasSaves);
                pick = name;
                break;
            default:
                return;
        }
        if (error != null)
        {
            DialogError = error;
            return;
        }
        var done = Dialog switch
        {
            DialogKind.New => $"{name} is made.",
            DialogKind.Copy => $"{name} is made, a copy of {SelectedName}.",
            DialogKind.Rename => $"Renamed to {name}.",
            DialogKind.Import => ImportDone(name),
            _ => $"{SelectedName} is deleted.",
        };
        if (Dialog == DialogKind.Import)
        {
            DropImport();
            ModsChanged?.Invoke();
        }
        Dialog = DialogKind.None;
        Reload(pick);
        _say(done);
    }

    /// <summary>An import made a project with mods (the Mods page reads it again).</summary>
    public event Action? ModsChanged;

    // --- Export to share ---

    private ExportPlan? _export;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ExportPathExists))]
    private string _exportPath = "";

    partial void OnExportPathChanged(string value) => DialogError = "";

    [ObservableProperty] private bool _exportSaves;

    public string ExportModsTitle => _export is { } e
        ? (e.Mods.Count switch { 0 => "No mods", 1 => "Its mod", var n => $"Its {n} mods" }) : "";
    public string ExportModsSize => _export is { Mods.Count: > 0 } e ? " · " + ProjectShare.Size(e.ModBytes) : "";
    public string ExportModsList => _export is { } e
        ? (e.Mods.Count > 0 ? string.Join(" · ", e.Mods.Select(m => m.Version.Length > 0 ? $"{m.Name} v{m.Version}" : m.Name))
            : "It has no mods: whoever imports it plays the original game with its own save.") : "";
    public bool ExportCanSaves => _export?.Project.HasSave ?? false;
    public bool ExportNoSaves => !ExportCanSaves;
    public string ExportSavesHint => $"Your profile and what you’ve unlocked in {SelectedName}. Off: they start with an empty memory card.";
    public bool ExportHasMissing => _export is { Missing.Count: > 0 };
    public string ExportMissingText => _export is { Missing.Count: > 0 } e
        ? (e.Missing.Count == 1
            ? $"{e.Missing[0]} is on but isn’t installed in {SelectedName}, so it can’t go in."
            : $"{string.Join(", ", e.Missing)} are on but aren’t installed in {SelectedName}, so they can’t go in.")
        : "";
    public bool ExportPathExists => ExportPath.Length > 0 && File.Exists(ExportPath);

    [RelayCommand]
    private void Export()
    {
        if (IsOriginal || Selected == null)
            return;
        var project = Projects.List(_folder).FirstOrDefault(p => p.Name == Key);
        if (project == null)
            return;
        _export = ProjectShare.PlanExport(_folder, project);
        ExportSaves = false;
        ExportPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), ProjectShare.DefaultFileName(project.Name));
        foreach (var name in new[]
                 {
                     nameof(ExportModsTitle), nameof(ExportModsSize), nameof(ExportModsList), nameof(ExportCanSaves),
                     nameof(ExportNoSaves), nameof(ExportSavesHint), nameof(ExportHasMissing), nameof(ExportMissingText),
                     nameof(ExportPathExists),
                 })
            OnPropertyChanged(name);
        DialogError = "";
        Dialog = DialogKind.Export;
    }

    /// <summary>The file picked with Choose… (a .zip).</summary>
    public void SetExportPath(string path) =>
        ExportPath = path.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) ? path : path + ".zip";

    private void ConfirmExport()
    {
        if (_export == null)
            return;
        var path = ExportPath.Trim();
        if (path.Length == 0 || !Path.IsPathFullyQualified(path))
        {
            DialogError = "Choose where to save it.";
            return;
        }
        if (!path.EndsWith(".zip", StringComparison.OrdinalIgnoreCase))
            path += ".zip";
        if (Path.GetDirectoryName(path) is not { } dir || !Directory.Exists(dir))
        {
            DialogError = "That folder isn't there.";
            return;
        }
        if (ProjectShare.Export(_export, ExportSaves && ExportCanSaves, path) is { } error)
        {
            DialogError = error;
            return;
        }
        var name = SelectedName;
        Dialog = DialogKind.None;
        _export = null;
        _say($"{name} is exported: {path}");
    }

    // --- Import ---

    private ImportPlan? _import;
    public ObservableCollection<ImportModRow> ImportMods { get; } = new();

    [ObservableProperty] private bool _importUseSaves = true;

    public string ImportFrom => _import?.Source ?? "";
    public string ImportModsHeading => _import is { } i
        ? (i.Mods.Count switch { 0 => "NO MODS", 1 => "ITS MOD", var n => $"ITS {n} MODS" }) : "";
    public bool ImportNoMods => _import is { Mods.Count: 0 };
    public bool ImportHasSaves => _import?.HasSaves ?? false;
    public bool ImportNoSaves => !ImportHasSaves;
    public bool ImportHasMissing => _import is { Missing.Count: > 0 };
    public string ImportMissingText => _import is { Missing.Count: > 0 } i
        ? $"{string.Join(", ", i.Missing)} didn’t come with it. It can’t be played until {(i.Missing.Count == 1 ? "that’s" : "they’re")} installed."
        : "";

    public string ImportNameHint
    {
        get
        {
            if (_import == null)
                return "";
            if (Projects.CheckName(_folder, _import.Name) == null)
                return $"Its folder: projects\\{DialogName.Trim()}";
            return $"You have a project called {_import.Name} already, so this one gets a new name.";
        }
    }

    public string ImportNote => "";
    public bool ImportHasNote => false;

    /// <summary>A shared project's zip picked (or dropped): read, and the Import dialog shown.</summary>
    public void PickImport(string path)
    {
        if (Busy())
            return;
        DropImport();
        var (plan, error) = ProjectShare.OpenImport(_folder, path);
        if (plan == null)
        {
            _say(error ?? "That isn't a shared project.");
            return;
        }
        _import = plan;
        foreach (var m in plan.Mods)
            ImportMods.Add(new ImportModRow { Item = m });
        ImportUseSaves = plan.HasSaves;
        DialogName = FreeName(plan.Name);
        foreach (var name in new[]
                 {
                     nameof(ImportFrom), nameof(ImportModsHeading), nameof(ImportNoMods), nameof(ImportHasSaves),
                     nameof(ImportNoSaves), nameof(ImportHasMissing), nameof(ImportMissingText), nameof(ImportNameHint),
                     nameof(ImportNote), nameof(ImportHasNote), nameof(DialogTitle),
                 })
            OnPropertyChanged(name);
        DialogError = "";
        Dialog = DialogKind.Import;
    }

    private string ImportDone(string name)
    {
        var count = _import?.Mods.Count ?? 0;
        return $"{name} is imported" + (count switch { 0 => ".", 1 => " with its mod.", _ => $" with its {count} mods." });
    }

    private void DropImport()
    {
        ProjectShare.Discard(_import);
        _import = null;
        ImportMods.Clear();
    }


    private string FreeName(string start)
    {
        var name = start;
        for (int n = 2; Projects.CheckName(_folder, name) != null && n < 100; n++)
            name = $"{start} {n}";
        return name;
    }

    /// <summary>For pictures (--shot): a dialog open.</summary>
    public void ShowSample(string which)
    {
        if (which != "new" && which != "import" && Choices.Count > 1)
            Selected = Choices.FirstOrDefault(c => c.Key == Environment.GetEnvironmentVariable("VENDETTA_SHOT_PROJECT")) ?? Choices[1];
        switch (which)
        {
            case "export": Export(); break;
            // VENDETTA_SHOT_SHARE=<a shared project's zip>: shown being imported
            case "import":
                if (Environment.GetEnvironmentVariable("VENDETTA_SHOT_SHARE") is { Length: > 0 } zip)
                    PickImport(zip);
                break;
            case "new": New(); DialogName = "WK3"; break;
            case "copy": Copy(); break;
            case "rename": Rename(); break;
            case "delete": Delete(); break;
        }
    }
}
