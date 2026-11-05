// Def Jam Vendetta (USA, SLUS-20639) — a menu theme's font file drawn into letters (vendetta_menus.cpp uses it).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
// Kept apart from the rest so raylib's header (stb_truetype inside) never meets the platform's.
#pragma once

#include <cstdint>
#include <vector>

namespace vendetta_menufont
{
    struct Letter
    {
        int code = 0;
        int offsetX = 0, offsetY = 0; // from the pen and from the line's top, in pixels
        int advance = 0;              // the pen's step, in pixels
        int width = 0, height = 0;
        std::vector<uint8_t> coverage; // width x height, 0-255
    };

    // The letters `first`..`last` of a .ttf / .otf file at `pixels` (raylib's LoadFontData); false if it can't be read.
    bool letters(const std::vector<uint8_t> &file, int pixels, int first, int last, std::vector<Letter> &out);
}
