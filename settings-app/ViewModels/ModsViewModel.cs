using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using VendettaSettings.Services;

namespace VendettaSettings.ViewModels;

/// <summary>One mod's line in the library.</summary>
public sealed class ModRow
{
    public required ModInfo Mod { get; init; }
    public required bool IsOn { get; init; }
    public required bool IsPicked { get; init; }
    public required IRelayCommand Toggle { get; init; }
    public required IRelayCommand Pick { get; init; }
    public string Name => Mod.Name;
    public string Changes => Mod.Changes;
    public bool HasNeeds => Mod.Needs.Count > 0;
    public string NeedsText => "needs " + string.Join(", ", Mod.Needs);
}

/// <summary>A category's group of lines, folding.</summary>
public sealed class ModGroup
{
    public required string Title { get; init; }
    public required string Counts { get; init; }
    public required bool IsOpen { get; init; }
    public required IReadOnlyList<ModRow> Rows { get; init; }
    public required IRelayCommand Fold { get; init; }
}

/// <summary>A category button over the list.</summary>
public sealed class CategoryChip
{
    public required string Label { get; init; }
    public required bool IsOn { get; init; }
    public required IRelayCommand Pick { get; init; }
}

/// <summary>One line of the load order.</summary>
public sealed class OrderRow
{
    public required string Number { get; init; }
    public required string Name { get; init; }
    public required bool Missing { get; init; }
    public required bool CanUp { get; init; }
    public required bool CanDown { get; init; }
    public required IRelayCommand Up { get; init; }
    public required IRelayCommand Down { get; init; }
    public required IRelayCommand Drop { get; init; }
}

/// <summary>The Mods page: the library (mods\, grouped by category, searchable), the ticks for one project (written to
/// its project.json at once), the load order, a mod's details, Install (a zip or a folder) and Remove from library.</summary>
public partial class ModsViewModel : ObservableObject
{
    private readonly GameFolder _folder;
    private readonly bool _readOnly;
    private readonly Action<string> _say;
    private readonly Action _projectsChanged;

    private List<ModInfo> _library = new();
    private List<string> _ticked = new();      // the project's list, as project.json names them
    private readonly HashSet<string> _closed = new(StringComparer.OrdinalIgnoreCase);

    public ObservableCollection<string> ProjectNames { get; } = new();
    public ObservableCollection<ModGroup> Groups { get; } = new();
    public ObservableCollection<CategoryChip> Chips { get; } = new();
    public ObservableCollection<OrderRow> Order { get; } = new();

    public ModsViewModel(GameFolder folder, bool readOnly, Action<string> say, Action projectsChanged)
    {
        _folder = folder;
        _readOnly = readOnly;
        _say = say;
        _projectsChanged = projectsChanged;
    }

    /// <summary>The page opened: the library and the projects read again; the ticks are for <paramref name="project"/> (the
    /// Play page's pick) when it is one, else the project already shown, else the first.</summary>
    public void Open(string project)
    {
        _library = ModLibrary.List(_folder);
        var names = Projects.List(_folder).Select(p => p.Name).ToList();
        _opening = true;
        ProjectNames.Clear();
        foreach (var n in names)
            ProjectNames.Add(n);
        ForProject = names.FirstOrDefault(n => n.Equals(project, StringComparison.OrdinalIgnoreCase))
                     ?? names.FirstOrDefault(n => n.Equals(ForProject, StringComparison.OrdinalIgnoreCase))
                     ?? names.FirstOrDefault();
        _opening = false;
        if (Picked == null || !_library.Any(m => m.FolderName == Picked))
            Picked = _library.FirstOrDefault()?.FolderName;
        LoadTicks();
    }

    private bool _opening;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasProject), nameof(NoProject), nameof(OrderHeading), nameof(NothingTickedText))]
    private string? _forProject;

    partial void OnForProjectChanged(string? value)
    {
        if (!_opening)
            LoadTicks();
    }

    public bool HasProject => ForProject != null;
    public bool NoProject => ForProject == null;
    public string OrderHeading => ForProject == null ? "LOAD ORDER" : "LOAD ORDER · " + ForProject.ToUpperInvariant();
    public string NothingTickedText => $"Nothing ticked: {ForProject} plays like the original game, with its own save.";

    private void LoadTicks()
    {
        _ticked = ForProject == null ? new List<string>()
            : Projects.List(_folder).FirstOrDefault(p => p.Name == ForProject)?.Mods.ToList() ?? new List<string>();
        Note = "";
        _pending = null;
        Rebuild();
    }

    private ModInfo? Find(string name) => _library.FirstOrDefault(m => m.Is(name));
    private bool IsOn(ModInfo mod) => _ticked.Any(mod.Is);

    private void SaveTicks()
    {
        if (_readOnly || ForProject == null)
            return;
        try
        {
            Projects.SetMods(_folder, ForProject, _ticked);
            _projectsChanged();
        }
        catch (Exception ex)
        {
            _say($"Couldn't save {ForProject}'s mods: {ex.Message}");
        }
    }

    // --- the list's filters ---

    [ObservableProperty] private string _search = "";
    [ObservableProperty] private string _category = "All";
    [ObservableProperty] private bool _tickedOnly;
    [ObservableProperty] private string? _picked;

    partial void OnSearchChanged(string value) => Rebuild();
    partial void OnTickedOnlyChanged(bool value) => Rebuild();
    partial void OnCategoryChanged(string value) => Rebuild();
    partial void OnPickedChanged(string? value) => Rebuild();

    public string LibraryHeading => $"YOUR LIBRARY · {_library.Count} MOD{(_library.Count == 1 ? "" : "S")}" +
                                    (ForProject != null ? $" · {_library.Count(IsOn)} TICKED" : "");
    public bool LibraryEmpty => _library.Count == 0;
    public bool NothingMatches => _library.Count > 0 && Groups.Count == 0;
    public bool NothingTicked => ForProject != null && _ticked.Count == 0;

    private IEnumerable<string> CategoriesInOrder() =>
        _library.Select(m => m.Category).Distinct(StringComparer.OrdinalIgnoreCase)
            .OrderBy(c => Array.FindIndex(ModLibrary.KnownCategories, k => k.Equals(c, StringComparison.OrdinalIgnoreCase)) is var i && i >= 0 ? i : 99)
            .ThenBy(c => c, StringComparer.OrdinalIgnoreCase);

    private void Rebuild()
    {
        // the search ignores case, spaces and punctuation ("readd" finds "Re-added")
        static string Letters(string text) => new(text.Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray());
        var q = Letters(Search);
        bool Shown(ModInfo m) =>
            (Category == "All" || m.Category.Equals(Category, StringComparison.OrdinalIgnoreCase)) &&
            (!TickedOnly || IsOn(m)) &&
            (q.Length == 0 || Letters($"{m.Name} {m.FolderName} {m.Changes} {m.Author} {m.Description}").Contains(q));

        Chips.Clear();
        var categories = CategoriesInOrder().ToList();
        if (!categories.Contains(Category, StringComparer.OrdinalIgnoreCase) && Category != "All")
            Category = "All";
        Chips.Add(new CategoryChip { Label = "All", IsOn = Category == "All", Pick = new RelayCommand(() => Category = "All") });
        foreach (var c in categories)
            Chips.Add(new CategoryChip
            {
                Label = $"{c} {_library.Count(m => m.Category.Equals(c, StringComparison.OrdinalIgnoreCase))}",
                IsOn = Category.Equals(c, StringComparison.OrdinalIgnoreCase),
                Pick = new RelayCommand(() => Category = c),
            });

        Groups.Clear();
        foreach (var c in categories)
        {
            var all = _library.Where(m => m.Category.Equals(c, StringComparison.OrdinalIgnoreCase)).ToList();
            var shown = all.Where(Shown).ToList();
            if (shown.Count == 0)
                continue;
            var open = !_closed.Contains(c);
            Groups.Add(new ModGroup
            {
                Title = c.ToUpperInvariant(),
                Counts = ForProject != null ? $"· {all.Count} · {all.Count(IsOn)} TICKED" : $"· {all.Count}",
                IsOpen = open,
                Rows = shown.Select(m => new ModRow
                {
                    Mod = m,
                    IsOn = IsOn(m),
                    IsPicked = m.FolderName == Picked,
                    Toggle = new RelayCommand(() => Toggle(m)),
                    Pick = new RelayCommand(() => Picked = m.FolderName),
                }).ToList(),
                Fold = new RelayCommand(() =>
                {
                    if (!_closed.Remove(c))
                        _closed.Add(c);
                    Rebuild();
                }),
            });
        }

        Order.Clear();
        for (int i = 0; i < _ticked.Count; i++)
        {
            var at = i;
            var mod = Find(_ticked[i]);
            Order.Add(new OrderRow
            {
                Number = (i + 1).ToString(),
                Name = mod?.Name ?? _ticked[i] + "  (not installed)",
                Missing = mod == null,
                CanUp = i > 0,
                CanDown = i < _ticked.Count - 1,
                Up = new RelayCommand(() => Move(at, -1)),
                Down = new RelayCommand(() => Move(at, 1)),
                Drop = new RelayCommand(() =>
                {
                    _ticked.RemoveAt(at);
                    SaveTicks();
                    Rebuild();
                }),
            });
        }

        foreach (var name in new[]
                 {
                     nameof(LibraryHeading), nameof(LibraryEmpty), nameof(NothingMatches), nameof(NothingTicked),
                     nameof(HasPicked), nameof(PickedHeading), nameof(PickedDescription), nameof(PickedCategory),
                     nameof(PickedAuthor), nameof(PickedChanges), nameof(PickedNeeds), nameof(PickedUsedBy),
                 })
            OnPropertyChanged(name);
    }

    private void Move(int i, int by)
    {
        var j = i + by;
        if (j < 0 || j >= _ticked.Count)
            return;
        (_ticked[i], _ticked[j]) = (_ticked[j], _ticked[i]);
        SaveTicks();
        Rebuild();
    }

    // --- ticking: a mod's needs come with it; unticking a needed one asks first ---

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasNote))]
    private string _note = "";

    [ObservableProperty] private bool _noteIsWarning;
    [ObservableProperty] private bool _askUntick;
    private ModInfo? _pending;

    public bool HasNote => Note.Length > 0;

    private void Toggle(ModInfo mod)
    {
        if (ForProject == null)
        {
            Note = "Make a project on the Play page first: mods are ticked for a project.";
            NoteIsWarning = true;
            AskUntick = false;
            Rebuild();
            return;
        }
        if (IsOn(mod))
        {
            var users = _library.Where(m => m != mod && IsOn(m) && m.Needs.Any(mod.Is)).Select(m => m.Name).ToList();
            if (users.Count > 0)
            {
                _pending = mod;
                Note = $"{string.Join(", ", users)} need{(users.Count == 1 ? "s" : "")} {mod.Name}. Untick {(users.Count == 1 ? "it" : "them")} too?";
                NoteIsWarning = true;
                AskUntick = true;
                Rebuild();
                return;
            }
            _ticked.RemoveAll(mod.Is);
            Note = "";
        }
        else
        {
            // what it needs, and what those need, first
            var added = new List<string>();
            void Add(ModInfo m, int depth)
            {
                foreach (var need in m.Needs)
                    if (Find(need) is { } n && !IsOn(n) && depth < 10)
                    {
                        Add(n, depth + 1);
                        if (!IsOn(n))
                        {
                            _ticked.Add(n.FolderName);
                            added.Add(n.Name);
                        }
                    }
            }
            Add(mod, 0);
            _ticked.Add(mod.FolderName);
            var missing = mod.Needs.Where(n => Find(n) == null).ToList();
            Note = missing.Count > 0
                ? $"{mod.Name} needs {string.Join(", ", missing)}, which isn't installed: the game skips it until it is."
                : added.Count > 0 ? $"Ticked {string.Join(", ", added)} too: {mod.Name} needs {(added.Count == 1 ? "it" : "them")}." : "";
            NoteIsWarning = missing.Count > 0;
        }
        AskUntick = false;
        _pending = null;
        SaveTicks();
        Rebuild();
    }

    [RelayCommand]
    private void UntickBoth()
    {
        if (_pending is not { } mod)
            return;
        var drop = new HashSet<ModInfo> { mod };
        // everything ticked that needs it, and what needs those
        for (bool more = true; more;)
        {
            more = false;
            foreach (var m in _library.Where(m => IsOn(m) && !drop.Contains(m) && m.Needs.Any(n => drop.Any(d => d.Is(n)))))
                more |= drop.Add(m);
        }
        _ticked.RemoveAll(t => drop.Any(d => d.Is(t)));
        Note = "";
        AskUntick = false;
        _pending = null;
        SaveTicks();
        Rebuild();
    }

    [RelayCommand]
    private void KeepTicked()
    {
        Note = "";
        AskUntick = false;
        _pending = null;
        Rebuild();
    }

    // --- the picked mod ---

    private ModInfo? PickedMod => Picked == null ? null : _library.FirstOrDefault(m => m.FolderName == Picked);
    public bool HasPicked => PickedMod != null;
    public string PickedHeading => PickedMod is { } m ? (m.Name + (m.Version.Length > 0 ? " · v" + m.Version : "")).ToUpperInvariant() : "";
    public string PickedDescription => PickedMod is { } m ? (m.Description.Length > 0 ? m.Description : "No description.") : "";
    public string PickedCategory => PickedMod?.Category ?? "";
    public string PickedAuthor => PickedMod is { } m ? (m.Author.Length > 0 ? m.Author : "not given") : "";
    public string PickedChanges => PickedMod?.Changes ?? "";
    public string PickedNeeds => PickedMod is { } m ? (m.Needs.Count > 0 ? string.Join(", ", m.Needs.Select(n => Find(n) == null ? n + " (not installed)" : n)) : "nothing") : "";
    public string PickedUsedBy => PickedMod is { } m ? (ModLibrary.UsedBy(_folder, m) is { Count: > 0 } u ? string.Join(", ", u) : "no project") : "";

    [RelayCommand]
    private void OpenModFolder()
    {
        if (PickedMod is { } m)
            GameFolder.Open(m.Folder);
    }

    [RelayCommand]
    private void OpenLibrary() => GameFolder.Open(ModLibrary.RootOf(_folder));

    // --- the dialog: Install, Remove ---

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DialogOpen), nameof(IsInstall), nameof(IsRemove))]
    private int _dialog; // 0 none, 1 install, 2 remove

    public bool DialogOpen => Dialog != 0;
    public bool IsInstall => Dialog == 1;
    public bool IsRemove => Dialog == 2;

    [ObservableProperty] private string _dialogError = "";
    private ModPackage? _package;

    public string InstallTitle => _package is { } p ? $"Install {p.Mod.Name}?" : "";
    public string InstallFrom => _package?.Source ?? "";
    public string InstallVersion => _package is { } p
        ? (p.Mod.Version.Length > 0 ? p.Mod.Version : "not given") + (p.Mod.Author.Length > 0 ? " · by " + p.Mod.Author : "") : "";
    public string InstallDescription => _package is { } p ? (p.Mod.Description.Length > 0 ? p.Mod.Description : "No description.") : "";
    public string InstallCategory => _package?.Mod.Category ?? "";
    public string InstallChanges => _package?.Mod.Changes ?? "";
    public string InstallNeeds => _package is { } p
        ? (p.Needs.Count > 0 ? string.Join(", ", p.Needs.Select(n => n.Name + (n.Installed ? " (installed)" : " (not installed)"))) : "nothing") : "";
    public bool InstallNeedsMissing => _package?.Needs.Any(n => !n.Installed) ?? false;
    public bool InstallReplaces => _package?.Replaces != null;
    public string InstallReplacesText
    {
        get
        {
            if (_package?.Replaces is not { } old)
                return "";
            var users = ModLibrary.UsedBy(_folder, old);
            var version = old.Version.Length > 0 ? $"version {old.Version}" : "a copy";
            return $"You have {version} of {old.Name}. Installing replaces it" +
                   (users.Count > 0 ? $", in every project that uses it ({string.Join(", ", users)})." : ".");
        }
    }
    // (not offered when the copy it replaces is ticked already: the new one stays ticked)
    public bool CanTickToo => ForProject != null && !(_package?.Replaces is { } old && IsOn(old));
    public string TickTooText => $"Tick it for {ForProject} too";
    [ObservableProperty] private bool _tickToo = true;

    /// <summary>A zip or a folder picked (or dropped): read, and the Install dialog shown.</summary>
    public void Pick(string path)
    {
        var (package, error) = ModLibrary.Open(_folder, path);
        _package = package;
        DialogError = error ?? "";
        if (package == null)
        {
            _say(error ?? "That isn't a mod.");
            return;
        }
        TickToo = ForProject != null && (package.Replaces == null || !IsOn(package.Replaces));
        foreach (var name in new[]
                 {
                     nameof(InstallTitle), nameof(InstallFrom), nameof(InstallVersion), nameof(InstallDescription),
                     nameof(InstallCategory), nameof(InstallChanges), nameof(InstallNeeds), nameof(InstallNeedsMissing),
                     nameof(InstallReplaces), nameof(InstallReplacesText), nameof(CanTickToo), nameof(TickTooText),
                 })
            OnPropertyChanged(name);
        Dialog = 1;
    }

    public string RemoveTitle => PickedMod is { } m ? $"Remove {m.Name}?" : "";
    public IReadOnlyList<string> RemoveUsers => PickedMod is { } m ? ModLibrary.UsedBy(_folder, m) : Array.Empty<string>();
    public bool RemoveHasUsers => RemoveUsers.Count > 0;
    public string RemoveText => RemoveUsers.Count switch
    {
        0 => "It is taken out of your mods library. No project ticks it.",
        1 => "It is taken out of your mods library, and out of the project that ticks it:",
        var n => $"It is taken out of your mods library, and out of the {n} projects that tick it:",
    };

    public string RemoveNote => PickedMod is { } m && m.Changes.Contains("moves")
        ? "Saves stay. A wrestler who had one of its moves in a slot gets the game's own move there again."
        : "Saves stay.";

    [RelayCommand]
    private void AskRemove()
    {
        if (PickedMod == null)
            return;
        DialogError = "";
        OnPropertyChanged(nameof(RemoveTitle));
        OnPropertyChanged(nameof(RemoveUsers));
        OnPropertyChanged(nameof(RemoveHasUsers));
        OnPropertyChanged(nameof(RemoveText));
        OnPropertyChanged(nameof(RemoveNote));
        Dialog = 2;
    }

    [RelayCommand]
    private void CancelDialog()
    {
        Dialog = 0;
        DialogError = "";
    }

    [RelayCommand]
    private void ConfirmDialog()
    {
        if (_readOnly)
            return;
        if (Dialog == 1 && _package is { } p)
        {
            var (folderName, error) = ModLibrary.Install(_folder, p);
            if (error != null)
            {
                DialogError = error;
                return;
            }
            Dialog = 0;
            _library = ModLibrary.List(_folder);
            Picked = folderName;
            if (TickToo && ForProject != null && _library.FirstOrDefault(m => m.FolderName == folderName) is { } mod && !IsOn(mod))
                Toggle(mod);
            else
                Rebuild();
            _projectsChanged();
            _say(p.Replaces != null ? $"{p.Mod.Name} is updated." : $"{p.Mod.Name} is installed.");
        }
        else if (Dialog == 2 && PickedMod is { } mod)
        {
            if (ModLibrary.Remove(_folder, mod) is { } error)
            {
                DialogError = error;
                return;
            }
            Dialog = 0;
            var name = mod.Name;
            Open(ForProject ?? "");
            Picked = _library.FirstOrDefault()?.FolderName;
            _projectsChanged();
            _say($"{name} is removed.");
        }
    }

    /// <summary>For pictures (--shot): the page, or a dialog open.</summary>
    public void ShowSample(string which, string project)
    {
        Open(project);
        switch (which)
        {
            case "install":
                // VENDETTA_SHOT_MOD=<zip or folder>: the mod shown being installed
                if (Environment.GetEnvironmentVariable("VENDETTA_SHOT_MOD") is { Length: > 0 } path)
                    Pick(path);
                break;
            case "remove":
                AskRemove();
                break;
        }
    }
}
