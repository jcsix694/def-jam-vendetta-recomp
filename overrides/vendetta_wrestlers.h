// Def Jam Vendetta (USA, SLUS-20639) — new wrestlers from mods: a slot on Choose Fighter, names, size, voice, the
// moves and fight values, read from a folder in a mod.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// A mod lists its wrestlers in mod.json, "new": { "wrestlers": ["wrestlers/Nova"] }. Everything a wrestler is sits in
// its folder, every value written out (nothing is taken from a game wrestler's values while the game runs; starting
// from one is the mod tool's job):
//
//   wrestler.json   everything about it but the pictures and the model, in three parts:
//   { "profile": { "name": "NOVA",     the short name (the HUD, the results); "full name" for the intro and the banner
//                  "full name": "JAY NOVA",
//                  "height": "6'1\"", "weight": "225lbs",
//                  "size": "normal",   short / normal / tall (the game's 0.95 / 1.0 / 1.05)
//                  "based on": "Briggs",   the game wrestler whose model it wears (and, for now, whose pictures
//                                      it shows); not used when it has a model of its own: Briggs' always stands
//                                      behind that (his pictures when the mod brings none, his model if the mod's
//                                      can't be used)
//                  "sounds like": "Spider",   whose voice it uses (intro line, win line, grunts); "based on" if
//                                      left out
//                  "voice": { ... } },   or a voice of its own instead, made from parts: "intro line" and
//                                      "win line" each a game wrestler's (his line), "none", or a recording of his
//                                      own (a WAV, its path from this wrestler.json's folder: "voice/intro.wav",
//                                      played from PC memory: vendetta_voicelines.h), "grunts" one of
//                                      Asian Male, Black Male 1, Black Male 2, Latino Male, White Male, Black
//                                      Woman, Deja, Penny, Carla, "gender" man / woman, and "special-move sound"
//                                      (none, kick, punch, cork, chest, boob, ass) and "shout type" (1-3), these
//                                      two none and by the gender when left out: a new row of the voice table
//     "fight":   { every value of its fight record (the game's WPAR): "styles", "attack" and "defence" by body area,
//                  "stats", "computer", "flags", five "combos", the 23 bits never read },
//     "moveset": { all 128 slots: "none", "game move <number> <name>", a mod's move by its name, or
//                  { "move": ..., "flags": 0-15 } } }
//   render.png, icon.png, logo.png      the select pictures (the big render, the grid icon, the name logo)
//   attires\1, attires\2, attires\3    its attires, as many as there are folders, in order to the first missing
//                   or empty one (none: one attire from the files above). An attire folder can hold any of the files
//                   above and its own model: model.wrs (the game's own format, as adjmk writes it), model.glb or
//                   model.gltf, with an optional textures folder (PNG at any size; a texture missing there comes
//                   from the model file); each file found there is used for that attire, else the wrestler folder's, and its
//                   wrestler.json can hold any of the three parts, each used whole, a part it leaves out coming from the
//                   wrestler folder's (so names, size, voice, moves and pictures can all differ per attire;
//                   vendetta_wrestlerdraw.cpp draws the model). A wrestler with a missing or wrong value is left
//                   out, with the value named (mods.log).
//
// A mod can also replace a game wrestler's attires: "replace": { "wrestlers": { "Briggs": "wrestlers/Stone Cold" } }.
// The same kind of folder; attires\N replaces the game wrestler's attire N (any of his own attires, in any mix; the
// folder itself, with no attire folders, is attire 1), and each file or wrestler.json part found there (else in the
// folder) replaces that part: "profile" (names, height, weight, size, sounds like, "story comment"; a value left out
// stays the game's), "fight" (also the story's copy of the stats), "moveset", render / icon / logo (a new identity
// without a logo shows none), the model. What it doesn't bring, and every attire it doesn't replace, stays the game's
// own; the locks stay the game's. One mod per attire (the first in the load order). Attires he doesn't have follow on
// from his own (DMX's attires\2, then 3): his Choose Fighter slot then points at new records (copies of his own attires,
// then the added ones; Story and Survival keep his own records), the added ones offered once his own are all unlocked.
//
// The three parts were once three files (fight.json and moveset.json beside wrestler.json, its names at the top level);
// those aren't read, and each one found is named in mods.log with where its contents go.
//
// Every new attire becomes a character record from 63 up (59 to 62 are turned into Briggs by the match setup), in the
// game's tables, which the game's code reads from copies in the store (vendetta_store.h: the luis that find them are
// patched, config/patches.toml): the character table, Choose Fighter's grid (its slots after the game's 44; its size at
// 0x400, the last scroll position at 0x404), the size classes, the voice map, the voice table (its own voices' rows
// after the game's 44), and the move data (which the boot reads straight into the store).
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

    // A new or replaced attire's moveset, for the per-match moves (vendetta_animations.cpp puts its mods' moves in).
    struct SlotFile
    {
        std::string modName;
        std::string wrestler;            // its short name
        std::filesystem::path file;      // the wrestler.json holding it
        std::string key;                 // the moveset's key in that file ("moveset"; empty: the whole file)
        std::vector<uint32_t> records;   // its attires
    };
    const std::vector<SlotFile> &slotFiles();

    // Every record (attire) of the wrestler called `name` (any case): a game wrestler by the game's own full or short
    // name (as the game has them, whatever a mod renames), a new one by its folder's name or any attire's names (they
    // may differ per attire). For a mod's "wrestlers": { "<name>": ... } moveset edits.
    std::vector<uint32_t> recordsNamed(const std::string &name);

    // A game record's full name as the game had it (upper case; kept at start-up, whatever a mod replaces), or "".
    std::string gameFullName(uint32_t record);

    // The base record Choose Fighter uses for a game wrestler (his own, or the first of his new records when a mod adds
    // attires he doesn't have): for a test match naming him by his game record.
    uint32_t choiceBase(uint32_t record);

    // A new attire's own model file (its folder's, else its wrestler's), or nullptr: it wears its "based on" model.
    struct ModelFile
    {
        std::string modName;
        std::string wrestler;             // its short name
        std::filesystem::path model;      // model.wrs / model.glb / model.gltf
        std::filesystem::path textures;   // the textures folder (empty when there is none)
    };
    const ModelFile *modelFile(uint32_t record);
}

void installVendettaWrestlers(PS2Runtime &runtime);
