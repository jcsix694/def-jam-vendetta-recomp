// Def Jam Vendetta (USA, SLUS-20639) — new wrestlers from mods (see vendetta_wrestlers.h for the folder).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// The game keeps a wrestler in five tables, each with a fixed number of entries and other data straight after it. The
// game's code finds them through luis that config/patches.toml points at copies in the store's fixed area (each copy
// keeps the original's low 16 bits, so no offset changes): this file makes the copies at start-up and adds the mods'
// wrestlers after the game's. The move data is not copied: the boot's loader reads it straight into the store's place
// (its address goes through a patched lui too); the new records' entries are added once it is there.
//   the character table 0x27E100: 63 records of 0x20 {+0 full name, +4 short name, +8 height, +0xC weight (pointers to
//     text), +0x10 render, +0x14 grid icon, +0x18 the logo's index (< 60: the unlock index), +0x1C the model's DATA.BIN
//     entry}; read at 32 places in 13 functions;
//   Choose Fighter's grid 0x239EF0: 44 slots {base record, attires shown} in columns of two (top, bottom);
//   the size classes 0x234910 (a float per record) and the voice map 0x2599B8 (a voice row per record);
//   the move data 0x2331C8 (0x420 bytes): +0x70 + 16 x record = {?, WPAR, ?, WWAZ} for records 0-58.
// The grid's 44 is also built into Choose Fighter FUN_0014fbd8 (patched to read the size from 0x400 and the last scroll
// position, size - 22, from 0x404) and into its seven helpers, replaced here.
#include "vendetta_wrestlers.h"

#include "ps2_runtime.h"
#include "vendetta_mods.h"
#include "vendetta_pictures.h"
#include "vendetta_store.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <set>

namespace vendetta_wrestlers
{
    namespace
    {
        using Fn = PS2Runtime::RecompiledFunction;

        constexpr uint32_t kGameCharacters = 0x27E100u, kGameGrid = 0x239EF0u;
        constexpr uint32_t kGameSizes = 0x234910u, kGameVoices = 0x2599B8u;
        constexpr uint32_t kGameRecords = 63u;     // character records; 59-62 are the empty "Character A-D"
        constexpr uint32_t kMovesetRecords = 59u;  // records with moves in the move data
        constexpr uint32_t kGameSlots = 44u;
        constexpr uint32_t kRecordSize = 0x20u;
        constexpr uint32_t kPairs = 0x70u;         // the move data + 0x70 + 16 x record
        constexpr uint32_t kMovesetSize = 8u + 256u; // "WWAZ", its size, 128 u16 slots
        constexpr uint32_t kWparSize = 0xE0u;        // a fight record: 1,769 bits the game reads, 23 it never does
        constexpr uint32_t kGridSizeWord = 0x400u; // read by Choose Fighter (patched)
        constexpr uint32_t kLastScrollWord = 0x404u;
        constexpr uint32_t kWindow = 22u;          // the tiles Choose Fighter shows
        constexpr uint32_t kNoLogo = 12u;          // FUN_0017c2c0's "no logo"
        constexpr uint32_t kFirstLogoIndex = 60u;  // new attires' +0x18: past the logo table, answered by the logo hook

        // Choose Fighter's helpers (all read the grid; a0 = the front end, a1 = a slot or the scroll position)
        constexpr uint32_t kRecount = 0x151460u;   // FUN_00151460(counts): each slot's attires from the unlock counts
        constexpr uint32_t kCanUp = 0x151498u;     // the slot above (a bottom slot) can be picked
        constexpr uint32_t kCanDown = 0x151500u;   // the slot below (a top slot) can be picked
        constexpr uint32_t kLeft = 0x151568u;      // columns to the nearest pickable slot left in the row (0 = none)
        constexpr uint32_t kRight = 0x151620u;     // ... right
        constexpr uint32_t kAnyBefore = 0x1516E0u; // a pickable slot before this one (the left arrow)
        constexpr uint32_t kAnyAfter = 0x151770u;  // a pickable slot after the window starting here (the right arrow)
        constexpr uint32_t kLogo = 0x17C2C0u;      // FUN_0017c2c0(index): the name logo, 12 = none
        constexpr uint32_t kLoadMatch = 0x165838u, kMatchInit = 0x119D70u;
        // FUN_00184d78(context, group, n): every wrestler picture (group 3 a name logo n, 4 grid icon n / 2 with its
        // shade when n is odd, 6 a render n) is uploaded to one GS slot before each draw
        constexpr uint32_t kFindPicture = 0x184D78u;
        constexpr uint32_t kLogoSlot = 0x3E50u, kIconSlot = 0x3E80u, kRenderSlot = 0x3EC0u;
        // a new attire's pictures: numbers from here (its render and icon in its character record, its logo from the
        // logo lookup); the game is handed pictures it never shows to upload, drawn over by the mod's files
        constexpr uint32_t kPictureBase = 0x4000u;
        constexpr uint32_t kStandInRender = 59u, kStandInIcon = 59u, kStandInLogo = 0u;
        constexpr uint32_t kLogoByRecordCall = 0x146458u; // Survival's right panel asks FUN_0017c2c0 by record

        // The winner banner FUN_0016b818 draws each line with FUN_0016ba50: a text object (+0 / +4 the x / y scale,
        // +8 the width of a character the font lacks, +0xC the spacing after each character) measured by
        // FUN_001586c0(object, text) and drawn by FUN_001587a0(object, text, x, y) at x = its centre - width / 2,
        // y = its middle - 25 x the y scale. A line wider than kBannerWidth at the steady size is drawn smaller.
        constexpr uint32_t kTextMeasure = 0x1586C0u, kTextDraw = 0x1587A0u;
        constexpr uint32_t kBannerMeasured[] = {0x16BB8Cu, 0x16BCB0u, 0x16BDBCu}; // the returns from its measures
        constexpr uint32_t kBannerDrawn[] = {0x16BBCCu, 0x16BCE8u, 0x16BDFCu};    // ... and from its draws
        constexpr uint32_t kGlyphs = 0x23B3B8u; // the banner font: 16 bytes a glyph, +0 its left edge, +8 its right
        constexpr float kBannerWidth = 600.0f;  // of the screen's 640
        constexpr float kBannerHalfHeight = 25.0f;

        struct AttirePictures
        {
            uint32_t render = 0, icon = 0, shade = 0, logo = 0; // vendetta_pictures ids (0 = none)
            uint32_t gameRender = 0, gameIcon = 0;              // the "based on" attire's, when a file can't be shown
        };

        struct Wrestler
        {
            std::string modName;
            std::string name;                     // short
            std::vector<uint32_t> records;        // its attires
            std::vector<uint32_t> wpar;           // per attire: its fight values ("WPAR", size, 0xE0 bytes) in the store
            std::vector<uint32_t> wwaz;           // ... its moveset ("WWAZ", size, 128 u16 slots) in the store
            std::set<std::string> names;          // upper case: its folder's name, every attire's short and full name
        };

        uint8_t *g_rdram = nullptr;
        std::vector<Wrestler> g_wrestlers;
        std::vector<SlotFile> g_slotFiles;
        std::vector<AttirePictures> g_attirePictures; // by record - 63
        std::vector<ModelFile> g_modelFiles;          // by record - 63 (an empty model: none of its own)
        constexpr const char *kModelNames[] = {"model.wrs", "model.glb", "model.gltf"};
        // the files that make an attire folder count (or its textures folder)
        constexpr const char *kAttireFiles[] = {"wrestler.json", "fight.json", "moveset.json", "render.png", "icon.png",
                                                "logo.png",      "model.wrs",  "model.glb",    "model.gltf"};
        constexpr size_t kMaxAttireFolders = 9; // looked at, to tell about the ones not used (3 at most for now)
        std::vector<std::pair<std::string, std::string>> g_gameNames; // the game's records' full and short names, upper
                                                                       // case, as the game has them (kept at start-up)

        // A game wrestler's attire replaced by a mod's (each attire is either replaced or the game's own; each part the
        // mod doesn't bring stays the game's)
        struct Replaced
        {
            std::string modName, gameName;
            std::set<std::string> names; // upper case: its own names (for moveset edits by name)
            uint32_t wpar = 0, wwaz = 0; // its own fight values / moveset in the store (0 = the game's)
            uint32_t pictures = ~0u;     // its pictures' index in g_attirePictures (~0u = none of its own)
            bool noLogo = false;         // a new identity (names or pictures) without a logo of its own: no logo
            uint32_t unlockIndex = 0;    // its +0x18: the logo and the story row go by it
            ModelFile model;             // empty: the game's model
            size_t slotFile = SIZE_MAX;  // its moveset.json's entry in g_slotFiles
        };
        std::map<uint32_t, Replaced> g_replaced; // by record (base + attire; records 63+: an added attire)

        // A game wrestler given attires he doesn't have: his Choose Fighter slot points at new records (63+): copies
        // of his own attires (replaced or not), then the added ones; Story and Survival keep his own records
        struct Moved
        {
            uint32_t base = 0, gameCount = 0; // his own first record and attires
            uint32_t slot = 0;                // his grid slot
            uint32_t first = 0, count = 0;    // the new records
            std::vector<uint32_t> sources;    // per new record: the record whose move data it starts from
        };
        std::vector<Moved> g_moved;
        uint32_t g_newEnd = kFirstNewRecord; // one past the new wrestlers' records (the moved slots' come after)
        constexpr uint32_t kUnlockTable = 0x27D610u; // 3 unlock flag indexes per base record: its attires (-1 = none)
        constexpr uint32_t kStoryRows = 0x27ED00u, kStoryRowSize = 0x38u, kStoryRowCount = 60u; // by unlock index
        constexpr uint32_t kHeroBuild = 0x13E958u; // FUN_0013e958(fighter): a story hero's build from his stats
        // FUN_00140bf0(out, profile, base): a signed-in profile's hero stats / 3, six ints at `out` (one block of 0x18 a
        // player from kHeroBonus), for a story hero's slot in Battle and Survival
        constexpr uint32_t kHeroUpgrades = 0x140BF0u, kHeroBonus = 0x2362A8u, kFrontEnd = 0x29E468u;
        constexpr uint32_t kIsArtist = 0x151808u; // FUN_00151808(record): records 37-48 = a locked tile's red silhouette
        constexpr const char *kStats[] = {"power", "speed", "grapple", "defense", "stamina", "charisma"};
        std::vector<uint32_t> g_firstAttires;         // each new wrestler's first attire (record - 63), in grid order
        constexpr int kWantBehind = 6, kWantAhead = 10; // wrestlers either side of a tile drawn whose pictures are read
        uint32_t g_endRecord = kFirstNewRecord;
        uint32_t g_slots = kGameSlots;            // Choose Fighter's slots (even)
        std::vector<uint32_t> g_newAttires;       // per slot from 44: its attires (0 = the filler of an odd count)
        bool g_moveDataReady = false;
        Fn g_originalLogo = nullptr, g_originalLoadMatch = nullptr, g_originalMatchInit = nullptr;
        Fn g_originalFindPicture = nullptr, g_originalHeroBuild = nullptr, g_originalHeroUpgrades = nullptr;
        Fn g_originalIsArtist = nullptr;
        Fn g_originalTextMeasure = nullptr, g_originalTextDraw = nullptr;
        float g_bannerShrink = 1.0f, g_bannerScaleY = 1.0f; // the line being drawn: its shrink and its y scale before it

        uint32_t read32(uint32_t address)
        {
            uint32_t value = 0;
            std::memcpy(&value, g_rdram + (address & PS2_RAM_MASK), 4);
            return value;
        }
        void write32(uint32_t address, uint32_t value) { std::memcpy(g_rdram + (address & PS2_RAM_MASK), &value, 4); }
        uint8_t *guest(uint32_t address) { return g_rdram + (address & PS2_RAM_MASK); }

        std::string upper(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return text;
        }

        std::string guestString(uint32_t address)
        {
            std::string text;
            if (address < 0x100000u || !vendetta_store::readable(address, 64u))
                return text;
            for (uint32_t i = 0; i < 64u && g_rdram[(address + i) & PS2_RAM_MASK]; ++i)
                text.push_back(static_cast<char>(g_rdram[(address + i) & PS2_RAM_MASK]));
            return text;
        }

        // Text the game's code reads (a name, a height): one copy each in the store's boot area.
        uint32_t guestText(const std::string &text)
        {
            const uint32_t address = vendetta_store::allocate(vendetta_store::Area::Boot,
                                                              static_cast<uint32_t>(text.size()) + 1u, "wrestler names", 4u);
            if (address)
                std::memcpy(guest(address), text.c_str(), text.size() + 1u);
            return address;
        }

        // A game wrestler by its full or short name (any case): its first record and how many attires it has (the
        // records after it with the same full name).
        bool gameWrestler(const std::string &name, uint32_t &base, uint32_t &attires)
        {
            const std::string want = upper(name);
            for (uint32_t r = 0; r < kMovesetRecords; ++r)
            {
                const uint32_t entry = kGameCharacters + kRecordSize * r;
                if (upper(guestString(read32(entry))) != want && upper(guestString(read32(entry + 4u))) != want)
                    continue;
                const std::string full = guestString(read32(entry));
                base = r;
                attires = 1u;
                while (base + attires < kMovesetRecords &&
                       guestString(read32(kGameCharacters + kRecordSize * (base + attires))) == full)
                    ++attires;
                return true;
            }
            return false;
        }

        std::string text(const vendetta_mods::Json &j, const char *key, const std::string &otherwise = {})
        {
            const vendetta_mods::Json *v = j.find(key);
            return v && v->isString() ? v->text : otherwise;
        }

        bool readJson(const std::filesystem::path &file, vendetta_mods::Json &j, std::string &error)
        {
            std::vector<uint8_t> bytes;
            if (file.empty() || !vendetta_mods::readWhole(file, bytes))
            {
                error = "can't be read";
                return false;
            }
            if (!vendetta_mods::parseJson(std::string(bytes.begin(), bytes.end()), j, error))
                return false;
            if (!j.isObject())
                error = "should be { ... }";
            return j.isObject();
        }

        // The fight record, bit by bit, most significant first, in the order the game's FUN_0011ccb8 reads it.
        struct Bits
        {
            std::array<uint8_t, kWparSize> bytes{};
            uint32_t at = 0;
            void put(uint32_t value, uint32_t width)
            {
                for (uint32_t i = width; i-- > 0; ++at)
                    if ((value >> i) & 1u)
                        bytes[at >> 3] |= static_cast<uint8_t>(0x80u >> (at & 7u));
            }
        };

        bool isNumber(const vendetta_mods::Json *v) { return v && v->type == vendetta_mods::Json::Type::Number; }

        // fight.json: every value of a wrestler's fight record (WPAR, unpacked by FUN_0011ccb8), each by name, none left
        // out. A missing or wrong value is named in `problems` (the wrestler is then left out).
        bool packFight(const vendetta_mods::Json &j, std::array<uint8_t, kWparSize> &out, std::vector<std::string> &problems)
        {
            Bits bits;
            const auto number = [&](const vendetta_mods::Json *group, const std::string &where, const char *key,
                                    uint32_t width, uint32_t scale = 1u) -> uint32_t
            {
                const vendetta_mods::Json *v = group ? group->find(key) : nullptr;
                const double most = static_cast<double>((1ull << width) - 1u) * scale;
                if (!isNumber(v) || v->number < 0 || v->number > most || v->number != std::floor(v->number) ||
                    static_cast<uint64_t>(v->number) % scale != 0u)
                {
                    problems.push_back(where + "\"" + key + "\": a whole number 0-" + std::to_string(static_cast<uint64_t>(most)) +
                                       (scale > 1u ? " in steps of " + std::to_string(scale) : ""));
                    return 0u;
                }
                return static_cast<uint32_t>(v->number) / scale;
            };
            const vendetta_mods::Json *styles = j.find("styles");
            bits.put(number(styles, "\"styles\": ", "stance", 4), 4);
            bits.put(number(styles, "\"styles\": ", "block and dodge", 3), 3);
            bits.put(number(styles, "\"styles\": ", "run", 2), 2);
            bits.put(number(styles, "\"styles\": ", "pin", 3), 3);
            bits.put(number(styles, "\"styles\": ", "reversal and parry", 2), 2);
            bits.put(number(styles, "\"styles\": ", "knockdown chance (never used)", 2), 2);
            bits.put(number(styles, "\"styles\": ", "dive reach", 3), 3);
            bits.put(number(styles, "\"styles\": ", "never read", 2), 2);
            bits.put(number(styles, "\"styles\": ", "grab reach", 2), 2);
            static const char *const kAreas[] = {"head", "body", "arms", "legs", "whole body"};
            for (const char *group : {"attack", "defence"})
                for (const char *area : kAreas)
                    bits.put(number(j.find(group), std::string("\"") + group + "\": ", area, 4), 4);
            const vendetta_mods::Json *stats = j.find("stats");
            for (const char *stat : {"power", "speed", "grapple", "defense", "stamina", "charisma"})
                bits.put(number(stats, "\"stats\": ", stat, 4), 4);
            const vendetta_mods::Json *computer = j.find("computer");
            const auto list = [&](const char *key, const std::vector<uint32_t> &widths)
            {
                const vendetta_mods::Json *a = computer ? computer->find(key) : nullptr;
                if (!a || !a->isArray() || a->items.size() != widths.size())
                {
                    problems.push_back(std::string("\"computer\": \"") + key + "\": a list of " + std::to_string(widths.size()) + " numbers");
                    for (uint32_t w : widths)
                        bits.put(0u, w);
                    return;
                }
                for (size_t k = 0; k < widths.size(); ++k)
                {
                    const double v = a->items[k].number;
                    const bool good = isNumber(&a->items[k]) && v >= 0 && v == std::floor(v) && v < static_cast<double>(1u << widths[k]);
                    if (!good)
                        problems.push_back(std::string("\"computer\": \"") + key + "\" item " + std::to_string(k + 1u) +
                                           ": a whole number 0-" + std::to_string((1u << widths[k]) - 1u));
                    bits.put(good ? static_cast<uint32_t>(v) : 0u, widths[k]);
                }
            };
            list("personality (the game replaces these)", {2, 2, 7, 3, 3, 7, 3, 2, 2, 3, 3, 2});
            list("skill in story", {7, 7, 7});
            list("skill in other modes", {7, 7, 7});
            const vendetta_mods::Json *flags = j.find("flags");
            static const char *const kFlags[] = {"man (never read)", "woman get-up animations", "state 94 other animation",
                                                 "ring in and out: athletic A", "ring in and out: heavy", "ring out: athletic B",
                                                 "ring out: athletic C", "ring in and out: woman", "fewer running grabs",
                                                 "story: KO never counts", "story: never taps out"};
            uint32_t flagBits = 0;
            for (uint32_t b = 0; b < 11u; ++b)
            {
                const vendetta_mods::Json *v = flags ? flags->find(kFlags[b]) : nullptr;
                if (!v || v->type != vendetta_mods::Json::Type::Bool)
                    problems.push_back(std::string("\"flags\": \"") + kFlags[b] + "\": true or false");
                else if (v->boolean)
                    flagBits |= 1u << b;
            }
            flagBits |= number(flags, "\"flags\": ", "bits 0x800-0x8000 (never read)", 5) << 11;
            bits.put(flagBits, 16);
            const vendetta_mods::Json *combos = j.find("combos");
            if (!combos || !combos->isArray() || combos->items.size() != 5u)
                problems.push_back("\"combos\": a list of 5 (an unused one: name \"\", steps [], points 0, momentum 0)");
            for (size_t c = 0; c < 5u; ++c)
            {
                const vendetta_mods::Json *combo = combos && combos->isArray() && c < combos->items.size() ? &combos->items[c] : nullptr;
                const std::string where = "\"combos\" " + std::to_string(c + 1u) + ": ";
                const vendetta_mods::Json *steps = combo ? combo->find("steps") : nullptr;
                if (!steps || !steps->isArray() || steps->items.size() > 5u)
                    problems.push_back(where + "\"steps\": a list of up to 5 (a slot 1-127, or a shared move 10000-10127)");
                for (size_t k = 0; k < 5u; ++k)
                {
                    uint32_t v = 0;
                    if (steps && steps->isArray() && k < steps->items.size())
                    {
                        const double n = steps->items[k].number;
                        if (isNumber(&steps->items[k]) && n >= 1 && n <= 127 && n == std::floor(n))
                            v = static_cast<uint32_t>(n);
                        else if (isNumber(&steps->items[k]) && n >= 10000 && n <= 10127 && n == std::floor(n))
                            v = static_cast<uint32_t>(n) - 10000u + 128u; // the game adds 0x2690 back (0x11D13C)
                        else
                            problems.push_back(where + "step " + std::to_string(k + 1u) + ": a slot 1-127 or a shared move 10000-10127");
                    }
                    bits.put(v, 8);
                }
                bits.put(number(combo, where, "points", 14, 10u), 14);
                bits.put(number(combo, where, "momentum", 7), 7);
                const vendetta_mods::Json *name = combo ? combo->find("name") : nullptr;
                std::string text = name && name->isString() ? name->text : std::string();
                if (!name || !name->isString() || text.size() > 31u)
                    problems.push_back(where + "\"name\": text of up to 31 letters");
                text.resize(32u, '\0');
                for (size_t k = 0; k < 32u; ++k)
                    bits.put(static_cast<uint8_t>(text[k]), 8);
            }
            bits.put(number(&j, "", "never read (the last 23 bits)", 23), 23);
            out = bits.bytes;
            return problems.empty();
        }

        // The number in "game move 56 CEMENT HANDS" (the name after it is for people), or -1.
        int gameMoveNumber(const std::string &value)
        {
            const std::string want = "GAME MOVE ";
            if (value.size() <= want.size() || upper(value.substr(0, want.size())) != want)
                return -1;
            char *end = nullptr;
            const long n = std::strtol(value.c_str() + want.size(), &end, 10);
            return end != value.c_str() + want.size() && n >= 0 && n < 999 ? static_cast<int>(n) : -1;
        }

        // moveset.json: all 128 slots, each "none", "game move <number> <name>", a mod's move by its name, or
        // { "move": ..., "flags": 0-15 } (bits 12-15 of the slot, which the game never reads). This gives the moveset
        // as far as it is known at start-up; a mod's move goes in at each match (vendetta_animations.cpp) and counts as
        // game move 0 until then.
        bool readMoveset(const vendetta_mods::Json &j, std::array<uint16_t, 128> &out, std::vector<std::string> &problems)
        {
            for (uint32_t slot = 0; slot < 128u; ++slot)
            {
                const vendetta_mods::Json *v = j.find(std::to_string(slot));
                const vendetta_mods::Json *move = v && v->isObject() ? v->find("move") : v;
                const vendetta_mods::Json *flags = v && v->isObject() ? v->find("flags") : nullptr;
                if (!move || !move->isString() || move->text.empty() ||
                    (flags && (!isNumber(flags) || flags->number < 0 || flags->number > 15)))
                {
                    problems.push_back("slot " + std::to_string(slot) +
                                       ": \"none\", \"game move <number>\", a mod's move by its name, or { \"move\": ..., \"flags\": 0-15 }");
                    out[slot] = 0u;
                    continue;
                }
                const uint32_t high = flags ? static_cast<uint32_t>(flags->number) << 12 : 0u;
                if (upper(move->text) == "NONE")
                    out[slot] = 0xFFFFu;
                else if (const int n = gameMoveNumber(move->text); n >= 0)
                    out[slot] = static_cast<uint16_t>(high | static_cast<uint32_t>(n));
                else if (upper(move->text.substr(0, 10)) == "GAME MOVE ")
                {
                    problems.push_back("slot " + std::to_string(slot) + ": \"" + move->text + "\": the game's moves are 0-998");
                    out[slot] = 0u;
                }
                else
                    out[slot] = static_cast<uint16_t>(high); // a mod's move: put in at each match
            }
            // the slots the game's code reads without asking FUN_00102ac8: empty = move 4095 or a division by zero
            for (uint32_t slot : {29u, 30u, 31u, 32u, 46u, 47u, 48u, 49u, 50u, 51u, 52u, 53u, 54u, 55u, 56u, 57u, 58u, 59u, 64u})
                if (out[slot] == 0xFFFFu)
                    problems.push_back("slot " + std::to_string(slot) + " can't be \"none\": the game always uses it");
            return problems.empty();
        }

        // The mods' wrestlers: their records, character records, sizes and voices (the tables already copied).
        void readWrestlers()
        {
            for (const vendetta_mods::Mod &mod : vendetta_mods::loadedMods())
            {
                const vendetta_mods::Json *section = mod.manifest.find("new");
                const vendetta_mods::Json *list = section ? section->find("wrestlers") : nullptr;
                if (!list)
                    continue;
                if (!list->isArray())
                {
                    vendetta_mods::problem() << "[wrestlers] " << mod.name << ": \"new\": \"wrestlers\" should be a list of folders" << std::endl;
                    continue;
                }
                for (const vendetta_mods::Json &item : list->items)
                {
                    if (!item.isString())
                        continue;
                    std::error_code ec;
                    // the attires: attires\1, \2, \3 in order, to the first missing or empty one (empty = no file read
                    // here); none = one attire from the wrestler folder's files
                    const auto hasFiles = [&](const std::string &folder)
                    {
                        for (const char *fileName : kAttireFiles)
                            if (std::filesystem::is_regular_file(mod.folder / (folder + "/" + fileName), ec))
                                return true;
                        return std::filesystem::is_directory(mod.folder / (folder + "/textures"), ec);
                    };
                    const auto attireFolder = [&](size_t n) { return item.text + "/attires/" + std::to_string(n); };
                    std::vector<std::string> attireFolders;
                    for (size_t n = 1; n <= kMaxAttireFolders; ++n)
                    {
                        const bool there = std::filesystem::is_directory(mod.folder / attireFolder(n), ec);
                        if (there && hasFiles(attireFolder(n)) && n <= 3u)
                        {
                            attireFolders.push_back(attireFolder(n));
                            continue;
                        }
                        if (there && n <= 3u)
                        {
                            vendetta_mods::problem() << "[wrestlers] " << mod.name << ": " << item.text << ": attires\\" << n
                                                     << " is skipped because it's empty (no files in it), and any after it"
                                                     << std::endl;
                            break;
                        }
                        for (size_t later = n + (n <= 3u ? 1u : 0u); later <= kMaxAttireFolders; ++later)
                            if (std::filesystem::is_directory(mod.folder / attireFolder(later), ec))
                            {
                                vendetta_mods::problem() << "[wrestlers] " << mod.name << ": " << item.text << ": attires\\" << later
                                                         << " is skipped because "
                                                         << (n > 3u ? std::string("3 attires is the most for now")
                                                                    : "there's no attires\\" + std::to_string(n) +
                                                                          " (a new wrestler's attires are numbered 1, 2, 3 with no gaps)")
                                                         << std::endl;
                                break;
                            }
                        break;
                    }
                    if (attireFolders.empty())
                        attireFolders.push_back(std::string());
                    const size_t attires = attireFolders.size();

                    // each attire's wrestler.json: its own folder's, whole, else the wrestler folder's
                    struct Info
                    {
                        std::string name, full, height, weight;
                        float size = 1.0f;
                        uint32_t looksBase = 0, looksCount = 0, voiceBase = 0, voiceCount = 0;
                        bool ownModel = false;
                    };
                    std::vector<Info> infos(attires);
                    std::map<std::string, vendetta_mods::Json> jsons; // by file (an attire without its own: the main one)
                    const auto modelIn = [&](const std::string &folder)
                    {
                        for (const char *modelName : kModelNames)
                            if (std::filesystem::is_regular_file(mod.folder / (folder + "/" + modelName), ec))
                                return true;
                        return false;
                    };
                    bool anyModel = false;
                    for (size_t k = 0; k < attires; ++k)
                        anyModel = anyModel || modelIn(item.text) || (!attireFolders[k].empty() && modelIn(attireFolders[k]));
                    bool good = true;
                    std::set<std::string> told; // a file's notes once
                    for (size_t k = 0; k < attires && good; ++k)
                    {
                        const std::string folder =
                            !attireFolders[k].empty() &&
                                    std::filesystem::is_regular_file(mod.folder / (attireFolders[k] + "/wrestler.json"), ec)
                                ? attireFolders[k]
                                : item.text;
                        const std::string where = folder + "/wrestler.json";
                        if (!jsons.count(where))
                        {
                            const std::filesystem::path file = vendetta_mods::modFile(mod, where, "wrestler");
                            std::vector<uint8_t> bytes;
                            vendetta_mods::Json j;
                            std::string error;
                            if (file.empty() || !vendetta_mods::readWhole(file, bytes) ||
                                !vendetta_mods::parseJson(std::string(bytes.begin(), bytes.end()), j, error) || !j.isObject())
                            {
                                vendetta_mods::problem() << "[wrestlers] " << mod.name << ": " << where << " can't be read"
                                                         << (error.empty() ? "" : ": " + error) << " - left out" << std::endl;
                                good = false;
                                break;
                            }
                            if (j.find("attires"))
                                vendetta_mods::problem() << "[wrestlers] " << mod.name << ": " << where
                                                         << ": \"attires\" isn't read: the attires are the folders attires\\1-3"
                                                         << std::endl;
                            jsons.emplace(where, std::move(j));
                        }
                        const vendetta_mods::Json &j = jsons.at(where);
                        Info &info = infos[k];
                        info.name = text(j, "name");
                        info.full = text(j, "full name", info.name);
                        info.height = text(j, "height");
                        info.weight = text(j, "weight");
                        if (info.name.empty())
                        {
                            vendetta_mods::problem() << "[wrestlers] " << mod.name << ": " << where << ": no \"name\" - left out" << std::endl;
                            good = false;
                            break;
                        }
                        // a model of its own (the attire's folder, else the wrestler's): Briggs' model always stands
                        // behind it, whatever "based on" says (only for the game's own work: its bones posed, its
                        // shadow; and the pictures when the mod brings none, and the whole model if the mod's can't be
                        // used). An attire without one, beside attires with one, also gets Briggs'.
                        info.ownModel = modelIn(item.text) || (!attireFolders[k].empty() && modelIn(attireFolders[k]));
                        const std::string named = text(j, "based on", "");
                        const std::string basedOn = info.ownModel || (anyModel && named.empty()) ? std::string("Briggs") : named;
                        if (info.ownModel && !named.empty() && upper(named) != "BRIGGS" && told.insert(where + "#based").second)
                            vendetta_mods::problem() << "[wrestlers] " << mod.name << ": " << info.name << ": \"based on\": \"" << named
                                                     << "\" isn't used: with a model of its own, Briggs' model stands behind it"
                                                     << std::endl;
                        if (!gameWrestler(basedOn, info.looksBase, info.looksCount))
                        {
                            vendetta_mods::problem() << "[wrestlers] " << mod.name << ": " << info.name
                                                     << ": \"based on\" must name a game wrestler (\"" << basedOn << "\" isn't one) - left out"
                                                     << std::endl;
                            good = false;
                            break;
                        }
                        const std::string soundsLike = text(j, "sounds like", basedOn);
                        if (!gameWrestler(soundsLike, info.voiceBase, info.voiceCount))
                        {
                            vendetta_mods::problem() << "[wrestlers] " << mod.name << ": " << info.name
                                                     << ": \"sounds like\" must name a game wrestler (\"" << soundsLike << "\" isn't one) - left out"
                                                     << std::endl;
                            good = false;
                            break;
                        }
                        const std::string sizeText = upper(text(j, "size", "normal"));
                        if (sizeText == "SHORT")
                            info.size = 0.95f;
                        else if (sizeText == "TALL")
                            info.size = 1.05f;
                        else if (sizeText != "NORMAL" && told.insert(where + "#size").second)
                            vendetta_mods::problem() << "[wrestlers] " << mod.name << ": " << info.name
                                                     << ": \"size\" is short, normal or tall - normal used" << std::endl;
                    }
                    if (!good)
                        continue;
                    const std::string name = infos[0].name, full = infos[0].full;

                    // each attire's fight values and moveset: its own folder's fight.json / moveset.json, else the wrestler's
                    std::vector<std::array<uint8_t, kWparSize>> fights(attires);
                    std::vector<std::array<uint16_t, 128>> movesets(attires);
                    std::vector<std::filesystem::path> movesetFiles(attires);
                    std::set<std::filesystem::path> toldFiles; // a file shared by the attires: its problems once
                    for (size_t k = 0; k < attires; ++k)
                    {
                        const std::string &attireFolder = attireFolders[k];
                        for (const std::string fileName : {"fight.json", "moveset.json"})
                        {
                            const bool own = !attireFolder.empty() &&
                                             std::filesystem::is_regular_file(mod.folder / (attireFolder + "/" + fileName), ec);
                            const std::filesystem::path path =
                                vendetta_mods::modFile(mod, (own ? attireFolder : item.text) + "/" + fileName, "wrestler");
                            vendetta_mods::Json data;
                            std::string why;
                            std::vector<std::string> problems;
                            if (!readJson(path, data, why))
                                problems.push_back(why);
                            else if (fileName == "fight.json")
                                packFight(data, fights[k], problems);
                            else
                            {
                                readMoveset(data, movesets[k], problems);
                                movesetFiles[k] = path;
                            }
                            if (toldFiles.insert(path).second)
                                for (const std::string &problem : problems)
                                    std::cerr << "[wrestlers] " << mod.name << ": " << infos[k].name << ": "
                                              << (path.empty() ? fileName : path.filename().string()) << ": " << problem << std::endl;
                            good = good && problems.empty();
                        }
                    }
                    if (!good)
                    {
                        vendetta_mods::problem() << "[wrestlers] " << mod.name << ": " << name << " left out (every value must be in its files)" << std::endl;
                        continue;
                    }
                    Wrestler w;
                    w.modName = mod.name;
                    w.name = name;
                    w.names.insert(upper(item.text.substr(item.text.find_last_of("/\\") + 1)));
                    for (uint32_t k = 0; k < attires; ++k)
                    {
                        const Info &info = infos[k];
                        w.names.insert(upper(info.name));
                        w.names.insert(upper(info.full));
                        const uint32_t record = g_endRecord++;
                        const uint32_t looks = info.looksBase + std::min(k, info.looksCount - 1u);
                        const uint32_t entry = vendetta_store::kCharacterTable + kRecordSize * record;
                        std::memcpy(guest(entry), guest(kGameCharacters + kRecordSize * looks), kRecordSize);
                        write32(entry + 0x00u, guestText(info.full));
                        write32(entry + 0x04u, guestText(info.name));
                        write32(entry + 0x08u, guestText(info.height));
                        write32(entry + 0x0Cu, guestText(info.weight));
                        write32(entry + 0x18u, kFirstLogoIndex + record - kFirstNewRecord);
                        // its pictures: the attire's folder first, then the wrestler's
                        AttirePictures pictures;
                        pictures.gameRender = read32(entry + 0x10u);
                        pictures.gameIcon = read32(entry + 0x14u);
                        const auto picture = [&](const char *fileName) -> std::filesystem::path
                        {
                            std::error_code ec;
                            for (const std::string &folder : {attireFolders[k], item.text})
                                if (!folder.empty() && std::filesystem::is_regular_file(mod.folder / (folder + "/" + fileName), ec))
                                    return vendetta_mods::modFile(mod, folder + "/" + fileName, "picture");
                            return {};
                        };
                        const uint32_t pictureNumber = kPictureBase + record - kFirstNewRecord;
                        if (const std::filesystem::path png = picture("render.png"); !png.empty())
                        {
                            pictures.render = vendetta_pictures::add(png, 128u, 256u, 4u);
                            write32(entry + 0x10u, pictureNumber);
                        }
                        if (const std::filesystem::path png = picture("icon.png"); !png.empty())
                        {
                            pictures.icon = vendetta_pictures::add(png, 64u, 64u, 4u);
                            pictures.shade = vendetta_pictures::add(png, 64u, 64u, 4u, vendetta_pictures::Kind::Shadow);
                            write32(entry + 0x14u, pictureNumber);
                            if (k == 0)
                            {
                                // the grid shows every wrestler's first attire: read those from the start
                                vendetta_pictures::want(pictures.icon);
                                vendetta_pictures::want(pictures.shade);
                            }
                        }
                        if (const std::filesystem::path png = picture("logo.png"); !png.empty())
                            pictures.logo = vendetta_pictures::add(png, 128u, 64u, 4u);
                        g_attirePictures.push_back(pictures);
                        // its own model: the attire's folder first, then the wrestler's; its textures folder likewise
                        {
                            ModelFile model;
                            model.modName = mod.name;
                            model.wrestler = info.name;
                            for (const std::string &folder : {attireFolders[k], item.text})
                            {
                                if (folder.empty())
                                    continue;
                                if (model.model.empty())
                                    for (const char *modelName : kModelNames)
                                        if (std::filesystem::is_regular_file(mod.folder / (folder + "/" + modelName), ec))
                                        {
                                            model.model = mod.folder / (folder + "/" + modelName);
                                            break;
                                        }
                                if (model.textures.empty() && std::filesystem::is_directory(mod.folder / (folder + "/textures"), ec))
                                    model.textures = mod.folder / (folder + "/textures");
                            }
                            if (!model.model.empty())
                                std::cerr << "[wrestlers] " << mod.name << ": " << info.name << " (record " << record << "): its own model "
                                          << model.model.filename().string() << std::endl;
                            else if (anyModel)
                                vendetta_mods::problem() << "[wrestlers] " << mod.name << ": " << info.name << ": attire " << k + 1
                                                         << " has no model of its own (another attire has): Briggs' model shows"
                                                         << std::endl;
                            g_modelFiles.push_back(model);
                        }
                        float sizeValue = info.size;
                        std::memcpy(guest(vendetta_store::kSizeClasses + 4u * record), &sizeValue, 4);
                        write32(vendetta_store::kVoiceMap + 4u * record, read32(kGameVoices + 4u * info.voiceBase));
                        w.records.push_back(record);
                        // the fight values and the moveset, each after the tag and size the game's own blocks have
                        const uint32_t wpar = vendetta_store::allocate(vendetta_store::Area::Boot, 8u + kWparSize, "new wrestlers' fight values");
                        const uint32_t wwaz = vendetta_store::allocate(vendetta_store::Area::Boot, kMovesetSize, "new wrestlers' movesets");
                        if (wpar)
                        {
                            std::memcpy(guest(wpar), "WPAR", 4);
                            write32(wpar + 4u, kWparSize);
                            std::memcpy(guest(wpar + 8u), fights[k].data(), kWparSize);
                        }
                        if (wwaz)
                        {
                            std::memcpy(guest(wwaz), "WWAZ", 4);
                            write32(wwaz + 4u, 256u);
                            std::memcpy(guest(wwaz + 8u), movesets[k].data(), 256u);
                        }
                        w.wpar.push_back(wpar);
                        w.wwaz.push_back(wwaz);
                        g_slotFiles.push_back({mod.name, info.name, movesetFiles[k], {record}});
                    }
                    g_newAttires.push_back(static_cast<uint32_t>(attires));
                    g_firstAttires.push_back(w.records.front() - kFirstNewRecord);
                    std::cerr << "[wrestlers] " << mod.name << ": " << full << " (" << name << "), records " << w.records.front()
                              << "-" << w.records.back() << " (" << attires << " attire" << (attires == 1 ? "" : "s")
                              << "), looks like " << guestString(read32(kGameCharacters + kRecordSize * infos[0].looksBase))
                              << ", sounds like " << guestString(read32(kGameCharacters + kRecordSize * infos[0].voiceBase)) << std::endl;
                    g_wrestlers.push_back(std::move(w));
                }
            }
        }

        // A folder holds something an attire uses
        bool hasAttireFiles(const vendetta_mods::Mod &mod, const std::string &folder)
        {
            std::error_code ec;
            for (const char *fileName : kAttireFiles)
                if (std::filesystem::is_regular_file(mod.folder / (folder + "/" + fileName), ec))
                    return true;
            return std::filesystem::is_directory(mod.folder / (folder + "/textures"), ec);
        }

        // The mods' "replace": { "wrestlers": { "<game wrestler>": "<folder>" } }: each attire folder (attires\N, any of
        // the game wrestler's own attires in any mix; the folder itself, when it has none, is attire 1) replaces that
        // attire. Each file found (the attire's folder first, then the folder's) replaces that part; what it doesn't
        // bring stays the game's own. One mod per attire: the first in the load order. Written into the attire's own
        // record in the store's copies, its story row in place (names, stats, pictures), its move data at the first match.
        void readReplacements()
        {
            for (const vendetta_mods::Mod &mod : vendetta_mods::loadedMods())
            {
                const vendetta_mods::Json *section = mod.manifest.find("replace");
                const vendetta_mods::Json *list = section ? section->find("wrestlers") : nullptr;
                if (!list)
                    continue;
                if (!list->isObject())
                {
                    vendetta_mods::problem() << "[wrestlers] " << mod.name
                                             << ": \"replace\": \"wrestlers\" should be { \"<game wrestler>\": \"<folder>\" }" << std::endl;
                    continue;
                }
                for (const auto &[gameName, folderValue] : list->members)
                {
                    const std::string label = "[wrestlers] " + mod.name + ": " + gameName;
                    std::error_code ec;
                    if (!folderValue.isString() || folderValue.text.empty() ||
                        !std::filesystem::is_directory(mod.folder / folderValue.text, ec))
                    {
                        vendetta_mods::problem() << label << ": the replacement's folder isn't in the mod - skipped" << std::endl;
                        continue;
                    }
                    const std::string folder = folderValue.text;
                    // the game wrestler: his first record by the game's own names, his attires by his unlock row
                    const std::string want = upper(gameName);
                    uint32_t base = kMovesetRecords;
                    for (uint32_t r = 0; r < kMovesetRecords && base == kMovesetRecords; ++r)
                        if (g_gameNames[r].first == want || g_gameNames[r].second == want)
                            base = r;
                    if (base == kMovesetRecords)
                    {
                        vendetta_mods::problem() << label << ": the game has no wrestler called that - skipped" << std::endl;
                        continue;
                    }
                    uint32_t count = 0;
                    while (count < 3u && static_cast<int32_t>(read32(kUnlockTable + 12u * base + 4u * count)) >= 0)
                        ++count;
                    count = std::max(count, 1u);
                    std::vector<std::pair<uint32_t, std::string>> spots; // (attire 1-3, its folder; "" = the folder itself)
                    std::vector<std::pair<uint32_t, std::string>> added; // attires he doesn't have, after his own
                    bool anyAttireFolder = false;
                    for (uint32_t n = 1; n <= kMaxAttireFolders; ++n)
                    {
                        const std::string attire = folder + "/attires/" + std::to_string(n);
                        if (!std::filesystem::is_directory(mod.folder / attire, ec))
                            continue;
                        anyAttireFolder = true;
                        if (!hasAttireFiles(mod, attire))
                            vendetta_mods::problem() << label << ": attires\\" << n << " is skipped because it's empty (no files in it)"
                                                     << std::endl;
                        else if (n <= count)
                            spots.emplace_back(n, attire);
                        else if (n > 3u)
                            vendetta_mods::problem() << label << ": attires\\" << n << " is skipped because 3 attires is the most for now"
                                                     << std::endl;
                        else if (n != count + added.size() + 1u)
                            vendetta_mods::problem() << label << ": attires\\" << n << " is skipped because there's no attires\\"
                                                     << count + added.size() + 1u << " (" << gameName << " has " << count
                                                     << " in the game; added attires follow on with no gaps)" << std::endl;
                        else
                            added.emplace_back(n, attire);
                    }
                    if (!anyAttireFolder)
                        spots.emplace_back(1u, std::string()); // no attire folders: the folder itself is attire 1
                    // one attire, replaced (record = his own) or added (a new record, no story row)
                    const auto fill = [&](uint32_t n, const std::string &attireFolder, uint32_t record, bool isAdded) -> bool
                    {
                        const std::string here = label + " attire " + std::to_string(n);
                        if (const auto was = g_replaced.find(record); !isAdded && was != g_replaced.end())
                        {
                            vendetta_mods::problem() << here << " is skipped: " << was->second.modName
                                                     << " already replaces it (earlier in the load order)" << std::endl;
                            return false;
                        }
                        const auto find = [&](const char *fileName) -> std::string
                        {
                            for (const std::string &f : {attireFolder, folder})
                                if (!f.empty() && std::filesystem::is_regular_file(mod.folder / (f + "/" + fileName), ec))
                                    return f + "/" + fileName;
                            return {};
                        };
                        Replaced rep;
                        rep.modName = mod.name;
                        rep.gameName = gameName;
                        const uint32_t entry = vendetta_store::kCharacterTable + kRecordSize * record;
                        rep.unlockIndex = read32(entry + 0x18u);
                        const uint32_t row =
                            !isAdded && rep.unlockIndex < kStoryRowCount ? kStoryRows + kStoryRowSize * rep.unlockIndex : 0u;
                        std::vector<std::string> parts;
                        std::string shown = g_gameNames[base].second;
                        bool identity = false;
                        // wrestler.json: names, height, weight, size, voice, the story's comment (a value it leaves out
                        // stays the game's)
                        if (const std::string path = find("wrestler.json"); !path.empty())
                        {
                            vendetta_mods::Json j;
                            std::string why;
                            if (!readJson(vendetta_mods::modFile(mod, path, "wrestler"), j, why))
                                vendetta_mods::problem() << here << ": " << path << ": " << why
                                                         << " - the game's names, size and voice stay" << std::endl;
                            else
                            {
                                identity = true;
                                parts.push_back("names");
                                const auto put = [&](const char *key, uint32_t field, uint32_t rowField, const std::string &otherwise = {})
                                {
                                    const std::string value = text(j, key, otherwise);
                                    if (value.empty())
                                        return;
                                    const uint32_t address = guestText(value);
                                    write32(entry + field, address);
                                    if (row && rowField != ~0u)
                                        write32(row + rowField, address);
                                };
                                const std::string name = text(j, "name");
                                put("name", 0x04u, 0x08u);
                                put("full name", 0x00u, 0x04u, name);
                                put("height", 0x08u, 0x0Cu);
                                put("weight", 0x0Cu, 0x10u);
                                if (const std::string comment = text(j, "story comment"); !comment.empty() && row)
                                    write32(row, guestText(comment));
                                if (!name.empty())
                                {
                                    shown = name;
                                    rep.names.insert(upper(name));
                                    rep.names.insert(upper(text(j, "full name", name)));
                                }
                                if (const std::string sizeText = upper(text(j, "size")); !sizeText.empty())
                                {
                                    const float size = sizeText == "SHORT" ? 0.95f : sizeText == "TALL" ? 1.05f : 1.0f;
                                    if (sizeText != "SHORT" && sizeText != "TALL" && sizeText != "NORMAL")
                                        vendetta_mods::problem() << here << ": \"size\" is short, normal or tall - normal used" << std::endl;
                                    std::memcpy(guest(vendetta_store::kSizeClasses + 4u * record), &size, 4);
                                }
                                if (const std::string soundsLike = text(j, "sounds like"); !soundsLike.empty())
                                {
                                    uint32_t voiceBase = 0, voiceCount = 0;
                                    if (gameWrestler(soundsLike, voiceBase, voiceCount))
                                        write32(vendetta_store::kVoiceMap + 4u * record, read32(kGameVoices + 4u * voiceBase));
                                    else
                                        vendetta_mods::problem() << here << ": \"sounds like\" must name a game wrestler (\"" << soundsLike
                                                                 << "\" isn't one) - the game's voice stays" << std::endl;
                                }
                            }
                        }
                        // fight.json: its fight values, and the story's copy of its stats (x 10: story opponents fight
                        // with these)
                        if (const std::string path = find("fight.json"); !path.empty())
                        {
                            vendetta_mods::Json data;
                            std::string why;
                            std::vector<std::string> problems;
                            std::array<uint8_t, kWparSize> fight{};
                            if (!readJson(vendetta_mods::modFile(mod, path, "wrestler"), data, why))
                                problems.push_back(why);
                            else
                                packFight(data, fight, problems);
                            for (const std::string &problem : problems)
                                vendetta_mods::problem() << here << ": " << path << ": " << problem << std::endl;
                            if (!problems.empty())
                                vendetta_mods::problem() << here << ": the game's fight values stay" << std::endl;
                            else if ((rep.wpar = vendetta_store::allocate(vendetta_store::Area::Boot, 8u + kWparSize, "replaced attires' fight values")))
                            {
                                std::memcpy(guest(rep.wpar), "WPAR", 4);
                                write32(rep.wpar + 4u, kWparSize);
                                std::memcpy(guest(rep.wpar + 8u), fight.data(), kWparSize);
                                parts.push_back("fight values");
                                if (row)
                                    if (const vendetta_mods::Json *stats = data.find("stats"))
                                        for (uint32_t k = 0; k < 6u; ++k)
                                            if (const vendetta_mods::Json *v = stats->find(kStats[k]); isNumber(v))
                                                write32(row + 0x14u + 4u * k, static_cast<uint32_t>(v->number * 10.0));
                            }
                        }
                        // moveset.json: its moves (a mod's moves by name go in at each match; edits go on top)
                        if (const std::string path = find("moveset.json"); !path.empty())
                        {
                            vendetta_mods::Json data;
                            std::string why;
                            std::vector<std::string> problems;
                            std::array<uint16_t, 128> moveset{};
                            const std::filesystem::path file = vendetta_mods::modFile(mod, path, "wrestler");
                            if (!readJson(file, data, why))
                                problems.push_back(why);
                            else
                                readMoveset(data, moveset, problems);
                            for (const std::string &problem : problems)
                                vendetta_mods::problem() << here << ": " << path << ": " << problem << std::endl;
                            if (!problems.empty())
                                vendetta_mods::problem() << here << ": the game's moves stay" << std::endl;
                            else if ((rep.wwaz = vendetta_store::allocate(vendetta_store::Area::Boot, kMovesetSize, "replaced attires' movesets")))
                            {
                                std::memcpy(guest(rep.wwaz), "WWAZ", 4);
                                write32(rep.wwaz + 4u, 256u);
                                std::memcpy(guest(rep.wwaz + 8u), moveset.data(), 256u);
                                rep.slotFile = g_slotFiles.size();
                                g_slotFiles.push_back({mod.name, shown, file, {record}});
                                parts.push_back("moves");
                            }
                        }
                        // the pictures: drawn from the mod's files over the game's stand-ins, as a new wrestler's
                        const std::string render = find("render.png"), icon = find("icon.png"), logo = find("logo.png");
                        if (!render.empty() || !icon.empty() || !logo.empty() || (isAdded && identity))
                        {
                            AttirePictures pictures;
                            pictures.gameRender = read32(entry + 0x10u);
                            pictures.gameIcon = read32(entry + 0x14u);
                            rep.pictures = static_cast<uint32_t>(g_attirePictures.size());
                            const uint32_t number = kPictureBase + rep.pictures;
                            if (!render.empty())
                            {
                                pictures.render = vendetta_pictures::add(vendetta_mods::modFile(mod, render, "picture"), 128u, 256u, 4u);
                                write32(entry + 0x10u, number);
                                if (row)
                                    write32(row + 0x30u, number);
                            }
                            if (!icon.empty())
                            {
                                const std::filesystem::path png = vendetta_mods::modFile(mod, icon, "picture");
                                pictures.icon = vendetta_pictures::add(png, 64u, 64u, 4u);
                                pictures.shade = vendetta_pictures::add(png, 64u, 64u, 4u, vendetta_pictures::Kind::Shadow);
                                write32(entry + 0x14u, number);
                                if (row)
                                    write32(row + 0x34u, number);
                                if (n == 1u)
                                {
                                    // the grid shows every wrestler's first attire: read it from the start
                                    vendetta_pictures::want(pictures.icon);
                                    vendetta_pictures::want(pictures.shade);
                                }
                            }
                            if (!logo.empty())
                                pictures.logo = vendetta_pictures::add(vendetta_mods::modFile(mod, logo, "picture"), 128u, 64u, 4u);
                            // attire 1's render and logo read from the start too (Choose Fighter opens with the cursor
                            // on Briggs: his replacement's picture is ready at the first draw, as the icons are)
                            for (uint32_t id : {pictures.render, pictures.logo})
                                if (id && n == 1u)
                                    vendetta_pictures::want(id);
                            g_attirePictures.push_back(pictures);
                            identity = identity || !render.empty() || !icon.empty();
                            if (!render.empty() || !icon.empty() || !logo.empty())
                                parts.push_back("pictures");
                            if (isAdded)
                            {
                                // an added attire's logo by its own index (past the game's 60: the logo hook answers it)
                                rep.unlockIndex = kFirstLogoIndex + rep.pictures;
                                write32(entry + 0x18u, rep.unlockIndex);
                            }
                        }
                        rep.noLogo = identity && logo.empty();
                        // the model: the attire's folder first, then the folder's; its textures folder likewise
                        rep.model.modName = mod.name;
                        rep.model.wrestler = shown;
                        for (const std::string &f : {attireFolder, folder})
                        {
                            if (f.empty())
                                continue;
                            if (rep.model.model.empty())
                                for (const char *modelName : kModelNames)
                                    if (std::filesystem::is_regular_file(mod.folder / (f + "/" + modelName), ec))
                                    {
                                        rep.model.model = mod.folder / (f + "/" + modelName);
                                        break;
                                    }
                            if (rep.model.textures.empty() && std::filesystem::is_directory(mod.folder / (f + "/textures"), ec))
                                rep.model.textures = mod.folder / (f + "/textures");
                        }
                        if (!rep.model.model.empty())
                            parts.push_back("model " + rep.model.model.filename().string());
                        if (parts.empty() && !isAdded)
                        {
                            vendetta_mods::problem() << here << ": nothing in the folder replaces anything - skipped" << std::endl;
                            return false;
                        }
                        std::string joined;
                        for (const std::string &part : parts)
                            joined += (joined.empty() ? "" : ", ") + part;
                        std::cerr << here << " (record " << record << ") " << (isAdded ? "added" : "replaced") << ": "
                                  << (joined.empty() ? std::string("as attire 1") : joined) << std::endl;
                        g_replaced.emplace(record, std::move(rep));
                        return true;
                    };
                    for (const auto &[n, attireFolder] : spots)
                        fill(n, attireFolder, base + n - 1u, false);
                    if (added.empty())
                        continue;
                    // attires he doesn't have: his slot moves to new records, his own attires copied first
                    uint32_t slot = kGameSlots;
                    for (uint32_t s = 0; s < kGameSlots && slot == kGameSlots; ++s)
                        if (read32(vendetta_store::kSelectGrid + 8u * s) == base)
                            slot = s;
                    if (slot == kGameSlots || std::any_of(g_moved.begin(), g_moved.end(), [&](const Moved &m) { return m.base == base; }))
                    {
                        vendetta_mods::problem() << label << ": the added attires are skipped ("
                                                 << (slot == kGameSlots ? "he has no Choose Fighter slot" : "another mod already adds his")
                                                 << ")" << std::endl;
                        continue;
                    }
                    Moved m;
                    m.base = base;
                    m.gameCount = count;
                    m.slot = slot;
                    m.first = g_endRecord;
                    const auto newRecord = [&](uint32_t from)
                    {
                        const uint32_t record = g_endRecord++;
                        std::memcpy(guest(vendetta_store::kCharacterTable + kRecordSize * record),
                                    guest(vendetta_store::kCharacterTable + kRecordSize * from), kRecordSize);
                        write32(vendetta_store::kSizeClasses + 4u * record, read32(vendetta_store::kSizeClasses + 4u * from));
                        write32(vendetta_store::kVoiceMap + 4u * record, read32(vendetta_store::kVoiceMap + 4u * from));
                        ModelFile model; // g_modelFiles stays one entry per record from 63
                        if (const auto rep = g_replaced.find(from); rep != g_replaced.end())
                        {
                            model = rep->second.model;
                            if (rep->second.slotFile != SIZE_MAX)
                                g_slotFiles[rep->second.slotFile].records.push_back(record);
                        }
                        g_modelFiles.push_back(model);
                        return record;
                    };
                    for (uint32_t k = 0; k < count; ++k)
                    {
                        newRecord(base + k);
                        m.sources.push_back(base + k);
                    }
                    for (const auto &[n, attireFolder] : added)
                    {
                        const uint32_t record = newRecord(m.first);
                        m.sources.push_back(base);
                        fill(n, attireFolder, record, true);
                        // what it doesn't bring comes from his attire 1 as it now is (replaced or the game's): the
                        // names, pictures, size, voice and move data were copied with the record; a mod model and its
                        // per-match moveset file here
                        const auto first = g_replaced.find(base);
                        if (const auto own = g_replaced.find(record); own != g_replaced.end() && first != g_replaced.end())
                        {
                            if (own->second.model.model.empty())
                                own->second.model = first->second.model;
                            if (!own->second.wwaz && first->second.slotFile != SIZE_MAX)
                                g_slotFiles[first->second.slotFile].records.push_back(record);
                        }
                    }
                    m.count = g_endRecord - m.first;
                    write32(vendetta_store::kSelectGrid + 8u * slot, m.first);
                    std::cerr << label << ": " << m.count << " attires on Choose Fighter (records " << m.first << "-"
                              << g_endRecord - 1u << "; Story and Survival keep his own)" << std::endl;
                    g_moved.push_back(std::move(m));
                }
            }
        }

        // The tables the game's code reads, copied (with room) and the mods' wrestlers added; Choose Fighter's size.
        void buildTables()
        {
            const uint32_t room = 0x00900000u - (vendetta_store::kCharacterTable - vendetta_store::kFixedArea);
            if (!vendetta_store::fixed(vendetta_store::kCharacterTable - vendetta_store::kFixedArea, room, "roster tables"))
                return;
            std::memcpy(guest(vendetta_store::kCharacterTable), guest(kGameCharacters), kRecordSize * kGameRecords);
            std::memcpy(guest(vendetta_store::kSelectGrid), guest(kGameGrid), 8u * kGameSlots);
            std::memcpy(guest(vendetta_store::kSizeClasses), guest(kGameSizes), 4u * kGameRecords);
            std::memcpy(guest(vendetta_store::kVoiceMap), guest(kGameVoices), 4u * kGameRecords);
            g_gameNames.clear();
            for (uint32_t record = 0; record < kGameRecords; ++record)
                g_gameNames.emplace_back(upper(guestString(read32(kGameCharacters + kRecordSize * record))),
                                         upper(guestString(read32(kGameCharacters + kRecordSize * record + 4u))));
            readWrestlers();
            g_newEnd = g_endRecord;
            readReplacements();
            uint32_t slot = kGameSlots;
            for (const Wrestler &w : g_wrestlers)
            {
                write32(vendetta_store::kSelectGrid + 8u * slot, w.records.front());
                write32(vendetta_store::kSelectGrid + 8u * slot + 4u, static_cast<uint32_t>(w.records.size()));
                ++slot;
            }
            if (slot % 2u)
            {
                // an odd count: an empty slot ends the last column (no attires: a locked tile nobody can pick)
                write32(vendetta_store::kSelectGrid + 8u * slot, 0u);
                write32(vendetta_store::kSelectGrid + 8u * slot + 4u, 0u);
                g_newAttires.push_back(0u);
                ++slot;
            }
            g_slots = slot;
            if (const char *poison = std::getenv("VENDETTA_ANIM_POISON"); poison && *poison && *poison != '0')
            {
                // the test that nothing still reads the game's own tables: 0xFF over them (the copies are what counts)
                std::memset(guest(kGameCharacters), 0xFF, kRecordSize * kGameRecords);
                std::memset(guest(kGameGrid), 0xFF, 8u * kGameSlots);
                std::memset(guest(kGameSizes), 0xFF, 4u * kMovesetRecords);
                std::memset(guest(kGameVoices), 0xFF, 4u * kGameRecords);
            }
            write32(kGridSizeWord, g_slots);
            write32(kLastScrollWord, g_slots - kWindow);
            if (!g_wrestlers.empty())
                std::cerr << "[wrestlers] " << g_wrestlers.size() << " new wrestler(s), records " << kFirstNewRecord << "-"
                          << g_endRecord - 1u << "; Choose Fighter has " << g_slots << " slots" << std::endl;
        }

        // The move data: the game's boot fills it in place (its loader builds the address through a patched lui, so the
        // struct is read at boot into the store's copy and the original stays empty). Once it holds the moves (checked
        // at every match load and init until then): records 59 to 62 take record 0's entry (the match setup turns them
        // into Briggs anyway); each new attire its own fight values and moveset, pointed at the way the game points at
        // its own (at the payload, or at the tag before it).
        void completeMoveData(uint32_t ra)
        {
            const uint32_t md = vendetta_store::kMoveData;
            if (g_moveDataReady)
                return;
            if (read32(md + 8u) == 0u)
            {
                vendetta_mods::problem() << "[wrestlers] a match began before the move data was read (ra 0x" << std::hex << ra << std::dec
                          << ")" << std::endl;
                return;
            }
            g_moveDataReady = true;
            for (uint32_t r = kMovesetRecords; r < kFirstNewRecord; ++r)
                std::memcpy(guest(md + kPairs + 16u * r), guest(md + kPairs), 16u);
            const bool wparTagged = std::memcmp(guest(read32(md + kPairs + 4u)), "WPAR", 4) == 0;
            const bool wwazTagged = std::memcmp(guest(read32(md + kPairs + 0xCu)), "WWAZ", 4) == 0;
            for (const Wrestler &w : g_wrestlers)
                for (size_t k = 0; k < w.records.size(); ++k)
                {
                    const uint32_t pair = md + kPairs + 16u * w.records[k];
                    std::memcpy(guest(pair), guest(md + kPairs), 16u);
                    if (w.wpar[k])
                        write32(pair + 0x4u, w.wpar[k] + (wparTagged ? 0u : 8u));
                    if (w.wwaz[k])
                        write32(pair + 0xCu, w.wwaz[k] + (wwazTagged ? 0u : 8u));
                }
            const auto replacedPairs = [&](bool added)
            {
                for (const auto &[record, rep] : g_replaced)
                {
                    if ((record >= kFirstNewRecord) != added)
                        continue;
                    const uint32_t pair = md + kPairs + 16u * record;
                    if (rep.wpar)
                        write32(pair + 0x4u, rep.wpar + (wparTagged ? 0u : 8u));
                    if (rep.wwaz)
                        write32(pair + 0xCu, rep.wwaz + (wwazTagged ? 0u : 8u));
                }
            };
            replacedPairs(false);
            for (const Moved &m : g_moved)
                for (uint32_t k = 0; k < m.count; ++k)
                    std::memcpy(guest(md + kPairs + 16u * (m.first + k)), guest(md + kPairs + 16u * m.sources[k]), 16u);
            replacedPairs(true);
            if (!g_wrestlers.empty() || !g_replaced.empty())
                std::cerr << "[wrestlers] the new and replaced attires' fight values and movesets are in (the game points at "
                          << "its own " << (wparTagged ? "tags" : "payloads") << " / " << (wwazTagged ? "tags" : "payloads") << ")"
                          << std::endl;
        }

        void returnToCaller(R5900Context *ctx) { ctx->pc = getRegU32(ctx, 31); }

        uint32_t slotBase(uint32_t slot) { return read32(vendetta_store::kSelectGrid + 8u * slot); }
        uint32_t slotAttires(uint32_t slot) { return read32(vendetta_store::kSelectGrid + 8u * slot + 4u); }

        // FUN_00151418: an earlier player already picked this wrestler (front end +0x34[i] for i < +0x1C).
        bool taken(uint32_t fe, uint32_t base)
        {
            const uint32_t players = read32(fe + 0x1Cu);
            for (uint32_t i = 0; i < players && i < 4u; ++i)
                if (read32(fe + 0x34u + 4u * i) == base)
                    return true;
            return false;
        }

        bool pickable(uint32_t fe, uint32_t slot) { return slot < g_slots && !taken(fe, slotBase(slot)) && slotAttires(slot) != 0u; }

        void recount(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t counts = getRegU32(ctx, 4); // 63 attire counts by base record
            for (uint32_t slot = 0; slot < g_slots; ++slot)
            {
                const uint32_t base = slotBase(slot);
                uint32_t attires = slot < kGameSlots ? (base < kGameRecords ? read32(counts + 4u * base) : 0u)
                                                     : g_newAttires[slot - kGameSlots];
                // a moved slot: his own attires as the save unlocks them; with all of them, the added ones too
                for (const Moved &m : g_moved)
                    if (m.slot == slot)
                    {
                        const uint32_t unlocked = read32(counts + 4u * m.base);
                        attires = unlocked >= m.gameCount ? m.count : unlocked;
                    }
                write32(vendetta_store::kSelectGrid + 8u * slot + 4u, attires);
            }
            returnToCaller(ctx);
        }

        void canUp(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t slot = getRegU32(ctx, 5);
            setReturnU32(ctx, (slot & 1u) && pickable(getRegU32(ctx, 4), slot - 1u) ? 1u : 0u);
            returnToCaller(ctx);
        }

        void canDown(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t slot = getRegU32(ctx, 5);
            setReturnU32(ctx, !(slot & 1u) && pickable(getRegU32(ctx, 4), slot + 1u) ? 1u : 0u);
            returnToCaller(ctx);
        }

        void left(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t fe = getRegU32(ctx, 4);
            const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
            int32_t columns = 0;
            for (int32_t s = slot - 2; s >= 0; s -= 2)
                if (pickable(fe, static_cast<uint32_t>(s)))
                {
                    columns = (slot - s) / 2;
                    break;
                }
            setReturnU32(ctx, static_cast<uint32_t>(columns));
            returnToCaller(ctx);
        }

        void right(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t fe = getRegU32(ctx, 4);
            const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
            int32_t columns = 0;
            for (int32_t s = slot + 2; s < static_cast<int32_t>(g_slots); s += 2)
                if (pickable(fe, static_cast<uint32_t>(s)))
                {
                    columns = (s - slot) / 2;
                    break;
                }
            setReturnU32(ctx, static_cast<uint32_t>(columns));
            returnToCaller(ctx);
        }

        void anyBefore(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t fe = getRegU32(ctx, 4);
            uint32_t found = 0;
            for (int32_t s = static_cast<int32_t>(getRegU32(ctx, 5)) - 1; s >= 0 && !found; --s)
                found = pickable(fe, static_cast<uint32_t>(s)) ? 1u : 0u;
            setReturnU32(ctx, found);
            returnToCaller(ctx);
        }

        void anyAfter(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t fe = getRegU32(ctx, 4);
            uint32_t found = 0;
            for (uint32_t s = getRegU32(ctx, 5) + kWindow; s < g_slots && !found; ++s)
                found = pickable(fe, s) ? 1u : 0u;
            setReturnU32(ctx, found);
            returnToCaller(ctx);
        }

        const AttirePictures *picturesOf(uint32_t number)
        {
            return number >= kPictureBase && number - kPictureBase < g_attirePictures.size()
                       ? &g_attirePictures[number - kPictureBase]
                       : nullptr;
        }

        // FUN_0017c2c0(index): new attires have indexes from 60 (past the game's 60-entry logo table): their own
        // logo's number when they have one, else none.
        void logo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            uint32_t index = getRegU32(ctx, 4);
            bool byRecord = getRegU32(ctx, 31) == kLogoByRecordCall;
            if (byRecord && index >= g_newEnd && index < g_endRecord)
            {
                // a moved slot's record: by its unlock index (the game's, or an added attire's own)
                index = read32(vendetta_store::kCharacterTable + kRecordSize * index + 0x18u);
                byRecord = false;
            }
            if ((byRecord && index >= kFirstNewRecord && index < g_newEnd) ||
                (!byRecord && index >= kFirstLogoIndex && index < kFirstLogoIndex + g_attirePictures.size()))
            {
                const uint32_t attire = index - (byRecord ? kFirstNewRecord : kFirstLogoIndex);
                const bool own = g_attirePictures[attire].logo && vendetta_pictures::available();
                setReturnU32(ctx, own ? kPictureBase + attire : kNoLogo);
                returnToCaller(ctx);
                return;
            }
            // a replaced attire (by record, or by its unlock index): its own logo, none for a new identity without one,
            // else the game's
            const Replaced *rep = nullptr;
            if (byRecord)
            {
                if (const auto found = g_replaced.find(index); found != g_replaced.end())
                    rep = &found->second;
            }
            else
                for (const auto &[record, replaced] : g_replaced)
                    if (replaced.unlockIndex == index)
                        rep = &replaced;
            if (rep)
            {
                const bool own = rep->pictures != ~0u && g_attirePictures[rep->pictures].logo && vendetta_pictures::available();
                if (own || rep->noLogo)
                {
                    setReturnU32(ctx, own ? kPictureBase + rep->pictures : kNoLogo);
                    returnToCaller(ctx);
                    return;
                }
            }
            g_originalLogo(rdram, ctx, runtime);
        }

        // The pictures of the wrestlers from `from - kWantBehind` to `from + kWantAhead` in grid order, read ahead.
        void wantAround(int from)
        {
            for (int w = std::max(0, from - kWantBehind);
                 w <= std::min(static_cast<int>(g_firstAttires.size()) - 1, from + kWantAhead); ++w)
            {
                const AttirePictures &q = g_attirePictures[g_firstAttires[w]];
                for (uint32_t id : {q.render, q.icon, q.shade, q.logo})
                    if (id)
                        vendetta_pictures::want(id);
            }
        }

        // FUN_00184d78(context, group, n): a new attire's picture is drawn from its file over a stand-in the game
        // uploads (or, when the file can't be shown, the "based on" attire's own picture). Showing any of an
        // attire's pictures reads its others in the background, so they are ready when the cursor reaches it.
        void findPicture(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t group = getRegU32(ctx, 5), n = getRegU32(ctx, 6);
            const AttirePictures *p = picturesOf(group == 4u ? n >> 1 : n);
            if (p && (group == 3u || group == 4u || group == 6u))
            {
                uint32_t number = n;
                if (group == 6u)
                    number = p->render && vendetta_pictures::show(kRenderSlot, p->render) ? kStandInRender : p->gameRender;
                else if (group == 4u)
                {
                    const uint32_t id = (n & 1u) ? p->shade : p->icon;
                    number = (id && vendetta_pictures::show(kIconSlot, id) ? kStandInIcon : p->gameIcon) * 2u + (n & 1u);
                }
                else
                {
                    if (!(p->logo && vendetta_pictures::show(kLogoSlot, p->logo)))
                        vendetta_pictures::hide(kLogoSlot); // asked for after its file failed: shown as nothing
                    number = kStandInLogo;
                }
                for (uint32_t id : {p->render, p->icon, p->shade, p->logo})
                    if (id)
                        vendetta_pictures::want(id);
                if (group == 4u)
                {
                    // a grid tile: the wrestlers around it in the grid too, so the cursor never meets an unread picture
                    const uint32_t attire = (n >> 1) - kPictureBase;
                    const auto here = std::find(g_firstAttires.begin(), g_firstAttires.end(), attire);
                    if (here != g_firstAttires.end())
                        wantAround(static_cast<int>(here - g_firstAttires.begin()));
                }
                SET_GPR_U32(ctx, 6, number);
            }
            g_originalFindPicture(rdram, ctx, runtime);
        }

        // A story hero's attire replaced with fight values or moves of its own (the hero rules below are skipped for it;
        // one that brings only names, pictures or a model keeps them, as the game's)
        bool ownHeroData(uint32_t record)
        {
            const auto rep = g_replaced.find(record);
            return record < 12u && rep != g_replaced.end() && (rep->second.wpar || rep->second.wwaz);
        }

        // FUN_0013e958(fighter): a story hero (records 0 to 11) fights with the moves and fight values of the build his
        // stats reach, whatever attire he wears; a replaced hero attire with its own fights with its own
        void heroBuild(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t fighter = getRegU32(ctx, 4);
            const uint32_t record = read32(fighter + 0x34u);
            if (ownHeroData(record))
            {
                write32(fighter + 0x38u, record);
                returnToCaller(ctx);
                return;
            }
            g_originalHeroBuild(rdram, ctx, runtime);
        }

        // ... and no profile upgrades for a replaced hero attire (the player's attire: the front end's +0x44[p], p from
        // the block written)
        void heroUpgrades(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t out = getRegU32(ctx, 4), base = getRegU32(ctx, 6);
            const uint32_t player = (out - kHeroBonus) / 0x18u;
            const bool known = out >= kHeroBonus && (out - kHeroBonus) % 0x18u == 0u && player < 4u;
            const uint32_t record = known ? base + read32(kFrontEnd + 0x44u + 4u * player) : ~0u;
            if (std::getenv("VENDETTA_WRESTLERLOG"))
                std::cerr << "[wrestlers] hero upgrades: out 0x" << std::hex << out << std::dec << " base " << base
                          << (known ? " player " + std::to_string(player) + " record " + std::to_string(record) : std::string(" (not a player's block)"))
                          << (known && ownHeroData(record) ? ": a replaced attire with its own fight values, none added" : "")
                          << std::endl;
            if (known && ownHeroData(record))
            {
                for (uint32_t k = 0; k < 6u; ++k)
                    write32(out + 4u * k, 0u);
                returnToCaller(ctx);
                return;
            }
            g_originalHeroUpgrades(rdram, ctx, runtime);
        }

        // FUN_00151808(record): a moved slot's base (a new record) asks for its game record, so a locked artist given
        // added attires keeps his red silhouette on Choose Fighter
        void isArtist(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t record = getRegU32(ctx, 4);
            for (const Moved &m : g_moved)
                if (m.first == record)
                    SET_GPR_U32(ctx, 4, m.base);
            g_originalIsArtist(rdram, ctx, runtime);
        }

        void loadMatch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            completeMoveData(getRegU32(ctx, 31));
            g_originalLoadMatch(rdram, ctx, runtime);
        }

        void matchInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            completeMoveData(getRegU32(ctx, 31));
            g_originalMatchInit(rdram, ctx, runtime);
        }

        float readFloat(uint32_t address)
        {
            float value = 0.0f;
            std::memcpy(&value, g_rdram + (address & PS2_RAM_MASK), 4);
            return value;
        }
        void writeFloat(uint32_t address, float value) { std::memcpy(g_rdram + (address & PS2_RAM_MASK), &value, 4); }

        // FUN_001588c0: the font's glyph for a character (A-Z, then ' . ! & -), -1 for any other
        int glyph(char c)
        {
            if (c >= 'A' && c <= 'Z')
                return c - 'A';
            switch (c)
            {
            case '\'': return 26;
            case '.': return 27;
            case '!': return 28;
            case '&': return 29;
            case '-': return 30;
            default: return -1;
            }
        }

        // FUN_001586c0's width of a text at x scale 1
        float naturalWidth(uint32_t object, uint32_t text)
        {
            float width = 0.0f;
            for (uint32_t i = 0; i < 512u; ++i)
            {
                const char c = static_cast<char>(g_rdram[(text + i) & PS2_RAM_MASK]);
                if (!c)
                    break;
                const int g = glyph(c);
                width += g < 0 ? readFloat(object + 8u) : readFloat(kGlyphs + 16u * g + 8u) - readFloat(kGlyphs + 16u * g);
                width += readFloat(object + 0xCu);
            }
            return width;
        }

        bool isOneOf(uint32_t ra, const uint32_t (&list)[3]) { return ra == list[0] || ra == list[1] || ra == list[2]; }

        // A winner banner line too wide for the screen: the object shrunk before it is measured, so the banner centres
        // the smaller width itself (every phase alike, the zoom in and out included)
        void textMeasure(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_bannerShrink = 1.0f;
            if (isOneOf(getRegU32(ctx, 31), kBannerMeasured))
            {
                const uint32_t object = getRegU32(ctx, 4);
                const float width = naturalWidth(object, getRegU32(ctx, 5));
                if (width > kBannerWidth)
                {
                    g_bannerShrink = kBannerWidth / width;
                    g_bannerScaleY = readFloat(object + 4u);
                    for (uint32_t field : {0x0u, 0x4u, 0xCu})
                        writeFloat(object + field, readFloat(object + field) * g_bannerShrink);
                }
            }
            g_originalTextMeasure(rdram, ctx, runtime);
        }

        // ... and drawn lower by what it lost, so its middle stays where the banner put it
        void textDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_bannerShrink < 1.0f && isOneOf(getRegU32(ctx, 31), kBannerDrawn))
                ctx->f[13] += kBannerHalfHeight * g_bannerScaleY * (1.0f - g_bannerShrink);
            g_bannerShrink = 1.0f;
            g_originalTextDraw(rdram, ctx, runtime);
        }
    }

    uint32_t endRecord() { return g_endRecord; }

    const std::vector<SlotFile> &slotFiles() { return g_slotFiles; }

    std::string gameFullName(uint32_t record) { return record < g_gameNames.size() ? g_gameNames[record].first : std::string(); }

    uint32_t choiceBase(uint32_t record)
    {
        for (const Moved &m : g_moved)
            if (m.base == record)
                return m.first;
        return record;
    }

    std::vector<uint32_t> recordsNamed(const std::string &name)
    {
        const std::string want = upper(name);
        std::vector<uint32_t> records;
        for (uint32_t record = 0; record < g_gameNames.size(); ++record)
            if (g_gameNames[record].first == want || g_gameNames[record].second == want)
                records.push_back(record);
        for (const Wrestler &w : g_wrestlers)
            if (w.names.count(want))
                records.insert(records.end(), w.records.begin(), w.records.end());
        for (const auto &[record, rep] : g_replaced)
            if (rep.names.count(want) && std::find(records.begin(), records.end(), record) == records.end())
                records.push_back(record);
        for (const Moved &m : g_moved)
            if (std::any_of(records.begin(), records.end(), [&](uint32_t r) { return r >= m.base && r < m.base + m.gameCount; }))
                for (uint32_t k = 0; k < m.count; ++k)
                    if (std::find(records.begin(), records.end(), m.first + k) == records.end())
                        records.push_back(m.first + k);
        return records;
    }

    void install(PS2Runtime &runtime)
    {
        g_rdram = runtime.memory().getRDRAM();
        g_originalLogo = runtime.lookupFunction(kLogo);
        g_originalLoadMatch = runtime.lookupFunction(kLoadMatch);
        g_originalMatchInit = runtime.lookupFunction(kMatchInit);
        g_originalFindPicture = runtime.lookupFunction(kFindPicture);
        g_originalTextMeasure = runtime.lookupFunction(kTextMeasure);
        g_originalTextDraw = runtime.lookupFunction(kTextDraw);
        if (!g_originalLogo || !g_originalLoadMatch || !g_originalMatchInit || !g_originalFindPicture ||
            !g_originalTextMeasure || !g_originalTextDraw)
        {
            vendetta_mods::problem() << "[wrestlers] the game's routines were not found" << std::endl;
            return;
        }
        // the copies are read by the patched game code whether or not a mod adds a wrestler
        buildTables();
        runtime.replaceFunction(kRecount, recount);
        runtime.replaceFunction(kCanUp, canUp);
        runtime.replaceFunction(kCanDown, canDown);
        runtime.replaceFunction(kLeft, left);
        runtime.replaceFunction(kRight, right);
        runtime.replaceFunction(kAnyBefore, anyBefore);
        runtime.replaceFunction(kAnyAfter, anyAfter);
        runtime.replaceFunction(kLogo, logo);
        if (!g_attirePictures.empty())
        {
            runtime.replaceFunction(kFindPicture, findPicture);
            // the first new wrestlers are reached straight from the game's own: read theirs from the start
            wantAround(0);
        }
        g_originalHeroBuild = runtime.lookupFunction(kHeroBuild);
        g_originalHeroUpgrades = runtime.lookupFunction(kHeroUpgrades);
        if (g_originalHeroBuild && g_originalHeroUpgrades &&
            std::any_of(g_replaced.begin(), g_replaced.end(), [](const auto &r) { return ownHeroData(r.first); }))
        {
            runtime.replaceFunction(kHeroBuild, heroBuild);
            runtime.replaceFunction(kHeroUpgrades, heroUpgrades);
        }
        g_originalIsArtist = runtime.lookupFunction(kIsArtist);
        if (g_originalIsArtist && !g_moved.empty())
            runtime.replaceFunction(kIsArtist, isArtist);
        runtime.replaceFunction(kLoadMatch, loadMatch);
        runtime.replaceFunction(kMatchInit, matchInit);
        runtime.replaceFunction(kTextMeasure, textMeasure);
        runtime.replaceFunction(kTextDraw, textDraw);
    }

    const ModelFile *modelFile(uint32_t record)
    {
        if (const auto rep = g_replaced.find(record); rep != g_replaced.end())
            return rep->second.model.model.empty() ? nullptr : &rep->second.model;
        if (record < kFirstNewRecord || record - kFirstNewRecord >= g_modelFiles.size())
            return nullptr;
        const ModelFile &model = g_modelFiles[record - kFirstNewRecord];
        return model.model.empty() ? nullptr : &model;
    }
}

void installVendettaWrestlers(PS2Runtime &runtime) { vendetta_wrestlers::install(runtime); }
