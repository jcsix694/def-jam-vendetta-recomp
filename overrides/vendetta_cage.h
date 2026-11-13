// Def Jam Vendetta (USA, SLUS-20639) - the cage match: the rules a Battle match is played by, as Choose Match's second
// step sets them (vendetta_cage_menu.cpp) or a test match's file does (vendetta_testmatch.cpp).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
#pragma once

#include <cstdint>
#include <filesystem>

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

        // Rope breaks in the match: the choice counts only while pins and submissions do. With those off a rope break
        // would only cut short a hold that can't win, so the match has none whatever the choice was left on.
        bool ropeBreaksOn() const { return pinSubmission && ropeBreaks; }
    };

    // The match's rule halfword 0x2361CC (the match block 0x2361C8 + 4). Every match set-up writes all of it (Battle
    // FUN_00140298, Survival FUN_00145630, Story FUN_00187e28), and a rematch keeps it. Its bits 0x0200-0x8000 are read
    // and written by nothing in the game: the cage's own two live there, so they never reach Story or Survival.
    constexpr uint32_t kRuleWord = 0x2361CCu;
    constexpr uint16_t kRuleKO = 0x0001u, kRuleSubmission = 0x0002u, kRulePin = 0x0004u, kRuleRopeBreaks = 0x0010u,
                       kRuleTagBothIn = 0x0020u; // with match kind 2: both partners start in the ring, no tags
    constexpr uint16_t kRuleCage = 0x8000u, kRuleEscape = 0x4000u;

    // The cage's clips go by the numbers they had in the game they were converted from (3203 getting on, ...). One more is
    // made by the loader from 3204 (hanging): the hang at the rope hold, the lowest foot on the top rope, which its
    // playback record bends to that foot (vendetta_animations.cpp, vendetta_cage_climb.cpp).
    constexpr uint32_t kHangOnRopeClip = 0x13204u;

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
    // The cage's files (its model cage4.vcage / cage6.vcage, its clips in animations\, its cage.json): <the game's
    // folder>\standard\cage\, the standard cage, copied there by the setup from the repository's standard\cage\ (as the
    // standard arenas are; a cage of a mod's own will come through a project, as arenas do).
    std::filesystem::path cageFolder();
    // Which way a step from the world place (x, y, z) along (dx, dz) goes across the screen, by the last picture's camera
    // (both ends projected as the cage's own points are): sign +1 right, -1 left; false before a picture, or with the
    // place behind the camera.
    bool screenSide(float x, float y, float z, float dx, float dz, float &sign);
    // Where the world place (x, y, z) is across the screen, by the last picture's camera: pan -1 the left edge, 0 the
    // middle, +1 the right edge (held there beyond); false before a picture, or with the place behind the camera.
    bool screenPan(float x, float y, float z, float &pan);

    // This match is a cage match (the rule word's 0x8000).
    bool cageMatch(uint8_t *rdram);
    // The climb (vendetta_cage_climb.cpp). Each match's start (before the game's init): the climb's state set up for a
    // cage match. climbing: the fighter is on the cage.
    void climbMatchStarting(uint8_t *rdram, PS2Runtime &runtime);
    bool climbing(uint8_t *rdram, uint32_t fighter);
    // A team member over the top waiting outside (tag and handicap): out of play, so the fight camera leaves it out
    // (vendetta_camera.cpp); anyoneWaiting: one of them this match
    bool waitingOutside(uint8_t *rdram, uint32_t fighter);
    bool anyoneWaiting(uint8_t *rdram);
    // A move's rope window (FUN_00131848's type) for this fighter skipped: a dive from the cage's top while it is above
    // the mat (its take-off's windows would pull the top rope up to the feet); the ring shaking (6) never
    bool skipRopeWindow(uint8_t *rdram, uint32_t fighter, uint32_t type);
    // The escape's result lists ({a fighter's +0x30 ...} each, the last -1) written into the cage's own store area, for
    // the game's result routine (FUN_00135BB8): their guest addresses; false without the area.
    bool resultLists(const int32_t *winners, int winnerCount, const int32_t *losers, int loserCount, uint32_t &winnersAt,
                     uint32_t &losersAt);
    // VENDETTA_CAGECLIMBLOG=2: the pictures after a landing, logged (from walls, every fighter every picture)
    void climbWatch(uint8_t *rdram, uint32_t fighter);

    // Leaving by the door (vendetta_cage_leave.cpp). In a cage, tag and handicap are last man wins: someone pinned,
    // submitted or knocked out who is not the team's last still in the ring is out of the match while it goes on. The
    // game takes that wrestler over: up from the mat (a knockout after a wait), the walk to the ropes in front of the
    // cage's door, the door swinging open, out through the ropes and down through the doorway to the floor, the door
    // shut again, and standing beside it facing the ring, waiting as one over the top for the team does. All against
    // all is first man wins: the first fall ends the match.
    void leaveMatchStarting();
    // out by a fall this match (on the way out or outside): not a team-mate still in the ring, not to be targeted
    bool outByFall(uint8_t *rdram, uint32_t fighter);
    // The climb's state for one waiting outside asks, each picture: the walk from the doorway to its place beside the
    // door. Moves the fighter; the clip to play as the routine's last call (0: none), its flags.
    uint32_t leaveAside(uint8_t *rdram, uint32_t fighter, uint32_t &flags);
    // (vendetta_cage_climb.cpp) a fighter on the floor outside made to wait there as one over the top for the team does
    // (out of play, the climb's state, out of reach, not targeted, left out by the fight camera); notTargeted: everyone
    // targeting this fighter given someone still in the ring
    void waitOutside(uint8_t *rdram, uint32_t fighter);
    void notTargeted(uint8_t *rdram, uint32_t fighter);
    // The cage's door (vendetta_cage_draw.cpp): where the model's file says it is, for the cage being drawn (the hinge's
    // and the latch's place on the floor plan); false when the model has none. doorTo: the door told to swing open or
    // shut, over so many pictures (it moves as the cage is drawn, so through a match's ending too); doorOpen: how far
    // open it is now, 0 shut to 1 open (the leaf turned 90 degrees outward about its hinges).
    bool door(float &hingeX, float &hingeZ, float &latchX, float &latchZ);
    void doorTo(bool open, float pictures);
    float doorOpen();
}
