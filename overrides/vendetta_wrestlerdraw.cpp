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
// come) is always drawn by our code, whatever the switch: the game still loads a game model behind it (its "based on",
// else Briggs') and poses it; before each pose (FUN_001b1b20) our code writes the mod file's skeleton into that copy
// (every bone's inverse bind and rest matrix of BODY, BODY2, FACE and both hands, and the face, lip, eye and hand
// templates), so the game poses the mod's own face and hands; the body's part call then draws the mod's body, hands
// and face with those posed bones and the game's lights, the hands' and face's calls draw nothing for it, and the
// game's see-through parts of the model behind (FUN_001b2088) are not drawn. The shadow is the game's (the model
// behind's: every game model's is the same); a shadow or a Low Poly body in the mod's file is ignored. The skin: the
// mod file's own (decoded from its upload packet), or textures/skin.png at any size, as a host image bound to the
// skin's address (GS register 0x71) around the mod's triangles. A file that can't be used leaves the game's model.
//
// VENDETTA_WRESTLERDRAW=1      our draw (sampling the skin the game uploaded)
// VENDETTA_WRESTLERDRAW=ab[:N] the game's draw and ours in turn, N seconds each (default 2): pictures compared
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
#include "vendetta_wrestlers.h"
#include "runtime/gs/gs_gpu.h"

#include <algorithm>
#include <array>
#include <chrono>
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
            uint64_t parts = 0, gameParts = 0, batches = 0, triangles = 0, skipped = 0, unreadable = 0;
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
            for (uint32_t k = 0; k < bones && k < 256u; ++k)
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

        // One whole GIF packet (EOP on its last tag) into the game's GS stream, in order (check: read back instead)
        void send(Packet &packet)
        {
            if (packet.q.empty())
                return;
            packet.end();
            if (g_mode == Mode::Check)
            {
                GsReader reader;
                reader.out = &g_ourTriangles;
                reader.feed(reinterpret_cast<const uint8_t *>(packet.q.data()), static_cast<uint32_t>(packet.q.size() * 8u));
            }
            else
                g_runtime->memory().submitChainGif(reinterpret_cast<const uint8_t *>(packet.q.data()),
                                                   static_cast<uint32_t>(packet.q.size() * 8u));
            packet.q.clear();
            packet.lastTag = SIZE_MAX;
        }

        // The part drawn from its stream (the rows already written). `image`: a host image bound to TEX0's address
        // around its triangles (a mod's skin), 0 for none. False: not readable (the caller lets the game draw it).
        bool drawPart(const uint8_t *p, uint32_t image = 0u)
        {
            const uint8_t *stream = p + (word(p) & ~3u);
            const uint32_t bytes = word(p + 4);
            uint32_t setupRows = 0;
            std::vector<Batch> batches;
            if (bytes == 0u || bytes > 0x100000u || !readStream(stream, bytes, setupRows, batches))
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
                    for (uint32_t j = 0; j < count; ++j)
                    {
                        const Out *o = vertexAt(word(idx + 4u * (k + j)));
                        if (!o)
                            return false;
                        const bool adc = j >= 2u && (f1 || f2 || o->flagged);
                        vertexQwords(packet, *o, adc);
                        if (j >= 2u)
                            ++(adc ? g_stats.skipped : g_stats.triangles);
                        f1 = f2;
                        f2 = o->flagged;
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
                }
                ++g_stats.batches;
                if (packet.q.size() * 8u >= kPacketBytes)
                    send(packet);
            }
            if (image)
                packet.ad(0x71u, uint64_t(tbp)); // unbound: the game's own draws at this address are untouched
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
                      << " by the game, " << g_stats.unreadable << " unreadable" << std::endl;
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
            if (bytes != 0u && bytes <= 0x100000u)
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
        constexpr int kBody = 1, kBody2 = 2, kFace = 3, kLeftHand = 4, kRightHand = 5, kTexture = 18;

        PS2Runtime::RecompiledFunction g_originalPose = nullptr, g_originalSeeThrough = nullptr;

        struct ModModel
        {
            std::vector<uint8_t> file;
            int32_t part[32];
            bool ok = false;
            uint32_t image = 0;
            const uint8_t *at(int slot) const { return part[slot] >= 0 ? file.data() + part[slot] : nullptr; }
            uint8_t *at(int slot) { return part[slot] >= 0 ? file.data() + part[slot] : nullptr; }
        };

        std::unordered_map<uint32_t, std::unique_ptr<ModModel>> g_models; // by record
        uint32_t g_lastRecords[4] = {~0u, ~0u, ~0u, ~0u};
        uint32_t g_nextImage = kSkinImageBase;
        // VENDETTA_MODMODEL_GAMEDRAW=1 (a test, the reference picture): a mod's WRS copied whole into the game's model
        // buffer (the PS2 way; it must fit 614,400 bytes), so the game draws it itself; our draw leaves it alone
        bool g_modGameDraw = false;

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
                    std::cerr << "[wrestlers] " << files.modName << ": " << files.wrestler << ": textures/skin.png can't be read"
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

        // A mod's model read and checked: the game's format (WRS); glTF to come.
        std::unique_ptr<ModModel> loadModel(const vendetta_wrestlers::ModelFile &files)
        {
            auto m = std::make_unique<ModModel>();
            const std::string what = "[wrestlers] " + files.modName + ": " + files.wrestler + ": " + files.model.filename().string();
            if (files.model.extension() != ".wrs")
            {
                std::cerr << what << ": glTF models are not read yet: its game model shows" << std::endl;
                return m;
            }
            std::ifstream in(files.model, std::ios::binary);
            m->file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (m->file.size() < 0x100u || std::memcmp(m->file.data(), "WRS ", 4) != 0)
            {
                std::cerr << what << ": not a model in the game's format (WRS): its game model shows" << std::endl;
                return m;
            }
            for (int n = 0; n < 32; ++n)
                m->part[n] = slotOffset(m->file.data(), m->file.size(), n);
            // the parts our draw uses: the body (required), the face and the hands (if there); the bones as the game's rig
            const struct { int slot; uint32_t bones; const char *name; bool required; } parts[] = {
                {kBody, 42u, "the body (BODY)", true}, {kFace, 11u, "the face", false},
                {kLeftHand, 13u, "the left hand", false}, {kRightHand, 13u, "the right hand", false}};
            for (const auto &part : parts)
            {
                const uint8_t *p = m->at(part.slot);
                if (!p)
                {
                    if (part.required)
                    {
                        std::cerr << what << ": no " << part.name << ": its game model shows" << std::endl;
                        return m;
                    }
                    continue;
                }
                uint32_t setupRows = 0;
                std::vector<Batch> batches;
                const uint32_t bytes = word(p + 4);
                if (word(p + 8) != part.bones || p + (word(p) & ~3u) + bytes > m->file.data() + m->file.size() ||
                    !readStream(p + (word(p) & ~3u), bytes, setupRows, batches))
                {
                    std::cerr << what << ": " << part.name << " can't be read (" << word(p + 8) << " bones, the game's rig has "
                              << part.bones << "): its game model shows" << std::endl;
                    return m;
                }
            }
            m->ok = true;
            const bool skin = makeSkin(*m, files);
            std::cerr << what << ": read (" << m->file.size() / 1024 << " KB" << (skin ? ", its skin" : ", no skin of its own")
                      << ")" << std::endl;
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
                found = g_models.emplace(record, loadModel(*files)).first;
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

        // The mod file's skeleton into the game's copy of the model behind it: every bone's inverse bind (+0x10) and
        // rest matrix (+0x50) of BODY and BODY2 (both from the mod's BODY: a Low Poly body in the file is ignored), FACE
        // and the hands, and the templates (slots 8-13: face, lip, eye, hands, the body's) where the counts agree.
        void writeSkeleton(uint8_t *rdram, uint32_t carrier, const ModModel &m)
        {
            const uint8_t *game = mem(rdram, carrier);
            auto gamePart = [&](int slot) -> uint8_t * {
                const int32_t o = slotOffset(game, kModelBuffer, slot);
                return o < 0 ? nullptr : mem(rdram, carrier + static_cast<uint32_t>(o));
            };
            const struct { int game, mod; } parts[] = {{kBody, kBody}, {kBody2, kBody}, {kFace, kFace},
                                                        {kLeftHand, kLeftHand}, {kRightHand, kRightHand}};
            for (const auto &pair : parts)
            {
                uint8_t *to = gamePart(pair.game);
                const uint8_t *from = m.at(pair.mod);
                if (!to || !from || word(to + 8) != word(from + 8))
                    continue;
                for (uint32_t k = 0; k < word(from + 8); ++k)
                    std::memcpy(to + 0x10 + 0x190 * k + 0x10, from + 0x10 + 0x190 * k + 0x10, 0x80);
            }
            for (int slot = 8; slot <= 13; ++slot)
            {
                uint8_t *to = gamePart(slot);
                const uint8_t *from = m.at(slot);
                if (to && from && word(to) == word(from) && word(from) <= 64u)
                    std::memcpy(to + 4, from + 4, 32u * word(from));
            }
        }

        // The fighter whose model holds this address, or 4
        uint32_t fighterOf(uint8_t *rdram, uint32_t address)
        {
            for (uint32_t i = 0; i < 4u; ++i)
            {
                const uint32_t model = word(mem(rdram, kRigs + kRigSize * i + kRigModel));
                if (model && address >= model && address < model + kModelBuffer)
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
            if (ModModel *m = modelOf(rdram, i))
                if (const uint32_t carrier = word(mem(rdram, kRigs + kRigSize * i + kRigModel)))
                    writeSkeleton(rdram, carrier, *m);
            g_originalPose(rdram, ctx, runtime);
        }

        // FUN_001b2088(fighter): the model behind a mod model has no see-through parts on screen
        void seeThroughHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (modelOf(rdram, getRegU32(ctx, 4)))
            {
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
            const uint8_t *game = mem(rdram, carrier);
            const uint8_t *body = mem(rdram, getRegU32(ctx, 4)); // the posed body: BODY or BODY2 (the same skeleton)
            const uint8_t *light = mem(rdram, getRegU32(ctx, 5)), *colours = mem(rdram, getRegU32(ctx, 6));
            const uint8_t *tex0 = body + (word(body) & ~3u) + 0x30u;
            const struct { int slot; } order[] = {{kBody}, {kLeftHand}, {kRightHand}, {kFace}};
            for (const auto &o : order)
            {
                uint8_t *part = m.at(o.slot);
                if (!part)
                    continue;
                const uint8_t *posed = body;
                if (o.slot != kBody)
                {
                    const int32_t off = slotOffset(game, kModelBuffer, o.slot);
                    if (off < 0)
                        continue;
                    posed = mem(rdram, carrier + static_cast<uint32_t>(off));
                }
                uint8_t *stream = part + (word(part) & ~3u);
                std::memcpy(stream + 0x30u, tex0, 16); // the skin's address (the slot the game uploaded to)
                writeRows(posed, part, light, colours, stream);
                drawPart(part, m.image);
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
            if (const uint32_t fighter = fighterOf(rdram, part); fighter < 4u)
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

    void install(PS2Runtime &runtime)
    {
        const char *value = std::getenv("VENDETTA_WRESTLERDRAW");
        const std::string v = value ? value : "";
        // a mod wrestler with its own model: our draw for it whatever the switch
        bool modModels = false;
        for (uint32_t record = vendetta_wrestlers::kFirstNewRecord; record < vendetta_wrestlers::endRecord(); ++record)
            modModels = modModels || vendetta_wrestlers::modelFile(record) != nullptr;
        if ((v.empty() || v == "0") && !modModels)
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
        if (!g_originalDrawPart || !g_originalPose || !g_originalSeeThrough)
        {
            std::cerr << "[wrestlers] the game's fighter draw routines not found: off" << std::endl;
            g_mode = Mode::Off;
            return;
        }
        runtime.replaceFunction(kDrawPart, drawPartHook);
        if (modModels)
        {
            runtime.replaceFunction(kPose, poseHook);
            runtime.replaceFunction(kSeeThrough, seeThroughHook);
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
