// Def Jam Vendetta (USA, SLUS-20639) — files of the recomp's own that the game opens by name as if they were on its
// disc (vendetta_discfiles.cpp), and the standard songs those files are.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace vendetta_discfiles
{
    // A standard song: a folder in game\standard\music\ with the game's own three match files (<code>.mpf,
    // Mai_<code>.mus, Ovl_<code>.mus), its menu version (menu.mus: the menu file's pieces of it, one after another, as
    // on the disc) and song.json { "key", "code", "artist", "song", "album" }.
    struct Song
    {
        std::string key, code;             // "kross-ova-zan"; "KROS" (upper case, as its files are named)
        std::string artist, title, album;  // the song box's three lines
        std::filesystem::path menu;        // menu.mus, empty when the folder has none
    };

    // The standard songs, by key (none with VENDETTA_STANDARD=0 or on an install without them).
    const std::vector<Song> &songs();

    // From now on the game's lookups of song `gameCode`'s three files ("STOM": STOM.MPF, MAI_STOM.MUS, OVL_STOM.MUS)
    // are answered with standard song `ourCode`'s; answerOwn(): every name its own again.
    void answerAs(const std::string &gameCode, const std::string &ourCode);
    void answerOwn();
}
