// Def Jam Vendetta (USA, SLUS-20639) - new moves from a mods\ folder (vendetta_moves.cpp).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// A mod's mod.json lists its new moves: "new": { "moves": ["moves/Sweet Chin Music"] }. Each is a folder: move.json (the
// move's type, its name on screen, every number the type uses, which animation plays each role) and each animation's
// .bin with its settings .json beside it (speed, timings, windows, sounds; an intro / victory also its camera and eye
// track). Linked parts sit in sub-folders: a grapple's second stage, a strike's next hit or parry pair.
//
// The animation loader (vendetta_animations.cpp) drives it, once both clip banks are in memory:
//   prepare()       reads and checks every mod's new moves
//   clipsToAdd()    their animations, in the order the game needs them (a pair's receiver right after its attacker, a
//                   hold's eight in a row ...), added as new clips with the mods' new animations
//   clipNumbered()  each one's clip id, as it is added
//   apply()         the moves go in: move records after the game's 999 (and their links), their names, the playback
//                   records of their clips, the intros' and victories' cameras, the eye tracks
//   recordOf()      a new move's record number by its name, for the "wrestlers" files' moveset slots
#pragma once

#include "vendetta_clipsounds.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vendetta_moves
{
    void prepare(uint8_t *rdram);

    struct ClipToAdd
    {
        std::string name; // "<move name>: <role>"
        std::string from; // the mod
        const std::vector<uint8_t> *bytes = nullptr;
        const std::vector<vendetta_clipsounds::Event> *sounds = nullptr;
    };
    std::vector<ClipToAdd> clipsToAdd();
    void clipNumbered(size_t which, uint32_t clipId);

    void apply();

    // A new move's record number by its name on screen or its folder's name (any case); -1 if there is none.
    int recordOf(const std::string &name);
}
