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
}

/// <summary>A save to copy into a new project: the original game's ("") or a project's.</summary>
public sealed record SaveSource(string Label, string Key);

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
        var installed = Projects.InstalledMods(_folder);
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
            var mods = p.Mods.Count switch { 0 => "no mods ticked yet", 1 => "1 mod on", _ => $"{p.Mods.Count} mods on" };
            Choices.Add(new PlayChoice
            {
                Key = p.Name,
                Name = p.Name,
                Line = mods + " · " + (p.HasSave ? "saved" : "no save yet"),
                Picture = LoadPicture(p.Picture),
                Mods = p.Mods.Select((m, i) => $"{i + 1}.  {m}" + (installed.Contains(m.ToLowerInvariant()) ? "" : "  (not installed)")).ToList(),
                SaveLine = p.HasSave ? $"Saved · projects\\{p.Name}\\saves" : "No save yet (an empty memory card)",
                SavesPath = p.SavesPath,
                Folder = p.Folder,
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
                     nameof(HasMods), nameof(NoMods), nameof(SaveLine),
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

    public enum DialogKind { None, New, Copy, Rename, Delete }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DialogOpen), nameof(IsNameDialog), nameof(IsNew), nameof(IsCopy), nameof(IsRename),
        nameof(IsDelete), nameof(DialogTitle), nameof(ConfirmText))]
    private DialogKind _dialog;

    public bool DialogOpen => Dialog != DialogKind.None;
    public bool IsNameDialog => Dialog is DialogKind.New or DialogKind.Copy or DialogKind.Rename;
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
        _ => "",
    };

    public string ConfirmText => Dialog switch
    {
        DialogKind.New => "Create",
        DialogKind.Copy => "Copy",
        DialogKind.Rename => "Rename",
        DialogKind.Delete => $"Delete {SelectedName}",
        _ => "",
    };

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(FolderHint))]
    private string _dialogName = "";

    partial void OnDialogNameChanged(string value) => DialogError = "";

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
            _ => $"{SelectedName} is deleted.",
        };
        Dialog = DialogKind.None;
        Reload(pick);
        _say(done);
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
        if (which != "new" && Choices.Count > 1)
            Selected = Choices[1];
        switch (which)
        {
            case "new": New(); DialogName = "WK3"; break;
            case "copy": Copy(); break;
            case "rename": Rename(); break;
            case "delete": Delete(); break;
        }
    }
}
