// Def Jam Vendetta (USA, SLUS-20639) - the wrestlers drawn by our own code (VENDETTA_WRESTLERDRAW).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// Every scene draws a fighter through FUN_001b1b20(fighter, 1): it poses the bones, uploads the skin, then sends each
// model part (the body, BODY or BODY2 by the High Poly bit; the left hand; the right hand; the face on High Poly) with
// FUN_001b93a0(part, light matrix, colour matrix). That routine writes into the part's VIF1 stream the colour matrix
// (stream + 0x40 = VU 0x3C-0x3F) and, per bone with a segment s >= 0, 8 rows at stream + 0x80 + 0x80 s (VU 0x40 + 8 s):
// rows 0-3 the bone's + 0x150 (inverse bind x world x the camera), rows 4-7 its + 0x110 with row 3's x, y, z zeroed,
// times the light matrix; then it starts the VIF1 transfer of the stream to the VU1 program MPG #7/#8, which draws it.
//
// With VENDETTA_WRESTLERDRAW on, our code takes that routine's place for those four calls (from FUN_001b1b20 only:
// return addresses 0x1B1F40 body, 0x1B1FD0 left hand, 0x1B1FF4 right hand, 0x1B2054 face; the crowd's and the ring's
// calls stay the game's): it writes the same rows into the stream (the game's own maths, so memory stays as the game
// leaves it), then reads the stream's batches and draws them as GS triangles into the game's GS stream, at the same
// moment, with the VU1 program's maths step by step (round toward zero, no fused multiply-add):
//   per vertex (1 bone, or 2: each bone's result times its weight, then added) p = rows 0-3 x position; Q = 1 / p.w;
//   x, y = p.xy x Q, z not divided; a vertex past the near limit (z > 1048574), behind (z < 0) or more than 2047
//   pixels from the screen's centre is flagged and every triangle using it left out (ADC), nothing is cut; the light
//   d = max(rows 4-7 x normal, 0), colour = d . (3 light colours + the ambient, alpha 128) truncated; ST = (s, t, 1) x Q;
//   strips (PRIM 0x21C) then the loose triangles (0x21B), from the part's own GIF tags (VU 0x39 / 0x3A), its TEX0
//   (VU 0x3B, the skin the game uploaded).
// Everything else stays the game's: posing, the face, the lights, the skin upload and TEXFLUSH, the shadow pass, the
// see-through parts (ALPHA1 / 2, drawn on the main processor), the body points.
//
// A mod wrestler with a model of its own (vendetta_wrestlers.h: model.wrs, as adjmk writes the game's format; glTF to
// come) is always drawn by our code, whatever the switch: the game still loads a game model behind it and poses it.
// The model behind is one whose rig has the mod's parents (chosen at start, the record's + 0x1C changed when needed):
// a replaced attire's own game model, a new attire's Briggs', or, when that rig differs from the mod's, Briggs' and
// then the first game model whose rig matches (Capone's for a model built on his: his body bone 41 hangs from bone 1,
// every other model's from bone 0). The model behind's slot words are pointed at the mod's own parts and data (placed
// in the store), so the game poses the mod's own skeleton, face and hands; the body's part call then draws the mod's
// body, hands and face with those posed bones and the game's lights, the hands' and face's calls draw nothing for it.
// The mod's own see-through parts (ALPHA1 / ALPHA2, when its file has them) are posed and mounted by the game's code
// too and drawn by our port of the game's see-through draw (FUN_001b49a0) in FUN_001b2088's place, with the mod's skin.
// The shadow is the game's (the model behind's: every game model's is the same); a shadow or a Low Poly body in the
// mod's file is ignored. The skin: the mod file's own (decoded from its upload packet), or textures/skin.png at any
// size, as a host image bound to the skin's address (GS register 0x71) around the mod's triangles. A file that can't
// be used leaves the game's model.
// A skin with pixels not solid (PNG transparency): the body's call draws only the solid pixels (alpha test), and the
// triangles touching the others are drawn again, blended and sorted far to near, in the see-through pass's slot: fully
// clear = a hole, partly clear = see-through.
//
// VENDETTA_SEETHROUGH_CHECK=1  the game's see-through draw (FUN_001b49a0) checked against our port: before each call ours
//                              is worked out, after it the game's packet (0x1E15300) and the vertices it wrote are
//                              compared, byte for byte ([wrestlers] see-through check lines); a mod fighter's own
//                              see-through parts are then drawn by the game's code (with the model behind's skin), so
//                              they are checked too
//
// VENDETTA_WRESTLERDRAW=1      our draw (sampling the skin the game uploaded)
// VENDETTA_WRESTLERDRAW=ab[:N] the game's draw and ours in turn, N seconds each (default 2): pictures compared; a mod
//                              model too (the game's turn: its parts, posed as ever, drawn by the game's routines with
//                              the model behind's skin)
// VENDETTA_WRESTLERDRAW=flip   ours, and F9 (with the game's window in front) swaps between the game's draw and ours; the
//                              window title says which ("Wrestlers: ours" / "Wrestlers: the game's")
// VENDETTA_WRESTLERDRAW=check  the game draws; its GS packets (tapped in the GS front end) are compared with ours,
//                              triangle by triangle, and the rows ours would write with the game's ([wrestlers] check
//                              lines; needs VENDETTA_CHAIN_ASYNC=0 and VENDETTA_GS_ASYNC=0; not with
//                              VENDETTA_ARENADRAW=check at the same time: one GS tap)
// VENDETTA_WRESTLERLOG=1       a count of what was drawn every 5 s
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "vendetta_gsdraw.h"
#include "vendetta_mods.h"
#include "vendetta_store.h"
#include "vendetta_wrestlers.h"
#include "runtime/gs/gs_gpu.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <string>
#include <unordered_set>
#include <vector>

namespace vendetta_wrestlerdraw
{
    namespace
    {
        using namespace vendetta_gsdraw;

        constexpr uint32_t kDrawPart = 0x1B93A0u; // FUN_001b93a0(part, light matrix, colour matrix)
        constexpr uint32_t kFromBody = 0x1B1F40u, kFromLeftHand = 0x1B1FD0u, kFromRightHand = 0x1B1FF4u,
                           kFromFace = 0x1B2054u; // its return addresses in FUN_001b1b20
        constexpr float kNearZ = 1048574.0f;      // VU 0x34.w
        constexpr float kCentre = 2048.0f;        // VU 0x34.x / y
        constexpr float kGuard = 2047.0f;         // VU 0x33.w
        // a part's VIF stream at most this long: a sanity bound only (the game's biggest body's is about 200 KB), far
        // above any real model, so a mod part's size is never limited by it
        constexpr uint32_t kMaxStreamBytes = 0x4000000u; // 64 MB

        enum class Mode
        {
            Off,
            Ours,  // our draw
            Ab,    // the game's and ours in turn
            Check, // the game's draw, ours worked out and compared
            Flip,  // ours or the game's, F9 swaps
        };

        Mode g_mode = Mode::Off;
        double g_abSeconds = 2.0;
        bool g_abOurs = false;
        bool g_log = false;
        PS2Runtime *g_runtime = nullptr;
        PS2Runtime::RecompiledFunction g_originalDrawPart = nullptr;
        using Clock = std::chrono::steady_clock;
        const Clock::time_point g_start = Clock::now();

        struct Stats
        {
            uint64_t parts = 0, gameParts = 0, batches = 0, triangles = 0, skipped = 0, unreadable = 0, seeThrough = 0;
        } g_stats;
        Clock::time_point g_lastStats = Clock::now();

        uint8_t *mem(uint8_t *rdram, uint32_t address) { return rdram + (address & PS2_RAM_MASK); }

        uint32_t word(const uint8_t *p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }

        float real(const uint8_t *p)
        {
            float v;
            std::memcpy(&v, p, 4);
            return v;
        }

        // ------------------------------------------------------------------ the rows FUN_001b93a0 writes

        // FUN_001b6598(a, b): a = a x b, as the recomp's VU0 macro code does it (VMULAx / VMADDAy / VMADDAz / VMADDw:
        // each product rounded, then added, in the game thread's own rounding)
        void multiply(float a[16], const float b[16])
        {
            float out[16];
            for (int i = 0; i < 4; ++i)
                for (int c = 0; c < 4; ++c)
                {
                    float acc = b[c] * a[4 * i];
                    acc = acc + b[4 + c] * a[4 * i + 1];
                    acc = acc + b[8 + c] * a[4 * i + 2];
                    out[4 * i + c] = acc + b[12 + c] * a[4 * i + 3];
                }
            std::memcpy(a, out, sizeof(out));
        }

        // The stream's rows as FUN_001b93a0 writes them (0x1B93E4-0x1B9470), into `stream`: the bones' matrices from
        // `posed` (the game's posed part), their segments from `layout` (the part whose stream it is: the same part
        // for the game's own models, the mod's part for a mod's model)
        void writeRows(const uint8_t *posed, const uint8_t *layout, const uint8_t *lightMatrix, const uint8_t *colours,
                       uint8_t *stream)
        {
            std::memcpy(stream + 0x40, colours, 64);
            float light[16];
            std::memcpy(light, lightMatrix, 64);
            const uint32_t bones = std::min(word(posed + 8), word(layout + 8));
            for (uint32_t k = 0; k < bones; ++k)
            {
                const uint8_t *bone = posed + 0x10 + 0x190 * k;
                const int32_t segment = static_cast<int32_t>(word(layout + 0x10 + 0x190 * k + 8));
                if (segment < 0)
                    continue;
                uint8_t *rows = stream + 0x80 + 0x80 * static_cast<uint32_t>(segment);
                std::memcpy(rows, bone + 0x150, 64);
                float m[16];
                std::memcpy(m, bone + 0x110, 64);
                m[12] = m[13] = m[14] = 0.0f; // sw 0 at + 0x70 / 0x74 / 0x78
                multiply(m, light);
                std::memcpy(rows + 0x40, m, 64);
            }
        }

        void writeRows(uint8_t *rdram, uint32_t part, uint32_t lightMatrix, uint32_t colours, uint8_t *stream)
        {
            const uint8_t *p = mem(rdram, part);
            writeRows(p, p, mem(rdram, lightMatrix), mem(rdram, colours), stream);
        }

        // ------------------------------------------------------------------ reading the part's stream

        struct Batch
        {
            uint32_t at = 0;   // the batch's first row in the stream (bytes from the stream's start)
            uint32_t rows = 0; // its rows
            uint32_t top = 0;  // its VU address (TOP)
        };

        // The part's VIF1 commands (BASE, OFFSET, NOP, the setup UNPACK at VU 0x39, FLUSH, MSCALF, then per batch an
        // UNPACK with flg and MSCALF 0x0B). False: something this reader doesn't know (the game draws it).
        bool readStream(const uint8_t *stream, uint32_t bytes, uint32_t &setupRows, std::vector<Batch> &batches)
        {
            uint32_t p = 0, base = 0, offset = 0, dbf = 0;
            bool pending = false, haveSetup = false;
            Batch next;
            while (p + 4u <= bytes)
            {
                const uint32_t w = word(stream + p);
                const uint32_t imm = w & 0xFFFFu, num = (w >> 16) & 0xFFu, cmd = (w >> 24) & 0x7Fu;
                p += 4;
                if (cmd == 0x00u || cmd == 0x10u || cmd == 0x11u || cmd == 0x13u)
                    continue;
                if (cmd == 0x03u)
                    base = imm;
                else if (cmd == 0x02u)
                {
                    offset = imm;
                    dbf = 0;
                }
                else if (cmd == 0x14u || cmd == 0x15u || cmd == 0x17u)
                {
                    if (pending)
                    {
                        if (imm != 0x0Bu)
                            return false;
                        next.top = base + (dbf ? offset : 0u);
                        batches.push_back(next);
                        pending = false;
                    }
                    dbf ^= 1u;
                }
                else if (cmd >= 0x60u)
                {
                    const uint32_t vn = (cmd >> 2) & 3u, vl = cmd & 3u;
                    const uint32_t rows = num ? num : 256u;
                    const uint32_t size = ((rows * (vn + 1u) * (32u >> vl) / 8u) + 3u) & ~3u;
                    if (vn != 3u || vl != 0u) // V4-32 only
                        return false;
                    if (!((imm >> 15) & 1u))
                    {
                        if ((imm & 0x3FFu) != 0x39u || p != 0x10u)
                            return false;
                        setupRows = rows;
                        haveSetup = true;
                    }
                    else
                    {
                        if ((imm & 0x3FFu) != 0u)
                            return false;
                        next.at = p;
                        next.rows = rows;
                        pending = true;
                    }
                    p += size;
                }
                else
                    return false;
            }
            return haveSetup && p == bytes;
        }

        // ------------------------------------------------------------------ the VU1 program's maths

        struct Out
        {
            float s = 0, t = 0, q = 0;
            uint32_t rgba[4] = {};
            uint32_t x = 0, y = 0, z = 0;
            bool flagged = true;
        };

        // A mod model's triangle that touches its skin's partly clear pixels: drawn again, blended, in the see-through
        // pass (the game's FUN_001b2088 slot), sorted far to near
        struct SoftTri
        {
            Out v[3];
        };
        constexpr uint32_t kSoftGrid = 64; // the skin's not-solid pixels marked on a 64 x 64 grid over its area

        bool touchesSoft(const Out &a, const Out &b, const Out &c, const std::vector<uint8_t> &grid)
        {
            float u0 = 2.0f, u1 = -1.0f, v0 = 2.0f, v1 = -1.0f;
            for (const Out *o : {&a, &b, &c})
            {
                const float u = o->s / o->q, v = o->t / o->q; // ST = (u, v, 1) x Q
                u0 = std::min(u0, u);
                u1 = std::max(u1, u);
                v0 = std::min(v0, v);
                v1 = std::max(v1, v);
            }
            // the skin repeats (CLAMP_2 0): cells taken modulo the grid; a triangle spanning the whole skin, or odd, is
            // drawn again to be safe
            const float g = static_cast<float>(kSoftGrid);
            if (!(u1 - u0 < 1.0f && v1 - v0 < 1.0f))
                return true;
            const int64_t x0 = static_cast<int64_t>(std::floor(u0 * g)), x1 = static_cast<int64_t>(std::floor(u1 * g));
            const int64_t y0 = static_cast<int64_t>(std::floor(v0 * g)), y1 = static_cast<int64_t>(std::floor(v1 * g));
            const int64_t n = kSoftGrid;
            for (int64_t y = y0; y <= y1; ++y)
                for (int64_t x = x0; x <= x1; ++x)
                    if (grid[static_cast<size_t>(((y % n + n) % n) * n + ((x % n + n) % n))])
                        return true;
            return false;
        }

        // MULA, MADDA, MADDA, MADD: each lane rounded at every step (toward zero)
        void xform(const float m[16], const float v[4], float r[4])
        {
            for (int c = 0; c < 4; ++c)
            {
                float acc = m[c] * v[0];
                acc = acc + m[4 + c] * v[1];
                acc = acc + m[8 + c] * v[2];
                r[c] = acc + m[12 + c] * v[3];
            }
        }

        void finish(const float row0[4], const float p[4], float d[4], const float colour[16], Out &o)
        {
            const float q = vuDiv(1.0f, p[3]);
            const float sx = p[0] * q, sy = p[1] * q;
            const bool behind = (clipFlags(sx, sy, p[2], 0.0f) & 0x20u) != 0u;
            const bool pastNear = (clipFlags(sx, sy, p[2], kNearZ) & 0x10u) != 0u;
            const bool outside = (clipFlags(sx - kCentre, sy - kCentre, 0.0f, kGuard) & 0x0Fu) != 0u;
            if (behind || pastNear || outside)
            {
                o.flagged = true;
                return;
            }
            o.flagged = false;
            for (int c = 0; c < 4; ++c)
                d[c] = d[c] > 0.0f ? d[c] : 0.0f; // MAXx with +0
            for (int c = 0; c < 4; ++c)
            {
                float acc = colour[c] * d[0];
                acc = acc + colour[4 + c] * d[1];
                acc = acc + colour[8 + c] * d[2];
                o.rgba[c] = static_cast<uint32_t>(ftoi(acc + colour[12 + c] * d[3], 1.0)) & 0xFFu;
            }
            o.s = row0[0] * q;
            o.t = row0[1] * q;
            o.q = row0[2] * q;
            o.x = static_cast<uint32_t>(ftoi(sx, 16.0)) & 0xFFFFu;
            o.y = static_cast<uint32_t>(ftoi(sy, 16.0)) & 0xFFFFu;
            o.z = static_cast<uint32_t>(ftoi(p[2], 16.0));
        }

        void vertexQwords(Packet &packet, const Out &o, bool adc)
        {
            uint32_t s, t, q;
            std::memcpy(&s, &o.s, 4);
            std::memcpy(&t, &o.t, 4);
            std::memcpy(&q, &o.q, 4);
            packet.add(uint64_t(s) | (uint64_t(t) << 32), uint64_t(q));
            packet.add(uint64_t(o.rgba[0]) | (uint64_t(o.rgba[1]) << 32), uint64_t(o.rgba[2]) | (uint64_t(o.rgba[3]) << 32));
            packet.add(uint64_t(o.x) | (uint64_t(o.y) << 32), uint64_t(o.z) | (adc ? (uint64_t(0x8000u) << 32) : 0ull));
        }

        std::vector<GsTriangle> g_ourTriangles;
        std::vector<GsTriangle> *g_capture = nullptr; // drawGamePart's caller reads the triangles back (its check)

        // One whole GIF packet (EOP on its last tag) into the game's GS stream, in order (check: read back instead)
        void send(Packet &packet)
        {
            if (packet.q.empty())
                return;
            packet.end();
            if (g_capture || g_mode == Mode::Check)
            {
                GsReader reader;
                reader.out = g_capture ? g_capture : &g_ourTriangles;
                reader.feed(reinterpret_cast<const uint8_t *>(packet.q.data()), static_cast<uint32_t>(packet.q.size() * 8u));
            }
            else
                g_runtime->memory().submitChainGif(reinterpret_cast<const uint8_t *>(packet.q.data()),
                                                   static_cast<uint32_t>(packet.q.size() * 8u));
            packet.q.clear();
            packet.lastTag = SIZE_MAX;
        }

        // TEST_2 while a wrestler draws (z test greater-or-equal, no alpha test), and for a mod skin with pixels not
        // solid the same with the alpha test on: only solid pixels write colour and z; the rest are drawn blended in the
        // see-through pass. "Solid" = alpha >= 120 of the GS's 128: a solid texel comes back as 127 (the skin is an 8-bit
        // UNORM image on the graphics card, 128 / 255 read back truncated) and the lit vertex alpha can be 127 too.
        // TEST_2 / ALPHA_2 in the see-through pass, as the game's own see-through parts have them: alpha >= 1,
        // (Cs - Cd) As + Cd
        constexpr uint32_t kSolidAlpha = 120u;
        constexpr uint64_t kWrestlerTest = 0x50000u;
        constexpr uint64_t kCutoutTest = kWrestlerTest | 1u | (5u << 1) | (uint64_t(kSolidAlpha) << 4);
        constexpr uint64_t kSeeThroughTest = 0x5001Bu;
        // VENDETTA_SEETHROUGH=0 (a test): holes only, as before soft see-through: a pixel under half solid is cut, the
        // rest solid, nothing drawn again
        const bool g_softOff = [] { const char *v = std::getenv("VENDETTA_SEETHROUGH"); return v && v[0] == '0'; }();
        const uint64_t g_cutoutTest = g_softOff ? (kWrestlerTest | 1u | (5u << 1) | (64u << 4)) : kCutoutTest;
        constexpr uint64_t kSeeThroughAlpha = 0x44u;

        // The part drawn from its stream (the rows already written). `image`: a host image bound to TEX0's address
        // around its triangles (a mod's skin), 0 for none; `cutout`: the alpha test on around them; `soft`: the
        // triangles touching `grid`'s marked cells kept for the see-through pass. False: not readable (the caller lets
        // the game draw it).
        bool drawPart(const uint8_t *p, uint32_t image = 0u, bool cutout = false, std::vector<SoftTri> *soft = nullptr,
                      const std::vector<uint8_t> *grid = nullptr)
        {
            const uint8_t *stream = p + (word(p) & ~3u);
            const uint32_t bytes = word(p + 4);
            uint32_t setupRows = 0;
            std::vector<Batch> batches;
            if (bytes == 0u || bytes > kMaxStreamBytes || !readStream(stream, bytes, setupRows, batches))
            {
                ++g_stats.unreadable;
                return false;
            }
            // VU address a of the setup rows = stream + 0x10 + 16 (a - 0x39)
            auto vu = [&](uint32_t a) -> const uint8_t * {
                return a >= 0x39u && a < 0x39u + setupRows ? stream + 0x10u + 16u * (a - 0x39u) : nullptr;
            };
            float colour[16];
            std::memcpy(colour, vu(0x3Cu), 64);
            const uint8_t *stripTag = vu(0x39u), *looseTag = vu(0x3Au), *tex0 = vu(0x3Bu);
            const VuRounding rounding;
            Packet packet;
            packet.ad(word(tex0 + 8) & 0xFFu, uint64_t(word(tex0)) | (uint64_t(word(tex0 + 4)) << 32));
            const uint32_t tbp = word(tex0) & 0x3FFFu;
            if (image)
                packet.ad(0x71u, uint64_t(tbp) | (uint64_t(image) << 32)); // GS register 0x71: our image at this address
            if (cutout)
                packet.ad(0x48u, g_cutoutTest); // TEST_2
            std::vector<Out> outs;
            for (const Batch &b : batches)
            {
                const uint8_t *rows = stream + b.at;
                const uint32_t one = word(rows) & 0xFFFFu, two = word(rows + 4) & 0xFFFFu, strips = word(rows + 8) & 0xFFFFu;
                const uint32_t loose = word(rows + 12) & 0x7FFFu;
                const uint32_t vertexRows = 3u * one + 4u * two;
                if (1u + vertexRows > b.rows)
                    return false;
                outs.assign(b.rows, Out{});
                auto palette = [&](uint32_t address, float m[16], uint32_t first) {
                    const uint8_t *r = vu((address & 0xFFFFu) + first);
                    if (!r || !vu((address & 0xFFFFu) + first + 3u))
                        return false;
                    std::memcpy(m, r, 64);
                    return true;
                };
                uint32_t row = 1;
                for (uint32_t v = 0; v < one + two; ++v)
                {
                    const uint8_t *r = rows + 16u * row;
                    float r0[4], n[4], pos[4], pp[4], d[4];
                    std::memcpy(r0, r, 16);
                    std::memcpy(n, r + 16, 16);
                    std::memcpy(pos, r + 32, 16);
                    if (v < one)
                    {
                        float P[16], L[16];
                        if (!palette(word(r + 12), P, 0) || !palette(word(r + 12), L, 4))
                            return false;
                        xform(P, pos, pp);
                        xform(L, n, d);
                        row += 3;
                    }
                    else
                    {
                        const uint8_t *wr = r + 48;
                        const float wA = real(wr + 4), wB = real(wr + 12);
                        float A[16], AL[16], B[16], BL[16], pa[4], pb[4], na[4], nb[4];
                        if (!palette(word(wr), A, 0) || !palette(word(wr), AL, 4) || !palette(word(wr + 8), B, 0) ||
                            !palette(word(wr + 8), BL, 4))
                            return false;
                        xform(A, pos, pa);
                        xform(B, pos, pb);
                        xform(AL, n, na);
                        xform(BL, n, nb);
                        for (int c = 0; c < 4; ++c)
                        {
                            pa[c] = pa[c] * wA;
                            pb[c] = pb[c] * wB;
                            pp[c] = pa[c] + pb[c];
                            na[c] = na[c] * wA;
                            nb[c] = nb[c] * wB;
                            d[c] = nb[c] + na[c];
                        }
                        row += 4;
                    }
                    finish(r0, pp, d, colour, outs[row - (v < one ? 3u : 4u)]);
                }
                // the index words (absolute VU addresses: TOP + row)
                const uint32_t words = (b.rows - 1u - vertexRows) * 4u;
                const uint8_t *idx = rows + 16u * (1u + vertexRows);
                uint32_t k = 0;
                auto vertexAt = [&](uint32_t address) -> const Out * {
                    const uint32_t r = (address & 0xFFFFu) - b.top;
                    return r >= 1u && r < b.rows ? &outs[r] : nullptr;
                };
                for (uint32_t s = 0; s < strips; ++s)
                {
                    if (k >= words)
                        return false;
                    const uint32_t count = word(idx + 4u * k++);
                    if (k + count > words || count < 3u)
                        return false;
                    uint64_t lo = (uint64_t(word(stripTag)) | (uint64_t(word(stripTag + 4)) << 32));
                    lo = (lo & ~0x7FFFull & ~0x8000ull) | count;
                    packet.tag(lo, uint64_t(word(stripTag + 8)) | (uint64_t(word(stripTag + 12)) << 32));
                    bool f1 = false, f2 = false;
                    const Out *o1 = nullptr, *o2 = nullptr;
                    for (uint32_t j = 0; j < count; ++j)
                    {
                        const Out *o = vertexAt(word(idx + 4u * (k + j)));
                        if (!o)
                            return false;
                        const bool adc = j >= 2u && (f1 || f2 || o->flagged);
                        vertexQwords(packet, *o, adc);
                        if (j >= 2u)
                        {
                            ++(adc ? g_stats.skipped : g_stats.triangles);
                            if (soft && !adc && touchesSoft(*o1, *o2, *o, *grid))
                                soft->push_back(SoftTri{{*o1, *o2, *o}});
                        }
                        f1 = f2;
                        f2 = o->flagged;
                        o1 = o2;
                        o2 = o;
                    }
                    k += count;
                }
                if (k + 3u * loose > words)
                    return false;
                uint64_t lo = (uint64_t(word(looseTag)) | (uint64_t(word(looseTag + 4)) << 32));
                lo = (lo & ~0x7FFFull & ~0x8000ull) | loose;
                packet.tag(lo, uint64_t(word(looseTag + 8)) | (uint64_t(word(looseTag + 12)) << 32));
                for (uint32_t t = 0; t < loose; ++t)
                {
                    const Out *v[3];
                    for (int c = 0; c < 3; ++c)
                        if (!(v[c] = vertexAt(word(idx + 4u * (k + 3u * t + c)))))
                            return false;
                    const bool adc = v[0]->flagged || v[1]->flagged || v[2]->flagged;
                    vertexQwords(packet, *v[0], false);
                    vertexQwords(packet, *v[1], false);
                    vertexQwords(packet, *v[2], adc);
                    ++(adc ? g_stats.skipped : g_stats.triangles);
                    if (soft && !adc && touchesSoft(*v[0], *v[1], *v[2], *grid))
                        soft->push_back(SoftTri{{*v[0], *v[1], *v[2]}});
                }
                ++g_stats.batches;
                if (packet.q.size() * 8u >= kPacketBytes)
                    send(packet);
            }
            if (image)
                packet.ad(0x71u, uint64_t(tbp)); // unbound: the game's own draws at this address are untouched
            if (cutout)
                packet.ad(0x48u, kWrestlerTest);
            send(packet);
            ++g_stats.parts;
            return true;
        }

        // ------------------------------------------------------------------ the test modes

        bool g_flipOurs = true, g_flipTitled = false;

        bool flipNow()
        {
#if defined(_WIN32)
            static auto lastPoll = Clock::now();
            static bool wasDown = false;
            const auto now = Clock::now();
            auto title = [] { showInTitle(L" | Wrestlers: ", g_flipOurs ? L"ours (F9: the game's)" : L"the game's (F9: ours)"); };
            if (!g_flipTitled)
            {
                g_flipTitled = true;
                title();
            }
            if (now - lastPoll >= std::chrono::milliseconds(30))
            {
                lastPoll = now;
                const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0 && ourWindowInFront();
                if (down && !wasDown)
                {
                    g_flipOurs = !g_flipOurs;
                    std::cerr << "[wrestlers] F9: " << (g_flipOurs ? "ours" : "the game's") << std::endl;
                    title();
                }
                wasDown = down;
            }
#endif
            return g_flipOurs;
        }

        bool oursNow()
        {
            if (g_mode == Mode::Flip)
                return flipNow();
            if (g_mode != Mode::Ab)
                return true;
            const double t = std::chrono::duration<double>(Clock::now() - g_start).count();
            const bool ours = static_cast<int64_t>(t / g_abSeconds) % 2 == 1;
            if (ours != g_abOurs)
            {
                g_abOurs = ours;
                std::cerr << "[wrestlers] ab " << t << " s: " << (ours ? "ours" : "the game's") << std::endl;
            }
            return ours;
        }

        // A mod model drawn by our code now: always, but in VENDETTA_WRESTLERDRAW=ab the game's turns leave the drawing
        // to the game's own routines (FUN_001b93a0, FUN_001b2088) on the same posed parts, the mod's, with the skin the
        // game uploads (the model behind's): for a mod whose file is the model behind's, the game's own picture)
        bool modTurn() { return g_mode != Mode::Ab || oursNow(); }

        void logStats()
        {
            if (!g_log)
                return;
            const auto now = Clock::now();
            if (now - g_lastStats < std::chrono::seconds(5))
                return;
            g_lastStats = now;
            std::cerr << "[wrestlers] 5 s: " << g_stats.parts << " parts drawn by us (" << g_stats.batches << " batches, "
                      << g_stats.triangles << " triangles, " << g_stats.skipped << " left out), " << g_stats.gameParts
                      << " by the game, " << g_stats.unreadable << " unreadable, " << g_stats.seeThrough
                      << " drawn again see-through" << std::endl;
            g_stats = {};
        }

        // ---- the check (the arenas' way: windows of 2 s, ours looked up among the game's of this window and the last)
        std::vector<GsTriangle> g_gameWindow, g_gamePrevious, g_ourWindow, g_ourPrevious;
        bool g_checkStarted = false;
        uint64_t g_checkParts = 0, g_rowsSame = 0, g_rowsDifferent = 0;
        int g_checkReports = 0;

        void gifTap(void *, uint8_t, const uint8_t *data, uint32_t bytes)
        {
            if (!g_checkStarted)
                return;
            GsReader reader;
            reader.out = &g_gameWindow;
            reader.feed(data, bytes);
        }

        void checkPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t part = getRegU32(ctx, 4), lightMatrix = getRegU32(ctx, 5), colours = getRegU32(ctx, 6);
            const uint32_t ra = getRegU32(ctx, 31), sp = getRegU32(ctx, 29);
            g_checkStarted = true;
            g_originalDrawPart(rdram, ctx, runtime);
            if (ctx->pc != ra || getRegU32(ctx, 29) != sp)
                return; // the call was left early (a scheduler checkpoint): ours not worked out
            // the rows ours would write, against the game's
            const uint8_t *p = mem(rdram, part);
            const uint32_t at = part + (word(p) & ~3u), bytes = word(p + 4);
            if (bytes != 0u && bytes <= kMaxStreamBytes)
            {
                std::vector<uint8_t> copy(mem(rdram, at), mem(rdram, at) + bytes);
                writeRows(rdram, part, lightMatrix, colours, copy.data());
                (std::memcmp(copy.data(), mem(rdram, at), bytes) == 0 ? g_rowsSame : g_rowsDifferent)++;
            }
            g_ourTriangles.clear();
            drawPart(mem(rdram, part));
            g_ourWindow.insert(g_ourWindow.end(), g_ourTriangles.begin(), g_ourTriangles.end());
            ++g_checkParts;
            static auto last = Clock::now();
            if (Clock::now() - last < std::chrono::seconds(2))
                return;
            last = Clock::now();
            std::unordered_set<GsTriangle, TriangleHash, TriangleEq> game(g_gamePrevious.begin(), g_gamePrevious.end());
            game.insert(g_gameWindow.begin(), g_gameWindow.end());
            size_t found = 0, missing = 0;
            for (const GsTriangle &t : g_ourPrevious)
            {
                if (game.count(t))
                {
                    ++found;
                    continue;
                }
                ++missing;
                if (g_checkReports < 6)
                {
                    ++g_checkReports;
                    const GsTriangle *best = nullptr;
                    int64_t bestDistance = INT64_MAX;
                    for (const GsTriangle &g : game)
                    {
                        int64_t dist = 0;
                        for (int k = 0; k < 3; ++k)
                            dist += std::llabs(int64_t(g.v[k].x) - t.v[k].x) + std::llabs(int64_t(g.v[k].y) - t.v[k].y);
                        if (dist < bestDistance)
                        {
                            bestDistance = dist;
                            best = &g;
                        }
                    }
                    std::cerr << "[wrestlers] check: a triangle of ours the game did not draw" << std::endl;
                    for (int k = 0; k < 3; ++k)
                        std::cerr << "[wrestlers]   ours " << describe(t.v[k]) << "\n[wrestlers]   game "
                                  << (best ? describe(best->v[k]) : std::string("-")) << std::endl;
                }
            }
            std::cerr << "[wrestlers] check: " << g_ourPrevious.size() << " triangles of ours (" << g_checkParts
                      << " parts in the window), " << found << " drawn the same by the game, " << missing
                      << " not; the rows ours writes: " << g_rowsSame << " parts the same as the game's, " << g_rowsDifferent
                      << " different; the game's triangles in the two windows: " << game.size() << std::endl;
            g_ourPrevious.swap(g_ourWindow);
            g_ourWindow.clear();
            g_gamePrevious.swap(g_gameWindow);
            g_gameWindow.clear();
            g_checkParts = g_rowsSame = g_rowsDifferent = 0;
        }

        // ------------------------------------------------------------------ the mods' own models

        constexpr uint32_t kPose = 0x1B1B20u;       // FUN_001b1b20(fighter, draw): pose, then draw the fighter
        constexpr uint32_t kSeeThrough = 0x1B2088u; // FUN_001b2088(fighter): its see-through parts (ALPHA1 / 2)
        constexpr uint32_t kRigs = 0x1E13980u, kRigSize = 0x34Cu, kRigModel = 0x334u; // the rig's model pointer
        constexpr uint32_t kFighters = 0x22FE08u, kFighterSize = 0xCD8u, kFighterRecord = 0x34u;
        constexpr uint32_t kModelBuffer = 0x96000u; // a fighter's model buffer (0x8C4500 + n x 0x96000)
        constexpr uint32_t kSkinImageBase = 0x50000000u; // our host image ids for the mods' skins
        constexpr int kBody = 1, kBody2 = 2, kFace = 3, kLeftHand = 4, kRightHand = 5, kAlpha1 = 6, kAlpha2 = 7,
                      kTexture = 18;
        constexpr uint32_t kRigLights = 0x328u, kRigColours = 0x32Cu, kRigFlags = 0x344u; // + 0x344 bit 0: High Poly
        constexpr uint32_t kRecordSize = 0x20u, kRecordModel = 0x1Cu; // a character record: + 0x1C its DATA.BIN entry
        constexpr uint32_t kGameCharacters = 0x27E100u;  // the game's own character table (the game reads the store's copy)
        constexpr uint32_t kGameModelRecords = 59u;      // records 0-58 (59-62 have no model)
        constexpr uint32_t kDataTable = 0x256F40u, kDataEntries = 293u; // DATA.BIN's table: {first sector, sectors}

        PS2Runtime::RecompiledFunction g_originalPose = nullptr, g_originalSeeThrough = nullptr;

        // The mod's own parts and their data are not copied into the model behind but pointed at: a model's slot words are
        // self-relative (slot n's data = model + 0x10 + 4n + the word) and the game follows them afresh each time
        // (FUN_001b1b20 the parts, FUN_001b22e8 / 2620 / 2848 the body and its template, FUN_001b2998 the face, hands
        // and slots 9-17, FUN_001b3400 the body points), so the mod's BODY (for BODY2 too), FACE, hands, templates and
        // motions, with any number of bones past the game's, sit in the store's match area and the model behind's words
        // are rewritten to reach them; its see-through parts (6 / 7) point at the mod's own when it has them and they are
        // safe for the game's posing (checkAlpha, and ALPHA2 against the face's template), else at none (every reader
        // checks)
        constexpr int kFirstPointed = 1, kLastPointed = 17;
        constexpr uint32_t kChannelRoom = 25u; // the game's sample buffers (0x1E146B0 + 0x190 n; the rig's 0x190): 25 channels
        constexpr uint32_t kBodyBones = 42u, kFaceBones = 11u, kHandBones = 13u; // the game's rig: the clips, the body
                                                                               // points, the mounts, the eyes and lips
        constexpr uint32_t kWalkDepth = 512u; // FUN_001b95f0 walks the bones recursively (0x90 bytes of stack a level)

        // the game's rig bones of a part slot (1-5) or a template slot (8-13)
        uint32_t standardBones(int slot)
        {
            switch (slot)
            {
            case 1: case 2: case 8: return kBodyBones;
            case 3: case 9: case 10: case 11: return kFaceBones;
            default: return kHandBones;
            }
        }

        // the part a template (8-13) is applied to
        int partOfTemplate(int slot) { return slot == 8 ? 1 : slot <= 11 ? 3 : slot == 12 ? 4 : 5; }

        struct ModModel
        {
            std::vector<uint8_t> file;
            int32_t part[32];
            uint32_t partSize[32] = {};  // each part's bytes: to the next part or the file's end
            bool pointable[32] = {};     // slots 1-17: checked, the game may read the mod's own
            uint32_t placedAt[32] = {};  // their guest addresses in the store's match area (0: not placed this match)
            std::string what;            // "[wrestlers] <mod>: <wrestler>: <file>", for mods.log
            std::string behind = "Briggs'"; // whose game model stands behind it ("Briggs'", "Capone's"), for mods.log
            bool ok = false;
            uint32_t image = 0;
            bool cutout = false;           // its skin has pixels not solid: the alpha test on, those drawn see-through
            std::vector<uint8_t> softGrid; // where they are (kSoftGrid x kSoftGrid over the skin)
            const uint8_t *at(int slot) const { return part[slot] >= 0 ? file.data() + part[slot] : nullptr; }
            uint8_t *at(int slot) { return part[slot] >= 0 ? file.data() + part[slot] : nullptr; }
        };

        std::unordered_map<uint32_t, std::unique_ptr<ModModel>> g_models; // by record
        uint32_t g_lastRecords[4] = {~0u, ~0u, ~0u, ~0u};
        uint32_t g_nextImage = kSkinImageBase;
        // VENDETTA_MODMODEL_GAMEDRAW=1 (a test, the reference picture): a mod's WRS copied whole into the game's model
        // buffer (the PS2 way; it must fit 614,400 bytes), so the game draws it itself; our draw leaves it alone
        bool g_modGameDraw = false;

        // Per fighter: its mod model's triangles for the see-through pass, kept from the body's call
        struct SeeThroughList
        {
            std::vector<SoftTri> tris;
            uint32_t tex0Reg = 0x07u, image = 0;
            uint64_t tex0 = 0;
            bool pending = false, noPass = false;
        };
        SeeThroughList g_seeThrough[4];

        // The kept triangles sorted far to near (Z grows nearer) and drawn blended, as the game's own see-through parts:
        // ALPHA (Cs - Cd) As + Cd, TEST alpha >= 1 with the z test, z written; TEST = `testAfter` after (the context of
        // the skin's TEX0: TEX0_1 0x06 / TEX0_2 0x07)
        void drawSeeThrough(SeeThroughList &list, uint64_t testAfter)
        {
            if (list.tris.empty())
                return;
            std::stable_sort(list.tris.begin(), list.tris.end(), [](const SoftTri &a, const SoftTri &b) {
                return uint64_t(a.v[0].z) + a.v[1].z + a.v[2].z < uint64_t(b.v[0].z) + b.v[1].z + b.v[2].z;
            });
            const uint32_t context = list.tex0Reg == 0x06u ? 0u : 1u;
            Packet packet;
            packet.ad(list.tex0Reg, list.tex0);
            const uint32_t tbp = static_cast<uint32_t>(list.tex0) & 0x3FFFu;
            if (list.image)
                packet.ad(0x71u, uint64_t(tbp) | (uint64_t(list.image) << 32));
            packet.ad(0x42u + context, kSeeThroughAlpha);
            packet.ad(0x47u + context, kSeeThroughTest);
            for (size_t i = 0; i < list.tris.size();)
            {
                // a triangle list: PRIM 0x25B (IIP, TME, ABE, context 2), PACKED, ST / RGBAQ / XYZF2 (as the wrestler
                // kernel's own tags: our z is already shifted for XYZF2), NLOOP 3 a triangle
                const size_t n = std::min<size_t>(list.tris.size() - i, 0x2000u);
                packet.tag(uint64_t(3u * n) | (1ull << 46) | (uint64_t(0x25Bu & ~(context ? 0u : 0x200u)) << 47) |
                               (3ull << 60),
                           0x412ull);
                for (size_t k = 0; k < n; ++k, ++i)
                    for (const Out &o : list.tris[i].v)
                        vertexQwords(packet, o, false);
                if (packet.q.size() * 8u >= kPacketBytes)
                    send(packet);
            }
            packet.ad(0x47u + context, testAfter);
            if (list.image)
                packet.ad(0x71u, uint64_t(tbp)); // unbound
            send(packet);
            g_stats.seeThrough += list.tris.size();
            list.tris.clear();
        }

        // ------------------------------------------------------------------ the see-through parts (ALPHA1 / ALPHA2)
        //
        // FUN_001b2088(fighter), High Poly only (rig + 0x344 bit 0): the skin uploaded again to the other slot, then for
        // ALPHA1 (slot 6) and ALPHA2 (slot 7) FUN_001b44d0 writes the skin's TEX0 into the part (+0x0C / +0x10) and
        // FUN_001b49a0(part, rig + 0x328, rig + 0x32C) draws it on the main processor. A part: +0x00 bones, +0x04
        // vertices, +0x08 triangles, the bones from +0x20 (0x190 each: +0x00 child, +0x04 sibling, +0x10 rest local,
        // +0x50 current local, +0x90 inverse bind, +0xD0 world, +0x110 inverse bind x world, +0x150 that x the camera),
        // then the vertices (0x48 each: +0x00 position, +0x0C normal, +0x18 u, +0x1C v, +0x20 bone A, +0x24 bone B or -1,
        // +0x28 / +0x2C their weights; the routine writes +0x30 x, +0x34 y, +0x38 z, +0x3C-0x44 the colour), then the
        // triangles (3 x u32 vertex numbers). Posing (FUN_001b4620: from FUN_001b22e8 / 2620 with the body's template
        // cut to 9 bones, from FUN_001b2998 with the face's) and mounting (FUN_001b4950 from FUN_001b1b20: ALPHA1 on the
        // scaled world, ALPHA2 on body bone 7) are the game's.
        // alphaPacket is FUN_001b49a0 (0x1B49A0-0x1B52E4) operation by operation in its order (the recomp's FPU: plain
        // IEEE single, div.s by 0 an infinity, cvt.w.s truncating and saturating), with FUN_001b53d0's cull and sort
        // (centroid x 1536-2560, y 1824-2272, every z 0-1048574; far to near, ties in triangle order), but with no room
        // limit (the game's node list at 0x1E24D00 runs into its GIF buffer after 264 triangles).
        constexpr uint32_t kThird = 0x295394u;                       // gp - 0x71DC: the centroid's 1/3 (0x3EAAAA9F)
        constexpr uint32_t kCullX0 = 0x29A2F4u, kCullX1 = 0x29A2F8u; // gp - 0x227C / - 0x2278
        constexpr uint32_t kCullY0 = 0x29A2ECu, kCullY1 = 0x29A2F0u; // gp - 0x2284 / - 0x2280
        constexpr uint32_t kCullZ = 0x29A2E8u;                       // gp - 0x2288
        constexpr uint32_t kListHead = 0x25D178u, kListTail = 0x25D188u; // the sort list's end nodes (+0x04 their keys)
        constexpr uint32_t kAlphaPacket = 0x1E15300u;                // FUN_001b49a0's GIF packet
        constexpr uint32_t kAlphaDraw = 0x1B49A0u;
        constexpr uint32_t kAlpha1Posed = 9u; // the body's template count FUN_001b22e8 / 2620 set for ALPHA1's posing
        constexpr uint64_t kAlphaTagHigh = 0x312DC000ull; // the triangles' tag: PRE, PRIM 0x25B, PACKED, NREG 3
        constexpr uint64_t kAlphaRegs = 0x412ull;         // ST, RGBAQ, XYZF2

        float fpuDiv(float a, float b) { return b == 0.0f ? std::copysign(INFINITY, a * 0.0f) : a / b; }

        struct AlphaVertex // what FUN_001b49a0 writes into a vertex at + 0x30
        {
            float x, y, z, c[3];
        };

        // An ALPHA part's triangles as FUN_001b49a0 draws them: 3 quadwords a vertex (ST, RGBAQ, XYZF2: two words a
        // quadword) into `q`, far to near; `out` (if given) every vertex's results. Returns the triangles drawn.
        uint32_t alphaPacket(uint8_t *rdram, uint32_t part, uint32_t lightAt, uint32_t colourAt, std::vector<uint64_t> &q,
                             std::vector<AlphaVertex> *out = nullptr)
        {
            const uint8_t *p = mem(rdram, part);
            const uint32_t bones = word(p), vertices = word(p + 4), triangles = word(p + 8);
            const uint8_t *bone0 = p + 0x20, *vertex0 = bone0 + 0x190u * bones, *triangle0 = vertex0 + 0x48u * vertices;
            float L[16], C[16];
            std::memcpy(L, mem(rdram, lightAt), 64);
            std::memcpy(C, mem(rdram, colourAt), 64);
            std::vector<AlphaVertex> v(vertices);
            for (uint32_t i = 0; i < vertices; ++i)
            {
                const uint8_t *r = vertex0 + 0x48u * i;
                const float px = real(r), py = real(r + 4), pz = real(r + 8);
                const float n0 = real(r + 0xC), n1 = real(r + 0x10), n2 = real(r + 0x14);
                const int32_t a = static_cast<int32_t>(word(r + 0x20)), b = static_cast<int32_t>(word(r + 0x24));
                AlphaVertex &o = v[i];
                float d0, d1, d2;
                if (b == -1)
                {
                    // one bone (0x1B4A30-0x1B4C8C)
                    float M[16], N[16];
                    std::memcpy(M, bone0 + 0x190u * uint32_t(a) + 0x150u, 64);
                    std::memcpy(N, bone0 + 0x190u * uint32_t(a) + 0x110u, 64);
                    const float x = ((px * M[0] + py * M[4]) + pz * M[8]) + M[12];
                    const float y = ((px * M[1] + py * M[5]) + pz * M[9]) + M[13];
                    o.z = ((px * M[2] + py * M[6]) + pz * M[10]) + M[14];
                    const float w = ((px * M[3] + py * M[7]) + pz * M[11]) + M[15];
                    const float k = fpuDiv(1.0f, w);
                    o.y = y * k;
                    o.x = x * k;
                    const float m0 = (n0 * N[0] + n1 * N[4]) + n2 * N[8];
                    const float m1 = (n0 * N[1] + n1 * N[5]) + n2 * N[9];
                    const float m2 = (n0 * N[2] + n1 * N[6]) + n2 * N[10];
                    d0 = (m0 * L[0] + m1 * L[4]) + m2 * L[8];
                    d2 = (m0 * L[2] + m1 * L[6]) + m2 * L[10];
                    d1 = (m0 * L[1] + m1 * L[5]) + m2 * L[9];
                }
                else
                {
                    // two bones (0x1B4C90-0x1B5068): each bone's projected x, y times (1 / w) x its weight, z times
                    // its weight, then added; the normals blended the same way before the lights
                    const float wa = real(r + 0x28), wb = real(r + 0x2C);
                    float MA[16], MB[16], NA[16], NB[16];
                    std::memcpy(MA, bone0 + 0x190u * uint32_t(a) + 0x150u, 64);
                    std::memcpy(MB, bone0 + 0x190u * uint32_t(b) + 0x150u, 64);
                    std::memcpy(NA, bone0 + 0x190u * uint32_t(a) + 0x110u, 64);
                    std::memcpy(NB, bone0 + 0x190u * uint32_t(b) + 0x110u, 64);
                    float xa = ((px * MA[0] + py * MA[4]) + pz * MA[8]) + MA[12];
                    float ya = ((px * MA[1] + py * MA[5]) + pz * MA[9]) + MA[13];
                    float za = ((px * MA[2] + py * MA[6]) + pz * MA[10]) + MA[14];
                    za = za * wa;
                    const float wA = ((px * MA[3] + py * MA[7]) + pz * MA[11]) + MA[15];
                    const float ka = fpuDiv(1.0f, wA) * wa;
                    ya = ya * ka;
                    xa = xa * ka;
                    const float wB = ((px * MB[3] + py * MB[7]) + pz * MB[11]) + MB[15];
                    float xb = ((px * MB[0] + py * MB[4]) + pz * MB[8]) + MB[12];
                    float yb = ((px * MB[1] + py * MB[5]) + pz * MB[9]) + MB[13];
                    float zb = ((px * MB[2] + py * MB[6]) + pz * MB[10]) + MB[14];
                    const float kb = fpuDiv(1.0f, wB) * wb;
                    xb = xb * kb;
                    yb = yb * kb;
                    zb = zb * wb;
                    o.x = xa + xb;
                    o.y = ya + yb;
                    o.z = za + zb;
                    const float a0 = (n0 * NA[0] + n1 * NA[4]) + n2 * NA[8];
                    const float a1 = (n0 * NA[1] + n1 * NA[5]) + n2 * NA[9];
                    const float a2 = (n0 * NA[2] + n1 * NA[6]) + n2 * NA[10];
                    const float b0 = (n0 * NB[0] + n1 * NB[4]) + n2 * NB[8];
                    const float b1 = (n0 * NB[1] + n1 * NB[5]) + n2 * NB[9];
                    const float b2 = (n0 * NB[2] + n1 * NB[6]) + n2 * NB[10];
                    const float m0 = a0 * wa + b0 * wb;
                    const float m1 = a1 * wa + b1 * wb;
                    const float m2 = a2 * wa + b2 * wb;
                    d0 = (m0 * L[0] + m1 * L[4]) + m2 * L[8];
                    d2 = (m0 * L[2] + m1 * L[6]) + m2 * L[10];
                    d1 = (m0 * L[1] + m1 * L[5]) + m2 * L[9];
                }
                // c.lt.s with 0: a negative light term is 0 (a -0 stays)
                if (d0 < 0.0f)
                    d0 = 0.0f;
                if (d1 < 0.0f)
                    d1 = 0.0f;
                if (d2 < 0.0f)
                    d2 = 0.0f;
                for (int c = 0; c < 3; ++c)
                    o.c[c] = ((d0 * C[c] + d1 * C[4 + c]) + d2 * C[8 + c]) + C[12 + c];
            }
            if (out)
                *out = v;
            // FUN_001b53d0, one call a triangle: kept unless its centroid is off the screen's double or any z is outside
            // 0-1048574; inserted before the first node with a larger key (the centroid's z): a stable sort
            const float third = real(mem(rdram, kThird));
            const float x0 = real(mem(rdram, kCullX0)), x1 = real(mem(rdram, kCullX1));
            const float y0 = real(mem(rdram, kCullY0)), y1 = real(mem(rdram, kCullY1)), zMax = real(mem(rdram, kCullZ));
            const float headKey = real(mem(rdram, kListHead + 4u)), tailKey = real(mem(rdram, kListTail + 4u));
            struct Node
            {
                float key;
                uint32_t triangle;
            };
            std::vector<Node> kept;
            kept.reserve(triangles);
            for (uint32_t t = 0; t < triangles; ++t)
            {
                const uint8_t *tri = triangle0 + 12u * t;
                const AlphaVertex &a = v[word(tri)], &b = v[word(tri + 4)], &c = v[word(tri + 8)];
                const float cx = ((a.x + b.x) + c.x) * third;
                const float cz = ((a.z + b.z) + c.z) * third;
                const float cy = ((a.y + b.y) + c.y) * third;
                if (cx < x0 || x1 < cx || cy < y0 || y1 < cy)
                    continue;
                if (zMax < a.z || a.z < 0.0f || zMax < b.z || b.z < 0.0f || zMax < c.z || c.z < 0.0f)
                    continue;
                // past the list's end node (NaN, or not below its key): the game drops it; below the first node's key
                // (never: every z is >= 0 and its key is 0) it would be counted but not sent
                if (cz < headKey || !(cz < tailKey))
                    continue;
                kept.push_back({cz, t});
            }
            std::stable_sort(kept.begin(), kept.end(), [](const Node &l, const Node &r) { return l.key < r.key; });
            // the packet's vertices (0x1B51F0-0x1B52E4): ST (u, v, 1.0), RGBAQ (the colour truncated, alpha 0x80),
            // XYZF2 (x x 16, y x 16 truncated, z truncated << 4)
            q.reserve(q.size() + 18u * kept.size());
            for (const Node &n : kept)
            {
                const uint8_t *tri = triangle0 + 12u * n.triangle;
                for (int k = 0; k < 3; ++k)
                {
                    const uint32_t i = word(tri + 4 * k);
                    const uint8_t *r = vertex0 + 0x48u * i;
                    const AlphaVertex &o = v[i];
                    auto cvt = [](float f) { return static_cast<uint32_t>(ps2FpuCvtWS(f)); };
                    q.push_back(uint64_t(word(r + 0x18)) | (uint64_t(word(r + 0x1C)) << 32));
                    q.push_back(0x3F800000ull);
                    q.push_back(uint64_t(cvt(o.c[0])) | (uint64_t(cvt(o.c[1])) << 32));
                    q.push_back(uint64_t(cvt(o.c[2])) | (0x80ull << 32));
                    q.push_back(uint64_t(cvt(o.x * 16.0f)) | (uint64_t(cvt(o.y * 16.0f)) << 32));
                    q.push_back(uint64_t(cvt(o.z) << 4));
                }
            }
            return static_cast<uint32_t>(kept.size());
        }

        // A mod's see-through part drawn in FUN_001b2088's place: the packet FUN_001b49a0 would send (TEX0, then the
        // triangles, PRIM 0x25B), with the skin of the mod's body (its TEX0 and host image, as its own triangles have) and
        // the GS state the game has set for its see-through parts (ALPHA_2 0x44, TEST_2 0x5001B)
        void drawAlpha(uint8_t *rdram, uint32_t part, uint32_t rig, const SeeThroughList &list)
        {
            std::vector<uint64_t> q;
            const uint32_t n = alphaPacket(rdram, part, word(mem(rdram, rig + kRigLights)), word(mem(rdram, rig + kRigColours)), q);
            const uint32_t context = list.tex0Reg == 0x06u ? 0u : 1u;
            Packet packet;
            packet.ad(list.tex0Reg, list.tex0);
            const uint32_t tbp = static_cast<uint32_t>(list.tex0) & 0x3FFFu;
            if (list.image)
                packet.ad(0x71u, uint64_t(tbp) | (uint64_t(list.image) << 32));
            const uint64_t high = context ? kAlphaTagHigh : kAlphaTagHigh & ~(0x200ull << 15); // PRIM's CTXT
            size_t i = 0;
            do
            {
                // one tag a run of triangles (NLOOP has 15 bits; the game's own parts fit one)
                const size_t run = std::min<size_t>(n - i, 0x2000u);
                packet.tag(uint64_t(3u * run) | (high << 32), kAlphaRegs);
                packet.q.insert(packet.q.end(), q.begin() + 18 * i, q.begin() + 18 * (i + run));
                i += run;
                if (packet.q.size() * 8u >= kPacketBytes)
                    send(packet);
            } while (i < n);
            if (list.image)
                packet.ad(0x71u, uint64_t(tbp)); // unbound
            send(packet);
            g_stats.seeThrough += n;
        }

        // VENDETTA_SEETHROUGH_CHECK=1: FUN_001b49a0 hooked; ours worked out before the game's call, its packet and the
        // vertices it wrote compared after
        PS2Runtime::RecompiledFunction g_originalAlphaDraw = nullptr;
        bool g_alphaCheck = false;
        struct
        {
            uint64_t parts = 0, same = 0, different = 0, triangles = 0, vertices = 0, verticesDifferent = 0;
            int reports = 0;
            Clock::time_point last = Clock::now();
        } g_alphaChecked;

        void alphaCheckHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t part = getRegU32(ctx, 4), lights = getRegU32(ctx, 5), colours = getRegU32(ctx, 6);
            const uint32_t ra = getRegU32(ctx, 31), sp = getRegU32(ctx, 29);
            const uint8_t *p = mem(rdram, part);
            // ours in the game's form: tag (A+D), TEX0_2 from the part, the triangles' tag (EOP), the vertices
            std::vector<uint64_t> ours = {0x1000000000000001ull, 0xEull,
                                          uint64_t(word(p + 0xC)) | (uint64_t(word(p + 0x10)) << 32), 7ull, 0ull, kAlphaRegs};
            std::vector<AlphaVertex> outs;
            const uint32_t n = alphaPacket(rdram, part, lights, colours, ours, &outs);
            ours[4] = uint64_t(3u * n) | 0x8000ull | (kAlphaTagHigh << 32);
            g_originalAlphaDraw(rdram, ctx, runtime);
            if (ctx->pc != ra || getRegU32(ctx, 29) != sp)
                return; // the call was left early (a scheduler checkpoint): not compared
            const uint32_t bones = word(p), vertices = word(p + 4);
            const uint8_t *game = mem(rdram, kAlphaPacket);
            const bool same = std::memcmp(ours.data(), game, ours.size() * 8u) == 0;
            uint32_t vertexDiffs = 0;
            for (uint32_t i = 0; i < vertices; ++i)
                vertexDiffs += std::memcmp(&outs[i], p + 0x20 + 0x190u * bones + 0x48u * i + 0x30, sizeof(AlphaVertex)) != 0;
            ++g_alphaChecked.parts;
            ++(same ? g_alphaChecked.same : g_alphaChecked.different);
            g_alphaChecked.triangles += n;
            g_alphaChecked.vertices += vertices;
            g_alphaChecked.verticesDifferent += vertexDiffs;
            if ((!same || vertexDiffs) && g_alphaChecked.reports < 6)
            {
                ++g_alphaChecked.reports;
                size_t at = 0;
                while (at < ours.size() && std::memcmp(&ours[at], game + 8u * at, 8) == 0)
                    ++at;
                uint64_t theirs = 0;
                if (at < ours.size())
                    std::memcpy(&theirs, game + 8u * at, 8);
                std::cerr << "[wrestlers] see-through check: part 0x" << std::hex << part << ": first different word " << std::dec
                          << at << " of " << ours.size() << " (ours 0x" << std::hex << (at < ours.size() ? ours[at] : 0ull)
                          << ", the game's 0x" << theirs << std::dec << "), " << vertexDiffs << " of " << vertices
                          << " vertices different" << std::endl;
            }
            if (Clock::now() - g_alphaChecked.last >= std::chrono::seconds(5))
            {
                g_alphaChecked.last = Clock::now();
                std::cerr << "[wrestlers] see-through check, 5 s: " << g_alphaChecked.parts << " parts (" << g_alphaChecked.triangles
                          << " triangles): " << g_alphaChecked.same << " packets the same as the game's byte for byte, "
                          << g_alphaChecked.different << " different; vertices: " << g_alphaChecked.vertices << ", "
                          << g_alphaChecked.verticesDifferent << " different" << std::endl;
                const int reports = g_alphaChecked.reports;
                g_alphaChecked = {};
                g_alphaChecked.reports = reports;
            }
        }

        GSGpuRenderer *gpu() { return g_runtime ? g_runtime->gs().gpuRendererForSettings() : nullptr; }

        // a part's offset in a WRS (self-relative entries at + 0x10 + 4 n, -1 = none), or -1
        int32_t slotOffset(const uint8_t *wrs, size_t size, int slot)
        {
            const uint32_t at = 0x10u + 4u * static_cast<uint32_t>(slot);
            if (at + 4u > size)
                return -1;
            const int32_t v = static_cast<int32_t>(word(wrs + at));
            if (v == -1)
                return -1;
            const int64_t o = int64_t(at) + v;
            return o > 0 && o < int64_t(size) ? static_cast<int32_t>(o) : -1;
        }

        // The skin from the file's upload packet (slot 18: the T8 picture sent as CT32, then its CLUT), decoded as the
        // GS reads it; or textures/skin.png. Made a host image. False: none (the model behind's skin shows).
        bool makeSkin(ModModel &m, const vendetta_wrestlers::ModelFile &files)
        {
            GSGpuRenderer *renderer = gpu();
            if (!renderer)
                return false;
            std::vector<uint32_t> rgba;
            uint32_t w = 0, h = 0;
            std::error_code ec;
            if (!files.textures.empty() && std::filesystem::is_regular_file(files.textures / "skin.png", ec))
            {
                std::ifstream in(files.textures / "skin.png", std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (gsDecodePicture(bytes.data(), bytes.size(), 0, 0, rgba, w, h))
                    for (uint32_t &c : rgba) // PNG alpha (255 = solid) to the GS's (128 = solid)
                        c = (c & 0x00FFFFFFu) | ((((c >> 24) * 128u + 127u) / 255u) << 24);
                else
                    vendetta_mods::problem() << "[wrestlers] " << files.modName << ": " << files.wrestler
                                             << ": textures\\skin.png can't be read as a picture: the model file's own skin is used"
                                             << std::endl;
            }
            if (rgba.empty())
            {
                const uint8_t *t = m.at(kTexture);
                if (!t)
                    return false;
                const uint8_t *packet = t + (word(t) & ~3u);
                const uint32_t bytes = word(t + 4);
                if (packet + bytes > m.file.data() + m.file.size())
                    return false;
                // the uploads (the picture, then its CLUT) sent to the addresses the game gives them (slot 0: BITBLTBUF's
                // DBP, word + 0x14, as FUN_001b8568 patches it), in a copy
                std::vector<uint8_t> copy(packet, packet + bytes);
                const uint32_t dbp[2] = {0x1800u, 0x2000u};
                uint32_t uploads = 0;
                for (uint32_t o = 0; o + 0x60u <= bytes && uploads < 2u; ++uploads)
                {
                    const uint32_t w = (word(copy.data() + o + 0x14) & ~0x3FFFu) | dbp[uploads];
                    std::memcpy(copy.data() + o + 0x14, &w, 4);
                    o += 0x60u + (word(copy.data() + o + 0x50) & 0x7FFFu) * 16u;
                }
                if (uploads < 2u)
                    return false;
                GsScratch scratch;
                scratch.replay(copy.data(), bytes);
                const uint64_t tex0 = uint64_t(dbp[0]) | (8ull << 14) | (0x13ull << 20) | (9ull << 26) | (9ull << 30) |
                                      (1ull << 34) | (uint64_t(dbp[1]) << 37);
                if (!scratch.decode(tex0, rgba, w, h))
                    return false;
            }
            // Pixels not solid (alpha < kSolidAlpha) marked on the grid (their triangles drawn again see-through); the
            // colour hidden behind fully clear ones (often white) taken from the nearest pixel with any colour, so the
            // bilinear read at a hole's edge doesn't mix it in as a fringe
            {
                m.softGrid.assign(kSoftGrid * kSoftGrid, 0);
                std::vector<uint32_t> queue;
                std::vector<uint8_t> filled(rgba.size(), 0);
                for (uint32_t i = 0; i < rgba.size(); ++i)
                {
                    const uint32_t alpha = rgba[i] >> 24;
                    if (alpha < kSolidAlpha)
                    {
                        m.cutout = true;
                        m.softGrid[(i / w) * kSoftGrid / h * kSoftGrid + (i % w) * kSoftGrid / w] = 1;
                    }
                    if (alpha > 0u)
                    {
                        filled[i] = 1;
                        queue.push_back(i);
                    }
                }
                if (queue.size() < rgba.size() && !queue.empty())
                    for (size_t q = 0; q < queue.size(); ++q)
                    {
                        const uint32_t i = queue[q], x = i % w, y = i / w;
                        const uint32_t next[4] = {x > 0 ? i - 1 : i, x + 1 < w ? i + 1 : i, y > 0 ? i - w : i, y + 1 < h ? i + w : i};
                        for (uint32_t n : next)
                            if (!filled[n])
                            {
                                filled[n] = 1;
                                rgba[n] = (rgba[n] & 0xFF000000u) | (rgba[i] & 0x00FFFFFFu);
                                queue.push_back(n);
                            }
                    }
            }
            // VENDETTA_MODSKIN_DUMP=<file>: the skin as it is made (u32 width, u32 height, then RGBA8, the GS's alpha)
            if (const char *dump = std::getenv("VENDETTA_MODSKIN_DUMP"); dump && *dump)
            {
                std::ofstream out(dump, std::ios::binary);
                out.write(reinterpret_cast<const char *>(&w), 4);
                out.write(reinterpret_cast<const char *>(&h), 4);
                out.write(reinterpret_cast<const char *>(rgba.data()), static_cast<std::streamsize>(rgba.size() * 4u));
            }
            m.image = g_nextImage++;
            renderer->setHostImage(m.image, w, h, std::move(rgba));
            return true;
        }

        // Everything drawPart relies on in a part, checked once when a mod's model is read, so a model that is read can't
        // fail in the middle of a match (by then its skeleton is in the model behind): the batches, the setup rows drawPart
        // reads (the tags, TEX0 and the colours, VU 0x39-0x3F), each vertex's matrix palette inside the setup rows, and
        // every strip's and loose triangle's vertex address inside its batch
        bool validatePart(const uint8_t *p, const uint8_t *end)
        {
            const uint8_t *stream = p + (word(p) & ~3u);
            const uint32_t bytes = word(p + 4);
            uint32_t setupRows = 0;
            std::vector<Batch> batches;
            if (bytes == 0u || bytes > kMaxStreamBytes || stream < p || stream + bytes > end ||
                !readStream(stream, bytes, setupRows, batches) || setupRows < 7u)
                return false;
            auto inSetup = [&](uint32_t address) { // a palette: 8 rows from its address
                address &= 0xFFFFu;
                return address >= 0x39u && address + 8u <= 0x39u + setupRows;
            };
            for (const Batch &b : batches)
            {
                if (b.rows < 1u || uint64_t(b.at) + 16ull * b.rows > bytes)
                    return false;
                const uint8_t *rows = stream + b.at;
                const uint32_t one = word(rows) & 0xFFFFu, two = word(rows + 4) & 0xFFFFu, strips = word(rows + 8) & 0xFFFFu;
                const uint32_t loose = word(rows + 12) & 0x7FFFu;
                const uint64_t vertexRows = 3ull * one + 4ull * two;
                if (1ull + vertexRows > b.rows)
                    return false;
                uint32_t row = 1;
                for (uint32_t v = 0; v < one + two; ++v)
                {
                    const uint8_t *r = rows + 16u * row;
                    if (v < one ? !inSetup(word(r + 12)) : !inSetup(word(r + 48)) || !inSetup(word(r + 56)))
                        return false;
                    row += v < one ? 3u : 4u;
                }
                const uint32_t words = (b.rows - 1u - static_cast<uint32_t>(vertexRows)) * 4u;
                const uint8_t *idx = rows + 16u * (1u + static_cast<uint32_t>(vertexRows));
                auto vertexOk = [&](uint32_t address) {
                    const uint32_t r = (address & 0xFFFFu) - b.top;
                    return r >= 1u && r < b.rows;
                };
                uint64_t k = 0;
                for (uint32_t s = 0; s < strips; ++s)
                {
                    if (k >= words)
                        return false;
                    const uint32_t count = word(idx + 4u * k++);
                    if (k + count > words || count < 3u)
                        return false;
                    for (uint32_t j = 0; j < count; ++j)
                        if (!vertexOk(word(idx + 4u * (k + j))))
                            return false;
                    k += count;
                }
                if (k + 3ull * loose > words)
                    return false;
                for (uint64_t t = 0; t < 3ull * loose; ++t)
                    if (!vertexOk(word(idx + 4u * (k + t))))
                        return false;
            }
            return true;
        }

        // A part's bones as the game walks them (FUN_001b95f0: from bone 0, each bone's child (+0x00) then its sibling
        // (+0x04), negative = none) and as our draw writes their rows (writeRows: segment +0x08, its 0x80 bytes at stream
        // + 0x80 + 0x80 x segment): every bone reached once, every link and row inside the part, the walk not deeper
        // than kWalkDepth. nullptr, or what is wrong.
        const char *checkSkeleton(const uint8_t *p, const uint8_t *end)
        {
            const uint32_t bones = word(p + 8);
            if (uint64_t(0x10u) + 0x190ull * bones > uint64_t(end - p))
                return "its bones run past the file";
            const uint32_t streamBytes = word(p + 4);
            std::vector<uint8_t> seen(bones, 0);
            std::vector<std::pair<uint32_t, uint32_t>> stack{{0u, 1u}}; // bone, depth
            uint32_t reached = 0;
            while (!stack.empty())
            {
                const auto [b, depth] = stack.back();
                stack.pop_back();
                if (b >= bones)
                    return "a bone links to a bone it doesn't have";
                if (seen[b]++)
                    return "its bones link in a loop";
                if (depth > kWalkDepth)
                    return "its bones nest deeper than the game's walk allows (512)";
                ++reached;
                const uint8_t *bone = p + 0x10 + 0x190 * b;
                const int32_t segment = static_cast<int32_t>(word(bone + 8));
                if (segment >= 0 && 0x100ull + 0x80ull * uint32_t(segment) > streamBytes)
                    return "a bone's matrix rows lie outside its stream";
                for (int link = 0; link < 2; ++link)
                    if (const int32_t next = static_cast<int32_t>(word(bone + 4 * link)); next >= 0)
                        stack.push_back({static_cast<uint32_t>(next), depth + 1u});
            }
            return reached == bones ? nullptr : "some bones aren't linked to the rest";
        }

        // The parent of each bone (from the child / sibling links; -1 = the root's level)
        std::vector<int32_t> parents(const uint8_t *p, uint32_t bones)
        {
            std::vector<int32_t> parent(bones, -1);
            for (uint32_t b = 0; b < bones; ++b)
            {
                int32_t c = static_cast<int32_t>(word(p + 0x10 + 0x190 * b));
                for (uint32_t guard = 0; c >= 0 && uint32_t(c) < bones && guard < bones; ++guard)
                {
                    parent[uint32_t(c)] = static_cast<int32_t>(b);
                    c = static_cast<int32_t>(word(p + 0x10 + 0x190 * uint32_t(c) + 4));
                }
            }
            return parent;
        }

        // A mod's see-through part (ALPHA1 slot 6 / ALPHA2 slot 7) checked whole, so the game's posing and mounting and
        // our draw stay inside it: its bones, vertices and triangles inside the part; every bone link -1 or one of its
        // bones (FUN_001b4880 follows any other value), every bone reached once from bone 0, no deeper than the walk
        // allows; every vertex's bones its own (B -1: one bone), every triangle's vertices its own; ALPHA1 at least the
        // 9 bones the body's template poses in it. nullptr, or what is wrong.
        const char *checkAlpha(const uint8_t *p, uint32_t size, int slot)
        {
            if (size < 0x20u)
                return "it is cut short";
            const uint32_t bones = word(p), vertices = word(p + 4), triangles = word(p + 8);
            if (bones == 0u || 0x20ull + 0x190ull * bones + 0x48ull * vertices + 12ull * triangles > size)
                return "its data runs past the part";
            if (slot == kAlpha1 && bones < kAlpha1Posed)
                return "it has fewer bones than the 9 the game poses in it";
            std::vector<uint8_t> seen(bones, 0);
            std::vector<std::pair<uint32_t, uint32_t>> stack{{0u, 1u}};
            uint32_t reached = 0;
            while (!stack.empty())
            {
                const auto [b, depth] = stack.back();
                stack.pop_back();
                if (seen[b]++)
                    return "its bones link in a loop";
                if (depth > kWalkDepth)
                    return "its bones nest deeper than the game's walk allows (512)";
                ++reached;
                for (int link = 0; link < 2; ++link)
                {
                    const int32_t next = static_cast<int32_t>(word(p + 0x20 + 0x190 * b + 4 * link));
                    if (next == -1)
                        continue;
                    if (next < 0 || uint32_t(next) >= bones)
                        return "a bone links to a bone it doesn't have";
                    stack.push_back({static_cast<uint32_t>(next), depth + 1u});
                }
            }
            if (reached != bones)
                return "some bones aren't linked to the rest";
            const uint8_t *vertex0 = p + 0x20 + 0x190u * bones;
            for (uint32_t i = 0; i < vertices; ++i)
            {
                const int32_t a = static_cast<int32_t>(word(vertex0 + 0x48u * i + 0x20));
                const int32_t b = static_cast<int32_t>(word(vertex0 + 0x48u * i + 0x24));
                if (a < 0 || uint32_t(a) >= bones || (b != -1 && (b < 0 || uint32_t(b) >= bones)))
                    return "a vertex hangs from a bone it doesn't have";
            }
            const uint8_t *triangle0 = vertex0 + 0x48u * vertices;
            for (uint32_t k = 0; k < 3u * triangles; ++k)
                if (word(triangle0 + 4u * k) >= vertices)
                    return "a triangle uses a vertex it doesn't have";
            return nullptr;
        }

        // ---- the model behind: one whose rig has the mod's parents

        // A model's rig: per part (BODY, FACE, the hands) the parents of the game's rig's bones (42, 11, 13, 13); a part
        // missing, or with fewer bones, left empty
        struct Rig
        {
            std::vector<int32_t> parents[6];
        };

        // The rig read from a WRS (`read(offset, bytes, to)` reads its bytes). False: not a WRS.
        template <class Read>
        bool readRig(Read read, Rig &rig)
        {
            uint8_t head[0x90];
            if (!read(0u, 0x90u, head) || std::memcmp(head, "WRS ", 4) != 0)
                return false;
            for (int slot : {kBody, kFace, kLeftHand, kRightHand})
            {
                const int32_t w = static_cast<int32_t>(word(head + 0x10 + 4 * slot));
                const int64_t at = 0x10 + 4 * slot + int64_t(w);
                uint8_t top[0x10];
                if (w == -1 || at <= 0 || at > 0x10000000 || !read(uint32_t(at), 0x10u, top))
                    continue;
                const uint32_t bones = word(top + 8);
                if (bones < standardBones(slot) || bones > 0x10000u)
                    continue;
                std::vector<uint8_t> part(0x10u + 0x190u * bones);
                if (!read(uint32_t(at), uint32_t(part.size()), part.data()))
                    continue;
                rig.parents[slot] = parents(part.data(), bones);
                rig.parents[slot].resize(standardBones(slot));
            }
            return true;
        }

        // The check pointSlots makes: every part both have, the game's rig's bones hanging from the same bones
        bool sameRig(const Rig &mod, const Rig &game)
        {
            for (int slot : {kBody, kFace, kLeftHand, kRightHand})
                if (!mod.parents[slot].empty() && !game.parents[slot].empty() && mod.parents[slot] != game.parents[slot])
                    return false;
            return true;
        }

        // A game model's rig, by its DATA.BIN entry (read from the game's DATA\DATA.BIN through the table at 0x256F40,
        // as FUN_00165838 loads it); nullptr: not readable
        const Rig *gameRig(uint8_t *rdram, uint32_t entry)
        {
            static std::unordered_map<uint32_t, std::unique_ptr<Rig>> cache;
            if (const auto found = cache.find(entry); found != cache.end())
                return found->second.get();
            std::unique_ptr<Rig> rig;
            if (entry > 0u && entry < kDataEntries)
            {
                const uint32_t sector = word(mem(rdram, kDataTable + 8u * entry)), sectors = word(mem(rdram, kDataTable + 8u * entry + 4u));
                std::ifstream in(PS2Runtime::getIoPaths().elfDirectory / "DATA" / "DATA.BIN", std::ios::binary);
                auto read = [&](uint32_t offset, uint32_t bytes, uint8_t *to) {
                    if (!in.is_open() || uint64_t(offset) + bytes > uint64_t(sectors) * 2048u)
                        return false;
                    in.clear();
                    in.seekg(static_cast<std::streamoff>(uint64_t(sector) * 2048u + offset));
                    in.read(reinterpret_cast<char *>(to), bytes);
                    return static_cast<bool>(in);
                };
                auto made = std::make_unique<Rig>();
                if (readRig(read, *made))
                    rig = std::move(made);
            }
            return (cache[entry] = std::move(rig)).get();
        }

        // "Briggs'" / "Capone's": the game wrestler whose model a DATA.BIN entry is (the first record wearing it)
        std::string modelOwner(uint8_t *rdram, uint32_t entry)
        {
            for (uint32_t record = 0; record < kGameModelRecords; ++record)
                if (word(mem(rdram, kGameCharacters + kRecordSize * record + kRecordModel)) == entry)
                {
                    std::string name = vendetta_wrestlers::gameFullName(record);
                    for (size_t i = 0; i < name.size(); ++i) // BRIGGS -> Briggs, GHOSTFACE KILLAH -> Ghostface Killah
                        if (i > 0 && std::isalpha(static_cast<unsigned char>(name[i - 1])) && name[i - 1] != '\'')
                            name[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
                    if (!name.empty())
                        return name + (name.back() == 's' || name.back() == 'S' ? "'" : "'s");
                }
            return "the game's";
        }

        // At start: each record with a mod model of the game's format gets a model behind whose rig has the mod's
        // parents (the record's + 0x1C, which FUN_00165838 loads at each match): the one it has (a replaced attire's own,
        // a new attire's Briggs') when it matches, else Briggs', else the first game model in record order that does
        // (Capone's for a model built on his rig). None matches: it stays, and the match says so (pointSlots).
        void chooseModelsBehind(uint8_t *rdram)
        {
            for (uint32_t record = 0; record < vendetta_wrestlers::endRecord(); ++record)
            {
                const vendetta_wrestlers::ModelFile *files = vendetta_wrestlers::modelFile(record);
                if (!files || files->model.extension() != ".wrs")
                    continue;
                std::ifstream in(files->model, std::ios::binary);
                const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                Rig mod;
                auto read = [&](uint32_t offset, uint32_t bytes, uint8_t *to) {
                    if (uint64_t(offset) + bytes > file.size())
                        return false;
                    std::memcpy(to, file.data() + offset, bytes);
                    return true;
                };
                if (!readRig(read, mod))
                    continue; // not readable: the match's read says what is wrong
                const uint32_t field = vendetta_store::kCharacterTable + kRecordSize * record + kRecordModel;
                const uint32_t now = word(mem(rdram, field));
                const Rig *own = gameRig(rdram, now);
                if (!own || sameRig(mod, *own))
                    continue;
                uint32_t found = 0;
                for (uint32_t r = 0; r < kGameModelRecords && !found; ++r)
                {
                    const uint32_t entry = word(mem(rdram, kGameCharacters + kRecordSize * r + kRecordModel));
                    if (entry != 0u && entry != now)
                        if (const Rig *rig = gameRig(rdram, entry); rig && sameRig(mod, *rig))
                            found = entry;
                }
                const std::string what = "[wrestlers] " + files->modName + ": " + files->wrestler + " (record " +
                                         std::to_string(record) + "): " + files->model.filename().string();
                if (!found)
                {
                    std::cerr << what << ": its rig is no game model's (" << modelOwner(rdram, now) << " model stays behind it)"
                              << std::endl;
                    continue;
                }
                std::memcpy(mem(rdram, field), &found, 4);
                const std::string owner = modelOwner(rdram, found);
                std::cerr << what << ": its rig matches " << owner << " model, not " << modelOwner(rdram, now) << ": " << owner
                          << " model stands behind it" << std::endl;
            }
        }

        // A template's keyed channels (its bones not skipped: rotation, position), or ~0u when it isn't one (a u32 count
        // of 32-byte bones {flags, rotation, position}: flags 1 rotation keyed, 2 position keyed, 4 skipped)
        uint32_t templateChannels(const uint8_t *t, uint32_t size, uint32_t least, uint32_t most)
        {
            if (!t || size < 4u || word(t) < least || word(t) > most || 4ull + 32ull * word(t) > size)
                return ~0u;
            uint32_t channels = 0;
            for (uint32_t k = 0; k < word(t); ++k)
            {
                const uint32_t flags = word(t + 4u + 32u * k);
                if (!(flags & 4u))
                    channels += (flags & 1u) + ((flags >> 1) & 1u);
            }
            return channels;
        }

        // Whether a motion (the clip header {u16 last pose, u8 n6}, n6 x 6 bytes, then per keyed channel of its template a
        // u8 key count and 8-byte keys) ends inside its `size` bytes
        bool motionFits(const uint8_t *motion, uint32_t size, const uint8_t *t)
        {
            if (!motion || size < 3u)
                return false;
            uint64_t p = 3u + 6u * uint64_t(motion[2]);
            for (uint32_t k = 0; k < word(t); ++k)
            {
                const uint32_t flags = word(t + 4u + 32u * k);
                if (flags & 4u)
                    continue;
                for (uint32_t bit = 1u; bit <= 2u; bit <<= 1)
                    if (flags & bit)
                    {
                        if (p >= size)
                            return false;
                        p += 1u + 8u * uint64_t(motion[p]);
                    }
            }
            return p <= size;
        }

        // Which of slots 1-17 the game may read from the mod's file: its parts (checked whole when read); each template's
        // count from the game's rig's bones to its part's (the game applies a template to the part bone by bone) and its
        // channels within the game's sample buffers (the body's counted over the rig's 42: its copy keeps only those, the
        // clips key nothing more), each motion whole over its template (a motion and its template only together: the keys
        // follow the template's channels). The rest stays the model behind's, with a line in mods.log.
        void checkPointed(ModModel &m)
        {
            for (int slot : {kBody, kFace, kLeftHand, kRightHand})
                m.pointable[slot] = m.at(slot) != nullptr;
            const struct { int slot, motion; const char *name; } templates[] = {
                {8, -1, "the body's template"}, {9, 14, "the face expressions"}, {10, 15, "the lip shapes"},
                {11, -1, "the eyes' template"}, {12, 16, "the left hand's poses"}, {13, 17, "the right hand's poses"}};
            for (const auto &t : templates)
            {
                const uint8_t *tp = m.at(t.slot);
                const uint8_t *part = m.at(partOfTemplate(t.slot));
                if (!tp || !part)
                    continue;
                uint32_t channels = templateChannels(tp, m.partSize[t.slot], standardBones(t.slot), word(part + 8));
                if (t.slot == 8 && channels != ~0u)
                {
                    channels = 0;
                    for (uint32_t k = 0; k < kBodyBones; ++k)
                        if (const uint32_t flags = word(tp + 4u + 32u * k); !(flags & 4u))
                            channels += (flags & 1u) + ((flags >> 1) & 1u);
                }
                const char *wrong = channels == ~0u ? "its template is damaged"
                                    : channels > kChannelRoom ? "its template keys more channels than the game samples (25)"
                                    : t.motion >= 0 && !m.at(t.motion) ? nullptr
                                    : t.motion >= 0 && !motionFits(m.at(t.motion), m.partSize[t.motion], tp) ? "its motion is damaged"
                                    : nullptr;
                if (wrong)
                {
                    vendetta_mods::problem() << m.what << ": " << t.name << ": " << wrong << ": " << m.behind << " are used" << std::endl;
                    continue;
                }
                if (t.motion >= 0 && !m.at(t.motion))
                    continue; // a template without its motion: both the model behind's
                m.pointable[t.slot] = true;
                if (t.motion >= 0)
                    m.pointable[t.motion] = true;
            }
        }

        // A mod's model read and checked: the game's format (WRS); glTF to come. Anything wrong: a line in mods.log and
        // the model behind it (`behind`: "Briggs'", or whose it is) is used instead, the whole match.
        std::unique_ptr<ModModel> loadModel(const vendetta_wrestlers::ModelFile &files, const std::string &behind)
        {
            auto m = std::make_unique<ModModel>();
            m->behind = behind;
            const std::string what = "[wrestlers] " + files.modName + ": " + files.wrestler + ": " + files.model.filename().string();
            const std::string instead = ": " + behind + " model is used instead";
            if (files.model.extension() != ".wrs")
            {
                vendetta_mods::problem() << what << ": glTF models are not read yet" << instead << std::endl;
                return m;
            }
            std::ifstream in(files.model, std::ios::binary);
            m->file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (m->file.size() < 0x100u || std::memcmp(m->file.data(), "WRS ", 4) != 0)
            {
                vendetta_mods::problem() << what << (m->file.empty() ? ": can't be read" : ": not a model in the game's format (WRS)")
                                         << instead << std::endl;
                return m;
            }
            for (int n = 0; n < 32; ++n)
                m->part[n] = slotOffset(m->file.data(), m->file.size(), n);
            for (int n = 0; n < 32; ++n)
            {
                if (m->part[n] < 0)
                    continue;
                int64_t end = static_cast<int64_t>(m->file.size());
                for (int k = 0; k < 32; ++k)
                    if (m->part[k] > m->part[n])
                        end = std::min<int64_t>(end, m->part[k]);
                m->partSize[n] = static_cast<uint32_t>(end - m->part[n]);
            }
            m->what = what;
            // the parts our draw uses: the body (required), the face and the hands (if there); at least the game's rig's
            // bones, in its order (the clips, the body points, the mounts, the eyes and lips use those), any more after them
            const struct { int slot; uint32_t bones; const char *name; bool required; } parts[] = {
                {kBody, kBodyBones, "the body (BODY)", true}, {kFace, kFaceBones, "the face", false},
                {kLeftHand, kHandBones, "the left hand", false}, {kRightHand, kHandBones, "the right hand", false}};
            for (const auto &part : parts)
            {
                const uint8_t *p = m->at(part.slot);
                if (!p)
                {
                    if (part.required)
                    {
                        vendetta_mods::problem() << what << ": it has no " << part.name << instead << std::endl;
                        return m;
                    }
                    continue;
                }
                if (word(p + 8) < part.bones)
                {
                    vendetta_mods::problem() << what << ": " << part.name << " has " << word(p + 8) << " bones, fewer than the "
                                             << "game's rig's " << part.bones << instead << std::endl;
                    return m;
                }
                if (!validatePart(p, m->file.data() + m->file.size()))
                {
                    vendetta_mods::problem() << what << ": " << part.name << " is damaged (its data runs past the file or points "
                                             << "outside itself)" << instead << std::endl;
                    return m;
                }
                if (const char *wrong = checkSkeleton(p, m->file.data() + m->file.size()))
                {
                    vendetta_mods::problem() << what << ": " << part.name << ": " << wrong << instead << std::endl;
                    return m;
                }
            }
            checkPointed(*m);
            // its own see-through parts, when it has them: posed and mounted by the game's code, drawn by ours
            for (int slot : {kAlpha1, kAlpha2})
                if (const uint8_t *a = m->at(slot))
                {
                    if (const char *wrong = checkAlpha(a, m->partSize[slot], slot))
                        vendetta_mods::problem() << what << ": its see-through part " << (slot == kAlpha1 ? "ALPHA1" : "ALPHA2")
                                                 << ": " << wrong << ": not drawn" << std::endl;
                    else
                        m->pointable[slot] = true;
                }
            if (gpu() && !makeSkin(*m, files)) // (the CPU renderer has no host images: the model behind's skin there)
            {
                vendetta_mods::problem() << what << ": no skin can be read (none in textures\\ and the file's own is damaged)"
                                         << instead << std::endl;
                return m;
            }
            m->ok = true;
            std::cerr << what << ": read (" << m->file.size() / 1024 << " KB)" << std::endl;
            return m;
        }

        // The mod model of fighter i (its record's own model), read on first use; nullptr: none or unusable
        ModModel *modelOf(uint8_t *rdram, uint32_t i)
        {
            if (i >= 4u || g_modGameDraw)
                return nullptr;
            const uint32_t record = word(mem(rdram, kFighters + kFighterSize * i + kFighterRecord));
            const vendetta_wrestlers::ModelFile *files = vendetta_wrestlers::modelFile(record);
            if (!files)
                return nullptr;
            auto found = g_models.find(record);
            if (found == g_models.end())
                found = g_models.emplace(record, loadModel(*files, modelOwner(rdram, word(mem(rdram, vendetta_store::kCharacterTable +
                                                                                          kRecordSize * record + kRecordModel)))))
                            .first;
            return found->second->ok ? found->second.get() : nullptr;
        }

        // Models of records no longer in the match let go (and their skins)
        void dropUnused(uint8_t *rdram)
        {
            uint32_t records[4];
            for (uint32_t i = 0; i < 4u; ++i)
                records[i] = word(mem(rdram, kFighters + kFighterSize * i + kFighterRecord));
            if (std::equal(records, records + 4, g_lastRecords))
                return;
            std::copy(records, records + 4, g_lastRecords);
            for (auto it = g_models.begin(); it != g_models.end();)
            {
                if (std::find(records, records + 4, it->first) == records + 4)
                {
                    if (it->second->image)
                        if (GSGpuRenderer *renderer = gpu())
                            renderer->dropHostImage(it->second->image);
                    it = g_models.erase(it);
                }
                else
                    ++it;
            }
        }

        // The model behinds whose slot words point at a mod's blocks: the words as the game had them and as written
        struct Pointed
        {
            uint32_t original[32] = {}, written[32] = {};
            bool set[32] = {};
        };
        std::unordered_map<uint32_t, Pointed> g_pointed; // by the model behind's guest address

        uint32_t slotField(uint32_t model, int slot) { return model + 0x10u + 4u * static_cast<uint32_t>(slot); }

        // The model behind's slot words as the game had them (the ones still ours: a model loaded again keeps its own)
        void pointBack(uint32_t carrier)
        {
            const auto found = g_pointed.find(carrier);
            if (found == g_pointed.end())
                return;
            for (int slot = kFirstPointed; slot <= kLastPointed; ++slot)
                if (found->second.set[slot])
                {
                    uint8_t *field = vendetta_store::host(slotField(carrier, slot));
                    if (word(field) == found->second.written[slot])
                        std::memcpy(field, &found->second.original[slot], 4);
                }
            g_pointed.erase(found);
        }

        // The match area emptied: every model behind back to its own blocks before anything reads them, the mods' placed
        // again at their next picture
        void pointAllBack()
        {
            while (!g_pointed.empty())
                pointBack(g_pointed.begin()->first);
            for (auto &entry : g_models)
                std::fill(std::begin(entry.second->placedAt), std::end(entry.second->placedAt), 0u);
        }

        // Whatever a model's slot word reaches now (the game's block or the mod's), as a guest address; 0 = none
        uint32_t slotTarget(uint32_t model, int slot)
        {
            const uint32_t at = slotField(model, slot), w = word(vendetta_store::host(at));
            return w == ~0u ? 0u : at + w;
        }

        // The first `count` entries of two templates keyed the same way (the game's clips, its shared eye tracks and the
        // face routine's fixed channels, the lips' 1, 2, 3, 6 and the eyes' 7, 8, key the rig's bones that way)
        bool sameChannels(const uint8_t *a, const uint8_t *b, uint32_t count)
        {
            if (word(a) < count || word(b) < count)
                return false;
            for (uint32_t k = 0; k < count; ++k)
                if ((word(a + 4u + 32u * k) & 7u) != (word(b + 4u + 32u * k) & 7u))
                    return false;
            return true;
        }

        // The mod's parts and data placed in the store's match area (once a match) and the model behind's words pointed
        // there: BODY and BODY2 at the mod's BODY, its FACE and hands (every bone's current local preset to its rest
        // local, so a bone no template reaches holds its rest pose under its parent), the body's template cut to the
        // rig's 42 (the SHADOW, the model behind's own, is posed with it too), the other templates and the motions whole,
        // the see-through parts at the mod's own (ALPHA2 only with at least the bones the face's template poses in it:
        // FUN_001b2998 poses it with that template, FUN_001b4620 bone by bone for its count) or at none. Before a model
        // behind's words first change: the rig's bones must have the same parents in the mod's parts as in the game's
        // (chooseModelsBehind picked a model behind that has). False: the model can't be posed (a line in mods.log).
        bool pointSlots(uint32_t carrier, ModModel &m)
        {
            if (!g_pointed.count(carrier))
                for (int slot : {kBody, kFace, kLeftHand, kRightHand})
                {
                    const uint32_t gameAt = slotTarget(carrier, slot);
                    const uint8_t *mod = m.at(slot), *game = gameAt ? vendetta_store::host(gameAt) : nullptr;
                    if (!mod || !game || word(game + 8) < standardBones(slot))
                        continue;
                    const std::vector<int32_t> pm = parents(mod, word(mod + 8)), pg = parents(game, word(game + 8));
                    for (uint32_t k = 0; k < standardBones(slot); ++k)
                        if (pm[k] != pg[k])
                        {
                            vendetta_mods::problem() << m.what << ": its bone " << k << (slot == kBody ? " of the body" : slot == kFace ? " of the face" : " of a hand")
                                                     << " hangs from another bone than in " << m.behind << " rig: " << m.behind
                                                     << " model is used instead" << std::endl;
                            return false;
                        }
                }
            if (!m.placedAt[kBody] && m.pointable[kAlpha2])
            {
                // the face's template the game will pose ALPHA2 with: the mod's (if pointed) or the model behind's
                uint32_t poses = m.pointable[9] ? word(m.at(9)) : 0u;
                if (const uint32_t game = slotTarget(carrier, 9))
                    poses = std::max(poses, word(vendetta_store::host(game)));
                if (word(m.at(kAlpha2)) < poses)
                {
                    m.pointable[kAlpha2] = false;
                    vendetta_mods::problem() << m.what << ": its see-through part ALPHA2 has " << word(m.at(kAlpha2))
                                             << " bones, fewer than the " << poses << " the face's template poses in it: not drawn"
                                             << std::endl;
                }
            }
            if (!m.placedAt[kBody])
            {
                uint64_t total = 0;
                for (int slot = kFirstPointed; slot <= kLastPointed; ++slot)
                    if (m.pointable[slot])
                        total += ((m.part[slot] & 15u) + m.partSize[slot] + 15u) & ~15ull;
                const uint32_t base = total < 0x10000000ull
                                          ? vendetta_store::allocate(vendetta_store::Area::Match, static_cast<uint32_t>(total), "mod models")
                                          : 0u;
                if (!base)
                {
                    vendetta_mods::problem() << m.what << ": no room for it in the game's memory: " << m.behind << " model is used instead"
                                             << std::endl;
                    return false;
                }
                uint32_t at = base;
                for (int slot = kFirstPointed; slot <= kLastPointed; ++slot)
                    if (m.pointable[slot])
                    {
                        const uint32_t phase = static_cast<uint32_t>(m.part[slot]) & 15u; // as loaded: the file's alignment
                        uint8_t *copy = vendetta_store::host(at + phase);
                        m.placedAt[slot] = at + phase;
                        std::memcpy(copy, m.at(slot), m.partSize[slot]);
                        if (slot <= kRightHand)
                            for (uint32_t k = 0; k < word(copy + 8); ++k)
                                std::memcpy(copy + 0x10 + 0x190 * k + 0x90, copy + 0x10 + 0x190 * k + 0x50, 64);
                        if (slot == 8)
                            std::memcpy(copy, &kBodyBones, 4);
                        at += (phase + m.partSize[slot] + 15u) & ~15u;
                    }
                std::cerr << m.what << ": placed for the match (" << (at - base) / 1024u << " KB: the body's "
                          << word(m.at(kBody) + 8) << " bones";
                if (m.at(kFace))
                    std::cerr << ", the face's " << word(m.at(kFace) + 8);
                if (m.at(kLeftHand) && m.at(kRightHand))
                    std::cerr << ", the hands' " << word(m.at(kLeftHand) + 8) << " / " << word(m.at(kRightHand) + 8);
                std::cerr << "; " << m.partSize[14] << " bytes of face expressions";
                for (int slot : {kAlpha1, kAlpha2})
                    if (m.pointable[slot])
                        std::cerr << "; see-through " << (slot == kAlpha1 ? "ALPHA1" : "ALPHA2") << " " << word(m.at(slot)) << " bones, "
                                  << word(m.at(slot) + 8) << " triangles";
                std::cerr << "), " << m.behind << " model behind pointed at it" << std::endl;
            }
            Pointed &pointed = g_pointed[carrier];
            for (int slot = kFirstPointed; slot <= kLastPointed; ++slot)
            {
                const int from = slot == kBody2 ? kBody : slot;
                const bool none = (slot == kAlpha1 || slot == kAlpha2) && !m.pointable[slot];
                if (!none && !m.pointable[from])
                    continue;
                const uint32_t at = slotField(carrier, slot);
                uint8_t *field = vendetta_store::host(at);
                const uint32_t now = word(field), want = none ? ~0u : m.placedAt[from] - at;
                if (now == want)
                    continue;
                if (slot >= 8 && slot <= 11 && (!pointed.set[slot] || now != pointed.written[slot]))
                {
                    // the game's template, before it is pointed away: the mod's must key the rig's bones the same way
                    if (now != ~0u && !sameChannels(m.at(slot), vendetta_store::host(at + now), standardBones(slot)))
                    {
                        m.pointable[slot] = false;
                        if (slot == 9 || slot == 10)
                            m.pointable[slot + 5] = false; // its motion only with it
                        vendetta_mods::problem() << m.what << ": "
                                                 << (slot == 8 ? "the body's" : slot == 9 ? "the face's" : slot == 10 ? "the lips'" : "the eyes'")
                                                 << " template keys the game's rig differently from the game's animations: "
                                                 << m.behind << " is used" << std::endl;
                        continue;
                    }
                }
                if (!pointed.set[slot] || now != pointed.written[slot])
                    pointed.original[slot] = now; // first time, or the model behind loaded again
                pointed.set[slot] = true;
                pointed.written[slot] = want;
                std::memcpy(field, &want, 4);
            }
            return true;
        }

        // The fighter whose model holds this address (in its buffer, or a part its words point at), or 4
        uint32_t fighterOf(uint8_t *rdram, uint32_t address)
        {
            for (uint32_t i = 0; i < 4u; ++i)
            {
                const uint32_t model = word(mem(rdram, kRigs + kRigSize * i + kRigModel));
                if (!model)
                    continue;
                if (address >= model && address < model + kModelBuffer)
                    return i;
                for (int slot = kBody; slot <= kRightHand; ++slot)
                    if (slotTarget(model, slot) == address)
                        return i;
            }
            return 4u;
        }

        // FUN_001b1b20(fighter, draw): a mod model's skeleton into the model behind it, then the game's own routine
        void poseHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            dropUnused(rdram);
            const uint32_t i = getRegU32(ctx, 4);
            if (g_modGameDraw)
            {
                g_modGameDraw = false; // modelOf: the model itself, read
                ModModel *m = modelOf(rdram, i);
                g_modGameDraw = true;
                const uint32_t carrier = word(mem(rdram, kRigs + kRigSize * i + kRigModel));
                if (m && carrier && m->file.size() <= kModelBuffer &&
                    std::memcmp(mem(rdram, carrier), m->file.data(), 0x90) != 0)
                {
                    std::memcpy(mem(rdram, carrier), m->file.data(), m->file.size());
                    std::cerr << "[wrestlers] the mod's model copied into fighter " << i << "'s buffer: the game draws it" << std::endl;
                }
                g_originalPose(rdram, ctx, runtime);
                return;
            }
            if (const uint32_t carrier = word(mem(rdram, kRigs + kRigSize * i + kRigModel)))
            {
                ModModel *m = modelOf(rdram, i);
                if (m && !pointSlots(carrier, *m))
                {
                    m->ok = false; // the model behind drawn whole from now on (modelOf: none)
                    m = nullptr;
                }
                if (!m)
                    pointBack(carrier); // a game wrestler's own model in that buffer now, or a mod's that can't be posed
            }
            g_originalPose(rdram, ctx, runtime);
        }

        // FUN_001b2088(fighter) for a mod model: its own see-through parts (ALPHA1, then ALPHA2), as the game draws a game
        // model's, High Poly only (the game's test at 0x1B20B8), each the part the model behind's slot word reaches (the
        // mod's, posed this picture by the game); then its triangles touching partly clear skin, High Poly or not
        void seeThroughHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t fighter = getRegU32(ctx, 4);
            if (ModModel *m = modTurn() ? modelOf(rdram, fighter) : nullptr)
            {
                SeeThroughList &list = g_seeThrough[fighter & 3u];
                list.noPass = false;
                const uint32_t rig = kRigs + kRigSize * (fighter & 3u);
                const uint32_t carrier = word(mem(rdram, rig + kRigModel));
                if (!g_alphaCheck && carrier && (word(mem(rdram, rig + kRigFlags)) & 1u))
                    for (int slot : {kAlpha1, kAlpha2})
                        if (m->placedAt[slot] && slotTarget(carrier, slot) == m->placedAt[slot])
                            drawAlpha(rdram, m->placedAt[slot], rig, list);
                if (list.pending)
                    drawSeeThrough(list, kSeeThroughTest);
                list.pending = false;
                if (g_alphaCheck)
                {
                    // the check: the game's own routine draws the mod's parts (the model behind's skin), its FUN_001b49a0
                    // compared with ours
                    g_originalSeeThrough(rdram, ctx, runtime);
                    return;
                }
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            g_originalSeeThrough(rdram, ctx, runtime);
        }

        // The body's call for a mod fighter: its body, hands and face, each with the posed bones of the game's part of the
        // same kind, the game's lights, the skin's address the game just wrote into the body's stream (TEX0, VU 0x3B)
        void drawModel(uint8_t *rdram, R5900Context *ctx, ModModel &m, uint32_t fighter)
        {
            const uint32_t carrier = word(mem(rdram, kRigs + kRigSize * fighter + kRigModel));
            // each fighter's own copy of the model behind (where the mod's skeleton is written): one line when it changes
            static uint32_t lastCarrier[4] = {};
            if (lastCarrier[fighter & 3u] != carrier)
            {
                lastCarrier[fighter & 3u] = carrier;
                std::cerr << "[wrestlers] fighter " << fighter << " (record "
                          << word(mem(rdram, kFighters + kFighterSize * fighter + kFighterRecord)) << "): the model behind in "
                          << std::hex << "0x" << carrier << std::dec << ", pointed at the mod's parts" << std::endl;
            }
            const uint8_t *body = mem(rdram, getRegU32(ctx, 4)); // the posed body: BODY or BODY2 (the same skeleton)
            const uint8_t *light = mem(rdram, getRegU32(ctx, 5)), *colours = mem(rdram, getRegU32(ctx, 6));
            const uint8_t *tex0 = body + (word(body) & ~3u) + 0x30u;
            // the see-through pass's list: still full from the last picture = this scene has no see-through pass (only
            // the match picture calls FUN_001b2088): drawn straight after the solid parts from then on
            SeeThroughList &list = g_seeThrough[fighter & 3u];
            if (list.pending)
                list.noPass = true;
            list.pending = false;
            list.tris.clear();
            list.tex0Reg = word(tex0 + 8) & 0xFFu;
            list.tex0 = uint64_t(word(tex0)) | (uint64_t(word(tex0 + 4)) << 32);
            list.image = m.image;
            const struct { int slot; } order[] = {{kBody}, {kLeftHand}, {kRightHand}, {kFace}};
            for (const auto &o : order)
            {
                uint8_t *part = m.at(o.slot);
                if (!part)
                    continue;
                const uint8_t *posed = body;
                if (o.slot != kBody)
                {
                    const uint32_t at = slotTarget(carrier, o.slot); // the mod's part the game posed
                    if (!at)
                        continue;
                    posed = mem(rdram, at);
                }
                uint8_t *stream = part + (word(part) & ~3u);
                std::memcpy(stream + 0x30u, tex0, 16); // the skin's address (the slot the game uploaded to)
                writeRows(posed, part, light, colours, stream);
                drawPart(part, m.image, m.cutout, m.cutout && !g_softOff ? &list.tris : nullptr, &m.softGrid);
            }
            if (!list.tris.empty())
            {
                if (list.noPass)
                    drawSeeThrough(list, kWrestlerTest);
                else
                    list.pending = true;
            }
        }

        // ------------------------------------------------------------------ the hook

        void drawPartHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31), part = getRegU32(ctx, 4);
            const bool wrestler = ra == kFromBody || ra == kFromLeftHand || ra == kFromRightHand || ra == kFromFace;
            if (!wrestler || part == 0u)
            {
                g_originalDrawPart(rdram, ctx, runtime);
                return;
            }
            // a mod fighter's own model: drawn at the body's call, nothing at the hands' and face's
            if (const uint32_t fighter = fighterOf(rdram, part); fighter < 4u && modTurn())
                if (ModModel *m = modelOf(rdram, fighter))
                {
                    if (ra == kFromBody)
                        drawModel(rdram, ctx, *m, fighter);
                    logStats();
                    ctx->pc = ra;
                    return;
                }
            if (g_mode == Mode::Off)
            {
                g_originalDrawPart(rdram, ctx, runtime);
                return;
            }
            if (g_mode == Mode::Check)
            {
                checkPart(rdram, ctx, runtime);
                return;
            }
            if (!oursNow())
            {
                ++g_stats.gameParts;
                logStats();
                g_originalDrawPart(rdram, ctx, runtime);
                return;
            }
            const uint8_t *p = mem(rdram, part);
            uint8_t *stream = mem(rdram, part + (word(p) & ~3u));
            writeRows(rdram, part, getRegU32(ctx, 5), getRegU32(ctx, 6), stream);
            if (!drawPart(mem(rdram, part)))
            {
                logStats();
                g_originalDrawPart(rdram, ctx, runtime); // not readable: the game's own draw (it writes the same rows)
                return;
            }
            logStats();
            ctx->pc = ra;
        }
    }

    bool drawGamePart(PS2Runtime &runtime, uint8_t *rdram, uint32_t part, uint32_t lightMatrix, uint32_t colours,
                      bool rows, uint32_t image, std::vector<GsTriangle> *capture, uint8_t *own)
    {
        if (!g_runtime)
            g_runtime = &runtime;
        const Stats wrestlers = g_stats; // the wrestlers' own count stays theirs
        uint8_t *p = mem(rdram, part);
        if (own)
            writeRows(p, own, mem(rdram, lightMatrix), mem(rdram, colours), own + (word(own) & ~3u));
        else if (rows)
            writeRows(rdram, part, lightMatrix, colours, mem(rdram, part + (word(p) & ~3u)));
        g_capture = capture;
        const bool drawn = drawPart(own ? own : p, image);
        g_capture = nullptr;
        g_stats = wrestlers;
        return drawn;
    }

    void install(PS2Runtime &runtime)
    {
        const char *value = std::getenv("VENDETTA_WRESTLERDRAW");
        const std::string v = value ? value : "";
        // a mod wrestler with its own model: our draw for it whatever the switch
        bool modModels = false;
        for (uint32_t record = 0; record < vendetta_wrestlers::endRecord(); ++record) // new and replaced attires
            modModels = modModels || vendetta_wrestlers::modelFile(record) != nullptr;
        g_alphaCheck = std::getenv("VENDETTA_SEETHROUGH_CHECK") && *std::getenv("VENDETTA_SEETHROUGH_CHECK") != '0';
        if ((v.empty() || v == "0") && !modModels && !g_alphaCheck)
            return;
        if (v.empty() || v == "0")
            g_mode = Mode::Off;
        else if (v == "flip")
            g_mode = Mode::Flip;
        else if (v == "check")
        {
            g_mode = Mode::Check;
            runtime.gs().setGifTap(gifTap, nullptr);
        }
        else if (v.rfind("ab", 0) == 0)
        {
            g_mode = Mode::Ab;
            const size_t colon = v.find(':');
            if (colon != std::string::npos)
                g_abSeconds = std::max(0.25, std::atof(v.c_str() + colon + 1));
        }
        else
            g_mode = Mode::Ours;
        g_log = std::getenv("VENDETTA_WRESTLERLOG") && *std::getenv("VENDETTA_WRESTLERLOG") != '0';
        g_modGameDraw = std::getenv("VENDETTA_MODMODEL_GAMEDRAW") && *std::getenv("VENDETTA_MODMODEL_GAMEDRAW") != '0';
        g_runtime = &runtime;
        g_originalDrawPart = runtime.lookupFunction(kDrawPart);
        g_originalPose = runtime.lookupFunction(kPose);
        g_originalSeeThrough = runtime.lookupFunction(kSeeThrough);
        g_originalAlphaDraw = runtime.lookupFunction(kAlphaDraw);
        if (!g_originalDrawPart || !g_originalPose || !g_originalSeeThrough || !g_originalAlphaDraw)
        {
            vendetta_mods::problem() << "[wrestlers] the game's fighter draw routines not found: off" << std::endl;
            g_mode = Mode::Off;
            return;
        }
        runtime.replaceFunction(kDrawPart, drawPartHook);
        if (g_alphaCheck)
        {
            runtime.replaceFunction(kAlphaDraw, alphaCheckHook);
            std::cerr << "[wrestlers] VENDETTA_SEETHROUGH_CHECK: the game's see-through draw checked against ours" << std::endl;
        }
        if (modModels)
        {
            chooseModelsBehind(runtime.memory().getRDRAM());
            runtime.replaceFunction(kPose, poseHook);
            runtime.replaceFunction(kSeeThrough, seeThroughHook);
            vendetta_store::onMatchEmptied(pointAllBack);
            std::cerr << "[wrestlers] mods' own models: drawn by our code" << std::endl;
        }
        if (g_mode != Mode::Off)
            std::cerr << "[wrestlers] on: the wrestlers drawn by our code ("
                      << (g_mode == Mode::Check ? "checked against the game's" : g_mode == Mode::Ab ? "in turn with the game's"
                          : g_mode == Mode::Flip ? "F9 swaps it with the game's" : "the skin the game uploads")
                      << ")" << std::endl;
    }
}

void installVendettaWrestlerDraw(PS2Runtime &runtime) { vendetta_wrestlerdraw::install(runtime); }
