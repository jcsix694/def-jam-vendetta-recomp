// Def Jam Vendetta (USA, SLUS-20639) - the cage match: throws into the cage.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// In a grapple, triangle with a direction: the lock-up's commit FUN_0011f1f8 (+0x738 0x1000, not from behind) calls
// FUN_00122370, the game's throw over the top rope (an edge 10.5 ahead with the rope's 0x10: states 68 / 69) or against
// the ring's side from outside (70 / 71). In a cage match the ropes' 0x10 is off (vendetta_cage.cpp), so that throw never
// happens; FUN_00122370 is wrapped here, and in a cage match, both in the ring, it is a throw into the cage instead:
// - the wall: the one the stick points at (+0x734, the directions held now or last picture: up -z, right +x, down +z,
//   left -x; the nearest of the ring's four or six sides by its outward normal), with no direction the one behind the
//   thrower (the game's own pick for its throws);
// - the reversal: the held one's try in the lock-up passed (+0x5C4 bit 4, a press while held, a chance by momentum,
//   FUN_0011f848: as every grapple's), read and cleared here;
// - both put in the climb's state 102 (its routine, vendetta_cage_climb.cpp, asks throwState first) as the game pairs
//   two fighters, FUN_00101728 (the thrower leads), the wrapper's last call; then it answers 1 (a state was set).
// The throw (the cage's clips, converted from WWF No Mercy's cage set; both wrestlers' anchors and facings are ours,
// the clips move the bodies from them). Measured in the clips' own space (the hips' place and the way the body faces,
// frame by frame), every one starts from the lock-up's own arrangement, the two facing each other kApart apart, and
// throws the held one the way the thrower's back points. So the pair is turned (and slid, over kTurnPictures) until
// the thrower stands between the held one and the wall, its back to the wall:
// - close (the held one's reach in 3218, 13 straight ahead, fits within kCloseSlack of where it stands, or it stands
//   nearer): the whip into the cage, the thrower 3217 stepping aside and swinging the held one, 3218, past it into the
//   cage, the pair placed so the held one's farthest place is kHipsAtCage from the middle;
// - (both, close or far: the pair stands nearer the middle when that would put a part of either body through the cage's
//   wires in the clips at the cage: the climb's room points and rooms, cage.json's "the wires", measureKeptOff)
// - farther: the grab 321B / 321C (the thrower turns round to face the cage, the held one's head under its arm), the
//   run 321D / 321E looped, the thrower ahead pulling the held one along behind, both moved toward the cage at
//   kRunSpeed a picture until the held one's reach in 3220 fits, then the ram 321F / 3220: the held one swung forward
//   past the thrower, head first into the cage.
// Reversed (the held one's try passed): No Mercy's reversals carry on from part way through the throw and put the
// thrower down on the mat behind, back toward the ring's middle, the reverser stepping to the cage: the whip plays to
// kReverseAt of 3217 / 3218 and goes on as 3219 (the thrower thrown down) / 321A; the run goes to the cage as ever and
// 3221 (the thrower thrown down) / 3222 play in place of the ram. The thrower takes the hit and lies; nobody meets the
// cage.
// At the thrown one's clip end (the held one, or reversed the thrower) the lying landing (ending 51, lying face up) is written at +0x38C and the game's own
// clip-end step FUN_00132480(f, 0xA1, move) applies the hit as its whip into the ropes does (the shared move 10087 from a
// strong grapple, else 10116, loaded on the thrown one and its damage taken with the stats, FUN_00117868; the received
// ending: lying, state 22; the Blazin' KO check); the other's clip end FUN_00132480(f, 0, 0) (its own ending: standing).
// Neither can be hit or grabbed by anyone else meanwhile. A routine of ours calls at most one game routine, as its last
// step. The cage's own sounds on the clips are the cage's cage.json (DJV Workbench); none are made here.
// VENDETTA_CAGECLIMBLOG=1: a line at each throw's start and at each part (=2: the thrown one's hips each picture).
// Tests: VENDETTA_CAGETEST_REVERSE=1 every throw reversed; VENDETTA_CAGETEST_FACEOFF (throwWatch); VENDETTA_CAGETHROWLOG=1.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "vendetta_cage.h"
#include "vendetta_cage_file.h"
#include "vendetta_ring.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

uint32_t vendetta_animations_cageClip(uint32_t number); // vendetta_animations.cpp: this match's clip id, 0 none

namespace vendetta_cage_throw
{
    constexpr uint32_t kEdgeThrow = 0x122370u;    // FUN_00122370(f): triangle in the lock-up at an edge
    constexpr uint32_t kPair = 0x101728u;         // FUN_00101728(a, s, b, t): a pair, a in s leads b in t
    constexpr uint32_t kPlayClip = 0x132158u;     // FUN_00132158(fighter, clip, flags)
    constexpr uint32_t kClipEnd = 0x132480u;      // FUN_00132480(fighter, flags, move): the clip-end step
    constexpr uint32_t kHookStarted = 0x1DF79Cu;  // the pair's call returns here (no recompiled function covers it)
    constexpr uint32_t kThrowState = 102u;        // the climb's state (its routine asks throwState first)
    constexpr uint32_t kFighters = 0x22FE08u, kFighterBytes = 0xCD8u;

    constexpr uint32_t kOpponent = 0x0u, kState = 0x40u, kX = 0xCCu, kY = 0xD0u, kZ = 0xD4u, kFacing = 0xDCu,
                       kClipFacing = 0x148u, kClip = 0xFCu, kRemaining = 0x114u, kHeld = 0x734u, kGrapple = 0x5A6u,
                       kPlace = 0x5ACu, kTry = 0x5C4u, kEndingNext = 0x38Cu;
    constexpr uint16_t kInRing = 4u, kCantBeGrabbed = 0x1u, kCantBeHit = 0x4000u;
    constexpr uint32_t kUp = 1u, kRight = 2u, kDown = 4u, kLeft = 8u;
    constexpr uint32_t kStrongHit = 10087u, kWeakHit = 10116u; // the game's whip into the ropes' end, strong / weak
    constexpr uint32_t kLyingFaceUp = 51u, kLyingFaceDown = 52u; // the endings V101 / V102
    constexpr uint32_t kOnAllFours = 76u; // the ending K401: held on all fours (half down), the hips over the anchor;
                                          // the get-up from it is the game's K402
    constexpr float kLyingHips = 1.1f; // the lying poses' hips from the anchor: V101 behind it, V102 ahead
    constexpr uint32_t kFrame = 0x110u, kHipsPoint = 0x15Cu;
    constexpr uint32_t kScore = 0x117D48u; // FUN_00117d48(fighter, points, move): the score adder, the move's name shown

    constexpr uint32_t kWhip = 0x3217u, kWhipped = 0x3218u, kWhipReversed = 0x3219u, kWhipReverser = 0x321Au;
    constexpr uint32_t kGrab = 0x321Bu, kGrabbed = 0x321Cu, kRun = 0x321Du, kRunHeld = 0x321Eu, kRam = 0x321Fu,
                       kRammed = 0x3220u, kRamReversed = 0x3221u, kRamReverser = 0x3222u;

    constexpr float kApart = 8.8f;        // No Mercy's 73.5 between the pair, at the conversion's 0.12
    constexpr float kHipsAtCage = 36.5f;  // the thrown one's hips at the hit, from the middle (the ropes, the cage behind)
    constexpr float kRopes = 35.0f;       // the ropes from the middle
    constexpr float kThrowerAside = 12.5f; // the thrower's body at its farthest aside in the clips (9, and its width)
    constexpr float kHeldAside = 6.0f;     // the held one's, to the other side (3, and its width)
    constexpr float kInside = 33.0f;       // the pair's anchors kept this far from the middle while turned into place
    constexpr float kCloseSlack = 6.0f;   // close: the thrown one this near where the reach fits, slid there
    constexpr float kRunSpeed = 1.1f;     // the run, a picture at full speed
    constexpr float kTurnPictures = 12.0f; // the pair turned and slid into place over so many pictures
    constexpr int kRunMostPictures = 240;
    constexpr float kReverseAt = 33.0f;   // the frame of 3217 / 3218 where 3219 / 321A carry on (3219 starts where
                                          // 3217 has the thrower then: its hips 6.3 aside, 1.8 ahead)

    enum Part { kNone, kClose, kGrabPart, kRunPart, kRamPart };

    struct Throw
    {
        Part part = kNone;
        bool thrower = false;   // this entry is the thrower's (the pair's leader); the other's points at it
        uint32_t other = 0;     // the other fighter
        bool reversed = false;
        float nx = 0.0f, nz = 1.0f; // the wall's outward normal
        int pictures = 0;       // in this part
        // the pair's places at this part's start and where they go (thrower 0, thrown-at 1)
        float fromX[2] = {}, fromZ[2] = {}, fromFacing[2] = {}, toX[2] = {}, toZ[2] = {}, toFacing[2] = {};
        float reachX = 0.0f, reachZ = 0.0f; // the held one's farthest hips in this throw's clip (3218 or 3220; its own space)
        float reach() const { return std::fabs(reachZ); }
        // the held one's place no farther out than this, so no part of either body (the climb's room points) goes
        // through the cage's wires in this throw's clips
        float keptOff = 1e9f;
        // where the held one stands for the hit: its hips' farthest place kHipsAtCage from the middle, or nearer the
        // middle when that would put a hand, an elbow or the head through the wires
        float needed() const { return std::min(kHipsAtCage - reach(), keptOff); }
        bool inPlace = false;     // the turn into place is done: the clips carry the pair from here
        bool turnedRound = false; // the reversed whip: gone on from 3217 / 3218 to 3219 / 321A
        uint32_t move = kWeakHit;
        bool endedA = false, endedB = false;
    };
    Throw g_throws[4];
    PS2Runtime *g_runtime = nullptr;
    PS2Runtime::RecompiledFunction g_original = nullptr;
    bool g_installed = false;
    uint32_t g_startReturn = 0; // the wrapper's caller
    PS2Runtime::RecompiledFunction g_originalScore = nullptr;
    uint32_t g_noScoreFor = 0;  // the fighter whose next score (the cage hit's, this picture) is left out with its name
    int g_afterSettle[4] = {};  // VENDETTA_CAGECLIMBLOG=2: pictures still to log after a clip's end
    uint32_t g_noScoreTick = 0; // the game's tick (0x29A538) that picture
    const auto g_programStart = std::chrono::steady_clock::now(); // the watch's times (as a test run's pictures)

    bool logOn()
    {
        const char *v = std::getenv("VENDETTA_CAGECLIMBLOG");
        return v && *v && *v != '0';
    }
    float readFloat(uint8_t *rdram, uint32_t a) { float f; uint32_t v = Ps2FastRead32(rdram, a); std::memcpy(&f, &v, 4); return f; }
    void writeFloat(uint8_t *rdram, uint32_t a, float f) { uint32_t v; std::memcpy(&v, &f, 4); Ps2FastWrite32(rdram, a, v); }
    int indexOf(uint32_t fighter)
    {
        const uint32_t i = (fighter - kFighters) / kFighterBytes;
        return (fighter >= kFighters && i < 4u && (fighter - kFighters) % kFighterBytes == 0u) ? static_cast<int>(i) : -1;
    }
    int player(uint8_t *rdram, uint32_t f) { return static_cast<int>(Ps2FastRead32(rdram, f + 0x30u) & 3u); }
    float rad(float d) { return d * 3.14159265f / 180.0f; }
    float wrap(float a)
    {
        while (a > 180.0f)
            a -= 360.0f;
        while (a <= -180.0f)
            a += 360.0f;
        return a;
    }
    float facingOf(float dx, float dz) { return std::atan2(dx, dz) * 180.0f / 3.14159265f; }

    // the wall a direction points at: the nearest of the ring's sides by its outward normal
    void wallToward(float dx, float dz, float &nx, float &nz)
    {
        const int sides = vendetta_ring::activeSides() == 6 ? 6 : 4;
        float best = -2.0f;
        for (int k = 0; k < sides; ++k)
        {
            const float a = rad(sides == 6 ? 60.0f - 60.0f * k : 90.0f * k);
            const float x = std::sin(a), z = std::cos(a), d = x * dx + z * dz;
            if (d > best)
            {
                best = d;
                nx = x;
                nz = z;
            }
        }
    }
    // a place kept inside the ring's sides (kInside from the middle along each side's normal)
    void keepInside(float &x, float &z)
    {
        const int sides = vendetta_ring::activeSides() == 6 ? 6 : 4;
        for (int k = 0; k < sides; ++k)
        {
            const float a = rad(sides == 6 ? 60.0f - 60.0f * k : 90.0f * k);
            const float nx = std::sin(a), nz = std::cos(a), over = x * nx + z * nz - kInside;
            if (over > 0.0f)
            {
                x -= nx * over;
                z -= nz * over;
            }
        }
    }
    float halfSide() { return vendetta_ring::activeSides() == 6 ? vendetta_cage::cageFile().numbers.walls * 0.57735f
                                                                 : vendetta_cage::cageFile().numbers.walls; }

    // the clip of the fighter's part, playing (flags 0: from its start)
    void play(uint8_t *rdram, R5900Context *ctx, uint32_t fighter, uint32_t number)
    {
        const uint32_t id = vendetta_animations_cageClip(number);
        if (!id)
        {
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
        SET_GPR_U64(ctx, 4, fighter);
        SET_GPR_U64(ctx, 5, id);
        SET_GPR_U64(ctx, 6, 0u);
        g_runtime->lookupFunction(kPlayClip)(rdram, ctx, g_runtime);
    }

    void clipEnd(uint8_t *rdram, R5900Context *ctx, uint32_t fighter, uint32_t flags, uint32_t move)
    {
        SET_GPR_U64(ctx, 4, fighter);
        SET_GPR_U64(ctx, 5, flags);
        SET_GPR_U64(ctx, 6, move);
        g_runtime->lookupFunction(kClipEnd)(rdram, ctx, g_runtime);
    }

    void keepOutOfReach(uint8_t *rdram, uint32_t f)
    {
        Ps2FastWrite16(rdram, f + kPlace, static_cast<uint16_t>(Ps2FastRead16(rdram, f + kPlace) | kCantBeHit | kCantBeGrabbed));
    }

    void place(uint8_t *rdram, uint32_t f, float x, float z, float facing)
    {
        writeFloat(rdram, f + kX, x);
        writeFloat(rdram, f + kZ, z);
        writeFloat(rdram, f + kFacing, wrap(facing));
        writeFloat(rdram, f + kClipFacing, wrap(facing));
    }

    // A clip's end: the anchor and the facing put where the body is, so the pose the game plays next (standing, or
    // lying: V101 face up for 51, V102 face down for 52, both lying along the facing with the hips kLyingHips from the
    // anchor) starts where the clip left the body, with no turn and no slide.
    void settle(uint8_t *rdram, uint32_t f, uint32_t ending)
    {
        const float facing = vendetta_cage::bodyFacingNow(rdram, f);
        const float hips = ending == kLyingFaceUp ? -kLyingHips : ending == kLyingFaceDown ? kLyingHips : 0.0f;
        const float x = readFloat(rdram, f + kHipsPoint), z = readFloat(rdram, f + kHipsPoint + 8u);
        if (logOn())
            std::cerr << "[cage] throw: player " << player(rdram, f) << " settles (ending " << ending << "): the anchor "
                      << readFloat(rdram, f + kX) << " " << readFloat(rdram, f + kZ) << " facing " << readFloat(rdram, f + kFacing)
                      << " to the hips " << x << " " << z << " body " << facing << std::endl;
        place(rdram, f, x - std::sin(rad(facing)) * hips, z - std::cos(rad(facing)) * hips, facing);
        if (indexOf(f) >= 0)
            g_afterSettle[indexOf(f)] = 40;
    }

    // How far out the held one may stand in this throw (t.keptOff) so neither body goes through the wires: the clips
    // the two play at the cage (the whip, or the ram; reversed: the whip to kReverseAt and the reversal, or the
    // reversal at the cage), each posed on its own wrestler's skeleton. The held one faces the wall (its clip's ahead is
    // out), the thrower kApart nearer the wall facing the other way (its clip's behind is out).
    void measureKeptOff(Throw &t, uint8_t *rdram, uint32_t thrower, uint32_t held, bool farThrow)
    {
        const float wires = vendetta_cage::cageFile().numbers.wires;
        t.keptOff = 1e9f;
        auto clip = [&](uint32_t fighter, uint32_t number, float upTo) {
            float ahead = 0.0f, behind = 0.0f;
            if (!vendetta_cage::clipBodyReach(rdram, fighter, number, upTo, ahead, behind))
                return;
            t.keptOff = std::min(t.keptOff, fighter == held ? wires - ahead : wires - kApart - behind);
        };
        if (!farThrow)
        {
            const float whipTo = t.reversed ? kReverseAt : 1e9f;
            clip(thrower, kWhip, whipTo);
            clip(held, kWhipped, whipTo);
            if (t.reversed)
            {
                clip(thrower, kWhipReversed, 1e9f);
                clip(held, kWhipReverser, 1e9f);
            }
        }
        else
        {
            clip(thrower, t.reversed ? kRamReversed : kRam, 1e9f);
            clip(held, t.reversed ? kRamReverser : kRammed, 1e9f);
        }
    }

    // The pair's places for a throw at the wall: the held one facing it, standing so its hips' farthest place in its clip
    // (straight ahead, t.reachZ) is kHipsAtCage from the middle, along the wall where it stood (kept off the corners); the
    // thrower kApart in front of it, between it and the wall, facing it (its back to the wall): the lock-up's arrangement,
    // which every one of the cage's throw clips starts from.
    void placesFor(Throw &t, uint8_t *rdram, uint32_t thrower, uint32_t held)
    {
        const uint32_t pair[2] = {thrower, held};
        for (int i = 0; i < 2; ++i)
        {
            t.fromX[i] = readFloat(rdram, pair[i] + kX);
            t.fromZ[i] = readFloat(rdram, pair[i] + kZ);
            t.fromFacing[i] = readFloat(rdram, pair[i] + kFacing);
        }
        const float facing = facingOf(t.nx, t.nz);
        const float tx = t.nz, tz = -t.nx; // along the wall
        // along the wall: where the held one stood, kept so far from the two corners that the clips stay inside the
        // ropes: the thrower steps aside (the whip 8, the run 5 to 7, thrown down on a reversal 9) to this side of the
        // held one, the held one 3 to the other
        float along = t.fromX[1] * tx + t.fromZ[1] * tz;
        const float ropes = kRopes * (vendetta_ring::activeSides() == 6 ? 0.57735f : 1.0f);
        const float low = std::min(0.0f, -ropes + kHeldAside), high = std::max(0.0f, ropes - kThrowerAside);
        along = std::clamp(along, low, high);
        const float out = t.needed();
        t.toX[1] = t.nx * out + tx * along;
        t.toZ[1] = t.nz * out + tz * along;
        t.toFacing[1] = facing;
        t.toX[0] = t.toX[1] + t.nx * kApart;
        t.toZ[0] = t.toZ[1] + t.nz * kApart;
        t.toFacing[0] = facing + 180.0f;
        t.pictures = 0;
        t.inPlace = false;
    }

    // The pair's places this picture, from where the part started toward its places: the two swing round the point
    // between them (the middle, the way from the held one to the thrower and the gap each eased from the start's to the
    // end's), each still facing the other as in the lock-up, so a throw at the wall behind the held one walks the pair
    // half round; kTurnPictures, and as many again for a half turn.
    void moveTowardPlaces(Throw &t, uint8_t *rdram, uint32_t thrower, uint32_t held)
    {
        const float ax = t.fromX[0] - t.fromX[1], az = t.fromZ[0] - t.fromZ[1];
        const float bx = t.toX[0] - t.toX[1], bz = t.toZ[0] - t.toZ[1];
        const float a0 = facingOf(ax, az), turn = wrap(facingOf(bx, bz) - a0);
        const float pictures = kTurnPictures * (1.0f + std::fabs(turn) / 180.0f);
        if (t.inPlace)
            return; // (placing on would put back one of the pair whose clip has ended and who stands elsewhere by now)
        float k = std::min(1.0f, static_cast<float>(t.pictures + 1) / pictures);
        t.inPlace = k >= 1.0f;
        k = k * k * (3.0f - 2.0f * k); // eased in and out
        const float mx = (t.fromX[0] + t.fromX[1]) * 0.5f * (1.0f - k) + (t.toX[0] + t.toX[1]) * 0.5f * k;
        const float mz = (t.fromZ[0] + t.fromZ[1]) * 0.5f * (1.0f - k) + (t.toZ[0] + t.toZ[1]) * 0.5f * k;
        const float gap = std::sqrt(ax * ax + az * az) * (1.0f - k) + std::sqrt(bx * bx + bz * bz) * k;
        const float a = rad(a0 + turn * k), sx = std::sin(a) * gap * 0.5f, sz = std::cos(a) * gap * 0.5f;
        const uint32_t pair[2] = {thrower, held};
        for (int i = 0; i < 2; ++i)
        {
            float x = mx + (i == 0 ? sx : -sx), z = mz + (i == 0 ? sz : -sz);
            keepInside(x, z);
            place(rdram, pair[i], x, z, t.fromFacing[i] + wrap(t.toFacing[i] - t.fromFacing[i] - turn) * k + turn * k);
        }
    }

    void say(uint8_t *rdram, uint32_t thrower, const Throw &t, const char *what)
    {
        if (!logOn())
            return;
        std::cerr << "[cage] throw: player " << player(rdram, thrower) << " " << what << " (wall " << t.nx << " " << t.nz
                  << (t.reversed ? ", reversed" : "") << ", reach " << t.reach() << ", the thrower at "
                  << readFloat(rdram, thrower + kX) << " " << readFloat(rdram, thrower + kZ) << ", the other at "
                  << readFloat(rdram, t.other + kX) << " " << readFloat(rdram, t.other + kZ) << ")" << std::endl;
    }

    // The cage hit is the game's whip-into-the-ropes hit (the shared move 10087 / 10116, "OVERDOSE"): its damage and its
    // momentum are wanted, its name and points on screen are not (the hit FUN_00117868 gives them to the other through
    // the score adder): that one call is answered without scoring.
    void score(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_noScoreFor && getRegU32(ctx, 4) == g_noScoreFor && Ps2FastRead32(rdram, 0x29A538u) == g_noScoreTick)
        {
            g_noScoreFor = 0;
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
        g_originalScore(rdram, ctx, runtime);
    }

    // ---- the start: FUN_00122370 wrapped

    void started(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        SET_GPR_U64(ctx, 2, 1u); // a state was set
        ctx->pc = g_startReturn;
    }

    void edgeThrow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_runtime = runtime;
        const uint32_t f = getRegU32(ctx, 4);
        const uint32_t o = Ps2FastRead32(rdram, f + kOpponent);
        const int fi = indexOf(f), oi = indexOf(o);
        if (!vendetta_cage::cageMatch(rdram) || fi < 0 || oi < 0 || !vendetta_animations_cageClip(kWhip) ||
            !vendetta_animations_cageClip(kRam) || !(Ps2FastRead16(rdram, f + kPlace) & kInRing) ||
            !(Ps2FastRead16(rdram, o + kPlace) & kInRing))
        {
            if (logOn() && vendetta_cage::cageMatch(rdram))
                std::cerr << "[cage] throw: triangle in a grapple, the game's own (fighters " << fi << " " << oi
                          << ", the clips " << vendetta_animations_cageClip(kWhip) << " " << vendetta_animations_cageClip(kRam)
                          << ", places 0x" << std::hex << Ps2FastRead16(rdram, f + kPlace) << " 0x"
                          << Ps2FastRead16(rdram, o + kPlace) << std::dec << ")" << std::endl;
            g_original(rdram, ctx, runtime);
            return;
        }
        // the wall: the stick's way, else behind the thrower
        const uint32_t held = Ps2FastRead32(rdram, f + kHeld) & 0xFu;
        float dx = ((held & kRight) ? 1.0f : 0.0f) - ((held & kLeft) ? 1.0f : 0.0f);
        float dz = ((held & kDown) ? 1.0f : 0.0f) - ((held & kUp) ? 1.0f : 0.0f);
        if (dx == 0.0f && dz == 0.0f)
        {
            const float a = rad(readFloat(rdram, f + kFacing));
            dx = -std::sin(a);
            dz = -std::cos(a);
        }
        Throw &t = g_throws[fi];
        t = Throw{};
        wallToward(dx, dz, t.nx, t.nz);
        t.thrower = true;
        t.other = o;
        t.reversed = (Ps2FastRead8(rdram, o + kTry) & 4u) != 0u;
        if (const char *force = std::getenv("VENDETTA_CAGETEST_REVERSE"); force && *force && *force != '0')
            t.reversed = true; // (tests: every throw reversed, as if the held one's try had passed)
        Ps2FastWrite8(rdram, o + kTry, static_cast<uint8_t>(Ps2FastRead8(rdram, o + kTry) & ~4u));
        t.move = (Ps2FastRead16(rdram, f + kGrapple) & 2u) ? kStrongHit : kWeakHit;
        if (t.reversed)
            t.move = kStrongHit; // a reversal always the strong hit (No Mercy's reversals hurt more than its throws: 7 to 5)
        // close or far: the whip's reach (the held one's clip) against where the held one stands
        float at = 0.0f, last = 0.0f;
        const bool known = vendetta_cage::clipReach(rdram, o, kWhipped, t.reachX, t.reachZ, at, last);
        measureKeptOff(t, rdram, f, o, false);
        const float reach = t.reach();
        const float stands = readFloat(rdram, o + kX) * t.nx + readFloat(rdram, o + kZ) * t.nz;
        const float needed = t.needed();
        // close: the reach fits within kCloseSlack, or the thrown one is already nearer the cage than it needs (the
        // pair slid back); the run only toward a cage farther than that
        t.part = known && needed - stands <= kCloseSlack ? kClose : kGrabPart;
        Throw &u = g_throws[oi];
        u = Throw{};
        u.other = f;
        u.part = t.part;
        if (logOn())
            std::cerr << "[cage] throw: player " << player(rdram, f) << " throws player " << player(rdram, o)
                      << " toward the wall " << t.nx << " " << t.nz << (t.part == kClose ? ", close" : ", far, the run")
                      << (t.reversed ? ", reversed (the lock-up's try passed)" : "") << ", the whip's reach " << reach
                      << " at frame " << at << ", the held one at " << stands << " of " << needed << " (kept off the wires: "
                      << t.keptOff << ")" << std::endl;
        // both into the throw's state, paired as the game pairs them (the thrower leads); then 1
        g_startReturn = getRegU32(ctx, 31);
        SET_GPR_U64(ctx, 4, f);
        SET_GPR_U64(ctx, 5, kThrowState);
        SET_GPR_U64(ctx, 6, o);
        SET_GPR_U64(ctx, 7, kThrowState);
        SET_GPR_U32(ctx, 31, kHookStarted);
        runtime->lookupFunction(kPair)(rdram, ctx, runtime);
    }

    // ---- each picture, each of the pair (the thrower first)

    bool clipEnded(uint8_t *rdram, uint32_t f) { return readFloat(rdram, f + kRemaining) <= 0.0f; }

    void stateThrower(uint8_t *rdram, R5900Context *ctx, uint32_t f, Throw &t)
    {
        const uint32_t o = t.other;
        keepOutOfReach(rdram, f);
        keepOutOfReach(rdram, o);
        switch (t.part)
        {
        case kClose:
            if (t.pictures == 0)
            {
                float at = 0.0f, last = 0.0f;
                vendetta_cage::clipReach(rdram, o, kWhipped, t.reachX, t.reachZ, at, last);
                measureKeptOff(t, rdram, f, o, false);
                placesFor(t, rdram, f, o);
                say(rdram, f, t, "the whip into the cage");
                moveTowardPlaces(t, rdram, f, o);
                ++t.pictures;
                play(rdram, ctx, f, kWhip);
                return;
            }
            if (t.reversed && !t.turnedRound && readFloat(rdram, f + 0x110u) >= kReverseAt)
            {
                // the held one turns it round: on from here as the reversal
                t.turnedRound = true;
                moveTowardPlaces(t, rdram, f, o);
                ++t.pictures;
                say(rdram, f, t, "reversed: the thrower thrown down");
                play(rdram, ctx, f, kWhipReversed);
                return;
            }
            break;
        case kGrabPart:
            if (t.pictures == 0)
            {
                float at = 0.0f, last = 0.0f;
                vendetta_cage::clipReach(rdram, o, kRammed, t.reachX, t.reachZ, at, last);
                measureKeptOff(t, rdram, f, o, true);
                // turned to the wall now, where the held one stands: the run moves them to it
                placesFor(t, rdram, f, o);
                const float back = (t.toX[1] - t.fromX[1]) * t.nx + (t.toZ[1] - t.fromZ[1]) * t.nz;
                for (int i = 0; i < 2; ++i)
                {
                    t.toX[i] -= t.nx * back;
                    t.toZ[i] -= t.nz * back;
                }
                say(rdram, f, t, "the grab, then the run at the cage");
                moveTowardPlaces(t, rdram, f, o);
                ++t.pictures;
                play(rdram, ctx, f, kGrab);
                return;
            }
            moveTowardPlaces(t, rdram, f, o);
            ++t.pictures;
            if (clipEnded(rdram, f))
            {
                t.part = kRunPart;
                t.pictures = 0;
                play(rdram, ctx, f, kRun);
                return;
            }
            ctx->pc = getRegU32(ctx, 31);
            return;
        case kRunPart:
        {
            // on toward the cage until the held one's reach fits
            const float step = kRunSpeed * std::max(0.1f, readFloat(rdram, 0x22FBD4u));
            const float stands = readFloat(rdram, o + kX) * t.nx + readFloat(rdram, o + kZ) * t.nz;
            const float needed = t.needed();
            // the run goes round: a clip at its end holds its last frame, and starting the clip that is playing does
            // nothing, so each of the pair's is put back to its start (the frame +0x110, the frames left +0x114 from the
            // length +0x118)
            for (uint32_t g : {f, o})
                if (readFloat(rdram, g + kRemaining) <= 0.0f && Ps2FastRead32(rdram, g + kClip) == vendetta_animations_cageClip(g == f ? kRun : kRunHeld))
                {
                    writeFloat(rdram, g + kFrame, 0.0f);
                    writeFloat(rdram, g + kRemaining, readFloat(rdram, g + 0x118u));
                }
            ++t.pictures;
            if (stands + step >= needed || t.pictures > kRunMostPictures)
            {
                const float last = std::max(0.0f, needed - stands);
                for (uint32_t g : {f, o})
                    place(rdram, g, readFloat(rdram, g + kX) + t.nx * last, readFloat(rdram, g + kZ) + t.nz * last,
                          readFloat(rdram, g + kFacing));
                t.part = kRamPart;
                t.pictures = 0;
                say(rdram, f, t, t.reversed ? "at the cage: reversed, the thrower thrown down" : "at the cage: the ram");
                play(rdram, ctx, f, t.reversed ? kRamReversed : kRam);
                return;
            }
            for (uint32_t g : {f, o})
                place(rdram, g, readFloat(rdram, g + kX) + t.nx * step, readFloat(rdram, g + kZ) + t.nz * step,
                      readFloat(rdram, g + kFacing));
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
        default:
            break;
        }
        // the whip or the ram playing: the close whip's slide; then the end
        if (t.part == kClose)
            moveTowardPlaces(t, rdram, f, o);
        ++t.pictures;
        if (clipEnded(rdram, f))
        {
            const bool thrownHere = t.reversed;
            t.part = kNone;
            if (thrownHere)
            {
                // 3219 / 3221 leave the thrower lying face down
                settle(rdram, f, kLyingFaceDown);
                Ps2FastWrite32(rdram, f + kEndingNext, kLyingFaceDown);
                g_noScoreFor = o;
                g_noScoreTick = Ps2FastRead32(rdram, 0x29A538u);
                say(rdram, f, t, "the end: the thrower down");
                clipEnd(rdram, ctx, f, 0xA1u, t.move);
            }
            else
            {
                settle(rdram, f, 0u);
                clipEnd(rdram, ctx, f, 0u, 0u);
            }
            return;
        }
        ctx->pc = getRegU32(ctx, 31);
    }

    void stateHeld(uint8_t *rdram, R5900Context *ctx, uint32_t f, Throw &u)
    {
        const int ti = indexOf(u.other);
        Throw &t = g_throws[ti >= 0 ? ti : 0];
        // the clip that goes with the thrower's part, started when the thrower's changes
        const uint32_t want = t.part == kClose ? (t.turnedRound ? kWhipReverser : kWhipped)
                            : t.part == kGrabPart ? kGrabbed
                            : t.part == kRunPart ? kRunHeld
                            : t.part == kRamPart ? (t.reversed ? kRamReverser : kRammed)
                            : 0u;
        const uint32_t id = want ? vendetta_animations_cageClip(want) : 0u;
        if (logOn() && std::getenv("VENDETTA_CAGECLIMBLOG")[0] == '2' && (t.part == kClose || t.part == kRamPart))
        {
            const float hx = readFloat(rdram, f + 0x15Cu), hz = readFloat(rdram, f + 0x164u);
            const float ax = readFloat(rdram, u.other + 0x15Cu), az = readFloat(rdram, u.other + 0x164u);
            std::cerr << "[cage] throw: frame " << readFloat(rdram, f + 0x110u) << ", the held one's hips out "
                      << hx * t.nx + hz * t.nz << " along " << hx * t.nz - hz * t.nx << ", the thrower's out "
                      << ax * t.nx + az * t.nz << " along " << ax * t.nz - az * t.nx << std::endl;
        }
        if (id && Ps2FastRead32(rdram, f + kClip) != id)
        {
            u.part = t.part;
            play(rdram, ctx, f, want);
            return;
        }
        if ((t.part == kClose || t.part == kRamPart || t.part == kNone) && clipEnded(rdram, f))
        {
            const bool thrownHere = !t.reversed;
            u.part = kNone;
            if (thrownHere)
            {
                // the whip's 3218 leaves the held one on all fours (as the game's K401), the ram's 3220 on its back
                const uint32_t ending = Ps2FastRead32(rdram, f + kClip) == vendetta_animations_cageClip(kWhipped) ? kOnAllFours
                                                                                                                 : kLyingFaceUp;
                settle(rdram, f, ending);
                Ps2FastWrite32(rdram, f + kEndingNext, ending);
                g_noScoreFor = u.other;
                g_noScoreTick = Ps2FastRead32(rdram, 0x29A538u);
                if (logOn())
                    std::cerr << "[cage] throw: player " << player(rdram, f) << " hits the cage (the hit "
                              << t.move << ")" << std::endl;
                clipEnd(rdram, ctx, f, 0xA1u, t.move);
            }
            else
            {
                settle(rdram, f, 0u);
                clipEnd(rdram, ctx, f, 0u, 0u);
            }
            return;
        }
        ctx->pc = getRegU32(ctx, 31);
    }
}

void vendetta_cage::throwMatchStarting(PS2Runtime &runtime)
{
    using namespace vendetta_cage_throw;
    g_runtime = &runtime;
    for (Throw &t : g_throws)
        t = Throw{};
    if (g_installed)
        return;
    g_installed = true;
    g_original = runtime.lookupFunction(kEdgeThrow);
    if (!g_original)
    {
        std::cerr << "[cage] the throw at an edge FUN_00122370 isn't where the original program has it: no throws into the cage"
                  << std::endl;
        return;
    }
    runtime.registerFunction(kHookStarted, started);
    runtime.replaceFunction(kEdgeThrow, edgeThrow);
    g_originalScore = runtime.lookupFunction(kScore);
    if (g_originalScore)
        runtime.replaceFunction(kScore, score);
}

bool vendetta_cage::throwsReady()
{
    return vendetta_animations_cageClip(vendetta_cage_throw::kWhip) && vendetta_animations_cageClip(vendetta_cage_throw::kRam);
}

bool vendetta_cage::throwState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t fighter)
{
    using namespace vendetta_cage_throw;
    g_runtime = runtime;
    const int index = indexOf(fighter);
    if (index < 0)
        return false;
    Throw &t = g_throws[index];
    const bool mine = t.thrower ? true : (t.other != 0u && indexOf(t.other) >= 0 && g_throws[indexOf(t.other)].thrower &&
                                         g_throws[indexOf(t.other)].other == fighter);
    if (!mine)
        return false;
    if ((t.thrower && t.part == kNone && t.pictures > 0) || (!t.thrower && t.part == kNone))
    {
        // this one's end already asked for (the game's clip-end step has set its next state): done with the throw
        t = Throw{};
        return false;
    }
    if (t.thrower)
        stateThrower(rdram, ctx, fighter, t);
    else
        stateHeld(rdram, ctx, fighter, t);
    return true;
}

void vendetta_cage::throwWatch(uint8_t *rdram, uint32_t fighter)
{
    using namespace vendetta_cage_throw;
    static const bool on = [] { const char *v = std::getenv("VENDETTA_CAGETHROWLOG"); return v && *v && *v != '0'; }();
    const int index = indexOf(fighter);
    // VENDETTA_CAGETEST_FACEOFF=<x>,<z>,<gap>[,<momentum>] (tests): once a match, when the fight starts, players 0 and 1 stood face
    // to face that far apart with their middle at (x, z), player 0 on the -x side
    static const char *faceoff = std::getenv("VENDETTA_CAGETEST_FACEOFF");
    static bool placed = false;
    if (faceoff && *faceoff && index == 1)
    {
        const uint32_t p0 = kFighters, p1 = kFighters + kFighterBytes;
        const bool fight = Ps2FastRead32(rdram, 0x22FBE4u) == 3u;
        if (!fight)
            placed = false;
        else if (!placed && Ps2FastRead32(rdram, p0 + kState) == 2u && Ps2FastRead32(rdram, p1 + kState) == 2u)
        {
            float x = 0.0f, z = 0.0f, gap = 9.0f, momentum = -1.0f;
            std::sscanf(faceoff, "%f,%f,%f,%f", &x, &z, &gap, &momentum);
            place(rdram, p0, x - gap / 2.0f, z, 90.0f);
            place(rdram, p1, x + gap / 2.0f, z, -90.0f);
            if (momentum >= 0.0f) // ",<momentum>": both meters set (a reversal's chance in the lock-up grows with it)
                for (uint32_t p : {p0, p1})
                    writeFloat(rdram, p + 0x3B8u, momentum);
            placed = true;
            std::cerr << "[cage] watch: the test's face-off at " << x << " " << z << ", " << gap << " apart" << std::endl;
        }
    }
    if (index >= 0 && g_afterSettle[index] > 0)
    {
        --g_afterSettle[index];
        if (const char *v = std::getenv("VENDETTA_CAGECLIMBLOG"); v && *v == '2')
            std::cerr << "[cage] after: player " << player(rdram, fighter) << " state " << Ps2FastRead32(rdram, fighter + kState)
                      << " clip " << Ps2FastRead32(rdram, fighter + kClip) << " f " << readFloat(rdram, fighter + kFrame)
                      << " anchor " << readFloat(rdram, fighter + kX) << " " << readFloat(rdram, fighter + kZ) << " facing "
                      << readFloat(rdram, fighter + kFacing) << " clip facing " << readFloat(rdram, fighter + kClipFacing)
                      << " body " << vendetta_cage::bodyFacingNow(rdram, fighter) << std::endl;
    }
    if (!on || index < 0)
        return;
    static uint32_t was[4][4] = {};
    const uint32_t now[4] = {Ps2FastRead32(rdram, fighter + kState), Ps2FastRead32(rdram, fighter + 0x44u),
                             Ps2FastRead32(rdram, fighter + kClip), Ps2FastRead32(rdram, fighter + 0x738u) & 0xFFFFu};
    if (std::memcmp(now, was[index], sizeof(now)) == 0)
        return;
    std::memcpy(was[index], now, sizeof(now));
    std::cerr << "[cage] watch: " << std::chrono::duration<double>(std::chrono::steady_clock::now() - g_programStart).count()
              << " s, player " << index << " state " << now[0] << " step " << now[1] << " clip 0x" << std::hex
              << now[2] << " pressed 0x" << now[3] << " grapple 0x" << Ps2FastRead16(rdram, fighter + kGrapple) << std::dec
              << " at " << readFloat(rdram, fighter + kX) << " " << readFloat(rdram, fighter + kZ) << std::endl;
}
