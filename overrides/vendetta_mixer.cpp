// Def Jam Vendetta (USA, SLUS-20639) - sound of our own mixed into the game's output (vendetta_mixer.h).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
#include "vendetta_mixer.h"

#include "runtime/ps2_audio.h"

#include <array>
#include <atomic>
#include <iostream>

namespace vendetta_mixer
{
    namespace
    {
        constexpr size_t kMost = 8;
        std::array<std::atomic<Mixer>, kMost> g_mixers{};
        std::atomic<size_t> g_count{0};

        void all(int16_t *frames, size_t frameCount, size_t queuedFrames)
        {
            const size_t count = g_count.load(std::memory_order_acquire);
            for (size_t i = 0; i < count; ++i)
                if (Mixer mixer = g_mixers[i].load(std::memory_order_acquire))
                    mixer(frames, frameCount, queuedFrames);
        }
    }

    void add(Mixer mixer)
    {
        if (!mixer)
            return;
        const size_t count = g_count.load(std::memory_order_acquire);
        for (size_t i = 0; i < count; ++i)
            if (g_mixers[i].load(std::memory_order_acquire) == mixer)
                return;
        if (count >= kMost)
        {
            std::cerr << "[mixer] more than " << kMost << " mixers: one left out" << std::endl;
            return;
        }
        g_mixers[count].store(mixer, std::memory_order_release);
        g_count.store(count + 1, std::memory_order_release);
        PS2AudioBackend::setPcmMixer(all);
    }
}
