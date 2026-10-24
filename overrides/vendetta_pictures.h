// Def Jam Vendetta (USA, SLUS-20639) — pictures from mods' files, drawn by the graphics card renderer at any size in
// place of a picture the game uploads ("host images", runtime/gs/gs_gpu.h).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// The game keeps drawing its own way: it uploads one of its pictures (a stand-in) to a GS address and draws it. Just
// before, show() puts the GS register 0x70 into the game's GS stream, in order: paraLLEl-GS then draws the mod's
// picture wherever that upload is sampled, until the next upload to the address. Nothing of the picture enters the
// game's memory, and the GS's own texture size is kept, so the picture lines up whatever its size.
//
// Pictures are read and decoded on a background thread, several files at once, and kept on the graphics card within a
// budget (the least recently shown go first). A picture asked for before it is ready shows as nothing (transparent)
// for that moment: the game never waits. With the CPU renderer (VENDETTA_GS=cpu) there are no host images: available()
// says so, and callers let the game draw its own picture.
#pragma once

#include <cstdint>
#include <filesystem>

class PS2Runtime;

namespace vendetta_pictures
{
    void install(PS2Runtime &runtime);

    // Whether host images can be drawn (the graphics card renderer is running).
    bool available();

    enum class Kind
    {
        Plain,  // the file as it is
        Shadow, // a grid icon's shade: black, as dark as the file is dark, clear where it is clear
    };

    // A picture from a file, shown on a GS texture of `width` x `height` texels (the game's shape: the file is
    // stretched to it), kept at up to `scale` times that size (smaller when the file is smaller). Nothing is read
    // until it is wanted or shown. Returns its id (never 0).
    uint32_t add(const std::filesystem::path &file, uint32_t width, uint32_t height, uint32_t scale, Kind kind = Kind::Plain);

    // Read it in the background (if not yet), so it is ready when shown.
    void want(uint32_t id);

    // Just before the game uploads its stand-in picture to GS address `tbp` (in blocks): that upload draws picture id
    // (nothing while it is still being read). The game thread, in its GS order. False (nothing done) when the picture
    // can't be shown (no graphics card renderer, or its file can't be read): the caller lets the game draw its own.
    bool show(uint32_t tbp, uint32_t id);

    // The same, drawing nothing there (transparent).
    void hide(uint32_t tbp);
}

void installVendettaPictures(PS2Runtime &runtime);
