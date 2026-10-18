// Def Jam Vendetta (USA, SLUS-20639) - the arena drawn by our own code (VENDETTA_ARENADRAW).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// The game draws an arena object with FUN_001b8bf8(object, matrix, colour) (the scrolled ones with FUN_001b9bb0, the
// same plus a (u, v) offset in f12 / f13): it copies the matrix to object + 0xD0 and the colour to + 0x110, then sends
// the object's VIF1 stream (object + 0x80, QWC = object + 0) to the VU1 kernel at entry 0xC, which transforms the
// vertices and sends them to the GS. With VENDETTA_ARENADRAW on, our code reads that stream itself (once, then kept:
// batches of {bounding box, counts, vertices (s, t) / colour / position, strips, loose triangles}) and draws it as GS
// triangles in the game's GS stream, at the same moment, with the same maths:
//   p' = (x, y, z, w) x matrix (row vectors; the matrix already holds the camera: view 0x258000 x projection 0x258040)
//   XY = p'.xy / p'.w in 12.4, Z = p'.z (not divided: 2^20 - 128 x distance), STQ = (s, t, 1) / p'.w, colour x scale
// and each triangle drawn, skipped or cut exactly as the kernel's own clipper does (see "the game's VU1 kernel" below:
// strips with the ADC bit, cut triangles as fans to the screen's edges). The GS
// state (context 2, TEST, ALPHA, ZBUF) is what the game set before the draw; the primitive's bits come from the
// object's own GIF tags (VU 0x39 / 0x3A), the texture from its material slots (VU 0x40 + k = object + 0x120 + 16k, where
// the game's picture cards write each frame's TEX0), so everything the venue routines decide stays theirs. The shadow
// floor (object 99, FUN_001b9880 from FUN_001a2930, the VU1 kernel at 0x10) is drawn the same way: positions by the
// camera matrix, the shadow picture's coordinates by the shadow matrix the game builds, its colour (0, 0, 0, 48).
//
// The textures: the arena's part 1 (FUN_001b85c0 from the arena set-up FUN_00165bd8) is read when the game uploads it,
// replayed into our own copy of GS memory, and each texture a draw uses is decoded from it into a host image of the
// GS texture's size (GS register 0x71 binds it around our primitives: paraLLEl-GS, a local addition). The game's own
// upload still happens (the 3-D crowd and the ring use it). Without the graphics card renderer the game's uploaded
// textures are used.
//
// VENDETTA_ARENADRAW=1      our draw, textures as host images (decoded from the game's files)
// VENDETTA_ARENADRAW=vram   our draw, sampling the textures the game uploaded (the geometry alone)
// VENDETTA_ARENADRAW=ab[:N] the game's draw and ours in turn, N seconds each (default 2): pictures compared
// VENDETTA_ARENADRAW=abvram[:N] the same, ours sampling the game's uploaded textures
// VENDETTA_ARENADRAW=flip   ours, and F9 (with the game's window in front) swaps between the game's draw and ours; the
//                           window title says which ("Arena: ours" / "Arena: the game's")
// VENDETTA_ARENADRAW=check  the game draws; for each object its GS packets (tapped in the GS front end) are compared
//                           with the packet ours would send, triangle by triangle ([arenas] check lines; needs
//                           VENDETTA_CHAIN_ASYNC=0 and VENDETTA_GS_ASYNC=0, so the game's packets arrive in the call)
// VENDETTA_ARENALOG=1       each arena's objects, textures and a count of what was drawn every 5 s
// VENDETTA_ARENA_GAMEIDS=<ids, comma separated>  those scene objects left to the game (finding a difference)
// VENDETTA_ARENA_STRESS=<N> each object drawn N times over itself, VENDETTA_ARENA_SPLIT=<N> each triangle drawn as N
//                           smaller ones over the same area (N times the detail), VENDETTA_ARENA_TEXSCALE=<k> each
//                           texture made k times larger (speed tests: a bigger arena, HD textures)
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_gpu.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cfenv>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <vector>

// The VU rounds every multiply and every add on its own, toward zero (the recomp's VU1 runs with FE_TOWARDZERO, as the
// PS2's VU rounds): no fused multiply-adds here, and the rounding mode is read at run time.
#if defined(_MSC_VER)
#pragma fp_contract(off)
#pragma fenv_access(on)
#endif

namespace vendetta_arenas
{
    namespace
    {
        constexpr uint32_t kDraw = 0x1B8BF8u;         // FUN_001b8bf8(object, matrix, colour): an object through VU1
        constexpr uint32_t kDrawScrolled = 0x1B9BB0u; // FUN_001b9bb0: the same, (u, v) in f12 / f13 into VU 0x40
        constexpr uint32_t kDrawShadow = 0x1B9880u;   // FUN_001b9880(object, camera, shadow): the shadow floor (99)
        constexpr uint32_t kUpload = 0x1B85C0u;       // FUN_001b85c0(p): one GIF DMA from p + p[0], p[1] bytes
        constexpr uint32_t kUploadFromArena = 0x165C70u; // the return address of its call in the arena set-up
        constexpr uint32_t kIntros = 0x1245C8u;          // FUN_001245c8(a0): the match's intros by match kind (a log line)
        constexpr uint32_t kMatchKind = 0x2361CEu;       // the match kind byte
        constexpr uint32_t kScenePointer = 0x29A930u;    // gp - 0x1C40: the scene (part 0), 0xB1C500 in the game
        constexpr uint32_t kSceneBytes = 0xB9000u;       // the scene buffer's size
        constexpr uint32_t kVenue = 0x296600u;           // the renderer's venue
        constexpr uint32_t kImageIdBase = 0x40000000u;   // our host image ids (the mods' pictures use small ones)

        enum class Mode
        {
            Off,
            Host, // our draw, host images
            Vram, // our draw, the game's uploaded textures
            Ab,   // the game's and ours in turn
            Check, // the game's draw, ours worked out and compared
            Flip, // ours or the game's, F9 swaps
        };

        Mode g_mode = Mode::Off;
        bool g_vramTextures = false; // ours samples the game's uploaded textures, not host images
        double g_abSeconds = 2.0;
        bool g_log = false;
        PS2Runtime *g_runtime = nullptr;
        PS2Runtime::RecompiledFunction g_originalDraw = nullptr;
        PS2Runtime::RecompiledFunction g_originalDrawScrolled = nullptr;
        PS2Runtime::RecompiledFunction g_originalDrawShadow = nullptr;
        PS2Runtime::RecompiledFunction g_originalUpload = nullptr;
        PS2Runtime::RecompiledFunction g_originalIntros = nullptr;
        using Clock = std::chrono::steady_clock;
        const Clock::time_point g_start = Clock::now();

        struct Vertex
        {
            float s, t, sz;  // the file's ST quadword (s, t, 1, 1): its z goes through as Q's factor
            float rgba[4];
            float pos[4];
        };

        struct Batch
        {
            uint32_t material = 0;           // the VU address of its material slot {TEX0, register}
            uint32_t top = 0, data = 0, nq = 0; // where it is unpacked in VU memory; its data's guest address, size
            uint32_t slot = 0;               // the guest address of what the kernel reads at `material` (0: nothing)
            float boxMin[4] = {}, boxMax[4] = {}; // its first two quadwords: the bounding box the kernel tests first
            std::vector<Vertex> vertices;
            std::vector<uint16_t> strips;    // the strips' vertices, one strip after another (into vertices)
            std::vector<uint16_t> lengths;   // each strip's length (3 or more)
            std::vector<uint16_t> loose;     // 3 per loose triangle
            size_t triangles() const
            {
                size_t n = loose.size() / 3u;
                for (uint16_t length : lengths)
                    n += length - 2u;
                return n;
            }
        };

        struct Mesh
        {
            bool ours = false;               // meshes of the textured kernel (entry 0xC) or the shadow floor's (0x10)
            uint32_t kernel = 0;             // 0xC or 0x10
            uint32_t qwc = 0;
            std::array<uint8_t, 32> head{};  // the stream's first bytes: a changed object is read again
            uint32_t table = 0;              // the guest address of VU 0x39's data (the GIF tags, then the slots)
            uint32_t slots = 0;              // material slots after the 7 quadwords at VU 0x39-0x3F
            uint32_t fanTag = 0;             // the guest address of VU 0x002's data: the clipped triangles' fan tag
            std::vector<Batch> batches;
        };

        std::unordered_map<uint32_t, Mesh> g_meshes; // by object address
        std::vector<int32_t> g_gameIds;              // VENDETTA_ARENA_GAMEIDS: objects left to the game (a test)
        uint32_t g_stress = 1, g_texScale = 1;       // VENDETTA_ARENA_STRESS / _TEXSCALE (speed tests)
        uint32_t g_split = 1;                        // VENDETTA_ARENA_SPLIT: each drawn triangle as N (a speed test)

        // Our copy of GS memory: the arena's part 1 replayed into it.
        std::vector<uint8_t> g_vram;
        uint32_t g_uploadLow = 0, g_uploadHigh = 0;  // the blocks the arena's uploads wrote [low, high)
        std::unordered_map<uint64_t, uint32_t> g_images; // TEX0 (texture bits) -> host image id
        uint32_t g_nextImage = kImageIdBase;

        struct Stats
        {
            uint64_t objects = 0, triangles = 0, clipped = 0, culled = 0, gameObjects = 0, badSlots = 0, skippedBatches = 0;
        } g_stats;
        Clock::time_point g_lastStats = Clock::now();
        bool g_abOurs = false;

        // Main memory or the scratchpad (the venue routines build the matrix on the scratchpad's stack)
        const uint8_t *guest(const uint8_t *rdram, uint32_t address)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            return p ? p : rdram + (address & PS2_RAM_MASK);
        }

        float readFloat(const uint8_t *rdram, uint32_t address)
        {
            float value;
            std::memcpy(&value, guest(rdram, address), sizeof(value));
            return value;
        }

        GSGpuRenderer *gpu() { return g_runtime ? g_runtime->gs().gpuRendererForSettings() : nullptr; }

        // ------------------------------------------------------------------ reading an object's stream

        // A batch: {bounding box (2 quadwords), {strips, loose, vertices, material slot}, vertices, strips, loose}.
        // Kernel 0xC: a vertex is 3 quadwords {(s, t, 1, 1), colour, position}; 0x10: 2 {(s, t, 1, 1), position}.
        bool readBatch(const uint8_t *d, uint32_t nq, uint32_t top, uint32_t stride, Batch &batch)
        {
            if (nq < 3)
                return false;
            uint32_t q2[4];
            std::memcpy(q2, d + 32, 16);
            std::memcpy(batch.boxMin, d, 16);
            std::memcpy(batch.boxMax, d + 16, 16);
            const uint32_t strips = q2[0], loose = q2[1], count = q2[2];
            batch.material = q2[3];
            const uint32_t bytes = nq * 16u;
            uint32_t w = 48u + count * 16u * stride;
            if (w > bytes)
                return false;
            batch.vertices.resize(count);
            for (uint32_t k = 0; k < count; ++k)
            {
                const uint8_t *v = d + 48u + k * 16u * stride;
                float st[4];
                std::memcpy(st, v, 16);
                Vertex &out = batch.vertices[k];
                out.s = st[0];
                out.t = st[1];
                out.sz = st[2];
                if (stride == 3u)
                    std::memcpy(out.rgba, v + 16, 16);
                else
                    std::fill(out.rgba, out.rgba + 4, 0.0f);
                std::memcpy(out.pos, v + 16u * (stride - 1u), 16);
            }
            const uint32_t first = top + 3u;
            auto index = [&](uint32_t address, uint16_t &out) {
                if (address < first || (address - first) % stride || (address - first) / stride >= count)
                    return false;
                out = static_cast<uint16_t>((address - first) / stride);
                return true;
            };
            for (uint32_t s = 0; s < strips; ++s)
            {
                if (w + 4u > bytes)
                    return false;
                uint32_t n;
                std::memcpy(&n, d + w, 4);
                w += 4u;
                if (w + 4u * n > bytes)
                    return false;
                std::vector<uint16_t> ix(n);
                std::vector<char> good(n);
                for (uint32_t i = 0; i < n; ++i)
                {
                    uint32_t a;
                    std::memcpy(&a, d + w + 4u * i, 4);
                    good[i] = index(a, ix[i]) ? 1 : 0;
                }
                w += 4u * n;
                // the strip, split where an index names no vertex (none does in the game's files)
                for (uint32_t i = 0; i < n;)
                {
                    uint32_t j = i;
                    while (j < n && good[j])
                        ++j;
                    if (j - i >= 3u)
                    {
                        batch.strips.insert(batch.strips.end(), ix.begin() + i, ix.begin() + j);
                        batch.lengths.push_back(static_cast<uint16_t>(j - i));
                    }
                    i = j + 1u;
                }
            }
            for (uint32_t l = 0; l < loose; ++l)
            {
                if (w + 12u > bytes)
                    return false;
                uint16_t ix[3];
                bool ok = true;
                for (uint32_t i = 0; i < 3; ++i)
                {
                    uint32_t a;
                    std::memcpy(&a, d + w + 4u * i, 4);
                    ok = index(a, ix[i]) && ok;
                }
                w += 12u;
                if (ok)
                    batch.loose.insert(batch.loose.end(), ix, ix + 3);
            }
            return true;
        }

        // The VIF1 stream at object + 0x80: the slot table's UNPACK at VU 0x39, and each batch (an UNPACK V4-32 to
        // VU 0 with FLG, then MSCALF). Batch k is at VU 0x200 (even) or 0x100 (odd), the kernel's double buffer.
        void readMesh(const uint8_t *rdram, uint32_t object, Mesh &mesh)
        {
            mesh.ours = false;
            mesh.kernel = 0;
            mesh.batches.clear();
            mesh.qwc = Ps2FastRead32(rdram, object);
            const uint32_t start = object + 0x80u;
            std::memcpy(mesh.head.data(), guest(rdram, start), mesh.head.size());
            const uint32_t end = start + mesh.qwc * 16u;
            const uint8_t *d = guest(rdram, 0);
            uint32_t o = start;
            uint32_t pending = 0, pendingQ = 0, k = 0;
            bool otherKernel = false;
            while (o + 4u <= end)
            {
                const uint32_t code = Ps2FastRead32(rdram, o);
                const uint32_t imm = code & 0xFFFFu, num = (code >> 16) & 0xFFu, cmd = (code >> 24) & 0x7Fu;
                o += 4u;
                if (cmd >= 0x60u)
                {
                    const uint32_t vn = (cmd >> 2) & 3u, vl = cmd & 3u, n = num ? num : 256u;
                    const uint32_t size = ((n * (vn + 1u) * (32u >> vl) / 8u) + 3u) & ~3u;
                    const uint32_t address = imm & 0x3FFu;
                    if (vn == 3u && vl == 0u)
                    {
                        if ((imm & 0x8000u) && address == 0u)
                        {
                            pending = o;
                            pendingQ = n;
                        }
                        else if (address == 2u && n == 1u)
                            mesh.fanTag = o;
                        else if (address == 0x39u && n > 7u)
                        {
                            mesh.table = o;
                            mesh.slots = n - 7u;
                        }
                    }
                    o += size;
                    continue;
                }
                uint32_t extra = 0;
                switch (cmd)
                {
                case 0x20: extra = 4; break;                    // STMASK
                case 0x30: case 0x31: extra = 16; break;        // STROW, STCOL
                case 0x4A: extra = (num ? num : 256u) * 8u; break; // MPG
                case 0x50: case 0x51: extra = (imm ? imm : 65536u) * 16u; break; // DIRECT, DIRECTHL
                case 0x14: case 0x15: case 0x17:                // MSCAL, MSCALF, MSCNT
                    if (cmd != 0x17 && imm != 0 && pending)
                    {
                        if ((imm != 0xCu && imm != 0x10u) || (mesh.kernel && imm != mesh.kernel))
                            otherKernel = true;
                        else
                        {
                            mesh.kernel = imm;
                            Batch batch;
                            batch.top = (k % 2u) ? 0x100u : 0x200u;
                            batch.data = pending;
                            batch.nq = pendingQ;
                            if (readBatch(d + (pending & PS2_RAM_MASK), pendingQ, batch.top, imm == 0xCu ? 3u : 2u, batch))
                                mesh.batches.push_back(std::move(batch));
                            else
                                otherKernel = true;
                        }
                        ++k;
                        pending = 0;
                    }
                    break;
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
                case 0x10: case 0x11: case 0x13:
                    break;
                default:
                    otherKernel = true; // not a VIF code: not a stream we know
                    break;
                }
                if (otherKernel)
                    break;
                o += extra;
            }
            mesh.ours = !otherKernel && mesh.table != 0u && !mesh.batches.empty();
            // What the kernel reads as a batch's material: the latest write to that VU address, this batch's own data
            // or an earlier one's, else the slot table. A table longer than 0xC7 slots reaches VU 0x100, where the odd
            // batches are unpacked: 6 batches of The Dragon House and Club Luda read a slot an earlier batch overwrote
            // and draw with what is there (a crowd card's other picture), as on the PS2.
            for (size_t i = 0; i < mesh.batches.size(); ++i)
            {
                Batch &b = mesh.batches[i];
                const uint32_t a = b.material;
                b.slot = 0;
                for (size_t j = i + 1; j-- > 0;)
                    if (a >= mesh.batches[j].top && a < mesh.batches[j].top + mesh.batches[j].nq)
                    {
                        b.slot = mesh.batches[j].data + (a - mesh.batches[j].top) * 16u;
                        break;
                    }
                if (!b.slot && a >= 0x40u && a < 0x40u + mesh.slots)
                    b.slot = mesh.table + (a - 0x39u) * 16u;
            }
        }

        Mesh &meshFor(const uint8_t *rdram, uint32_t object)
        {
            Mesh &mesh = g_meshes[object];
            const uint32_t qwc = Ps2FastRead32(rdram, object);
            if (mesh.qwc != qwc || std::memcmp(mesh.head.data(), guest(rdram, object + 0x80u), mesh.head.size()) != 0)
            {
                readMesh(rdram, object, mesh);
                if (g_log)
                {
                    size_t tris = 0, verts = 0;
                    for (const Batch &b : mesh.batches)
                    {
                        tris += b.triangles();
                        verts += b.vertices.size();
                    }
                    std::cerr << "[arenas] object 0x" << std::hex << object << std::dec << ": "
                              << (mesh.ours ? "ours" : "the game's (another kernel)") << ", " << mesh.batches.size()
                              << " batches, " << verts << " vertices, " << tris << " triangles" << std::endl;
                }
            }
            return mesh;
        }

        bool inScene(const uint8_t *rdram, uint32_t object)
        {
            const uint32_t scene = Ps2FastRead32(rdram, kScenePointer);
            return scene && object >= scene && object < scene + kSceneBytes;
        }

        // ------------------------------------------------------------------ the textures

        // The arena's part 1 (GIF A+D BITBLTBUF / TRXPOS / TRXREG / TRXDIR + IMAGE, per texture and per CLUT) written
        // into our copy of GS memory the way the GS lays it out.
        void replayUploads(const uint8_t *data, uint32_t bytes)
        {
            static std::once_flag tables;
            std::call_once(tables, [] { GSMem::InitLookupTables(); });
            g_vram.assign(GSMem::MEMORY_SIZE, 0);
            g_uploadLow = 0x4000u;
            g_uploadHigh = 0u;
            uint32_t dbp = 0, dbw = 0, dpsm = 0, dsax = 0, dsay = 0, rrw = 0, rrh = 0, x = 0, y = 0;
            uint32_t o = 0, uploads = 0;
            while (o + 16u <= bytes)
            {
                uint64_t lo, hi;
                std::memcpy(&lo, data + o, 8);
                std::memcpy(&hi, data + o + 8, 8);
                o += 16u;
                const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
                const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
                const uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu) ? static_cast<uint32_t>((lo >> 60) & 0xFu) : 16u;
                if (flg == 0u)
                {
                    for (uint32_t i = 0; i < nloop * nreg && o + 16u <= bytes; ++i, o += 16u)
                    {
                        if (((hi >> (4u * (i % nreg))) & 0xFu) != 0xEu)
                            continue;
                        uint64_t value, reg;
                        std::memcpy(&value, data + o, 8);
                        std::memcpy(&reg, data + o + 8, 8);
                        switch (reg & 0xFFu)
                        {
                        case 0x50:
                            dbp = static_cast<uint32_t>((value >> 32) & 0x3FFFu);
                            dbw = static_cast<uint32_t>((value >> 48) & 0x3Fu);
                            dpsm = static_cast<uint32_t>((value >> 56) & 0x3Fu);
                            break;
                        case 0x51:
                            dsax = static_cast<uint32_t>((value >> 32) & 0x7FFu);
                            dsay = static_cast<uint32_t>((value >> 48) & 0x7FFu);
                            break;
                        case 0x52:
                            rrw = static_cast<uint32_t>(value & 0xFFFu);
                            rrh = static_cast<uint32_t>((value >> 32) & 0xFFFu);
                            break;
                        case 0x53:
                            x = y = 0;
                            break;
                        }
                    }
                }
                else if (flg == 2u)
                {
                    const uint32_t end = std::min(bytes, o + nloop * 16u);
                    const uint32_t bpp = dpsm == 0x14u ? 4u : dpsm == 0x13u ? 8u : (dpsm == 0x02u || dpsm == 0x0Au) ? 16u : 32u;
                    auto put = [&](uint32_t value) {
                        if (!rrw || y >= rrh)
                            return;
                        const uint32_t px = dsax + x, py = dsay + y;
                        uint8_t *v = g_vram.data();
                        switch (dpsm)
                        {
                        case 0x00: GSMem::WriteCT32(v, dbp, dbw, px, py, value); break;
                        case 0x01: GSMem::WriteCT24(v, dbp, dbw, px, py, value); break;
                        case 0x02: GSMem::WriteCT16(v, dbp, dbw, px, py, value); break;
                        case 0x0A: GSMem::WriteCT16S(v, dbp, dbw, px, py, value); break;
                        case 0x13: GSMem::WriteP8(v, dbp, dbw, px, py, value); break;
                        case 0x14: GSMem::WriteP4(v, dbp, dbw, px, py, value); break;
                        default: break;
                        }
                        if (++x >= rrw)
                        {
                            x = 0;
                            ++y;
                        }
                    };
                    for (uint32_t p = o; p < end; p += 4u)
                    {
                        uint32_t word;
                        std::memcpy(&word, data + p, 4);
                        if (bpp == 32u)
                            put(word);
                        else if (bpp == 16u)
                        {
                            put(word & 0xFFFFu);
                            put(word >> 16);
                        }
                        else if (bpp == 8u)
                            for (uint32_t b = 0; b < 4u; ++b)
                                put((word >> (8u * b)) & 0xFFu);
                        else
                            for (uint32_t b = 0; b < 8u; ++b)
                                put((word >> (4u * b)) & 0xFu);
                    }
                    g_uploadLow = std::min(g_uploadLow, dbp);
                    g_uploadHigh = std::max(g_uploadHigh, dbp + 1u);
                    ++uploads;
                    o = end;
                }
                else
                    o += nloop * 16u; // REGLIST: none in the arena's files
                if (lo & 0x8000u)
                    break; // EOP
            }
            if (g_log)
                std::cerr << "[arenas] textures: " << uploads << " uploads replayed, blocks 0x" << std::hex << g_uploadLow
                          << "-0x" << g_uploadHigh << std::dec << std::endl;
        }

        // One texture (TEX0) decoded from our copy into RGBA8 with the GS's alpha (0x80 = opaque). T4 / T8 with a
        // CT32 CLUT (CSM1) and CT32: what the arenas use.
        bool decode(uint64_t tex0, std::vector<uint32_t> &rgba, uint32_t &w, uint32_t &h)
        {
            const uint32_t tbp = static_cast<uint32_t>(tex0 & 0x3FFFu), tbw = static_cast<uint32_t>((tex0 >> 14) & 0x3Fu);
            const uint32_t psm = static_cast<uint32_t>((tex0 >> 20) & 0x3Fu);
            const uint32_t tw = static_cast<uint32_t>((tex0 >> 26) & 0xFu), th = static_cast<uint32_t>((tex0 >> 30) & 0xFu);
            const uint32_t cbp = static_cast<uint32_t>((tex0 >> 37) & 0x3FFFu), cpsm = static_cast<uint32_t>((tex0 >> 51) & 0xFu);
            const uint32_t csm = static_cast<uint32_t>((tex0 >> 55) & 1u);
            if (g_vram.empty() || tw > 10u || th > 10u || tbp < g_uploadLow || tbp >= g_uploadHigh)
                return false;
            if ((psm == 0x13u || psm == 0x14u) && (cpsm != 0u || csm != 0u))
                return false;
            if (psm != 0x00u && psm != 0x13u && psm != 0x14u)
                return false;
            w = 1u << tw;
            h = 1u << th;
            uint8_t *v = g_vram.data();
            uint32_t clut[256] = {};
            if (psm == 0x13u)
                for (uint32_t i = 0; i < 256u; ++i)
                {
                    const uint32_t j = (i & 0xE7u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1);
                    clut[i] = GSMem::ReadCT32(v, cbp, 1, j & 0xFu, j >> 4);
                }
            else if (psm == 0x14u)
                for (uint32_t i = 0; i < 16u; ++i)
                    clut[i] = GSMem::ReadCT32(v, cbp, 1, i & 7u, i >> 3);
            rgba.resize(size_t(w) * h);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    uint32_t c;
                    if (psm == 0x13u)
                        c = clut[GSMem::ReadP8(v, tbp, tbw, x, y) & 0xFFu];
                    else if (psm == 0x14u)
                        c = clut[GSMem::ReadP4(v, tbp, tbw, x, y) & 0xFu];
                    else
                        c = GSMem::ReadCT32(v, tbp, tbw, x, y);
                    rgba[size_t(y) * w + x] = c;
                }
            return true;
        }

        // The host image for a TEX0 (decoded on first use), 0 when there is none.
        uint32_t imageFor(uint64_t tex0)
        {
            // the bits that pick the texels and colours (not TCC / TFX / CLD)
            const uint64_t key = tex0 & ((0x3FFFFFFFFull) | (0x3FFFull << 37) | (0xFull << 51) | (1ull << 55) | (0x1Full << 56));
            auto found = g_images.find(key);
            if (found != g_images.end())
                return found->second;
            GSGpuRenderer *renderer = gpu();
            std::vector<uint32_t> rgba;
            uint32_t w = 0, h = 0;
            uint32_t id = 0;
            if (renderer && decode(tex0, rgba, w, h))
            {
                if (g_texScale > 1u)
                {
                    gsResizePicture(rgba, w, h, w * g_texScale, h * g_texScale);
                    w *= g_texScale;
                    h *= g_texScale;
                }
                id = g_nextImage++;
                renderer->setHostImage(id, w, h, std::move(rgba));
            }
            g_images[key] = id;
            if (g_log)
                std::cerr << "[arenas] texture TBP 0x" << std::hex << (tex0 & 0x3FFFu) << " PSM 0x" << ((tex0 >> 20) & 0x3F)
                          << std::dec << " " << w << "x" << h << (id ? ": host image" : ": the game's") << std::endl;
            return id;
        }

        void dropImages()
        {
            if (GSGpuRenderer *renderer = gpu())
                for (const auto &[key, id] : g_images)
                    if (id)
                        renderer->dropHostImage(id);
            g_images.clear();
        }

        // ------------------------------------------------------------------ the draw

        // The game's VU1 kernel, step for step (overlay A, kernel 0xC; overlay C, kernel 0x10, clips the same way), so
        // each triangle comes out as the game's does: drawn as it is, skipped, or cut and sent as a fan.
        //   the transform loop: p = position x M, Q = 1 / p.w, (x, y) = p.xy x Q, ST = (s, t, z, Q), colour x scale
        //   per triangle: every corner past the near limit (p.z > 1048574, VU 0x34.z): skipped; any corner past it:
        //   cut at it (edges a-b, b-c, c-a; VU 0x25F8), then to the screen; a corner more than 2047 from the centre
        //   (VU 0x33.w): cut to the screen (0x2588); else drawn. Cutting to the screen (0x2B10): four passes, y >= -112,
        //   y <= 112, x >= -256, x <= 256 about 2048 (VU 0x35-0x38), each edge from its first corner; the result goes
        //   out as a fan (the tag at VU 0x002) before the batch's strips. All in floats in the kernel's order (the
        //   recomp's VU1 rounds as IEEE floats, one rounding per step).

        // DIV as the VU does it (a zero divisor gives the largest float with the sign)
        float vuDiv(float num, float den)
        {
            if (den == 0.0f)
                return std::signbit(num) != std::signbit(den) ? -std::numeric_limits<float>::max()
                                                               : std::numeric_limits<float>::max();
            return num / den;
        }

        // FTOI0 / FTOI4: scaled in double, truncated, held to the int32 range
        int32_t ftoi(float value, double scale)
        {
            const double scaled = static_cast<double>(value) * scale;
            if (scaled >= 2147483647.0)
                return std::numeric_limits<int32_t>::max();
            if (scaled <= -2147483648.0)
                return std::numeric_limits<int32_t>::min();
            return static_cast<int32_t>(scaled);
        }

        constexpr float kNearZ = 1048574.0f;   // VU 0x34.z
        constexpr float kGuard = 2047.0f;      // VU 0x33.w
        constexpr float kHalfW = 256.0f;       // VU 0x37 / 0x38: x -256 / +256 about 2048
        constexpr float kHalfH = 112.0f;       // VU 0x35 / 0x36: y -112 / +112

        // CLIP's flags (bit 0 x+, 1 x-, 2 y+, 3 y-, 4 z+, 5 z-) of (x, y, z) against |w|, as the VU compares them: on
        // the bits (a w that is zero or denormal gives the largest denormal as the limit)
        uint32_t clipFlags(float x, float y, float z, float w)
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

        enum class BatchPath
        {
            Skip,  // nothing of it drawn
            Fast,  // every triangle drawn as it is, untested
            Slow,  // each triangle tested: drawn, skipped or cut
        };

        // The kernel's first step (overlay A 0x0060-0x06D0): the batch's bounding box, its 8 corners transformed
        // and tested in two groups of 4 (CLIP's flags hold the last 4): a corner past the far end (z < 0) or every
        // corner past the near limit: skipped; a corner past the near limit: Slow; then each screen edge in turn
        // (y -112, y +112, x -256, x +256): every corner beyond it: skipped, some: the guard band test; all within the
        // screen: Fast; the guard band (2047): a corner beyond it: Slow, else Fast.
        BatchPath boxTest(const float *lo, const float *hi, const float *m)
        {
            const float corner[8][4] = {
                {lo[0], lo[1], lo[2], lo[3]}, {hi[0], hi[1], hi[2], hi[3]},
                {lo[0], hi[1], lo[2], lo[3]}, {hi[0], lo[1], hi[2], hi[3]},
                {lo[0], lo[1], hi[2], lo[3]}, {hi[0], hi[1], lo[2], hi[3]},
                {lo[0], hi[1], hi[2], lo[3]}, {hi[0], lo[1], lo[2], hi[3]},
            };
            float sx[8], sy[8], sz[8];
            for (int c = 0; c < 8; ++c)
            {
                float p[4];
                for (int j = 0; j < 4; ++j)
                    p[j] = corner[c][0] * m[j] + corner[c][1] * m[4 + j] + corner[c][2] * m[8 + j] + corner[c][3] * m[12 + j];
                const float q = vuDiv(1.0f, p[3]);
                sx[c] = p[0] * q - 2048.0f;
                sy[c] = p[1] * q - 2048.0f;
                sz[c] = p[2];
            }
            auto group = [&](int first, float w, uint32_t bit, bool &all, bool &any) {
                all = true;
                any = false;
                for (int c = first; c < first + 4; ++c)
                {
                    const bool out = (clipFlags(sx[c], sy[c], sz[c], w) & bit) != 0u;
                    all = all && out;
                    any = any || out;
                }
            };
            bool allA, anyA, allB, anyB;
            group(0, 0.0f, 0x20u, allA, anyA); // z-: past the far end
            if (anyA)
                return BatchPath::Skip;
            group(4, 0.0f, 0x20u, allB, anyB);
            if (anyB)
                return BatchPath::Skip;
            group(0, kNearZ, 0x10u, allA, anyA); // z+: past the near limit
            group(4, kNearZ, 0x10u, allB, anyB);
            if (allA)
            {
                if (allB)
                    return BatchPath::Skip;
                if (anyB)
                    return BatchPath::Slow;
            }
            else if (anyA || anyB)
                return BatchPath::Slow;
            const struct
            {
                float w;
                uint32_t bit;
            } edges[4] = {{-kHalfH, 0x08u}, {kHalfH, 0x04u}, {-kHalfW, 0x02u}, {kHalfW, 0x01u}};
            bool guard = false;
            for (const auto &edge : edges)
            {
                group(0, edge.w, edge.bit, allA, anyA);
                group(4, edge.w, edge.bit, allB, anyB);
                if (allA && allB)
                    return BatchPath::Skip;
                if (anyA || anyB)
                {
                    guard = true;
                    break;
                }
            }
            if (!guard)
                return BatchPath::Fast;
            for (int c = 0; c < 8; ++c)
                if (clipFlags(sx[c], sy[c], sz[c], kGuard) & 0x0Fu)
                    return BatchPath::Slow;
            return BatchPath::Fast;
        }

        // One vertex after the transform loop
        struct Corner
        {
            float st[4];   // (s, t, z, Q)
            float col[4];  // colour x scale
            float p[4];    // clip space
            float x, y;    // p.xy x Q
            bool pastNear, guard;
        };

        // One vertex in the clipper's buffer (VU 3 / 27): STQ, the colour, (x - 2048, y - 2048, z, w)
        struct ClipVertex
        {
            float stq[4], col[4], pos[4];
        };

        ClipVertex fromCorner(const Corner &c)
        {
            ClipVertex v;
            v.stq[0] = c.st[0] * c.st[3];
            v.stq[1] = c.st[1] * c.st[3];
            v.stq[2] = c.st[2] * c.st[3];
            v.stq[3] = c.st[3];
            std::copy(c.col, c.col + 4, v.col);
            v.pos[0] = c.x - 2048.0f;
            v.pos[1] = c.y - 2048.0f;
            v.pos[2] = c.p[2];
            v.pos[3] = c.p[3];
            return v;
        }

        // (b - a) x t + a on each lane, as SUB, MULq, ADD
        void lerp4(const float *a, const float *b, float t, float *out)
        {
            for (int k = 0; k < 4; ++k)
            {
                const float d = b[k] - a[k];
                const float dt = d * t;
                out[k] = dt + a[k];
            }
        }

        // The near limit over a triangle's edges a-b, b-c, c-a; returns the vertices written
        int clipNear(const Corner *const corner[3], ClipVertex *out)
        {
            int n = 0;
            for (int e = 0; e < 3; ++e)
            {
                const Corner &a = *corner[e], &b = *corner[(e + 1) % 3];
                if (!a.pastNear)
                    out[n++] = fromCorner(a);
                if (a.pastNear == b.pastNear)
                    continue;
                const float num = kNearZ - a.p[2];
                const float t = vuDiv(num, b.p[2] - a.p[2]);
                float p[4], col[4], st[4];
                lerp4(a.p, b.p, t, p);
                lerp4(a.col, b.col, t, col);
                lerp4(a.st, b.st, t, st);
                const float q = vuDiv(1.0f, p[3]);
                ClipVertex &v = out[n++];
                const float px = p[0] * q, py = p[1] * q;
                v.pos[0] = px - 2048.0f;
                v.pos[1] = py - 2048.0f;
                v.pos[2] = p[2];
                v.pos[3] = p[3];
                v.stq[0] = st[0] * q;
                v.stq[1] = st[1] * q;
                v.stq[2] = st[2] * q;
                v.stq[3] = st[3];
                std::copy(col, col + 4, v.col);
            }
            return n;
        }

        // One pass of the screen cut: lane `axis` of pos against `plane` (outside: beyond it, away from the centre)
        int clipPass(const ClipVertex *in, int n, ClipVertex *out, int axis, float plane)
        {
            const float limit = std::fabs(plane);
            auto outside = [&](const ClipVertex &v) { return plane < 0.0f ? v.pos[axis] < -limit : v.pos[axis] > limit; };
            int m = 0;
            for (int i = 0; i < n; ++i)
            {
                const ClipVertex &cur = in[i], &next = in[(i + 1) % n];
                const bool curOut = outside(cur), nextOut = outside(next);
                if (curOut && nextOut)
                    continue;
                if (!curOut)
                    out[m++] = cur;
                if (curOut == nextOut)
                    continue;
                const float num = plane - cur.pos[axis];
                const float t = vuDiv(num, next.pos[axis] - cur.pos[axis]);
                ClipVertex &v = out[m++];
                lerp4(cur.stq, next.stq, t, v.stq);
                lerp4(cur.col, next.col, t, v.col);
                lerp4(cur.pos, next.pos, t, v.pos);
            }
            return m;
        }

        // A triangle cut as the game cuts it (near limit if any corner is past it, then the screen): the fan's
        // vertices as GS words appended to out; returns how many
        uint32_t clipTriangle(const Corner &a, const Corner &b, const Corner &c, std::vector<uint64_t> &out)
        {
            ClipVertex one[16], two[16];
            const Corner *const corners[3] = {&a, &b, &c};
            int n;
            if (a.pastNear || b.pastNear || c.pastNear)
                n = clipNear(corners, one);
            else
            {
                one[0] = fromCorner(a);
                one[1] = fromCorner(b);
                one[2] = fromCorner(c);
                n = 3;
            }
            if (n)
                n = clipPass(one, n, two, 1, -kHalfH);
            if (n)
                n = clipPass(two, n, one, 1, kHalfH);
            if (n)
                n = clipPass(one, n, two, 0, -kHalfW);
            if (n)
                n = clipPass(two, n, one, 0, kHalfW);
            for (int i = 0; i < n; ++i)
            {
                const ClipVertex &v = one[i];
                uint32_t sb, tb, qb;
                std::memcpy(&sb, &v.stq[0], 4);
                std::memcpy(&tb, &v.stq[1], 4);
                std::memcpy(&qb, &v.stq[2], 4);
                out.push_back(uint64_t(sb) | (uint64_t(tb) << 32)); // ST: S, T, Q
                out.push_back(uint64_t(qb));
                auto byte = [](float c) { return uint64_t(uint32_t(ftoi(c, 1.0)) & 0xFFu); };
                out.push_back(byte(v.col[0]) | (byte(v.col[1]) << 32)); // RGBAQ
                out.push_back(byte(v.col[2]) | (byte(v.col[3]) << 32));
                const float x = v.pos[0] + 2048.0f, y = v.pos[1] + 2048.0f, z = v.pos[2] + 0.0f;
                out.push_back(uint64_t(uint32_t(ftoi(x, 16.0)) & 0xFFFFu) |
                              (uint64_t(uint32_t(ftoi(y, 16.0)) & 0xFFFFu) << 32)); // XYZF2
                out.push_back(uint64_t(uint32_t(ftoi(z, 16.0)) & 0x0FFFFFF0u));
            }
            return n >= 3 ? static_cast<uint32_t>(n) : 0u;
        }

        // A vertex drawn as it is (the kernel's output loop): ST = (s, t, z) x Q, the colour truncated, XYZF2 =
        // FTOI4(x, y, p.z)
        void packCorner(uint64_t *out, const Corner &c)
        {
            const float sq = c.st[0] * c.st[3], tq = c.st[1] * c.st[3], q = c.st[2] * c.st[3];
            uint32_t sb, tb, qb;
            std::memcpy(&sb, &sq, 4);
            std::memcpy(&tb, &tq, 4);
            std::memcpy(&qb, &q, 4);
            out[0] = uint64_t(sb) | (uint64_t(tb) << 32);
            out[1] = uint64_t(qb);
            auto byte = [](float v) { return uint64_t(uint32_t(ftoi(v, 1.0)) & 0xFFu); };
            out[2] = byte(c.col[0]) | (byte(c.col[1]) << 32);
            out[3] = byte(c.col[2]) | (byte(c.col[3]) << 32);
            out[4] = uint64_t(uint32_t(ftoi(c.x, 16.0)) & 0xFFFFu) | (uint64_t(uint32_t(ftoi(c.y, 16.0)) & 0xFFFFu) << 32);
            out[5] = uint64_t(uint32_t(ftoi(c.p[2], 16.0)) & 0x0FFFFFF0u);
        }

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

        // VENDETTA_ARENADRAW=flip: which draw is on, F9 to swap
        bool g_flipOurs = true;
        bool g_flipTitled = false;

#if defined(_WIN32)
        bool ourWindowInFront()
        {
            DWORD pid = 0;
            const HWND front = GetForegroundWindow();
            return front && GetWindowThreadProcessId(front, &pid) && pid == GetCurrentProcessId();
        }

        // The window title with " | Arena: ..." at the end, set from a thread of its own: the window belongs to another
        // thread, and the game thread must never wait for it.
        void showFlipInTitle(bool ours)
        {
            std::thread([ours] {
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
                wchar_t text[512] = {};
                SendMessageTimeoutW(find.window, WM_GETTEXT, 512, reinterpret_cast<LPARAM>(text), SMTO_ABORTIFHUNG, 500, nullptr);
                std::wstring title = text;
                const size_t at = title.find(L" | Arena: ");
                if (at != std::wstring::npos)
                    title.resize(at);
                title += ours ? L" | Arena: ours (F9: the game's)" : L" | Arena: the game's (F9: ours)";
                SendMessageTimeoutW(find.window, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(title.c_str()), SMTO_ABORTIFHUNG, 500,
                                    nullptr);
            }).detach();
        }
#endif

        bool flipNow()
        {
#if defined(_WIN32)
            static auto lastPoll = Clock::now();
            static bool wasDown = false;
            const auto now = Clock::now();
            if (!g_flipTitled)
            {
                g_flipTitled = true;
                showFlipInTitle(g_flipOurs);
            }
            if (now - lastPoll >= std::chrono::milliseconds(30))
            {
                lastPoll = now;
                const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0 && ourWindowInFront();
                if (down && !wasDown)
                {
                    g_flipOurs = !g_flipOurs;
                    std::cerr << "[arenas] F9: " << (g_flipOurs ? "ours" : "the game's") << std::endl;
                    showFlipInTitle(g_flipOurs);
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
                std::cerr << "[arenas] ab " << t << " s: " << (ours ? "ours" : "the game's") << std::endl;
            }
            return ours;
        }

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

        std::vector<GsTriangle> g_ourTriangles;
        struct CheckStats
        {
            uint64_t objects = 0, exact = 0, gameTris = 0, ourTris = 0, matched = 0;
        } g_check;
        int g_checkReports = 0;

        // The game's VU1 output for an object arrives partly during the next object's draw: the check collects every
        // path 1 triangle of a window of draws, and each of ours is looked up among this window's and the next one's.
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
        std::vector<GsTriangle> g_gameWindow, g_gamePrevious, g_ourWindow, g_ourPrevious;
        struct TriangleSource
        {
            uint32_t object, ra, call, a1, a2;
        };
        std::vector<TriangleSource> g_ourWindowObject, g_ourPreviousObject;
        bool g_checkStarted = false;

        void gifTap(void *, uint8_t path, const uint8_t *data, uint32_t bytes)
        {
            (void)path; // every path: ours must be among what the game drew
            if (!g_checkStarted)
                return;
            GsReader reader;
            reader.out = &g_gameWindow;
            reader.feed(data, bytes);
        }

        std::string describe(const GsVertex &v)
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

        constexpr size_t kPacketBytes = 64u * 1024u; // a GIF packet of ours is sent at about this size

        // One whole GIF packet (EOP on its last tag) into the game's GS stream, in order.
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

        // An object drawn by us, from what the game put into its VU area (object + 0xB0 = VU 0x39 onwards) for this
        // draw. Kernel 0xC: the GIF tags at 0x39 / 0x3A, the matrix at 0x3B-0x3E, the colour scale at 0x3F, the slots
        // {TEX0, register} from 0x40. Kernel 0x10 (the shadow floor, object 99): TEX0 at 0x39 (the shadow picture),
        // the colour at 0x3A (integers), the camera matrix at 0x3B-0x3E, the shadow matrix at 0x3F-0x42 (s, t =
        // (p x it).xy / w), the GIF tags at 0x43 / 0x44.
        // The VU's rounding (toward zero) while our draw does the kernel's maths, the caller's after
        struct VuRounding
        {
            int saved = std::fegetround();
            VuRounding() { std::fesetround(FE_TOWARDZERO); }
            ~VuRounding() { std::fesetround(saved); }
        };

        void drawOurs(uint8_t *rdram, const Mesh &mesh, bool scrolled, float du, float dv)
        {
            const VuRounding rounding;
            const bool shadow = mesh.kernel == 0x10u;
            auto vu = [&](uint32_t address) { return mesh.table + (address - 0x39u) * 16u; };
            float m[16], m2[16] = {}, scale[4] = {};
            for (int i = 0; i < 16; ++i)
                m[i] = readFloat(rdram, vu(0x3Bu) + 4u * i);
            if (shadow)
                for (int i = 0; i < 16; ++i)
                    m2[i] = readFloat(rdram, vu(0x3Fu) + 4u * i);
            else
                for (int i = 0; i < 4; ++i)
                    scale[i] = readFloat(rdram, vu(0x3Fu) + 4u * i);
            float shadowColour[4] = {};
            if (shadow)
                for (int i = 0; i < 4; ++i)
                    shadowColour[i] = static_cast<float>(Ps2FastRead32(rdram, vu(0x3Au) + 4u * i) & 0xFFu);

            uint64_t tagLo;
            std::memcpy(&tagLo, guest(rdram, vu(shadow ? 0x43u : 0x39u)), 8); // the strips' GIF tag
            const uint64_t primBits = ((tagLo >> 47) & 0x7FFull) & ~7ull;      // IIP TME FGE ABE AA1 FST CTXT FIX
            const uint64_t prim = primBits | 3ull;                             // triangles
            uint64_t fanLo = 0;
            if (mesh.fanTag)
                std::memcpy(&fanLo, guest(rdram, mesh.fanTag), 8);
            const uint64_t fanPrim = mesh.fanTag ? (fanLo >> 47) & 0x7FFull : (primBits | 5ull); // the fan's PRIM

            const bool host = !g_vramTextures && gpu() != nullptr;
            // kept from draw to draw (the game thread only): no allocation once they have grown
            static Packet packet;
            static std::vector<uint64_t> verts, packed, stripWords, fanWords;
            static std::vector<uint32_t> stripTags, fanTags;
            static std::vector<Corner> corners;
            uint64_t lastTex0 = 0, lastReg = 0; // the TEX0 the GS holds after our last batch
            // VENDETTA_ARENA_SPLIT (a speed test): a drawn triangle as N, its edge a-b cut in N, each new corner
            // worked out from clip space like a real vertex
            auto splitTriangle = [&](const Corner &a, const Corner &b, const Corner &c) {
                auto between = [&](float f) {
                    Corner v;
                    for (int j = 0; j < 4; ++j)
                    {
                        v.p[j] = a.p[j] + (b.p[j] - a.p[j]) * f;
                        v.col[j] = a.col[j] + (b.col[j] - a.col[j]) * f;
                    }
                    for (int j = 0; j < 3; ++j)
                        v.st[j] = a.st[j] + (b.st[j] - a.st[j]) * f;
                    v.st[3] = vuDiv(1.0f, v.p[3]);
                    v.x = v.p[0] * v.st[3];
                    v.y = v.p[1] * v.st[3];
                    v.pastNear = v.guard = false;
                    return v;
                };
                uint64_t words[18];
                for (uint32_t i = 0; i < g_split; ++i)
                {
                    packCorner(words, c);
                    packCorner(words + 6, between(float(i) / float(g_split)));
                    packCorner(words + 12, between(float(i + 1) / float(g_split)));
                    verts.insert(verts.end(), words, words + 18);
                }
                g_stats.triangles += g_split;
            };
            for (const Batch &batch : mesh.batches)
            {
                if (batch.lengths.empty() && batch.loose.empty())
                    continue;
                uint64_t tex0, reg;
                if (shadow)
                {
                    std::memcpy(&tex0, guest(rdram, vu(0x39u)), 8);
                    reg = 0x07u;
                }
                else
                {
                    if (!batch.slot)
                    {
                        ++g_stats.badSlots;
                        continue;
                    }
                    std::memcpy(&tex0, guest(rdram, batch.slot), 8);
                    std::memcpy(&reg, guest(rdram, batch.slot + 8u), 8);
                    // The kernel sends the slot's quadword as an A+D write. Where an earlier batch's vertices lie (the
                    // 6 batches above), its register byte is 0x00 (PRIM, which the next tag sets again) or 0xE4 (no
                    // register): the TEX0 stays the last batch's, and the card shows that picture.
                    if ((reg & 0xFFu) != 0x06u && (reg & 0xFFu) != 0x07u)
                    {
                        ++g_stats.badSlots;
                        if (!lastReg)
                            continue;
                        tex0 = lastTex0;
                        reg = lastReg;
                    }
                }
                const BatchPath path = boxTest(batch.boxMin, batch.boxMax, m);
                if (path == BatchPath::Skip)
                {
                    ++g_stats.skippedBatches;
                    continue;
                }
                const size_t count = batch.vertices.size();
                corners.resize(count);
                for (size_t k = 0; k < count; ++k)
                {
                    const Vertex &v = batch.vertices[k];
                    Corner &c = corners[k];
                    for (int j = 0; j < 4; ++j)
                        c.p[j] = v.pos[0] * m[j] + v.pos[1] * m[4 + j] + v.pos[2] * m[8 + j] + v.pos[3] * m[12 + j];
                    const float q = vuDiv(1.0f, c.p[3]);
                    if (shadow)
                    {
                        // overlay C: the shadow picture's (s, t) = (p x M2).xy x 1 / (p x M2).w
                        float st[4];
                        for (int j = 0; j < 4; ++j)
                            st[j] = v.pos[0] * m2[j] + v.pos[1] * m2[4 + j] + v.pos[2] * m2[8 + j] + v.pos[3] * m2[12 + j];
                        const float q2 = vuDiv(1.0f, st[3]);
                        c.st[0] = st[0] * q2;
                        c.st[1] = st[1] * q2;
                        std::copy(shadowColour, shadowColour + 4, c.col);
                    }
                    else
                    {
                        c.st[0] = scrolled ? v.s + du : v.s;
                        c.st[1] = scrolled ? v.t + dv : v.t;
                        for (int j = 0; j < 4; ++j)
                            c.col[j] = v.rgba[j] * scale[j];
                    }
                    c.st[2] = v.sz;
                    c.st[3] = q;
                    c.x = c.p[0] * q;
                    c.y = c.p[1] * q;
                    c.pastNear = (clipFlags(c.x, c.y, c.p[2], kNearZ) & 0x10u) != 0u;
                    c.guard = (clipFlags(c.x - 2048.0f, c.y - 2048.0f, c.p[2], kGuard) & 0x0Fu) != 0u;
                }
                packed.resize(count * 6u);
                for (size_t k = 0; k < count; ++k)
                    packCorner(&packed[k * 6u], corners[k]);
                // each triangle: 0 drawn, 1 skipped, 2 cut (a fan)
                auto kind = [&](uint16_t i, uint16_t j, uint16_t k) {
                    if (path == BatchPath::Fast)
                        return 0;
                    const Corner &a = corners[i], &b = corners[j], &c = corners[k];
                    if (a.pastNear && b.pastNear && c.pastNear)
                        return 1;
                    if (a.pastNear || b.pastNear || c.pastNear || a.guard || b.guard || c.guard)
                        return 2;
                    return 0;
                };
                fanWords.clear();
                fanTags.clear();
                stripWords.clear();
                stripTags.clear();
                verts.clear();
                size_t at = 0;
                for (uint16_t length : batch.lengths)
                {
                    const uint16_t *strip = &batch.strips[at];
                    at += length;
                    int first = -1, last = -1;
                    for (int t = 0; t + 2 < length; ++t)
                    {
                        const int k = kind(strip[t], strip[t + 1], strip[t + 2]);
                        if (k == 0 && g_split > 1)
                            splitTriangle(corners[strip[t]], corners[strip[t + 1]], corners[strip[t + 2]]);
                        else if (k == 0)
                        {
                            if (first < 0)
                                first = t;
                            last = t;
                            ++g_stats.triangles;
                        }
                        else if (k == 1)
                            ++g_stats.culled;
                        else
                        {
                            const size_t before = fanWords.size();
                            const uint32_t n = clipTriangle(corners[strip[t]], corners[strip[t + 1]], corners[strip[t + 2]], fanWords);
                            if (n)
                                fanTags.push_back(n);
                            else
                                fanWords.resize(before);
                            ++g_stats.clipped;
                        }
                    }
                    if (first < 0)
                        continue;
                    // vertices first .. last + 2; a triangle not drawn in between gets ADC on its third vertex
                    stripTags.push_back(static_cast<uint32_t>(last + 3 - first));
                    for (int i = first; i <= last + 2; ++i)
                    {
                        const size_t base = stripWords.size();
                        stripWords.insert(stripWords.end(), &packed[strip[i] * 6u], &packed[strip[i] * 6u] + 6);
                        if (i >= first + 2 && kind(strip[i - 2], strip[i - 1], strip[i]) != 0)
                            stripWords[base + 5] |= 1ull << 47; // ADC: no drawing kick
                    }
                }
                for (size_t i = 0; i + 2 < batch.loose.size(); i += 3)
                {
                    const uint16_t x = batch.loose[i], y = batch.loose[i + 1], z = batch.loose[i + 2];
                    const int k = kind(x, y, z);
                    if (k == 0 && g_split > 1)
                        splitTriangle(corners[x], corners[y], corners[z]);
                    else if (k == 0)
                    {
                        verts.insert(verts.end(), &packed[x * 6u], &packed[x * 6u] + 6);
                        verts.insert(verts.end(), &packed[y * 6u], &packed[y * 6u] + 6);
                        verts.insert(verts.end(), &packed[z * 6u], &packed[z * 6u] + 6);
                        ++g_stats.triangles;
                    }
                    else if (k == 1)
                        ++g_stats.culled;
                    else
                    {
                        const size_t before = fanWords.size();
                        const uint32_t n = clipTriangle(corners[x], corners[y], corners[z], fanWords);
                        if (n)
                            fanTags.push_back(n);
                        else
                            fanWords.resize(before);
                        ++g_stats.clipped;
                    }
                }
                if (verts.empty() && stripTags.empty() && fanTags.empty())
                    continue;
                packet.ad(static_cast<uint32_t>(reg & 0xFFu), tex0);
                lastTex0 = tex0;
                lastReg = reg;
                const uint32_t tbp = static_cast<uint32_t>(tex0 & 0x3FFFu);
                const uint32_t id = host && !shadow ? imageFor(tex0) : 0u;
                if (id)
                    packet.ad(0x71u, uint64_t(tbp) | (uint64_t(id) << 32));
                // the cut triangles first (the kernel kicks each fan as it cuts it), then the strips and the rest
                size_t word = 0;
                for (uint32_t n : fanTags)
                {
                    packet.tag(uint64_t(n) | (1ull << 46) | (fanPrim << 47) | (3ull << 60), 0x412ull); // a fan
                    packet.q.insert(packet.q.end(), fanWords.begin() + word, fanWords.begin() + word + size_t(n) * 6u);
                    word += size_t(n) * 6u;
                }
                word = 0;
                for (uint32_t n : stripTags)
                {
                    packet.tag(uint64_t(n) | (1ull << 46) | ((primBits | 4ull) << 47) | (3ull << 60), 0x412ull); // a strip
                    packet.q.insert(packet.q.end(), stripWords.begin() + word, stripWords.begin() + word + size_t(n) * 6u);
                    word += size_t(n) * 6u;
                }
                const uint32_t vertices = static_cast<uint32_t>(verts.size() / 6u);
                for (uint32_t done = 0; done < vertices;)
                {
                    const uint32_t n = std::min<uint32_t>(vertices - done, 0x7FFEu); // whole triangles
                    packet.tag(uint64_t(n) | (1ull << 46) | (prim << 47) | (3ull << 60), 0x412ull); // ST RGBAQ XYZF2
                    packet.q.insert(packet.q.end(), verts.begin() + size_t(done) * 6u, verts.begin() + size_t(done + n) * 6u);
                    done += n;
                }
                if (id)
                    packet.ad(0x71u, uint64_t(tbp)); // unbound: the game's own draws at this address are untouched
                if (packet.q.size() * 8u >= kPacketBytes)
                    send(packet);
            }
            send(packet);
            ++g_stats.objects;
        }

        void logStats()
        {
            if (!g_log)
                return;
            const auto now = Clock::now();
            if (now - g_lastStats < std::chrono::seconds(5))
                return;
            g_lastStats = now;
            std::cerr << "[arenas] 5 s: " << g_stats.objects << " objects drawn by us, " << g_stats.gameObjects
                      << " by the game; " << g_stats.triangles << " triangles (" << g_stats.clipped << " clipped, "
                      << g_stats.culled << " culled, " << g_stats.badSlots << " batches with a bad slot)" << std::endl;
            g_stats = {};
        }

        enum class Call
        {
            Plain,    // FUN_001b8bf8(object, matrix, colour)
            Scrolled, // FUN_001b9bb0(object, matrix, colour), (u, v) in f12 / f13
            Shadow,   // FUN_001b9880(object, camera matrix, shadow matrix): the shadow floor
        };

        // Writes the draw's values into the object as the game does (+0xD0 the matrix, then + 0x110 the colour or
        // the shadow matrix, + 0x120 the scroll), then draws it ourselves. False: the game's routine draws it.
        bool tryDraw(uint8_t *rdram, R5900Context *ctx, Call call)
        {
            const uint32_t object = getRegU32(ctx, 4);
            if (!inScene(rdram, object))
                return false;
            if (!g_gameIds.empty())
            {
                // the scene's directory at + 0x14: {id, offset from the next word}
                const uint32_t scene = Ps2FastRead32(rdram, kScenePointer);
                const uint32_t count = Ps2FastRead32(rdram, scene);
                for (uint32_t i = 0; i < count && i < 4096u; ++i)
                {
                    const uint32_t entry = scene + 0x14u + 8u * i;
                    if (entry + 4u + Ps2FastRead32(rdram, entry + 4u) == object &&
                        std::find(g_gameIds.begin(), g_gameIds.end(), int32_t(Ps2FastRead32(rdram, entry))) != g_gameIds.end())
                        return false;
                }
            }
            Mesh &mesh = meshFor(rdram, object);
            if (!mesh.ours || mesh.kernel != (call == Call::Shadow ? 0x10u : 0xCu) || mesh.table != object + 0xB0u)
                return false;
            if (g_mode == Mode::Check)
                return false; // the game's draw (checkDraw compares)
            if (!oursNow())
            {
                ++g_stats.gameObjects;
                return false;
            }
            uint8_t *o = rdram + (object & PS2_RAM_MASK);
            std::memcpy(o + 0xD0, guest(rdram, getRegU32(ctx, 5)), 64);
            std::memcpy(o + 0x110, guest(rdram, getRegU32(ctx, 6)), call == Call::Shadow ? 64 : 16);
            if (call == Call::Scrolled)
            {
                std::memcpy(o + 0x120, &ctx->f[12], 4);
                std::memcpy(o + 0x124, &ctx->f[13], 4);
            }
            const bool scrolled = call == Call::Scrolled;
            for (uint32_t i = 0; i < g_stress; ++i)
                drawOurs(rdram, mesh, scrolled, scrolled ? ctx->f[12] : 0.0f, scrolled ? ctx->f[13] : 0.0f);
            logStats();
            return true;
        }

        // VENDETTA_ARENADRAW=check: the game's draw with its packets tapped, then ours worked out from the same values
        // and compared triangle by triangle (positions, depth, ST / Q bits, colour)
        bool checkDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, PS2Runtime::RecompiledFunction original, Call call)
        {
            const uint32_t object = getRegU32(ctx, 4);
            if (g_mode != Mode::Check || !inScene(rdram, object))
                return false;
            Mesh &mesh = meshFor(rdram, object);
            if (!mesh.ours || mesh.kernel != (call == Call::Shadow ? 0x10u : 0xCu) || mesh.table != object + 0xB0u)
                return false;
            const float du = ctx->f[12], dv = ctx->f[13];
            const uint32_t ra = getRegU32(ctx, 31), sp = getRegU32(ctx, 29);
            const uint32_t m1 = getRegU32(ctx, 5), m2 = getRegU32(ctx, 6);
            g_checkStarted = true;
            original(rdram, ctx, runtime);
            if (ctx->pc != ra || getRegU32(ctx, 29) != sp)
                return true; // the call was left early (a scheduler checkpoint): ours not worked out
            // the game wrote the matrix (and colour / shadow matrix / scroll) into the object: ours reads them there
            g_ourTriangles.clear();
            const bool scrolled = call == Call::Scrolled;
            drawOurs(rdram, mesh, scrolled, scrolled ? du : 0.0f, scrolled ? dv : 0.0f);
            g_ourWindow.insert(g_ourWindow.end(), g_ourTriangles.begin(), g_ourTriangles.end());
            g_ourWindowObject.insert(g_ourWindowObject.end(), g_ourTriangles.size(),
                                     {object, ra, static_cast<uint32_t>(call), m1, m2});
            ++g_check.objects;
            static auto last = Clock::now();
            if (Clock::now() - last < std::chrono::seconds(2))
                return true;
            last = Clock::now();
            // the previous window's triangles of ours against the game's of the previous and this window
            std::unordered_set<GsTriangle, TriangleHash, TriangleEq> game(g_gamePrevious.begin(), g_gamePrevious.end());
            game.insert(g_gameWindow.begin(), g_gameWindow.end());
            size_t found = 0, missing = 0;
            // the game's triangles by their corners' positions alone: what differs when only the positions match
            struct PosHash
            {
                size_t operator()(const GsTriangle &t) const
                {
                    uint64_t h = 1469598103934665603ull;
                    for (const GsVertex &v : t.v)
                        for (uint32_t w : {v.x, v.y, v.z})
                            h = (h ^ w) * 1099511628211ull;
                    return static_cast<size_t>(h);
                }
            };
            struct PosEq
            {
                bool operator()(const GsTriangle &a, const GsTriangle &b) const
                {
                    for (int k = 0; k < 3; ++k)
                        if (a.v[k].x != b.v[k].x || a.v[k].y != b.v[k].y || a.v[k].z != b.v[k].z)
                            return false;
                    return true;
                }
            };
            std::unordered_multimap<GsTriangle, const GsTriangle *, PosHash, PosEq> byPosition;
            for (const GsTriangle &g : game)
                byPosition.emplace(g, &g);
            std::map<std::pair<uint32_t, int>, size_t> kinds; // (object, kind) -> count
            int detailed = 0;
            for (size_t ti = 0; ti < g_ourPrevious.size(); ++ti)
            {
                const GsTriangle &t = g_ourPrevious[ti];
                if (game.count(t))
                {
                    ++found;
                    continue;
                }
                ++missing;
                {
                    auto range = byPosition.equal_range(t);
                    int kind = 0; // 0: no game triangle at these positions
                    for (auto it = range.first; it != range.second && kind != 1; ++it)
                    {
                        const GsTriangle &g = *it->second;
                        bool st = false, q = false, col = false;
                        for (int k = 0; k < 3; ++k)
                        {
                            st |= g.v[k].s != t.v[k].s || g.v[k].t != t.v[k].t;
                            q |= g.v[k].q != t.v[k].q;
                            col |= g.v[k].rgba != t.v[k].rgba;
                        }
                        kind = 1 + (st ? 1 : 0) + (q ? 2 : 0) + (col ? 4 : 0);
                        if (detailed < 6 && kind > 1)
                        {
                            ++detailed;
                            std::cerr << "[arenas] check: same positions, kind " << kind << " (2 st, 4 q, 8 colour), object 0x"
                                      << std::hex << g_ourPreviousObject[ti].object << std::dec << std::endl;
                            for (int k = 0; k < 3; ++k)
                                std::cerr << "[arenas]   ours " << describe(t.v[k]) << "\n[arenas]   game " << describe(g.v[k]) << std::endl;
                        }
                    }
                    ++kinds[{g_ourPreviousObject[ti].object, kind}];
                }
                if (g_checkReports < 8)
                {
                    ++g_checkReports;
                    // the game's triangle nearest to it (by its corners' screen positions)
                    const GsTriangle *best = nullptr;
                    int64_t bestDistance = INT64_MAX;
                    for (const GsTriangle &g : game)
                    {
                        int64_t d = 0;
                        for (int k = 0; k < 3; ++k)
                            d += std::llabs(int64_t(g.v[k].x) - t.v[k].x) + std::llabs(int64_t(g.v[k].y) - t.v[k].y);
                        if (d < bestDistance)
                        {
                            bestDistance = d;
                            best = &g;
                        }
                    }
                    const TriangleSource &src = g_ourPreviousObject[ti];
                    std::cerr << "[arenas] check: a triangle of ours the game did not draw: object 0x" << std::hex
                              << src.object << " call " << src.call << " from 0x" << src.ra << " a1 0x" << src.a1
                              << " a2 0x" << src.a2 << std::dec << std::endl;
                    for (int k = 0; k < 3; ++k)
                        std::cerr << "[arenas]   ours " << describe(t.v[k]) << "\n[arenas]   game "
                                  << (best ? describe(best->v[k]) : std::string("-")) << std::endl;
                }
            }
            for (const auto &[key, n] : kinds)
                std::cerr << "[arenas] check:   object 0x" << std::hex << key.first << std::dec << " kind " << key.second
                          << " (0 nowhere, else 1 + 1 st + 2 q + 4 colour): " << n << std::endl;
            std::cerr << "[arenas] check: " << g_ourPrevious.size() << " triangles of ours (" << g_check.objects
                      << " object draws in the window), " << found << " drawn the same by the game, " << missing
                      << " not; the game's path 1 triangles in the two windows: " << game.size() << std::endl;
            g_ourPrevious.swap(g_ourWindow);
            g_ourWindow.clear();
            g_ourPreviousObject.swap(g_ourWindowObject);
            g_ourWindowObject.clear();
            g_gamePrevious.swap(g_gameWindow);
            g_gameWindow.clear();
            g_check = {};
            return true;
        }

        void draw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (checkDraw(rdram, ctx, runtime, g_originalDraw, Call::Plain))
                return;
            if (tryDraw(rdram, ctx, Call::Plain))
            {
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            g_originalDraw(rdram, ctx, runtime);
        }

        void drawScrolled(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (checkDraw(rdram, ctx, runtime, g_originalDrawScrolled, Call::Scrolled))
                return;
            if (tryDraw(rdram, ctx, Call::Scrolled))
            {
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            g_originalDrawScrolled(rdram, ctx, runtime);
        }

        void drawShadow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (checkDraw(rdram, ctx, runtime, g_originalDrawShadow, Call::Shadow))
                return;
            if (tryDraw(rdram, ctx, Call::Shadow))
            {
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            g_originalDrawShadow(rdram, ctx, runtime);
        }

        // A picture card's frame as the game writes it into its slot (FUN_001b8af8 with FUN_001b8ca8 / FUN_001b8cf0):
        // TBP, TBW = (width + 63) / 64, PSM from the bits per pixel (4: T4, 8: T8, 16: CT16, 24: CT24, else CT32),
        // TW / TH = log2 of the size, TCC 1, CBP, CLD 1
        uint64_t frameTex0(uint32_t width, uint32_t height, uint32_t bpp, uint32_t tbp, uint32_t cbp)
        {
            auto log2 = [](uint32_t v) {
                uint32_t n = 0;
                while ((1u << (n + 1)) <= v && n < 15)
                    ++n;
                return n;
            };
            const uint32_t psm = bpp == 4u ? 0x14u : bpp == 8u ? 0x13u : bpp == 16u ? 0x02u : bpp == 24u ? 0x01u : 0x00u;
            const uint32_t th = log2(height), tw = log2(width), tbw = (width + 63u) >> 6;
            const uint32_t lo = tbp | (tbw << 14) | (psm << 20) | (tw << 26) | (th << 30);
            const uint32_t hi = (th >> 2) | 0x20000004u | (cbp << 5);
            return uint64_t(lo) | (uint64_t(hi) << 32);
        }

        // At the arena's load (its loading screen): every object read, and a host image made for every texture any
        // draw of it can use (each slot's TEX0, each picture card frame), so none is made in the middle of the match
        // (making an image holds the graphics card thread for a moment: a dropped picture).
        void prepareArena(uint8_t *rdram)
        {
            const uint32_t scene = Ps2FastRead32(rdram, kScenePointer);
            if (!scene)
                return;
            const uint32_t count = std::min<uint32_t>(Ps2FastRead32(rdram, scene), 4096u);
            const bool host = !g_vramTextures && gpu() != nullptr;
            size_t images = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t entry = scene + 0x14u + 8u * i;
                const int32_t id = static_cast<int32_t>(Ps2FastRead32(rdram, entry));
                const uint32_t object = entry + 4u + Ps2FastRead32(rdram, entry + 4u);
                if (object < scene || object >= scene + kSceneBytes)
                    continue;
                if (id == -1)
                {
                    // the picture cards (BBD): {"BBD ", size, entries, animations, entry offsets, animation offsets}
                    if (!host)
                        continue;
                    const uint32_t entries = Ps2FastRead32(rdram, object + 8u), anims = Ps2FastRead32(rdram, object + 12u);
                    for (uint32_t a = 0; a < anims && a < 4096u; ++a)
                    {
                        const uint32_t at = object + 16u + 4u * (entries + a);
                        const uint32_t anim = at + (Ps2FastRead32(rdram, at) & ~3u);
                        const uint32_t w = Ps2FastRead32(rdram, anim + 0xC), h = Ps2FastRead32(rdram, anim + 0x10);
                        const uint32_t bpp = Ps2FastRead32(rdram, anim + 0x14), frames = Ps2FastRead32(rdram, anim + 0x18);
                        for (uint32_t f = 0; f < frames && f < 4096u; ++f)
                        {
                            const uint32_t frame = anim + 0x1Cu + 12u * f;
                            if (imageFor(frameTex0(w, h, bpp, Ps2FastRead32(rdram, frame), Ps2FastRead32(rdram, frame + 4u))))
                                ++images;
                        }
                    }
                    continue;
                }
                Mesh &mesh = meshFor(rdram, object);
                if (!mesh.ours || mesh.kernel != 0xCu || !host)
                    continue;
                for (const Batch &batch : mesh.batches)
                {
                    if (!batch.slot)
                        continue;
                    uint64_t tex0, reg;
                    std::memcpy(&tex0, guest(rdram, batch.slot), 8);
                    std::memcpy(&reg, guest(rdram, batch.slot + 8u), 8);
                    if (((reg & 0xFFu) == 0x06u || (reg & 0xFFu) == 0x07u) && imageFor(tex0))
                        ++images;
                }
            }
            if (host)
            {
                // a register write the graphics card thread answers by making the queued images now, in the loading
                // screen (0x71 with id 0 on an address nothing binds)
                Packet packet;
                packet.ad(0x71u, 0x3FFFu);
                send(packet);
            }
            if (g_log)
                std::cerr << "[arenas] prepared: " << g_meshes.size() << " objects read, " << g_images.size()
                          << " textures (" << images << " uses)" << std::endl;
        }

        // VENDETTA_ARENALOG: a line each time the match's intros start (a0 = 1 when Survival skips an arena's fly-in it
        // has shown in this run: the "intro skip"), with the match kind and the venue
        void intros(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            std::cerr << "[arenas] intros: FUN_001245c8(a0 = " << getRegU32(ctx, 4) << "), match kind 0x" << std::hex
                      << (Ps2FastRead32(rdram, kMatchKind & ~3u) >> ((kMatchKind & 3u) * 8u) & 0xFFu) << std::dec
                      << ", venue " << Ps2FastRead32(rdram, kVenue) << std::endl;
            g_originalIntros(rdram, ctx, runtime);
        }

        // The arena's textures, read as the game uploads them.
        void upload(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (getRegU32(ctx, 31) == kUploadFromArena)
            {
                const uint32_t p = getRegU32(ctx, 4);
                const uint32_t start = p + (Ps2FastRead32(rdram, p) & ~3u);
                const uint32_t bytes = (Ps2FastRead32(rdram, p + 4u) >> 4) * 16u;
                const auto t0 = Clock::now();
                g_meshes.clear();
                dropImages();
                replayUploads(guest(rdram, start), std::min<uint32_t>(bytes, 0x100000u));
                prepareArena(rdram);
                std::cerr << "[arenas] venue " << Ps2FastRead32(rdram, kVenue) << ": its textures read ("
                          << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()
                          << " ms); our code draws it" << std::endl;
            }
            g_originalUpload(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *value = std::getenv("VENDETTA_ARENADRAW");
        if (!value || !*value || std::string(value) == "0")
            return;
        const std::string v = value;
        if (v == "vram")
        {
            g_mode = Mode::Vram;
            g_vramTextures = true;
        }
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
            g_vramTextures = v.rfind("abvram", 0) == 0;
            const size_t colon = v.find(':');
            if (colon != std::string::npos)
                g_abSeconds = std::max(0.25, std::atof(v.c_str() + colon + 1));
        }
        else
            g_mode = Mode::Host;
        if (const char *ids = std::getenv("VENDETTA_ARENA_GAMEIDS"))
            for (const char *p = ids; *p;)
            {
                char *end = nullptr;
                const long id = std::strtol(p, &end, 10);
                if (end == p)
                    break;
                g_gameIds.push_back(static_cast<int32_t>(id));
                p = *end ? end + 1 : end;
            }
        if (const char *n = std::getenv("VENDETTA_ARENA_STRESS"))
            g_stress = static_cast<uint32_t>(std::clamp(std::atoi(n), 1, 100));
        if (const char *n = std::getenv("VENDETTA_ARENA_SPLIT"))
            g_split = static_cast<uint32_t>(std::clamp(std::atoi(n), 1, 100));
        if (const char *k = std::getenv("VENDETTA_ARENA_TEXSCALE"))
            g_texScale = static_cast<uint32_t>(std::clamp(std::atoi(k), 1, 16));
        g_log = std::getenv("VENDETTA_ARENALOG") && *std::getenv("VENDETTA_ARENALOG") != '0';
        g_runtime = &runtime;
        g_originalDraw = runtime.lookupFunction(kDraw);
        g_originalDrawScrolled = runtime.lookupFunction(kDrawScrolled);
        g_originalDrawShadow = runtime.lookupFunction(kDrawShadow);
        g_originalUpload = runtime.lookupFunction(kUpload);
        if (!g_originalDraw || !g_originalDrawScrolled || !g_originalDrawShadow || !g_originalUpload)
        {
            std::cerr << "[arenas] the game's draw routines not found: off" << std::endl;
            g_mode = Mode::Off;
            return;
        }
        runtime.replaceFunction(kDraw, draw);
        runtime.replaceFunction(kDrawScrolled, drawScrolled);
        runtime.replaceFunction(kDrawShadow, drawShadow);
        runtime.replaceFunction(kUpload, upload);
        if (g_log && (g_originalIntros = runtime.lookupFunction(kIntros)) != nullptr)
            runtime.replaceFunction(kIntros, intros);
        std::cerr << "[arenas] on: the arena drawn by our code ("
                  << (g_mode == Mode::Vram ? "the game's uploaded textures" : g_mode == Mode::Ab ? "in turn with the game's"
                      : g_mode == Mode::Flip ? "F9 swaps it with the game's" : "textures as host images")
                  << ")" << std::endl;
    }
}

void installVendettaArenas(PS2Runtime &runtime) { vendetta_arenas::install(runtime); }
