// Def Jam Vendetta (USA, SLUS-20639) — new wrestlers from mods: a slot on Choose Fighter, names, size, voice, the
// moves and fight values, read from a folder in a mod.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// A mod lists its wrestlers in mod.json, "new": { "wrestlers": ["wrestlers/Nova"] }. Everything a wrestler is sits in
// its folder, every value written out (nothing is taken from a game wrestler's values while the game runs; starting
// from one is the mod tool's job):
//
//   wrestler.json
//   { "name": "NOVA",                  the short name (the HUD, the results); "full name" for the intro and the banner
//     "full name": "JAY NOVA",
//     "height": "6'1\"", "weight": "225lbs",
//     "size": "normal",                short / normal / tall (the game's 0.95 / 1.0 / 1.05)
//     "based on": "Briggs",            the game wrestler whose model it wears (and, for now, whose pictures it shows)
//     "sounds like": "Spider",         whose voice it uses (intro line, win line, grunts); "based on" if left out
//     "attires": ["attires/1", "attires/2"] }   one to three
//   fight.json      every value of its fight record (the game's WPAR): "styles", "attack" and "defence" by body area,
//                   "stats", "computer", "flags", five "combos", the 23 bits never read
//   moveset.json    all 128 slots: "none", "game move <number> <name>", a mod's move by its name, or
//                   { "move": ..., "flags": 0-15 }
// An attire's own folder can hold its own fight.json / moveset.json. A wrestler with a missing or wrong value is left
// out, with the value named. The model, the voice and (for now) the pictures are the game's own files standing in.
//
// Every new attire becomes a character record from 63 up (59 to 62 are turned into Briggs by the match setup), in the
// game's tables, which the game's code reads from copies in the store (vendetta_store.h: the luis that find them are
// patched, config/patches.toml): the character table, Choose Fighter's grid (its slots after the game's 44; its size at
// 0x400, the last scroll position at 0x404), the size classes, the voice map, and the move data (which the boot reads
// straight into the store).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class PS2Runtime;

namespace vendetta_wrestlers
{
    // Called once at start, after the animations (its match hooks then run first): the tables copied, the mods'
    // wrestlers read, Choose Fighter's helpers replaced.
    void install(PS2Runtime &runtime);

    constexpr uint32_t kFirstNewRecord = 63u;

    // One past the last record in use (63 when no mod adds a wrestler).
    uint32_t endRecord();

    // A new attire's moveset.json, for the per-match moves (vendetta_animations.cpp puts its mods' moves in).
    struct SlotFile
    {
        std::string modName;
        std::string wrestler;            // its short name
        std::filesystem::path file;
        std::vector<uint32_t> records;   // its attires
    };
    const std::vector<SlotFile> &slotFiles();
}

void installVendettaWrestlers(PS2Runtime &runtime);
