using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using VendettaSettings.Services;

namespace VendettaSettings.ViewModels;

/// <summary>One line of the project's list: a mod installed in it (or one project.json names that isn't there), on or
/// off, what it does in one line, or what's wrong with it and the button that fixes it.</summary>
public sealed class ModRow
{
    public ModInfo? Mod { get; init; }
    public required string Name { get; init; }
    public required string Kind { get; init; }
    public required string Line { get; init; }
    public required bool IsOn { get; init; }
    /// <summary>It has a problem that stops the project from starting (outlined, its line says what).</summary>
    public required bool IsBad { get; init; }
    /// <summary>It is off and a mod that is on needs it.</summary>
    public required bool IsNeeded { get; init; }
    public string FixText { get; init; } = "";
    public IRelayCommand? Fix { get; init; }
    public bool HasFix => Fix != null;
    public required bool CanUp { get; init; }
    public required bool CanDown { get; init; }
    public required IRelayCommand Toggle { get; init; }
    public required IRelayCommand Up { get; init; }
    public required IRelayCommand Down { get; init; }
    public required IRelayCommand Remove { get; init; }
    public bool IsInstalled => Mod != null;
    public string ToggleTip => (IsOn ? "Turn off " : "Turn on ") + Name;
}

/// <summary>A category button over the list.</summary>
public sealed class CategoryChip
{
    public required string Label { get; init; }
    public required bool IsOn { get; init; }
    public required IRelayCommand Pick { get; init; }
}

/// <summary>The Mods page for one project (wrestler-manager plan step 5, 2026-10-22: the settings app plays, the mod kit
/// installs): the mods in the project as one list in load order (the ones on first, top loads first, then the ones off),
/// each turned on or off (written to its project.json at once), moved up or down, removed; a mod with a problem that
/// stops the project from starting is outlined with what's wrong and its fix in its own row. Mods are added with the
/// mod kit (Open the mod kit).</summary>
public partial class ModsViewModel : ObservableObject
{
    private readonly GameFolder _folder;
    private readonly bool _readOnly;
    private readonly Action<string> _say;
    private readonly Action _projectsChanged;

    private List<ModInfo> _mods = new();
    private List<string> _on = new();          // the project's list, as project.json names them
    private List<ModProblem> _problems = new();

    public ObservableCollection<string> ProjectNames { get; } = new();
    public ObservableCollection<ModRow> Rows { get; } = new();
    public ObservableCollection<CategoryChip> Chips { get; } = new();

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
    [NotifyPropertyChangedFor(nameof(HasProject), nameof(NoProject))]
    private string? _forProject;

    partial void OnForProjectChanged(string? value)
    {
        if (!_opening)
            LoadProject();
    }

    public bool HasProject => ForProject != null;
    public bool NoProject => ForProject == null;

    private void LoadProject()
    {
        _mods = ForProject == null ? new List<ModInfo>() : ProjectMods.List(_folder, ForProject);
        _on = ForProject == null ? new List<string>()
            : Projects.List(_folder).FirstOrDefault(p => p.Name == ForProject)?.Mods.ToList() ?? new List<string>();
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

    private void CheckProblems() =>
        _problems = ForProject == null ? new List<ModProblem>() : ProjectCheck.Problems(_folder, ForProject);

    // --- the list ---

    [ObservableProperty] private string _search = "";
    [ObservableProperty] private string _category = "All";
    partial void OnSearchChanged(string value) => Rebuild();
    partial void OnCategoryChanged(string value) => Rebuild();

    public string ListHeading => ForProject == null ? "" :
        $"IN {ForProject.ToUpperInvariant()} · {_mods.Count} MOD{(_mods.Count == 1 ? "" : "S")} · {_mods.Count(IsOn)} ON · TOP LOADS FIRST";
    public bool ListEmpty => ForProject != null && _mods.Count == 0 && _on.Count == 0;
    public bool NothingMatches => !ListEmpty && ForProject != null && Rows.Count == 0;
    public int BadCount => Rows.Count(r => r.IsBad);
    public bool HasBad => _problems.Count > 0;
    public string BadText
    {
        get
        {
            var n = _problems.Select(p => p.Mod).Distinct(StringComparer.OrdinalIgnoreCase).Count();
            return n == 1 ? "1 mod needs fixing" : $"{n} mods need fixing";
        }
    }
    public string EmptyText => $"No mods in {ForProject} yet. Add mods with the mod kit.";

    private IEnumerable<string> CategoriesInOrder() =>
        _mods.Select(m => m.Category).Distinct(StringComparer.OrdinalIgnoreCase)
            .OrderBy(c => Array.FindIndex(ProjectMods.KnownCategories, k => k.Equals(c, StringComparison.OrdinalIgnoreCase)) is var i && i >= 0 ? i : 99)
            .ThenBy(c => c, StringComparer.OrdinalIgnoreCase);

    private void Rebuild()
    {
        // the search ignores case, spaces and punctuation ("readd" finds "Re-added")
        static string Letters(string text) => new(text.Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray());
        var q = Letters(Search);
        bool Shown(string name, ModInfo? m) =>
            (Category == "All" || (m?.Category.Equals(Category, StringComparison.OrdinalIgnoreCase) ?? false)) &&
            (q.Length == 0 || Letters($"{name} {m?.FolderName} {m?.Contents}").Contains(q));

        Chips.Clear();
        var categories = CategoriesInOrder().ToList();
        if (!categories.Contains(Category, StringComparer.OrdinalIgnoreCase) && Category != "All")
            Category = "All";
        if (categories.Count > 1)
        {
            Chips.Add(new CategoryChip { Label = "All", IsOn = Category == "All", Pick = new RelayCommand(() => Category = "All") });
            foreach (var c in categories)
                Chips.Add(new CategoryChip
                {
                    Label = $"{c} {_mods.Count(m => m.Category.Equals(c, StringComparison.OrdinalIgnoreCase))}",
                    IsOn = Category.Equals(c, StringComparison.OrdinalIgnoreCase),
                    Pick = new RelayCommand(() => Category = c),
                });
        }

        Rows.Clear();
        // the load order first (a name there that isn't installed too), then the mods that are off, by name
        for (var i = 0; i < _on.Count; i++)
        {
            var mod = Find(_on[i]);
            if (Shown(mod?.Name ?? _on[i], mod))
                Rows.Add(RowFor(mod, _on[i], i));
        }
        foreach (var m in _mods.Where(m => !IsOn(m)).OrderBy(m => m.Name, StringComparer.CurrentCultureIgnoreCase))
            if (Shown(m.Name, m))
                Rows.Add(RowFor(m, m.FolderName, -1));

        foreach (var name in new[] { nameof(ListHeading), nameof(ListEmpty), nameof(NothingMatches), nameof(HasBad), nameof(BadText),
                                     nameof(BadCount), nameof(EmptyText), nameof(HasChips) })
            OnPropertyChanged(name);
    }

    public bool HasChips => Chips.Count > 0;

    // a row: what it does, or (a problem) what's wrong and the fix
    private ModRow RowFor(ModInfo? mod, string listName, int at)
    {
        var name = mod?.Name ?? listName;
        var mine = _problems.Where(p => p.Mod.Equals(mod?.FolderName ?? listName, StringComparison.OrdinalIgnoreCase)
                                        || (mod != null && mod.Is(p.Mod))).ToList();
        var on = mod == null || IsOn(mod);
        string line;
        string fixText = "";
        IRelayCommand? fix = null;
        if (mine.FirstOrDefault() is { } p)
        {
            switch (p.Kind)
            {
                case ProblemKind.NotInstalled:
                    line = "On, but it isn't in this project";
                    fixText = "Take it off the list";
                    fix = new RelayCommand(() => { _on.RemoveAt(at); SaveOn(); Rebuild(); });
                    break;
                case ProblemKind.NeedMissing when Find(p.Other) is { } need:
                    line = $"Needs {need.Name}, which is off";
                    fixText = $"Turn on {need.Name}";
                    fix = new RelayCommand(() => Toggle(need));
                    break;
                case ProblemKind.NeedMissing:
                    line = $"Needs {p.Other}, which isn't in this project (add it with the mod kit)";
                    fixText = "Turn it off";
                    fix = new RelayCommand(() => Toggle(mod!));
                    break;
                default:
                    line = p.Text.TrimEnd('.');
                    fixText = "Turn it off";
                    fix = mod != null ? new RelayCommand(() => Toggle(mod)) : null;
                    break;
            }
            if (mine.Count > 1)
                line += $" (and {mine.Count - 1} more)";
        }
        else
        {
            var what = mod is { Wrestlers.Count: > 0 } ? Capital(mod.PlacesSummary) : mod?.Contents ?? "";
            var users = mod == null || on ? new List<string>() : _mods.Where(x => x != mod && IsOn(x) && x.Needs.Any(mod.Is)).Select(x => x.Name).ToList();
            line = users.Count > 0 ? $"Off · needed by {string.Join(", ", users)}" : on ? what : "Off · " + what;
        }
        return new ModRow
        {
            Mod = mod,
            Name = name,
            Kind = mod?.Category ?? "",
            Line = line,
            IsOn = on,
            IsBad = mine.Count > 0,
            IsNeeded = mine.Count == 0 && !on && mod != null && _mods.Any(x => x != mod && IsOn(x) && x.Needs.Any(mod.Is)),
            FixText = fixText,
            Fix = fix,
            CanUp = at > 0,
            CanDown = at >= 0 && at < _on.Count - 1,
            Toggle = new RelayCommand(() => { if (mod != null) Toggle(mod); }),
            Up = new RelayCommand(() => Move(at, -1)),
            Down = new RelayCommand(() => Move(at, 1)),
            Remove = new RelayCommand(() => { if (mod != null) AskRemove(mod); }),
        };
    }

    private static string Capital(string s) => s.Length == 0 ? s : char.ToUpperInvariant(s[0]) + s[1..];

    private void Move(int i, int by)
    {
        var j = i + by;
        if (i < 0 || j < 0 || j >= _on.Count)
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

    // the mods on that need any of these, and what needs those
    private HashSet<ModInfo> WithUsers(ModInfo mod)
    {
        var drop = new HashSet<ModInfo> { mod };
        for (bool more = true; more;)
        {
            more = false;
            foreach (var m in _mods.Where(m => IsOn(m) && !drop.Contains(m) && m.Needs.Any(n => drop.Any(d => d.Is(n)))))
                more |= drop.Add(m);
        }
        return drop;
    }

    [RelayCommand]
    private void OffBoth()
    {
        if (_pending is not { } mod)
            return;
        var drop = WithUsers(mod);
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

    [RelayCommand]
    private void OpenProjectMods()
    {
        if (ForProject == null)
            return;
        var dir = ProjectMods.RootOf(_folder, ForProject);
        Directory.CreateDirectory(dir);
        GameFolder.Open(dir);
    }

    // --- the mod kit: it writes where it is (mod-kit.txt beside the projects) when it links to this recomp ---

    private string? ModKit
    {
        get
        {
            try
            {
                var file = Path.Combine(_folder.Data, "mod-kit.txt");
                return File.Exists(file) && File.ReadAllText(file).Trim() is { Length: > 0 } exe && File.Exists(exe) ? exe : null;
            }
            catch
            {
                return null;
            }
        }
    }

    public bool HasModKit => ModKit != null;

    [RelayCommand]
    private void OpenModKit()
    {
        if (ModKit is not { } exe)
        {
            _say("The mod kit isn't linked to this recomp yet: open the mod kit once and link it (Linked games).");
            return;
        }
        try
        {
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo { FileName = exe, UseShellExecute = true, WorkingDirectory = Path.GetDirectoryName(exe)! });
        }
        catch (Exception ex)
        {
            _say($"The mod kit couldn't be started: {ex.Message}");
        }
    }

    // --- Remove from this project (a mod that others need turns them off too) ---

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DialogOpen))]
    private ModInfo? _removing;
    public bool DialogOpen => Removing != null;
    [ObservableProperty] private string _dialogError = "";
    public string RemoveTitle => Removing is { } m ? $"Remove {m.Name} from {ForProject}?" : "";
    public string RemoveText => "It's deleted from this project. It stays in the mod kit, so you can add it back.";
    public string RemoveWarning
    {
        get
        {
            if (Removing is not { } m)
                return "";
            var users = WithUsers(m).Where(x => x != m).Select(x => x.Name).ToList();
            return users.Count == 0 ? "" : $"{string.Join(", ", users)} use{(users.Count == 1 ? "s" : "")} it, so {(users.Count == 1 ? "it is" : "they are")} turned off too.";
        }
    }
    public bool RemoveHasWarning => RemoveWarning.Length > 0;
    public string RemoveConfirmText => Removing is { } m && WithUsers(m).Where(x => x != m).Select(x => x.Name).ToList() is { Count: > 0 } users
        ? $"Remove and turn off {string.Join(", ", users)}" : "Remove";

    private void AskRemove(ModInfo mod)
    {
        DialogError = "";
        Removing = mod;
        foreach (var name in new[] { nameof(RemoveTitle), nameof(RemoveText), nameof(RemoveWarning), nameof(RemoveHasWarning), nameof(RemoveConfirmText) })
            OnPropertyChanged(name);
    }

    [RelayCommand]
    private void CancelDialog()
    {
        Removing = null;
        DialogError = "";
    }

    [RelayCommand]
    private void ConfirmDialog()
    {
        if (_readOnly || ForProject == null || Removing is not { } mod)
            return;
        var users = WithUsers(mod);
        if (ProjectMods.Remove(_folder, ForProject, mod) is { } error)
        {
            DialogError = error;
            return;
        }
        var project = ForProject;
        Removing = null;
        LoadProject();
        if (users.Count > 1)
        {
            _on.RemoveAll(t => users.Any(d => d.Is(t)));
            SaveOn();
            Rebuild();
        }
        _projectsChanged();
        _say($"{mod.Name} is removed from {project}.");
    }

    /// <summary>For pictures (--shot): the page, or a dialog open (VENDETTA_SHOT_PICK: the mod; "off" turns it off first).</summary>
    public void ShowSample(string which, string project)
    {
        Open(project);
        var pick = Environment.GetEnvironmentVariable("VENDETTA_SHOT_PICK") is { Length: > 0 } p ? Find(p) : null;
        switch (which)
        {
            case "remove":
                if ((pick ?? _mods.FirstOrDefault()) is { } m)
                    AskRemove(m);
                break;
            case "off":
                if (pick != null)
                {
                    _on.RemoveAll(pick.Is);
                    SaveOn();
                    Rebuild();
                }
                break;
        }
    }

    /// <summary>For the self-test: a row's buttons, by the mod's name.</summary>
    public ModRow? Row(string name) => Rows.FirstOrDefault(r => r.Name.Equals(name, StringComparison.OrdinalIgnoreCase));
}
