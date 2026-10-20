using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using VendettaSettings.Services;

namespace VendettaSettings.ViewModels;

/// <summary>One mod's line in the project's list.</summary>
public sealed class ModRow
{
    public required ModInfo Mod { get; init; }
    public required bool IsOn { get; init; }
    public required bool IsPicked { get; init; }
    public required bool HasProblem { get; init; }
    public required IRelayCommand Toggle { get; init; }
    public required IRelayCommand Pick { get; init; }
    public string Name => Mod.Name;
    public string Contents => Mod.Contents;
    public string PlacesText => Mod.PlacesSummary;
    public bool HasPlaces => PlacesText.Length > 0;
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

/// <summary>A button that fixes a problem (Install a mod… opens the file picker).</summary>
public sealed class ProblemFix
{
    public required string Label { get; init; }
    public IRelayCommand? Run { get; init; }
    public bool IsInstall { get; init; }
    public bool IsCommand => !IsInstall;
}

/// <summary>One problem on the Mods page, with its fixes.</summary>
public sealed class ProblemRow
{
    public required string Text { get; init; }
    public required IReadOnlyList<ProblemFix> Fixes { get; init; }
}

/// <summary>One wrestler and where it goes, in the picked mod's details.</summary>
public sealed record PlaceLine(string Name, string Place);

/// <summary>One choice in a "where it goes" list: a new wrestler ("") or a game wrestler's place; a place another
/// wrestler already fills shows who and can't be picked.</summary>
public partial class PlaceOption : ObservableObject
{
    public required string Value { get; init; }
    public required string Name { get; init; }
    [ObservableProperty] private string _label = "";
    [ObservableProperty] private bool _isEnabled = true;
}

/// <summary>One wrestler in the "where they go" list.</summary>
public partial class PlaceRow : ObservableObject
{
    public required string Folder { get; init; }
    public required string Name { get; init; }
    /// <summary>The attires it brings (a replacement takes those of the game wrestler's).</summary>
    public required IReadOnlyList<int> Attires { get; init; }
    public required IReadOnlyList<PlaceOption> Options { get; init; }
    public required Action Changed { get; init; }
    [ObservableProperty] private PlaceOption? _selected;
    partial void OnSelectedChanged(PlaceOption? value) => Changed();
    public string Value => Selected?.Value ?? "";
}

/// <summary>The Mods page for one project: the mods installed in it (projects\&lt;name&gt;\mods\, grouped by category,
/// searchable), each on or off (written to its project.json at once), the load order, a mod's details and where its
/// wrestlers go, the problems that stop the project from starting (with their fixes), Install (into this project) and
/// Remove from this project.</summary>
public partial class ModsViewModel : ObservableObject
{
    private readonly GameFolder _folder;
    private readonly bool _readOnly;
    private readonly Action<string> _say;
    private readonly Action _projectsChanged;

    private List<ModInfo> _mods = new();
    private List<string> _on = new();          // the project's list, as project.json names them
    private List<ModProblem> _problems = new();
    private readonly HashSet<string> _closed = new(StringComparer.OrdinalIgnoreCase);

    public ObservableCollection<string> ProjectNames { get; } = new();
    public ObservableCollection<ModGroup> Groups { get; } = new();
    public ObservableCollection<CategoryChip> Chips { get; } = new();
    public ObservableCollection<OrderRow> Order { get; } = new();
    public ObservableCollection<ProblemRow> Problems { get; } = new();
    public ObservableCollection<PlaceLine> PickedPlaces { get; } = new();
    public ObservableCollection<PlaceRow> PlaceRows { get; } = new();
    public ObservableCollection<string> InstallNeedLines { get; } = new();
    public ObservableCollection<string> InstallBreaks { get; } = new();

    public ModsViewModel(GameFolder folder, bool readOnly, Action<string> say, Action projectsChanged)
    {
        _folder = folder;
        _readOnly = readOnly;
        _say = say;
        _projectsChanged = projectsChanged;
    }

    /// <summary>The page opened: the projects read again; the mods of <paramref name="project"/> (the Play page's pick) when
    /// it is one, else the project already shown, else the first.</summary>
    public void Open(string project)
    {
        var names = Projects.List(_folder).Select(p => p.Name).ToList();
        _opening = true;
        ProjectNames.Clear();
        foreach (var n in names)
            ProjectNames.Add(n);
        ForProject = names.FirstOrDefault(n => n.Equals(project, StringComparison.OrdinalIgnoreCase))
                     ?? names.FirstOrDefault(n => n.Equals(ForProject, StringComparison.OrdinalIgnoreCase))
                     ?? names.FirstOrDefault();
        _opening = false;
        LoadProject();
    }

    private bool _opening;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasProject), nameof(NoProject), nameof(OrderHeading), nameof(NothingOnText))]
    private string? _forProject;

    partial void OnForProjectChanged(string? value)
    {
        if (!_opening)
            LoadProject();
    }

    public bool HasProject => ForProject != null;
    public bool NoProject => ForProject == null;
    public string OrderHeading => ForProject == null ? "LOAD ORDER" : "LOAD ORDER · " + ForProject.ToUpperInvariant();
    public string NothingOnText => "Nothing on: plays like the original game.";

    private void LoadProject()
    {
        _mods = ForProject == null ? new List<ModInfo>() : ProjectMods.List(_folder, ForProject);
        _on = ForProject == null ? new List<string>()
            : Projects.List(_folder).FirstOrDefault(p => p.Name == ForProject)?.Mods.ToList() ?? new List<string>();
        if (Picked == null || !_mods.Any(m => m.FolderName == Picked))
            Picked = _mods.FirstOrDefault()?.FolderName;
        Note = "";
        _pending = null;
        CheckProblems();
        Rebuild();
    }

    private ModInfo? Find(string name) => _mods.FirstOrDefault(m => m.Is(name));
    private bool IsOn(ModInfo mod) => _on.Any(mod.Is);

    private void SaveOn()
    {
        if (_readOnly || ForProject == null)
            return;
        try
        {
            Projects.SetMods(_folder, ForProject, _on);
            _projectsChanged();
        }
        catch (Exception ex)
        {
            _say($"Couldn't save {ForProject}'s mods: {ex.Message}");
        }
        CheckProblems();
    }

    // --- the problems, with their fixes ---

    public bool HasProblems => Problems.Count > 0;
    public string ProblemsHeading => Problems.Count == 1 ? "1 problem to fix before playing" : $"{Problems.Count} problems to fix before playing";

    private void CheckProblems()
    {
        _problems = ForProject == null ? new List<ModProblem>() : ProjectCheck.Problems(_folder, ForProject);
        Problems.Clear();
        foreach (var p in _problems)
        {
            var fixes = new List<ProblemFix>();
            var mod = Find(p.Mod);
            var modName = mod?.Name ?? p.Mod;
            ProblemFix TurnOff(ModInfo? m, string label) => new()
            {
                Label = label,
                Run = new RelayCommand(() =>
                {
                    _on.RemoveAll(n => n.Equals(p.Mod, StringComparison.OrdinalIgnoreCase) || (m?.Is(n) ?? false));
                    SaveOn();
                    Rebuild();
                }),
            };
            switch (p.Kind)
            {
                case ProblemKind.NotInstalled:
                    fixes.Add(TurnOff(null, "Take it off the list"));
                    break;
                case ProblemKind.Unreadable:
                    fixes.Add(TurnOff(null, "Turn it off"));
                    break;
                case ProblemKind.NeedMissing:
                    if (Find(p.Other) is { } need)
                        fixes.Add(new ProblemFix { Label = $"Turn {need.Name} on", Run = new RelayCommand(() => Toggle(need)) });
                    else
                        fixes.Add(new ProblemFix { Label = "Install a mod…", IsInstall = true });
                    fixes.Add(TurnOff(mod, $"Turn {modName} off"));
                    break;
                case ProblemKind.Clash:
                case ProblemKind.NoSuchWrestler when mod != null && mod.Wrestlers.Any(w => !w.IsNew && w.Folder == p.Other):
                    if (mod != null)
                        fixes.Add(new ProblemFix { Label = $"Choose where {modName} goes", Run = new RelayCommand(() => OpenWhere(mod)) });
                    fixes.Add(TurnOff(mod, $"Turn {modName} off"));
                    break;
                default:
                    fixes.Add(TurnOff(mod, $"Turn {modName} off"));
                    break;
            }
            Problems.Add(new ProblemRow { Text = p.Text, Fixes = fixes });
        }
        OnPropertyChanged(nameof(HasProblems));
        OnPropertyChanged(nameof(ProblemsHeading));
    }

    // --- the list's filters ---

    [ObservableProperty] private string _search = "";
    [ObservableProperty] private string _category = "All";
    [ObservableProperty] private string? _picked;

    partial void OnSearchChanged(string value) => Rebuild();
    partial void OnCategoryChanged(string value) => Rebuild();
    partial void OnPickedChanged(string? value) => Rebuild();

    public string ListHeading => ForProject == null ? "" :
        $"IN {ForProject.ToUpperInvariant()} · {_mods.Count} MOD{(_mods.Count == 1 ? "" : "S")} · {_mods.Count(IsOn)} ON";
    public bool ListEmpty => ForProject != null && _mods.Count == 0;
    public bool NothingMatches => _mods.Count > 0 && Groups.Count == 0;
    public bool NothingOn => ForProject != null && _on.Count == 0;

    private IEnumerable<string> CategoriesInOrder() =>
        _mods.Select(m => m.Category).Distinct(StringComparer.OrdinalIgnoreCase)
            .OrderBy(c => Array.FindIndex(ProjectMods.KnownCategories, k => k.Equals(c, StringComparison.OrdinalIgnoreCase)) is var i && i >= 0 ? i : 99)
            .ThenBy(c => c, StringComparer.OrdinalIgnoreCase);

    private void Rebuild()
    {
        // the search ignores case, spaces and punctuation ("readd" finds "Re-added")
        static string Letters(string text) => new(text.Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray());
        var q = Letters(Search);
        bool Shown(ModInfo m) =>
            (Category == "All" || m.Category.Equals(Category, StringComparison.OrdinalIgnoreCase)) &&
            (q.Length == 0 || Letters($"{m.Name} {m.FolderName} {m.Contents}").Contains(q));

        Chips.Clear();
        var categories = CategoriesInOrder().ToList();
        if (!categories.Contains(Category, StringComparer.OrdinalIgnoreCase) && Category != "All")
            Category = "All";
        Chips.Add(new CategoryChip { Label = "All", IsOn = Category == "All", Pick = new RelayCommand(() => Category = "All") });
        foreach (var c in categories)
            Chips.Add(new CategoryChip
            {
                Label = $"{c} {_mods.Count(m => m.Category.Equals(c, StringComparison.OrdinalIgnoreCase))}",
                IsOn = Category.Equals(c, StringComparison.OrdinalIgnoreCase),
                Pick = new RelayCommand(() => Category = c),
            });

        var troubled = new HashSet<string>(_problems.Select(p => p.Mod), StringComparer.OrdinalIgnoreCase);
        Groups.Clear();
        foreach (var c in categories)
        {
            var all = _mods.Where(m => m.Category.Equals(c, StringComparison.OrdinalIgnoreCase)).ToList();
            var shown = all.Where(Shown).ToList();
            if (shown.Count == 0)
                continue;
            var open = !_closed.Contains(c);
            Groups.Add(new ModGroup
            {
                Title = c.ToUpperInvariant(),
                Counts = $"· {all.Count} · {all.Count(IsOn)} ON",
                IsOpen = open,
                Rows = shown.Select(m => new ModRow
                {
                    Mod = m,
                    IsOn = IsOn(m),
                    IsPicked = m.FolderName == Picked,
                    HasProblem = troubled.Contains(m.FolderName),
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
        for (int i = 0; i < _on.Count; i++)
        {
            var at = i;
            var mod = Find(_on[i]);
            Order.Add(new OrderRow
            {
                Number = (i + 1).ToString(),
                Name = mod?.Name ?? _on[i] + "  (not installed)",
                Missing = mod == null,
                CanUp = i > 0,
                CanDown = i < _on.Count - 1,
                Up = new RelayCommand(() => Move(at, -1)),
                Down = new RelayCommand(() => Move(at, 1)),
                Drop = new RelayCommand(() =>
                {
                    _on.RemoveAt(at);
                    SaveOn();
                    Rebuild();
                }),
            });
        }

        PickedPlaces.Clear();
        if (PickedMod is { } picked)
            foreach (var w in picked.Wrestlers)
                PickedPlaces.Add(new PlaceLine(w.Name, w.PlaceText));

        foreach (var name in new[]
                 {
                     nameof(ListHeading), nameof(ListEmpty), nameof(NothingMatches), nameof(NothingOn),
                     nameof(HasPicked), nameof(PickedHeading), nameof(PickedContents), nameof(PickedNeeds),
                     nameof(PickedHasWrestlers), nameof(PickedPlacesHeading),
                 })
            OnPropertyChanged(name);
    }

    private void Move(int i, int by)
    {
        var j = i + by;
        if (j < 0 || j >= _on.Count)
            return;
        (_on[i], _on[j]) = (_on[j], _on[i]);
        SaveOn();
        Rebuild();
    }

    // --- on / off: a mod's needs come with it; turning off a needed one asks first ---

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasNote))]
    private string _note = "";

    [ObservableProperty] private bool _noteIsWarning;
    [ObservableProperty] private bool _askOff;
    private ModInfo? _pending;

    public bool HasNote => Note.Length > 0;

    private void Toggle(ModInfo mod)
    {
        if (ForProject == null)
            return;
        if (IsOn(mod))
        {
            var users = _mods.Where(m => m != mod && IsOn(m) && m.Needs.Any(mod.Is)).Select(m => m.Name).ToList();
            if (users.Count > 0)
            {
                _pending = mod;
                Note = $"{string.Join(", ", users)} need{(users.Count == 1 ? "s" : "")} {mod.Name}. Turn {(users.Count == 1 ? "it" : "them")} off too?";
                NoteIsWarning = true;
                AskOff = true;
                Rebuild();
                return;
            }
            _on.RemoveAll(mod.Is);
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
                            _on.Add(n.FolderName);
                            added.Add(n.Name);
                        }
                    }
            }
            Add(mod, 0);
            _on.Add(mod.FolderName);
            Note = added.Count > 0 ? $"Turned {string.Join(", ", added)} on too: {mod.Name} needs {(added.Count == 1 ? "it" : "them")}." : "";
            NoteIsWarning = false;
        }
        AskOff = false;
        _pending = null;
        SaveOn();
        Rebuild();
    }

    [RelayCommand]
    private void OffBoth()
    {
        if (_pending is not { } mod)
            return;
        var drop = new HashSet<ModInfo> { mod };
        // everything on that needs it, and what needs those
        for (bool more = true; more;)
        {
            more = false;
            foreach (var m in _mods.Where(m => IsOn(m) && !drop.Contains(m) && m.Needs.Any(n => drop.Any(d => d.Is(n)))))
                more |= drop.Add(m);
        }
        _on.RemoveAll(t => drop.Any(d => d.Is(t)));
        Note = "";
        AskOff = false;
        _pending = null;
        SaveOn();
        Rebuild();
    }

    [RelayCommand]
    private void KeepOn()
    {
        Note = "";
        AskOff = false;
        _pending = null;
        Rebuild();
    }

    // --- the picked mod ---

    private ModInfo? PickedMod => Picked == null ? null : _mods.FirstOrDefault(m => m.FolderName == Picked);
    public bool HasPicked => PickedMod != null;
    public string PickedHeading => PickedMod is { } m ? (m.Name + (m.Version.Length > 0 ? " · v" + m.Version : "")).ToUpperInvariant() : "";
    public string PickedContents => PickedMod?.Contents ?? "";
    public string PickedNeeds => PickedMod is { } m
        ? (m.Needs.Count > 0 ? string.Join(", ", m.Needs.Select(n => Find(n) is { } x ? x.Name + (IsOn(x) ? "" : " (off)") : n + " (not installed)")) : "nothing")
        : "";
    public bool PickedHasWrestlers => PickedMod is { Wrestlers.Count: > 0 };
    public string PickedPlacesHeading => PickedMod is { Wrestlers.Count: > 0 } m ? "WHERE THEY GO · " + m.PlacesSummary.ToUpperInvariant() : "";

    [RelayCommand]
    private void OpenModFolder()
    {
        if (PickedMod is { } m)
            GameFolder.Open(m.Folder);
    }

    [RelayCommand]
    private void OpenProjectMods()
    {
        if (ForProject == null)
            return;
        var dir = ProjectMods.RootOf(_folder, ForProject);
        Directory.CreateDirectory(dir);
        GameFolder.Open(dir);
    }

    [RelayCommand]
    private void ChangePlaces()
    {
        if (PickedMod is { } m)
            OpenWhere(m);
    }

    // --- the dialogs: Install, Remove, Where they go ---

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DialogOpen), nameof(IsInstall), nameof(IsRemove), nameof(IsWhere), nameof(DialogWidth))]
    private int _dialog; // 0 none, 1 install, 2 remove, 3 where they go

    public bool DialogOpen => Dialog != 0;
    public bool IsInstall => Dialog == 1;
    public bool IsRemove => Dialog == 2;
    public bool IsWhere => Dialog == 3;
    public double DialogWidth => Dialog == 3 ? 580 : 560;

    [ObservableProperty] private string _dialogError = "";
    private ModPackage? _package;
    // where the package's wrestlers go (by folder; "" = new), chosen in the Install dialog
    private Dictionary<string, string> _installPlaces = new();

    public string InstallTitle => _package is { } p ? $"Install {p.Mod.Name} into {ForProject}?" : "";
    public string InstallFrom => _package?.Source ?? "";
    public string InstallVersion => _package is { } p ? (p.Mod.Version.Length > 0 ? p.Mod.Version : "not given") : "";
    public string InstallContents => _package?.Mod.Contents ?? "";
    public bool InstallHasWrestlers => _package is { Mod.Wrestlers.Count: > 0 };
    public bool InstallPlacesInline => _package is { Mod.Wrestlers.Count: > 0 and <= 3 };
    public bool InstallPlacesSummaryShown => _package is { Mod.Wrestlers.Count: > 3 };
    public string InstallPlacesHeading => _package is { Mod.Wrestlers.Count: 1 } p
        ? $"WHERE {p.Mod.Wrestlers[0].Name.ToUpperInvariant()} GOES" : "WHERE THEY GO";
    public string InstallPlacesSummary
    {
        get
        {
            if (_package is not { } p)
                return "";
            var replace = p.Mod.Wrestlers.Count(w => _installPlaces.TryGetValue(w.Folder, out var v) ? v.Length > 0 : !w.IsNew);
            return $"{p.Mod.Wrestlers.Count} wrestlers · {replace} replace · {p.Mod.Wrestlers.Count - replace} new";
        }
    }
    public bool InstallHasNeeds => InstallNeedLines.Count > 0;
    public bool InstallNeedsMissing => _package?.Needs.Any(n => !n.InProject) ?? false;
    public string InstallMissingText
    {
        get
        {
            var missing = _package?.Needs.Where(n => !n.InProject).Select(n => n.Name).ToList() ?? new List<string>();
            return missing.Count switch
            {
                0 => "",
                1 => $"{missing[0]} is missing. {ForProject} can't be played until it's installed.",
                _ => $"{string.Join(", ", missing)} are missing. {ForProject} can't be played until they're installed.",
            };
        }
    }
    public bool InstallReplaces => _package?.Replaces != null;
    public string InstallReplacesText => _package?.Replaces is { } old
        ? $"This replaces {(old.Version.Length > 0 ? "version " + old.Version : "the one installed")}, including any changes you made to it." : "";
    public bool InstallBreaksSomething => InstallBreaks.Count > 0;
    public string InstallBreaksAfter => $"{ForProject} can't be played until this is fixed.";
    public string InstallConfirmText => InstallBreaksSomething ? "Install anyway" : "Install";

    /// <summary>A zip or a folder picked (or dropped): read, and the Install dialog shown.</summary>
    public void Pick(string path)
    {
        if (ForProject == null)
        {
            _say("Make a project on the Play page first: mods are installed into a project.");
            return;
        }
        ProjectMods.Discard(_package);
        var (package, error) = ProjectMods.Open(_folder, ForProject, path);
        _package = package;
        DialogError = "";
        if (package == null)
        {
            _say(error ?? "That isn't a mod.");
            return;
        }
        _installPlaces = package.Mod.Wrestlers.ToDictionary(w => w.Folder, w => GameRoster.Find(w.Place)?.Name ?? w.Place);
        // with a mod already installed it replaces, its places kept for the wrestlers it still has
        if (package.Replaces is { } old)
            foreach (var w in old.Wrestlers.Where(w => _installPlaces.ContainsKey(w.Folder)))
                _installPlaces[w.Folder] = GameRoster.Find(w.Place)?.Name ?? w.Place;
        InstallNeedLines.Clear();
        foreach (var (name, inProject) in package.Needs)
            InstallNeedLines.Add((inProject ? "✓  " : "✗  ") + name + (inProject ? "" : "  (not in this project)"));
        // what it would break in the project's other mods (an update that takes something away)
        InstallBreaks.Clear();
        var now = ProjectCheck.Problems(_folder, ForProject).Select(p => p.Text).ToHashSet();
        foreach (var p in ProjectCheck.Problems(_folder, ForProject, package.Replaces?.FolderName ?? package.Mod.FolderName, package.Staged))
            if (!now.Contains(p.Text) && !p.Mod.Equals(package.Replaces?.FolderName ?? package.Mod.FolderName, StringComparison.OrdinalIgnoreCase))
                InstallBreaks.Add(p.Text);
        if (package.Mod.Wrestlers.Count > 0)
        {
            BuildPlaceRows(package.Mod, package.Staged, _installPlaces);
            // a place another mod already fills: in as a new wrestler (no clash is made here; the player can change it)
            foreach (var row in PlaceRows.Where(r => r.Selected is { IsEnabled: false }))
                row.Selected = row.Options[0];
            _installPlaces = PlaceRows.ToDictionary(r => r.Folder, r => r.Value);
        }
        RaiseInstall();
        Dialog = 1;
    }

    private void RaiseInstall()
    {
        foreach (var name in new[]
                 {
                     nameof(InstallTitle), nameof(InstallFrom), nameof(InstallVersion), nameof(InstallContents),
                     nameof(InstallHasWrestlers), nameof(InstallPlacesInline), nameof(InstallPlacesSummaryShown),
                     nameof(InstallPlacesHeading), nameof(InstallPlacesSummary), nameof(InstallHasNeeds),
                     nameof(InstallNeedsMissing), nameof(InstallMissingText), nameof(InstallReplaces),
                     nameof(InstallReplacesText), nameof(InstallBreaksSomething), nameof(InstallBreaksAfter),
                     nameof(InstallConfirmText),
                 })
            OnPropertyChanged(name);
    }

    // Where they go: the rows for a mod's wrestlers (each with every place), the places another mod on (or another row)
    // already fills greyed with who has them
    private ModInfo? _whereMod;          // null: the Install dialog's package
    private Dictionary<string, string> _whereStart = new();
    private string _whereDir = "";

    public string WhereTitle => _whereMod is { } m ? $"Where {m.Name}'s wrestlers go in {ForProject}"
        : _package is { } p ? $"Where {p.Mod.Name}'s wrestlers go in {ForProject}" : "";
    public string WhereSummary
    {
        get
        {
            var replace = PlaceRows.Count(r => r.Value.Length > 0);
            return $"{PlaceRows.Count} wrestler{(PlaceRows.Count == 1 ? "" : "s")} · {replace} replace · {PlaceRows.Count - replace} new";
        }
    }
    [ObservableProperty] private string _whereSearch = "";
    public IEnumerable<PlaceRow> WhereRows =>
        WhereSearch.Trim().Length == 0 ? PlaceRows : PlaceRows.Where(r => r.Name.Contains(WhereSearch.Trim(), StringComparison.OrdinalIgnoreCase));
    partial void OnWhereSearchChanged(string value) => OnPropertyChanged(nameof(WhereRows));

    private void OpenWhere(ModInfo mod)
    {
        _whereMod = mod;
        var places = mod.Wrestlers.ToDictionary(w => w.Folder, w => GameRoster.Find(w.Place)?.Name ?? w.Place);
        BuildPlaceRows(mod, mod.Folder, places);
        WhereSearch = "";
        DialogError = "";
        OnPropertyChanged(nameof(WhereTitle));
        OnPropertyChanged(nameof(WhereRows));
        Dialog = 3;
    }

    [RelayCommand]
    private void OpenInstallWhere()
    {
        if (_package == null)
            return;
        _whereMod = null;
        BuildPlaceRows(_package.Mod, _package.Staged, _installPlaces);
        WhereSearch = "";
        OnPropertyChanged(nameof(WhereTitle));
        OnPropertyChanged(nameof(WhereRows));
        Dialog = 3;
    }

    private void BuildPlaceRows(ModInfo mod, string modDir, Dictionary<string, string> places)
    {
        _whereStart = new Dictionary<string, string>(places);
        _whereDir = modDir;
        PlaceRows.Clear();
        foreach (var w in mod.Wrestlers)
        {
            var options = new List<PlaceOption> { new() { Value = "", Name = "New wrestler", Label = "New wrestler" } };
            options.AddRange(GameRoster.All.Select(g => new PlaceOption { Value = g.Name, Name = g.Name, Label = "Replaces " + g.Name }));
            var row = new PlaceRow
            {
                Folder = w.Folder,
                Name = w.Name,
                Attires = ProjectCheck.AttireSpots(Path.Combine(modDir, w.Folder)),
                Options = options,
                Changed = UpdatePlaceOptions,
            };
            var want = places.TryGetValue(w.Folder, out var v) ? v : "";
            row.Selected = options.FirstOrDefault(o => o.Value.Equals(GameRoster.Find(want)?.Name ?? want, StringComparison.OrdinalIgnoreCase)) ?? options[0];
            PlaceRows.Add(row);
        }
        UpdatePlaceOptions();
    }

    private bool _updatingPlaces;

    private void UpdatePlaceOptions()
    {
        if (_updatingPlaces)
            return;
        _updatingPlaces = true;
        // the places the project's other mods (on) fill, and which attires
        var others = new List<(string Wrestler, IReadOnlyList<int> Attires, string Who)>();
        var self = _whereMod?.FolderName ?? _package?.Replaces?.FolderName ?? _package?.Mod.FolderName;
        foreach (var m in _mods.Where(m => IsOn(m) && !m.FolderName.Equals(self, StringComparison.OrdinalIgnoreCase)))
            foreach (var w in m.Wrestlers.Where(w => !w.IsNew))
                others.Add((GameRoster.Find(w.Place)?.Name ?? w.Place, ProjectCheck.AttireSpots(Path.Combine(m.Folder, w.Folder)), w.Name));
        foreach (var row in PlaceRows)
            foreach (var option in row.Options.Skip(1))
            {
                var game = GameRoster.Find(option.Value);
                bool Overlaps(IReadOnlyList<int> a) => a.Any(n => n <= (game?.Attires ?? 1) && row.Attires.Contains(n));
                var holder = others.FirstOrDefault(o => o.Wrestler.Equals(option.Value, StringComparison.OrdinalIgnoreCase) && Overlaps(o.Attires)).Who
                             ?? PlaceRows.FirstOrDefault(r => r != row && r.Value.Equals(option.Value, StringComparison.OrdinalIgnoreCase) && Overlaps(r.Attires))?.Name;
                option.IsEnabled = holder == null;
                option.Label = "Replaces " + option.Name + (holder != null ? $"  ({holder})" : "");
            }
        _updatingPlaces = false;
        OnPropertyChanged(nameof(WhereSummary));
        if (Dialog == 1)
        {
            _installPlaces = PlaceRows.ToDictionary(r => r.Folder, r => r.Value);
            OnPropertyChanged(nameof(InstallPlacesSummary));
        }
    }

    [RelayCommand]
    private void ResetPlaces()
    {
        foreach (var row in PlaceRows)
        {
            var want = _whereStart.TryGetValue(row.Folder, out var v) ? v : "";
            row.Selected = row.Options.FirstOrDefault(o => o.Value.Equals(want, StringComparison.OrdinalIgnoreCase)) ?? row.Options[0];
        }
        UpdatePlaceOptions();
    }

    public string RemoveTitle => PickedMod is { } m ? $"Remove {m.Name} from {ForProject}?" : "";
    public string RemoveText => "It's deleted from this project, including any changes you made to it.";
    public string RemoveWarning
    {
        get
        {
            if (PickedMod is not { } m)
                return "";
            var users = _mods.Where(x => x != m && IsOn(x) && x.Needs.Any(m.Is)).Select(x => x.Name).ToList();
            return users.Count == 0 ? ""
                : $"{string.Join(", ", users)} need{(users.Count == 1 ? "s" : "")} {m.Name}. {ForProject} can't be played until that's fixed.";
        }
    }
    public bool RemoveHasWarning => RemoveWarning.Length > 0;

    [RelayCommand]
    private void AskRemove()
    {
        if (PickedMod == null)
            return;
        DialogError = "";
        OnPropertyChanged(nameof(RemoveTitle));
        OnPropertyChanged(nameof(RemoveText));
        OnPropertyChanged(nameof(RemoveWarning));
        OnPropertyChanged(nameof(RemoveHasWarning));
        Dialog = 2;
    }

    [RelayCommand]
    private void CancelDialog()
    {
        if (Dialog == 3 && _whereMod == null && _package != null)
        {
            // back to the Install dialog, its places as they were
            if (InstallPlacesInline)
                BuildPlaceRows(_package.Mod, _package.Staged, _installPlaces);
            Dialog = 1;
            return;
        }
        if (Dialog == 1)
        {
            ProjectMods.Discard(_package);
            _package = null;
        }
        Dialog = 0;
        DialogError = "";
    }

    [RelayCommand]
    private void ConfirmDialog()
    {
        if (_readOnly || ForProject == null)
            return;
        if (Dialog == 1 && _package is { } p)
        {
            if (InstallPlacesInline)
                _installPlaces = PlaceRows.ToDictionary(r => r.Folder, r => r.Value);
            var (folderName, error) = ProjectMods.Install(_folder, ForProject, p, _installPlaces);
            if (error != null)
            {
                DialogError = error;
                return;
            }
            ProjectMods.Discard(p);
            _package = null;
            Dialog = 0;
            Picked = folderName;
            LoadProject();
            _projectsChanged();
            _say(p.Replaces != null ? $"{p.Mod.Name} is updated in {ForProject}." : $"{p.Mod.Name} is installed in {ForProject}.");
        }
        else if (Dialog == 2 && PickedMod is { } mod)
        {
            if (ProjectMods.Remove(_folder, ForProject, mod) is { } error)
            {
                DialogError = error;
                return;
            }
            Dialog = 0;
            Picked = null;
            LoadProject();
            _projectsChanged();
            _say($"{mod.Name} is removed from {ForProject}.");
        }
        else if (Dialog == 3)
        {
            var places = PlaceRows.ToDictionary(r => r.Folder, r => r.Value);
            if (_whereMod == null)
            {
                // the Install dialog's choice
                _installPlaces = places;
                if (InstallPlacesInline)
                    BuildPlaceRows(_package!.Mod, _package.Staged, _installPlaces);
                OnPropertyChanged(nameof(InstallPlacesSummary));
                Dialog = 1;
                return;
            }
            if (ProjectMods.Place(_whereMod, places) is { } error)
            {
                DialogError = error;
                return;
            }
            Dialog = 0;
            LoadProject();
            _projectsChanged();
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
            case "where":
                if (Environment.GetEnvironmentVariable("VENDETTA_SHOT_PICK") is { Length: > 0 } pick)
                    Picked = _mods.FirstOrDefault(m => m.Is(pick))?.FolderName ?? Picked;
                ChangePlaces();
                break;
            case "pick":
                if (Environment.GetEnvironmentVariable("VENDETTA_SHOT_PICK") is { Length: > 0 } picked)
                    Picked = _mods.FirstOrDefault(m => m.Is(picked))?.FolderName ?? Picked;
                break;
        }
    }
}
