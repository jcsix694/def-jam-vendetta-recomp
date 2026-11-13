// Def Jam Vendetta (USA, SLUS-20639) - the cage match: a tag match's intros as the handicap pair's.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// FUN_001245c8 builds the intro scenes at 0x29AF48 (0x84 bytes each: four entries of 0x20 {player, place x y z, angles
// x y z, clip}, the camera path at + 0x80, the flags at + 0x84, the next scene's address at + 0x88) by the match kind;
// for tag FUN_00124748: each team a tag pair (FUN_00124cc0: clips 0x2A10 + 2k, camera paths 16-21), team A (fighters
// 0, 1) at spot 2 (facing 180), team B (2, 3) at spot 0. A handicap match's pair (FUN_00124fd8) has its own: clips
// 0x2A20 + 2k and + 1, k a random 0-2, facing 0, camera path 0x1C + k (28-30), made for spot 0's end only.
//
// In a tag cage match FUN_00124748 is replaced: both teams get a handicap pair's intro (different ones), team B as the
// handicap pair has it, team A turned half round (facing 180: its clips carry it to the far end) with its camera path
// turned half round about the ring's middle when its scene starts (vendetta_ring.cpp's scene start, through
// vendetta_cage::introTurnedHalf; put back after, as it puts back a path it moves).
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "vendetta_cage.h"

#include <cstring>
#include <iostream>
#include <random>

namespace vendetta_cage_intros
{
    constexpr uint32_t kTagIntros = 0x124748u;   // FUN_00124748(skippable): a tag match's two scenes
    constexpr uint32_t kScenes = 0x29AF48u, kSecondScene = 0x29AFD4u, kSceneFlags = 0x84u, kNextScene = 0x88u;
    constexpr uint32_t kFighters = 0x22FE08u, kFighterBytes = 0xCD8u, kPlayer = 0x30u;
    constexpr uint32_t kRuleWord = 0x2361CCu;
    constexpr uint32_t kHumans = 0x22FDE0u;      // a pointer: + 4 a u16 of which fighters a person plays (bit per fighter)
    constexpr uint32_t kPairClips = 0x2A20u, kPairCameras = 0x1Cu;

    PS2Runtime::RecompiledFunction g_tagIntros = nullptr;
    bool g_turned = false; // this match's first scene is a handicap pair's turned half round
    std::mt19937 g_random{std::random_device{}()};

    uint32_t fighterAt(uint32_t i) { return kFighters + kFighterBytes * i; }
    void writeFloat(uint8_t *rdram, uint32_t a, float f) { uint32_t v; std::memcpy(&v, &f, 4); Ps2FastWrite32(rdram, a, v); }

    // a handicap pair's scene as FUN_00124fd8 writes it, its pair k, facing as asked
    void pairScene(uint8_t *rdram, uint32_t scene, uint32_t a, uint32_t b, uint32_t k, float facing)
    {
        for (uint32_t i = 0; i < 2u; ++i)
        {
            const uint32_t entry = scene + 0x20u * i;
            Ps2FastWrite32(rdram, entry, Ps2FastRead32(rdram, (i == 0u ? a : b) + kPlayer));
            for (uint32_t w = 1; w < 7u; ++w)
                Ps2FastWrite32(rdram, entry + 4u * w, 0u);
            writeFloat(rdram, entry + 0x14u, facing);
            Ps2FastWrite32(rdram, entry + 0x1Cu, kPairClips + 2u * k + i);
        }
        Ps2FastWrite32(rdram, scene + 0x40u, 0xFFFFFFFFu); // no third and fourth wrestler
        Ps2FastWrite32(rdram, scene + 0x60u, 0xFFFFFFFFu);
        Ps2FastWrite32(rdram, scene + 0x80u, kPairCameras + k);
        Ps2FastWrite16(rdram, scene + kSceneFlags, 0x10u);
    }

    void orFlags(uint8_t *rdram, uint32_t at, uint16_t bits) { Ps2FastWrite16(rdram, at, static_cast<uint16_t>(Ps2FastRead16(rdram, at) | bits)); }

    // ---- FUN_00124748: in a cage match each team a handicap pair's intro
    void tagIntros(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!(Ps2FastRead16(rdram, kRuleWord) & vendetta_cage::kRuleCage))
        {
            g_turned = false;
            g_tagIntros(rdram, ctx, runtime);
            return;
        }
        const uint32_t skippable = getRegU32(ctx, 4);
        const uint32_t kA = std::uniform_int_distribution<uint32_t>(0u, 2u)(g_random);
        uint32_t kB = std::uniform_int_distribution<uint32_t>(0u, 1u)(g_random);
        if (kB >= kA)
            ++kB; // the other team's a different one
        const uint32_t humans = Ps2FastRead32(rdram, kHumans);
        const uint16_t people = humans ? Ps2FastRead16(rdram, humans + 4u) : 0u;
        pairScene(rdram, kScenes, fighterAt(0), fighterAt(1), kA, 180.0f);
        if (skippable)
            orFlags(rdram, kScenes + kSceneFlags, 4u);
        if (people & 3u)
            orFlags(rdram, kScenes + kSceneFlags, 0x20u);
        Ps2FastWrite32(rdram, kScenes + kNextScene, kSecondScene);
        pairScene(rdram, kSecondScene, fighterAt(2), fighterAt(3), kB, 0.0f);
        if (people & 0xCu)
            orFlags(rdram, kSecondScene + kSceneFlags, 0x20u);
        g_turned = true;
        std::cerr << "[cage] tag intros: each team a handicap pair's (team A " << kA << ", turned half round; team B " << kB << ")"
                  << std::endl;
        ctx->pc = getRegU32(ctx, 31);
    }

    void install(PS2Runtime &runtime)
    {
        g_tagIntros = runtime.lookupFunction(kTagIntros);
        if (!g_tagIntros)
        {
            std::cerr << "[cage] the tag intros' builder wasn't found: a tag cage match has the tag intros" << std::endl;
            return;
        }
        runtime.replaceFunction(kTagIntros, tagIntros);
    }
}

bool vendetta_cage::introTurnedHalf(uint32_t scene)
{
    return vendetta_cage_intros::g_turned && scene == vendetta_cage_intros::kScenes;
}

bool vendetta_cage::introOneMove(uint32_t scene)
{
    return vendetta_cage_intros::g_turned &&
           (scene == vendetta_cage_intros::kScenes || scene == vendetta_cage_intros::kSecondScene);
}

void installVendettaCageIntros(PS2Runtime &runtime) { vendetta_cage_intros::install(runtime); }
