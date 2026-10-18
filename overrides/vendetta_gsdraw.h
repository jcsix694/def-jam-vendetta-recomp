// Def Jam Vendetta (USA, SLUS-20639) - shared pieces for drawing the game's models with our own code (the arenas,
// vendetta_arenas.cpp; the wrestlers, vendetta_wrestlerdraw.cpp).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// - the VU's number rules (round toward zero while our draw works out a kernel's maths, DIV, FTOI, the CLIP flags);
// - a GIF packet being built (PACKED tags, A+D) and sent whole into the game's GS stream;
// - the check: GS packets read back into the triangles the GS draws (the vertex queue, ADC), compared bit for bit;
// - the window title's " | <what>: ..." for the F9 swap test modes.
#pragma once

#include "ps2_runtime.h"

#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// The VU rounds every multiply and every add on its own, toward zero (the recomp's VU1 runs with FE_TOWARDZERO, as the
// PS2's VU rounds): no fused multiply-adds, and the rounding mode is read at run time.
#if defined(_MSC_VER)
#pragma fp_contract(off)
#pragma fenv_access(on)
#endif

namespace vendetta_gsdraw
{
    // ---- the VU's numbers

    // The VU's rounding (toward zero) while our draw does a kernel's maths, the caller's after
    struct VuRounding
    {
        int saved = std::fegetround();
        VuRounding() { std::fesetround(FE_TOWARDZERO); }
        ~VuRounding() { std::fesetround(saved); }
    };

    // DIV as the VU does it (a zero divisor gives the largest float with the sign)
    inline float vuDiv(float num, float den)
    {
        if (den == 0.0f)
            return std::signbit(num) != std::signbit(den) ? -std::numeric_limits<float>::max()
                                                           : std::numeric_limits<float>::max();
        return num / den;
    }

    // FTOI0 / FTOI4: scaled in double, truncated, held to the int32 range
    inline int32_t ftoi(float value, double scale)
    {
        const double scaled = static_cast<double>(value) * scale;
        if (scaled >= 2147483647.0)
            return std::numeric_limits<int32_t>::max();
        if (scaled <= -2147483648.0)
            return std::numeric_limits<int32_t>::min();
        return static_cast<int32_t>(scaled);
    }

    // CLIP's flags (bit 0 x+, 1 x-, 2 y+, 3 y-, 4 z+, 5 z-) of (x, y, z) against |w|, as the VU compares them: on
    // the bits (a w that is zero or denormal gives the largest denormal as the limit)
    inline uint32_t clipFlags(float x, float y, float z, float w)
    {
        uint32_t wBits;
        std::memcpy(&wBits, &w, 4);
        const int32_t limit = (wBits & 0x7F800000u) ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
        auto exceeds = [limit](float v, uint32_t sign) {
            uint32_t bits;
            std::memcpy(&bits, &v, 4);
            bits ^= sign;
            int32_t ordered;
            std::memcpy(&ordered, &bits, 4);
            return ordered > limit;
        };
        return (exceeds(x, 0u) ? 0x01u : 0u) | (exceeds(x, 0x80000000u) ? 0x02u : 0u) |
               (exceeds(y, 0u) ? 0x04u : 0u) | (exceeds(y, 0x80000000u) ? 0x08u : 0u) |
               (exceeds(z, 0u) ? 0x10u : 0u) | (exceeds(z, 0x80000000u) ? 0x20u : 0u);
    }

    // ---- a GIF packet

    struct Packet
    {
        std::vector<uint64_t> q; // pairs: a quadword each
        size_t lastTag = SIZE_MAX;
        void add(uint64_t lo, uint64_t hi)
        {
            q.push_back(lo);
            q.push_back(hi);
        }
        void tag(uint64_t lo, uint64_t hi)
        {
            lastTag = q.size();
            add(lo, hi);
        }
        void ad(uint32_t reg, uint64_t value)
        {
            tag(0x1000000000000001ull, 0xEull); // GIFtag NLOOP 1, PACKED, NREG 1, A+D
            add(value, reg);
        }
        // The last tag ends the packet (EOP): PATH3 holds the GIF until it sees one.
        void end() { q[lastTag] |= 0x8000ull; }
    };

    constexpr size_t kPacketBytes = 64u * 1024u; // a GIF packet of ours is sent at about this size

    // ---- the check: GS packets read back into the triangles they draw

    struct GsVertex
    {
        uint32_t s = 0, t = 0, q = 0, rgba = 0, x = 0, y = 0, z = 0;
        bool operator==(const GsVertex &o) const
        {
            return s == o.s && t == o.t && q == o.q && rgba == o.rgba && x == o.x && y == o.y && z == o.z;
        }
    };
    struct GsTriangle
    {
        GsVertex v[3];
    };

    // The vertex queue as the GS keeps it: a list kicks every third vertex, a strip each vertex from the third, a
    // fan each from the third around the first; a vertex with ADC adds without kicking; PRIM resets the queue.
    struct GsReader
    {
        uint32_t type = 0, count = 0;
        GsVertex queue[3], first;
        GsVertex cur;
        std::vector<GsTriangle> *out = nullptr;

        void prim(uint64_t value)
        {
            type = static_cast<uint32_t>(value & 7u);
            count = 0;
        }
        void vertex(uint32_t x, uint32_t y, uint32_t z, bool adc)
        {
            GsVertex v = cur;
            v.x = x;
            v.y = y;
            v.z = z;
            if (type == 3u)
            {
                queue[count++] = v;
                if (count == 3u)
                {
                    if (!adc)
                        out->push_back({{queue[0], queue[1], queue[2]}});
                    count = 0;
                }
                return;
            }
            if (type == 5u && count == 0u)
                first = v;
            queue[0] = queue[1];
            queue[1] = queue[2];
            queue[2] = v;
            ++count;
            if (count >= 3u && !adc && (type == 4u || type == 5u))
                out->push_back({{type == 5u ? first : queue[0], queue[1], queue[2]}});
        }
        void feed(const uint8_t *d, uint32_t bytes)
        {
            uint32_t o = 0;
            while (o + 16u <= bytes)
            {
                uint64_t lo, hi;
                std::memcpy(&lo, d + o, 8);
                std::memcpy(&hi, d + o + 8, 8);
                o += 16u;
                const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
                const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
                const uint32_t nreg = ((lo >> 60) & 0xFu) ? static_cast<uint32_t>((lo >> 60) & 0xFu) : 16u;
                if ((lo >> 46) & 1u)
                    prim((lo >> 47) & 0x7FFu);
                if (flg != 0u)
                {
                    o += flg == 2u ? nloop * 16u : (nloop * nreg + 1u) / 2u * 16u;
                    continue;
                }
                for (uint32_t i = 0; i < nloop * nreg && o + 16u <= bytes; ++i, o += 16u)
                {
                    uint32_t w[4];
                    std::memcpy(w, d + o, 16);
                    switch ((hi >> (4u * (i % nreg))) & 0xFu)
                    {
                    case 0x0: prim(w[0]); break;
                    case 0x1: cur.rgba = (w[0] & 0xFFu) | ((w[1] & 0xFFu) << 8) | ((w[2] & 0xFFu) << 16) | ((w[3] & 0xFFu) << 24); break;
                    case 0x2: cur.s = w[0]; cur.t = w[1]; cur.q = w[2]; break;
                    case 0x4: vertex(w[0] & 0xFFFFu, w[1] & 0xFFFFu, (w[2] >> 4) & 0xFFFFFFu, (w[3] >> 15) & 1u); break;
                    case 0x5: vertex(w[0] & 0xFFFFu, w[1] & 0xFFFFu, w[2], (w[3] >> 15) & 1u); break;
                    case 0xE:
                    {
                        uint64_t value;
                        std::memcpy(&value, d + o, 8);
                        if ((w[2] & 0xFFu) == 0x00u)
                            prim(value);
                        break;
                    }
                    default: break;
                    }
                }
                if (lo & 0x8000u)
                    break;
            }
        }
    };

    struct TriangleHash
    {
        size_t operator()(const GsTriangle &t) const
        {
            uint64_t h = 1469598103934665603ull;
            for (const GsVertex &v : t.v)
                for (uint32_t w : {v.s, v.t, v.q, v.rgba, v.x, v.y, v.z})
                    h = (h ^ w) * 1099511628211ull;
            return static_cast<size_t>(h);
        }
    };
    struct TriangleEq
    {
        bool operator()(const GsTriangle &a, const GsTriangle &b) const
        {
            return a.v[0] == b.v[0] && a.v[1] == b.v[1] && a.v[2] == b.v[2];
        }
    };

    inline std::string describe(const GsVertex &v)
    {
        float s, t, q;
        std::memcpy(&s, &v.s, 4);
        std::memcpy(&t, &v.t, 4);
        std::memcpy(&q, &v.q, 4);
        char text[200];
        std::snprintf(text, sizeof(text), "xy %.4f %.4f z %u st %.9g %.9g q %.9g (%08x %08x %08x) rgba %08x",
                      v.x / 16.0, v.y / 16.0, v.z, s, t, q, v.s, v.t, v.q, v.rgba);
        return text;
    }

    // ---- the window title, for the F9 swap test modes

#if defined(_WIN32)
    inline bool ourWindowInFront()
    {
        DWORD pid = 0;
        const HWND front = GetForegroundWindow();
        return front && GetWindowThreadProcessId(front, &pid) && pid == GetCurrentProcessId();
    }

    // The window title with "<marker><text>" at the end (marker e.g. L" | Arena: "), set from a thread of its own: the
    // window belongs to another thread, and the game thread must never wait for it.
    inline void showInTitle(const wchar_t *marker, std::wstring text)
    {
        std::wstring mark = marker;
        std::thread([mark, text] {
            struct Find
            {
                HWND window = nullptr;
            } find;
            EnumWindows([](HWND window, LPARAM data) -> BOOL {
                DWORD pid = 0;
                GetWindowThreadProcessId(window, &pid);
                if (pid != GetCurrentProcessId() || !IsWindowVisible(window))
                    return TRUE;
                reinterpret_cast<Find *>(data)->window = window;
                return FALSE;
            }, reinterpret_cast<LPARAM>(&find));
            if (!find.window)
                return;
            wchar_t current[512] = {};
            SendMessageTimeoutW(find.window, WM_GETTEXT, 512, reinterpret_cast<LPARAM>(current), SMTO_ABORTIFHUNG, 500, nullptr);
            std::wstring title = current;
            const size_t at = title.find(mark);
            if (at != std::wstring::npos)
                title.resize(at);
            title += mark + text;
            SendMessageTimeoutW(find.window, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(title.c_str()), SMTO_ABORTIFHUNG, 500,
                                nullptr);
        }).detach();
    }
#endif
}
