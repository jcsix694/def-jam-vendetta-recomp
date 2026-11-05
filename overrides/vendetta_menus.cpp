// Def Jam Vendetta (USA, SLUS-20639) — menu themes (see vendetta_menus.h).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
#include "vendetta_menus.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_gpu.h"
#include "runtime/ps2_widescreen.h"
#include "vendetta_menufont.h"
#include "vendetta_mods.h"
#include "vendetta_pictures.h"
#include "vendetta_store.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace vendetta_menus
{
    namespace
    {
        using Fn = PS2Runtime::RecompiledFunction;

        // the catalogue (vendetta_menu_pieces.inc, made by tools/make_menu_pieces.py from config/menu_pieces.csv)
        struct MenuPiece
        {
            const char *name;
            uint8_t kind;   // 1 picture, 2 words, 3 words built at run time, 4 a plain quad, 5 a screen copy
            uint8_t owner;  // 0 menu, 1 wrestler, 2 arena, 3 the game's own
            uint8_t source; // pictures: 1 PRM pack, 2 group
            uint32_t a, b;  // pack / group and record (words: the text's address)
            int16_t u0, v0, u1, v1;
            uint32_t firstRa, raCount;
            uint8_t grows;  // 1: a bar, drawn from its left edge as wide as a value (u1 = the widest seen)
        };
#include "vendetta_menu_pieces.inc"
        constexpr size_t kPieceCount = sizeof(kMenuPieces) / sizeof(kMenuPieces[0]);
        constexpr int kFormat = 1; // the newest "menus" format this game reads

        constexpr uint32_t kQuad = 0x183C88u;                  // FUN_00183c88(ctx, quad): every menu picture
        constexpr uint32_t kPrm = 0x157570u, kPrmEnd = 0x157A70u; // FUN_00157570(pack, record): picker 1
        constexpr uint32_t kArt = 0x184D78u, kArtEnd = 0x185128u; // FUN_00184d78(ctx, group, record): picker 2
        constexpr uint32_t kTexSet = 0x185128u;                // the texture set the pickers use; called directly too
        constexpr uint32_t kTex0 = 0x23F600u;                  // the quad's texture (a TEX0 register)
        constexpr uint32_t kQuadSize = 8u + 4u * 0x24u;        // {blend, textured, 4 x {x, y, z, r, g, b, a, u, v}}

        enum class Fit
        {
            Stretch,
            Inside,
            Fill,
        };

        // one piece a theme changes
        struct Change
        {
            std::string theme, file;
            uint32_t image = 0, width = 0, height = 0; // the picture (vendetta_pictures) and its own size
            Fit fit = Fit::Stretch;
            int colour = 0; // 0 the game's, 1 white, 2 rgb
            float rgb[3] = {1.0f, 1.0f, 1.0f};
            bool wide = false; // "shape": "16:9" (drawn as its middle 4:3 until wide menus)
            bool logged = false;
        };

        constexpr uint32_t kFontDraw = 0x1BAC70u;    // FUN_001bac70(font, x, y, text, ...): every word in the menu font
        constexpr uint32_t kFontMeasure = 0x1BA8A0u; // FUN_001ba8a0(font, text, ...): its width (the callers centre by it)
        constexpr uint32_t kFontColour = 0x1BA7D8u;  // FUN_001ba7d8(font, r, g, b, a): every word's colour (0x80 = full)
        constexpr uint32_t kTint = 0x1857B8u;        // FUN_001857b8(corner, r, g, b, a in f12-f15): a picture's tint

        // one words piece a theme changes
        struct Words
        {
            std::string theme, piece;
            std::string text;                 // the new words ("" = the game's)
            uint32_t buffer = 0;              // the new words in guest memory (the text pointer swapped to it)
            int colour = 0;                   // 0 the game's, 1 the theme's highlight, 2 rgb
            uint32_t rgb[3] = {0x80u, 0x80u, 0x80u}; // 0x80 = full
            bool shadow = true;               // false: the game's black shadow under it not drawn
            int checked = 0;                  // the % codes against the game's text: 0 not yet, 1 the same, -1 not (the game's kept)
            bool logged = false;
        };

        PS2Runtime *g_runtime = nullptr;
        Fn g_quad = nullptr, g_prm = nullptr, g_art = nullptr, g_texSet = nullptr;
        Fn g_fontDraw = nullptr, g_fontMeasure = nullptr, g_fontColour = nullptr, g_tint = nullptr;
        std::unordered_map<uint32_t, Words> g_words;       // a text's address in the executable -> the theme's change
        std::unordered_map<uint32_t, Words> g_builtWords;  // words built at run time: by the draw's return address
        bool g_highlightOn = false;
        uint32_t g_highlight[3] = {0x80u, 0x78u, 0u};      // the theme's highlight (0x80 = full)
        uint32_t g_lastColour[3] = {0x80u, 0x80u, 0x80u}, g_lastAlpha = 0x80u; // the font's colour as last set

        // a theme's font file ("fonts": { "menu": ..., "header": ... }): its letters drawn instead of the game's
        constexpr uint32_t kFontChunks[4] = {0xFEF500u, 0xFFCD00u, 0x100A500u, 0x1017D00u}; // fonts 130-133 in RAM
        constexpr uint32_t kGlyphSlot = 0x3FBDu;  // the GS address every letter is uploaded to before it is drawn
        constexpr uint32_t kGlyphBytes = 288u;    // a letter: 24 x 24 at 4 bits a pixel, uploaded straight from its chunk
        constexpr uint32_t kFontScale = 4u;       // our letters drawn at 4x the game's 32 x 32 texture
        constexpr uint32_t kFontImageBase = 0x51000000u;
        std::filesystem::path g_fontFile[2]; // the menu font (every chunk), the header font (131: the header's words)
        std::string g_fontTheme;
        bool g_fontsBuilt = false;
        std::unordered_map<std::string, uint32_t> g_byName;
        std::unordered_multimap<uint64_t, uint32_t> g_byKey; // a picture's source + UV rect -> its pieces
        std::unordered_multimap<uint64_t, uint32_t> g_bars;  // a bar's source + its row (u0, v0, v1; u1 0) -> its pieces
        std::unordered_map<uint32_t, Change> g_changes;      // piece -> the theme's change
        uint32_t g_scratch = 0;                              // the changed quad's copy (guest memory)
        uint32_t g_srcKind = 0, g_srcA = 0, g_srcB = 0;      // the current picture: 1 PRM, 2 group, 3 a direct set
        bool g_log = false;
        std::set<uint64_t> g_gapsSaid;

        uint64_t key(uint32_t source, uint32_t a, uint32_t b, int u0, int v0, int u1, int v1)
        {
            auto c = [](int v) { return uint64_t(std::clamp(v, 0, 2047)); };
            return (uint64_t(source & 3u) << 62) | (uint64_t(a & 0xFFu) << 54) | (uint64_t(b & 0x3FFu) << 44) | (c(u0) << 33) |
                   (c(v0) << 22) | (c(u1) << 11) | c(v1);
        }

        // ---- code after a guest call (the scheduler can leave a hooked call early; this runs `post` when it really
        // returns: at once, or from a trampoline on the return address's table slot) ----
        using Post = std::function<void(uint8_t *, R5900Context *)>;
        struct Pending
        {
            uint32_t sp;
            Post post;
        };
        std::mutex g_deferLock;
        std::unordered_map<uint32_t, Fn> g_raOriginal;
        std::unordered_multimap<uint32_t, Pending> g_pending;

        void raTrampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t pc = ctx->pc, sp = getRegU32(ctx, 29);
            std::vector<Pending> run;
            Fn orig = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_deferLock);
                if (const auto it = g_raOriginal.find(pc); it != g_raOriginal.end())
                    orig = it->second;
                const auto range = g_pending.equal_range(pc);
                for (auto p = range.first; p != range.second;)
                    if (p->second.sp == sp)
                    {
                        run.push_back(std::move(p->second));
                        p = g_pending.erase(p);
                    }
                    else
                        ++p;
            }
            for (auto &p : run)
                p.post(rdram, ctx);
            if (orig)
                orig(rdram, ctx, runtime);
        }

        void defer(uint32_t ra, uint32_t sp, Post post)
        {
            std::lock_guard<std::mutex> lock(g_deferLock);
            if (!g_raOriginal.count(ra))
            {
                Fn orig = g_runtime->lookupFunction(ra);
                if (!orig || orig == raTrampoline || !g_runtime->replaceFunction(ra, raTrampoline))
                    return;
                g_raOriginal[ra] = orig;
            }
            g_pending.insert({ra, Pending{sp, std::move(post)}});
        }

        void callThen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, Fn fn, Post post)
        {
            const uint32_t ra = getRegU32(ctx, 31), sp = getRegU32(ctx, 29);
            try
            {
                fn(rdram, ctx, runtime);
            }
            catch (...)
            {
                defer(ra, sp, std::move(post));
                throw;
            }
            if (ctx->pc == ra && getRegU32(ctx, 29) == sp)
                post(rdram, ctx);
            else
                defer(ra, sp, std::move(post));
        }

        // ---- the hooks ----
        void prm(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_srcKind = 1u, g_srcA = getRegU32(ctx, 4), g_srcB = getRegU32(ctx, 5);
            g_prm(rdram, ctx, runtime);
        }

        void art(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_srcKind = 2u, g_srcA = getRegU32(ctx, 5), g_srcB = getRegU32(ctx, 6);
            g_art(rdram, ctx, runtime);
        }

        void texSet(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31);
            if (!((ra >= kPrm && ra < kPrmEnd) || (ra >= kArt && ra < kArtEnd)))
                g_srcKind = 3u; // a screen copy, a photo, a fade: never a theme's
            g_texSet(rdram, ctx, runtime);
        }

        float readFloat(const uint8_t *p)
        {
            float f;
            std::memcpy(&f, p, 4);
            return f;
        }

        void writeFloat(uint8_t *p, float f) { std::memcpy(p, &f, 4); }

        // The piece a quad draws: its source + UV rect, the one drawn from this return address when several share it.
        int findPiece(const uint8_t *q, uint32_t ra)
        {
            float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
            for (int i = 0; i < 4; ++i)
            {
                const uint8_t *vtx = q + 8 + 0x24 * i;
                const float u = readFloat(vtx + 0x1C), v = readFloat(vtx + 0x20);
                u0 = std::min(u0, u), u1 = std::max(u1, u), v0 = std::min(v0, v), v1 = std::max(v1, v);
            }
            const uint64_t k = key(g_srcKind, g_srcA, g_srcB, int(u0 + 0.5f), int(v0 + 0.5f), int(u1 + 0.5f), int(v1 + 0.5f));
            auto range = g_byKey.equal_range(k);
            if (range.first == range.second) // a bar: its row, any width from its left edge
                range = g_bars.equal_range(key(g_srcKind, g_srcA, g_srcB, int(u0 + 0.5f), int(v0 + 0.5f), 0, int(v1 + 0.5f)));
            int first = -1;
            for (auto it = range.first; it != range.second; ++it)
            {
                const MenuPiece &p = kMenuPieces[it->second];
                if (first < 0)
                    first = int(it->second);
                for (uint32_t i = 0; i < p.raCount; ++i)
                    if (kMenuPieceRas[p.firstRa + i] == ra)
                        return int(it->second);
            }
            if (first < 0 && g_log && g_gapsSaid.insert(k ^ (uint64_t(ra) << 1)).second)
                std::cerr << "[menus] a picture no catalogue piece names: source " << g_srcKind << " " << g_srcA << "/" << g_srcB
                          << " uv " << int(u0 + 0.5f) << "," << int(v0 + 0.5f) << "-" << int(u1 + 0.5f) << "," << int(v1 + 0.5f)
                          << " ra 0x" << std::hex << ra << std::dec << std::endl;
            return first;
        }

        // The changed quad: its UVs over the whole picture (fit / fill), its corners (fit inside), its colour.
        void changeQuad(uint8_t *q, const MenuPiece &piece, const Change &c, uint32_t texW, uint32_t texH, float widen)
        {
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            for (int i = 0; i < 4; ++i)
            {
                const uint8_t *vtx = q + 8 + 0x24 * i;
                x0 = std::min(x0, readFloat(vtx)), x1 = std::max(x1, readFloat(vtx));
                y0 = std::min(y0, readFloat(vtx + 4)), y1 = std::max(y1, readFloat(vtx + 4));
            }
            // widen: the box is shown that much wider than drawn (a wide frame), so a 16:9 backdrop fills the wide frame
            const float boxW = std::max(1e-3f, x1 - x0), boxH = std::max(1e-3f, y1 - y0), boxA = boxW / boxH * widen;
            const float imgA = c.height ? float(c.width) / float(c.height) : boxA;
            // a 16:9 backdrop: filled, so the wide frame shows all of it and a 4:3 frame its middle (never squashed)
            const Fit fit = c.wide ? Fit::Fill : c.fit;
            float du = std::max(1.0f, float(piece.u1 - piece.u0));
            const float dv = std::max(1.0f, float(piece.v1 - piece.v0));
            if (piece.grows) // a bar: its picture over its full width, so a part-full bar shows that part of it
                for (int i = 0; i < 4; ++i)
                    du = std::max(du, readFloat(q + 8 + 0x24 * i + 0x1C) - piece.u0);
            for (int i = 0; i < 4; ++i)
            {
                uint8_t *vtx = q + 8 + 0x24 * i;
                float tu = (readFloat(vtx + 0x1C) - piece.u0) / du, tv = (readFloat(vtx + 0x20) - piece.v0) / dv;
                if (fit == Fit::Fill)
                {
                    if (imgA > boxA)
                        tu = 0.5f + (tu - 0.5f) * (boxA / imgA);
                    else
                        tv = 0.5f + (tv - 0.5f) * (imgA / boxA);
                }
                else if (fit == Fit::Inside)
                {
                    const float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1);
                    if (imgA > boxA)
                        writeFloat(vtx + 4, cy + (readFloat(vtx + 4) - cy) * (boxA / imgA));
                    else
                        writeFloat(vtx, cx + (readFloat(vtx) - cx) * (imgA / boxA));
                }
                writeFloat(vtx + 0x1C, std::clamp(tu, 0.0f, 1.0f) * float(texW));
                writeFloat(vtx + 0x20, std::clamp(tv, 0.0f, 1.0f) * float(texH));
                if (c.colour)
                    for (int k = 0; k < 3; ++k)
                        writeFloat(vtx + 0xC + 4 * k, c.colour == 1 ? 1.0f : c.rgb[k]);
            }
        }

        // ---- the 16:9 backdrop: a wide frame with the menu's 4:3 in the middle ----
        constexpr float kFrameW = 640.0f; // the game's screen space: 640 x 480
        uint32_t g_backdrop = ~0u;        // the backdrop's catalogue index
        bool g_wideBackdrop = false;      // the theme's backdrop is 16:9 and the player has widescreen on

        // A quad reaching past the 4:3 frame's sides (a screen sliding in or out) cut at them, its colours and UVs with
        // it, so nothing of it shows over the wide backdrop's sides; false when nothing of it is left. Only an upright
        // rectangle is cut (every sliding piece is one); any other shape is left to the renderer's squeeze.
        bool cutAtFrame(uint8_t *q)
        {
            float xs[4], ys[4];
            for (int i = 0; i < 4; ++i)
                xs[i] = readFloat(q + 8 + 0x24 * i), ys[i] = readFloat(q + 8 + 0x24 * i + 4);
            const float x0 = *std::min_element(xs, xs + 4), x1 = *std::max_element(xs, xs + 4);
            if (x0 >= 0.0f && x1 <= kFrameW)
                return true;
            if (x1 <= 0.0f || x0 >= kFrameW)
                return false;
            for (int i = 0; i < 4; ++i)
                if ((xs[i] != x0 && xs[i] != x1))
                    return true;
            const float cut0 = std::max(x0, 0.0f), cut1 = std::min(x1, kFrameW);
            uint8_t before[kQuadSize];
            std::memcpy(before, q, kQuadSize);
            for (int i = 0; i < 4; ++i)
            {
                // its partner: the corner on the same row at the other side
                int j = -1;
                for (int k = 0; k < 4; ++k)
                    if (k != i && ys[k] == ys[i] && xs[k] != xs[i])
                        j = k;
                if (j < 0)
                    return true; // not an upright rectangle after all
                const float to = xs[i] == x0 ? cut0 : cut1;
                const float t = (to - xs[i]) / (xs[j] - xs[i]);
                const uint8_t *a = before + 8 + 0x24 * i, *b = before + 8 + 0x24 * j;
                uint8_t *out = q + 8 + 0x24 * i;
                writeFloat(out, to);
                for (int f = 2; f < 9; ++f) // z, r, g, b, a, u, v along the row
                    writeFloat(out + 4 * f, readFloat(a + 4 * f) + t * (readFloat(b + 4 * f) - readFloat(a + 4 * f)));
            }
            return true;
        }

        void quad(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t q = getRegU32(ctx, 5), ra = getRegU32(ctx, 31);
            const uint8_t *qp = rdram + (q & PS2_RAM_MASK);
            uint32_t textured = 0;
            std::memcpy(&textured, qp + 4, 4);
            const bool picked = textured && g_srcKind >= 1u && g_srcKind <= 2u;
            const int index = picked ? findPiece(qp, ra) : -1;
            const auto it = index < 0 ? g_changes.end() : g_changes.find(uint32_t(index));
            const bool change = it != g_changes.end() && g_scratch && vendetta_pictures::available();
            if (g_wideBackdrop && change && uint32_t(index) == g_backdrop)
                ps2_widescreen::markCentredFrame(); // this frame is a wide menu (the backdrop is drawn first)
            // in a wide menu frame, a piece crossing the 4:3 frame's sides is cut at them (the backdrop and the fades are
            // the frame's own size: never cut)
            const bool cut = g_wideBackdrop && g_scratch && ps2_widescreen::centredNow(100);
            if (!change)
            {
                if (!cut)
                {
                    g_quad(rdram, ctx, runtime);
                    return;
                }
                uint8_t *copy = rdram + (g_scratch & PS2_RAM_MASK);
                std::memcpy(copy, qp, kQuadSize);
                if (!cutAtFrame(copy))
                {
                    ctx->pc = ra; // all of it outside the frame: not drawn
                    return;
                }
                SET_GPR_U32(ctx, 5, g_scratch);
                g_quad(rdram, ctx, runtime);
                return;
            }
            Change &c = it->second;
            uint64_t tex0 = 0;
            std::memcpy(&tex0, rdram + kTex0, 8);
            const uint32_t tbp = uint32_t(tex0 & 0x3FFFu);
            const uint32_t texW = 1u << ((tex0 >> 26) & 0xFu), texH = 1u << ((tex0 >> 30) & 0xFu);
            uint8_t *copy = rdram + (g_scratch & PS2_RAM_MASK);
            std::memcpy(copy, qp, kQuadSize);
            const float widen = c.wide && g_wideBackdrop && uint32_t(index) == g_backdrop ? ps2_widescreen::targetAspect() / (4.0f / 3.0f) : 1.0f;
            changeQuad(copy, kMenuPieces[index], c, texW, texH, widen);
            if (cut && !cutAtFrame(copy))
            {
                ctx->pc = ra;
                return;
            }
            if (!vendetta_pictures::bindNow(tbp, c.image))
            {
                g_quad(rdram, ctx, runtime);
                return;
            }
            if (g_log && !c.logged)
            {
                c.logged = true;
                std::cerr << "[menus] " << kMenuPieces[index].name << " drawn from " << c.theme << "'s " << c.file << std::endl;
            }
            SET_GPR_U32(ctx, 5, g_scratch);
            callThen(rdram, ctx, runtime, g_quad, [tbp](uint8_t *, R5900Context *) { vendetta_pictures::unbindNow(tbp); });
        }

        // ---- words: the text swapped at the draw and the measure, the colours at the colour call ----
        std::string guestText(uint8_t *rdram, uint32_t address)
        {
            std::string s;
            for (uint32_t i = 0; i < 1024u; ++i)
            {
                const char c = static_cast<char>(rdram[(address + i) & PS2_RAM_MASK]);
                if (!c)
                    break;
                s += c;
            }
            return s;
        }

        // a text's % codes in order ("%s|%d|"), "%%" left out: the new words must keep the game's
        std::string codes(const std::string &t)
        {
            std::string out;
            for (size_t i = 0; i < t.size(); ++i)
            {
                if (t[i] != '%')
                    continue;
                if (i + 1 < t.size() && t[i + 1] == '%')
                {
                    ++i;
                    continue;
                }
                size_t j = i + 1;
                while (j < t.size() && std::strchr("-+ #0123456789.lh", t[j]))
                    ++j;
                if (j < t.size())
                {
                    out += t.substr(i, j - i + 1) + "|";
                    i = j;
                }
            }
            return out;
        }

        // the first time a text is drawn or measured: its % codes against the game's (else the game's text is kept)
        bool usable(uint8_t *rdram, Words &w, uint32_t text)
        {
            if (!w.buffer)
                return false;
            if (w.checked == 0)
            {
                const std::string game = guestText(rdram, text);
                w.checked = codes(game) == codes(w.text) ? 1 : -1;
                if (w.checked < 0)
                    vendetta_mods::problem() << "[menus] " << w.theme << ": " << w.piece << "'s words need the game's codes in order ("
                                             << (codes(game).empty() ? std::string("none") : codes(game)) << "): the game's words kept" << std::endl;
            }
            return w.checked > 0;
        }

        uint32_t read32(uint8_t *rdram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4);
            return v;
        }

        // The theme's font, once (at the first word drawn: the font chunks are in RAM by then). For each of the game's
        // four menu fonts: its letters drawn from the file at kFontScale x the game's 32 x 32 letter texture, their
        // capital H as tall as the game's and on its baseline; each one bound to the game's letter by the letter's own
        // 288 bytes (the renderer binds it when those bytes are uploaded to kGlyphSlot), and each letter's width in the
        // chunk's table set to the new letter's, so the game lays out and measures (centres) with the new font.
        void buildFonts(uint8_t *rdram, PS2Runtime *runtime)
        {
            g_fontsBuilt = true;
            GSGpuRenderer *renderer = runtime->gs().gpuRendererForSettings();
            if (!renderer)
            {
                std::cerr << "[menus] the CPU renderer draws no font files: the game's font is shown" << std::endl;
                return;
            }
            for (uint32_t chunk = 0; chunk < 4u; ++chunk)
            {
                const std::filesystem::path &file = chunk == 1u && !g_fontFile[1].empty() ? g_fontFile[1] : g_fontFile[0];
                if (file.empty())
                    continue;
                std::vector<uint8_t> bytes;
                std::ifstream in(file, std::ios::binary);
                bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                const uint32_t base = kFontChunks[chunk];
                const uint32_t cellW = read32(rdram, base + 4u), cellH = read32(rdram, base + 8u);
                if (cellW != 24u || cellH != 24u)
                    continue;
                // the game's capital H: its top and its baseline (the bottom of its last row)
                const uint32_t hEntry = base + 0x40Cu + 8u * 'H', hOffset = read32(rdram, hEntry), hWidth = read32(rdram, hEntry + 4u);
                int top = -1, bottom = -1;
                for (uint32_t y = 0; y < 24u && hOffset; ++y)
                    for (uint32_t x = 0; x <= hWidth && x < 24u; ++x)
                    {
                        const uint8_t b = rdram[(base + hOffset + (y * 24u + x) / 2u) & PS2_RAM_MASK];
                        if (((x & 1u) ? b >> 4 : b & 0xFu) != 0u)
                        {
                            if (top < 0)
                                top = int(y);
                            bottom = int(y);
                        }
                    }
                if (top < 0)
                    continue;
                const int capH = bottom - top + 1, baseline = bottom + 1;
                std::vector<vendetta_menufont::Letter> probe, letters;
                if (!vendetta_menufont::letters(bytes, 100, 'H', 'H', probe) || probe.empty() || probe[0].height <= 0)
                {
                    vendetta_mods::problem() << "[menus] " << g_fontTheme << ": " << file.filename().string()
                                             << " can't be read as a font: the game's font kept" << std::endl;
                    return;
                }
                const int pixels = std::max(8, int(std::lround(100.0 * capH * kFontScale / probe[0].height)));
                if (!vendetta_menufont::letters(bytes, pixels, 0x21, 0x7E, letters))
                    continue;
                const auto H = std::find_if(letters.begin(), letters.end(), [](const auto &l) { return l.code == 'H'; });
                if (H == letters.end())
                    continue;
                const int fromTop = H->offsetY + H->height; // our baseline, from the line's top
                const uint32_t side = 32u * kFontScale;
                uint32_t made = 0;
                for (const vendetta_menufont::Letter &l : letters)
                {
                    const uint32_t entry = base + 0x40Cu + 8u * uint32_t(l.code), offset = read32(rdram, entry);
                    if (!offset)
                        continue; // a code the game's font has no letter for
                    std::vector<uint32_t> rgba(size_t(side) * side, 0x00FFFFFFu);
                    const int x0 = std::max(0, l.offsetX), y0 = int(baseline * kFontScale) - (fromTop - l.offsetY);
                    for (int y = 0; y < l.height; ++y)
                        for (int x = 0; x < l.width; ++x)
                        {
                            const int dx = x0 + x, dy = y0 + y;
                            if (dx < 0 || dy < 0 || dx >= int(side) || dy >= int(side))
                                continue;
                            const uint32_t a = (uint32_t(l.coverage[size_t(y) * l.width + x]) * 128u + 127u) / 255u;
                            rgba[size_t(dy) * side + dx] = 0x00FFFFFFu | (a << 24);
                        }
                    // its width in the game's terms: the sprite is (width + 1) texels wide, the pen steps that much
                    int width = int(std::lround(double(l.advance) / kFontScale)) - 1;
                    width = std::max(width, int((x0 + l.width + kFontScale - 1) / kFontScale) - 1);
                    width = std::clamp(width, 1, 31);
                    uint64_t hash = 0xcbf29ce484222325ull; // FNV-1a 64 over the game's letter, as the renderer hashes uploads
                    for (uint32_t i = 0; i < kGlyphBytes; ++i)
                        hash = (hash ^ rdram[(base + offset + i) & PS2_RAM_MASK]) * 0x100000001b3ull;
                    const uint32_t id = kFontImageBase + chunk * 0x100u + uint32_t(l.code);
                    renderer->setHostImage(id, side, side, std::move(rgba));
                    renderer->setHostImageForContent(kGlyphSlot, hash, id);
                    const uint32_t w = uint32_t(width);
                    std::memcpy(rdram + ((entry + 4u) & PS2_RAM_MASK), &w, 4);
                    ++made;
                }
                std::cerr << "[menus] font " << (130 + chunk) << ": " << made << " letters from " << g_fontTheme << "'s "
                          << file.filename().string() << " (" << pixels << " px, cap height " << capH << ")" << std::endl;
            }
        }

        // the game's yellows (every highlight site): 1 the full ones, 0.5 the half one, 2 the over-full one (Choose Photo)
        float yellow(uint32_t r, uint32_t g, uint32_t b)
        {
            if (b != 0u)
                return 0.0f;
            if (r == 0x80u && (g == 0x78u || g == 0x80u))
                return 1.0f;
            if (r == 0x40u && g == 0x40u)
                return 0.5f;
            if (r == 0xFFu && g == 0xFFu)
                return 2.0f;
            return 0.0f;
        }

        void fontColour(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            uint32_t rgb[3] = {getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7)};
            if (g_highlightOn)
            {
                if (const float f = yellow(rgb[0], rgb[1], rgb[2]); f > 0.0f)
                    for (int k = 0; k < 3; ++k)
                        rgb[k] = std::min(255u, uint32_t(float(g_highlight[k]) * f + 0.5f));
            }
            for (int k = 0; k < 3; ++k)
                SET_GPR_U32(ctx, 5 + k, rgb[k]);
            std::memcpy(g_lastColour, rgb, sizeof(rgb));
            g_lastAlpha = getRegU32(ctx, 8);
            g_fontColour(rdram, ctx, runtime);
        }

        void tint(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // a picture tinted yellow by its corners' colour (window choices, digits, The Ladder's list): the highlight
            if (g_highlightOn)
            {
                const auto v = [&](int reg) { return uint32_t(std::lround(std::max(0.0f, ctx->f[reg]) * 128.0f)); };
                if (const float f = yellow(v(12), v(13), v(14)); f > 0.0f)
                    for (int k = 0; k < 3; ++k)
                        ctx->f[12 + k] = std::min(2.0f, float(g_highlight[k]) * f / 128.0f);
            }
            g_tint(rdram, ctx, runtime);
        }

        // The game's colour routine called for a text of its own colour (it never waits: it and its one helper are leaves of
        // the font's list), the registers kept as they were, so the hooked draw goes on with its own arguments.
        void setColour(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t font, const uint32_t rgb[3], uint32_t alpha)
        {
            __m128i regs[32];
            std::memcpy(regs, ctx->r, sizeof(regs));
            const uint32_t pc = ctx->pc;
            const uint64_t hi = ctx->hi, lo = ctx->lo;
            SET_GPR_U32(ctx, 4, font);
            for (int k = 0; k < 3; ++k)
                SET_GPR_U32(ctx, 5 + k, rgb[k]);
            SET_GPR_U32(ctx, 8, alpha);
            g_fontColour(rdram, ctx, runtime);
            std::memcpy(ctx->r, regs, sizeof(regs));
            ctx->pc = pc;
            ctx->hi = hi;
            ctx->lo = lo;
            std::memcpy(g_lastColour, rgb, sizeof(g_lastColour));
            g_lastAlpha = alpha;
        }

        Words *wordsFor(uint32_t text, uint32_t ra, uint32_t &key)
        {
            if (const auto it = g_words.find(text); it != g_words.end())
            {
                key = text;
                return &it->second;
            }
            if (const auto it = g_builtWords.find(ra); it != g_builtWords.end())
            {
                key = ra | 0x80000000u;
                return &it->second;
            }
            return nullptr;
        }

        void fontDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_fontsBuilt && (!g_fontFile[0].empty() || !g_fontFile[1].empty()))
                buildFonts(rdram, runtime);
            const uint32_t text = getRegU32(ctx, 7), ra = getRegU32(ctx, 31);
            uint32_t key = 0;
            Words *w = wordsFor(text, ra, key);
            if (!w)
            {
                g_fontDraw(rdram, ctx, runtime);
                return;
            }
            const bool black = !(g_lastColour[0] | g_lastColour[1] | g_lastColour[2]);
            if (!w->shadow && black)
            {
                ctx->pc = ra; // its black shadow: not drawn
                SET_GPR_U32(ctx, 2, 0u);
                return;
            }
            const bool swapped = key < 0x80000000u && usable(rdram, *w, text);
            if (swapped)
                SET_GPR_U32(ctx, 7, w->buffer);
            if (g_log && !w->logged)
            {
                w->logged = true;
                std::cerr << "[menus] " << w->piece << " drawn with " << w->theme << (swapped ? "'s words" : "'s colour or shadow, the game's words") << std::endl;
            }
            if (!w->colour || black)
            {
                g_fontDraw(rdram, ctx, runtime); // the game's colour (its shadow stays black)
                return;
            }
            // its own colour for this draw only, the font's colour back after it (the next words keep theirs)
            const uint32_t font = getRegU32(ctx, 4), alpha = g_lastAlpha;
            uint32_t before[3], own[3];
            std::memcpy(before, g_lastColour, sizeof(before));
            for (int k = 0; k < 3; ++k)
                own[k] = w->colour == 1 ? g_highlight[k] : w->rgb[k];
            setColour(rdram, ctx, runtime, font, own, alpha);
            callThen(rdram, ctx, runtime, g_fontDraw, [font, alpha, before, runtime](uint8_t *rdram, R5900Context *ctx)
                     { setColour(rdram, ctx, runtime, font, before, alpha); });
        }

        void fontMeasure(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_fontsBuilt && (!g_fontFile[0].empty() || !g_fontFile[1].empty()))
                buildFonts(rdram, runtime);
            const uint32_t text = getRegU32(ctx, 5);
            if (const auto it = g_words.find(text); it != g_words.end() && usable(rdram, it->second, text))
                SET_GPR_U32(ctx, 5, it->second.buffer); // measured as drawn: the game's centring follows the new words
            g_fontMeasure(rdram, ctx, runtime);
        }

        // ---- the themes ----
        bool readText(const std::filesystem::path &file, std::string &text)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in)
                return false;
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return true;
        }

        // a colour of a theme's file: [r, g, b] (0-255) to the game's (0x80 = full)
        bool readColour(const vendetta_mods::Json *colour, uint32_t out[3])
        {
            if (!colour || !colour->isArray() || colour->items.size() != 3)
                return false;
            for (int k = 0; k < 3; ++k)
                out[k] = uint32_t(std::clamp(colour->items[k].number, 0.0, 255.0) * 128.0 / 255.0 + 0.5);
            return true;
        }

        // a words piece: "text" (or "lines": its first), "colour" ("game", "highlight", [r, g, b]), "shadow"
        void readWords(const vendetta_mods::Mod &mod, uint32_t index, const vendetta_mods::Json &entry)
        {
            const MenuPiece &piece = kMenuPieces[index];
            Words w;
            w.theme = mod.name;
            w.piece = piece.name;
            const vendetta_mods::Json *text = entry.find("text");
            if (!text || !text->isString())
                if (const vendetta_mods::Json *lines = entry.find("lines"); lines && lines->isArray() && !lines->items.empty())
                    text = &lines->items[0];
            if (text && text->isString())
            {
                if (piece.kind == 3)
                    vendetta_mods::problem() << "[menus] " << mod.name << ": " << piece.name
                                             << "'s words are made by the game as it runs: only their colour and shadow change" << std::endl;
                else if (text->text.size() > 255u)
                    vendetta_mods::problem() << "[menus] " << mod.name << ": " << piece.name << "'s words are longer than 255 letters: the game's kept" << std::endl;
                else
                {
                    w.text = text->text;
                    w.buffer = vendetta_store::allocate(vendetta_store::Area::Boot, uint32_t(w.text.size() + 1u), "menu themes: words");
                    if (w.buffer)
                        std::memcpy(vendetta_store::host(w.buffer), w.text.c_str(), w.text.size() + 1u);
                }
            }
            if (const vendetta_mods::Json *colour = entry.find("colour"))
            {
                if (colour->isString() && colour->text == "highlight")
                    w.colour = 1;
                else if (readColour(colour, w.rgb))
                    w.colour = 2;
            }
            if (const vendetta_mods::Json *shadow = entry.find("shadow"); shadow && shadow->type == vendetta_mods::Json::Type::Bool)
                w.shadow = shadow->boolean;
            if (!w.buffer && !w.colour && w.shadow)
                return;
            if (piece.kind == 2)
            {
                if (const auto had = g_words.find(piece.a); had != g_words.end())
                    vendetta_mods::problem() << "[menus] " << piece.name << ": " << mod.name << "'s words used, not "
                                             << had->second.theme << "'s (the later in the project wins)" << std::endl;
                g_words[piece.a] = std::move(w);
            }
            else
                for (uint32_t i = 0; i < piece.raCount; ++i)
                    g_builtWords[kMenuPieceRas[piece.firstRa + i]] = w;
        }

        void readPieces(const vendetta_mods::Mod &mod, const std::string &relative, const std::string &screen)
        {
            const std::filesystem::path file = vendetta_mods::modFile(mod, relative, "menu theme file");
            if (file.empty())
                return;
            std::string text, error;
            vendetta_mods::Json json;
            if (!readText(file, text) || !vendetta_mods::parseJson(text, json, error))
            {
                vendetta_mods::problem() << "[menus] " << mod.name << ": " << relative << " can't be read: " << error << std::endl;
                return;
            }
            if (screen.empty() && readColour(json.find("highlight"), g_highlight))
                g_highlightOn = true; // the theme's highlight: every yellow the game uses for it (the later theme's wins)
            if (const vendetta_mods::Json *fonts = json.find("fonts"); screen.empty() && fonts && fonts->isObject())
            {
                const std::string folder = std::filesystem::path(relative).parent_path().generic_string();
                for (const auto &[which, value] : fonts->members)
                {
                    const int slot = which == "menu" ? 0 : which == "header" ? 1 : -1;
                    if (slot < 0 || !value.isString())
                        continue;
                    const std::filesystem::path font = vendetta_mods::modFile(mod, folder.empty() ? value.text : folder + "/" + value.text, "menu font");
                    if (!font.empty())
                    {
                        g_fontFile[slot] = font; // the later theme's wins
                        g_fontTheme = mod.name;
                    }
                }
            }
            const vendetta_mods::Json *pieces = json.find("pieces");
            if (!pieces || !pieces->isObject())
                return;
            const std::string folder = std::filesystem::path(relative).parent_path().generic_string();
            for (const auto &[name, entry] : pieces->members)
            {
                auto found = screen.empty() ? g_byName.end() : g_byName.find(screen + "." + name);
                if (found == g_byName.end())
                    found = g_byName.find(name);
                if (found == g_byName.end())
                {
                    vendetta_mods::problem() << "[menus] " << mod.name << ": " << relative << ": the game's menus have no piece \""
                                             << name << "\"" << std::endl;
                    continue;
                }
                const MenuPiece &piece = kMenuPieces[found->second];
                if (piece.owner != 0)
                {
                    static const char *const owners[] = {"", "a wrestler's", "an arena's", "the game's own"};
                    vendetta_mods::problem() << "[menus] " << mod.name << ": " << name << " is " << owners[std::min<int>(piece.owner, 3)]
                                             << " picture, not changed by a menu theme" << std::endl;
                    continue;
                }
                if (piece.kind == 2 || piece.kind == 3)
                {
                    readWords(mod, found->second, entry);
                    continue;
                }
                const vendetta_mods::Json *picture = entry.find("picture");
                if (piece.kind != 1 || !picture || !picture->isString())
                    continue;
                Change c;
                c.theme = mod.name;
                c.file = picture->text;
                const std::string within = folder.empty() ? picture->text : folder + "/" + picture->text;
                const std::filesystem::path path = vendetta_mods::modFile(mod, within, "menu picture");
                if (path.empty())
                    continue;
                vendetta_pictures::fileSize(path, c.width, c.height);
                if (!c.width || !c.height)
                {
                    vendetta_mods::problem() << "[menus] " << mod.name << ": " << within << " can't be read as a picture" << std::endl;
                    continue;
                }
                uint32_t w = c.width, h = c.height; // kept at its own size, at most 4096 a side
                while (w > 4096u || h > 4096u)
                    w = std::max(1u, w / 2u), h = std::max(1u, h / 2u);
                c.image = vendetta_pictures::add(path, w, h, 1u);
                vendetta_pictures::want(c.image);
                if (const vendetta_mods::Json *fit = entry.find("fit"); fit && fit->isString())
                    c.fit = fit->text == "fit" ? Fit::Inside : fit->text == "fill" ? Fit::Fill : Fit::Stretch;
                if (const vendetta_mods::Json *colour = entry.find("colour"))
                {
                    if (colour->isString() && colour->text == "white")
                        c.colour = 1;
                    else if (colour->isArray() && colour->items.size() == 3)
                    {
                        c.colour = 2;
                        for (int k = 0; k < 3; ++k)
                            c.rgb[k] = float(std::clamp(colour->items[k].number, 0.0, 255.0) / 255.0);
                    }
                }
                if (const vendetta_mods::Json *shape = entry.find("shape"); shape && shape->isString())
                    c.wide = shape->text == "16:9";
                if (const auto had = g_changes.find(found->second); had != g_changes.end())
                    vendetta_mods::problem() << "[menus] " << piece.name << ": " << mod.name << "'s picture used, not "
                                             << had->second.theme << "'s (the later in the project wins)" << std::endl;
                g_changes[found->second] = std::move(c);
            }
        }

        void readThemes()
        {
            for (const vendetta_mods::Mod &mod : vendetta_mods::loadedMods())
            {
                const vendetta_mods::Json *menus = mod.manifest.find("menus");
                if (!menus || !menus->isObject())
                    continue;
                const vendetta_mods::Json *format = menus->find("format");
                if (format && format->number > kFormat)
                {
                    vendetta_mods::problem() << "[menus] " << mod.name << "'s menus need a newer game (format "
                                             << int(format->number) << "): skipped" << std::endl;
                    continue;
                }
                if (const vendetta_mods::Json *shared = menus->find("shared"); shared && shared->isString())
                    readPieces(mod, shared->text, "");
                if (const vendetta_mods::Json *screens = menus->find("screens"); screens && screens->isObject())
                    for (const auto &[screen, file] : screens->members)
                        if (file.isString())
                            readPieces(mod, file.text, screen);
            }
        }

        void hook(PS2Runtime &runtime, uint32_t address, Fn &original, Fn wrapper)
        {
            original = runtime.lookupFunction(address);
            if (original)
                runtime.replaceFunction(address, wrapper);
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *off = std::getenv("VENDETTA_MENUS"); off && *off == '0')
            return;
        g_runtime = &runtime;
        g_log = std::getenv("VENDETTA_MENULOG") && *std::getenv("VENDETTA_MENULOG") != '0';
        for (uint32_t i = 0; i < kPieceCount; ++i)
        {
            const MenuPiece &p = kMenuPieces[i];
            g_byName.emplace(p.name, i);
            if (p.kind == 1 && p.source && p.grows)
                g_bars.emplace(key(p.source, p.a, p.b, p.u0, p.v0, 0, p.v1), i);
            else if (p.kind == 1 && p.source)
                g_byKey.emplace(key(p.source, p.a, p.b, p.u0, p.v0, p.u1, p.v1), i);
        }
        readThemes();
        if (const auto b = g_byName.find("backdrop.background"); b != g_byName.end())
        {
            g_backdrop = b->second;
            const auto c = g_changes.find(g_backdrop);
            g_wideBackdrop = c != g_changes.end() && c->second.wide && ps2_widescreen::targetAspect() > 0.0f;
            if (g_wideBackdrop)
                std::cerr << "[menus] " << c->second.theme << "'s backdrop is 16:9: menu screens shown wide, the menus in the 4:3 middle" << std::endl;
        }
        const bool words = !g_words.empty() || !g_builtWords.empty() || g_highlightOn || !g_fontFile[0].empty() || !g_fontFile[1].empty();
        if (g_changes.empty() && !words)
            return; // no menu theme: nothing hooked, the game's menus untouched
        if (!g_changes.empty() &&
            (g_scratch = vendetta_store::allocate(vendetta_store::Area::Boot, 0x100u, "menu themes: a changed quad")) != 0u)
        {
            // last among the picture hooks, so these see the game's own arguments (the wrestlers' and arenas' hooks run inside)
            hook(runtime, kPrm, g_prm, prm);
            hook(runtime, kArt, g_art, art);
            hook(runtime, kTexSet, g_texSet, texSet);
            hook(runtime, kQuad, g_quad, quad);
        }
        if (words)
        {
            hook(runtime, kFontDraw, g_fontDraw, fontDraw);
            hook(runtime, kFontMeasure, g_fontMeasure, fontMeasure);
            hook(runtime, kFontColour, g_fontColour, fontColour);
            if (g_highlightOn)
                hook(runtime, kTint, g_tint, tint);
        }
        std::cerr << "[menus] the project's menu themes change " << g_changes.size() << " pictures, " << g_words.size() + g_builtWords.size()
                  << " words" << (g_highlightOn ? ", the highlight colour" : "") << std::endl;
    }
}

void installVendettaMenus(PS2Runtime &runtime) { vendetta_menus::install(runtime); }
