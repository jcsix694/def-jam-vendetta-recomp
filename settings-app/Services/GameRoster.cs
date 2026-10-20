namespace VendettaSettings.Services;

/// <summary>The game's 44 wrestlers (the select grid's slots): the name shown here, the game's own full and short names
/// (a mod's "replace" may give any of them: the game compares them in upper case) and how many attires each has.</summary>
public static class GameRoster
{
    public sealed record Wrestler(string Name, string Full, string Short, int Attires);

    /// <summary>In the order shown (by name).</summary>
    public static readonly IReadOnlyList<Wrestler> All = new Wrestler[]
    {
        new("Arii", "ARII", "ARII", 1),
        new("Briggs", "BRIGGS", "BRIGGS", 3),
        new("Capone", "CAPONE", "CAPONE", 1),
        new("Carla", "CARLA", "CARLA", 1),
        new("Chukklez", "CHUKKLEZ", "CHUKKLEZ", 1),
        new("Cruz", "CRUZ", "CRUZ", 1),
        new("D-Mob", "D-MOB", "D-MOB", 2),
        new("Dan G", "DAN G", "DAN G", 1),
        new("Deebo", "DEEBO", "DEEBO", 1),
        new("Deja", "DEJA", "DEJA", 1),
        new("DMX", "DMX", "DMX", 1),
        new("Drake", "DRAKE", "DRAKE", 1),
        new("Funkmaster Flex", "FUNKMASTER FLEX", "FUNK FLEX", 1),
        new("Ghostface Killah", "GHOSTFACE KILLAH", "GHOSTFACE", 1),
        new("Headache", "HEADACHE", "HEADACHE", 1),
        new("House", "HOUSE", "HOUSE", 1),
        new("Iceberg", "ICEBERG", "ICEBERG", 1),
        new("Joe Budden", "JOE BUDDEN", "JOE", 1),
        new("Keith Murray", "KEITH MURRAY", "K. MURRAY", 1),
        new("Ludacris", "LUDACRIS", "LUDACRIS", 1),
        new("Manny", "MANNY", "MANNY", 2),
        new("Masa", "MASA", "MASA", 1),
        new("Method Man", "METHOD MAN", "METHOD", 1),
        new("Moses", "MOSES", "MOSES", 1),
        new("NORE", "NORE", "NORE", 1),
        new("Nyne", "NYNE", "NYNE", 1),
        new("Omar", "OMAR", "OMAR", 1),
        new("Opal", "OPAL", "OPAL", 1),
        new("Peewee", "PEEWEE", "PEEWEE", 2),
        new("Penny", "PENNY", "PENNY", 1),
        new("Pockets", "POCKETS", "POCKETS", 1),
        new("Proof", "PROOF", "PROOF", 3),
        new("Razor", "RAZOR", "RAZOR", 2),
        new("Redman", "REDMAN", "REDMAN", 1),
        new("Ruffneck", "RUFFNECK", "RUFFNECK", 2),
        new("Scarface", "SCARFACE", "SCARFACE", 1),
        new("Sketch", "SKETCH", "SKETCH", 2),
        new("Snowman", "SNOWMAN", "SNOWMAN", 1),
        new("Spider", "SPIDER", "SPIDER", 3),
        new("Steel", "STEEL", "STEEL", 1),
        new("T'ai Monique", "T'AI MONIQUE", "T'AI", 1),
        new("Tank", "TANK", "TANK", 3),
        new("WC", "WC", "WC", 1),
        new("Zaheer", "ZAHEER", "ZAHEER", 1),
    };

    /// <summary>The wrestler a name means (the name shown, or the game's full or short name, any case), or null.</summary>
    public static Wrestler? Find(string name)
    {
        var want = name.Trim();
        return All.FirstOrDefault(w => w.Name.Equals(want, StringComparison.OrdinalIgnoreCase) ||
                                       w.Full.Equals(want, StringComparison.OrdinalIgnoreCase) ||
                                       w.Short.Equals(want, StringComparison.OrdinalIgnoreCase));
    }
}
