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
        // shade when n is odd, 6 a render n) is uploaded to one GS slot before each draw (RE 8.62 B)
        constexpr uint32_t kFindPicture = 0x184D78u;
        constexpr uint32_t kLogoSlot = 0x3E50u, kIconSlot = 0x3E80u, kRenderSlot = 0x3EC0u;
        // a new attire's pictures: numbers from here (its render and icon in its character record, its logo from the
        // logo lookup); the game is handed pictures it never shows to upload, drawn over by the mod's files
        constexpr uint32_t kPictureBase = 0x4000u;
        constexpr uint32_t kStandInRender = 59u, kStandInIcon = 59u, kStandInLogo = 0u;
        constexpr uint32_t kLogoByRecordCall = 0x146458u; // Survival's right panel asks FUN_0017c2c0 by record

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
        };

        uint8_t *g_rdram = nullptr;
        std::vector<Wrestler> g_wrestlers;
        std::vector<SlotFile> g_slotFiles;
        std::vector<AttirePictures> g_attirePictures; // by record - 63
        std::vector<uint32_t> g_firstAttires;         // each new wrestler's first attire (record - 63), in grid order
        constexpr int kWantBehind = 6, kWantAhead = 10; // wrestlers either side of a tile drawn whose pictures are read
        uint32_t g_endRecord = kFirstNewRecord;
        uint32_t g_slots = kGameSlots;            // Choose Fighter's slots (even)
        std::vector<uint32_t> g_newAttires;       // per slot from 44: its attires (0 = the filler of an odd count)
        bool g_moveDataReady = false;
        Fn g_originalLogo = nullptr, g_originalLoadMatch = nullptr, g_originalMatchInit = nullptr;
        Fn g_originalFindPicture = nullptr;

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

        // fight.json: every value of a wrestler's fight record (RE §8.62 D), each by name, none left out. A missing or
        // wrong value is named in `problems` (the wrestler is then left out).
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
            // the slots the game's code reads without asking (RE 8.42, 8.59): empty = move 4095 or a division by zero
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
                    std::cerr << "[wrestlers] " << mod.name << ": \"new\": \"wrestlers\" should be a list of folders" << std::endl;
                    continue;
                }
                for (const vendetta_mods::Json &item : list->items)
                {
                    if (!item.isString())
                        continue;
                    const std::filesystem::path file = vendetta_mods::modFile(mod, item.text + "/wrestler.json", "wrestler");
                    std::vector<uint8_t> bytes;
                    vendetta_mods::Json j;
                    std::string error;
                    if (file.empty() || !vendetta_mods::readWhole(file, bytes) ||
                        !vendetta_mods::parseJson(std::string(bytes.begin(), bytes.end()), j, error) || !j.isObject())
                    {
                        std::cerr << "[wrestlers] " << mod.name << ": " << item.text << ": wrestler.json can't be read"
                                  << (error.empty() ? "" : ": " + error) << " - left out" << std::endl;
                        continue;
                    }
                    const std::string name = text(j, "name");
                    const std::string full = text(j, "full name", name);
                    if (name.empty())
                    {
                        std::cerr << "[wrestlers] " << mod.name << ": " << item.text << ": no \"name\" - left out" << std::endl;
                        continue;
                    }
                    uint32_t looksBase = 0, looksCount = 0, voiceBase = 0, voiceCount = 0;
                    const std::string basedOn = text(j, "based on");
                    if (!gameWrestler(basedOn, looksBase, looksCount))
                    {
                        std::cerr << "[wrestlers] " << mod.name << ": " << name << ": \"based on\" must name a game wrestler (\""
                                  << basedOn << "\" isn't one) - left out" << std::endl;
                        continue;
                    }
                    const std::string soundsLike = text(j, "sounds like", basedOn);
                    if (!gameWrestler(soundsLike, voiceBase, voiceCount))
                    {
                        std::cerr << "[wrestlers] " << mod.name << ": " << name << ": \"sounds like\" must name a game wrestler (\""
                                  << soundsLike << "\" isn't one) - left out" << std::endl;
                        continue;
                    }
                    float size = 1.0f;
                    const std::string sizeText = upper(text(j, "size", "normal"));
                    if (sizeText == "SHORT")
                        size = 0.95f;
                    else if (sizeText == "TALL")
                        size = 1.05f;
                    else if (sizeText != "NORMAL")
                        std::cerr << "[wrestlers] " << mod.name << ": " << name << ": \"size\" is short, normal or tall - normal used"
                                  << std::endl;
                    size_t attires = 1u;
                    if (const vendetta_mods::Json *a = j.find("attires"); a && a->isArray() && !a->items.empty())
                        attires = a->items.size();
                    if (attires > 3u)
                    {
                        std::cerr << "[wrestlers] " << mod.name << ": " << name << ": Choose Fighter offers up to 3 attires - the first 3 used"
                                  << std::endl;
                        attires = 3u;
                    }

                    // each attire's fight values and moveset: its own folder's fight.json / moveset.json, else the wrestler's
                    const vendetta_mods::Json *attireList = j.find("attires");
                    std::vector<std::array<uint8_t, kWparSize>> fights(attires);
                    std::vector<std::array<uint16_t, 128>> movesets(attires);
                    std::vector<std::filesystem::path> movesetFiles(attires);
                    std::vector<std::string> attireFolders(attires);
                    bool good = true;
                    std::set<std::filesystem::path> told; // a file shared by the attires: its problems once
                    for (size_t k = 0; k < attires; ++k)
                    {
                        const std::string attireFolder =
                            attireList && attireList->isArray() && k < attireList->items.size() && attireList->items[k].isString()
                                ? item.text + "/" + attireList->items[k].text
                                : std::string();
                        attireFolders[k] = attireFolder;
                        for (const std::string fileName : {"fight.json", "moveset.json"})
                        {
                            std::error_code ec;
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
                            if (told.insert(path).second)
                                for (const std::string &problem : problems)
                                    std::cerr << "[wrestlers] " << mod.name << ": " << name << ": "
                                              << (path.empty() ? fileName : path.filename().string()) << ": " << problem << std::endl;
                            good = good && problems.empty();
                        }
                    }
                    if (!good)
                    {
                        std::cerr << "[wrestlers] " << mod.name << ": " << name << " left out (every value must be in its files)" << std::endl;
                        continue;
                    }
                    Wrestler w;
                    w.modName = mod.name;
                    w.name = name;
                    const uint32_t fullText = guestText(full), shortText = guestText(name);
                    const uint32_t heightText = guestText(text(j, "height")), weightText = guestText(text(j, "weight"));
                    for (uint32_t k = 0; k < attires; ++k)
                    {
                        const uint32_t record = g_endRecord++;
                        const uint32_t looks = looksBase + std::min(k, looksCount - 1u);
                        const uint32_t entry = vendetta_store::kCharacterTable + kRecordSize * record;
                        std::memcpy(guest(entry), guest(kGameCharacters + kRecordSize * looks), kRecordSize);
                        write32(entry + 0x00u, fullText);
                        write32(entry + 0x04u, shortText);
                        write32(entry + 0x08u, heightText);
                        write32(entry + 0x0Cu, weightText);
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
                        float sizeValue = size;
                        std::memcpy(guest(vendetta_store::kSizeClasses + 4u * record), &sizeValue, 4);
                        write32(vendetta_store::kVoiceMap + 4u * record, read32(kGameVoices + 4u * voiceBase));
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
                        g_slotFiles.push_back({mod.name, name, movesetFiles[k], {record}});
                    }
                    g_newAttires.push_back(static_cast<uint32_t>(attires));
                    g_firstAttires.push_back(w.records.front() - kFirstNewRecord);                    std::cerr << "[wrestlers] " << mod.name << ": " << full << " (" << name << "), records " << w.records.front()
                              << "-" << w.records.back() << ", looks like " << guestString(read32(kGameCharacters + kRecordSize * looksBase))
                              << ", sounds like " << guestString(read32(kGameCharacters + kRecordSize * voiceBase)) << std::endl;
                    g_wrestlers.push_back(std::move(w));
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
            readWrestlers();
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
                std::cerr << "[wrestlers] a match began before the move data was read (ra 0x" << std::hex << ra << std::dec
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
            if (!g_wrestlers.empty())
                std::cerr << "[wrestlers] the new attires' fight values and movesets are in (the game points at its own "
                          << (wparTagged ? "tags" : "payloads") << " / " << (wwazTagged ? "tags" : "payloads") << ")" << std::endl;
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
                const uint32_t attires = slot < kGameSlots ? read32(counts + 4u * base) : g_newAttires[slot - kGameSlots];
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
            const uint32_t index = getRegU32(ctx, 4);
            const bool byRecord = getRegU32(ctx, 31) == kLogoByRecordCall;
            if ((byRecord && index >= kFirstNewRecord && index < g_endRecord) ||
                (!byRecord && index >= kFirstLogoIndex && index < kFirstLogoIndex + g_attirePictures.size()))
            {
                const uint32_t attire = index - (byRecord ? kFirstNewRecord : kFirstLogoIndex);
                const bool own = g_attirePictures[attire].logo && vendetta_pictures::available();
                setReturnU32(ctx, own ? kPictureBase + attire : kNoLogo);
                returnToCaller(ctx);
                return;
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
    }

    uint32_t endRecord() { return g_endRecord; }

    const std::vector<SlotFile> &slotFiles() { return g_slotFiles; }

    void install(PS2Runtime &runtime)
    {
        g_rdram = runtime.memory().getRDRAM();
        g_originalLogo = runtime.lookupFunction(kLogo);
        g_originalLoadMatch = runtime.lookupFunction(kLoadMatch);
        g_originalMatchInit = runtime.lookupFunction(kMatchInit);
        g_originalFindPicture = runtime.lookupFunction(kFindPicture);
        if (!g_originalLogo || !g_originalLoadMatch || !g_originalMatchInit || !g_originalFindPicture)
        {
            std::cerr << "[wrestlers] the game's routines were not found" << std::endl;
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
        runtime.replaceFunction(kLoadMatch, loadMatch);
        runtime.replaceFunction(kMatchInit, matchInit);
    }
}

void installVendettaWrestlers(PS2Runtime &runtime) { vendetta_wrestlers::install(runtime); }
