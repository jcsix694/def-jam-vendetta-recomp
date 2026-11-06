namespace VendettaSettings.Services;

/// <summary>The game's 12 arenas as a mod's "replace": "arenas" names them (the game's own matching, vendetta_arenas.cpp
/// venueNamed: upper or lower case, a leading "The" left off, After Hours also as "Face Club After Hours") and their
/// names as the game writes them in its messages.</summary>
public static class GameArenas
{
    public static readonly string[] Names =
    {
        "The Bounty Club", "The Junkyard", "The Warehouse", "Grimeyville", "The Dragon House", "Da Bridge", "Club Luda",
        "The Face Club", "After Hours", "Sanctuary", "Def Jam", "Def Jam Vendetta",
    };

    private static readonly (string Name, int Venue)[] Matches =
    {
        ("bounty club", 0), ("junkyard", 1), ("warehouse", 2), ("grimeyville", 3), ("dragon house", 4), ("da bridge", 5),
        ("club luda", 6), ("face club", 7), ("face club after hours", 8), ("after hours", 8), ("sanctuary", 9), ("def jam", 10),
        ("def jam vendetta", 11),
    };

    /// <summary>The arena's place (0-11), or -1 when the game has no arena called that.</summary>
    public static int Find(string name)
    {
        var n = name.ToLowerInvariant();
        if (n.StartsWith("the "))
            n = n[4..];
        foreach (var (match, venue) in Matches)
            if (n == match)
                return venue;
        return -1;
    }
}
