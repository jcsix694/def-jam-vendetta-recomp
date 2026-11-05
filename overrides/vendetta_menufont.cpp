// Def Jam Vendetta (USA, SLUS-20639) — a menu theme's font file drawn into letters (see vendetta_menufont.h).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
#include "vendetta_menufont.h"

#include "raylib.h"

#include <cstring>

namespace vendetta_menufont
{
    bool letters(const std::vector<uint8_t> &file, int pixels, int first, int last, std::vector<Letter> &out)
    {
        out.clear();
        if (file.empty() || pixels < 4 || last < first)
            return false;
        std::vector<int> codes;
        for (int c = first; c <= last; ++c)
            codes.push_back(c);
        GlyphInfo *glyphs = LoadFontData(file.data(), int(file.size()), pixels, codes.data(), int(codes.size()), FONT_DEFAULT);
        if (!glyphs)
            return false;
        for (size_t i = 0; i < codes.size(); ++i)
        {
            const GlyphInfo &g = glyphs[i];
            Letter l;
            l.code = g.value;
            l.offsetX = g.offsetX;
            l.offsetY = g.offsetY;
            l.advance = g.advanceX;
            l.width = g.image.width;
            l.height = g.image.height;
            if (g.image.data && l.width > 0 && l.height > 0 && g.image.format == PIXELFORMAT_UNCOMPRESSED_GRAYSCALE)
            {
                l.coverage.resize(size_t(l.width) * l.height);
                std::memcpy(l.coverage.data(), g.image.data, l.coverage.size());
            }
            else
                l.width = l.height = 0;
            out.push_back(std::move(l));
        }
        UnloadFontData(glyphs, int(codes.size()));
        return !out.empty();
    }
}
