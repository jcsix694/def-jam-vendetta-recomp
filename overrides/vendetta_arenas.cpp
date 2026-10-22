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
// The ring and the 3-D crowd are drawn by our code too. The ring draw FUN_001a2de8 (each match picture, after the shadow
// floor) keeps its own work: the fade near the camera (FIX by distance, the faded pieces sorted far to near), its GS
// register writes, the ropes' bend and the bones' posing. Its leaves are ours:
//   the posts, pads and turnbuckles (ids 81-92, the High Poly venues): FUN_001b3820(object, matrix, colour), the game's
//   own draw on the main processor, copied step for step: each vertex x the matrix ((x m0 + y m4) + z m8) + m12, kept
//   at vertex + 0x20; per triangle the back faces skipped, then cut at the near limit (z < 1048574), divided by w and
//   cut to the screen (y 1936-2160, x 1792-2304) in the game's order, sent as a fan: PRIM 0x5D (context 1, blended:
//   the fade's ALPHA_1), ST / RGBAQ / XYZ2, the float to integer conversions of FUN_0020aa90 (below 1 or NaN: 0);
//   the ropes (object 2, three to a side) and the crowd's people (one model part each, posed by the crowd's code):
//   FUN_001b93a0(part, light matrix, colour matrix) from 0x1A3240 / 0x1A34C0 / 0x1A36F4 (ropes) and 0x1978F8 (crowd),
//   the wrestlers' routine and VU1 program, drawn by the wrestlers' code (drawGamePart). The crowd's logic (its
//   clips, moods and rand()), its poses and light sets stay the game's.
//
// The textures: the arena's part 1 (FUN_001b85c0 from the arena set-up FUN_00165bd8) is read when the game uploads it,
// replayed into our own copy of GS memory, and each texture a draw uses is decoded from it into a host image of the
// GS texture's size (GS register 0x71 binds it around our primitives: paraLLEl-GS, a local addition). The game's own
// upload still happens (the game's own draws use it, in the test modes). Without the graphics card renderer the game's
// uploaded textures are used.
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
// VENDETTA_ARENA_FULLSTANDIN=1  a mod arena's stand-in keeps its triangles (a test: with VENDETTA_ARENADRAW=check the game
//                           draws the mod's arena and ours, read from the mod's file, is compared with it)
// VENDETTA_ARENA_GAMEIDS=<ids, comma separated>  those scene objects left to the game (finding a difference)
// VENDETTA_ARENA_STRESS=<N> each object drawn N times over itself, VENDETTA_ARENA_SPLIT=<N> each triangle drawn as N
//                           smaller ones over the same area (N times the detail), VENDETTA_ARENA_TEXSCALE=<k> each
//                           texture made k times larger (speed tests: a bigger arena, HD textures)
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_gpu.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "vendetta_gsdraw.h"
#include "vendetta_mods.h"
#include "vendetta_pictures.h"
#include "vendetta_store.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cfenv>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
        using namespace vendetta_gsdraw;

        constexpr uint32_t kDraw = 0x1B8BF8u;         // FUN_001b8bf8(object, matrix, colour): an object through VU1
        constexpr uint32_t kDrawScrolled = 0x1B9BB0u; // FUN_001b9bb0: the same, (u, v) in f12 / f13 into VU 0x40
        constexpr uint32_t kDrawShadow = 0x1B9880u;   // FUN_001b9880(object, camera, shadow): the shadow floor (99)
        constexpr uint32_t kDrawPost = 0x1B3820u;     // FUN_001b3820(object, matrix, colour): a post / pad / turnbuckle
        constexpr uint32_t kDrawPart = 0x1B93A0u;     // FUN_001b93a0(part, light matrix, colour matrix): a model part
        constexpr uint32_t kFromCrowd = 0x1978F8u;    // its return addresses: a crowd person (FUN_00197830),
        constexpr uint32_t kFromRope = 0x1A3240u, kFromRopeFaded = 0x1A34C0u, kFromRopeLowPoly = 0x1A36F4u; // a rope
        constexpr uint32_t kSetUp = 0x1A1C38u;        // FUN_001a1c38(venue, scene): a venue's set-up, at each venue load
        constexpr uint32_t kSounds = 0x1B0958u;       // FUN_001b0958(venue, ...): the match's sounds, the set 0x2598C8[venue]
        constexpr uint32_t kFlyInPaths = 0x234A00u;   // the venue's fly-in: its camera path's number
        constexpr uint32_t kCameraPaths = 0x140FD0Cu; // the camera paths (PAK 0): path n's self-relative offset at + 4n
        // Choose Stage's pictures: FUN_00157570(pack, index) uploads pack 0xC's big picture n (stage n) to 0x3621, pack
        // 0xD's thumbnail n to 0x36A5, before each draw; the names are rows of two sheets uploaded once at boot (the
        // upload returning to 0x1A0FD0): stage n = sheet n >> 3, row n & 7 (32 texels each)
        constexpr uint32_t kPicture = 0x157570u;
        constexpr uint32_t kBigSlot = 0x3621u, kThumbSlot = 0x36A5u;
        constexpr uint32_t kUploadNames = 0x1A0FD0u;
        constexpr uint32_t kNameScale = 4u;           // a mod's name drawn at 4 times the sheet's texels
        constexpr uint32_t kGameCrowd = 0xBD5500u;    // the loader's buffers: the 3-D crowd's people (AUD: "AUD ", size,
        constexpr uint32_t kGameClips = 0xD1A500u;    // count, people of 0x70 from + 0x10) and their clips (PAK)
        constexpr uint32_t kGameCrowdBytes = 0x145000u, kGameClipsBytes = 0xC8000u;
        constexpr uint32_t kCrowd = vendetta_store::kCrowdPeople; // where the game's crowd code reads them (patched)
        constexpr uint32_t kCrowdBytes = vendetta_store::kCrowdRoom;
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
        PS2Runtime::RecompiledFunction g_originalDrawPost = nullptr;
        PS2Runtime::RecompiledFunction g_originalDrawPart = nullptr;
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
        uint32_t g_checker = 0;                      // VENDETTA_ARENA_TESTTEX=checker[:N]: N-texel squares (a shimmer test)
        uint32_t g_split = 1;                        // VENDETTA_ARENA_SPLIT: each drawn triangle as N (a speed test)

        // Our copy of GS memory: the arena's part 1 replayed into it.
        GsScratch g_scratch; // the arena's uploads replayed (the blocks they wrote: low, high)
        std::unordered_map<uint64_t, uint32_t> g_images; // TEX0 (texture bits) -> host image id
        uint32_t g_nextImage = kImageIdBase;

        // ------------------------------------------------------------------ the mods' arenas (their data)

        // A mod arena replacing a game arena (mod.json "replace": {"arenas": {"Def Jam": "arenas/Ring"}}): its folder
        // holds model.arena (the game's arena format as the mod kit writes it: "DJARENA1", then the offsets and lengths of
        // the scene, its textures, the crowd's people and their clips, each part as the game keeps it) and an optional
        // textures\ folder (PNGs at any size; pictures.json names each one's GS address, else a PNG named "@<address>").
        struct ModArena
        {
            std::string what;                // "<mod>: <folder>", for messages
            std::filesystem::path folder;
            uint32_t venue = 0;              // the game arena it replaces
            std::vector<uint8_t> file;       // model.arena, read at the arena's load (only the current arena's kept)
            uint32_t scene = 0, sceneBytes = 0, textures = 0, texturesBytes = 0;
            uint32_t people = 0, peopleBytes = 0, clips = 0, clipsBytes = 0;
            std::map<uint32_t, std::filesystem::path> pictures; // a texture's GS address (TBP) -> its PNG
            // arena.json: "sounds" (a game arena whose sound set it uses), "fly-in" (its camera path: {"frames",
            // "keys": [{"frame", "eye": [x, y, z], "target": [x, y, z], "roll", "fov"}]}); missing = the arena it replaces
            uint32_t soundsVenue = ~0u;
            std::vector<uint8_t> flyIn; // the path as the game keeps it: {s32 last frame, u32 keys, keys of 0x24 bytes}
            // Choose Stage: picture.png (the big picture), thumbnail.png, name.png (the name strip), each optional
            uint32_t picture = 0, thumbnail = 0;
            std::filesystem::path name;
        };
        std::vector<ModArena> g_modArenas;
        ModArena *g_current = nullptr; // the mod arena loaded now (its stand-in in the store), or none
        bool g_fullStandIn = false;    // VENDETTA_ARENA_FULLSTANDIN: the stand-in keeps its triangles (the check)

        // A mod arena drawn by our code alone (always, except the check of a full stand-in: then the game draws it too)
        bool modOnly() { return g_current && !(g_fullStandIn && g_mode == Mode::Check); }

        // Where our draw reads a mod arena's triangles: an address in its stand-in (the scene at kArenaScene, the crowd's
        // people at kCrowdPeople) is the same offset in the mod's file, whose triangles the stand-in leaves out; null
        // for anything else (guest memory).
        uint8_t *modBytes(uint32_t address, uint32_t bytes)
        {
            if (!g_current)
                return nullptr;
            ModArena &m = *g_current;
            auto in = [&](uint32_t base, uint32_t at, uint32_t size) -> uint8_t * {
                return address >= base && uint64_t(address - base) + bytes <= size ? m.file.data() + at + (address - base)
                                                                                     : nullptr;
            };
            if (uint8_t *p = in(vendetta_store::kArenaScene, m.scene, m.sceneBytes))
                return p;
            return in(kCrowd, m.people, m.peopleBytes);
        }

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
                            const uint8_t *own = modBytes(pending, pendingQ * 16u); // a mod's triangles
                            if (readBatch(own ? own : d + (pending & PS2_RAM_MASK), pendingQ, batch.top, imm == 0xCu ? 3u : 2u,
                                          batch))
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
            mesh.ours = !otherKernel && mesh.table != 0u; // none of its batches: an empty object (the mod kit writes some)
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

        // A batch's material slot: in the slot table (the game writes there: the picture cards' frames), or where an earlier
        // batch's data lies (a mod's triangles: from its file)
        const uint8_t *slotBytes(const uint8_t *rdram, const Mesh &mesh, const Batch &batch)
        {
            const bool table = batch.slot >= mesh.table && batch.slot < mesh.table + 16u * (7u + mesh.slots);
            const uint8_t *own = table ? nullptr : modBytes(batch.slot, 16u);
            return own ? own : guest(rdram, batch.slot);
        }

        // A mod arena's object our draw can't read: skipped, never handed to the game's routine (it can't reach the store
        // above 32 MB: it would send whatever lies at the address cut to 32 MB). One line each (mods.log).
        void skipUnreadable(const uint8_t *rdram, uint32_t object, const char *what)
        {
            static std::unordered_set<uint64_t> told;
            const uint32_t scene = Ps2FastRead32(rdram, kScenePointer);
            int32_t id = INT32_MIN;
            const uint32_t count = std::min<uint32_t>(Ps2FastRead32(rdram, scene), 4096u);
            for (uint32_t i = 0; i < count && id == INT32_MIN; ++i)
            {
                const uint32_t entry = scene + 0x14u + 8u * i;
                if (entry + 4u + Ps2FastRead32(rdram, entry + 4u) == object)
                    id = static_cast<int32_t>(Ps2FastRead32(rdram, entry));
            }
            if (!told.insert((uint64_t(reinterpret_cast<uintptr_t>(g_current)) << 32) ^ object).second)
                return;
            char where[64];
            if (id != INT32_MIN)
                std::snprintf(where, sizeof(where), " (part %d at 0x%x)", id, object);
            else
                std::snprintf(where, sizeof(where), " at 0x%x (not in the scene's list)", object);
            vendetta_mods::problem() << "[arenas] " << (g_current ? g_current->what : std::string("?")) << ": " << what
                                     << where << " can't be read by our draw: left out" << std::endl;
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
            const uint32_t bytes = g_current && scene == vendetta_store::kArenaScene ? g_current->sceneBytes : kSceneBytes;
            return scene && object >= scene && object < scene + bytes;
        }

        // ------------------------------------------------------------------ the textures

        // The arena's part 1 (GIF A+D BITBLTBUF / TRXPOS / TRXREG / TRXDIR + IMAGE, per texture and per CLUT) written
        // into our copy of GS memory the way the GS lays it out.
        void replayUploads(const uint8_t *data, uint32_t bytes)
        {
            const uint32_t uploads = g_scratch.replay(data, bytes);
            if (g_log)
                std::cerr << "[arenas] textures: " << uploads << " uploads replayed, blocks 0x" << std::hex << g_scratch.low
                          << "-0x" << g_scratch.high << std::dec << std::endl;
        }

        bool decode(uint64_t tex0, std::vector<uint32_t> &rgba, uint32_t &w, uint32_t &h)
        {
            return g_scratch.decode(tex0, rgba, w, h);
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
            bool own = false; // a mod's PNG: any size
            if (renderer && g_current)
            {
                const auto png = g_current->pictures.find(static_cast<uint32_t>(tex0 & 0x3FFFu));
                std::vector<uint8_t> bytes;
                if (png != g_current->pictures.end() && vendetta_mods::readWhole(png->second, bytes))
                {
                    if (gsDecodePicture(bytes.data(), bytes.size(), 0, 0, rgba, w, h))
                    {
                        for (uint32_t &c : rgba) // PNG alpha (255 = solid) to the GS's (128 = solid)
                            c = (c & 0x00FFFFFFu) | ((((c >> 24) * 128u + 127u) / 255u) << 24);
                        own = true;
                    }
                    else
                        vendetta_mods::problem() << "[arenas] " << g_current->what << ": " << png->second.filename().string()
                                                 << " can't be read as a picture: the model file's own texture is used"
                                                 << std::endl;
                }
            }
            if (renderer && (own || decode(tex0, rgba, w, h)))
            {
                if (g_texScale > 1u && !own)
                {
                    gsResizePicture(rgba, w, h, w * g_texScale, h * g_texScale);
                    w *= g_texScale;
                    h *= g_texScale;
                }
                // the finest detail a texture can have: every other square of N texels at half brightness (the alpha,
                // and so the see-through parts, kept)
                if (g_checker)
                    for (uint32_t y = 0; y < h; ++y)
                        for (uint32_t x = 0; x < w; ++x)
                            if (((x / g_checker) ^ (y / g_checker)) & 1u)
                            {
                                uint32_t &c = rgba[size_t(y) * w + x];
                                c = (c & 0xFF000000u) | ((c >> 1) & 0x007F7F7Fu);
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

        constexpr float kNearZ = 1048574.0f;   // VU 0x34.z
        constexpr float kGuard = 2047.0f;      // VU 0x33.w
        constexpr float kHalfW = 256.0f;       // VU 0x37 / 0x38: x -256 / +256 about 2048
        constexpr float kHalfH = 112.0f;       // VU 0x35 / 0x36: y -112 / +112

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

        // VENDETTA_ARENADRAW=flip: which draw is on, F9 to swap
        bool g_flipOurs = true;
        bool g_flipTitled = false;

#if defined(_WIN32)
        void showFlipInTitle(bool ours)
        {
            showInTitle(L" | Arena: ", ours ? L"ours (F9: the game's)" : L"the game's (F9: ours)");
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

        std::vector<GsTriangle> g_ourTriangles;
        struct CheckStats
        {
            uint64_t objects = 0, exact = 0, gameTris = 0, ourTris = 0, matched = 0;
        } g_check;
        int g_checkReports = 0;

        // The game's VU1 output for an object arrives partly during the next object's draw: the check collects every
        // path 1 triangle of a window of draws, and each of ours is looked up among this window's and the next one's.
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
                    const uint8_t *slot = slotBytes(rdram, mesh, batch);
                    std::memcpy(&tex0, slot, 8);
                    std::memcpy(&reg, slot + 8, 8);
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
            Part,     // FUN_001b93a0(part, light matrix, colour matrix): a rope or a crowd person
            Post,     // FUN_001b3820(object, matrix, colour): a post, pad or turnbuckle
        };

        // Writes the draw's values into the object as the game does (+0xD0 the matrix, then + 0x110 the colour or
        // the shadow matrix, + 0x120 the scroll), then draws it ourselves. False: the game's routine draws it.
        bool tryDraw(uint8_t *rdram, R5900Context *ctx, Call call)
        {
            const uint32_t object = getRegU32(ctx, 4);
            const bool mod = modOnly(); // a mod's arena: always ours (the game's would draw its stand-in: nothing)
            if (mod && object == 0u)
                return true; // a part the arena's routine asks for and the mod hasn't: skipped (the game would draw address 0)
            if ((g_mode == Mode::Off && !mod) || !inScene(rdram, object))
                return false;
            if (!g_gameIds.empty() && !mod)
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
            if (!mesh.ours || (mesh.kernel && mesh.kernel != (call == Call::Shadow ? 0x10u : 0xCu)) || mesh.table != object + 0xB0u)
            {
                if (mod)
                    skipUnreadable(rdram, object, "an object");
                return mod; // a mod's: skipped
            }
            if (g_mode == Mode::Check && !mod)
                return false; // the game's draw (checkDraw compares)
            if (!mod && !oursNow())
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
        void checkWindow(const TriangleSource &source);

        bool checkDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, PS2Runtime::RecompiledFunction original, Call call)
        {
            const uint32_t object = getRegU32(ctx, 4);
            if (g_mode != Mode::Check || modOnly() || !inScene(rdram, object))
                return false;
            Mesh &mesh = meshFor(rdram, object);
            if (!mesh.ours || (mesh.kernel && mesh.kernel != (call == Call::Shadow ? 0x10u : 0xCu)) || mesh.table != object + 0xB0u)
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
            checkWindow({object, ra, static_cast<uint32_t>(call), m1, m2});
            return true;
        }

        // The draw's triangles of ours (g_ourTriangles) into the check's window; every 2 s the last window's looked up
        // among the game's
        void checkWindow(const TriangleSource &source)
        {
            g_ourWindow.insert(g_ourWindow.end(), g_ourTriangles.begin(), g_ourTriangles.end());
            g_ourWindowObject.insert(g_ourWindowObject.end(), g_ourTriangles.size(), source);
            ++g_check.objects;
            static auto last = Clock::now();
            if (Clock::now() - last < std::chrono::seconds(2))
                return;
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
            // by kind: 0 the scenery (and the shadow floor), 1 the ropes, 2 the crowd, 3 the posts
            size_t kindFound[4] = {}, kindMissing[4] = {};
            auto kindOf = [](const TriangleSource &src) {
                if (src.call == static_cast<uint32_t>(Call::Post))
                    return 3;
                if (src.call == static_cast<uint32_t>(Call::Part))
                    return src.ra == kFromCrowd ? 2 : 1;
                return 0;
            };
            for (size_t ti = 0; ti < g_ourPrevious.size(); ++ti)
            {
                const GsTriangle &t = g_ourPrevious[ti];
                if (game.count(t))
                {
                    ++found;
                    ++kindFound[kindOf(g_ourPreviousObject[ti])];
                    continue;
                }
                ++missing;
                ++kindMissing[kindOf(g_ourPreviousObject[ti])];
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
            std::cerr << "[arenas] check by kind (the same / not): scenery " << kindFound[0] << " / " << kindMissing[0]
                      << ", ropes " << kindFound[1] << " / " << kindMissing[1] << ", crowd " << kindFound[2] << " / "
                      << kindMissing[2] << ", posts " << kindFound[3] << " / " << kindMissing[3] << std::endl;
            g_ourPrevious.swap(g_ourWindow);
            g_ourWindow.clear();
            g_ourPreviousObject.swap(g_ourWindowObject);
            g_ourWindowObject.clear();
            g_gamePrevious.swap(g_gameWindow);
            g_gameWindow.clear();
            g_check = {};
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

        // ------------------------------------------------------------------ the ring's posts (FUN_001b3820)

        // The post format: {u32 vertices, u32 triangles, u64 TEX0}, then 64-byte vertices {(s, t, q, 0), (r, g, b, a),
        // a quadword the draw writes (the vertex x the matrix), (x, y, z, w)}, then u16 x 3 per triangle. A vertex in the
        // clipper as the game keeps it: 12 floats, s t q 0 | r g b a | x y z w.
        struct PostVertex
        {
            float f[12];
        };

        // FUN_0020aa90: the game's float to unsigned (soft float): below 1 or NaN 0, from 2^32 0xFFFFFFFF, else truncated
        uint32_t toUnsigned(float v)
        {
            if (!(v >= 1.0f))
                return 0u;
            if (v >= 4294967296.0f)
                return 0xFFFFFFFFu;
            return static_cast<uint32_t>(v);
        }

        // div.s as the recompiled game does it: a zero divisor gives infinity with the dividend's sign
        float eeDiv(float a, float b) { return b == 0.0f ? std::copysign(INFINITY, a * 0.0f) : a / b; }

        // One cut of the post clipper (Sutherland-Hodgman, every float carried along, always from the current vertex
        // toward the next): keep lane `lane` below `plane` (or above it)
        int clipPost(const PostVertex *in, int n, PostVertex *out, int lane, float plane, bool keepBelow)
        {
            int m = 0;
            for (int i = 0; i < n; ++i)
            {
                const PostVertex &cur = in[i], &next = in[(i + 1) % n];
                const bool curIn = keepBelow ? cur.f[lane] < plane : plane < cur.f[lane];
                const bool nextIn = keepBelow ? next.f[lane] < plane : plane < next.f[lane];
                if (curIn)
                {
                    out[m++] = cur;
                    if (nextIn)
                        continue;
                }
                else if (!nextIn)
                    continue;
                const float t = eeDiv(plane - cur.f[lane], next.f[lane] - cur.f[lane]);
                PostVertex &v = out[m++];
                for (int k = 0; k < 12; ++k)
                    v.f[k] = cur.f[k] + (next.f[k] - cur.f[k]) * t;
            }
            return m;
        }

        // A post drawn as FUN_001b3820 draws it (in the game thread's own rounding, as the game's code runs), into one
        // packet: the TEX0 (TEX0_1) once, then a fan for each triangle that shows. False: not a post (the game draws it).
        bool drawPost(uint8_t *rdram, uint32_t object, uint32_t matrix, Packet &packet)
        {
            const uint32_t vertices = Ps2FastRead32(rdram, object), triangles = Ps2FastRead32(rdram, object + 4u);
            if (vertices == 0u || vertices > 0x10000u || triangles > 0x10000u || !inScene(rdram, object) ||
                !inScene(rdram, object + 16u + 64u * vertices + 6u * triangles - 1u))
                return false;
            float m[16];
            std::memcpy(m, guest(rdram, matrix), 64);
            // a mod's post: its vertices and triangles from its file (the stand-in has neither)
            uint8_t *own = modBytes(object, 16u + 64u * vertices + 6u * triangles);
            static std::vector<PostVertex> transformed;
            transformed.resize(vertices);
            for (uint32_t k = 0; k < vertices; ++k)
            {
                uint8_t *v = own ? own + 16u + 64u * k : rdram + ((object + 16u + 64u * k) & PS2_RAM_MASK);
                float f[16];
                std::memcpy(f, v, 64);
                float c[4];
                for (int j = 0; j < 4; ++j)
                {
                    const float a = f[12] * m[j], b = f[13] * m[4 + j], d = f[14] * m[8 + j];
                    const float ab = a + b, abd = ab + d;
                    c[j] = abd + m[12 + j];
                }
                std::memcpy(v + 0x20, c, 16);
                PostVertex &out = transformed[k];
                std::copy(f, f + 8, out.f);
                std::copy(c, c + 4, out.f + 8);
            }
            uint64_t tex0;
            std::memcpy(&tex0, guest(rdram, object + 8u), 8);
            packet.ad(0x06u, tex0); // TEX0_1
            const uint32_t tbp = static_cast<uint32_t>(tex0 & 0x3FFFu);
            const uint32_t id = !g_vramTextures && gpu() ? imageFor(tex0) : 0u;
            if (id)
                packet.ad(0x71u, uint64_t(tbp) | (uint64_t(id) << 32));
            const uint8_t *index = own ? own + 16u + 64u * vertices : guest(rdram, object + 16u + 64u * vertices);
            for (uint32_t t = 0; t < triangles; ++t)
            {
                uint16_t ix[3];
                std::memcpy(ix, index + 6u * t, 6);
                if (ix[0] >= vertices || ix[1] >= vertices || ix[2] >= vertices)
                    continue;
                PostVertex one[16], two[16];
                for (int c = 0; c < 3; ++c)
                    one[c] = transformed[ix[c]];
                // the back faces (FUN_001b3e18), on the three vertices before any cut
                float sx[3], sy[3];
                for (int c = 0; c < 3; ++c)
                {
                    const float r = eeDiv(1.0f, one[c].f[11]);
                    sx[c] = one[c].f[8] * r;
                    sy[c] = one[c].f[9] * r;
                }
                const float a = (sy[1] - sy[0]) * (sx[2] - sx[0]);
                const float b = (sx[1] - sx[0]) * (sy[2] - sy[0]);
                if (0.0f < b - a)
                    continue;
                int n = clipPost(one, 3, two, 10, 1048574.0f, true); // the near limit, z not yet divided
                for (int i = 0; i < n; ++i)
                {
                    PostVertex &p = two[i];
                    const float w = p.f[11];
                    p.f[0] = eeDiv(p.f[0], w);
                    p.f[1] = eeDiv(p.f[1], w);
                    p.f[2] = eeDiv(p.f[2], w);
                    p.f[8] = eeDiv(p.f[8], w);
                    p.f[9] = eeDiv(p.f[9], w);
                }
                if (n)
                    n = clipPost(two, n, one, 9, 1936.0f, false);
                if (n)
                    n = clipPost(one, n, two, 9, 2160.0f, true);
                if (n)
                    n = clipPost(two, n, one, 8, 1792.0f, false);
                if (n)
                    n = clipPost(one, n, two, 8, 2304.0f, true);
                if (n < 3)
                {
                    ++g_stats.culled;
                    continue;
                }
                // a fan: PRIM 0x5D (fan, Gouraud, textured, blended, context 1), PACKED, ST / RGBAQ / XYZ2
                packet.tag(uint64_t(n) | (uint64_t(0x302EC000u) << 32), 0x412ull);
                for (int i = 0; i < n; ++i)
                {
                    const PostVertex &p = two[i];
                    uint32_t s, tt, q;
                    std::memcpy(&s, &p.f[0], 4);
                    std::memcpy(&tt, &p.f[1], 4);
                    std::memcpy(&q, &p.f[2], 4);
                    packet.add(uint64_t(s) | (uint64_t(tt) << 32), uint64_t(q));
                    packet.add(uint64_t(toUnsigned(p.f[4])) | (uint64_t(toUnsigned(p.f[5])) << 32),
                               uint64_t(toUnsigned(p.f[6])) | (uint64_t(0x80u) << 32));
                    const float x = p.f[8] * 16.0f, y = p.f[9] * 16.0f;
                    packet.add(uint64_t(toUnsigned(x)) | (uint64_t(toUnsigned(y)) << 32), uint64_t(toUnsigned(p.f[10]) << 4));
                }
                g_stats.triangles += static_cast<uint64_t>(n - 2);
                if (n > 3)
                    ++g_stats.clipped;
            }
            if (id)
                packet.ad(0x71u, uint64_t(tbp)); // unbound
            ++g_stats.objects;
            return true;
        }

        void drawPostHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t object = getRegU32(ctx, 4), matrix = getRegU32(ctx, 5), ra = getRegU32(ctx, 31);
            const bool mod = modOnly();
            if (g_mode == Mode::Off && !mod)
            {
                g_originalDrawPost(rdram, ctx, runtime);
                return;
            }
            if (g_mode == Mode::Check && !mod)
            {
                const uint32_t sp = getRegU32(ctx, 29);
                g_checkStarted = true;
                g_originalDrawPost(rdram, ctx, runtime);
                if (ctx->pc != ra || getRegU32(ctx, 29) != sp)
                    return;
                g_ourTriangles.clear();
                Packet packet;
                if (drawPost(rdram, object, matrix, packet))
                {
                    send(packet);
                    checkWindow({object, ra, static_cast<uint32_t>(Call::Post), matrix, 0u});
                }
                return;
            }
            if (!mod && !oursNow())
            {
                ++g_stats.gameObjects;
                g_originalDrawPost(rdram, ctx, runtime);
                return;
            }
            Packet packet;
            if (!drawPost(rdram, object, matrix, packet))
            {
                if (mod)
                {
                    skipUnreadable(rdram, object, "a post");
                    ctx->pc = ra;
                    return;
                }
                g_originalDrawPost(rdram, ctx, runtime);
                return;
            }
            send(packet);
            logStats();
            ctx->pc = ra;
        }

        // ------------------------------------------------------------------ the ropes and the crowd (FUN_001b93a0)

        // The part's TEX0 (its stream's VU 0x3B row), for its host image
        uint64_t partTex0(const uint8_t *rdram, uint32_t part)
        {
            const uint32_t stream = part + (Ps2FastRead32(rdram, part) & ~3u);
            uint64_t tex0;
            std::memcpy(&tex0, guest(rdram, stream + 0x30u), 8);
            return tex0;
        }

        void drawPartHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31), part = getRegU32(ctx, 4);
            const uint32_t light = getRegU32(ctx, 5), colours = getRegU32(ctx, 6);
            const bool arena = ra == kFromCrowd || ra == kFromRope || ra == kFromRopeFaded || ra == kFromRopeLowPoly;
            const bool mod = modOnly();
            if (!arena || part == 0u || (g_mode == Mode::Off && !mod))
            {
                g_originalDrawPart(rdram, ctx, runtime); // the wrestlers' (their own hook, if on, comes before this one)
                return;
            }
            if (g_mode == Mode::Check && !mod)
            {
                const uint32_t sp = getRegU32(ctx, 29);
                g_checkStarted = true;
                g_originalDrawPart(rdram, ctx, runtime);
                if (ctx->pc != ra || getRegU32(ctx, 29) != sp)
                    return;
                // the game wrote the rows: ours read them there (a mod's part: its own copy, the rows written into it)
                g_ourTriangles.clear();
                uint8_t *own = nullptr;
                if (modBytes(part, 16u))
                    own = modBytes(part, (Ps2FastRead32(rdram, part) & ~3u) + Ps2FastRead32(rdram, part + 4u));
                if (vendetta_wrestlerdraw::drawGamePart(*runtime, rdram, part, light, colours, false, 0u, &g_ourTriangles, own))
                    checkWindow({part, ra, static_cast<uint32_t>(Call::Part), light, colours});
                return;
            }
            if (!mod && !oursNow())
            {
                ++g_stats.gameObjects;
                g_originalDrawPart(rdram, ctx, runtime);
                return;
            }
            const uint32_t id = !g_vramTextures && gpu() ? imageFor(partTex0(rdram, part)) : 0u;
            // a mod's rope or crowd person: its triangles from its file, the stand-in's bones as the game posed them
            uint8_t *own = nullptr;
            if (uint8_t *head = modBytes(part, 16u))
            {
                uint32_t streamAt, streamBytes;
                std::memcpy(&streamAt, head, 4);
                std::memcpy(&streamBytes, head + 4, 4);
                own = modBytes(part, (streamAt & ~3u) + streamBytes);
                if (!own)
                {
                    ctx->pc = ra; // its data runs past the file: nothing drawn (the stand-in has no triangles)
                    return;
                }
            }
            if (!vendetta_wrestlerdraw::drawGamePart(*runtime, rdram, part, light, colours, true, id, nullptr, own))
            {
                if (mod)
                {
                    skipUnreadable(rdram, part, ra == kFromCrowd ? "a crowd person" : "a rope");
                    ctx->pc = ra;
                    return;
                }
                g_originalDrawPart(rdram, ctx, runtime); // not readable: the game's own draw (the same rows)
                return;
            }
            ++g_stats.objects;
            logStats();
            ctx->pc = ra;
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
            const uint32_t venue = Ps2FastRead32(rdram, kVenue);
            const bool highPoly = venue < 12u || venue >= 24u; // the ring draw's posts by the main processor
            size_t images = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t entry = scene + 0x14u + 8u * i;
                const int32_t id = static_cast<int32_t>(Ps2FastRead32(rdram, entry));
                const uint32_t object = entry + 4u + Ps2FastRead32(rdram, entry + 4u);
                if (!inScene(rdram, object))
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
                if (host && id >= 81 && id <= 92 && highPoly)
                {
                    // a post in the post format of the High Poly venues (its TEX0 at + 8); a Low Poly one is a mesh
                    uint64_t tex0;
                    std::memcpy(&tex0, guest(rdram, object + 8u), 8);
                    if (imageFor(tex0))
                        ++images;
                }
                if (host && id == 2 && imageFor(partTex0(rdram, object))) // the rope (a model part)
                    ++images;
                Mesh &mesh = meshFor(rdram, object);
                if (!mesh.ours || mesh.kernel != 0xCu || !host)
                    continue;
                for (const Batch &batch : mesh.batches)
                {
                    if (!batch.slot)
                        continue;
                    uint64_t tex0, reg;
                    const uint8_t *slot = slotBytes(rdram, mesh, batch);
                    std::memcpy(&tex0, slot, 8);
                    std::memcpy(&reg, slot + 8, 8);
                    if (((reg & 0xFFu) == 0x06u || (reg & 0xFFu) == 0x07u) && imageFor(tex0))
                        ++images;
                }
            }
            // the 3-D crowd's people: four model pointers each (self-relative), their skins in the part 1 just replayed
            const uint32_t people = Ps2FastRead32(rdram, kCrowd + 8u);
            if (host && Ps2FastRead32(rdram, kCrowd) == 0x20445541u && people < 0x1000u &&
                0x10u + 0x70u * people <= kCrowdBytes) // "AUD "
                for (uint32_t i = 0; i < people; ++i)
                    for (uint32_t k = 0; k < 4u; ++k)
                    {
                        const uint32_t at = kCrowd + 0x10u + 0x70u * i + 4u * k;
                        const uint32_t model = at + Ps2FastRead32(rdram, at);
                        if (model > kCrowd && model < kCrowd + kCrowdBytes && imageFor(partTex0(rdram, model)))
                            ++images;
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

        void nameSheets(const uint8_t *packet, uint32_t bytes); // Choose Stage's names (below)

        // The arena's textures, read as the game uploads them.
        void upload(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (getRegU32(ctx, 31) == kUploadNames && !g_modArenas.empty())
            {
                const uint32_t p = getRegU32(ctx, 4);
                const uint32_t start = p + (Ps2FastRead32(rdram, p) & ~3u);
                const uint32_t bytes = (Ps2FastRead32(rdram, p + 4u) >> 4) * 16u;
                nameSheets(guest(rdram, start), std::min<uint32_t>(bytes, 0x100000u));
            }
            if (getRegU32(ctx, 31) == kUploadFromArena && (g_mode != Mode::Off || g_current))
            {
                const uint32_t p = getRegU32(ctx, 4);
                const uint32_t start = p + (Ps2FastRead32(rdram, p) & ~3u);
                const uint32_t bytes = (Ps2FastRead32(rdram, p + 4u) >> 4) * 16u;
                const auto t0 = Clock::now();
                g_meshes.clear();
                dropImages();
                if (g_current) // the mod's own part 1 (the game still uploads its arena's: our draw doesn't use it)
                {
                    const uint8_t *t = g_current->file.data() + g_current->textures;
                    uint32_t at, size;
                    std::memcpy(&at, t, 4);
                    std::memcpy(&size, t + 4, 4);
                    at &= ~3u;
                    size = (size >> 4) * 16u;
                    if (uint64_t(at) + size <= g_current->texturesBytes)
                        replayUploads(t + at, size);
                }
                else
                    replayUploads(guest(rdram, start), std::min<uint32_t>(bytes, 0x100000u));
                prepareArena(rdram);
                std::cerr << "[arenas] venue " << Ps2FastRead32(rdram, kVenue) << ": its textures read ("
                          << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()
                          << " ms); our code draws it" << std::endl;
            }
            g_originalUpload(rdram, ctx, runtime);
        }

        // ------------------------------------------------------------------ Choose Stage: a mod's pictures and name

        PS2Runtime::RecompiledFunction g_originalPicture = nullptr;

        // The mod arena that replaces stage n (the last one in load order, as at the set-up), or null
        const ModArena *replacing(uint32_t stage)
        {
            const ModArena *found = nullptr;
            for (const ModArena &m : g_modArenas)
                if (m.venue == stage)
                    found = &m;
            return found;
        }

        // FUN_00157570(pack, index): a replaced stage's big picture or thumbnail drawn from the mod's PNG over the game's
        // own upload (the game draws its own when the file can't be shown)
        void picture(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t pack = getRegU32(ctx, 4), index = getRegU32(ctx, 5);
            if ((pack == 0xCu || pack == 0xDu) && index < 12u)
                if (const ModArena *m = replacing(index))
                {
                    const uint32_t id = pack == 0xCu ? m->picture : m->thumbnail;
                    if (id)
                        vendetta_pictures::show(pack == 0xCu ? kBigSlot : kThumbSlot, id);
                    for (uint32_t other : {m->picture, m->thumbnail})
                        if (other)
                            vendetta_pictures::want(other);
                }
            g_originalPicture(rdram, ctx, runtime);
        }

        // At the boot upload of the two name sheets: each sheet with a replaced stage's row decoded from the packet, the
        // mod's name.png painted over that row (the sheet made kNameScale times larger, the game's rows kept as they are),
        // and bound to the sheet's address (GS register 0x70: the image the next upload there draws as)
        void nameSheets(const uint8_t *packet, uint32_t bytes)
        {
            GSGpuRenderer *renderer = gpu();
            if (!renderer)
                return;
            GsScratch sheets;
            sheets.replay(packet, bytes);
            const struct { uint32_t tbp, cbp, th; } layout[2] = {{0x3000u, 0x3080u, 8u}, {0x3081u, 0x30C1u, 7u}};
            for (uint32_t sheet = 0; sheet < 2u; ++sheet)
            {
                std::vector<std::pair<uint32_t, const ModArena *>> rows;
                for (uint32_t stage = 8u * sheet; stage < 8u * sheet + 8u && stage < 12u; ++stage)
                    if (const ModArena *m = replacing(stage); m && !m->name.empty())
                        rows.push_back({stage & 7u, m});
                if (rows.empty())
                    continue;
                const auto &l = layout[sheet];
                const uint64_t tex0 = uint64_t(l.tbp) | (4ull << 14) | (0x14ull << 20) | (8ull << 26) | (uint64_t(l.th) << 30) |
                                      (uint64_t(l.cbp) << 37);
                std::vector<uint32_t> game;
                uint32_t w = 0, h = 0;
                if (!sheets.decode(tex0, game, w, h))
                    continue;
                const uint32_t W = w * kNameScale, H = h * kNameScale;
                std::vector<uint32_t> big(size_t(W) * H);
                for (uint32_t y = 0; y < H; ++y) // the game's names as they are (each texel kNameScale x kNameScale)
                    for (uint32_t x = 0; x < W; ++x)
                        big[size_t(y) * W + x] = game[size_t(y / kNameScale) * w + x / kNameScale];
                size_t painted = 0;
                for (const auto &[row, m] : rows)
                {
                    std::vector<uint8_t> file;
                    std::vector<uint32_t> rgba;
                    uint32_t pw = 0, ph = 0;
                    if (!vendetta_mods::readWhole(m->name, file) || !gsDecodePicture(file.data(), file.size(), 0, 0, rgba, pw, ph))
                    {
                        vendetta_mods::problem() << "[arenas] " << m->what << ": name.png can't be read as a picture: the game's "
                                                 << "name is shown" << std::endl;
                        continue;
                    }
                    const uint32_t rowH = 32u * kNameScale;
                    gsResizePicture(rgba, pw, ph, W, rowH);
                    for (uint32_t y = 0; y < rowH && (row * rowH + y) < H; ++y)
                        for (uint32_t x = 0; x < W; ++x)
                        {
                            const uint32_t c = rgba[size_t(y) * W + x]; // PNG alpha (255 = solid) to the GS's (128)
                            big[size_t(row * rowH + y) * W + x] = (c & 0x00FFFFFFu) | ((((c >> 24) * 128u + 127u) / 255u) << 24);
                        }
                    ++painted;
                }
                if (!painted)
                    continue;
                const uint32_t id = kImageIdBase + 0x08000000u + sheet; // kept for the whole run (never dropped with an arena's)
                renderer->setHostImage(id, W, H, std::move(big));
                Packet bind;
                bind.ad(0x70u, uint64_t(l.tbp) | (uint64_t(id) << 32));
                send(bind);
                std::cerr << "[arenas] Choose Stage's name sheet " << sheet << ": " << painted << " name(s) from the mods" << std::endl;
            }
        }

        // ------------------------------------------------------------------ the arena's set-up: the crowd, a mod's arena

        PS2Runtime::RecompiledFunction g_originalSetUp = nullptr;

        uint32_t word(const uint8_t *p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }

        // A VIF1 stream's batch data (each UNPACK with FLG to VU 0: a batch's vertices, strips and triangles) zeroed: what
        // a stand-in leaves out. The setup rows (tags, slots, matrices, the colour scale) stay: the game writes there.
        void zeroBatches(uint8_t *stream, uint32_t bytes)
        {
            uint32_t o = 0;
            while (o + 4u <= bytes)
            {
                const uint32_t code = word(stream + o);
                const uint32_t imm = code & 0xFFFFu, num = (code >> 16) & 0xFFu, cmd = (code >> 24) & 0x7Fu;
                o += 4u;
                if (cmd >= 0x60u)
                {
                    const uint32_t vn = (cmd >> 2) & 3u, vl = cmd & 3u, n = num ? num : 256u;
                    const uint32_t size = ((n * (vn + 1u) * (32u >> vl) / 8u) + 3u) & ~3u;
                    if ((imm & 0x8000u) && (imm & 0x3FFu) == 0u)
                        std::memset(stream + o, 0, std::min(size, bytes - std::min(bytes, o)));
                    o += size;
                    continue;
                }
                switch (cmd)
                {
                case 0x20: o += 4u; break;                                  // STMASK
                case 0x30: case 0x31: o += 16u; break;                      // STROW, STCOL
                case 0x4A: o += (num ? num : 256u) * 8u; break;             // MPG
                case 0x50: case 0x51: o += (imm ? imm : 65536u) * 16u; break; // DIRECT, DIRECTHL
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
                case 0x10: case 0x11: case 0x13: case 0x14: case 0x15: case 0x17:
                    break;
                default:
                    return; // not a stream this knows: the rest left
                }
            }
        }

        // A model part (a rope, a crowd person: {stream offset, stream bytes, bones, 0}, bones, the stream) at `at` in a
        // copy of `bytes`: its batches zeroed
        void zeroPart(uint8_t *copy, uint32_t bytes, uint32_t at)
        {
            if (uint64_t(at) + 16u > bytes)
                return;
            const uint32_t stream = at + (word(copy + at) & ~3u), size = word(copy + at + 4u);
            if (stream > at && uint64_t(stream) + size <= bytes)
                zeroBatches(copy + stream, size);
        }

        // The stand-in scene: the mod's scene, its objects' triangles left out (in place, so every offset stays and our
        // draw finds them at the same offset in the mod's file): the meshes' batches, the posts' vertices and triangles
        // (ids 81-92, the High Poly venues' format), the rope's batches; the directory, the lights (LIT), the picture cards
        // (BBD), the header (the rope spring) and every object's own header and setup rows kept.
        void zeroScene(uint8_t *copy, uint32_t bytes, bool highPoly)
        {
            const uint32_t count = std::min<uint32_t>(word(copy), (bytes - std::min<uint32_t>(bytes, 0x14u)) / 8u);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t entry = 0x14u + 8u * i;
                const int32_t id = static_cast<int32_t>(word(copy + entry));
                const uint32_t object = entry + 4u + word(copy + entry + 4u);
                if (id < 0 || object < 0x14u || uint64_t(object) + 0x80u > bytes)
                    continue; // the picture cards (-1), the lights (-2), or nothing
                if (highPoly && id >= 81 && id <= 92)
                {
                    const uint64_t end = uint64_t(object) + 16u + 64ull * word(copy + object) + 6ull * word(copy + object + 4u);
                    if (end <= bytes)
                        std::memset(copy + object + 16u, 0, static_cast<size_t>(end - object - 16u));
                    continue;
                }
                bool zeroHead = true; // a mesh: {QWC, 0x7C zero bytes}, then its stream
                for (uint32_t k = 4; k < 0x80u && zeroHead; k += 4)
                    zeroHead = word(copy + object + k) == 0u;
                const uint32_t qwc = word(copy + object);
                if (zeroHead && qwc && uint64_t(object) + 0x80u + 16ull * qwc <= bytes)
                    zeroBatches(copy + object + 0x80u, 16u * qwc);
                else if (id == 2)
                    zeroPart(copy, bytes, object); // the rope: a model part
            }
        }

        // The mod arena's file read and its stand-in put in the store: the scene at kArenaScene, the crowd's people (each
        // model without its triangles) and clips where the game's crowd code reads them. False: unusable (a mods.log line;
        // the game's own arena is used).
        bool loadModArena(ModArena &m)
        {
            const char *const instead = ": the game's own arena is used";
            std::vector<uint8_t> file;
            const std::filesystem::path model = m.folder / "model.arena";
            if (!vendetta_mods::readWhole(model, file))
            {
                vendetta_mods::problem() << "[arenas] " << m.what << ": model.arena can't be read" << instead << std::endl;
                return false;
            }
            if (file.size() < 0x40u || std::memcmp(file.data(), "DJARENA1", 8) != 0)
            {
                vendetta_mods::problem() << "[arenas] " << m.what << ": model.arena is not an arena in the game's format"
                                         << instead << std::endl;
                return false;
            }
            auto part = [&](uint32_t at, uint32_t &offset, uint32_t &bytes) {
                offset = word(file.data() + at);
                bytes = word(file.data() + at + 4u);
                return bytes == 0u || (offset >= 0x40u && uint64_t(offset) + bytes <= file.size());
            };
            if (!part(0x10, m.scene, m.sceneBytes) || !part(0x18, m.textures, m.texturesBytes) ||
                !part(0x30, m.people, m.peopleBytes) || !part(0x38, m.clips, m.clipsBytes) || m.sceneBytes < 0x14u ||
                m.texturesBytes < 8u)
            {
                vendetta_mods::problem() << "[arenas] " << m.what << ": model.arena is damaged (a part runs past the file)"
                                         << instead << std::endl;
                return false;
            }
            const uint32_t venue = word(file.data() + 0x0C);
            if (venue != m.venue)
            {
                vendetta_mods::problem() << "[arenas] " << m.what << ": model.arena is made for venue " << venue
                                         << ", not the arena it replaces (venue " << m.venue << "); replacing another arena "
                                         << "comes later" << instead << std::endl;
                return false;
            }
            if (m.sceneBytes > vendetta_store::kArenaSceneRoom || m.peopleBytes > vendetta_store::kCrowdRoom ||
                m.clipsBytes > vendetta_store::kCrowdRoom)
            {
                vendetta_mods::problem() << "[arenas] " << m.what << ": model.arena is too big (the scene up to 4 MB, the crowd "
                                         << "and its clips up to 3 MB each)" << instead << std::endl;
                return false;
            }
            m.file = std::move(file);
            // the stand-in scene
            uint8_t *scene = vendetta_store::host(vendetta_store::kArenaScene);
            std::memcpy(scene, m.file.data() + m.scene, m.sceneBytes);
            if (!g_fullStandIn)
                zeroScene(scene, m.sceneBytes, m.venue < 12u || m.venue >= 24u);
            // the crowd: its people (each model without its triangles) and their clips; none = no one ("AUD ", 8, 0)
            uint8_t *people = vendetta_store::host(kCrowd);
            if (m.peopleBytes >= 0x10u)
            {
                std::memcpy(people, m.file.data() + m.people, m.peopleBytes);
                const uint32_t count = std::min<uint32_t>(word(people + 8u), (m.peopleBytes - 0x10u) / 0x70u);
                for (uint32_t i = 0; i < count; ++i)
                    for (uint32_t k = 0; k < 4u; ++k)
                    {
                        const uint32_t at = 0x10u + 0x70u * i + 4u * k;
                        const uint32_t model = at + word(people + at); // the models' pointers are self-relative
                        if (model < m.peopleBytes && !g_fullStandIn)
                            zeroPart(people, m.peopleBytes, model); // a model shared by several pointers: zeroed once more
                    }
            }
            else
            {
                const uint32_t empty[4] = {0x20445541u, 8u, 0u, 0u}; // "AUD ", size, no people
                std::memcpy(people, empty, sizeof(empty));
            }
            if (m.clipsBytes)
                std::memcpy(vendetta_store::host(vendetta_store::kCrowdClips), m.file.data() + m.clips, m.clipsBytes);
            std::cerr << "[arenas] " << m.what << ": in place of venue " << m.venue << " (" << m.file.size() / 1024
                      << " KB; the stand-in scene " << m.sceneBytes / 1024 << " KB, the crowd " << m.peopleBytes / 1024
                      << " KB without its triangles)" << std::endl;
            return true;
        }

        // FUN_001a1c38(venue, scene): the game's crowd (the loader's buffers) copied where its crowd code reads it, or a
        // mod arena's stand-in in its place (a1 = the stand-in scene), before the set-up reads any of it
        // A mod arena's fly-in: the camera path its venue uses pointed at the mod's own (in the store) while it is loaded,
        // the game's put back at the next venue's load
        uint32_t g_redirectedPath = ~0u, g_gamePathOffset = 0;

        void restoreFlyIn(uint8_t *rdram)
        {
            if (g_redirectedPath == ~0u)
                return;
            std::memcpy(rdram + kCameraPaths + 4u * g_redirectedPath, &g_gamePathOffset, 4);
            g_redirectedPath = ~0u;
        }

        void redirectFlyIn(uint8_t *rdram, const ModArena &m)
        {
            if (m.flyIn.empty() || m.venue >= 28u)
                return;
            const uint32_t path = word(rdram + kFlyInPaths + 4u * m.venue);
            if (path >= 369u)
                return;
            std::memcpy(vendetta_store::host(vendetta_store::kArenaFlyIn), m.flyIn.data(), m.flyIn.size());
            const uint32_t entry = kCameraPaths + 4u * path;
            g_gamePathOffset = word(rdram + entry);
            g_redirectedPath = path;
            const uint32_t offset = vendetta_store::kArenaFlyIn - entry; // self-relative
            std::memcpy(rdram + entry, &offset, 4);
        }

        // FUN_001b0958(venue, ...): a mod arena's sounds are the sound set of the game arena its arena.json names
        PS2Runtime::RecompiledFunction g_originalSounds = nullptr;
        void sounds(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_current && g_current->soundsVenue != ~0u && getRegU32(ctx, 4) == g_current->venue)
            {
                std::cerr << "[arenas] " << g_current->what << ": the sounds of venue " << g_current->soundsVenue << std::endl;
                SET_GPR_U32(ctx, 4, g_current->soundsVenue);
            }
            g_originalSounds(rdram, ctx, runtime);
        }

        void setUp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t venue = getRegU32(ctx, 4);
            restoreFlyIn(rdram);
            for (ModArena &m : g_modArenas)
                if (&m != g_current)
                    std::vector<uint8_t>().swap(m.file);
            g_current = nullptr;
            for (ModArena &m : g_modArenas) // the mods in load order: the last one replacing this arena wins
                if (m.venue == venue)
                    g_current = &m;
            if (g_current && !loadModArena(*g_current))
                g_current = nullptr;
            if (g_current)
            {
                SET_GPR_U32(ctx, 5, vendetta_store::kArenaScene);
                redirectFlyIn(rdram, *g_current);
                if (g_redirectedPath != ~0u)
                    std::cerr << "[arenas] " << g_current->what << ": its own fly-in (camera path " << g_redirectedPath << ")"
                              << std::endl;
            }
            else
            {
                std::memcpy(vendetta_store::host(kCrowd), rdram + kGameCrowd, kGameCrowdBytes);
                std::memcpy(vendetta_store::host(vendetta_store::kCrowdClips), rdram + kGameClips, kGameClipsBytes);
            }
            g_originalSetUp(rdram, ctx, runtime);
        }

        // The game's arenas by the names a mod uses (Choose Stage's), and their venues
        uint32_t venueNamed(std::string name)
        {
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (name.rfind("the ", 0) == 0)
                name = name.substr(4);
            static const std::pair<const char *, uint32_t> names[] = {
                {"bounty club", 0}, {"junkyard", 1}, {"warehouse", 2}, {"grimeyville", 3}, {"dragon house", 4},
                {"da bridge", 5}, {"club luda", 6}, {"face club", 7}, {"face club after hours", 8}, {"sanctuary", 9},
                {"def jam", 10}, {"def jam vendetta", 11}};
            for (const auto &[n, v] : names)
                if (name == n)
                    return v;
            return ~0u;
        }

        // arena.json: the sounds (a game arena named) and the fly-in (camera keys). Missing or unusable: the replaced
        // arena's own, with a mods.log line.
        void readArenaJson(ModArena &m)
        {
            std::vector<uint8_t> bytes;
            std::error_code ec;
            if (!std::filesystem::is_regular_file(m.folder / "arena.json", ec) || !vendetta_mods::readWhole(m.folder / "arena.json", bytes))
            {
                vendetta_mods::problem() << "[arenas] " << m.what << ": no arena.json: the sounds and the fly-in of the arena it "
                                         << "replaces" << std::endl;
                return;
            }
            vendetta_mods::Json json;
            std::string error;
            if (!vendetta_mods::parseJson(std::string(bytes.begin(), bytes.end()), json, error) || !json.isObject())
            {
                vendetta_mods::problem() << "[arenas] " << m.what << ": arena.json can't be read (" << error
                                         << "): the sounds and the fly-in of the arena it replaces" << std::endl;
                return;
            }
            if (const vendetta_mods::Json *sounds = json.find("sounds"))
            {
                const uint32_t venue = sounds->isString() ? venueNamed(sounds->text) : ~0u;
                if (venue == ~0u)
                    vendetta_mods::problem() << "[arenas] " << m.what << ": arena.json's \"sounds\" is not one of the game's "
                                             << "arenas: the sounds of the arena it replaces" << std::endl;
                else
                    m.soundsVenue = venue;
            }
            const vendetta_mods::Json *flyIn = json.find("fly-in");
            if (!flyIn)
                return;
            const vendetta_mods::Json *frames = flyIn->find("frames"), *keys = flyIn->find("keys");
            auto number = [](const vendetta_mods::Json *j) { return j && j->type == vendetta_mods::Json::Type::Number; };
            auto vec3 = [&](const vendetta_mods::Json *j, float out[3]) {
                if (!j || !j->isArray() || j->items.size() != 3u)
                    return false;
                for (int i = 0; i < 3; ++i)
                {
                    if (!number(&j->items[i]))
                        return false;
                    out[i] = static_cast<float>(j->items[i].number);
                }
                return true;
            };
            std::vector<uint8_t> path;
            bool ok = number(frames) && frames->number >= 1.0 && keys && keys->isArray() && !keys->items.empty() &&
                      keys->items.size() <= 1024u;
            int32_t last = -1;
            if (ok)
            {
                const int32_t lastFrame = static_cast<int32_t>(frames->number);
                const uint32_t count = static_cast<uint32_t>(keys->items.size());
                path.resize(8u + 0x24u * count);
                std::memcpy(path.data(), &lastFrame, 4);
                std::memcpy(path.data() + 4, &count, 4);
                for (uint32_t k = 0; k < count && ok; ++k)
                {
                    const vendetta_mods::Json &key = keys->items[k];
                    float eye[3], target[3];
                    const vendetta_mods::Json *frame = key.find("frame"), *roll = key.find("roll"), *fov = key.find("fov");
                    ok = number(frame) && vec3(key.find("eye"), eye) && vec3(key.find("target"), target) && number(roll) &&
                         number(fov) && static_cast<int32_t>(frame->number) > last && frame->number <= frames->number;
                    if (!ok)
                        break;
                    last = static_cast<int32_t>(frame->number);
                    const float values[8] = {eye[0], eye[1], eye[2], target[0], target[1], target[2],
                                             static_cast<float>(roll->number), static_cast<float>(fov->number)};
                    uint8_t *at = path.data() + 8u + 0x24u * k;
                    std::memcpy(at, &last, 4);
                    std::memcpy(at + 4, values, 32);
                }
            }
            if (!ok || path.size() > vendetta_store::kArenaFlyInRoom)
            {
                vendetta_mods::problem() << "[arenas] " << m.what << ": arena.json's \"fly-in\" can't be used (\"frames\", then "
                                         << "\"keys\" in frame order, each with \"frame\", \"eye\", \"target\", \"roll\", "
                                         << "\"fov\"): the fly-in of the arena it replaces" << std::endl;
                return;
            }
            m.flyIn = std::move(path);
        }

        // The mods' arenas ("replace": {"arenas": {"<game arena>": "<folder>"}}), read at start (their files at each load)
        void findModArenas()
        {
            for (const vendetta_mods::Mod &mod : vendetta_mods::loadedMods())
            {
                const vendetta_mods::Json *replace = mod.manifest.find("replace");
                const vendetta_mods::Json *arenas = replace ? replace->find("arenas") : nullptr;
                if (!arenas || !arenas->isObject())
                    continue;
                for (const auto &[name, value] : arenas->members)
                {
                    const std::string what = mod.name + ": " + (value.isString() ? value.text : name);
                    const uint32_t venue = venueNamed(name);
                    if (venue == ~0u)
                    {
                        vendetta_mods::problem() << "[arenas] " << what << ": \"" << name << "\" is not one of the game's arenas"
                                                 << std::endl;
                        continue;
                    }
                    const std::filesystem::path folder = value.isString() ? vendetta_mods::fileNamed(mod.folder, value.text)
                                                                          : std::filesystem::path();
                    std::error_code ec;
                    if (folder.empty() || !std::filesystem::is_regular_file(folder / "model.arena", ec))
                    {
                        vendetta_mods::problem() << "[arenas] " << what << ": no model.arena in its folder: " << name
                                                 << " stays the game's" << std::endl;
                        continue;
                    }
                    ModArena m;
                    m.what = what;
                    m.folder = folder;
                    m.venue = venue;
                    // textures\: pictures.json {"pictures": [{"file", "dbp"}, ...]}, else PNGs named "@<address>"
                    const std::filesystem::path textures = folder / "textures";
                    std::vector<uint8_t> list;
                    if (std::filesystem::is_regular_file(textures / "pictures.json", ec) &&
                        vendetta_mods::readWhole(textures / "pictures.json", list))
                    {
                        vendetta_mods::Json json;
                        std::string error;
                        const vendetta_mods::Json *pictures = nullptr;
                        if (vendetta_mods::parseJson(std::string(list.begin(), list.end()), json, error) &&
                            (pictures = json.find("pictures")) != nullptr && pictures->isArray())
                        {
                            for (const vendetta_mods::Json &picture : pictures->items)
                            {
                                const vendetta_mods::Json *file = picture.find("file"), *dbp = picture.find("dbp");
                                if (file && file->isString() && dbp && dbp->type == vendetta_mods::Json::Type::Number &&
                                    std::filesystem::is_regular_file(textures / file->text, ec))
                                    m.pictures[static_cast<uint32_t>(dbp->number)] = textures / file->text;
                            }
                        }
                        else
                            vendetta_mods::problem() << "[arenas] " << what << ": textures\\pictures.json can't be read ("
                                                     << error << "): the model file's own textures are used" << std::endl;
                    }
                    if (std::filesystem::is_directory(textures, ec))
                        for (const auto &f : std::filesystem::directory_iterator(textures, ec))
                        {
                            const std::string n = f.path().stem().string();
                            if (f.path().extension() == ".png" && n.size() > 1 && n[0] == '@')
                                m.pictures.emplace(static_cast<uint32_t>(std::strtoul(n.c_str() + 1, nullptr, 10)), f.path());
                        }
                    readArenaJson(m);
                    // Choose Stage's pictures (the GS textures' shapes: 256 x 128, 64 x 64; the name a 256 x 32 row)
                    if (std::filesystem::is_regular_file(folder / "picture.png", ec))
                        m.picture = vendetta_pictures::add(folder / "picture.png", 256u, 128u, 4u);
                    if (std::filesystem::is_regular_file(folder / "thumbnail.png", ec))
                        m.thumbnail = vendetta_pictures::add(folder / "thumbnail.png", 64u, 64u, 4u);
                    if (std::filesystem::is_regular_file(folder / "name.png", ec))
                        m.name = folder / "name.png";
                    std::cerr << "[arenas] " << what << ": replaces " << name << " (" << m.pictures.size()
                              << " textures of its own; " << (m.flyIn.empty() ? "the arena's fly-in" : "its own fly-in")
                              << ", the sounds of venue " << (m.soundsVenue == ~0u ? m.venue : m.soundsVenue) << ")" << std::endl;
                    g_modArenas.push_back(std::move(m));
                }
            }
        }
    }

    void install(PS2Runtime &runtime)
    {
        g_runtime = &runtime;
        // the 3-D crowd is always read from the store (the game's own copied there at each venue's set-up)
        const uint32_t crowdAt = kCrowd - vendetta_store::kFixedArea, clipsAt = vendetta_store::kCrowdClips - vendetta_store::kFixedArea;
        const uint32_t sceneAt = vendetta_store::kArenaScene - vendetta_store::kFixedArea;
        g_originalSetUp = runtime.lookupFunction(kSetUp);
        if (!g_originalSetUp || !vendetta_store::fixed(crowdAt, vendetta_store::kCrowdRoom, "the arena's crowd") ||
            !vendetta_store::fixed(clipsAt, vendetta_store::kCrowdRoom, "the arena's crowd clips") ||
            !vendetta_store::fixed(sceneAt, vendetta_store::kArenaSceneRoom, "a mod arena's stand-in") ||
            !vendetta_store::fixed(vendetta_store::kArenaFlyIn - vendetta_store::kFixedArea, vendetta_store::kArenaFlyInRoom,
                                   "a mod arena's fly-in"))
        {
            std::cerr << "[arenas] the arena's set-up or the store's room not found: the 3-D crowd can't be read" << std::endl;
            return;
        }
        runtime.replaceFunction(kSetUp, setUp);
        if ((g_originalSounds = runtime.lookupFunction(kSounds)) != nullptr)
            runtime.replaceFunction(kSounds, sounds);
        g_fullStandIn = std::getenv("VENDETTA_ARENA_FULLSTANDIN") && *std::getenv("VENDETTA_ARENA_FULLSTANDIN") != '0';
        findModArenas();
        const char *value = std::getenv("VENDETTA_ARENADRAW");
        const bool off = !value || !*value || std::string(value) == "0";
        if (off && g_modArenas.empty())
            return;
        if (!g_modArenas.empty() && (g_originalPicture = runtime.lookupFunction(kPicture)) != nullptr)
            runtime.replaceFunction(kPicture, picture);
        const std::string v = off ? "" : value;
        if (off)
            g_mode = Mode::Off; // our draw only for the mods' arenas
        else if (v == "vram")
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
        if (const char *t = std::getenv("VENDETTA_ARENA_TESTTEX"); t && std::strncmp(t, "checker", 7) == 0)
        {
            g_checker = t[7] == ':' ? static_cast<uint32_t>(std::clamp(std::atoi(t + 8), 1, 64)) : 1u;
            g_texScale = 8;
        }
        if (const char *k = std::getenv("VENDETTA_ARENA_TEXSCALE"))
            g_texScale = static_cast<uint32_t>(std::clamp(std::atoi(k), 1, 16));
        g_log = std::getenv("VENDETTA_ARENALOG") && *std::getenv("VENDETTA_ARENALOG") != '0';
        g_originalDraw = runtime.lookupFunction(kDraw);
        g_originalDrawScrolled = runtime.lookupFunction(kDrawScrolled);
        g_originalDrawShadow = runtime.lookupFunction(kDrawShadow);
        g_originalUpload = runtime.lookupFunction(kUpload);
        g_originalDrawPost = runtime.lookupFunction(kDrawPost);
        g_originalDrawPart = runtime.lookupFunction(kDrawPart);
        if (!g_originalDraw || !g_originalDrawScrolled || !g_originalDrawShadow || !g_originalUpload ||
            !g_originalDrawPost || !g_originalDrawPart)
        {
            std::cerr << "[arenas] the game's draw routines not found: off" << std::endl;
            g_mode = Mode::Off;
            return;
        }
        runtime.replaceFunction(kDraw, draw);
        runtime.replaceFunction(kDrawScrolled, drawScrolled);
        runtime.replaceFunction(kDrawShadow, drawShadow);
        runtime.replaceFunction(kDrawPost, drawPostHook);
        // installed before the wrestlers' draw, which hooks the same routine and passes the arena's calls on to this one
        runtime.replaceFunction(kDrawPart, drawPartHook);
        runtime.replaceFunction(kUpload, upload);
        if (g_log && (g_originalIntros = runtime.lookupFunction(kIntros)) != nullptr)
            runtime.replaceFunction(kIntros, intros);
        if (g_mode == Mode::Off)
        {
            std::cerr << "[arenas] the mods' arenas drawn by our code (" << g_modArenas.size() << ")" << std::endl;
            return;
        }
        std::cerr << "[arenas] on: the arena drawn by our code ("
                  << (g_mode == Mode::Vram ? "the game's uploaded textures" : g_mode == Mode::Ab ? "in turn with the game's"
                      : g_mode == Mode::Flip ? "F9 swaps it with the game's" : "textures as host images")
                  << ")" << std::endl;
    }
}

void installVendettaArenas(PS2Runtime &runtime) { vendetta_arenas::install(runtime); }
