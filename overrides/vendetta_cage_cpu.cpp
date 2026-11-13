// Def Jam Vendetta (USA, SLUS-20639) - the cage match: the computer's wrestlers in it (climbing out, shaking a climber,
// never climbing by accident).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// The game's computer plays through a pad of its own: each picture its brain (0x22BDE8 + 0x3C0 x the fighter, B below)
// runs its current action B+0x0C through the table 0x27A928 with a0 = B, a1 = the fighter; the action writes the held
// and pressed buttons B+0x1C / B+0x20, which FUN_00107410 copies into the fighter's +0x668 / +0x66C before the state
// routines run. Every action is started by FUN_0010fcf0(B, fighter, n). The thinker (action 0, FUN_00109610) runs while
// no other action does, and acts only while the fighter's class B+0x44 is 1 or 0xA (the cage's state 102 is class 0xA).
//
// In a cage match:
// - FUN_0010fcf0 replaced: the actions that press triangle toward the ropes or try to leave the ring are not started
//   (57, backing into the ropes from a Blazin' opponent, becomes 3, the plain back away; 41, the apron move, and 54,
//   off the apron, become 4, wait; 45, after him out of the ring, becomes the shake when he is on the cage, else 4).
// - Action 16 (0x1079A0, an empty routine nothing starts) is the cage's own: CLIMB walks to a side and presses
//   triangle with the stick toward it; SHAKE walks to the climber's side and presses cross toward it; once on the cage,
//   shaking or waiting outside it presses nothing (the climb's own state makes a computer's inputs there).
// - The thinker wrapped: a computer in the cage's state is given action 16; with an opponent on the cage that can be
//   shaken, by a chance (the game's own answer to a turnbuckle climber: skill x 0.7 + 0.5, a third at level 0) it goes
//   to shake; with ESCAPE on, by a chance a second (much higher while every opponent is down and hurt, as No Mercy's
//   trigger), it goes to climb out; with ESCAPE off, by a chance a second while an opponent is down, it climbs the
//   side nearest that opponent to dive off the top onto it (on the truss the climb's state presses cross: the
//   wrestler's own top-rope dive; none fitting, it climbs back down); otherwise the game's thinker, as the routine's
//   last call.
// - The climb's state lets a computer onto the cage only from action 16's CLIMB (vendetta_cage::cpuMayClimb).
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "vendetta_cage.h"
#include "vendetta_cage_file.h"
#include "vendetta_ring.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>

namespace vendetta_cage_cpu
{
    constexpr uint32_t kStartAction = 0x10FCF0u; // FUN_0010fcf0(B, fighter, n): an action started (a leaf)
    constexpr uint32_t kThinker = 0x109610u;     // FUN_00109610(B, fighter): action 0, the thinker
    constexpr uint32_t kAction16 = 0x1079A0u;    // action 16: jr ra, never started by the game

    constexpr uint32_t kBrains = 0x22BDE8u, kBrainBytes = 0x3C0u;
    constexpr uint32_t kLevel = 0x00u, kSkill = 0x04u, kBaseSkill = 0x08u, kAction = 0x0Cu, kStep = 0x10u, kSpare = 0x14u,
                       kHeldOut = 0x1Cu, kPressedOut = 0x20u, kMyClass = 0x44u, kMyStateThen = 0x54u,
                       kHisStateThen = 0x84u, kFlags = 0xB6u, kTimer = 0xC4u;
    constexpr uint32_t kFighters = 0x22FE08u, kFighterBytes = 0xCD8u;
    constexpr uint32_t kRuleWord = 0x2361CCu, kMatchKind = 0x2361CEu, kMatchStep = 0x22FBD4u;
    constexpr uint32_t kMatchFlags = 0x22FDE4u, kDecided = 0x1000u;
    // the fighter's fields
    constexpr uint32_t kTarget = 0x0u, kPlayer = 0x30u, kState = 0x40u, kX = 0xCCu, kZ = 0xD4u, kHealth = 0x3A8u,
                       kFullHealth = 0x3ACu, kMomentum = 0x3B8u, kPlace = 0x5ACu, kOut = 0x5BBu, kPlay = 0x5BCu,
                       kBlazin = 0x5C3u;
    constexpr uint16_t kInRing = 4u;
    constexpr uint8_t kOutOfMatch = 0x10u, kInPlay = 2u, kTeamB = 4u, kComputer = 1u;
    constexpr uint32_t kStanding = 2u, kCageState = 102u;
    constexpr uint32_t kUp = 1u, kRight = 2u, kDown = 4u, kLeft = 8u, kTriangle = 0x1000u, kCross = 0x4000u;
    constexpr uint32_t kOwnAction = 16u;

    PS2Runtime::RecompiledFunction g_startAction = nullptr, g_thinker = nullptr;

    enum class Mode { None, Climb, Shake, Wait };
    struct Cpu
    {
        Mode mode = Mode::None;
        float nx = 0.0f, nz = 1.0f, along = 0.0f; // CLIMB: the side and the place along it
        float pictures = 0.0f;                   // in this action
        float sinceRoll = 0.0f;                  // the climb's clock: a roll a second
        float sinceShakeRoll = 0.0f;
        float sinceTried = 1e9f;                 // pictures since the last climb tried
        bool climbNext = false;                  // a passed climb roll, used at the next think
        bool beenOnCage = false;                 // this action has had the fighter in the cage's state: done once out
        uint32_t diveAt = 0;                     // ESCAPE off: the opponent down that the passed roll is for
    };
    Cpu g_cpu[4];
    float g_fight = 0.0f; // pictures of this cage match's fight
    std::mt19937 g_random{0xC4A6Eu};

    bool cageMatch(uint8_t *rdram) { return (Ps2FastRead16(rdram, kRuleWord) & vendetta_cage::kRuleCage) != 0u; }
    bool escapeOn(uint8_t *rdram) { return (Ps2FastRead16(rdram, kRuleWord) & vendetta_cage::kRuleEscape) != 0u; }
    uint32_t fighterAt(uint32_t i) { return kFighters + kFighterBytes * i; }
    int indexOf(uint32_t fighter)
    {
        const uint32_t i = (fighter - kFighters) / kFighterBytes;
        return (fighter >= kFighters && i < 4u) ? static_cast<int>(i) : -1;
    }
    float readFloat(uint8_t *rdram, uint32_t a) { float f; uint32_t v = Ps2FastRead32(rdram, a); std::memcpy(&f, &v, 4); return f; }
    bool roll(float chance) { return std::uniform_real_distribution<float>(0.0f, 1.0f)(g_random) < chance; }
    const vendetta_cage::CpuNumbers &num() { return vendetta_cage::cageFile().cpu; }

    // a fighter fighting in the ring: in the match, in play, not out, not waiting outside or leaving by the door
    bool fighting(uint8_t *rdram, uint32_t f)
    {
        return static_cast<int32_t>(Ps2FastRead32(rdram, f + kPlayer)) != -1 && (Ps2FastRead8(rdram, f + kPlay) & kInPlay) &&
               !(Ps2FastRead8(rdram, f + kOut) & kOutOfMatch) && !vendetta_cage::waitingOutside(rdram, f) &&
               !vendetta_cage::outByFall(rdram, f);
    }
    // the other side: another team in tag and handicap, anyone in one on one and all against all
    bool opponents(uint8_t *rdram, uint32_t a, uint32_t b)
    {
        if (a == b)
            return false;
        if (!(Ps2FastRead8(rdram, kMatchKind) & 0x32u))
            return true;
        return ((Ps2FastRead8(rdram, a + kPlay) ^ Ps2FastRead8(rdram, b + kPlay)) & kTeamB) != 0u;
    }

    // the stick's bits toward a direction in the world (up -z, right +x, down +z, left -x)
    uint32_t bitsToward(float dx, float dz)
    {
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len < 0.0001f)
            return 0u;
        dx /= len, dz /= len;
        return (dz < -0.38f ? kUp : 0u) | (dz > 0.38f ? kDown : 0u) | (dx < -0.38f ? kLeft : 0u) | (dx > 0.38f ? kRight : 0u);
    }

    // the ring's side nearest a place, leaving out a side an opponent is climbing (a race on another side): its outward
    // normal, and how far the ropes run each way from its middle
    void nearestSide(uint8_t *rdram, uint32_t fighter, float x, float z, float &nx, float &nz, float &half)
    {
        const int sides = vendetta_ring::activeSides() == 6 ? 6 : 4;
        float best = -1e9f;
        for (int k = 0; k < sides; ++k)
        {
            const float a = (sides == 6 ? 60.0f - 60.0f * k : 90.0f * k) * 3.14159265f / 180.0f;
            const float sx = std::sin(a), sz = std::cos(a);
            float d = x * sx + z * sz;
            for (uint32_t j = 0; j < 4u; ++j)
            {
                const uint32_t o = fighterAt(j);
                float onx = 0.0f, onz = 0.0f, along = 0.0f;
                if (opponents(rdram, fighter, o) && vendetta_cage::climbing(rdram, o) &&
                    vendetta_cage::climberSide(rdram, o, onx, onz, along) && onx * sx + onz * sz > 0.99f)
                    d -= 1000.0f;
            }
            if (d > best)
                best = d, nx = sx, nz = sz;
        }
        half = sides == 6 ? 35.0f * 0.57735f : 35.0f;
    }

    // the action's start, as FUN_0010fcf0 writes it (our own writes: no game call)
    void startAction(uint8_t *rdram, uint32_t brain, uint32_t fighter, uint32_t n)
    {
        const uint32_t target = Ps2FastRead32(rdram, fighter + kTarget);
        Ps2FastWrite32(rdram, brain + kAction, n);
        Ps2FastWrite32(rdram, brain + kStep, 0u);
        Ps2FastWrite32(rdram, brain + kSpare, 0u);
        Ps2FastWrite32(rdram, brain + kMyStateThen, Ps2FastRead32(rdram, fighter + kState));
        Ps2FastWrite32(rdram, brain + kHisStateThen, target ? Ps2FastRead32(rdram, target + kState) : 0u);
        Ps2FastWrite16(rdram, brain + kFlags, static_cast<uint16_t>((Ps2FastRead16(rdram, brain + kFlags) & 0xFFECu) | 1u));
    }
    void startOwn(uint8_t *rdram, uint32_t brain, uint32_t fighter, int index, Mode mode, const char *why)
    {
        Cpu &c = g_cpu[index];
        c.mode = mode;
        c.pictures = 0.0f;
        c.beenOnCage = Ps2FastRead32(rdram, fighter + kState) == kCageState;
        startAction(rdram, brain, fighter, kOwnAction);
        if (mode == Mode::Climb || mode == Mode::Shake)
            std::cerr << "[cage] cpu: player " << index
                      << (mode == Mode::Shake ? " goes to shake" : escapeOn(rdram) ? " goes to climb out" : " goes to climb to dive")
                      << " (" << why << ")" << std::endl;
    }

    // momentum for every step to the top hold, or Blazin'
    bool enoughMomentum(uint8_t *rdram, uint32_t fighter)
    {
        const auto &numbers = vendetta_cage::cageFile().numbers;
        const float need = numbers.stepCost * static_cast<float>(numbers.steps);
        return readFloat(rdram, fighter + kMomentum) >= need || (Ps2FastRead8(rdram, fighter + kBlazin) & 2u);
    }

    // the opponent nearest this fighter that is on the cage and can be shaken
    uint32_t climberToShake(uint8_t *rdram, uint32_t fighter)
    {
        uint32_t best = 0;
        float bestD = 1e30f;
        const float x = readFloat(rdram, fighter + kX), z = readFloat(rdram, fighter + kZ);
        for (uint32_t j = 0; j < 4u; ++j)
        {
            const uint32_t o = fighterAt(j);
            float nx = 0.0f, nz = 0.0f, along = 0.0f;
            if (!opponents(rdram, fighter, o) || static_cast<int32_t>(Ps2FastRead32(rdram, o + kPlayer)) == -1 ||
                !vendetta_cage::climberToShake(rdram, o, nx, nz, along))
                continue;
            const float dx = readFloat(rdram, o + kX) - x, dz = readFloat(rdram, o + kZ) - z;
            if (dx * dx + dz * dz < bestD)
                bestD = dx * dx + dz * dz, best = o;
        }
        return best;
    }

    // ---- FUN_0010fcf0: in a cage match, nothing that presses triangle toward the ropes or leaves the ring
    void startHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (cageMatch(rdram))
        {
            const uint32_t brain = getRegU32(ctx, 4), fighter = getRegU32(ctx, 5), n = getRegU32(ctx, 6);
            const int index = indexOf(fighter);
            uint32_t to = n;
            if (n == 57u)
                to = 3u;
            else if (n == 41u || n == 54u || ((n == 23u || n == 24u) && (Ps2FastRead16(rdram, fighter + kPlace) & kInRing)))
                to = 4u;
            else if (n == 45u)
            {
                const uint32_t target = Ps2FastRead32(rdram, fighter + kTarget);
                float nx = 0.0f, nz = 0.0f, along = 0.0f;
                if (index >= 0 && target && vendetta_cage::climberToShake(rdram, target, nx, nz, along))
                {
                    startOwn(rdram, brain, fighter, index, Mode::Shake, "after him onto the cage: the shake");
                    ctx->pc = getRegU32(ctx, 31);
                    return;
                }
                to = 4u;
            }
            SET_GPR_U64(ctx, 6, to);
        }
        g_startAction(rdram, ctx, runtime);
    }

    // ---- action 16, the cage's own
    void ownAction(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t brain = getRegU32(ctx, 4), fighter = getRegU32(ctx, 5);
        const int index = indexOf(fighter);
        ctx->pc = getRegU32(ctx, 31);
        if (!cageMatch(rdram) || index < 0)
            return;
        Cpu &c = g_cpu[index];
        const float step = readFloat(rdram, kMatchStep);
        c.pictures += step;
        const uint32_t state = Ps2FastRead32(rdram, fighter + kState);
        const float x = readFloat(rdram, fighter + kX), z = readFloat(rdram, fighter + kZ);
        uint32_t held = 0, pressed = 0;
        auto end = [&](const char *why) {
            if (why && (c.mode == Mode::Climb || c.mode == Mode::Shake))
                std::cerr << "[cage] cpu: player " << index << " stops: " << why << std::endl;
            c.mode = Mode::None;
            startAction(rdram, brain, fighter, 0u);
        };
        if (state == kCageState)
        {
            // on the cage, shaking or waiting outside: the cage's state makes a computer's inputs; until it is done
            c.beenOnCage = true;
            Ps2FastWrite32(rdram, brain + kHeldOut, 0u);
            Ps2FastWrite32(rdram, brain + kPressedOut, 0u);
            return;
        }
        if (c.beenOnCage)
            return end(nullptr); // the climb or the shake is over (down, shaken off, let go, the bar full)
        if ((Ps2FastRead16(rdram, kMatchFlags) & kDecided) || c.mode == Mode::None || c.mode == Mode::Wait ||
            state != kStanding || !(Ps2FastRead16(rdram, fighter + kPlace) & kInRing))
        {
            end(state != kStanding ? nullptr : "the match is decided or nothing to do");
            return;
        }
        float tx = 0.0f, tz = 0.0f, nx = c.nx, nz = c.nz;
        if (c.mode == Mode::Climb)
        {
            if (c.pictures > num().climbGiveUp)
                return end("couldn't reach the ropes in time");
            if (!enoughMomentum(rdram, fighter))
                return end("not enough momentum left for the steps");
            tx = nx * 37.0f + nz * c.along; // into the ropes, at the place along the side
            tz = nz * 37.0f - nx * c.along;
        }
        else // Shake
        {
            const uint32_t target = Ps2FastRead32(rdram, fighter + kTarget);
            float along = 0.0f;
            if (!target || !vendetta_cage::climberToShake(rdram, target, nx, nz, along))
                return end("nobody to shake");
            if (c.pictures > num().shakeGiveUp)
                return end("couldn't reach the climber's side in time");
            along += along > 0.0f ? -12.0f : 12.0f; // beside the climber, toward the side's middle
            tx = nx * 37.0f + nz * along;
            tz = nz * 37.0f - nx * along;
        }
        const float out = x * nx + z * nz;
        if (out >= 30.0f)
        {
            // at the ropes: the stick toward the cage, the button pressed now and then (a press is one picture)
            held = bitsToward(nx, nz);
            if (std::fmod(c.pictures, 8.0f) < step)
                pressed = c.mode == Mode::Climb ? kTriangle : kCross;
            // along the side, still far from the place: keep walking to it
            const float alongNow = x * nz - z * nx, target = c.mode == Mode::Climb ? c.along : alongNow;
            if (std::fabs(alongNow - target) > 6.0f)
                held = bitsToward(tx - x, tz - z), pressed = 0u;
        }
        else
            held = bitsToward(tx - x, tz - z);
        Ps2FastWrite32(rdram, brain + kHeldOut, held);
        Ps2FastWrite32(rdram, brain + kPressedOut, pressed);
        static const bool log = []() { const char *v = std::getenv("VENDETTA_CAGECLIMBLOG"); return v && v[0] >= '1'; }();
        if (log && std::fmod(c.pictures, 60.0f) < step)
            std::cerr << "[cage] cpu: player " << index << (c.mode == Mode::Climb ? " walking to climb" : " walking to shake")
                      << " at " << x << " " << z << " (out " << out << "), held 0x" << std::hex << held << " pressed 0x"
                      << pressed << std::dec << ", touched wall " << (Ps2FastRead32(rdram, fighter + 0x7A0u) ? "yes" : "no")
                      << std::endl;
    }

    // the climb's chance this second for a computer standing in the ring; 0 = not now. ESCAPE on: the climb out;
    // ESCAPE off: the climb to dive, only while an opponent is down (diveAt: the nearest one down)
    float climbChance(uint8_t *rdram, uint32_t fighter, uint32_t brain, const char *&why, uint32_t &diveAt)
    {
        const bool escape = escapeOn(rdram);
        diveAt = 0;
        float nearestDown = 1e30f;
        const vendetta_cage::CpuNumbers &n = num();
        if (!enoughMomentum(rdram, fighter))
            return 0.0f;
        bool anyOpponent = false, allDown = true, mateIn = false, mateGone = false;
        const float x = readFloat(rdram, fighter + kX), z = readFloat(rdram, fighter + kZ);
        for (uint32_t j = 0; j < 4u; ++j)
        {
            const uint32_t o = fighterAt(j);
            if (o == fighter || static_cast<int32_t>(Ps2FastRead32(rdram, o + kPlayer)) == -1)
                continue;
            if (!opponents(rdram, fighter, o))
            {
                if (fighting(rdram, o) && !vendetta_cage::climbing(rdram, o))
                    mateIn = true;
                else
                    mateGone = true;
                continue;
            }
            if (!fighting(rdram, o))
                continue;
            anyOpponent = true;
            const uint32_t s = Ps2FastRead32(rdram, o + kState);
            const bool down = s == 21u || s == 22u || s == 23u || s == 29u;
            const float health = readFloat(rdram, o + kHealth), full = std::max(readFloat(rdram, o + kFullHealth), 1.0f);
            if (!down || health / full > n.downHurt)
                allDown = false;
            const float dx = readFloat(rdram, o + kX) - x, dz = readFloat(rdram, o + kZ) - z;
            if (down && dx * dx + dz * dz < nearestDown)
                nearestDown = dx * dx + dz * dz, diveAt = o;
            if (!down && dx * dx + dz * dz < 15.0f * 15.0f)
                return 0.0f; // someone standing right by me
        }
        if (!anyOpponent || (!escape && !diveAt))
            return 0.0f;
        const float skill = std::clamp(readFloat(rdram, brain + kSkill), 0.0f, 1.0f);
        // VENDETTA_CAGETEST_CLIMB=<chance a second> (tests): every computer climbs that often, whatever else
        static const float testChance = []() { const char *v = std::getenv("VENDETTA_CAGETEST_CLIMB"); return v && *v ? static_cast<float>(std::atof(v)) : -1.0f; }();
        if (testChance >= 0.0f)
        {
            why = "the test's chance";
            return testChance;
        }
        if (!escape)
        {
            if (readFloat(rdram, fighter + kMomentum) < n.diveMomentum && !(Ps2FastRead8(rdram, fighter + kBlazin) & 2u))
                return 0.0f;
            why = "an opponent down: the dive";
            return std::min(n.dive * (0.5f + skill), 1.0f);
        }
        float p = allDown ? n.climbDown : std::min(n.climb + n.climbMore * g_fight / 3600.0f, n.climbMost);
        why = allDown ? "every opponent down and hurt" : "a chance";
        p *= 0.5f + skill;
        if (mateIn)
            p *= 0.5f;
        if (mateGone)
            p *= 3.0f;
        return std::min(p, 1.0f);
    }

    // ---- the thinker (action 0), in a cage match
    void thinkerHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t brain = getRegU32(ctx, 4), fighter = getRegU32(ctx, 5);
        const int index = indexOf(fighter);
        if (!cageMatch(rdram) || index < 0 || (Ps2FastRead16(rdram, kMatchFlags) & kDecided))
        {
            g_thinker(rdram, ctx, runtime);
            return;
        }
        Cpu &c = g_cpu[index];
        const uint32_t state = Ps2FastRead32(rdram, fighter + kState);
        // in the cage's state (on the cage, shaking, waiting outside): action 16 holds the pad still
        if (state == kCageState)
        {
            startOwn(rdram, brain, fighter, index, vendetta_cage::climbing(rdram, fighter) ? Mode::Climb : Mode::Wait, "");
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
        const bool standingIn = state == kStanding && (Ps2FastRead16(rdram, fighter + kPlace) & kInRing) &&
                                fighting(rdram, fighter) && Ps2FastRead32(rdram, brain + kMyClass) == 1u;
        if (standingIn)
        {
            const float skill = std::clamp(readFloat(rdram, brain + kSkill), 0.0f, 1.0f);
            const uint32_t level = Ps2FastRead32(rdram, brain + kLevel);
            // an opponent on the cage: the shake, by a roll each second (at once after coming down to shake)
            const bool cameDown = vendetta_cage::cpuDroppedToShake(rdram, fighter);
            // VENDETTA_CAGETEST_NOSHAKE=1 (tests): no shake by chance (coming down to shake still does)
            static const bool noShake = []() { const char *v = std::getenv("VENDETTA_CAGETEST_NOSHAKE"); return v && v[0] == '1'; }();
            if (const uint32_t climber = climberToShake(rdram, fighter);
                climber && (cameDown || (!noShake && c.sinceShakeRoll >= 60.0f)))
            {
                c.sinceShakeRoll = 0.0f;
                float p = (skill * 0.7f + 0.5f) * num().shake;
                if (level == 0u)
                    p *= 0.35f;
                if (cameDown || roll(p))
                {
                    Ps2FastWrite32(rdram, fighter + kTarget, climber);
                    startOwn(rdram, brain, fighter, index, Mode::Shake, "an opponent on the cage");
                    ctx->pc = getRegU32(ctx, 31);
                    return;
                }
            }
            // the climb: ESCAPE on, out, from the side nearest; ESCAPE off, to dive, from the side nearest the
            // opponent down (still fighting), at its place along the side, that opponent made the target
            const bool toDive = !escapeOn(rdram);
            if (c.climbNext && ((toDive && !(c.diveAt && fighting(rdram, c.diveAt) && opponents(rdram, fighter, c.diveAt))) ||
                                !enoughMomentum(rdram, fighter)))
                c.climbNext = false;
            if (c.climbNext)
            {
                c.climbNext = false;
                c.sinceTried = 0.0f;
                float nx = 0.0f, nz = 1.0f, half = 35.0f;
                float x = readFloat(rdram, fighter + kX), z = readFloat(rdram, fighter + kZ);
                if (toDive)
                {
                    x = readFloat(rdram, c.diveAt + kX), z = readFloat(rdram, c.diveAt + kZ);
                    Ps2FastWrite32(rdram, fighter + kTarget, c.diveAt);
                }
                nearestSide(rdram, fighter, x, z, nx, nz, half);
                c.nx = nx, c.nz = nz;
                c.along = std::clamp(x * nz - z * nx, -(half - 9.0f), half - 9.0f);
                startOwn(rdram, brain, fighter, index, Mode::Climb, toDive ? "an opponent down" : "its chance came");
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
        }
        g_thinker(rdram, ctx, runtime);
    }

    void install(PS2Runtime &runtime)
    {
        g_startAction = runtime.lookupFunction(kStartAction);
        g_thinker = runtime.lookupFunction(kThinker);
        if (!g_startAction || !g_thinker || !runtime.lookupFunction(kAction16))
        {
            std::cerr << "[cage] the computer's routines weren't found: the computer plays a cage match as a normal one"
                      << std::endl;
            return;
        }
        runtime.replaceFunction(kStartAction, startHook);
        runtime.replaceFunction(kThinker, thinkerHook);
        runtime.replaceFunction(kAction16, ownAction);
    }
}

void vendetta_cage::cpuMatchStarting()
{
    using namespace vendetta_cage_cpu;
    for (Cpu &c : g_cpu)
        c = Cpu();
    g_fight = 0.0f;
}

// once a picture (from the leave's per-picture step): the climb's clock for each computer standing in the ring
void vendetta_cage::cpuEveryPicture(uint8_t *rdram)
{
    using namespace vendetta_cage_cpu;
    if (!cageMatch(rdram) || (Ps2FastRead16(rdram, kMatchFlags) & kDecided))
        return;
    const float step = readFloat(rdram, kMatchStep);
    g_fight += step;
    for (uint32_t i = 0; i < 4u; ++i)
    {
        Cpu &c = g_cpu[i];
        c.sinceShakeRoll += step;
        c.sinceTried += step;
        const uint32_t f = fighterAt(i), brain = kBrains + kBrainBytes * i;
        if (c.mode != Mode::None && Ps2FastRead32(rdram, brain + kAction) != kOwnAction)
        {
            if (c.mode == Mode::Climb || c.mode == Mode::Shake)
                std::cerr << "[cage] cpu: player " << i << " stops: the game started its action "
                          << Ps2FastRead32(rdram, brain + kAction) << " (state " << Ps2FastRead32(rdram, f + kState) << ")"
                          << std::endl;
            c.mode = Mode::None; // the game started another action (knocked down, hit): ours is over
        }
        if (!(Ps2FastRead8(rdram, f + kPlay) & kComputer) || c.mode != Mode::None ||
            Ps2FastRead32(rdram, f + kState) != kStanding || !fighting(rdram, f) || c.sinceTried < num().climbAgain)
            continue;
        c.sinceRoll += step;
        if (c.sinceRoll < 60.0f)
            continue;
        c.sinceRoll = 0.0f;
        const char *why = "";
        uint32_t diveAt = 0;
        const float p = climbChance(rdram, f, brain, why, diveAt);
        if (p > 0.0f && roll(p))
            c.climbNext = true, c.diveAt = diveAt;
    }
}

bool vendetta_cage::cpuMayClimb(uint8_t *rdram, uint32_t fighter)
{
    using namespace vendetta_cage_cpu;
    const int index = indexOf(fighter);
    if (index < 0 || !(Ps2FastRead8(rdram, fighter + kPlay) & kComputer))
        return true; // a player
    const uint32_t brain = kBrains + kBrainBytes * static_cast<uint32_t>(index);
    return Ps2FastRead32(rdram, brain + kAction) == kOwnAction && g_cpu[index].mode == Mode::Climb;
}

// a computer's taps, at the pace the game's own computer kicks out of a pin (action 25): a press every
// floor(1 / rate) + 1 pictures, rate = base skill x 0.1166667 + 0.2166667 (+ 0.05 at level 1)
bool vendetta_cage::cpuTap(uint8_t *rdram, uint32_t fighter, int32_t pictures)
{
    using namespace vendetta_cage_cpu;
    const int index = indexOf(fighter);
    if (index < 0)
        return pictures % 6 == 0;
    const uint32_t brain = kBrains + kBrainBytes * static_cast<uint32_t>(index);
    float rate = std::clamp(readFloat(rdram, brain + kBaseSkill), 0.0f, 1.0f) * 0.1166667f + 0.2166667f;
    if (Ps2FastRead32(rdram, brain + kLevel) == 1u)
        rate += 0.05f;
    const int32_t every = static_cast<int32_t>(std::floor(1.0f / rate)) + 1;
    return pictures % std::max(every, 1) == 0;
}

// a computer resting on the truss: how long it waits before its button, by its skill
float vendetta_cage::cpuRestPictures(uint8_t *rdram, uint32_t fighter)
{
    using namespace vendetta_cage_cpu;
    const int index = indexOf(fighter);
    if (index < 0)
        return 30.0f;
    const float skill = std::clamp(readFloat(rdram, kBrains + kBrainBytes * static_cast<uint32_t>(index) + kSkill), 0.0f, 1.0f);
    return 60.0f - 40.0f * skill;
}

void installVendettaCageCpu(PS2Runtime &runtime) { vendetta_cage_cpu::install(runtime); }
