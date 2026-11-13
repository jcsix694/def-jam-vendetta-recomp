// Def Jam Vendetta (USA, SLUS-20639) - the cage match: who a fall puts out, and the way out by the cage's door.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// The game decides a fall in three routines. FUN_00135a18(bit), three times a picture while the fight is on (1 a pin, 4
// a knockout, 2 a submission), takes each fighter with the bit in +0x5BB: the winners are {his opponent, -1}, the
// losers {him, -1}; it asks FUN_00135b38(him), which marks him out of the match (+0x5BB |= 0x10) and says whether the
// match is over (one on one, tag and handicap: at once; three or four all against all: when fewer than two are left);
// then FUN_00135bb8(winners, losers, kind | 1 when over, 1) writes the result and puts each loser out of play.
//
// In a cage match:
// - Three or four all against all is first man wins: FUN_00135b38 answers "over" at the first fall, and the result's
//   losers are everyone but the winner (with the match decided, 0x22FDE4 0x1000, before the result, so no loser left
//   standing starts the game's walk out).
// - Tag and handicap are last man wins: a fall on someone with a team-mate still in the ring doesn't end the match
//   (FUN_00135b38 answers "not over"; the game's own result for one put out runs, as in all against all). The one put
//   out leaves by the cage's door, the game doing it all: nobody can hit, grab or target them from the fall on.
//     1. Up from the mat by the game's own lying state. That state rolls someone out of the match out under the ropes
//        (state 121) instead of getting them up, so while they are down the out-of-the-match bit is taken off; a
//        knockout is kept down first (the lying state's count topped up) for cage.json's wait.
//     2. Standing, the bit goes back on and the game's own idle starts its walk out (state 120: the walk clip, turning
//        toward the angle +0x540 until the ropes are reached, then state 14). Here the angle is, each picture, the
//        way to the ropes in front of the door.
//     3. At the ropes (state 14 starting) the door swings open; the wrestler stands until it is.
//     4. The game's state 14: out through the ropes and, being out of the match, on down to the floor, through the
//        doorway.
//     5. On the floor the door shuts, and the wrestler is made to wait outside as one over the top for the team does
//        (vendetta_cage_climb.cpp), first walking to a place beside the door.
// The door's place comes from the cage's model (vendetta_cage_draw.cpp); a model without one: the same, through the
// middle of the side the door would be on (nothing swings).
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "vendetta_cage.h"
#include "vendetta_cage_file.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace vendetta_cage_leave
{
    constexpr uint32_t kReport = 0x135A18u;       // FUN_00135a18(bit): the falls of this picture reported
    constexpr uint32_t kMarkOut = 0x135B38u;      // FUN_00135b38(fighter number): out of the match; 1 = the match is over
    constexpr uint32_t kResult = 0x135BB8u;       // FUN_00135bb8(winners, losers, kind, 1): the result
    constexpr uint32_t kWalkOut = 0x123D58u;      // FUN_00123d58: state 120, someone out of the match walking to the ropes
    constexpr uint32_t kThroughRopes = 0x123170u; // FUN_00123170: state 14, out through the ropes
    constexpr uint32_t kPlayClip = 0x132158u;     // FUN_00132158(fighter, clip, flags)
    constexpr uint32_t kSetState = 0x101680u;     // FUN_00101680(fighter, state)

    constexpr uint32_t kFighters = 0x22FE08u, kFighterBytes = 0xCD8u;
    constexpr uint32_t kRuleWord = 0x2361CCu, kMatchKind = 0x2361CEu; // the kind: 1 one on one, 2 tag, 4 / 8 all against
                                                                      // all, 0x10 / 0x20 handicap
    constexpr uint32_t kMatchFlags = 0x22FDE4u, kDecided = 0x1000u;
    constexpr uint32_t kMatchStep = 0x22FBD4u;    // this picture's time step (1 at full speed)

    // the fighter's fields
    constexpr uint32_t kPlayer = 0x30u, kState = 0x40u, kStep = 0x44u, kX = 0xCCu, kZ = 0xD4u, kFacing = 0xDCu,
                       kFacingToo = 0x148u, kClip = 0xFCu, kDownCount = 0x3F4u, kWalkAngle = 0x540u, kPlace = 0x5ACu,
                       kAtEdge = 0x79Cu,
                       kOut = 0x5BBu, kPlay = 0x5BCu, kStyle = 0x810u;
    constexpr uint16_t kInRing = 4u, kOnApron = 8u, kOnFloor = 0x10u, kCantBeGrabbed = 0x1u, kCantBeHit = 0x4000u;
    constexpr uint8_t kOutOfMatch = 0x10u, kInPlay = 2u, kTeamB = 4u;
    constexpr uint32_t kStanding = 2u, kLying = 0x16u, kWalkingOut = 0x78u;
    constexpr uint32_t kStanceClips = 0x2710u;    // standing: the clip 0x2710 + the fighter's style
    constexpr float kRopesFromCage = 5.0f;        // the ropes stand this far inside the cage's walls
    constexpr float kLineUp = 12.0f;              // the walk first goes to a place this far in from the ropes, in front
                                                  // of the door, then straight out at it
    constexpr float kOutsideTheWall = 7.5f, kFirstPlace = 6.0f, kPlaceApart = 9.0f; // standing beside the door

    PS2Runtime::RecompiledFunction g_report = nullptr, g_markOut = nullptr, g_result = nullptr, g_walkOut = nullptr,
                                   g_throughRopes = nullptr;

    enum class Stage { None, Down, Walking, AtDoor, Through, Aside, Outside };
    struct Leaver
    {
        Stage stage = Stage::None;
        float wait = 0.0f;       // a knockout: pictures still to stay down
        uint32_t walkClip = 0;   // the game's walk clip, as its walk out played it
        float standX = 0.0f, standZ = 0.0f;
        bool standing = false;   // outside: the stance started
        bool linedUp = false;    // the walk has reached the place in front of the door
        int pictures = 0;
    };
    Leaver g_leavers[4];
    uint32_t g_bit = 0;          // the fall being reported: 1 a pin, 2 a submission, 4 a knockout

    bool cageMatch(uint8_t *rdram) { return (Ps2FastRead16(rdram, kRuleWord) & vendetta_cage::kRuleCage) != 0u; }
    uint32_t fighterAt(uint32_t i) { return kFighters + kFighterBytes * i; }
    int indexOf(uint32_t fighter)
    {
        const uint32_t i = (fighter - kFighters) / kFighterBytes;
        return (fighter >= kFighters && i < 4u) ? static_cast<int>(i) : -1;
    }
    float readFloat(uint8_t *rdram, uint32_t a) { float f; uint32_t v = Ps2FastRead32(rdram, a); std::memcpy(&f, &v, 4); return f; }
    void writeFloat(uint8_t *rdram, uint32_t a, float f) { uint32_t v; std::memcpy(&v, &f, 4); Ps2FastWrite32(rdram, a, v); }
    void answer(R5900Context *ctx, uint64_t value)
    {
        SET_GPR_U64(ctx, 2, value);
        ctx->pc = getRegU32(ctx, 31);
    }
    bool inMatch(uint8_t *rdram, uint32_t f) { return static_cast<int32_t>(Ps2FastRead32(rdram, f + kPlayer)) != -1; }
    const char *fallName(uint32_t bit) { return bit == 1u ? "a pin" : bit == 2u ? "a submission" : bit == 4u ? "a knockout" : "a fall"; }

    // a team-mate of this fighter still fighting in the ring: in the match, in play, not out by a fall, not waiting
    // outside after going over the top
    bool teamMateInRing(uint8_t *rdram, uint32_t fighter)
    {
        for (uint32_t j = 0; j < 4u; ++j)
        {
            const uint32_t other = fighterAt(j);
            if (other == fighter || !inMatch(rdram, other))
                continue;
            if (((Ps2FastRead8(rdram, other + kPlay) ^ Ps2FastRead8(rdram, fighter + kPlay)) & kTeamB) != 0u)
                continue;
            if (!(Ps2FastRead8(rdram, other + kPlay) & kInPlay) || (Ps2FastRead8(rdram, other + kOut) & kOutOfMatch) ||
                g_leavers[j].stage != Stage::None || vendetta_cage::waitingOutside(rdram, other))
                continue;
            return true;
        }
        return false;
    }

    // ---- the door: where the way out is. The model's door, else the middle of the side toward the game's -z
    struct Way
    {
        float midX, midZ;   // the doorway's middle, on the cage's wall
        float alongX, alongZ; // along the wall, from the latch's end to the hinges' and on
        float outX, outZ;   // out of the cage
        float hingeX, hingeZ;
    };
    Way way()
    {
        float hx = 0.0f, hz = 0.0f, lx = 0.0f, lz = 0.0f;
        if (!vendetta_cage::door(hx, hz, lx, lz))
        {
            const float wall = vendetta_cage::cageFile().numbers.walls;
            hx = -6.0f, lx = 6.0f, hz = lz = -wall;
        }
        Way w{};
        w.midX = (hx + lx) * 0.5f, w.midZ = (hz + lz) * 0.5f;
        float ax = hx - lx, az = hz - lz;
        const float len = std::max(std::sqrt(ax * ax + az * az), 0.001f);
        w.alongX = ax / len, w.alongZ = az / len;
        w.outX = -w.alongZ, w.outZ = w.alongX;
        if (w.outX * w.midX + w.outZ * w.midZ < 0.0f)
            w.outX = -w.outX, w.outZ = -w.outZ;
        w.hingeX = hx, w.hingeZ = hz;
        return w;
    }

    // ---- FUN_00135a18(bit): the fall's kind remembered for the answers below; and, once a picture (its first call),
    // those leaving moved on
    void everyPicture(uint8_t *rdram);
    void report(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_bit = getRegU32(ctx, 4) & 0xFFu;
        if (g_bit == 1u && cageMatch(rdram))
            everyPicture(rdram);
        g_report(rdram, ctx, runtime);
    }

    // ---- FUN_00135b38(fighter number): out of the match; is the match over?
    void markOut(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t number = getRegU32(ctx, 4);
        const uint8_t kind = Ps2FastRead8(rdram, kMatchKind);
        if (!cageMatch(rdram) || number >= 4u || !(kind & 0x3Eu))
        {
            g_markOut(rdram, ctx, runtime);
            return;
        }
        const uint32_t fighter = fighterAt(number);
        Ps2FastWrite8(rdram, fighter + kOut, static_cast<uint8_t>(Ps2FastRead8(rdram, fighter + kOut) | kOutOfMatch));
        if (kind & 0xCu)
        {
            std::cerr << "[cage] " << fallName(g_bit) << " on player " << number
                      << ": all against all in a cage, the first fall ends the match" << std::endl;
            answer(ctx, 1u);
            return;
        }
        if (!teamMateInRing(rdram, fighter))
        {
            std::cerr << "[cage] " << fallName(g_bit) << " on player " << number
                      << ", the last of the team in the ring: the match is over" << std::endl;
            answer(ctx, 1u);
            return;
        }
        Leaver &l = g_leavers[number];
        l = Leaver();
        l.stage = Stage::Down;
        l.wait = g_bit == 4u ? vendetta_cage::cageFile().leave.knockoutWait : 0.0f;
        std::cerr << "[cage] " << fallName(g_bit) << " on player " << number
                  << ", a team-mate still in the ring: out of the match, the match goes on; leaving by the door"
                  << (l.wait > 0.0f ? " after the knockout's wait" : "") << std::endl;
        answer(ctx, 0u);
    }

    // ---- FUN_00135bb8(winners, losers, kind, 1): all against all decided by its first fall, everyone but the winner a
    // loser; any decided result: those on the way out are out of the match again for the ending
    void result(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t decided = getRegU32(ctx, 6) & 1u;
        if (cageMatch(rdram) && decided)
        {
            bool goingThrough = false;
            for (const Leaver &l : g_leavers)
                goingThrough = goingThrough || l.stage == Stage::Through;
            if (!goingThrough) // shut for the ending, unless someone is in the doorway
                vendetta_cage::doorTo(false, vendetta_cage::cageFile().leave.doorPictures);
            for (uint32_t j = 0; j < 4u; ++j)
                if (g_leavers[j].stage == Stage::Down || g_leavers[j].stage == Stage::Walking ||
                    g_leavers[j].stage == Stage::AtDoor)
                {
                    const uint32_t f = fighterAt(j);
                    Ps2FastWrite8(rdram, f + kOut, static_cast<uint8_t>(Ps2FastRead8(rdram, f + kOut) | kOutOfMatch));
                    g_leavers[j].stage = Stage::None;
                }
            const uint32_t winners = getRegU32(ctx, 4), losers = getRegU32(ctx, 5);
            if ((Ps2FastRead8(rdram, kMatchKind) & 0xCu) && winners && losers)
            {
                const int32_t winner = static_cast<int32_t>(Ps2FastRead32(rdram, winners));
                const int32_t first = static_cast<int32_t>(Ps2FastRead32(rdram, losers));
                const int32_t won[2] = {winner, -1};
                int32_t lost[5] = {first, -1, -1, -1, -1};
                int count = first == -1 ? 0 : 1;
                for (int32_t j = 0; j < 4; ++j)
                    if (j != winner && j != first && inMatch(rdram, fighterAt(static_cast<uint32_t>(j))) && count < 3)
                        lost[count++] = j;
                uint32_t winnersAt = 0, losersAt = 0;
                if (winner >= 0 && winner < 4 && count > 0 &&
                    vendetta_cage::resultLists(won, 2, lost, count + 1, winnersAt, losersAt))
                {
                    SET_GPR_U64(ctx, 4, winnersAt);
                    SET_GPR_U64(ctx, 5, losersAt);
                    Ps2FastWrite16(rdram, kMatchFlags, static_cast<uint16_t>(Ps2FastRead16(rdram, kMatchFlags) | kDecided));
                    std::cerr << "[cage] first man wins: player " << winner << "; the losers";
                    for (int k = 0; k < count; ++k)
                        std::cerr << " " << lost[k];
                    std::cerr << std::endl;
                }
            }
        }
        g_result(rdram, ctx, runtime);
    }

    // someone out of the match in a cage whose match has just been decided stops where they are
    bool stopForTheEnding(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t fighter)
    {
        if (!cageMatch(rdram) || !(Ps2FastRead16(rdram, kMatchFlags) & kDecided) ||
            !(Ps2FastRead16(rdram, fighter + kPlace) & kInRing))
            return false;
        SET_GPR_U64(ctx, 4, fighter);
        SET_GPR_U64(ctx, 5, kStanding);
        runtime->lookupFunction(kSetState)(rdram, ctx, runtime);
        return true;
    }

    // how far the fighter is from the doorway's middle, along the wall and in from it
    void fromDoor(uint8_t *rdram, uint32_t fighter, const Way &w, float &along, float &in)
    {
        const float rx = readFloat(rdram, fighter + kX) - w.midX, rz = readFloat(rdram, fighter + kZ) - w.midZ;
        along = rx * w.alongX + rz * w.alongZ;
        in = -(rx * w.outX + rz * w.outZ);
    }

    // ---- FUN_00123d58, state 120: the walk out's direction is first the way to a place in front of the door, a few
    // steps in from the ropes, then straight out at the door (walked at from the side, the ropes would be reached
    // beside it)
    void walkOut(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t fighter = getRegU32(ctx, 4);
        const int index = indexOf(fighter);
        if (index >= 0 && Ps2FastRead32(rdram, fighter + kStep) == 0u && stopForTheEnding(rdram, ctx, runtime, fighter))
            return;
        if (index >= 0 && cageMatch(rdram) && g_leavers[index].stage == Stage::Walking &&
            Ps2FastRead32(rdram, fighter + kStep) == 1u)
        {
            if (stopForTheEnding(rdram, ctx, runtime, fighter))
                return;
            Leaver &l = g_leavers[index];
            const Way w = way();
            float along = 0.0f, in = 0.0f;
            fromDoor(rdram, fighter, w, along, in);
            const float lineIn = kRopesFromCage + kLineUp;
            if (!l.linedUp && std::fabs(along) < 1.5f && in < lineIn + 6.0f)
                l.linedUp = true;
            const float tx = l.linedUp ? w.midX : w.midX - w.outX * lineIn;
            const float tz = l.linedUp ? w.midZ : w.midZ - w.outZ * lineIn;
            const float dx = tx - readFloat(rdram, fighter + kX), dz = tz - readFloat(rdram, fighter + kZ);
            if (dx * dx + dz * dz > 0.01f)
                writeFloat(rdram, fighter + kWalkAngle, std::atan2(dx, dz) * 180.0f / 3.14159265f);
            g_leavers[index].walkClip = Ps2FastRead32(rdram, fighter + kClip);
        }
        g_walkOut(rdram, ctx, runtime);
    }

    // ---- FUN_00123170, state 14: at the ropes the door swings open first, the wrestler standing until it is
    void throughRopes(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t fighter = getRegU32(ctx, 4);
        const int index = indexOf(fighter);
        if (index >= 0 && cageMatch(rdram))
        {
            Leaver &l = g_leavers[index];
            const bool starting = Ps2FastRead32(rdram, fighter + kStep) == 0u &&
                                  (Ps2FastRead16(rdram, fighter + kPlace) & kInRing);
            if (starting && (Ps2FastRead8(rdram, fighter + kOut) & kOutOfMatch) &&
                stopForTheEnding(rdram, ctx, runtime, fighter))
                return;
            if (l.stage == Stage::Walking && starting)
            {
                // the ropes reached somewhere else (up from the mat beside them): not out there, the walk goes on
                const Way w = way();
                float along = 0.0f, in = 0.0f;
                fromDoor(rdram, fighter, w, along, in);
                const float half = std::sqrt((w.hingeX - w.midX) * (w.hingeX - w.midX) + (w.hingeZ - w.midZ) * (w.hingeZ - w.midZ));
                if (std::fabs(along) > half - 2.0f || in > kRopesFromCage + 8.0f)
                {
                    Ps2FastWrite16(rdram, fighter + kAtEdge, 0u);
                    SET_GPR_U64(ctx, 4, fighter);
                    SET_GPR_U64(ctx, 5, kWalkingOut);
                    runtime->lookupFunction(kSetState)(rdram, ctx, runtime);
                    return;
                }
            }
            if (l.stage == Stage::Walking && starting)
            {
                l.stage = Stage::AtDoor;
                std::cerr << "[cage] player " << index << " is at the ropes in front of the door ("
                          << readFloat(rdram, fighter + kX) << ", " << readFloat(rdram, fighter + kZ)
                          << "): the door opens" << std::endl;
            }
            if (l.stage == Stage::AtDoor)
            {
                if (vendetta_cage::doorOpen() < 0.999f)
                {
                    // standing at the ropes while it swings: the stance, the routine's one call
                    SET_GPR_U64(ctx, 4, fighter);
                    SET_GPR_U64(ctx, 5, kStanceClips + Ps2FastRead32(rdram, fighter + kStyle));
                    SET_GPR_U64(ctx, 6, 0xEu);
                    runtime->lookupFunction(kPlayClip)(rdram, ctx, runtime);
                    return;
                }
                l.stage = Stage::Through;
                std::cerr << "[cage] the door is open: player " << index << " goes out through the ropes" << std::endl;
            }
        }
        g_throughRopes(rdram, ctx, runtime);
    }

    // ---- once a picture: each one leaving kept out of reach and moved on; the door swung
    void everyPicture(uint8_t *rdram)
    {
        const float step = readFloat(rdram, kMatchStep);
        bool open = false;
        for (uint32_t i = 0; i < 4u; ++i)
        {
            Leaver &l = g_leavers[i];
            if (l.stage == Stage::None || l.stage == Stage::Outside)
                continue;
            const uint32_t f = fighterAt(i);
            const uint32_t state = Ps2FastRead32(rdram, f + kState);
            const uint16_t place = Ps2FastRead16(rdram, f + kPlace);
            ++l.pictures;
            if (l.stage != Stage::Aside)
            {
                Ps2FastWrite16(rdram, f + kPlace, static_cast<uint16_t>(place | kCantBeHit | kCantBeGrabbed));
                vendetta_cage::notTargeted(rdram, f);
            }
            switch (l.stage)
            {
            case Stage::Down:
                if (l.wait > 0.0f)
                {
                    l.wait -= step;
                    if (state == kLying)
                        writeFloat(rdram, f + kDownCount, 30.0f); // kept down: the lying state's count topped up
                }
                if (state == kStanding && (place & kInRing) && l.wait <= 0.0f)
                {
                    Ps2FastWrite8(rdram, f + kOut, static_cast<uint8_t>(Ps2FastRead8(rdram, f + kOut) | kOutOfMatch));
                    l.stage = Stage::Walking;
                    std::cerr << "[cage] player " << i << " is up (" << l.pictures
                              << " pictures after the fall): walking to the door" << std::endl;
                }
                else // not out of the match in the game's eyes while down: its lying state then gets them up
                    Ps2FastWrite8(rdram, f + kOut, static_cast<uint8_t>(Ps2FastRead8(rdram, f + kOut) & ~kOutOfMatch));
                break;
            case Stage::Walking:
                break;
            case Stage::AtDoor:
            case Stage::Through:
                open = true;
                if ((place & kOnFloor) && state == kStanding)
                {
                    // outside: a place beside the door, the hinges' side, further along for each one already there
                    const Way w = way();
                    int there = 0;
                    for (const Leaver &o : g_leavers)
                        if (o.stage == Stage::Aside || o.stage == Stage::Outside)
                            ++there;
                    const float along = kFirstPlace + kPlaceApart * static_cast<float>(there);
                    l.standX = w.hingeX + w.alongX * along + w.outX * kOutsideTheWall;
                    l.standZ = w.hingeZ + w.alongZ * along + w.outZ * kOutsideTheWall;
                    l.standing = false;
                    l.stage = Stage::Aside;
                    open = false;
                    vendetta_cage::waitOutside(rdram, f);
                    std::cerr << "[cage] player " << i << " is outside (" << readFloat(rdram, f + kX) << ", "
                              << readFloat(rdram, f + kZ) << "): the door shuts; to stand at (" << l.standX << ", "
                              << l.standZ << ")" << std::endl;
                }
                break;
            default:
                break;
            }
        }
        vendetta_cage::doorTo(open, vendetta_cage::cageFile().leave.doorPictures);
    }

    void install(PS2Runtime &runtime)
    {
        g_report = runtime.lookupFunction(kReport);
        g_markOut = runtime.lookupFunction(kMarkOut);
        g_result = runtime.lookupFunction(kResult);
        g_walkOut = runtime.lookupFunction(kWalkOut);
        g_throughRopes = runtime.lookupFunction(kThroughRopes);
        if (!g_report || !g_markOut || !g_result || !g_walkOut || !g_throughRopes)
        {
            std::cerr << "[cage] the game's fall and walk-out routines weren't found: falls in a cage are the game's own"
                      << std::endl;
            return;
        }
        runtime.replaceFunction(kReport, report);
        runtime.replaceFunction(kMarkOut, markOut);
        runtime.replaceFunction(kResult, result);
        runtime.replaceFunction(kWalkOut, walkOut);
        runtime.replaceFunction(kThroughRopes, throughRopes);
    }
}

void vendetta_cage::leaveMatchStarting()
{
    using namespace vendetta_cage_leave;
    for (Leaver &l : g_leavers)
        l = Leaver();
    g_bit = 0;
    vendetta_cage::doorTo(false, 1.0f);
}

bool vendetta_cage::outByFall(uint8_t *rdram, uint32_t fighter)
{
    using namespace vendetta_cage_leave;
    const int index = indexOf(fighter);
    return index >= 0 && g_leavers[index].stage != Stage::None && cageMatch(rdram);
}

// the walk from the doorway to the place beside the door, then the stance facing the ring
uint32_t vendetta_cage::leaveAside(uint8_t *rdram, uint32_t fighter, uint32_t &flags)
{
    using namespace vendetta_cage_leave;
    const int index = indexOf(fighter);
    if (index < 0 || g_leavers[index].stage != Stage::Aside)
        return 0u;
    Leaver &l = g_leavers[index];
    const uint32_t stance = kStanceClips + Ps2FastRead32(rdram, fighter + kStyle);
    if (vendetta_cage::doorOpen() > 0.3f) // the door still swinging shut across the way
    {
        flags = 0xEu;
        return stance;
    }
    const float x = readFloat(rdram, fighter + kX), z = readFloat(rdram, fighter + kZ);
    const float dx = l.standX - x, dz = l.standZ - z;
    const float left = std::sqrt(dx * dx + dz * dz);
    const float step = vendetta_cage::cageFile().leave.walkOutside * readFloat(rdram, kMatchStep);
    if (left <= step)
    {
        const float toMiddle = std::atan2(-l.standX, -l.standZ) * 180.0f / 3.14159265f;
        writeFloat(rdram, fighter + kX, l.standX);
        writeFloat(rdram, fighter + kZ, l.standZ);
        writeFloat(rdram, fighter + kFacing, toMiddle);
        writeFloat(rdram, fighter + kFacingToo, toMiddle);
        l.stage = Stage::Outside;
        std::cerr << "[cage] player " << index << " stands beside the door, facing the ring" << std::endl;
        flags = 2u;
        return stance;
    }
    const float facing = std::atan2(dx, dz) * 180.0f / 3.14159265f;
    writeFloat(rdram, fighter + kX, x + dx / left * step);
    writeFloat(rdram, fighter + kZ, z + dz / left * step);
    writeFloat(rdram, fighter + kFacing, facing);
    writeFloat(rdram, fighter + kFacingToo, facing);
    flags = 0xEu;
    return l.walkClip ? l.walkClip : stance;
}

void installVendettaCageLeave(PS2Runtime &runtime) { vendetta_cage_leave::install(runtime); }
