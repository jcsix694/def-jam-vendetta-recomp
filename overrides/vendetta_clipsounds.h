// Def Jam Vendetta (USA, SLUS-20639) - the sound events of animations from mods (vendetta_clipsounds.cpp).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class PS2Runtime;

namespace vendetta_mods
{
    struct Json;
}

namespace vendetta_clipsounds
{
    // One sound event: at `frame` of the animation, event `id` (below 0x10000 a sound, 0x10000 and up a command;
    // 0x10000000 = only the first time the animation starts) with its values.
    struct Event
    {
        int16_t frame = 0;
        uint32_t id = 0;
        std::vector<uint32_t> values;
    };

    // Called once at start (game_overrides): the sound step's clip name table in the store, the lookup hooked.
    void install(PS2Runtime &runtime);

    // An animation's "sounds" list (from its settings file) as events, sorted by frame; an event that can't be used is
    // left out with a message naming `where`. `lastFrame` = the animation's last frame (events after it never play).
    std::vector<Event> readSounds(const vendetta_mods::Json &list, const std::string &where, uint32_t lastFrame);

    // A new bank 1 clip (2003 and up) gets its name in the sound step's table: ZNEW<clip>. Returns the name.
    std::string nameNewClip(uint32_t clip);

    // The events the clip named `soundName` plays (the name the sound step looks up: the game's clip name, or a new
    // clip's ZNEW name), in place of the game's; no events = silent. A later call for the same name wins.
    void setSounds(const std::string &soundName, const std::vector<Event> &events, const std::string &from);

    // Once the match's mods are in: the events go into the store's match area, where the game's sound code reads them.
    void place();

    // A new match: every mod's events forgotten (the game's own play again until place()).
    void reset();
}
