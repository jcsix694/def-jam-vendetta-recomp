// Def Jam Vendetta (USA, SLUS-20639) - new moves from a mods\ folder (vendetta_moves.cpp).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// A mod's mod.json lists its new moves: "new": { "moves": ["moves/Sweet Chin Music"] }. Each is a folder: move.json (the
// move's type, its name on screen, every number the type uses, which animation plays each role) and each animation's
// .bin with its settings .json beside it (speed, timings, windows, sounds; an intro / victory also its camera and eye
// track). Linked parts sit in sub-folders: a grapple's second stage, a strike's next hit or parry pair.
//
// Moves are loaded per match (vendetta_animations.cpp drives it): only the moves the match's wrestlers' slots name.
//   prepare()       once, at the first match load: every listed move.json read for its name; the move records, the
//                   names and the camera / eye track tables copied to the store at their full size
//   isMove()        a name a moveset slot gives is a new move's (its name on screen or its folder's)
//   reset()         each match: those tables back to the game's own (the spare entries pointing at stand-ins)
//   load()          each match: the named moves read whole from their folders, as many as the match's room holds
//   clipsToAdd()    their animations, in the order the game needs them (a pair's receiver right after its attacker, a
//                   hold's eight in a row ...), added as new clips with the mods' new animations
//   clipNumbered()  each one's clip id, as it is added
//   apply()         the moves go in: move records after the game's 999 (and their links), their names, the playback
//                   records of their clips, the intros' and victories' cameras, the eye tracks
//   recordOf()      a new move's record number by its name, for the "wrestlers" files' moveset slots
//   filesRead()     the files this match's moves came from (a rematch reads them again only if one changed)
#pragma once

#include "vendetta_clipsounds.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace vendetta_moves
{
    void prepare(uint8_t *rdram);

    bool isMove(const std::string &name);

    // The folders of the new moves a name gives (read-only after prepare(), so the read-ahead thread may use it).
    std::vector<std::filesystem::path> foldersNamed(const std::string &name);

    void reset();

    // `names`: what the match's slots name; `clipRoom`: how many new clip ids are left for the moves' animations.
    void load(const std::set<std::string> &names, uint32_t clipRoom);

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

    const std::vector<std::filesystem::path> &filesRead();
}
