// Def Jam Vendetta (USA, SLUS-20639) - ring shapes: an arena's "ring" description (arena.json), read by vendetta_arenas.cpp
// and run by vendetta_ring.cpp.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
#pragma once

#include "vendetta_mods.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vendetta_ring
{
    // The return address of the calls our rope draw makes: vendetta_arenas.cpp treats a part drawn from here as a rope
    constexpr uint32_t kRopeDrawReturn = 0x1DF454u;

    struct Ring
    {
        int sides = 0;
        float ropeDistance = 0.0f; // from the middle to each flat side's ropes
        float matHeight = 0.0f;
        float ropeHeights[3] = {}; // top, middle, bottom
        float slumpDistance = 0.0f;
        float apronInner = 0.0f, apronLine = 0.0f, apronOuter = 0.0f;
        float ringSide = 0.0f, outsideWall = 0.0f;
    };

    // arena.json's "ring". False, with a line in mods.log, when it can't be used: the arena then has the game's own ring.
    bool parse(const vendetta_mods::Json &json, const std::string &what, Ring &out);

    // The arena being loaded: its ring, or null for the game's own. Takes effect at the match's start.
    void setArena(const Ring *ring);

    // A ring of another shape has its own corner pieces, in the High Poly post format (as 81-92): part 1000 + corner a
    // post, 1020 + corner a pad, 1040 + corner a turnbuckle (corners from 0, up to 16)
    constexpr int32_t kCornerPost = 1000, kCornerPad = 1020, kCornerTurnbuckle = 1040, kCornerRoom = 16;
    inline bool cornerPiece(int32_t id)
    {
        for (const int32_t base : {kCornerPost, kCornerPad, kCornerTurnbuckle})
            if (id >= base && id < base + kCornerRoom)
                return true;
        return false;
    }

    // The arena being loaded: its corner pieces' spots on the floor (each piece's box middle, x and z), for their fade
    // near the camera; empty = none
    struct CornerSpot
    {
        int32_t part = 0;
        float x = 0.0f, z = 0.0f;
    };
    void setCornerSpots(const std::vector<CornerSpot> &spots);
}
