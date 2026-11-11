// Def Jam Vendetta (USA, SLUS-20639) - the cage match: climbing the cage from inside the ring.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// Triangle with the stick toward a rope, which a cage stops from stepping out (vendetta_cage.cpp), starts the climb
// instead, ESCAPE on or off: state 2's check FUN_0013c1b8 is wrapped. The climber is in state 102 (the state table
// 0x27A400 has no routine there; its entry is pointed at ours for a cage match and put back after) and our code moves the
// wrestler, as the clips (the cage's own, standard\cage\animations\, vendetta_animations.cpp: cageClip) don't:
//   onto the cage (3203), then hanging (3204), the anchor raised and moved out to the cage;
//   up a step (3212-3216 by momentum, Blazin' 320E, each step costing momentum unless in Blazin'), down (3208 / 320F),
//   sideways (3209 / 320A, Blazin' 3210 / 3211) with the stick held (up / down / left / right as on screen: the step's
//   way along the side projected by the last picture's camera); the stick still held toward the rope from getting on
//   counts for nothing until it is let go;
//   with too little momentum for a step: slips to dangling (323D), dangles (323E), back to the hang (323F);
//   down from the first hold: lets go (3205), lands in the ring standing, state 2.
// How far out from the cage: each picture the body points (FUN_00131438's 20, the last pose at this anchor) are measured
// along the side's normal, each with the room its flesh takes, and the anchor moved so the outermost sits on the cage's
// wires, whatever the wrestler's size: on the cage the body is kept touching it; getting on and letting go only keep it
// from going through.
// The climber can't be hit or grabbed (+0x5AC 0x4000 / 0x1 each picture: the hit test FUN_001290c0 and the grab skip it).
// A game routine called from here can be left half way by the scheduler (a checkpoint), so each of ours calls at most one,
// as its very last step, with its own return address: a pause then resumes it and it returns where it should. A state
// is set by our own writes (FUN_001015d0's, a leaf), never by calling it.
// At the top the climber stays hanging (the top's bar, sitting on top, the way out and the dive come next).
// The jump onto the cage (3203) ends with the hands just under the cage's top (its truss): that is the top hold; the
// anchor is lowered as the jump's body rises (the clip's own root, kOntoRoot) so the first hold is kSteps steps below it,
// and moved to the hang's place in the same picture the hang starts (kOntoToHang). Letting go (3205) raises it as the body
// falls (kLetGoRoot), so the feet land on the mat. The ankles are kept above the mat throughout (aboveMat).
// VENDETTA_CAGECLIMBLOG=1: each phase change with the anchor and the hips (=2: also a line a picture, detail()).
// VENDETTA_CAGECLIMB_MOMENTUM=1 (tests): the climber's momentum kept full.
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
                       kFrame = 0x110u, kRemaining = 0x114u, kPoints = 0x150u, kHips = 0x15Cu, kMomentum = 0x3B8u, kPlace = 0x5ACu,
                       kBlazin = 0x5C3u, kHeld = 0x668u, kPressed = 0x66Cu, kTouched = 0x7A0u;
    constexpr uint16_t kInRing = 4u, kCantBeGrabbed = 0x1u, kCantBeHit = 0x4000u;
    constexpr uint32_t kTriangle = 0x1000u;
    constexpr uint32_t kUp = 1u, kRight = 2u, kDown = 4u, kLeft = 8u; // the stick's bits in +0x668 (as the corners' 3 6 12 9)

    // the climb's sizes (game units; the cage's walls at 40, its top 54.5 from the floor, the mat at 10.5)
    constexpr float kCage = 40.0f, kMat = 10.5f;
    // two holds: the rope hold (hold 0: the lowest foot, the right, on the top rope, which bends to it) and the top hold
    // (the hands under the truss), a step apart (the user's choice: the lowest foot on the top rope; the cage being 2
    // wrestlers tall leaves room for one short step above it)
    constexpr int kSteps = 1;             // steps from the rope hold to the top hold
    constexpr float kStepUp = 3.9f;       // a step's rise (the hang's right ankle, 9.3 under its anchor, pressing the top
                                          // rope at 24.1 down by 1: the rope hold's anchor 32.4, the top's 36.3)
    constexpr float kStepSide = 5.0f;     // a sideways step
    constexpr float kCornerRoom = 8.0f;   // kept from each corner
    constexpr float kStepCost = 3.0f;     // momentum a step up costs (none in Blazin')
    constexpr int kDanglePictures = 90;   // dangling before back to the hang
    constexpr float kWires = 39.8f;       // the cage's wires, from the middle (the user's model: its walls' plane at 40)
    constexpr float kToCage = 0.5f;       // the most the body moves out to the cage in a picture
    constexpr float kOffCage = 2.0f;      // the most it moves back in a picture (a part coming through)

    // the body points that can meet the cage (point, the room its flesh takes round it: a bit under the hit test's radii)
    struct BodyPoint
    {
        uint32_t point;
        float room;
    };
    constexpr BodyPoint kBody[] = {{3, 2.2f},  {5, 1.8f},  {8, 1.0f},  {9, 0.6f},  {12, 1.0f},
                                   {13, 0.6f}, {15, 1.8f}, {16, 1.2f}, {18, 1.8f}, {19, 1.2f}};
    // chest, head, left elbow, left fist, right elbow, right fist, left knee, left ankle, right knee, right ankle

    // the clips (the numbers they had in the game they were converted from)
    constexpr uint32_t kOnto = 0x3203u, kHang = 0x3204u, kLetGo = 0x3205u, kDownStep = 0x3208u, kRightStep = 0x3209u,
                       kLeftStep = 0x320Au, kUpBlazin = 0x320Eu, kDownBlazin = 0x320Fu, kRightBlazin = 0x3210u,
                       kLeftBlazin = 0x3211u, kUpSlowest = 0x3212u, kSlip = 0x323Du, kDangle = 0x323Eu,
                       kRecover = 0x323Fu;

    // The clips' own poses (the wrestler's place the origin, y up, z forward toward the cage, x the wrestler's left), posed
    // as the game does from the standard cage's clips (the same for every wrestler: the root's keys are the clip's).
    // Getting on (3203) goes up the ropes as a ladder, all three touched, three steps up (the user's choice): how far the
    // anchor is under the mat, every 4th frame, set so each foot
    // presses its rope down by about 1 (the rope gives under it): the right foot on the bottom rope (frames 32-56), the
    // left on the middle one (48-90, pushing up off the bottom: the drop eases from 4.3 to 2.5), the right on the top one
    // (72-132), ending kSteps x kStepUp down for the rope hold. Nothing until the left foot leaves the mat (frame 24).
    // Letting go (3205) falls to the floor 24.3 under the hold (the ankles), further than ours: the anchor is raised in
    // step with its root's fall.
    constexpr float kOntoDrop[] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.8f, 3.9f, 4.8f, 5.7f, 5.6f,
                                   4.8f, 4.5f, 4.3f, 3.2f, 2.7f, 2.5f, 2.7f, 3.1f, 3.6f, 4.2f, 4.7f, 4.4f,
                                   4.0f, 3.9f, 3.9f, 3.9f, 3.9f, 3.9f, 3.9f, 3.9f, 3.9f, 3.9f};
    constexpr float kLetGoRoot[] = {-0.80f,  -1.30f,  -1.70f,  -2.40f,  -3.50f,  -4.70f,  -5.70f,  -5.90f,  -5.20f,
                                    -5.10f,  -5.40f,  -5.80f,  -6.10f,  -7.00f,  -8.30f,  -9.90f,  -12.10f, -14.40f,
                                    -17.00f, -18.80f, -19.00f, -18.80f, -18.10f, -17.50f, -16.80f, -16.40f};
    constexpr float kLetGoLanded = -18.8f;   // its root as the feet reach the floor (frame 76)
    constexpr float kLetGoAnkles = -24.3f;   // its ankles on the floor
    constexpr float kAnkleUp = 1.7f;         // a standing wrestler's ankles above the floor
    constexpr float kAnkleLeast = 1.3f;      // the lowest the ankles may come to the mat (the clips roll onto the toes)
    // the jump's end to the hang's start: the root moves (x, y, z) = (-0.6, 25.1, 6.2) - (0.0, -0.7, -3.7)
    constexpr float kOntoToHang[3] = {-0.6f, 25.8f, 9.9f};

    // the hang at a hold: at the rope hold its copy that keeps the top rope on the right foot
    uint32_t hangClip(int hold) { return hold == 0 ? vendetta_cage::kHangOnRopeClip : kHang; }

    template <size_t N>
    float tableAt(const float (&table)[N], float frame)
    {
        const float i = std::clamp(frame / 4.0f, 0.0f, static_cast<float>(N - 1));
        const size_t a = static_cast<size_t>(i);
        const size_t b = std::min(a + 1, N - 1);
        return table[a] + (table[b] - table[a]) * (i - static_cast<float>(a));
    }

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
        float fromAlong = 0, toAlong = 0, fromY = 0, toY = 0; // a step's ends (how far out: againstCage)
        float screen = 0;                      // +1 / -1: the side's direction across the screen, as last seen
        uint32_t toRope = 0;                   // the stick held toward the rope to get on: no step until let go
        int hold = 0;                          // 0 the first hold .. kSteps the top
        int pictures = 0;                      // in this phase
        uint32_t clip = 0;                     // the clip to start at the end of this picture's run (0 none)
        uint32_t clipFlags = 0;                // FUN_00132158's flags for it (2: blended in from the pose shown)
        float raised = 0;                      // letting go: the anchor's rise so far (it never comes back down)
        int settle = 0;                        // pictures the body points still show the clip before a jump of the anchor
    };
    Climber g_climbers[4];
    bool g_active = false; // this match is a cage match with the cage's clips
    uint32_t g_tableWas = 0;
    bool g_tableSet = false;
    PS2Runtime::RecompiledFunction g_originalRopes = nullptr;
    PS2Runtime *g_runtime = nullptr;

    int logLevel()
    {
        static const int level = []() { const char *v = std::getenv("VENDETTA_CAGECLIMBLOG"); return v ? std::atoi(v) : 0; }();
        return level;
    }
    bool logOn() { return logLevel() >= 1; }
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

    // the routine's end: back to the caller (the return address $ra), or by way of FUN_00132158(fighter, clip, flags), which
    // returns there itself, a pause or not (flags 0: a cut, the climb's clips follow on from each other; 2: blended in)
    void finish(uint8_t *rdram, R5900Context *ctx, uint32_t fighter, uint32_t clipNumber, uint32_t flags = 0u)
    {
        const uint32_t id = clipNumber ? vendetta_animations_cageClip(clipNumber) : 0u;
        if (!id)
        {
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
        SET_GPR_U64(ctx, 4, fighter);
        SET_GPR_U64(ctx, 5, id);
        SET_GPR_U64(ctx, 6, flags);
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
                  << readFloat(rdram, fighter + kHips + 8u) << ", momentum " << readFloat(rdram, fighter + kMomentum)
                  << ", screen " << c.screen << ")" << std::endl;
    }

    // the way the body faces in the world, in the facing's degrees: from the hips (body points 14 left, 17 right: the
    // wrestler's left is the model's +X, its front +Z)
    float bodyFacing(uint8_t *rdram, uint32_t fighter)
    {
        const uint32_t l = fighter + kPoints + 12u * 14u, r = fighter + kPoints + 12u * 17u;
        const float ax = readFloat(rdram, l) - readFloat(rdram, r), az = readFloat(rdram, l + 8u) - readFloat(rdram, r + 8u);
        float f = std::atan2(ax, az) * 180.0f / 3.14159265f - 90.0f;
        if (f <= -180.0f)
            f += 360.0f;
        return f;
    }
    int g_afterLanding[4] = {}; // VENDETTA_CAGECLIMBLOG=2: pictures still to log after a landing (state 2's ropes check)

    // VENDETTA_CAGECLIMBLOG=2: a line a picture while getting on or letting go, and for the first pictures of every
    // phase: the clip's frame, the blend's count (+0x10C), the anchor, and body points as (out, along, up) from the side:
    // 0 the root, 1 the hips, 9 / 13 the fists, 16 / 19 the ankles (the ropes at out 35, up 24.1 / 19.3 / 14.5; the mat 10.5)
    void detail(uint8_t *rdram, uint32_t fighter, const Climber &c)
    {
        if (logLevel() < 2 || !(c.phase == Phase::Onto || c.phase == Phase::LetGo || c.pictures <= 3))
            return;
        std::cerr << "[cage] frame: " << phaseName(c.phase) << " " << c.pictures << " clip " << Ps2FastRead32(rdram, fighter + kClip)
                  << " f " << readFloat(rdram, fighter + kFrame) << " blend " << Ps2FastRead32(rdram, fighter + 0x10Cu)
                  << " | anchor " << c.out << " " << c.along << " " << c.y;
        for (const uint32_t k : {0u, 1u, 9u, 13u, 16u, 19u})
        {
            const uint32_t a = fighter + kPoints + 12u * k;
            const float x = readFloat(rdram, a), y = readFloat(rdram, a + 4u), z = readFloat(rdram, a + 8u);
            std::cerr << " | p" << k << " " << x * c.nx + z * c.nz << " " << x * c.tx + z * c.tz << " " << y;
        }
        std::cerr << " | facing " << readFloat(rdram, fighter + kFacing) << " turn " << readFloat(rdram, fighter + 0x130u)
                  << " after " << readFloat(rdram, fighter + 0x148u) << " body " << bodyFacing(rdram, fighter);
        // the square's ropes of this side (0x1430E70: rope = side x 3 + level 0 top .. 2 bottom; side 0 the -Z side, 1 +X,
        // 2 +Z, 3 -X; each {along, up, out, half width}): how far each is pulled up and out
        if (vendetta_ring::activeSides() != 6)
        {
            const uint32_t side = c.nz < -0.5f ? 0u : c.nx > 0.5f ? 1u : c.nz > 0.5f ? 2u : 3u;
            std::cerr << " | ropes (up, out)";
            for (uint32_t level = 0; level < 3; ++level)
            {
                const uint32_t r = 0x1430E70u + 16u * (side * 3u + level);
                std::cerr << " " << readFloat(rdram, r + 4u) << " " << readFloat(rdram, r + 8u);
            }
        }
        std::cerr << std::endl;
    }

    void startPhase(uint8_t *rdram, uint32_t fighter, Climber &c, Phase phase, uint32_t clip, uint32_t flags = 0u)
    {
        c.phase = phase;
        c.pictures = 0;
        c.clip = clip;
        c.clipFlags = flags;
        c.fromY = c.y;
        c.raised = 0.0f;
        say(rdram, fighter, c, "starts");
    }

    // the feet kept out of the mat: the ankles (the last pose, posed at the anchor last written) no lower than kAnkleLeast
    // above it; a safety under the clips' own curves (another wrestler's legs, a clip of one's own)
    void aboveMat(uint8_t *rdram, uint32_t fighter, Climber &c)
    {
        if (c.settle > 0)
            return;
        const float placed = readFloat(rdram, fighter + kY);
        const float lowest = std::min(readFloat(rdram, fighter + kPoints + 12u * 16u + 4u),
                                      readFloat(rdram, fighter + kPoints + 12u * 19u + 4u)) - placed;
        c.y = std::max(c.y, kMat + kAnkleLeast - lowest);
    }

    uint32_t upClip(uint8_t *rdram, uint32_t fighter)
    {
        const float momentum = readFloat(rdram, fighter + kMomentum);
        return kUpSlowest + static_cast<uint32_t>(std::clamp(static_cast<int>(momentum / 20.0f), 0, 4));
    }
    bool blazin(uint8_t *rdram, uint32_t fighter) { return (Ps2FastRead8(rdram, fighter + kBlazin) & 2u) != 0u; }

    // which way along the side the stick's left / right go: the side's direction as it runs across the screen, from the
    // climber's chest (the last answer kept for a picture with no camera)
    float screenSign(Climber &c)
    {
        const float x = c.nx * c.out + c.tx * c.along, z = c.nz * c.out + c.tz * c.along;
        float sign = 0.0f;
        if (vendetta_cage::screenSide(x, c.y + 8.0f, z, c.tx, c.tz, sign))
            c.screen = sign;
        return c.screen != 0.0f ? c.screen : 1.0f;
    }

    // the body on the cage's wires (the top comment): touch = moved out to them as well as kept from going through
    void againstCage(uint8_t *rdram, uint32_t fighter, Climber &c, bool touch)
    {
        if (c.settle > 0)
            return;
        // the points stand at the anchor last written; c.out may have moved since (keepHips)
        const float placed = readFloat(rdram, fighter + kX) * c.nx + readFloat(rdram, fighter + kZ) * c.nz;
        float most = -1e9f;
        for (const BodyPoint &b : kBody)
        {
            const uint32_t a = fighter + kPoints + 12u * b.point;
            most = std::max(most, readFloat(rdram, a) * c.nx + readFloat(rdram, a + 8u) * c.nz + b.room);
        }
        const float move = kWires - (most + c.out - placed);
        if (move > 0.0f && !touch)
            return;
        c.out += std::clamp(move, -kOffCage, kToCage);
    }

    void step(uint8_t *rdram, uint32_t fighter, Climber &c, int up, int side)
    {
        const bool fast = blazin(rdram, fighter);
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
        startPhase(rdram, fighter, c, Phase::Step, clip);
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
        detail(rdram, fighter, c);
        if (c.settle > 0)
            --c.settle;
        ++c.pictures;
        if (logOn() && c.pictures % 30 == 0)
            std::cerr << "[cage] climb: " << phaseName(c.phase) << " picture " << c.pictures << ", clip "
                      << Ps2FastRead32(rdram, fighter + kClip) << " frame " << readFloat(rdram, fighter + kFrame) << " left "
                      << readFloat(rdram, fighter + kRemaining) << ", held " << (Ps2FastRead32(rdram, fighter + kHeld) & 0xFu)
                      << ", hips y " << readFloat(rdram, fighter + kHips + 4u) << ", anchor y " << c.y << " out " << c.out
                      << std::endl;
        uint32_t held = Ps2FastRead32(rdram, fighter + kHeld) & 0xFu;
        if (!(held & c.toRope))
            c.toRope = 0;
        held &= ~c.toRope;
        static const bool fullMomentum = []() { const char *v = std::getenv("VENDETTA_CAGECLIMB_MOMENTUM"); return v && v[0] == '1'; }();
        if (fullMomentum)
            writeFloat(rdram, fighter + kMomentum, 100.0f);
        const bool started = c.pictures > 1; // the clip asked for last picture is playing
        switch (c.phase)
        {
        case Phase::Onto:
            // up the ropes (kOntoDrop; nothing before the jump's clip plays: the stance's frame is not its own)
            if (started)
            {
                c.y = c.fromY - tableAt(kOntoDrop, readFloat(rdram, fighter + kFrame));
                aboveMat(rdram, fighter, c);
            }
            if (started && clipEnded(rdram, fighter))
            {
                // the hang starts where the jump ended, this picture: the anchor moved as the root moves between the two
                // clips (the body points show the jump's pose for one more picture: settle)
                c.along += kOntoToHang[0];
                c.y += kOntoToHang[1];
                c.out += kOntoToHang[2];
                c.settle = 1;
                startPhase(rdram, fighter, c, Phase::Hang, hangClip(c.hold));
            }
            break;
        case Phase::Hang:
            if (!started)
                break;
            if (held & kUp)
            {
                if (c.hold >= kSteps)
                    break; // the top (what comes next: the top's own step)
                if (!blazin(rdram, fighter) && readFloat(rdram, fighter + kMomentum) < kStepCost)
                    startPhase(rdram, fighter, c, Phase::Slip, kSlip);
                else
                    step(rdram, fighter, c, 1, 0);
            }
            else if (held & kDown)
            {
                if (c.hold > 0)
                    step(rdram, fighter, c, -1, 0);
                else
                    startPhase(rdram, fighter, c, Phase::LetGo, kLetGo);
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
            c.along = c.fromAlong + (c.toAlong - c.fromAlong) * t;
            c.y = c.fromY + (c.toY - c.fromY) * t;
            if (started && clipEnded(rdram, fighter))
            {
                c.along = c.toAlong;
                c.y = c.toY;
                startPhase(rdram, fighter, c, Phase::Hang, hangClip(c.hold));
            }
            break;
        }
        case Phase::Slip:
            if (started && clipEnded(rdram, fighter))
                startPhase(rdram, fighter, c, Phase::Dangle, kDangle);
            break;
        case Phase::Dangle:
            if (c.pictures >= kDanglePictures)
                startPhase(rdram, fighter, c, Phase::Recover, kRecover);
            break;
        case Phase::Recover:
            if (started && clipEnded(rdram, fighter))
                startPhase(rdram, fighter, c, Phase::Hang, hangClip(c.hold));
            break;
        case Phase::LetGo:
            // raised as the clip's body falls, so the feet reach the mat as the clip's reach its floor; never back down
            if (started)
            {
                const float fall = (kLetGoRoot[0] - tableAt(kLetGoRoot, readFloat(rdram, fighter + kFrame))) /
                                   (kLetGoRoot[0] - kLetGoLanded);
                const float lift = std::max(0.0f, (kMat + kAnkleUp) - (c.fromY + kLetGoAnkles));
                c.raised = std::max(c.raised, lift * std::clamp(fall, 0.0f, 1.0f));
                c.y = c.fromY + c.raised;
                aboveMat(rdram, fighter, c);
            }
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
                // facing the way the body ended up: the clip turns the body about 135 degrees back toward the ring but
                // keys no facing bone (+0x130 stays 0), so the game would start the stance (flags 0xE: facing = +0x148)
                // still facing the cage; then its turn to face (FUN_00102b08) takes the stance round to the opponent
                const float facing = bodyFacing(rdram, fighter);
                writeFloat(rdram, fighter + kFacing, facing);
                writeFloat(rdram, fighter + 0x148u, facing);
                g_afterLanding[index] = 40;
                setState(rdram, fighter, kStanding); // state 2 runs this picture and starts its own clip
                finish(rdram, ctx, fighter, 0u);
                return;
            }
            break;
        case Phase::None:
            break;
        }
        againstCage(rdram, fighter, c, c.phase != Phase::Onto && c.phase != Phase::LetGo);
        place(rdram, fighter, c);
        Ps2FastWrite16(rdram, fighter + kPlace,
                       static_cast<uint16_t>(Ps2FastRead16(rdram, fighter + kPlace) | kCantBeHit | kCantBeGrabbed));
        const uint32_t clip = c.clip;
        c.clip = 0;
        finish(rdram, ctx, fighter, clip, c.clipFlags);
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
                c.toRope = Ps2FastRead32(rdram, fighter + kHeld) & 0xFu;
                setState(rdram, fighter, kClimbState); // state 102 runs this picture: it starts the clip
                startPhase(rdram, fighter, c, Phase::Onto, kOnto, 2u); // blended in from the stance
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

// VENDETTA_CAGECLIMBLOG=2: after a landing, a line a picture for 40 (from the wall search, which runs for every fighter
// every picture: a picture once, by its clip and frame): the facing, the facing after the clip, the body's own facing,
// the heading from the hips to the other fighters' hips
void vendetta_cage::climbWatch(uint8_t *rdram, uint32_t fighter)
{
    using namespace vendetta_cage_climb;
    const int index = indexOf(fighter);
    if (index < 0 || g_afterLanding[index] <= 0 || logLevel() < 2)
        return;
    static float lastFrame[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    static uint32_t lastClip[4] = {};
    const float frame = readFloat(rdram, fighter + kFrame);
    const uint32_t clip = Ps2FastRead32(rdram, fighter + kClip);
    if (frame == lastFrame[index] && clip == lastClip[index])
        return;
    lastFrame[index] = frame;
    lastClip[index] = clip;
    --g_afterLanding[index];
    std::cerr << "[cage] after landing: clip " << clip << " f " << frame << " blend " << Ps2FastRead32(rdram, fighter + 0x10Cu)
              << " state " << Ps2FastRead32(rdram, fighter + kState) << " | facing " << readFloat(rdram, fighter + kFacing)
              << " after " << readFloat(rdram, fighter + 0x148u) << " body " << bodyFacing(rdram, fighter) << " | to";
    for (uint32_t j = 0; j < 4; ++j)
    {
        const uint32_t other = kFighters + kFighterBytes * j;
        if (static_cast<int>(j) == index || Ps2FastRead32(rdram, other + kState) == 0u)
            continue;
        const float dx = readFloat(rdram, other + kHips) - readFloat(rdram, fighter + kHips);
        const float dz = readFloat(rdram, other + kHips + 8u) - readFloat(rdram, fighter + kHips + 8u);
        std::cerr << " " << j << " " << std::atan2(dx, dz) * 180.0f / 3.14159265f;
    }
    std::cerr << std::endl;
}

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
