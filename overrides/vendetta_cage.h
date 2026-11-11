// Def Jam Vendetta (USA, SLUS-20639) - the cage match: the rules a Battle match is played by, as Choose Match's second
// step sets them (vendetta_cage_menu.cpp) or a test match's file does (vendetta_testmatch.cpp).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vendetta_cage
{
    // The ways a cage match is won. A normal match is the game's own (Battle's rule word 0x5F, nothing changed).
    struct Rules
    {
        bool cage = false;
        bool escape = true;         // climbing out of the cage wins
        bool pinSubmission = false; // pins and submissions win (rule bits 0x4, 0x2)
        bool tko = false;           // knockouts win (rule bit 0x1)
        bool ropeBreaks = true;     // a pin or a hold is broken at the ropes (rule bit 0x10)
    };

    // The match's rule halfword 0x2361CC (the match block 0x2361C8 + 4). Every match set-up writes all of it (Battle
    // FUN_00140298, Survival FUN_00145630, Story FUN_00187e28), and a rematch keeps it. Its bits 0x0200-0x8000 are read
    // and written by nothing in the game: the cage's own two live there, so they never reach Story or Survival.
    constexpr uint32_t kRuleWord = 0x2361CCu;
    constexpr uint16_t kRuleKO = 0x0001u, kRuleSubmission = 0x0002u, kRulePin = 0x0004u, kRuleRopeBreaks = 0x0010u,
                       kRuleTagBothIn = 0x0020u; // with match kind 2: both partners start in the ring, no tags
    constexpr uint16_t kRuleCage = 0x8000u, kRuleEscape = 0x4000u;

    // The test match (--test-match): these rules for its own match set-up instead of the menu's (once: a rematch keeps
    // the rule word).
    void setTestRules(const Rules &rules);

    // The walls (vendetta_cage.cpp). Each match's start (FUN_00119d70, before it runs): the walls are made again at its
    // first search.
    void matchStarting();
    // Every list of walls the fight's two wall searches take (vendetta_ring.cpp's hooks): `gameList` the one the game's
    // code asked for, `list` the one the ring gives for it. Returns the list to search: in a cage match the floor's have
    // the cage in front, and every list's walls have the cage's permissions.
    uint32_t walls(uint8_t *rdram, uint32_t fighter, uint32_t gameList, uint32_t list);

    // The cage drawn (vendetta_cage_draw.cpp). Each match's start: its pictures made (in the loading screen); each
    // picture: the ring's draw starting (FUN_001a2de8, its $a0 the camera matrix) and the ring drawn (FUN_001a2cf8's start,
    // the cage drawn then), both from vendetta_ring.cpp's hooks.
    void prepareDraw(uint8_t *rdram, PS2Runtime &runtime);
    void ringDrawing(uint8_t *rdram, uint32_t cameraMatrix);
    void drawCage(uint8_t *rdram, PS2Runtime &runtime);
    // The direction (x, z) the screen's right is in the world, from the last picture's camera; false before one.
    bool screenRight(float &x, float &z);

    // This match is a cage match (the rule word's 0x8000).
    bool cageMatch(uint8_t *rdram);
    // The climb (vendetta_cage_climb.cpp). Each match's start (before the game's init): the climb's state set up for a
    // cage match. climbing: the fighter is on the cage.
    void climbMatchStarting(uint8_t *rdram, PS2Runtime &runtime);
    bool climbing(uint8_t *rdram, uint32_t fighter);
}
