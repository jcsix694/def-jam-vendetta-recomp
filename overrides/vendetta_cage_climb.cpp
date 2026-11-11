// Def Jam Vendetta (USA, SLUS-20639) - the cage match: climbing the cage from inside the ring.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// Triangle with the stick toward a rope, which a cage stops from stepping out (vendetta_cage.cpp), starts the climb
// instead, ESCAPE on or off: state 2's check FUN_0013c1b8 is wrapped. The climber is in state 102 (the state table
// 0x27A400 has no routine there; its entry is pointed at ours for a cage match and put back after) and our code moves the
// wrestler, as the clips (the cage's own, standard\cage\animations\, vendetta_animations.cpp: cageClip) don't:
//   onto the cage (3203), then hanging (3204), the anchor raised and moved out to the cage;
//   up a step (3212-3216 by momentum, Blazin' 320E, each step costing momentum unless in Blazin'), down (3208 / 320F),
//   sideways (3209 / 320A, Blazin' 3210 / 3211) with the stick held (up / down / left / right as on screen);
//   with too little momentum for a step: slips to dangling (323D), dangles (323E), back to the hang (323F);
//   down from the first hold: lets go (3205), lands in the ring standing, state 2.
// The climber can't be hit or grabbed (+0x5AC 0x4000 / 0x1 each picture: the hit test FUN_001290c0 and the grab skip it).
// A game routine called from here can be left half way by the scheduler (a checkpoint), so each of ours calls at most one,
// as its very last step, with its own return address: a pause then resumes it and it returns where it should. A state
// is set by our own writes (FUN_001015d0's, a leaf), never by calling it.
// At the top the climber stays hanging (the top's bar, sitting on top, the way out and the dive come next).
// The jump onto the cage (3203) ends with the hands just under the cage's top (its truss): that is the top hold; the
// anchor is lowered through the jump so the first hold is kSteps steps below it.
// VENDETTA_CAGECLIMBLOG=1: each phase change with the anchor and the hips. VENDETTA_CAGECLIMB_MOMENTUM=1 (tests): the
// climber's momentum kept full.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "vendetta_cage.h"
#include "vendetta_ring.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

uint32_t vendetta_animations_cageClip(uint32_t number); // vendetta_animations.cpp: this match's clip id, 0 none

namespace vendetta_cage_climb
{
    constexpr uint32_t kStateTable = 0x27A400u, kClimbState = 102u, kStanding = 2u;
    constexpr uint32_t kHookClimbState = 0x1DF794u; // the climber's state routine (no recompiled function covers it)
    constexpr uint32_t kRopesCheck = 0x13C1B8u;     // FUN_0013c1b8: state 2's triangle at the ropes (out through them)
    constexpr uint32_t kSetState = 0x101680u;       // FUN_00101680(fighter, state)
    constexpr uint32_t kPlayClip = 0x132158u;       // FUN_00132158(fighter, clip)
    constexpr uint32_t kFighters = 0x22FE08u, kFighterBytes = 0xCD8u;

    // the fighter's fields
    constexpr uint32_t kState = 0x40u, kX = 0xCCu, kY = 0xD0u, kZ = 0xD4u, kFacing = 0xDCu, kClip = 0xFCu,
                       kFrame = 0x110u, kRemaining = 0x114u, kHips = 0x15Cu, kMomentum = 0x3B8u, kPlace = 0x5ACu,
                       kBlazin = 0x5C3u, kHeld = 0x668u, kPressed = 0x66Cu, kTouched = 0x7A0u;
    constexpr uint16_t kInRing = 4u, kCantBeGrabbed = 0x1u, kCantBeHit = 0x4000u;
    constexpr uint32_t kTriangle = 0x1000u;
    constexpr uint32_t kUp = 1u, kRight = 2u, kDown = 4u, kLeft = 8u; // the stick's bits in +0x668 (as the corners' 3 6 12 9)

    // the climb's sizes (game units; the cage's walls at 40, its top 54.5 from the floor, the mat at 10.5)
    constexpr float kCage = 40.0f, kMat = 10.5f;
    constexpr int kSteps = 2;             // steps from the first hold to the top hold (the hands under the truss)
    constexpr float kStepUp = 5.5f;       // a step's rise
    constexpr float kStepSide = 5.0f;     // a sideways step
    constexpr float kCornerRoom = 8.0f;   // kept from each corner
    constexpr float kStepCost = 3.0f;     // momentum a step up costs (none in Blazin')
    constexpr int kDanglePictures = 90;   // dangling before back to the hang

    // the clips (the numbers they had in the game they were converted from)
    constexpr uint32_t kOnto = 0x3203u, kHang = 0x3204u, kLetGo = 0x3205u, kDownStep = 0x3208u, kRightStep = 0x3209u,
                       kLeftStep = 0x320Au, kUpBlazin = 0x320Eu, kDownBlazin = 0x320Fu, kRightBlazin = 0x3210u,
                       kLeftBlazin = 0x3211u, kUpSlowest = 0x3212u, kSlip = 0x323Du, kDangle = 0x323Eu,
                       kRecover = 0x323Fu;

    enum class Phase
    {
        None,
        Onto,
        Hang,
        Step,
        Slip,
        Dangle,
        Recover,
        LetGo
    };
    const char *phaseName(Phase p)
    {
        switch (p)
        {
        case Phase::None: return "none";
        case Phase::Onto: return "onto the cage";
        case Phase::Hang: return "hanging";
        case Phase::Step: return "a step";
        case Phase::Slip: return "slipping";
        case Phase::Dangle: return "dangling";
        case Phase::Recover: return "back to the hang";
        case Phase::LetGo: return "letting go";
        }
        return "?";
    }

    struct Climber
    {
        Phase phase = Phase::None;
        float nx = 0, nz = 1, tx = 1, tz = 0; // the side's outward normal and its direction along (x, z)
        float halfSide = 40.0f;
        float out = 0, along = 0, y = kMat;    // the anchor: along the normal, along the side, height
        float fromOut = 0, toOut = 0, fromAlong = 0, toAlong = 0, fromY = 0, toY = 0; // a step's ends
        int hold = 0;                          // 0 the first hold .. kSteps the top
        int pictures = 0;                      // in this phase
        uint32_t clip = 0;                     // the clip to start at the end of this picture's run (0 none)
        bool glue = false;                     // keep the hips where they were at the last clip change
        float hips[3] = {};
    };
    Climber g_climbers[4];
    bool g_active = false; // this match is a cage match with the cage's clips
    uint32_t g_tableWas = 0;
    bool g_tableSet = false;
    PS2Runtime::RecompiledFunction g_originalRopes = nullptr;
    PS2Runtime *g_runtime = nullptr;

    bool logOn()
    {
        static const bool on = []() { const char *v = std::getenv("VENDETTA_CAGECLIMBLOG"); return v && v[0] == '1'; }();
        return on;
    }
    float readFloat(uint8_t *rdram, uint32_t a) { float f; uint32_t v = Ps2FastRead32(rdram, a); std::memcpy(&f, &v, 4); return f; }
    void writeFloat(uint8_t *rdram, uint32_t a, float f) { uint32_t v; std::memcpy(&v, &f, 4); Ps2FastWrite32(rdram, a, v); }
    int indexOf(uint32_t fighter)
    {
        const uint32_t i = (fighter - kFighters) / kFighterBytes;
        return (fighter >= kFighters && i < 4u) ? static_cast<int>(i) : -1;
    }

    // FUN_001015d0(fighter, state) and FUN_00101680's "run it this picture", as our own writes
    void setState(uint8_t *rdram, uint32_t f, uint32_t state)
    {
        uint16_t flags = static_cast<uint16_t>(Ps2FastRead16(rdram, f + 0x5B4u) & 0xF9F7u);
        Ps2FastWrite16(rdram, f + 0x5B2u, static_cast<uint16_t>(Ps2FastRead16(rdram, f + 0x5B2u) & 0xDE9Fu));
        Ps2FastWrite8(rdram, f + 0x5C3u, static_cast<uint8_t>(Ps2FastRead8(rdram, f + 0x5C3u) & 0xFBu));
        Ps2FastWrite16(rdram, f + 0x5ACu, static_cast<uint16_t>(Ps2FastRead16(rdram, f + 0x5ACu) | 1u));
        Ps2FastWrite8(rdram, f + 0x5BAu, static_cast<uint8_t>(Ps2FastRead8(rdram, f + 0x5BAu) | 0x10u | 0x1u));
        Ps2FastWrite32(rdram, f + 0x4Cu, Ps2FastRead32(rdram, f + 0x40u));
        Ps2FastWrite32(rdram, f + 0x50u, Ps2FastRead32(rdram, f + 0x44u));
        Ps2FastWrite32(rdram, f + 0x40u, state);
        Ps2FastWrite32(rdram, f + 0x44u, 0u);
        Ps2FastWrite32(rdram, f + 0x48u, 0u);
        Ps2FastWrite32(rdram, f + 0x438u, 0u);
        Ps2FastWrite16(rdram, f + 0x5AEu, 0u);
        Ps2FastWrite16(rdram, f + 0x5B0u, 0u);
        Ps2FastWrite8(rdram, f + 0x5C8u, 0u);
        Ps2FastWrite8(rdram, f + 0x5C7u, 0u);
        Ps2FastWrite8(rdram, f + 0x5C9u, 0u);
        Ps2FastWrite32(rdram, f + 0x790u, 0u);
        if (state != 0x16u)
            flags &= 0xF7FFu;
        if (state == kStanding)
        {
            Ps2FastWrite32(rdram, f + 0xCD0u, 0xFFFFFFFFu);
            flags &= 0xFFFBu;
        }
        Ps2FastWrite16(rdram, f + 0x5B4u, flags);
    }

    // the routine's end: back to the caller (the return address $ra), or by way of FUN_00132158(fighter, clip, 0), which
    // returns there itself, a pause or not
    void finish(uint8_t *rdram, R5900Context *ctx, uint32_t fighter, uint32_t clipNumber)
    {
        const uint32_t id = clipNumber ? vendetta_animations_cageClip(clipNumber) : 0u;
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
    bool clipEnded(uint8_t *rdram, uint32_t fighter) { return readFloat(rdram, fighter + kRemaining) <= 0.0f; }
    float progress(uint8_t *rdram, uint32_t fighter)
    {
        const float frame = readFloat(rdram, fighter + kFrame), left = readFloat(rdram, fighter + kRemaining);
        const float whole = frame + left;
        return whole > 0.0f ? std::clamp(frame / whole, 0.0f, 1.0f) : 1.0f;
    }

    // the side the fighter is at: its outward normal (the ring's shape: 4 sides or 6)
    void sideAt(Climber &c, float x, float z)
    {
        const int sides = vendetta_ring::activeSides() == 6 ? 6 : 4;
        float best = -1e9f;
        for (int k = 0; k < sides; ++k)
        {
            // six sides: normals at atan2(x, z) = 60 - 60k degrees (vendetta_ring.cpp's numbering); four: 0, 90, 180, 270
            const float a = (sides == 6 ? 60.0f - 60.0f * k : 90.0f * k) * 3.14159265f / 180.0f;
            const float nx = std::sin(a), nz = std::cos(a);
            const float d = x * nx + z * nz;
            if (d > best)
            {
                best = d;
                c.nx = nx;
                c.nz = nz;
            }
        }
        c.tx = c.nz;
        c.tz = -c.nx;
        c.halfSide = sides == 6 ? kCage * std::tan(3.14159265f / 6.0f) : kCage;
    }

    void place(uint8_t *rdram, uint32_t fighter, const Climber &c)
    {
        writeFloat(rdram, fighter + kX, c.nx * c.out + c.tx * c.along);
        writeFloat(rdram, fighter + kY, c.y);
        writeFloat(rdram, fighter + kZ, c.nz * c.out + c.tz * c.along);
        writeFloat(rdram, fighter + kFacing, std::atan2(c.nx, c.nz) * 180.0f / 3.14159265f);
    }

    void say(uint8_t *rdram, uint32_t fighter, const Climber &c, const char *what)
    {
        if (!logOn())
            return;
        std::cerr << "[cage] climb: player " << (Ps2FastRead32(rdram, fighter + 0x30u) & 3u) << " " << what << " ("
                  << phaseName(c.phase) << ", hold " << c.hold << ", out " << c.out << " along " << c.along << " y " << c.y
                  << ", hips " << readFloat(rdram, fighter + kHips) << " " << readFloat(rdram, fighter + kHips + 4u) << " "
                  << readFloat(rdram, fighter + kHips + 8u) << ", momentum " << readFloat(rdram, fighter + kMomentum) << ")"
                  << std::endl;
    }

    void startPhase(uint8_t *rdram, uint32_t fighter, Climber &c, Phase phase, uint32_t clip, bool glue)
    {
        if (glue)
        {
            c.glue = true;
            for (int i = 0; i < 3; ++i)
                c.hips[i] = readFloat(rdram, fighter + kHips + 4u * i);
        }
        c.phase = phase;
        c.pictures = 0;
        c.clip = clip;
        say(rdram, fighter, c, "starts");
    }

    // the hips back where they were before the clip changed: the anchor moved by the difference (the pose of the new clip
    // is in the hips a picture after it starts)
    void keepHips(uint8_t *rdram, uint32_t fighter, Climber &c)
    {
        if (!c.glue || c.pictures < 1)
            return;
        c.glue = false;
        const float dx = c.hips[0] - readFloat(rdram, fighter + kHips);
        const float dy = c.hips[1] - readFloat(rdram, fighter + kHips + 4u);
        const float dz = c.hips[2] - readFloat(rdram, fighter + kHips + 8u);
        c.out += dx * c.nx + dz * c.nz;
        c.along += dx * c.tx + dz * c.tz;
        c.y += dy;
        if (logOn())
            std::cerr << "[cage] climb: the hips kept: moved out " << dx * c.nx + dz * c.nz << ", along "
                      << dx * c.tx + dz * c.tz << ", up " << dy << std::endl;
    }

    uint32_t upClip(uint8_t *rdram, uint32_t fighter)
    {
        const float momentum = readFloat(rdram, fighter + kMomentum);
        return kUpSlowest + static_cast<uint32_t>(std::clamp(static_cast<int>(momentum / 20.0f), 0, 4));
    }
    bool blazin(uint8_t *rdram, uint32_t fighter) { return (Ps2FastRead8(rdram, fighter + kBlazin) & 2u) != 0u; }

    // which way along the side the stick's left / right go: the side's direction as it runs across the screen
    float screenSign(const Climber &c)
    {
        float rx = 0, rz = 0;
        if (!vendetta_cage::screenRight(rx, rz))
            return 1.0f;
        return (c.tx * rx + c.tz * rz) >= 0.0f ? 1.0f : -1.0f;
    }

    void step(uint8_t *rdram, uint32_t fighter, Climber &c, int up, int side)
    {
        const bool fast = blazin(rdram, fighter);
        c.fromOut = c.toOut = c.out;
        c.fromAlong = c.toAlong = c.along;
        c.fromY = c.toY = c.y;
        uint32_t clip = kHang;
        if (up > 0)
        {
            c.toY = c.y + kStepUp;
            ++c.hold;
            clip = fast ? kUpBlazin : upClip(rdram, fighter);
            if (!fast)
                writeFloat(rdram, fighter + kMomentum, std::max(0.0f, readFloat(rdram, fighter + kMomentum) - kStepCost));
        }
        else if (up < 0)
        {
            c.toY = c.y - kStepUp;
            --c.hold;
            clip = fast ? kDownBlazin : kDownStep;
        }
        else
        {
            const float along = std::clamp(c.along + kStepSide * static_cast<float>(side), -(c.halfSide - kCornerRoom),
                                           c.halfSide - kCornerRoom);
            c.toAlong = along;
            // the clip's "right" is the climber's right, facing the cage (outward): the side's direction turned the other way
            const bool climbersRight = side * (c.tx * -c.nz + c.tz * c.nx) > 0.0f; // guess: checked live
            clip = climbersRight ? (fast ? kRightBlazin : kRightStep) : (fast ? kLeftBlazin : kLeftStep);
        }
        startPhase(rdram, fighter, c, Phase::Step, clip, false);
    }

    // state 102: the climber, each picture
    void climbState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_runtime = runtime;
        const uint32_t fighter = getRegU32(ctx, 4);
        const int index = indexOf(fighter);
        if (index < 0 || !g_active || g_climbers[index].phase == Phase::None)
        {
            if (index >= 0)
                setState(rdram, fighter, kStanding);
            finish(rdram, ctx, fighter, 0u);
            return;
        }
        Climber &c = g_climbers[index];
        keepHips(rdram, fighter, c);
        ++c.pictures;
        if (logOn() && c.pictures % 30 == 0)
            std::cerr << "[cage] climb: " << phaseName(c.phase) << " picture " << c.pictures << ", clip "
                      << Ps2FastRead32(rdram, fighter + kClip) << " frame " << readFloat(rdram, fighter + kFrame) << " left "
                      << readFloat(rdram, fighter + kRemaining) << ", held " << (Ps2FastRead32(rdram, fighter + kHeld) & 0xFu)
                      << ", hips y " << readFloat(rdram, fighter + kHips + 4u) << ", anchor y " << c.y << std::endl;
        const uint32_t held = Ps2FastRead32(rdram, fighter + kHeld) & 0xFu;
        static const bool fullMomentum = []() { const char *v = std::getenv("VENDETTA_CAGECLIMB_MOMENTUM"); return v && v[0] == '1'; }();
        if (fullMomentum)
            writeFloat(rdram, fighter + kMomentum, 100.0f);
        const bool started = c.pictures > 1; // the clip asked for last picture is playing
        switch (c.phase)
        {
        case Phase::Onto:
            c.y = c.fromY - kStepUp * static_cast<float>(kSteps) * progress(rdram, fighter);
            if (started && clipEnded(rdram, fighter))
                startPhase(rdram, fighter, c, Phase::Hang, kHang, true);
            break;
        case Phase::Hang:
            if (!started)
                break;
            if (held & kUp)
            {
                if (c.hold >= kSteps)
                    break; // the top (what comes next: the top's own step)
                if (!blazin(rdram, fighter) && readFloat(rdram, fighter + kMomentum) < kStepCost)
                    startPhase(rdram, fighter, c, Phase::Slip, kSlip, false);
                else
                    step(rdram, fighter, c, 1, 0);
            }
            else if (held & kDown)
            {
                if (c.hold > 0)
                    step(rdram, fighter, c, -1, 0);
                else
                    startPhase(rdram, fighter, c, Phase::LetGo, kLetGo, false);
            }
            else if (held & (kLeft | kRight))
            {
                const int side = ((held & kRight) ? 1 : -1) * static_cast<int>(screenSign(c));
                const float to = c.along + kStepSide * side;
                if (std::fabs(to) <= c.halfSide - kCornerRoom + 0.01f)
                    step(rdram, fighter, c, 0, side);
            }
            break;
        case Phase::Step:
        {
            const float t = progress(rdram, fighter);
            c.out = c.fromOut + (c.toOut - c.fromOut) * t;
            c.along = c.fromAlong + (c.toAlong - c.fromAlong) * t;
            c.y = c.fromY + (c.toY - c.fromY) * t;
            if (started && clipEnded(rdram, fighter))
            {
                c.out = c.toOut;
                c.along = c.toAlong;
                c.y = c.toY;
                startPhase(rdram, fighter, c, Phase::Hang, kHang, false);
            }
            break;
        }
        case Phase::Slip:
            if (started && clipEnded(rdram, fighter))
                startPhase(rdram, fighter, c, Phase::Dangle, kDangle, false);
            break;
        case Phase::Dangle:
            if (c.pictures >= kDanglePictures)
                startPhase(rdram, fighter, c, Phase::Recover, kRecover, false);
            break;
        case Phase::Recover:
            if (started && clipEnded(rdram, fighter))
                startPhase(rdram, fighter, c, Phase::Hang, kHang, false);
            break;
        case Phase::LetGo:
            if (started && clipEnded(rdram, fighter))
            {
                say(rdram, fighter, c, "lands in the ring");
                c.phase = Phase::None;
                // standing where the landing left the body (its hips over the anchor, as the game's flag 4 does)
                const float hx = readFloat(rdram, fighter + kHips), hz = readFloat(rdram, fighter + kHips + 8u);
                c.out = hx * c.nx + hz * c.nz;
                c.along = hx * c.tx + hz * c.tz;
                c.y = kMat;
                place(rdram, fighter, c);
                setState(rdram, fighter, kStanding); // state 2 runs this picture and starts its own clip
                finish(rdram, ctx, fighter, 0u);
                return;
            }
            break;
        case Phase::None:
            break;
        }
        place(rdram, fighter, c);
        Ps2FastWrite16(rdram, fighter + kPlace,
                       static_cast<uint16_t>(Ps2FastRead16(rdram, fighter + kPlace) | kCantBeHit | kCantBeGrabbed));
        const uint32_t clip = c.clip;
        c.clip = 0;
        finish(rdram, ctx, fighter, clip);
    }

    // FUN_0013c1b8 (state 2's triangle at the ropes): in a cage match, triangle with the stick toward a rope climbs the cage
    void ropesCheck(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_runtime = runtime;
        const uint32_t fighter = getRegU32(ctx, 4);
        const int index = indexOf(fighter);
        const uint32_t wall = Ps2FastRead32(rdram, fighter + kTouched);
        if (g_active && index >= 0 && wall && vendetta_animations_cageClip(kOnto) != 0u &&
            (Ps2FastRead32(rdram, fighter + kPressed) & kTriangle) &&
            (Ps2FastRead16(rdram, fighter + kPlace) & kInRing))
        {
            const uint16_t kind = Ps2FastRead16(rdram, wall + 0x42u);
            if ((kind & 0x20u) && (Ps2FastRead32(rdram, fighter + kHeld) & kind & 0xFu))
            {
                Climber &c = g_climbers[index];
                c = Climber();
                const float x = readFloat(rdram, fighter + kX), z = readFloat(rdram, fighter + kZ);
                sideAt(c, x, z);
                c.out = x * c.nx + z * c.nz;
                c.along = std::clamp(x * c.tx + z * c.tz, -(c.halfSide - kCornerRoom), c.halfSide - kCornerRoom);
                c.y = c.fromY = readFloat(rdram, fighter + kY);
                setState(rdram, fighter, kClimbState); // state 102 runs this picture: it starts the clip
                startPhase(rdram, fighter, c, Phase::Onto, kOnto, false);
                place(rdram, fighter, c);
                SET_GPR_U64(ctx, 2, 1u);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
        }
        g_originalRopes(rdram, ctx, runtime);
    }
}

bool vendetta_cage::cageMatch(uint8_t *rdram) { return (Ps2FastRead16(rdram, kRuleWord) & kRuleCage) != 0u; }

bool vendetta_cage::climbing(uint8_t *rdram, uint32_t fighter)
{
    using namespace vendetta_cage_climb;
    const int index = indexOf(fighter);
    return g_active && index >= 0 && g_climbers[index].phase != Phase::None &&
           Ps2FastRead32(rdram, fighter + kState) == kClimbState;
}

// each match's start (before the game's init): the climb's state routine in the table for a cage match, else the
// table's own entry back
void vendetta_cage::climbMatchStarting(uint8_t *rdram, PS2Runtime &runtime)
{
    using namespace vendetta_cage_climb;
    g_runtime = &runtime;
    for (Climber &c : g_climbers)
        c = Climber();
    const uint32_t entry = kStateTable + 4u * kClimbState;
    if (!g_tableSet)
    {
        g_tableWas = Ps2FastRead32(rdram, entry);
        g_tableSet = true;
    }
    g_active = cageMatch(rdram);
    Ps2FastWrite32(rdram, entry, g_active ? kHookClimbState : g_tableWas);
}

void installVendettaCageClimb(PS2Runtime &runtime)
{
    using namespace vendetta_cage_climb;
    g_runtime = &runtime;
    g_originalRopes = runtime.lookupFunction(kRopesCheck);
    if (!g_originalRopes)
    {
        std::cerr << "[cage] state 2's ropes check isn't where the original program has it: no climbing" << std::endl;
        return;
    }
    runtime.registerFunction(kHookClimbState, climbState);
    runtime.replaceFunction(kRopesCheck, ropesCheck);
}
