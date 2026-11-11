// Def Jam Vendetta (USA, SLUS-20639) - sound of our own mixed into the game's output, shared by everything that has some.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// The audio backend takes one mixer (PS2AudioBackend::setPcmMixer: called with each chunk of the game's 48 kHz stereo
// output before it is queued). Every user adds its own here instead (the video themes, the music themes, the cage's
// own sounds): one mixer of ours is given to the backend and calls each in the order they were added.
#pragma once

#include <cstddef>
#include <cstdint>

namespace vendetta_mixer
{
    // frames: L / R interleaved, frameCount of them, added to in place; queuedFrames: the frames already waiting to be
    // played. Any thread.
    using Mixer = void (*)(int16_t *frames, size_t frameCount, size_t queuedFrames);

    // At start-up: a mixer added (at most 8; the same one once).
    void add(Mixer mixer);
}
