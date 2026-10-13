// Def Jam Vendetta (USA, SLUS-20639) - the match's speed (VENDETTA_MATCH_SPEED=<factor>, 0.75-1.25; settings.toml
// gameplay.match_speed / custom_speed / custom_match_speed: "vendetta" = 1.0, "nomercy" = 0.862, WWF No Mercy's pace).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// The fight runs on a float time step, not on whole frames: 0x22FBD4 = this picture's step, read by 114 routines (the
// fighters' states and clips, blends, the screen shake, the camera, the round clock FUN_00135660). FUN_0011a600, the
// match's logic once a picture, works it out from the base step 0x22FBD0 (1.0, set by FUN_00119d70 at each match /
// round start): 0 when paused (flags 0x22FDE4 & 2), x 0.45 (0x2948F8) for the game's own slow motion at the end. Another
// base slows (or quickens) the whole fight evenly at 60 pictures a second, by the game's own maths.
//
// Only while the fight is on: the match's phase 0x22FBE4 is 0 for the intros, 1 to 3 for the fight (3 ends only when
// the whole match is decided: a fighter going down in a 3- or 4-fighter match is reported and the fight goes on), 4 and
// up for the end. Elsewhere the base stays 1.0. Unset or 1: the game's own speed, nothing is hooked.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <bit>
#include <cstdlib>
#include <iostream>

namespace vendetta_matchspeed
{
    constexpr uint32_t kMatchLogic = 0x11A600u; // FUN_0011a600: the match's logic, once a picture
    constexpr uint32_t kBaseStep = 0x22FBD0u;   // float, 1.0
    constexpr uint32_t kPhase = 0x22FBE4u;      // 0 intros, 1-3 the fight, 4+ the end
    constexpr float kMinSpeed = 0.75f, kMaxSpeed = 1.25f;

    PS2Runtime::RecompiledFunction g_originalMatchLogic = nullptr;
    float g_speed = 1.0f;
    bool g_fighting = false;

    bool fighting(uint8_t *rdram)
    {
        const uint32_t phase = Ps2FastRead32(rdram, kPhase);
        return phase >= 1u && phase <= 3u;
    }

    void setBase(uint8_t *rdram, float base) { Ps2FastWrite32(rdram, kBaseStep, std::bit_cast<uint32_t>(base)); }

    void matchLogic(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (fighting(rdram))
        {
            setBase(rdram, g_speed);
            if (!g_fighting)
            {
                g_fighting = true;
                std::cerr << "[matchspeed] fight on (phase " << Ps2FastRead32(rdram, kPhase) << "): " << g_speed << "x"
                          << std::endl;
            }
        }
        g_originalMatchLogic(rdram, ctx, runtime);
        if (!fighting(rdram))
        {
            setBase(rdram, 1.0f);
            if (g_fighting)
            {
                g_fighting = false;
                std::cerr << "[matchspeed] fight over (phase " << Ps2FastRead32(rdram, kPhase) << "): 1x" << std::endl;
            }
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *value = std::getenv("VENDETTA_MATCH_SPEED");
        if (!value || !*value)
            return;
        const float speed = static_cast<float>(std::atof(value));
        if (!(speed >= kMinSpeed && speed <= kMaxSpeed))
        {
            std::cerr << "[matchspeed] " << value << " is not a speed from 0.75 to 1.25: the game's own speed" << std::endl;
            return;
        }
        if (speed == 1.0f)
            return;
        g_speed = speed;
        g_originalMatchLogic = runtime.lookupFunction(kMatchLogic);
        if (!g_originalMatchLogic)
        {
            std::cerr << "[matchspeed] FUN_0011a600 not found: off" << std::endl;
            return;
        }
        runtime.replaceFunction(kMatchLogic, matchLogic);
        std::cerr << "[matchspeed] on: the fight at " << g_speed << "x" << std::endl;
    }
}

void installVendettaMatchSpeed(PS2Runtime &runtime) { vendetta_matchspeed::install(runtime); }
