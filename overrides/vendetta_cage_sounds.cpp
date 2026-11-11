// Def Jam Vendetta (USA, SLUS-20639) - the cage match: the cage's own recordings ("custom sounds" in cage.json), played on
// a frame of a cage clip the way the game's own sounds are.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// cage.json's "custom sounds" names each recording ({ "<NAME>": { "file": "sounds/<NAME>.wav", "volume": 1 } }); a
// clip's "sounds" list names one by "sound" (with "frame" and "first time only" as for the game's own). When cage.json
// is read (each cage match's load) each recording is read (any WAV the voice lines read: made mono, then 48 kHz) and
// numbered; a clip's entry naming one becomes the event 0x1000D with that number (vendetta_clipsounds.h kCustomSound),
// so the game's own sound machinery decides when it fires: the frame, each time round a loop, "first time only", the
// clip cut short. The event dispatcher FUN_001c3e18(player, event, play) leaves 0x1000D unhandled, and its caller
// FUN_001c44a0 then plays an unhandled event as one of the game's sounds by its id: so 0x1000D is answered here
// (handled, $v0 = 1), never passed on; with play = 1 the recording starts, with 0 (a clip changed before the frame) not.
// The recordings are mixed into the game's output (vendetta_mixer.h), several at once, each:
// - as loud as its volume times the game's own sound-effects level: the options' effects byte (0x240F84, 0 to 32) set
//   through FUN_001bfb10 ("SetMasterSfxVolume") into the effects group 0x1E31540 (+8 its level) from the table
//   0x1E31650 (33 levels, VOLUMES.TXT's GMASTERVOL: 0 to 31,774 in a straight line); the level now over the table's top,
//   so a recording plays as recorded at full effects (the default) and quieter in step with the game's own effects;
// - placed across the screen where its wrestler is (the hips, body point 1, projected with the last picture's camera
//   as the cage is: the screen's middle 2048, its half-width 256): full left at the left edge, both sides in the middle.
// Both taken when it starts. The pause as the game's own sounds: FUN_001c0650(1) (the pause) fades the game's sound
// levels to nothing in 250 ms and FUN_001c0650(0) (the resume) back in 250 ms, the sounds playing on unheard (the song
// isn't stopped either); so ours fade out and back the same, playing on.
// VENDETTA_CAGESOUNDLOG=1: each recording read, each one started (its level and place), the pause's fades.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "vendetta_cage.h"
#include "vendetta_cage_file.h"
#include "vendetta_clipsounds.h"
#include "vendetta_mixer.h"
#include "vendetta_voicelines.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vendetta_cage_sounds
{
    constexpr uint32_t kDispatch = 0x1C3E18u; // FUN_001c3e18(player, event, play): a clip event's command or "a sound"
    constexpr uint32_t kPauseFade = 0x1C0650u; // FUN_001c0650(1 the pause, 0 the resume): the game's sound faded out / in
    constexpr uint32_t kRate = 48000u, kMostSeconds = 20u;
    constexpr uint32_t kEffectsGroup = 0x1E31540u; // + 8 the effects' level now
    constexpr uint32_t kEffectsTable = 0x1E31650u; // its 33 levels; the last the loudest
    constexpr uint32_t kFighters = 0x22FE08u, kFighterBytes = 0xCD8u, kHips = 0x15Cu;

    struct Recording
    {
        std::string name;
        std::vector<int16_t> pcm; // mono, 48 kHz
        float volume = 1.0f;
    };
    struct Playing
    {
        std::shared_ptr<const Recording> sound;
        size_t at = 0;
        float left = 1.0f, right = 1.0f; // its volume, the effects level and its place
    };

    std::mutex g_lock; // the recordings and the playing ones (the mixer only tries it)
    std::vector<std::shared_ptr<const Recording>> g_recordings;
    std::vector<Playing> g_playing;
    std::map<std::string, uint32_t> g_ids; // by name: the recordings read (the game thread's)
    PS2Runtime::RecompiledFunction g_original = nullptr, g_originalPause = nullptr;
    std::atomic<float> g_pauseTarget{1.0f}; // 0 while the game is paused
    float g_pauseGain = 1.0f;               // the mixer's: toward the target, 250 ms the whole way
    constexpr float kPauseStep = 1.0f / (0.25f * kRate);

    bool logOn()
    {
        static const bool on = [] { const char *v = std::getenv("VENDETTA_CAGESOUNDLOG"); return v && *v && *v != '0'; }();
        return on;
    }

    // to 48 kHz, a straight line between samples
    std::vector<int16_t> to48k(const std::vector<int16_t> &in, uint32_t rate)
    {
        if (rate == kRate || in.empty())
            return in;
        const double step = static_cast<double>(rate) / kRate;
        const size_t count = static_cast<size_t>(std::floor((in.size() - 1) / step)) + 1;
        std::vector<int16_t> out(count);
        for (size_t i = 0; i < count; ++i)
        {
            const double at = i * step;
            const size_t a = static_cast<size_t>(at), b = std::min(a + 1, in.size() - 1);
            const double t = at - static_cast<double>(a);
            out[i] = static_cast<int16_t>(std::lround(in[a] + (in[b] - in[a]) * t));
        }
        return out;
    }

    void mix(int16_t *frames, size_t frameCount, size_t)
    {
        std::unique_lock<std::mutex> lock(g_lock, std::try_to_lock);
        if (!lock.owns_lock())
            return;
        const float target = g_pauseTarget.load(std::memory_order_relaxed);
        if (g_playing.empty())
        {
            g_pauseGain = target;
            return;
        }
        const float gainFrom = g_pauseGain;
        for (Playing &p : g_playing)
        {
            const std::vector<int16_t> &pcm = p.sound->pcm;
            float gain = gainFrom;
            for (size_t i = 0; i < frameCount && p.at < pcm.size(); ++i, ++p.at)
            {
                gain = gain < target ? std::min(target, gain + kPauseStep) : std::max(target, gain - kPauseStep);
                const float v = pcm[p.at] * gain;
                frames[i * 2] = static_cast<int16_t>(std::clamp(frames[i * 2] + v * p.left, -32768.0f, 32767.0f));
                frames[i * 2 + 1] = static_cast<int16_t>(std::clamp(frames[i * 2 + 1] + v * p.right, -32768.0f, 32767.0f));
            }
        }
        for (size_t i = 0; i < frameCount; ++i)
            g_pauseGain = g_pauseGain < target ? std::min(target, g_pauseGain + kPauseStep)
                                               : std::max(target, g_pauseGain - kPauseStep);
        g_playing.erase(std::remove_if(g_playing.begin(), g_playing.end(),
                                       [](const Playing &p) { return p.at >= p.sound->pcm.size(); }),
                        g_playing.end());
    }

    // FUN_001c3e18: the cage's event ours; any other the game's
    void dispatch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t event = getRegU32(ctx, 5);
        if ((Ps2FastRead32(rdram, event + 4u) & ~0x10000000u) != vendetta_clipsounds::kCustomSound)
        {
            g_original(rdram, ctx, runtime);
            return;
        }
        if (getRegU32(ctx, 6) != 0u)
        {
            const uint32_t number = Ps2FastRead32(rdram, event + 8u), player = getRegU32(ctx, 4);
            // the game's own effects level, over its loudest
            float level = 0.0f, top = 0.0f;
            const uint32_t levelBits = Ps2FastRead32(rdram, kEffectsGroup + 8u);
            std::memcpy(&level, &levelBits, 4);
            top = static_cast<float>(static_cast<int32_t>(Ps2FastRead32(rdram, kEffectsTable + 4u * 32u)));
            const float effects = top > 0.0f ? std::clamp(level / top, 0.0f, 1.0f) : 1.0f;
            // where its wrestler is across the screen
            float pan = 0.0f;
            if (player < 4u)
            {
                const uint32_t f = kFighters + kFighterBytes * player;
                auto at = [&](uint32_t o) { float v; const uint32_t b = Ps2FastRead32(rdram, f + kHips + o); std::memcpy(&v, &b, 4); return v; };
                if (!vendetta_cage::screenPan(at(0u), at(4u), at(8u), pan))
                    pan = 0.0f;
            }
            std::lock_guard<std::mutex> lock(g_lock);
            if (number < g_recordings.size())
            {
                const float gain = g_recordings[number]->volume * effects;
                Playing p{g_recordings[number], 0, gain * std::min(1.0f, 1.0f - pan), gain * std::min(1.0f, 1.0f + pan)};
                g_playing.push_back(p);
                if (logOn())
                    std::cerr << "[cage] sound: " << g_recordings[number]->name << " (player " << player << ", frame "
                              << static_cast<int16_t>(Ps2FastRead16(rdram, event)) << ", effects " << effects << ", across "
                              << pan << ": left " << p.left << " right " << p.right << ")" << std::endl;
            }
        }
        SET_GPR_U64(ctx, 2, 1u);
        ctx->pc = getRegU32(ctx, 31);
    }
}

namespace vendetta_cage_sounds
{
    // FUN_001c0650: the pause's fade (1) or the resume's (0), then the game's own
    void pauseFade(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool paused = getRegU32(ctx, 4) != 0u;
        g_pauseTarget.store(paused ? 0.0f : 1.0f, std::memory_order_relaxed);
        if (logOn())
            std::cerr << "[cage] sounds: " << (paused ? "the pause fades them out" : "the resume fades them in") << std::endl;
        g_originalPause(rdram, ctx, runtime);
    }
}

void vendetta_cage::loadCustomSounds(const CageFile &file)
{
    using namespace vendetta_cage_sounds;
    std::vector<std::shared_ptr<const Recording>> recordings;
    std::map<std::string, uint32_t> ids;
    for (const auto &[name, custom] : file.customSounds)
    {
        auto r = std::make_shared<Recording>();
        r->name = name;
        r->volume = custom.volume;
        std::vector<int16_t> pcm;
        uint32_t rate = kRate;
        std::string error;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(custom.file, ec))
            error = "isn't there";
        else if (vendetta_voicelines::readWavFile(custom.file, pcm, rate, error))
        {
            r->pcm = to48k(pcm, rate);
            if (r->pcm.size() > kRate * kMostSeconds)
                r->pcm.resize(kRate * kMostSeconds);
        }
        if (!error.empty())
        {
            std::cerr << "[cage] cage.json: custom sound \"" << name << "\": " << custom.file.filename().string() << " "
                      << error << ": not used" << std::endl;
            continue;
        }
        ids[name] = static_cast<uint32_t>(recordings.size());
        if (logOn())
            std::cerr << "[cage] custom sound " << name << ": " << r->pcm.size() / static_cast<double>(kRate) << " s, volume "
                      << r->volume << std::endl;
        recordings.push_back(std::move(r));
    }
    std::lock_guard<std::mutex> lock(g_lock);
    g_recordings = std::move(recordings);
    g_playing.clear();
    g_ids = std::move(ids);
}

const std::map<std::string, uint32_t> &vendetta_cage::customSoundIds() { return vendetta_cage_sounds::g_ids; }

void installVendettaCageSounds(PS2Runtime &runtime)
{
    using namespace vendetta_cage_sounds;
    g_original = runtime.lookupFunction(kDispatch);
    if (!g_original)
    {
        std::cerr << "[cage] the sound events' dispatcher isn't where the original program has it: no custom sounds"
                  << std::endl;
        return;
    }
    runtime.replaceFunction(kDispatch, dispatch);
    vendetta_mixer::add(mix);
    g_originalPause = runtime.lookupFunction(kPauseFade);
    if (g_originalPause)
        runtime.replaceFunction(kPauseFade, pauseFade);
}
