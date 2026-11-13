using VendettaSettings.Services;

namespace VendettaSettings.ViewModels;

/// <summary>The mod kit, as this program knows it: everything about a project's mods is done there. Which kit there is,
/// and starting it on a project.</summary>
public sealed class ModsViewModel
{
    private readonly GameFolder _folder;
    private readonly Action<string> _say;

    public ModsViewModel(GameFolder folder, Action<string> say)
    {
        _folder = folder;
        _say = say;
    }

    // --- the mod kit: the one that comes with the game (Mod kit\adjvrmk.exe beside it, which links itself), always,
    // as it is updated with the game; only without it (the repository) the one that last linked to this recomp (it
    // writes where it is in mod-kit.txt beside the projects). A kit once linked to this install and later to another
    // game leaves its path in mod-kit.txt, so that file never wins over the kit beside the game. ---

    public const string ShippedModKit = "Mod kit\\adjvrmk.exe";

    internal string? ModKit
    {
        get
        {
            try
            {
                var shipped = Path.Combine(_folder.Root, ShippedModKit);
                if (File.Exists(shipped))
                    return shipped;
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

    /// <summary>The mod kit started on one project (its PROJECTS side): what the Play page's button does.</summary>
    public void OpenModKitOn(string project)
    {
        if (ModKit is not { } exe)
        {
            _say("The mod kit wasn't found: open the mod kit once and link it to this recomp (Linked games).");
            return;
        }
        try
        {
            var start = new System.Diagnostics.ProcessStartInfo { FileName = exe, UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(exe)! };
            start.ArgumentList.Add("--project");
            start.ArgumentList.Add(project);
            System.Diagnostics.Process.Start(start);
        }
        catch (Exception ex)
        {
            _say($"The mod kit couldn't be started: {ex.Message}");
        }
    }
}
