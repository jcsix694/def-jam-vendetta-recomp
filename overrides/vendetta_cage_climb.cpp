// Def Jam Vendetta (USA, SLUS-20639) - the cage match: climbing the cage from inside the ring, the game's side.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// Triangle with the stick toward a rope, which a cage stops from stepping out (vendetta_cage.cpp), starts the climb
// instead, ESCAPE on or off: state 2's check FUN_0013c1b8 is wrapped. The climber is in state 102 (the state table
// 0x27A400 has no routine there; its entry is pointed at ours for a cage match and put back after) and our code moves the
// wrestler, as the clips (the cage's own, standard\cage\animations\, vendetta_animations.cpp: cageClip) don't.
// Every decision of the climb is the climb's rules' (vendetta_cage_rules.h: one copy, built into this program and any
// other that plays the climb): the heights from the clips, the start, each picture's phase, anchor and clip. This file is
// the game's side of it: reading the fighter (its anchor, clip, stick, momentum, body points) and its model, posing the
// cage's clips on the climber's own skeleton for the rules, turning the stick's left and right into the side's way by the
// last picture's camera, the stick still held toward the rope from getting on (nothing until it is let go), writing back
// what the rules decide, playing the clip they name, and the logs. The rules' numbers come from the cage's cage.json
// (vendetta_cage_file.h).
// The climber can't be hit or grabbed (+0x5AC 0x4000 / 0x1 each picture: the hit test FUN_001290c0 and the grab skip it).
// At the top hold, up pulls the climber up to look over; with ESCAPE on the game's own tapping bar shows for the climber
// (the HUD FUN_0011B538 draws a fighter's bar from its own fields: +0x5CF bit 4, which the dispatcher clears before each
// state call, with +0x570 the taps needed and +0x574 the taps left; nothing else reads them), twice: the taps (any
// button, +0x66C & 0xFF0F, as the pin counts them) fill the first while straining, which puts the climber onto the top,
// and the second while crouched there, which leaves the climber resting on top; with ESCAPE off up there goes straight
// to resting on top. Resting on top, down with triangle (ESCAPE on) climbs over: at the end of the climb over, the escape wins the match, reported as
// the game reports a pin, through FUN_00135BB8(winners, losers, kind, 1) with a kind of its own (0x81: decided, and 0x80,
// which nothing reads: no end word, counted and scored as a pin is) with the slow motion flag 0x22FDE4 0x1000 set first
// (else a loser left standing walks out, state 120) and the match's phase 0x22FBE4 moved to 4; the climber then climbs
// down the outside and drops to the floor, standing there (+0x5AC the floor's 0x10), where state 2 starts the winner's
// victory. One on one and Every Man for Himself: the first out wins; tag and handicap (each of a team out) come later.
// Sat on top, cross dives off: the wrestler's own top-rope move, as from a turnbuckle (states 113 / 114, FUN_0013A828,
// which state 112 hands over to): the slot picked as FUN_0013A050 picks it (73 / 74 at a standing opponent, 75 / 76 at a
// downed one; 74 / 76 in Blazin'; none at an opponent outside the ring), the move record set by FUN_00102938(f, f, slot)
// as the climber's last call, the anchor on the cage's top ("the dive's take-off" numbers: the dive's clips stand the
// ankles 15.47 over the anchor), facing out (the state's own turn brings it round), +0x414 the ring's corner whose
// in-angle is nearest the way to the opponent (the aim's clamp is +-40 degrees round it). The dive's states are pointed
// at diveState in a cage match: on top it can't be grabbed or hit (a grab at a diver in 113-115 would be the
// turnbuckle's take-down), and in the air it comes down, +0x58C (the height the flight adds a picture, 0x138A5C, which
// the game keeps at 0 for an opponent in the ring) set so the anchor reaches the mat at the move's f0 (73 / 74, the
// hit window's start) or the flight's end (75 / 76); the take-off's rope windows are skipped while it is above the mat
// (they would pull the top rope up to the feet: vendetta_ring.cpp asks skipRopeWindow).
// A game routine called from here can be left half way by the scheduler (a checkpoint), so each of ours calls at most one,
// as its very last step, with its own return address: a pause then resumes it and it returns where it should. A state
// is set by our own writes (FUN_001015d0's, a leaf), never by calling it.
// VENDETTA_CAGECLIMBLOG=1: each phase change with the anchor and the hips (=2: also a line a picture, detail()).
// VENDETTA_CAGECLIMB_MOMENTUM=1 (tests): the climber's momentum kept full.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "vendetta_cage.h"
#include "vendetta_cage_file.h"
#include "vendetta_cage_rules.h"
#include "vendetta_ring.h"
#include "vendetta_store.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

uint32_t vendetta_animations_cageClip(uint32_t number); // vendetta_animations.cpp: this match's clip id, 0 none
const std::vector<uint8_t> *vendetta_animations_cageClipBytes(uint32_t number); // and its bytes, null none

namespace vendetta_cage_climb
{
    constexpr uint32_t kStateTable = 0x27A400u, kClimbState = 102u, kStanding = 2u;
    constexpr uint32_t kHookClimbState = 0x1DF794u; // the climber's state routine (no recompiled function covers it)
    constexpr uint32_t kRopesCheck = 0x13C1B8u;     // FUN_0013c1b8: state 2's triangle at the ropes (out through them)
    constexpr uint32_t kPlayClip = 0x132158u;       // FUN_00132158(fighter, clip, flags)
    constexpr uint32_t kFighters = 0x22FE08u, kFighterBytes = 0xCD8u;
    constexpr uint32_t kResult = 0x135BB8u;        // FUN_00135BB8(winners, losers, kind, 1): the match's result
    constexpr uint32_t kMatchFlags = 0x22FDE4u;    // halfword: 0x1000 the match decided (slow motion, no input)
    constexpr uint32_t kMatchPhase = 0x22FBE4u;    // 3 the fight, 4 its end
    constexpr uint32_t kMatchStep = 0x22FBD4u;     // this picture's time step (1 at full speed)
    constexpr uint32_t kMatchKind = 0x2361CEu;     // the kind byte: 1 one on one, 2 tag, 4 / 8 every man, 0x10 / 0x20 handicap
    constexpr uint32_t kDecided = 0x1000u, kFightPhase = 3u, kEndPhase = 4u, kEscapeKind = 0x81u;
    constexpr uint32_t kMoveSlot = 0x102938u;      // FUN_00102938(fighter, owner, slot): the move record for a slot
    constexpr uint32_t kHookDiveState = 0x1DF798u; // the dive's state routine in a cage match (no recompiled function covers it)
    constexpr uint32_t kDiveRoutine = 0x13A828u;   // FUN_0013A828: states 113-115, the top-rope dive
    constexpr uint32_t kDiveFirst = 0x71u, kDiveLast = 0x73u, kDiveStanding = 0x71u, kDiveDowned = 0x72u;
    constexpr uint32_t kMoveData = 0x025331C8u;    // the move data (moved into the store): + 0x7C + 16 x record = the moveset
    constexpr uint32_t kInAngles = 0x033A5C50u;    // the ring's corners' in-angles (the ring code's moved table)
    constexpr float kDiveAnkles = 15.47f;          // the dive clips' (and H202's) ankles over the anchor

    // the fighter's fields
    constexpr uint32_t kState = 0x40u, kX = 0xCCu, kY = 0xD0u, kZ = 0xD4u, kFacing = 0xDCu, kClip = 0xFCu,
                       kFrame = 0x110u, kRemaining = 0x114u, kPoints = 0x150u, kHips = 0x15Cu, kMomentum = 0x3B8u,
                       kPlace = 0x5ACu, kBlazin = 0x5C3u, kHeld = 0x668u, kPressed = 0x66Cu, kTouched = 0x7A0u,
                       kPlayer = 0x30u, kOut = 0x5BBu, kSide = 0x5BCu, kBarBits = 0x5CFu, kTapsNeeded = 0x570u,
                       kTapsLeft = 0x574u;
    constexpr uint16_t kInRing = 4u, kOnApron = 8u, kOnFloor = 0x10u, kCantBeGrabbed = 0x1u, kCantBeHit = 0x4000u;
    constexpr uint32_t kTriangle = 0x1000u, kCross = 0x4000u, kTapButtons = 0xFF0Fu;
    constexpr uint8_t kBar = 4u, kOutOfMatch = 0x10u, kComputer = 1u;
    constexpr uint32_t kUp = 1u, kRight = 2u, kDown = 4u, kLeft = 8u; // the stick's bits in +0x668 (as the corners' 3 6 12 9)

    // the rules' numbers, from the cage's cage.json (the standard cage's when it has none)
    const CageNumbers &num() { return vendetta_cage::cageFile().numbers; }

    // ---- the cage's clips posed on the climber's own skeleton as the game poses them, for the rules' heights. The rig
    // 0x1E13980 + 0x34C x fighter: + 0x334 its model, a WRS (a part's data at the model + 0x10 + 4n + its word there): slot
    // 1 the body (High Poly) with its bones (0x190 bytes each: child, sibling, the local matrix at + 0x50), slot 8 the
    // body's template ({flags 1 rotation / 2 position / 4 skip, a rest quaternion, a rest position} a bone); FUN_001b79e8
    // samples a keyed channel (the last key before the frame blended with the next: Euler keys x 1/4096 radians made
    // quaternions, FUN_001b6f08, slerped, FUN_001b7020; positions x 1/128 lerped); FUN_001b8f70: a bone the template
    // doesn't key keeps its rest local matrix; a bone's world = its local x its parent's. Times the model's draw scale
    // (+ 0x08; the body points include it). Body point 0 is bone 1 (the root), point 1 bone 3 (the hips), points 16 / 19
    // bones 29 / 39 (the ankles).
    constexpr uint32_t kRigs = 0x1E13980u, kRigSize = 0x34Cu, kRigModel = 0x334u, kBoneBytes = 0x190u;
    constexpr uint32_t kRootBone = 1u, kHipsBone = 3u, kLeftAnkleBone = 29u, kRightAnkleBone = 39u;

    using Matrix = std::array<float, 16>; // row-major, row vectors (the game's)
    struct TemplateBone
    {
        uint32_t flags = 0;
        float q[4] = {}, p[3] = {};
    };
    struct Skeleton
    {
        std::vector<Matrix> rest;
        std::vector<int> parent;
        std::vector<TemplateBone> tmpl;
        float scale = 1.0f;
    };
    struct Key
    {
        uint16_t t;
        int16_t x, y, z;
    };
    struct Channel
    {
        std::vector<Key> rot, pos;
    };

    // a climber's heights from the rules, for the model they were worked out on
    struct Heights
    {
        uint32_t model = 0;
        bool ready = false;
        CageHeights h{};
    };
    Heights g_heights[4];

    const uint8_t *guestBytes(uint8_t *rdram, uint32_t address, uint32_t size)
    {
        const uint32_t a = address & 0x1FFFFFFFu;
        if (a < 0x02000000u)
            return a + size <= 0x02000000u ? rdram + a : nullptr;
        return vendetta_store::readable(a, size) ? vendetta_store::host(a) : nullptr;
    }
    uint32_t u32(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
    float f32(const uint8_t *p) { float v; std::memcpy(&v, p, 4); return v; }

    // a model part's guest address (slot n), or 0
    uint32_t partOf(uint8_t *rdram, uint32_t model, uint32_t slot)
    {
        const uint8_t *w = guestBytes(rdram, model + 0x10u + 4u * slot, 4u);
        if (!w || u32(w) == 0xFFFFFFFFu)
            return 0u;
        return model + 0x10u + 4u * slot + u32(w);
    }

    bool readSkeleton(uint8_t *rdram, uint32_t model, Skeleton &s, std::string &why)
    {
        const uint8_t *head = guestBytes(rdram, model, 0x90u);
        if (!head || std::memcmp(head, "WRS ", 4) != 0)
            return why = "no model", false;
        s.scale = f32(head + 8);
        if (!(s.scale > 0.1f && s.scale < 10.0f))
            s.scale = 1.0f;
        const uint32_t body = partOf(rdram, model, 1u), tmpl = partOf(rdram, model, 8u);
        const uint8_t *bodyHead = body ? guestBytes(rdram, body, 16u) : nullptr;
        const uint8_t *tmplHead = tmpl ? guestBytes(rdram, tmpl, 4u) : nullptr;
        if (!bodyHead || !tmplHead)
            return why = "no body or template", false;
        const uint32_t bones = u32(bodyHead + 8), count = u32(tmplHead);
        if (bones < 40u || bones > 255u || count == 0u || count > 255u)
            return why = "a body of " + std::to_string(bones) + " bones, a template of " + std::to_string(count), false;
        const uint8_t *b = guestBytes(rdram, body + 0x10u, bones * kBoneBytes);
        const uint8_t *t = guestBytes(rdram, tmpl + 4u, count * 32u);
        if (!b || !t)
            return why = "the body's bones can't be read", false;
        s.rest.assign(bones, Matrix{});
        s.parent.assign(bones, -1);
        for (uint32_t i = 0; i < bones; ++i)
            std::memcpy(s.rest[i].data(), b + i * kBoneBytes + 0x50u, 64u);
        for (uint32_t i = 0; i < bones; ++i)
        {
            int32_t c = static_cast<int32_t>(u32(b + i * kBoneBytes));
            for (int guard = 0; c >= 0 && c < static_cast<int32_t>(bones) && guard < 256; ++guard)
            {
                s.parent[c] = static_cast<int>(i);
                c = static_cast<int32_t>(u32(b + c * kBoneBytes + 4u));
            }
        }
        s.tmpl.assign(count, TemplateBone{});
        for (uint32_t i = 0; i < count; ++i)
        {
            s.tmpl[i].flags = u32(t + i * 32u);
            std::memcpy(s.tmpl[i].q, t + i * 32u + 4u, 16u);
            std::memcpy(s.tmpl[i].p, t + i * 32u + 20u, 12u);
        }
        return true;
    }

    // a clip's channels for a template (the clip: {u16 last frame, u8 n6} n6 x 6 bytes, then per template bone not
    // skipped its rotation keys then its position keys, each a u8 count and {u16 frame, s16 x, y, z} a key)
    bool readClip(const std::vector<uint8_t> &bytes, const Skeleton &s, uint32_t &last, std::vector<Channel> &out)
    {
        if (bytes.size() < 3u)
            return false;
        last = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8);
        size_t p = 3u + static_cast<size_t>(bytes[2]) * 6u;
        out.assign(s.tmpl.size(), Channel{});
        auto keys = [&](std::vector<Key> &into) {
            if (p >= bytes.size())
                return false;
            const size_t n = bytes[p++];
            if (p + n * 8u > bytes.size())
                return false;
            into.resize(n);
            for (size_t k = 0; k < n; ++k, p += 8u)
                std::memcpy(&into[k], &bytes[p], 8u);
            return true;
        };
        for (size_t i = 0; i < s.tmpl.size(); ++i)
        {
            const uint32_t flags = s.tmpl[i].flags;
            if (flags & 4u)
                continue;
            if ((flags & 1u) && !keys(out[i].rot))
                return false;
            if ((flags & 2u) && !keys(out[i].pos))
                return false;
        }
        return true;
    }

    void eulerQuat(float x, float y, float z, float q[4])
    {
        const float sx = std::sin(x / 2), sy = std::sin(y / 2), sz = std::sin(z / 2);
        const float cx = std::cos(x / 2), cy = std::cos(y / 2), cz = std::cos(z / 2);
        q[0] = cz * sx * cy - sz * cx * sy;
        q[1] = cz * cx * sy + sz * sx * cy;
        q[2] = sz * cx * cy - cz * sx * sy;
        q[3] = -(cz * cx * cy + sz * sx * sy);
    }
    // the last key before frame t (else the first), the next, and the first's weight
    void pick(const std::vector<Key> &keys, float t, const Key *&a, const Key *&b, float &w)
    {
        size_t k = 0;
        for (size_t i = 0; i < keys.size(); ++i)
            if (static_cast<float>(keys[i].t) < t)
                k = i;
        a = &keys[k];
        b = k + 1 < keys.size() ? &keys[k + 1] : a;
        w = b->t == a->t ? 1.0f : 1.0f - (t - a->t) / static_cast<float>(b->t - a->t);
    }
    Matrix quatMatrix(const float q[4])
    {
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        return {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y), 0,
                2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x), 0,
                2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y), 0,
                0, 0, 0, 1};
    }
    Matrix mul(const Matrix &a, const Matrix &b)
    {
        Matrix r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
        return r;
    }

    // every bone's world matrix at frame t (in the clip's space, unscaled)
    std::vector<Matrix> pose(const Skeleton &s, const std::vector<Channel> &ch, float t)
    {
        const size_t n = s.rest.size();
        std::vector<Matrix> local(n), world(n);
        for (size_t i = 0; i < n; ++i)
        {
            if (i >= s.tmpl.size() || s.tmpl[i].flags == 0u)
            {
                local[i] = s.rest[i];
                continue;
            }
            const TemplateBone &tb = s.tmpl[i];
            Matrix m = s.rest[i];
            m[3] = m[7] = m[11] = 0.0f;
            m[15] = 1.0f;
            float pos[3] = {tb.p[0], tb.p[1], tb.p[2]};
            if (!(tb.flags & 4u) && (tb.flags & 1u))
            {
                float q[4] = {tb.q[0], tb.q[1], tb.q[2], tb.q[3]};
                if (!ch[i].rot.empty())
                {
                    const Key *a, *b;
                    float w;
                    pick(ch[i].rot, t, a, b, w);
                    float qa[4], qb[4];
                    eulerQuat(a->x / 4096.0f, a->y / 4096.0f, a->z / 4096.0f, qa);
                    eulerQuat(b->x / 4096.0f, b->y / 4096.0f, b->z / 4096.0f, qb);
                    float dot = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
                    if (dot < 0.0f)
                    {
                        for (float &v : qb)
                            v = -v;
                        dot = -dot;
                    }
                    float wa = w, wb = 1.0f - w;
                    if (1.0f - dot > 1e-4f)
                    {
                        const float th = std::acos(std::min(1.0f, dot)), sth = std::sin(th);
                        wb = std::sin(th * (1.0f - w)) / sth;
                        wa = std::sin(th * w) / sth;
                    }
                    for (int k = 0; k < 4; ++k)
                        q[k] = qa[k] * wa + qb[k] * wb;
                }
                const Matrix r = quatMatrix(q);
                std::copy(r.begin(), r.begin() + 12, m.begin());
            }
            if (!(tb.flags & 4u) && (tb.flags & 2u) && !ch[i].pos.empty())
            {
                const Key *a, *b;
                float w;
                pick(ch[i].pos, t, a, b, w);
                pos[0] = (a->x * w + b->x * (1.0f - w)) / 128.0f;
                pos[1] = (a->y * w + b->y * (1.0f - w)) / 128.0f;
                pos[2] = (a->z * w + b->z * (1.0f - w)) / 128.0f;
            }
            m[12] = pos[0];
            m[13] = pos[1];
            m[14] = pos[2];
            local[i] = m;
        }
        std::vector<char> done(n, 0);
        std::function<const Matrix &(size_t)> get = [&](size_t i) -> const Matrix & {
            if (!done[i])
            {
                const int p = s.parent[i];
                world[i] = p < 0 || static_cast<size_t>(p) == i ? local[i] : mul(local[i], get(static_cast<size_t>(p)));
                done[i] = 1;
            }
            return world[i];
        };
        for (size_t i = 0; i < n; ++i)
            get(i);
        return world;
    }

    // a dive from the top, by fighter: on from the hand-over until its states end
    struct Dive
    {
        bool on = false;
        uint32_t slot = 0;
    };
    Dive g_dives[4];
    uint32_t g_diveTableWas[3] = {};
    bool g_diveTableSet = false;

    const char *phaseName(int32_t p)
    {
        switch (p)
        {
        case CAGE_PHASE_NONE: return "none";
        case CAGE_PHASE_ONTO: return "onto the cage";
        case CAGE_PHASE_HANG: return "hanging";
        case CAGE_PHASE_STEP: return "a step";
        case CAGE_PHASE_SLIP: return "slipping";
        case CAGE_PHASE_DANGLE: return "dangling";
        case CAGE_PHASE_RECOVER: return "back to the hang";
        case CAGE_PHASE_LET_GO: return "letting go";
        case CAGE_PHASE_PULL_UP: return "pulling up";
        case CAGE_PHASE_STRAIN: return "straining at the top";
        case CAGE_PHASE_PULL_BACK: return "back from the top";
        case CAGE_PHASE_ONTO_TOP: return "onto the top";
        case CAGE_PHASE_CROUCH: return "crouched on top";
        case CAGE_PHASE_ON_TOP: return "resting on top";
        case CAGE_PHASE_OFF_TOP: return "down from the top";
        case CAGE_PHASE_OVER: return "over the top";
        case CAGE_PHASE_DOWN_OUTSIDE: return "down the outside";
        case CAGE_PHASE_DROP: return "dropping outside";
        }
        return "?";
    }

    // a climber: the rules' state, and the game's own: the screen's way of the side, the stick held toward the rope
    struct Climb
    {
        CageClimber c{};
        float screen = 0;    // +1 / -1: the side's way across the screen, as last seen
        uint32_t toRope = 0; // the stick held toward the rope to get on: nothing until let go
    };
    Climb g_climbs[4];
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

    // the anchor and facing the rules hand out, written to the fighter
    void place(uint8_t *rdram, uint32_t fighter, const CagePictureOut &out)
    {
        writeFloat(rdram, fighter + kX, out.x);
        writeFloat(rdram, fighter + kY, out.y);
        writeFloat(rdram, fighter + kZ, out.z);
        writeFloat(rdram, fighter + kFacing, out.facing);
    }

    void say(uint8_t *rdram, uint32_t fighter, const Climb &cl, const char *what, int32_t phase, int32_t hold, float out,
             float along, float y)
    {
        if (!logOn())
            return;
        std::cerr << "[cage] climb: player " << (Ps2FastRead32(rdram, fighter + 0x30u) & 3u) << " " << what << " ("
                  << phaseName(phase) << ", hold " << hold << ", out " << out << " along " << along << " y " << y
                  << ", hips " << readFloat(rdram, fighter + kHips) << " " << readFloat(rdram, fighter + kHips + 4u) << " "
                  << readFloat(rdram, fighter + kHips + 8u) << ", momentum " << readFloat(rdram, fighter + kMomentum)
                  << ", screen " << cl.screen << ")" << std::endl;
    }
    void sayStarted(uint8_t *rdram, uint32_t fighter, const Climb &cl, const CagePictureOut &out)
    {
        if (out.started)
            say(rdram, fighter, cl, "starts", out.startedPhase, out.startedHold, out.startedOut, out.startedAlong,
                out.startedY);
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
    int g_afterLanding[4] = {}; // VENDETTA_CAGECLIMBLOG=2: pictures still to log after a landing

    // VENDETTA_CAGECLIMBLOG=2: a line a picture while getting on or letting go, and for the first pictures of every
    // phase: the clip's frame, the blend's count (+0x10C), the anchor, and body points as (out, along, up) from the side:
    // 0 the root, 1 the hips, 9 / 13 the fists, 16 / 19 the ankles (the ropes at out 35, up 24.1 / 19.3 / 14.5; the mat 10.5)
    void detail(uint8_t *rdram, uint32_t fighter, const CageClimber &c)
    {
        if (logLevel() < 2 || !(c.phase == CAGE_PHASE_ONTO || c.phase == CAGE_PHASE_LET_GO || c.pictures <= 3))
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

    // A climber's heights for the rules, when a climb starts (again only for another model or match): the cage's clips
    // posed on its skeleton (getting on every frame, the hang's first, letting go every frame) and getting on's rope bends
    // from cage.json
    bool workOutHeights(uint8_t *rdram, int index, std::string &why)
    {
        Heights &hs = g_heights[index];
        const uint32_t model = Ps2FastRead32(rdram, kRigs + kRigSize * static_cast<uint32_t>(index) + kRigModel);
        if (model != 0u && hs.model == model && hs.ready)
            return true;
        Skeleton s;
        if (!readSkeleton(rdram, model, s, why))
            return false;
        if (s.rest.size() <= kRightAnkleBone)
            return why = "the body has too few bones", false;
        const std::vector<uint8_t> *onto = vendetta_animations_cageClipBytes(CAGE_CLIP_ONTO);
        const std::vector<uint8_t> *hang = vendetta_animations_cageClipBytes(CAGE_CLIP_HANG);
        const std::vector<uint8_t> *letGo = vendetta_animations_cageClipBytes(CAGE_CLIP_LET_GO);
        const std::vector<uint8_t> *dropOff = vendetta_animations_cageClipBytes(CAGE_CLIP_DROP);
        if (!onto || !hang || !letGo || !dropOff)
            return why = "the cage's clips 3203, 3204, 3205 and 3206 aren't all there", false;
        std::vector<Channel> co, ch, cl, cd;
        uint32_t lastO = 0, lastH = 0, lastL = 0, lastD = 0;
        if (!readClip(*onto, s, lastO, co) || !readClip(*hang, s, lastH, ch) || !readClip(*letGo, s, lastL, cl) ||
            !readClip(*dropOff, s, lastD, cd))
            return why = "a clip doesn't fit the body's template", false;
        const float sc = s.scale;
        auto frameOf = [&](const std::vector<Channel> &channels, float t) {
            const std::vector<Matrix> w = pose(s, channels, t);
            auto point = [&](uint32_t bone) { return CagePoint{w[bone][12] * sc, w[bone][13] * sc, w[bone][14] * sc}; };
            return CageClipFrame{point(kRootBone), point(kHipsBone), point(kLeftAnkleBone), point(kRightAnkleBone)};
        };
        std::vector<CageClipFrame> ontoFrames, letGoFrames;
        for (uint32_t f = 0; f <= lastO; ++f)
            ontoFrames.push_back(frameOf(co, static_cast<float>(f)));
        for (uint32_t f = 0; f <= lastL; ++f)
            letGoFrames.push_back(frameOf(cl, static_cast<float>(f)));
        const CageClipFrame hangFirst = frameOf(ch, 0.0f), dropLast = frameOf(cd, static_cast<float>(lastD));
        std::vector<CageRopeBend> bends;
        if (const auto b = vendetta_cage::cageFile().ropeBends.find(vendetta_cage::clipKey(CAGE_CLIP_ONTO));
            b != vendetta_cage::cageFile().ropeBends.end())
            for (const vendetta_cage::CageWindow &w : b->second)
                bends.push_back({static_cast<int32_t>(w.from), static_cast<int32_t>(w.to), static_cast<int32_t>(w.what)});
        if (!cage_rules_work_out_heights(&num(), ontoFrames.data(), static_cast<int32_t>(ontoFrames.size()), bends.data(),
                                         static_cast<int32_t>(bends.size()), &hangFirst, letGoFrames.data(),
                                         static_cast<int32_t>(letGoFrames.size()), &dropLast, &hs.h))
            return why = "a clip has more frames than the climb's rules take (" + std::to_string(CAGE_MOST_FRAMES) + ")", false;
        hs.model = model;
        hs.ready = true;
        const CageHeights &h = hs.h;
        std::cerr << "[cage] climb: player " << index << "'s heights from the clips (draw scale " << sc << "): getting on under the mat";
        for (int32_t f = 0; f < h.ontoFrames; f += 8)
            std::cerr << " " << std::round(h.drop[f] * 10.0f) / 10.0f;
        std::cerr << " (frame " << h.ontoFrames - 1 << " " << h.drop[h.ontoFrames - 1] << "); the hang moved along " << h.hang[0]
                  << ", up " << h.hang[1] << ", out " << h.hang[2] << " (the rope hold " << cage_rules_rope_hold(&h)
                  << "); letting go lands at frame " << h.landing << ", its lower ankle " << h.landingAnkle
                  << " from the hold; the hang's hips " << h.hangHips[0] << " " << h.hangHips[1] << " " << h.hangHips[2]
                  << "; the drop's lower ankle " << h.dropLanding << " (the climb's rules version " << cage_rules_version()
                  << ")" << std::endl;
        return true;
    }

    // which way along the side the stick's left / right go: the side's way as it runs across the screen, from the
    // climber's chest (the last answer kept for a picture with no camera)
    float screenSign(Climb &cl)
    {
        const CageClimber &c = cl.c;
        const float x = c.nx * c.out + c.tx * c.along, z = c.nz * c.out + c.tz * c.along;
        float sign = 0.0f;
        if (vendetta_cage::screenSide(x, c.y + 8.0f, z, c.tx, c.tz, sign))
            cl.screen = sign;
        return cl.screen != 0.0f ? cl.screen : 1.0f;
    }

    // a body point as (out, along, up) from the anchor the fighter has (where the last pose was posed)
    CagePoint fromAnchor(uint8_t *rdram, uint32_t fighter, const CageClimber &c, uint32_t point)
    {
        const uint32_t a = fighter + kPoints + 12u * point;
        const float dx = readFloat(rdram, a) - readFloat(rdram, fighter + kX);
        const float dy = readFloat(rdram, a + 4u) - readFloat(rdram, fighter + kY);
        const float dz = readFloat(rdram, a + 8u) - readFloat(rdram, fighter + kZ);
        return CagePoint{dx * c.nx + dz * c.nz, dx * c.tx + dz * c.tz, dy};
    }

    // the match kind's teams: tag and handicap (each member of a team must be out: not yet)
    bool teamMatch(uint8_t *rdram) { return (Ps2FastRead8(rdram, kMatchKind) & 0x32u) != 0u; }

    // the escape: the climber's side wins. The lists ({the fighters' +0x30 ...}, -1) in the cage's own store area; the
    // match decided (0x1000) before the report so no loser left standing walks out; the phase moved to the end as the
    // match step does after a decided report. False when the fight isn't on (the result is the game's already).
    bool reportEscape(uint8_t *rdram, uint32_t fighter, uint32_t &winners, uint32_t &losers)
    {
        if (Ps2FastRead32(rdram, kMatchPhase) != kFightPhase || (Ps2FastRead16(rdram, kMatchFlags) & kDecided))
            return false;
        int32_t won[2] = {static_cast<int32_t>(Ps2FastRead32(rdram, fighter + kPlayer)), -1};
        int32_t lost[5] = {-1, -1, -1, -1, -1};
        int count = 0;
        for (uint32_t j = 0; j < 4u; ++j)
        {
            const uint32_t other = kFighters + kFighterBytes * j;
            const int32_t who = static_cast<int32_t>(Ps2FastRead32(rdram, other + kPlayer));
            if (other == fighter || who == -1 || (Ps2FastRead8(rdram, other + kOut) & kOutOfMatch))
                continue;
            lost[count++] = who;
        }
        if (count == 0 || !vendetta_cage::resultLists(won, 2, lost, count + 1, winners, losers))
            return false;
        Ps2FastWrite16(rdram, kMatchFlags, static_cast<uint16_t>(Ps2FastRead16(rdram, kMatchFlags) | kDecided));
        Ps2FastWrite32(rdram, kMatchPhase, kEndPhase);
        if (logOn())
        {
            std::cerr << "[cage] escape: player " << won[0] << " is over the top and wins; the losers";
            for (int k = 0; k < count; ++k)
                std::cerr << " " << lost[k];
            std::cerr << std::endl;
        }
        return true;
    }

    // the wrestler's top-rope slot for its opponent, as FUN_0013A050 picks it (0: none fits): the opponent held for a
    // partner (state 0x3B, the shared 10004) and on the apron or the floor give none here
    uint32_t diveSlot(uint8_t *rdram, uint32_t fighter, uint32_t opponent)
    {
        const uint16_t place = Ps2FastRead16(rdram, opponent + kPlace);
        const uint32_t state = Ps2FastRead32(rdram, opponent + kState);
        if (!(place & kInRing) || (place & (kOnApron | kOnFloor)) || state == 0x3Bu)
            return 0u;
        const bool down = ((place & 0x80u) && !(place & 0xC00u)) || (state >= 0x50u && state <= 0x53u) || state == 0x69u ||
                          state == 0x6Au;
        const bool blazin = (Ps2FastRead8(rdram, fighter + kBlazin) & 2u) != 0u;
        // FUN_00102AC8: the moveset (at the move data + 0x7C + 16 x the record +0x38) has the slot when its entry isn't -1
        const uint8_t *entry = guestBytes(rdram, kMoveData + 0x7Cu + 16u * Ps2FastRead32(rdram, fighter + 0x38u), 4u);
        const uint32_t moveset = entry ? u32(entry) : 0u;
        auto has = [&](uint32_t slot) {
            const uint8_t *s = moveset ? guestBytes(rdram, moveset + 2u * slot, 2u) : nullptr;
            return s && (s[0] != 0xFFu || s[1] != 0xFFu);
        };
        const uint32_t plain = down ? 75u : 73u, hot = down ? 76u : 74u;
        if (blazin && has(hot))
            return hot;
        return has(plain) ? plain : 0u;
    }

    // the ring's corner whose in-angle is nearest the way from (x, z) to (toX, toZ): the aim is clamped round it
    uint32_t nearestCorner(uint8_t *rdram, float x, float z, float toX, float toZ)
    {
        const uint32_t corners = vendetta_ring::activeSides() == 6 ? 6u : 4u;
        const float way = std::atan2(toX - x, toZ - z) * 180.0f / 3.14159265f;
        uint32_t best = 0;
        float bestOff = 1e9f;
        for (uint32_t k = 0; k < corners; ++k)
        {
            const uint8_t *a = guestBytes(rdram, kInAngles + 4u * k, 4u);
            if (!a)
                break;
            float off = std::fmod(std::fabs(f32(a) - way), 360.0f);
            off = std::min(off, 360.0f - off);
            if (off < bestOff)
            {
                bestOff = off;
                best = k;
            }
        }
        return best;
    }

    // cross on top: the dive handed over (true: the climb is over, the routine's last call made)
    bool startDive(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t fighter, int index, Climb &cl)
    {
        const uint32_t opponent = Ps2FastRead32(rdram, fighter + 0x0u);
        if (indexOf(opponent) < 0)
            return false;
        const uint32_t slot = diveSlot(rdram, fighter, opponent);
        if (!slot)
        {
            static uint32_t told = 0;
            if (logOn() && told++ % 60u == 0u)
                std::cerr << "[cage] climb: player " << index << " on top: no dive fits the opponent now" << std::endl;
            return false;
        }
        CageClimber &c = cl.c;
        const CageNumbers &n = num();
        const float out = n.walls + n.diveOut, y = 10.5f + n.height + n.diveFeet - kDiveAnkles;
        const float x = c.nx * out + c.tx * c.along, z = c.nz * out + c.tz * c.along;
        const uint32_t corner = nearestCorner(rdram, readFloat(rdram, fighter + kHips), readFloat(rdram, fighter + kHips + 8u),
                                              readFloat(rdram, opponent + kHips), readFloat(rdram, opponent + kHips + 8u));
        const uint32_t state = (slot == 75u || slot == 76u) ? kDiveDowned : kDiveStanding;
        if (logOn())
            std::cerr << "[cage] climb: player " << index << " dives off the top: slot " << slot << ", state " << state
                      << ", take-off out " << out << " along " << c.along << " y " << y << ", corner " << corner
                      << " (pressed 0x" << std::hex << Ps2FastRead32(rdram, fighter + kPressed) << std::dec << ")" << std::endl;
        c.phase = CAGE_PHASE_NONE;
        g_dives[index] = Dive{true, slot};
        writeFloat(rdram, fighter + kX, x);
        writeFloat(rdram, fighter + kY, y);
        writeFloat(rdram, fighter + kZ, z);
        Ps2FastWrite32(rdram, fighter + 0x414u, corner);
        writeFloat(rdram, fighter + 0x58Cu, 0.0f);
        setState(rdram, fighter, state);
        Ps2FastWrite16(rdram, fighter + kPlace, static_cast<uint16_t>(Ps2FastRead16(rdram, fighter + kPlace) | kInRing |
                                                                        kCantBeHit | kCantBeGrabbed));
        SET_GPR_U64(ctx, 4, fighter);
        SET_GPR_U64(ctx, 5, fighter);
        SET_GPR_U64(ctx, 6, slot);
        runtime->lookupFunction(kMoveSlot)(rdram, ctx, runtime);
        return true;
    }

    // states 113-115 in a cage match: a dive from the cage's top kept safe on top and brought down in the air; any other
    // (a turnbuckle's) the game's own
    void diveState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t fighter = getRegU32(ctx, 4);
        const int index = indexOf(fighter);
        if (index >= 0 && g_dives[index].on)
        {
            const uint32_t step = Ps2FastRead32(rdram, fighter + 0x44u);
            if (step <= 1u)
                Ps2FastWrite16(rdram, fighter + kPlace,
                               static_cast<uint16_t>(Ps2FastRead16(rdram, fighter + kPlace) | kCantBeHit | kCantBeGrabbed));
            const float y = readFloat(rdram, fighter + kY), frame = readFloat(rdram, fighter + kFrame);
            const int32_t f0 = static_cast<int32_t>(Ps2FastRead32(rdram, fighter + 0x360u));
            const int32_t f5 = static_cast<int32_t>(Ps2FastRead32(rdram, fighter + 0x374u));
            const int32_t f6 = static_cast<int32_t>(Ps2FastRead32(rdram, fighter + 0x378u));
            const int32_t left = static_cast<int32_t>(Ps2FastRead32(rdram, fighter + 0x424u));
            float add = 0.0f;
            if (step >= 2u && frame >= static_cast<float>(f5) && left >= 1 && y > 10.5f)
            {
                // the frames the flight still has, over its pictures left: the pictures to the drop's end
                const bool standing = g_dives[index].slot == 73u || g_dives[index].slot == 74u;
                const float end = (standing && f0 > f5 && f0 < f6) ? static_cast<float>(f0) : static_cast<float>(f6);
                const float perPicture = (static_cast<float>(f6) - frame) / static_cast<float>(left);
                const float pictures = perPicture > 0.0f ? std::ceil((end - frame) / perPicture) : 1.0f;
                add = -(y - 10.5f) / std::max(1.0f, pictures);
            }
            writeFloat(rdram, fighter + 0x58Cu, add);
            if (logLevel() >= 2)
            {
                // the landing spot +0x57C (x, z), +0x5CB: 1 out of reach, 2 a downed target, 8 a hit, 0x40 aimed
                std::cerr << "[cage] dive: player " << index << " state " << Ps2FastRead32(rdram, fighter + kState) << " step "
                          << step << " clip " << Ps2FastRead32(rdram, fighter + kClip) << " f " << frame << " (f0 " << f0
                          << " f5 " << f5 << " f6 " << f6 << ") left " << left << " y " << y << " add " << add << " anchor "
                          << readFloat(rdram, fighter + kX) << " " << readFloat(rdram, fighter + kZ) << " facing "
                          << readFloat(rdram, fighter + kFacing) << " aim " << readFloat(rdram, fighter + 0x540u) << " to "
                          << readFloat(rdram, fighter + 0x57Cu) << " " << readFloat(rdram, fighter + 0x584u) << " flags 0x"
                          << std::hex << static_cast<uint32_t>(Ps2FastRead8(rdram, fighter + 0x5CBu)) << std::dec;
                if (const uint32_t o = Ps2FastRead32(rdram, fighter + 0x0u); indexOf(o) >= 0)
                    std::cerr << " | opponent hips " << readFloat(rdram, o + kHips) << " " << readFloat(rdram, o + kHips + 4u)
                              << " " << readFloat(rdram, o + kHips + 8u) << " state " << Ps2FastRead32(rdram, o + kState);
                std::cerr << std::endl;
            }
        }
        runtime->lookupFunction(kDiveRoutine)(rdram, ctx, runtime);
    }

    // state 102: the climber, each picture
    void climbState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_runtime = runtime;
        const uint32_t fighter = getRegU32(ctx, 4);
        const int index = indexOf(fighter);
        if (index < 0 || !g_active || g_climbs[index].c.phase == CAGE_PHASE_NONE)
        {
            if (index >= 0)
                setState(rdram, fighter, kStanding);
            finish(rdram, ctx, fighter, 0u);
            return;
        }
        Climb &cl = g_climbs[index];
        CageClimber &c = cl.c;
        detail(rdram, fighter, c);
        if (logOn() && (c.pictures + 1) % 30 == 0)
            std::cerr << "[cage] climb: " << phaseName(c.phase) << " picture " << c.pictures + 1 << ", clip "
                      << Ps2FastRead32(rdram, fighter + kClip) << " frame " << readFloat(rdram, fighter + kFrame) << " left "
                      << readFloat(rdram, fighter + kRemaining) << ", held " << (Ps2FastRead32(rdram, fighter + kHeld) & 0xFu)
                      << ", hips y " << readFloat(rdram, fighter + kHips + 4u) << ", anchor y " << c.y << " out " << c.out
                      << std::endl;
        uint32_t held = Ps2FastRead32(rdram, fighter + kHeld) & 0xFu;
        if (!(held & cl.toRope))
            cl.toRope = 0;
        held &= ~cl.toRope;
        static const bool fullMomentum = []() { const char *v = std::getenv("VENDETTA_CAGECLIMB_MOMENTUM"); return v && v[0] == '1'; }();
        if (fullMomentum)
            writeFloat(rdram, fighter + kMomentum, 100.0f);

        CagePictureIn in{};
        in.frame = readFloat(rdram, fighter + kFrame);
        const float left = readFloat(rdram, fighter + kRemaining), whole = in.frame + left;
        in.progress = whole > 0.0f ? std::clamp(in.frame / whole, 0.0f, 1.0f) : 1.0f;
        in.clipEnded = left <= 0.0f ? 1 : 0;
        in.up = (held & kUp) ? 1 : 0;
        in.down = (held & kDown) ? 1 : 0;
        if (held & (kLeft | kRight))
            in.along = ((held & kRight) ? 1 : -1) * static_cast<int32_t>(screenSign(cl));
        in.momentum = readFloat(rdram, fighter + kMomentum);
        in.blazin = (Ps2FastRead8(rdram, fighter + kBlazin) & 2u) ? 1 : 0;
        in.step = readFloat(rdram, kMatchStep);
        const uint32_t pressed = Ps2FastRead32(rdram, fighter + kPressed);
        // the computer's taps are ours (its own come only in a pin): one every 6 pictures, as its kick-out's
        in.pressed = (Ps2FastRead8(rdram, fighter + kSide) & kComputer) ? (c.pictures % 6 == 0 ? 1 : 0)
                                                                          : ((pressed & kTapButtons) ? 1 : 0);
        in.triangle = (pressed & kTriangle) ? 1 : 0;
        in.cross = (pressed & kCross) ? 1 : 0;
        in.escape = (Ps2FastRead16(rdram, vendetta_cage::kRuleWord) & vendetta_cage::kRuleEscape) ? 1 : 0;
        in.mayGoOver = in.escape && !teamMatch(rdram) ? 1 : 0;
        for (int32_t i = 0; i < CAGE_ROOMS; ++i)
            in.body.rooms[i] = fromAnchor(rdram, fighter, c, vendetta_cage::kRoomPoints[i]);
        in.body.ankleLeft = fromAnchor(rdram, fighter, c, 16u);
        in.body.ankleRight = fromAnchor(rdram, fighter, c, 19u);
        in.body.hips = fromAnchor(rdram, fighter, c, 1u);
        in.body.hipLeft = fromAnchor(rdram, fighter, c, 14u);
        in.body.hipRight = fromAnchor(rdram, fighter, c, 17u);

        const CageClimber before = c;
        CagePictureOut out{};
        cage_rules_picture(&num(), &g_heights[index].h, &c, &in, &out);
        if (out.momentumCost > 0.0f)
            writeFloat(rdram, fighter + kMomentum, std::max(0.0f, in.momentum - out.momentumCost));
        sayStarted(rdram, fighter, cl, out);
        if (out.bar)
        {
            // the game's bar for this fighter, this picture (its fill = 1 - left / needed)
            Ps2FastWrite8(rdram, fighter + kBarBits, static_cast<uint8_t>(Ps2FastRead8(rdram, fighter + kBarBits) | kBar));
            writeFloat(rdram, fighter + kTapsNeeded, out.barNeeded);
            writeFloat(rdram, fighter + kTapsLeft, out.barLeft);
            if (logOn() && c.taps != before.taps)
                std::cerr << "[cage] climb: player " << index << " taps " << c.taps << " of " << out.barNeeded << std::endl;
        }
        if (out.dive && startDive(rdram, ctx, runtime, fighter, index, cl))
            return;
        if (out.escaped)
        {
            // the match won this picture: the report is the routine's last call (the next clip starts next picture)
            place(rdram, fighter, out);
            Ps2FastWrite16(rdram, fighter + kPlace,
                           static_cast<uint16_t>(Ps2FastRead16(rdram, fighter + kPlace) | kCantBeHit | kCantBeGrabbed));
            uint32_t winners = 0, losers = 0;
            if (reportEscape(rdram, fighter, winners, losers))
            {
                SET_GPR_U64(ctx, 4, winners);
                SET_GPR_U64(ctx, 5, losers);
                SET_GPR_U64(ctx, 6, kEscapeKind);
                SET_GPR_U64(ctx, 7, 1u);
                runtime->lookupFunction(kResult)(rdram, ctx, runtime);
                return;
            }
            finish(rdram, ctx, fighter, 0u);
            return;
        }
        if (out.landed && out.outside)
        {
            say(rdram, fighter, cl, "lands outside the cage", before.phase, before.hold, before.out, before.along, before.y);
            place(rdram, fighter, out);
            writeFloat(rdram, fighter + 0x148u, out.facing);
            Ps2FastWrite16(rdram, fighter + kPlace, static_cast<uint16_t>(
                                                        (Ps2FastRead16(rdram, fighter + kPlace) & ~(kInRing | kOnApron)) | kOnFloor));
            g_afterLanding[index] = 40;
            setState(rdram, fighter, kStanding);
            finish(rdram, ctx, fighter, 0u);
            return;
        }
        if (out.landed)
        {
            say(rdram, fighter, cl, "lands in the ring", before.phase, before.hold, before.out, before.along, before.y);
            // standing where the landing left the body, facing the way it ended up: the clip turns the body about 135
            // degrees back toward the ring but keys no facing bone (+0x130 stays 0), so the game would start the stance
            // (flags 0xE: facing = +0x148) still facing the cage; then its turn to face (FUN_00102b08) takes it round
            place(rdram, fighter, out);
            writeFloat(rdram, fighter + 0x148u, out.facing);
            g_afterLanding[index] = 40;
            setState(rdram, fighter, kStanding); // state 2 runs this picture and starts its own clip
            finish(rdram, ctx, fighter, 0u);
            return;
        }
        place(rdram, fighter, out);
        Ps2FastWrite16(rdram, fighter + kPlace,
                       static_cast<uint16_t>(Ps2FastRead16(rdram, fighter + kPlace) | kCantBeHit | kCantBeGrabbed));
        finish(rdram, ctx, fighter, static_cast<uint32_t>(out.clip), out.blend ? 2u : 0u);
    }

    // FUN_0013c1b8 (state 2's triangle at the ropes): in a cage match, triangle with the stick toward a rope climbs the cage
    void ropesCheck(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_runtime = runtime;
        const uint32_t fighter = getRegU32(ctx, 4);
        const int index = indexOf(fighter);
        const uint32_t wall = Ps2FastRead32(rdram, fighter + kTouched);
        if (g_active && index >= 0 && wall && vendetta_animations_cageClip(CAGE_CLIP_ONTO) != 0u &&
            (Ps2FastRead32(rdram, fighter + kPressed) & kTriangle) &&
            (Ps2FastRead16(rdram, fighter + kPlace) & kInRing))
        {
            const uint16_t kind = Ps2FastRead16(rdram, wall + 0x42u);
            std::string why;
            if ((kind & 0x20u) && (Ps2FastRead32(rdram, fighter + kHeld) & kind & 0xFu) && !workOutHeights(rdram, index, why))
            {
                static bool told = false; // once a run: the climb needs the clips posed on the climber
                if (!told)
                    std::cerr << "[cage] climb: player " << index << " can't climb: " << why << std::endl;
                told = true;
            }
            else if ((kind & 0x20u) && (Ps2FastRead32(rdram, fighter + kHeld) & kind & 0xFu))
            {
                Climb &cl = g_climbs[index];
                cl = Climb();
                CagePictureOut out{};
                cage_rules_start(&num(), vendetta_ring::activeSides() == 6 ? 6 : 4, readFloat(rdram, fighter + kX),
                                 readFloat(rdram, fighter + kY), readFloat(rdram, fighter + kZ), &cl.c, &out);
                cl.toRope = Ps2FastRead32(rdram, fighter + kHeld) & 0xFu;
                setState(rdram, fighter, kClimbState); // state 102 runs this picture: it starts the clip
                sayStarted(rdram, fighter, cl, out);
                place(rdram, fighter, out);
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
    if (index >= 0 && g_dives[index].on)
    {
        const uint32_t state = Ps2FastRead32(rdram, fighter + kState);
        if (state < kDiveFirst || state > kDiveLast)
        {
            g_dives[index].on = false;
            if (logOn())
                std::cerr << "[cage] dive: player " << index << " lands (state " << state << ", anchor y "
                          << readFloat(rdram, fighter + kY) << ")" << std::endl;
        }
    }
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

bool vendetta_cage::skipRopeWindow(uint8_t *rdram, uint32_t fighter, uint32_t type)
{
    using namespace vendetta_cage_climb;
    const int index = indexOf(fighter);
    if (index < 0 || !g_dives[index].on || type == 6u)
        return false;
    const uint32_t state = Ps2FastRead32(rdram, fighter + kState);
    return state >= kDiveFirst && state <= kDiveLast && readFloat(rdram, fighter + kY) > 11.0f;
}

bool vendetta_cage::climbing(uint8_t *rdram, uint32_t fighter)
{
    using namespace vendetta_cage_climb;
    const int index = indexOf(fighter);
    return g_active && index >= 0 && g_climbs[index].c.phase != CAGE_PHASE_NONE &&
           Ps2FastRead32(rdram, fighter + kState) == kClimbState;
}

// each match's start (before the game's init): the climb's state routine in the table for a cage match, else the
// table's own entry back
void vendetta_cage::climbMatchStarting(uint8_t *rdram, PS2Runtime &runtime)
{
    using namespace vendetta_cage_climb;
    g_runtime = &runtime;
    for (Climb &cl : g_climbs)
        cl = Climb();
    for (Heights &h : g_heights) // the clips or cage.json may have changed: worked out again at the first climb
        h = Heights();
    for (Dive &d : g_dives)
        d = Dive();
    const uint32_t entry = kStateTable + 4u * kClimbState;
    if (!g_tableSet)
    {
        g_tableWas = Ps2FastRead32(rdram, entry);
        g_tableSet = true;
    }
    g_active = cageMatch(rdram);
    Ps2FastWrite32(rdram, entry, g_active ? kHookClimbState : g_tableWas);
    for (uint32_t s = kDiveFirst; s <= kDiveLast; ++s)
    {
        const uint32_t dive = kStateTable + 4u * s;
        if (!g_diveTableSet)
            g_diveTableWas[s - kDiveFirst] = Ps2FastRead32(rdram, dive);
        Ps2FastWrite32(rdram, dive, g_active ? kHookDiveState : g_diveTableWas[s - kDiveFirst]);
    }
    g_diveTableSet = true;
}

void installVendettaCageClimb(PS2Runtime &runtime)
{
    using namespace vendetta_cage_climb;
    g_runtime = &runtime;
    if (cage_rules_version() != CAGE_RULES_VERSION)
        std::cerr << "[cage] the climb's rules are version " << cage_rules_version() << ", this code version "
                  << CAGE_RULES_VERSION << std::endl;
    g_originalRopes = runtime.lookupFunction(kRopesCheck);
    if (!g_originalRopes)
    {
        std::cerr << "[cage] state 2's ropes check isn't where the original program has it: no climbing" << std::endl;
        return;
    }
    runtime.registerFunction(kHookClimbState, climbState);
    runtime.registerFunction(kHookDiveState, diveState);
    runtime.replaceFunction(kRopesCheck, ropesCheck);
}
