// Def Jam Vendetta (USA, SLUS-20639) - the cage match in the fight: its walls, on the game's square ring and on a mod
// arena's six-sided one (vendetta_ring.cpp).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// A match is a cage match when its rule word carries kRuleCage (vendetta_cage.h; only Battle's match set-up puts it
// there, for Choose Match's CAGE MATCH). The fight finds its walls through two searches, the edge push FUN_0012acc8 and
// the probe FUN_0012b600, which take a list of walls by the wrestler's place (in the ring, on the apron, on the floor);
// vendetta_ring.cpp's two hooks in them hand every list to walls() here. In a cage match:
// - Nobody goes on the apron in a cage (the user's rule): each wall's permissions (+0x44, the game asks them before every
//   way out a wrestler chooses) lose: a rope's 0x2 (stepping out through the ropes: triangle at the ropes does nothing,
//   later the climb) and 0x10 (thrown over the top rope), an apron wall's 0x2 (step down, dive off). Each wall is changed
//   the first time a search uses its list in the match; the game's own walls (the square's, built once at start-up) get
//   their values back at the first search of the next match that is not a cage match.
// - The floor's lists get the cage itself in front: one wall a side at the apron's outer edge (43 from the middle),
//   from the floor up, facing out, the outside barrier's kind (0x80, permissions 0xA1: a runner whipped into it crashes),
//   so someone outside (later: a wrestler who climbed out) stays outside. Wrestlers on the apron already stop at its
//   outer walls at the same 43.
// What data can't stop, the ways the game's code takes a wrestler to the floor without asking a wall, is closed in code
// (vendetta_cage_fight below): each hook stands in for one instruction (config/patches.toml: a jal to an address here) or
// a whole routine, and in a cage match gives the answer the game itself gives when that way isn't open, so the game's
// own other path follows; outside a cage match each does exactly what it replaces:
//   0x115494  the run out at a rope (FUN_00115438: circle to the floor, triangle onto the apron): the rope's +0x44 & 8;
//             a cage answers 0 for a rope: no run out, the rope rebounds the runner (state 11)
//   0x128564  state 19 (the big stagger) sliding into a rope, the 0.3 chance of state 20 (over the ropes); a cage answers
//             0: the game's own test for 21 follows (stunned on the ropes), else the stagger plays on
//   0x1150FC  the runner the whipper dodged (state 73, +0x5B6 0xC0) reaching a rope: state 20; a cage starts 11, the
//             rope's ordinary rebound, at the same call
//   0x123B5C  state 20 against the ropes when stunned on them (+0x5AE 1: a whipped runner's bump, an aerial): I405 to the
//             floor; a cage plays I406, state 21's own other ending (knocked down inside)
//   0x100E78  state 21 (stunned on the ropes) hit again, the 0.2 chance of I405 (to the floor); a cage answers 0: I406
// Kept as a safety net (with nobody on the apron and the ways above closed, none should be reached; each logs):
//   0x123354  state 14 (out through the ropes), FUN_00123170 step 2: + 2 (on down to the floor) when triangle is held;
//             a cage answers "not held": + 1, the apron
//   0x123AFC  state 20 on the apron, FUN_00123aa0 step 0: W106 / W107 to the floor; a cage plays W105 (the apron's stagger)
//   0x123B74  state 20 in the ring otherwise, the 0.25 chance of I303 / I301 (to the floor); a cage answers 0: I308 /
//             I307 (onto the apron, the game's 75 %)
//   0x11D4EC  the grab picker FUN_0011d468, the running catch at a rope (slot 41, the runner ends on the floor); a cage
//             answers "no rope": the ordinary catch
//   FUN_0011db38 the grab pairing: across the ropes (35 the man on the apron thrown to the floor, 37 / 38 the attacker
//             dropping to it) and pulling someone out from the floor (74-77); a cage answers "no pair": the grab misses
//   FUN_0013a050 the top-rope move for an opponent: at an opponent on the apron (or the floor) the diver lands on the
//             floor; a cage answers "none fits": no climb, or a climb back down
// Still open: the eliminated in a 3 / 4-way (they walk or roll out; a decision for later), handicap's partners on the
// apron, mod moves whose clips end on the floor or the apron. Each arrival on the floor in a cage match is logged with the states that led there.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "vendetta_cage.h"
#include "vendetta_cage_file.h"
#include "vendetta_ring.h"
#include "vendetta_store.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vendetta_cage_walls
{
    constexpr uint32_t kMatch = 0x2361C8u;            // the match block: +4 the rule halfword
    constexpr uint32_t kGameFloorLists = 0x235BA8u;   // the floor's four lists by side, 16 bytes each
    constexpr uint32_t kAreaScale = 0x294A84u;        // the float (0.1) a wall's +0x3C is scaled by
    constexpr uint32_t kRecordBytes = 0x50u;
    constexpr uint16_t kRope = 0x20u, kApron = 0x40u, kOutside = 0x80u; // a wall's kind (+0x42)
    constexpr uint16_t kThrowOverTop = 0x10u, kStepOut = 0x2u;         // a wall's permissions (+0x44): 2 a rope's step out,
                                                                       // an apron wall's step down
    constexpr uint16_t kCagePermissions = 0xA1u;                       // as the outside barrier's
    // the cage's walls from the middle: cage.json's "the cage's walls from the middle" (the standard cage's 40, where its
    // model's chain-link stands; they stood at the apron's outer edge, 43, until 2026-11-11)
    float cageDistance() { return vendetta_cage::cageFile().numbers.walls; }
    // the cage's top: the mat (10.5) and cage.json's "the cage's height above the mat" (the standard cage's 44, 2
    // wrestlers: the user's choice)
    float cageTop() { return 10.5f + vendetta_cage::cageFile().numbers.height; }

    // the fighter's fields
    constexpr uint32_t kPlayer = 0x30u, kState = 0x40u, kStep = 0x44u, kHistory = 0x54u, kX = 0xCCu, kZ = 0xD4u,
                       kTouched = 0x7A0u, kPlace = 0x5ACu; // +0x7A0 / +0x7A4 the walls the last push touched
    constexpr uint32_t kOnApron = 0x8u, kOnFloor = 0x10u;

    // the store's fixed area, offsets 0x400-0xFFF (vendetta_store.h): the cage's walls, their {first, last}, the lists
    constexpr uint32_t kRecords = 0x400u, kMaxSides = 16u, kPair = 0x900u, kLists = 0x910u, kListBytes = 0x20u,
                       kMaxLists = 32u, kAreaEnd = 0x1000u;
    constexpr uint32_t kResultLists = 0xF00u; // the escape's winners (16 bytes) and losers (16 bytes), after the lists' 0xD10

    struct Vec
    {
        float x, y, z;
    };
    Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    Vec cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    float length(Vec a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

    uint32_t g_area = 0;
    bool g_newMatch = true;              // set at each match's start: the walls made again at its first search
    bool g_cage = false;                 // this match is a cage match
    uint32_t g_listsUsed = 0;
    std::unordered_map<uint32_t, uint32_t> g_cageLists;     // a floor list -> the same with the cage in front
    std::unordered_set<uint32_t> g_listsDone;               // the lists whose walls are changed this match
    std::vector<std::pair<uint32_t, uint16_t>> g_gameValues; // the game's own walls changed: their permissions as they were
    bool g_onFloor[4] = {};
    bool g_onApron[4] = {};
    bool g_held[4] = {};
    uint32_t g_cageSides = 0, g_heldCount = 0;

    float readFloat(uint8_t *rdram, uint32_t a) { float f; std::memcpy(&f, vendetta_store::host(a), 4); return f; }
    uint16_t read16(uint32_t a) { uint16_t v; std::memcpy(&v, vendetta_store::host(a), 2); return v; }
    void write16(uint32_t a, uint16_t v) { std::memcpy(vendetta_store::host(a), &v, 2); }
    uint32_t read32(uint32_t a) { uint32_t v; std::memcpy(&v, vendetta_store::host(a), 4); return v; }
    void write32(uint32_t a, uint32_t v) { std::memcpy(vendetta_store::host(a), &v, 4); }

    // the game's own walls back as they were (the square's records live in the game's memory for the whole run)
    void putGameValuesBack()
    {
        for (const auto &[wall, value] : g_gameValues)
            write16(wall + 0x44u, value);
        g_gameValues.clear();
    }

    // one wall from a to b, from the floor to the cage's top, facing out: its corners in the game's order, normal and size as
    // FUN_00130ba8 works them out at a match's start
    void putCageWall(uint8_t *rdram, uint32_t at, Vec a, Vec b, Vec out, int number)
    {
        uint8_t *record = vendetta_store::host(at);
        std::memset(record, 0, kRecordBytes);
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            const float top = cageTop();
            const Vec c[4] = {{a.x, top, a.z}, {b.x, top, b.z}, {b.x, 0.0f, b.z}, {a.x, 0.0f, a.z}};
            std::memcpy(record, c, sizeof(c));
            Vec n = cross(c[3] - c[0], c[2] - c[0]);
            const float size = length(n);
            if (size > 0.0f)
                n = {n.x / size, n.y / size, n.z / size};
            const float area = (length(cross(c[1] - c[0], c[2] - c[0])) + length(cross(c[2] - c[0], c[3] - c[0]))) *
                               readFloat(rdram, kAreaScale);
            std::memcpy(record + 0x30u, &n, sizeof(n));
            std::memcpy(record + 0x3Cu, &area, sizeof(area));
            if (n.x * out.x + n.z * out.z >= 0.0f)
                break;
            std::swap(a, b);
        }
        // the push's directions (the low four bits, as the game's own walls: 1 +z, 2 -x, 4 -z, 8 +x), both axes on a
        // slanted side so it pushes straight off it
        uint16_t flags = kOutside;
        if (out.z > 0.01f)
            flags |= 1u;
        if (out.x < -0.01f)
            flags |= 2u;
        if (out.z < -0.01f)
            flags |= 4u;
        if (out.x > 0.01f)
            flags |= 8u;
        write16(at + 0x40u, static_cast<uint16_t>(number));
        write16(at + 0x42u, flags);
        write32(at + 0x44u, kCagePermissions);
    }

    // the cage's walls round the ring this match has: the six-sided ring's sides (its first corner at 90 degrees, as
    // vendetta_ring.cpp builds it) or the game's square
    void buildCage(uint8_t *rdram)
    {
        const int ringSides = vendetta_ring::activeSides();
        const int sides = ringSides > 0 ? ringSides : 4;
        const float firstAngle = ringSides > 0 ? 90.0f : 135.0f, pi = 3.14159265358979f;
        const float toCorner = 1.0f / std::cos(pi / sides);
        auto corner = [&](int k) {
            const float rad = (firstAngle - 360.0f * k / sides) * pi / 180.0f;
            return Vec{std::sin(rad) * cageDistance() * toCorner, 0.0f, std::cos(rad) * cageDistance() * toCorner};
        };
        const uint32_t first = g_area + kRecords;
        for (int k = 0; k < sides && k < int(kMaxSides); ++k)
        {
            const float rad = (firstAngle - 360.0f * (k + 0.5f) / sides) * pi / 180.0f;
            putCageWall(rdram, first + k * kRecordBytes, corner(k), corner(k + 1), {std::sin(rad), 0.0f, std::cos(rad)}, 50 + k);
            write32(first + k * kRecordBytes + 0x48u, k + 1 < sides ? first + (k + 1) * kRecordBytes : 0u);
        }
        g_cageSides = static_cast<uint32_t>(sides);
        write32(g_area + kPair, first);
        write32(g_area + kPair + 4u, first + (sides - 1) * kRecordBytes);
        std::cerr << "[cage] a cage match: " << sides << " cage walls at " << cageDistance() << " round the "
                  << (ringSides > 0 ? "six-sided ring" : "square ring") << std::endl;
    }

    // the cage's rules on every wall of a list: no step out through the ropes, no throw over the top rope, no step down
    void changeWalls(uint32_t list)
    {
        for (uint32_t entry = list; read32(entry) != 0u; entry += 4u)
        {
            const uint32_t pair = read32(entry), last = read32(pair + 4u);
            uint32_t wall = read32(pair);
            for (int n = 0; wall && n < 64; ++n)
            {
                const uint16_t kind = read16(wall + 0x42u), was = read16(wall + 0x44u);
                uint16_t now = was;
                if (kind & kRope)
                    now &= ~(kThrowOverTop | kStepOut);
                if (kind & kApron)
                    now &= ~kStepOut;
                if (now != was)
                {
                    if (wall < vendetta_store::kFixedArea) // the game's own: back at the next normal match
                    {
                        bool saved = false;
                        for (const auto &g : g_gameValues)
                            saved = saved || g.first == wall;
                        if (!saved)
                            g_gameValues.push_back({wall, was});
                    }
                    write16(wall + 0x44u, now);
                }
                if (wall == last)
                    break;
                wall = read32(wall + 0x48u);
            }
        }
    }

    // a floor list with the cage in front
    uint32_t cageList(uint32_t list)
    {
        if (auto it = g_cageLists.find(list); it != g_cageLists.end())
            return it->second;
        if (g_listsUsed >= kMaxLists)
            return list;
        const uint32_t out = g_area + kLists + kListBytes * g_listsUsed++;
        uint32_t at = out;
        write32(at, g_area + kPair);
        at += 4u;
        for (uint32_t entry = list; read32(entry) != 0u && at + 4u < out + kListBytes; entry += 4u, at += 4u)
            write32(at, read32(entry));
        write32(at, 0u);
        g_cageLists[list] = out;
        return out;
    }

    void arrived(uint8_t *rdram, uint32_t fighter, uint32_t player, const char *where)
    {
        std::cerr << "[cage] player " << player << " on the " << where << " at (" << readFloat(rdram, fighter + kX) << ", "
                  << readFloat(rdram, fighter + kZ) << "): state " << read32(fighter + kState) << " step "
                  << read32(fighter + kStep) << ", the states before";
        for (uint32_t i = 0; i < 6u; ++i) // the history FUN_00102c20 keeps, the newest first
            std::cerr << " " << static_cast<int32_t>(read32(fighter + kHistory + 4u * i));
        std::cerr << std::endl;
    }

    // each arrival outside the ring in a cage match (nobody should get there: the log says how one did)
    void logFloor(uint8_t *rdram, uint32_t fighter)
    {
        const uint32_t player = read32(fighter + kPlayer) & 3u;
        const uint32_t where = read32(fighter + kPlace);
        const bool floor = (where & kOnFloor) != 0u, apron = (where & kOnApron) != 0u;
        if (floor && !g_onFloor[player])
            arrived(rdram, fighter, player, "floor");
        if (apron && !g_onApron[player])
            arrived(rdram, fighter, player, "apron");
        g_onFloor[player] = floor;
        g_onApron[player] = apron;
        // the cage holding someone outside: once each time it starts to (the result of the fighter's last search)
        const uint32_t first = g_area + kRecords, end = first + g_cageSides * kRecordBytes;
        bool held = false;
        for (const uint32_t wall : {read32(fighter + kTouched), read32(fighter + kTouched + 4u)})
            held = held || (wall >= first && wall < end);
        if (held && !g_held[player])
            std::cerr << "[cage] player " << player << " held by the cage at (" << readFloat(rdram, fighter + kX) << ", "
                      << readFloat(rdram, fighter + kZ) << "): state " << read32(fighter + kState) << " (" << ++g_heldCount
                      << " this match)" << std::endl;
        g_held[player] = held;
    }
}

namespace vendetta_cage_fight
{
    constexpr uint32_t kRuleWord = 0x2361CCu;
    constexpr uint32_t kPlayClip = 0x132158u;  // FUN_00132158(fighter, clip, flags)
    constexpr uint32_t kChance = 0x1310F0u;    // FUN_001310f0(chance $f12): 1 that often
    constexpr uint32_t kEdgeRope = 0x11F7A8u;  // FUN_0011f7a8(fighter, slot): the rope a catch at an edge would use, or -1
    constexpr uint32_t kGrabPair = 0x11DB38u;  // FUN_0011db38(grabber, grabbed): the pair the grab makes, 0 none
    constexpr uint32_t kTopRopeMove = 0x13A050u; // FUN_0013a050(fighter): the top-rope move for the opponent, 0 none

    // code addresses no recompiled function covers (vendetta_cage_menu.cpp has 0x1DF760-0x1DF76C)
    constexpr uint32_t kHookStepOut = 0x1DF770u;    // jal from 0x123354, in place of lw $v0, 0x668($s0)
    constexpr uint32_t kHookRunOut = 0x1DF774u;     // jal from 0x115494, in place of lhu $v0, 0x44($a0)
    constexpr uint32_t kHookApronFall = 0x1DF778u;  // jal from 0x123AFC, in place of jal FUN_00132158
    constexpr uint32_t kHookStunnedFall = 0x1DF77Cu; // jal from 0x123B5C, in place of jal FUN_00132158
    constexpr uint32_t kHookFallChance = 0x1DF780u; // jal from 0x123B74, in place of jal FUN_001310f0
    constexpr uint32_t kHookStunnedChance = 0x1DF784u; // jal from 0x100E78, in place of jal FUN_001310f0
    constexpr uint32_t kHookEdgeCatch = 0x1DF788u;  // jal from 0x11D4EC, in place of jal FUN_0011f7a8
    constexpr uint32_t kHookStaggerChance = 0x1DF78Cu; // jal from 0x128564, in place of jal FUN_001310f0
    constexpr uint32_t kHookWhipState = 0x1DF790u;  // jal from 0x1150FC, in place of jal FUN_00101680
    constexpr uint32_t kSetState = 0x101680u;       // FUN_00101680(fighter, state)
    constexpr uint32_t kOverTheRopes = 20u, kRebound = 11u;

    constexpr uint32_t kW105 = 0x2917u, kI406 = 0x2810u; // the apron's stagger; knocked down inside against the ropes
    constexpr uint32_t kTriangle = 0x1000u;
    constexpr uint16_t kRing = 4u, kApron = 8u, kFloor = 0x10u; // a fighter's place +0x5AC

    PS2Runtime::RecompiledFunction g_grabPair = nullptr, g_topRopeMove = nullptr;

    bool cageMatch(uint8_t *rdram) { return (Ps2FastRead16(rdram, kRuleWord) & vendetta_cage::kRuleCage) != 0u; }
    uint16_t place(uint8_t *rdram, uint32_t fighter) { return Ps2FastRead16(rdram, fighter + 0x5ACu); }
    uint32_t player(uint8_t *rdram, uint32_t fighter) { return Ps2FastRead32(rdram, fighter + 0x30u) & 3u; }
    void answer(R5900Context *ctx, uint64_t value)
    {
        SET_GPR_U64(ctx, 2, value);
        ctx->pc = getRegU32(ctx, 31);
    }
    void callGame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t function)
    {
        runtime->lookupFunction(function)(rdram, ctx, runtime); // whatever is there now (a test's own hook too)
    }
    void kept(uint8_t *rdram, uint32_t fighter, const char *what)
    {
        std::cerr << "[cage] player " << player(rdram, fighter) << " kept inside: " << what << std::endl;
    }

    // 0x123354, the fighter $s0; the delay slot (andi $v0, $v0, 0x1000) has run: $v0 = the triangle held, masked
    void stepOut(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t fighter = getRegU32(ctx, 16);
        const uint32_t held = Ps2FastRead32(rdram, fighter + 0x668u) & kTriangle;
        if (held && cageMatch(rdram))
        {
            kept(rdram, fighter, "out through the ropes onto the apron, not on down to the floor");
            answer(ctx, 0u);
            return;
        }
        answer(ctx, held);
    }

    // 0x115494, the wall ahead $a0, the runner $s0; the delay slot (andi $v0, $v0, 0x8) has run: $v0 = may he run out
    // over it
    void runOut(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t wall = getRegU32(ctx, 4);
        const uint32_t allowed = Ps2FastRead16(rdram, wall + 0x44u) & 8u;
        if (allowed && (Ps2FastRead16(rdram, wall + 0x42u) & 0x20u) && cageMatch(rdram))
        {
            static uint32_t told = 0;
            if (told++ % 60u == 0u) // asked each picture of a run near a rope: a line now and then
                kept(rdram, getRegU32(ctx, 16), "no run out over the ropes: the rope rebounds him");
            answer(ctx, 0u);
            return;
        }
        answer(ctx, allowed);
    }

    // 0x128564, the fighter $s0: state 19's chance of going over the ropes
    void staggerChance(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (cageMatch(rdram))
        {
            kept(rdram, getRegU32(ctx, 16), "the big stagger into the ropes: not over them (stunned on them, 21, or the stagger)");
            answer(ctx, 0u);
            return;
        }
        callGame(rdram, ctx, runtime, kChance);
    }

    // 0x1150FC, FUN_00101680(runner $a0, state $a1): the dodged runner at a rope goes over it (20); a cage rebounds him
    void whipState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (getRegU32(ctx, 5) == kOverTheRopes && cageMatch(rdram))
        {
            kept(rdram, getRegU32(ctx, 4), "the dodged whip into the ropes: rebounds (11), not over them");
            SET_GPR_U32(ctx, 5, kRebound);
        }
        callGame(rdram, ctx, runtime, kSetState);
    }

    // 0x123AFC and 0x123B5C: the clip state 20 starts ($a0 the fighter, $a1 the clip, $a2 the flags)
    void apronFall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (cageMatch(rdram))
        {
            kept(rdram, getRegU32(ctx, 4), "knocked off the apron: staggers on it (W105)");
            SET_GPR_U32(ctx, 5, kW105);
        }
        callGame(rdram, ctx, runtime, kPlayClip);
    }
    void stunnedFall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (cageMatch(rdram))
        {
            kept(rdram, getRegU32(ctx, 4), "knocked over the ropes while stunned on them: down inside (I406)");
            SET_GPR_U32(ctx, 5, kI406);
        }
        callGame(rdram, ctx, runtime, kPlayClip);
    }

    // 0x123B74 (state 20, the fighter $s0) and 0x100E78 (state 21 step 8, the fighter $s0): the chance of the floor
    void fallChance(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (cageMatch(rdram))
        {
            kept(rdram, getRegU32(ctx, 16), "knocked over the ropes (not foreseen): onto the apron (I307 / I308)");
            answer(ctx, 0u);
            return;
        }
        callGame(rdram, ctx, runtime, kChance);
    }
    void stunnedChance(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (cageMatch(rdram))
        {
            kept(rdram, getRegU32(ctx, 16), "hit while stunned on the ropes: down inside (I406)");
            answer(ctx, 0u);
            return;
        }
        callGame(rdram, ctx, runtime, kChance);
    }

    // 0x11D4EC: the rope for a catch at an edge (slot 41, GETTING PAID: the runner ends on the floor)
    void edgeCatch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (cageMatch(rdram))
        {
            answer(ctx, ~0ull); // -1: no rope, the ordinary catch
            return;
        }
        callGame(rdram, ctx, runtime, kEdgeRope);
    }

    // FUN_0011db38(grabber, grabbed): the pairs that would take someone across the ropes to the floor answered "none"
    void grabPair(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (cageMatch(rdram))
        {
            const uint32_t grabber = getRegU32(ctx, 4), grabbed = getRegU32(ctx, 5);
            const uint16_t from = place(rdram, grabber), to = place(rdram, grabbed);
            const uint16_t flags = Ps2FastRead16(rdram, grabber + 0x5A6u);
            const char *refused = nullptr;
            if ((from & kApron) && (to & kRing) && !(Ps2FastRead8(rdram, grabbed + 0x5C0u) & 6u) && !(flags & 0x200u))
                refused = "no grapple from the apron across the ropes (37 / 38: the attacker drops to the floor)";
            else if ((from & kRing) && (to & kApron) && !(flags & 2u))
                refused = "no weak grapple across the ropes at the apron (35: thrown to the floor)";
            else if ((from & kFloor) && (to & (kRing | kApron)))
                refused = "no grapple from the floor pulling someone out (74-77)";
            if (refused)
            {
                kept(rdram, grabbed, refused);
                answer(ctx, 0u);
                return;
            }
        }
        g_grabPair(rdram, ctx, runtime);
    }

    // FUN_0013a050(fighter): no top-rope move at an opponent on the apron or the floor (the diver would land outside)
    void topRopeMove(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (cageMatch(rdram))
        {
            const uint32_t fighter = getRegU32(ctx, 4), opponent = Ps2FastRead32(rdram, fighter);
            if (opponent && (place(rdram, opponent) & (kApron | kFloor)))
            {
                static uint32_t told = 0;
                if (told++ % 300u == 0u) // asked each picture on a turnbuckle: a line now and then
                    kept(rdram, fighter, "no top-rope move at an opponent outside the ring");
                answer(ctx, 0u);
                return;
            }
        }
        g_topRopeMove(rdram, ctx, runtime);
    }

    void install(PS2Runtime &runtime)
    {
        // the patched instructions (config/patches.toml) have only these answers: always registered
        runtime.registerFunction(kHookStepOut, stepOut);
        runtime.registerFunction(kHookRunOut, runOut);
        runtime.registerFunction(kHookApronFall, apronFall);
        runtime.registerFunction(kHookStunnedFall, stunnedFall);
        runtime.registerFunction(kHookFallChance, fallChance);
        runtime.registerFunction(kHookStunnedChance, stunnedChance);
        runtime.registerFunction(kHookEdgeCatch, edgeCatch);
        runtime.registerFunction(kHookStaggerChance, staggerChance);
        runtime.registerFunction(kHookWhipState, whipState);
        g_grabPair = runtime.lookupFunction(kGrabPair);
        g_topRopeMove = runtime.lookupFunction(kTopRopeMove);
        if (g_grabPair)
            runtime.replaceFunction(kGrabPair, grabPair);
        if (g_topRopeMove)
            runtime.replaceFunction(kTopRopeMove, topRopeMove);
    }
}

void vendetta_cage::matchStarting()
{
    using namespace vendetta_cage_walls;
    g_newMatch = true;
    leaveMatchStarting();
    cpuMatchStarting();
}

uint32_t vendetta_cage::walls(uint8_t *rdram, uint32_t fighter, uint32_t gameList, uint32_t list)
{
    using namespace vendetta_cage_walls;
    climbWatch(rdram, fighter);
    if (g_newMatch)
    {
        g_newMatch = false;
        putGameValuesBack();
        g_listsDone.clear();
        g_cageLists.clear();
        g_listsUsed = 0;
        std::memset(g_onFloor, 0, sizeof(g_onFloor));
        std::memset(g_held, 0, sizeof(g_held));
        std::memset(g_onApron, 0, sizeof(g_onApron));
        g_cageSides = g_heldCount = 0;
        g_cage = (Ps2FastRead16(rdram, kMatch + 4u) & kRuleCage) != 0u && g_area != 0u;
        if (g_cage)
            buildCage(rdram);
    }
    if (!g_cage || list == 0u)
        return list;
    logFloor(rdram, fighter);
    if (g_listsDone.insert(list).second)
        changeWalls(list);
    if (gameList - kGameFloorLists < 0x40u)
        return cageList(list);
    return list;
}

bool vendetta_cage::resultLists(const int32_t *winners, int winnerCount, const int32_t *losers, int loserCount,
                                uint32_t &winnersAt, uint32_t &losersAt)
{
    using namespace vendetta_cage_walls;
    static_assert(kLists + kMaxLists * kListBytes <= kResultLists, "the escape's lists past the walls' lists");
    if (!g_area || winnerCount < 1 || winnerCount > 4 || loserCount < 1 || loserCount > 4)
        return false;
    winnersAt = g_area + kResultLists;
    losersAt = winnersAt + 16u;
    for (int i = 0; i < 4; ++i)
    {
        write32(winnersAt + 4u * static_cast<uint32_t>(i), static_cast<uint32_t>(i < winnerCount ? winners[i] : -1));
        write32(losersAt + 4u * static_cast<uint32_t>(i), static_cast<uint32_t>(i < loserCount ? losers[i] : -1));
    }
    return true;
}

void installVendettaCageLeave(PS2Runtime &runtime); // vendetta_cage_leave.cpp: who a fall puts out, the way out by the door
void installVendettaCageCpu(PS2Runtime &runtime);   // vendetta_cage_cpu.cpp: the computer in a cage match

void installVendettaCage(PS2Runtime &runtime)
{
    vendetta_cage_fight::install(runtime);
    installVendettaCageLeave(runtime);
    installVendettaCageCpu(runtime);
    vendetta_cage_walls::g_area = vendetta_store::fixed(vendetta_cage_walls::kRecords, vendetta_cage_walls::kAreaEnd -
                                                        vendetta_cage_walls::kRecords, "the cage's walls");
    if (vendetta_cage_walls::g_area)
        vendetta_cage_walls::g_area -= vendetta_cage_walls::kRecords; // offsets from the fixed area's 0x000
    else
        std::cerr << "[cage] no room for the cage's walls: cage matches have the game's walls" << std::endl;
}
