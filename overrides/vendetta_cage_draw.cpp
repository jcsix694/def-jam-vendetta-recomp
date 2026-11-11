// Def Jam Vendetta (USA, SLUS-20639) - the cage match's cage, drawn by our own code, as the user's blueprint and picture
// (the classic steel cage): it stands on the apron's outer edge from the apron up (only the ring's own posts reach the
// floor), each side (4 on the game's square, 6 on the six-sided ring) chain-link panels between upright tubes, a box
// truss of tubes round the top, a corner post at each corner, a framed door at one end of one side (only the look: the
// way out is over the top), no roof. Silver; the sides facing the camera fainter, the posts near it fading as the game's.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// When: each match picture, at the start of FUN_001a2cf8 (the ropes' spring, called right after the ring is drawn in
// every venue; vendetta_ring.cpp's hook calls drawCage): the arena, the crowd, the wrestlers and the ring are drawn,
// only the zoom trail and the HUD follow. The camera of the picture is the matrix FUN_001a2de8 (the ring's draw) is
// given in $a0 (the view 0x258000 times the projection 0x258040; vendetta_ring.cpp's hook calls ringDrawing).
// How: as the arena draw draws a post (vendetta_arenas.cpp drawPost, the game's FUN_001b3820): each polygon times the
// matrix, cut at the near limit, divided, cut to the screen, sent as a fan in one GIF packet of ours into the game's
// stream (submitChainGif, in order behind the game's own). Context 2 with the depth test on: the far sides behind the
// wrestlers and the ropes, the near sides over them. The mesh is a texture made here (no file, nothing from any game)
// as a host image of the graphics card renderer, larger than its GS size so paraLLEl-GS samples it with mipmaps and
// repeats it (no shimmer); the steel a second one. Without the graphics card renderer nothing is drawn.
// The pieces are a list (a polygon, its image, solid or see-through, its side) built once per match. A model can fill it:
// cage4.vcage (the square) / cage6.vcage (six sides) in the cage's folder (vendetta_cage::cageFolder: the standard cage
// the setup copies from standard\cage\, or a cage\ folder of one's own beside the game), made from a Blender model
// (the polygons in the game's units, their texture places, the textures; the textures drawn at 4 times their size so
// paraLLEl-GS samples them with mipmaps). Without the file the cage built here is drawn.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_gpu.h"
#include "vendetta_cage.h"
#include "vendetta_gsdraw.h"
#include "vendetta_ring.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace vendetta_cage_draw
{
    using vendetta_gsdraw::Packet;

    constexpr uint32_t kRuleWord = 0x2361CCu;
    constexpr uint32_t kArenaColour = 0x1430D60u; // the colour scale every scenery draw is given (floats, 1 = as is)
    constexpr uint32_t kCameraEye = 0x258090u;    // the fight camera's place (x, y, z)

    // the cage's places (the game's units: a wrestler about 22 tall, the mat and the apron at 10.5): its walls
    // (vendetta_cage.cpp) at 43; the frame's middle a little outside, so the posts clear the game's ring posts (to 42.7)
    constexpr float kFrame = 43.5f;
    constexpr float kBase = 10.5f, kTop = 54.5f;     // from the apron up, 2 wrestlers tall (the user's choice)
    constexpr float kTrussDepth = 4.0f, kTrussHalf = 0.9f; // the top's box truss: 4 high, 1.8 across
    constexpr float kTube = 0.45f, kChord = 0.32f, kBrace = 0.18f, kCornerTube = 0.6f; // tubes' half thickness
    constexpr float kPanelWidth = 28.0f;             // about this wide between upright tubes
    constexpr float kTileUnits = 8.0f;               // the mesh texture repeats every 8 units (8 diamonds across)
    constexpr float kDoorWidth = 12.0f, kDoorHeight = 26.0f, kDoorFromCorner = 4.0f;
    constexpr int kNearAlpha = 0x40, kSolid = 0x80;  // the sides facing the camera: half as strong

    // the host images (ids 0x52000000 on: free) at their GS addresses; GS size 64 x 64, larger images: the HD path
    constexpr uint32_t kMeshImage = 0x52000001u, kSteelImage = 0x52000002u;
    constexpr uint32_t kMeshTbp = 0x2B00u, kSteelTbp = 0x2B40u;
    constexpr uint32_t kMeshSize = 512u, kSteelSize = 256u;

    enum class Kind { Steel, Mesh };

    struct Point
    {
        float x, y, z, s, t;
    };

    struct Piece
    {
        Point p[8];
        int n = 4;
        int image = 0;       // its picture, in g_images
        Kind kind = Kind::Steel;
        int side = -1;       // the side it belongs to; a corner post has two (faint only when both face the camera)
        int sideB = -1;
        float shade = 1.0f;  // a face's own light
        bool upright = false; // an upright tube: fades as the camera comes near it, as the game's ring posts
        float cx = 0.0f, cz = 0.0f;
    };

    std::vector<Piece> g_pieces;
    int g_builtSides = -1;
    int g_sides = 0;
    float g_normal[16][2] = {}, g_middle[16][2] = {};
    float g_matrix[16] = {};
    bool g_haveMatrix = false, g_imagesMade = false, g_toldMatch = false;
    PS2Runtime *g_runtime = nullptr;

    GSGpuRenderer *gpu() { return g_runtime ? g_runtime->gs().gpuRendererForSettings() : nullptr; }
    float readF(uint8_t *rdram, uint32_t a) { float f; std::memcpy(&f, rdram + (a & PS2_RAM_MASK), 4); return f; }
    bool cageMatch(uint8_t *rdram) { return (Ps2FastRead16(rdram, kRuleWord) & vendetta_cage::kRuleCage) != 0u; }

    // ---- the two pictures, made once a run

    uint32_t rgba(int r, int g, int b, int a)
    {
        auto c = [](int v) { return static_cast<uint32_t>(std::clamp(v, 0, 255)); };
        return c(r) | c(g) << 8 | c(b) << 16 | c(a) << 24;
    }

    // chain-link: wires along both diagonals, 8 diamonds across, the GS's alpha (0x80 the wire, 0 a hole); the holes
    // keep the wire's colour so the mipmaps' averages stay silver
    std::vector<uint32_t> meshPixels()
    {
        const int n = static_cast<int>(kMeshSize), period = n / 8;
        const float half = 2.6f; // a wire's half width in pixels
        std::vector<uint32_t> out(static_cast<size_t>(n) * n);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
            {
                auto distance = [&](int v) {
                    const int m = ((v % period) + period) % period;
                    return std::min(m, period - m) / 1.41421356f;
                };
                const float d = std::min(distance(x + y), distance(x - y));
                const float cover = std::clamp(half + 0.5f - d, 0.0f, 1.0f);
                const float light = 1.0f - std::min(d / half, 1.0f) * 0.35f; // brighter along the wire's middle
                out[static_cast<size_t>(y) * n + x] =
                    rgba(int(196 * light), int(200 * light), int(206 * light), int(0x80 * cover));
            }
        return out;
    }

    // brushed steel: silver with fine streaks along the length (t), solid
    std::vector<uint32_t> steelPixels()
    {
        const int n = static_cast<int>(kSteelSize);
        std::vector<uint32_t> out(static_cast<size_t>(n) * n);
        uint32_t seed = 0x2545F491u;
        std::vector<int> streak(n);
        for (int x = 0; x < n; ++x)
        {
            seed = seed * 1664525u + 1013904223u;
            streak[x] = static_cast<int>((seed >> 24) % 21u) - 10;
        }
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
            {
                const float across = std::sin(3.14159265f * (x + 0.5f) / n); // rounded: lighter in the middle
                const int v = int(150 + 55 * across) + streak[x];
                out[static_cast<size_t>(y) * n + x] = rgba(v, v + 3, v + 8, 0x80);
            }
        return out;
    }

    void makeImages(uint8_t *rdram)
    {
        (void)rdram;
        GSGpuRenderer *r = gpu();
        if (g_imagesMade || !r)
            return;
        r->setHostImage(kMeshImage, kMeshSize, kMeshSize, meshPixels());
        r->setHostImage(kSteelImage, kSteelSize, kSteelSize, steelPixels());
        g_imagesMade = true;
        // a register write the graphics card thread answers by making the queued images now, in the loading screen
        Packet packet;
        packet.ad(0x71u, 0x3FFFu);
        packet.end();
        g_runtime->memory().submitChainGif(reinterpret_cast<const uint8_t *>(packet.q.data()),
                                           static_cast<uint32_t>(packet.q.size() * 8u));
    }

    // ---- the pictures a cage is drawn with: the two made here, or a model's

    struct Image
    {
        uint32_t id = 0, tbp = 0;
        uint64_t tex0 = 0;
        bool seeThrough = false; // drawn without writing depth (a mesh, a truss)
    };

    // TEX0_2 for a picture of 2^tw x 2^th at the GS (the host image larger: the HD path), CT32, TCC, MODULATE
    uint64_t tex0(uint32_t tbp, uint32_t tw, uint32_t th)
    {
        const uint64_t tbw = std::max<uint32_t>(1u, (1u << tw) / 64u);
        return uint64_t(tbp) | (tbw << 14) | (uint64_t(tw) << 26) | (uint64_t(th) << 30) | (1ull << 34);
    }

    std::vector<Image> g_images;

    struct Model
    {
        bool ok = false;
        std::vector<Image> images;
        std::vector<Piece> pieces; // their sides set when the cage is built
        std::vector<std::vector<uint32_t>> pixels;
        std::vector<uint32_t> widths, heights;
    };
    Model g_models[2]; // 0 the square, 1 six sides
    bool g_modelsRead = false, g_modelImagesMade = false;

    uint32_t log2of(uint32_t v)
    {
        uint32_t n = 0;
        while ((1u << n) < v && n < 10u)
            ++n;
        return n;
    }

    // a .vcage file: "VCAG", 1, the pictures {name[32], width, height, flags (1 see-through), RGBA (alpha 0-128)}, the
    // polygons {picture, n, n x (x, y, z, s, t)}
    bool readModel(const std::filesystem::path &path, Model &m, uint32_t firstId)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        auto u32 = [&]() {
            uint32_t v = 0;
            in.read(reinterpret_cast<char *>(&v), 4);
            return v;
        };
        char magic[4] = {};
        in.read(magic, 4);
        if (std::memcmp(magic, "VCAG", 4) != 0 || u32() != 1u)
            return false;
        const uint32_t images = u32();
        if (images == 0u || images > 8u)
            return false;
        for (uint32_t i = 0; i < images; ++i)
        {
            char name[32];
            in.read(name, 32);
            const uint32_t w = u32(), h = u32(), flags = u32();
            if (w == 0u || h == 0u || w > 1024u || h > 1024u || (w & (w - 1u)) || (h & (h - 1u)))
                return false;
            std::vector<uint32_t> px(static_cast<size_t>(w) * h);
            in.read(reinterpret_cast<char *>(px.data()), static_cast<std::streamsize>(px.size() * 4u));
            Image im;
            im.id = firstId + i;
            im.tbp = 0x2B00u + 0x40u * i;
            im.tex0 = tex0(im.tbp, log2of(w), log2of(h));
            im.seeThrough = (flags & 1u) != 0u;
            m.images.push_back(im);
            m.pixels.push_back(std::move(px));
            m.widths.push_back(w);
            m.heights.push_back(h);
        }
        const uint32_t polys = u32();
        if (!in || polys > 100000u)
            return false;
        for (uint32_t i = 0; i < polys; ++i)
        {
            const uint32_t image = u32(), n = u32();
            if (!in || image >= images || n < 3u || n > 64u)
                return false;
            std::vector<Point> loop(n);
            in.read(reinterpret_cast<char *>(loop.data()), static_cast<std::streamsize>(n * sizeof(Point)));
            // a fan of at most 8 points a piece
            for (uint32_t s = 1; s + 1 < n; s += 6)
            {
                Piece piece;
                piece.image = static_cast<int>(image);
                piece.kind = m.images[image].seeThrough ? Kind::Mesh : Kind::Steel;
                piece.p[0] = loop[0];
                piece.n = 1;
                for (uint32_t k = s; k < n && piece.n < 8; ++k)
                    piece.p[piece.n++] = loop[k];
                float x = 0.0f, z = 0.0f;
                for (int k = 0; k < piece.n; ++k)
                    x += piece.p[k].x, z += piece.p[k].z;
                piece.cx = x / piece.n, piece.cz = z / piece.n;
                m.pieces.push_back(piece);
                if (piece.n < 8)
                    break;
            }
        }
        m.ok = static_cast<bool>(in);
        return m.ok;
    }

    void readModels()
    {
        if (g_modelsRead)
            return;
        g_modelsRead = true;
        const std::filesystem::path folder = vendetta_cage::cageFolder();
        const char *names[2] = {"cage4.vcage", "cage6.vcage"};
        for (int i = 0; i < 2; ++i)
        {
            Model &m = g_models[i];
            if (readModel(folder / names[i], m, 0x52000010u + 0x10u * i))
                std::cerr << "[cage] the cage's model for " << (i ? "six sides" : "the square") << ": " << m.images.size()
                          << " pictures, " << m.pieces.size() << " pieces (" << (folder / names[i]).string() << ")" << std::endl;
            else
                m = Model{};
        }
    }

    // the models' pictures at 4 times their size (the HD path: mipmaps, no shimmer), made once a run in a loading screen
    void makeModelImages()
    {
        GSGpuRenderer *r = gpu();
        if (g_modelImagesMade || !r)
            return;
        g_modelImagesMade = true;
        bool any = false;
        for (Model &m : g_models)
            for (size_t i = 0; m.ok && i < m.images.size(); ++i)
            {
                const uint32_t w = m.widths[i], h = m.heights[i], k = 4u;
                std::vector<uint32_t> big(static_cast<size_t>(w) * k * h * k);
                for (uint32_t y = 0; y < h * k; ++y)
                    for (uint32_t x = 0; x < w * k; ++x)
                        big[static_cast<size_t>(y) * w * k + x] = m.pixels[i][static_cast<size_t>(y / k) * w + x / k];
                r->setHostImage(m.images[i].id, w * k, h * k, std::move(big));
                any = true;
            }
        if (any)
        {
            Packet packet;
            packet.ad(0x71u, 0x3FFFu); // the graphics card thread makes them now
            packet.end();
            g_runtime->memory().submitChainGif(reinterpret_cast<const uint8_t *>(packet.q.data()),
                                               static_cast<uint32_t>(packet.q.size() * 8u));
        }
    }

    // ---- the pieces, built for the ring's shape

    struct V3
    {
        float x, y, z;
    };
    V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
    float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    V3 unit(V3 a)
    {
        const float l = std::sqrt(dot(a, a));
        return l > 0.0f ? a * (1.0f / l) : a;
    }

    // a tube of square cross-section from a to b, `half` each way; `hint` the way one pair of its faces looks (the
    // side's outward normal, or the corner's), so each tube's faces line up with the cage
    void addTube(V3 a, V3 b, V3 hint, float half, int side, int sideB, bool caps = false)
    {
        const V3 d = unit(b - a);
        V3 u = hint - d * dot(hint, d);
        if (dot(u, u) < 1e-6f)
            u = {0.0f, 1.0f, 0.0f};
        u = unit(u);
        const V3 w = cross(d, u);
        const V3 c[4] = {(u + w) * half, (w - u) * half, (u * -1.0f - w) * half, (u - w) * half};
        const float along = std::sqrt(dot(b - a, b - a)) / kTileUnits;
        const V3 light = unit({0.3f, 1.0f, 0.5f});
        Piece f;
        f.kind = Kind::Steel;
        f.side = side;
        f.sideB = sideB;
        f.upright = std::fabs(d.y) > 0.9f;
        f.cx = (a.x + b.x) * 0.5f, f.cz = (a.z + b.z) * 0.5f;
        for (int i = 0; i < 4; ++i)
        {
            const V3 e0 = c[i], e1 = c[(i + 1) % 4];
            const V3 n = unit(e0 + e1);
            f.shade = 0.72f + 0.4f * std::max(0.0f, dot(n, light));
            const V3 p0 = a + e0, p1 = b + e0, p2 = b + e1, p3 = a + e1;
            f.p[0] = {p0.x, p0.y, p0.z, 0.0f, 0.0f};
            f.p[1] = {p1.x, p1.y, p1.z, 0.0f, along};
            f.p[2] = {p2.x, p2.y, p2.z, 0.25f, along};
            f.p[3] = {p3.x, p3.y, p3.z, 0.25f, 0.0f};
            g_pieces.push_back(f);
        }
        if (caps)
            for (const V3 &e : {a, b})
            {
                f.shade = 1.05f;
                for (int i = 0; i < 4; ++i)
                {
                    const V3 q = e + c[i];
                    f.p[i] = {q.x, q.y, q.z, i == 1 || i == 2 ? 0.25f : 0.0f, i >= 2 ? 0.25f : 0.0f};
                }
                g_pieces.push_back(f);
            }
    }

    void build()
    {
        g_pieces.clear();
        const int ringSides = vendetta_ring::activeSides();
        g_sides = ringSides > 0 ? ringSides : 4;
        g_builtSides = ringSides;
        const float firstAngle = ringSides > 0 ? 90.0f : 135.0f, pi = 3.14159265358979f;
        const float toCorner = 1.0f / std::cos(pi / g_sides);
        auto corner = [&](int k, float d, float y) {
            const float rad = (firstAngle - 360.0f * k / g_sides) * pi / 180.0f;
            return V3{std::sin(rad) * d * toCorner, y, std::cos(rad) * d * toCorner};
        };
        int doorSide = 0;
        for (int k = 0; k < g_sides; ++k)
        {
            const float rad = (firstAngle - 360.0f * (k + 0.5f) / g_sides) * pi / 180.0f;
            g_normal[k][0] = std::sin(rad), g_normal[k][1] = std::cos(rad);
            g_middle[k][0] = g_normal[k][0] * kFrame, g_middle[k][1] = g_normal[k][1] * kFrame;
            if (-g_normal[k][1] > -g_normal[doorSide][1])
                doorSide = k; // the side most toward -z: across the ring from where the fight camera starts
        }
        const Model &model = g_models[g_sides == 6 ? 1 : 0];
        if (model.ok && (g_sides == 4 || g_sides == 6))
        {
            // the model's pieces, each given the side it faces (a corner's piece both of its sides)
            g_images = model.images;
            g_pieces = model.pieces;
            for (Piece &p : g_pieces)
            {
                int best = 0, second = 1;
                float dots[16];
                for (int k = 0; k < g_sides; ++k)
                    dots[k] = p.cx * g_normal[k][0] + p.cz * g_normal[k][1];
                for (int k = 0; k < g_sides; ++k)
                    if (dots[k] > dots[best])
                        best = k;
                second = best == 0 ? 1 : 0;
                for (int k = 0; k < g_sides; ++k)
                    if (k != best && dots[k] > dots[second])
                        second = k;
                p.side = best;
                p.sideB = dots[best] - dots[second] < 2.0f ? second : -1;
            }
            std::cerr << "[cage] the cage drawn from its model: " << g_sides << " sides, " << g_pieces.size() << " pieces"
                      << std::endl;
            return;
        }
        g_images = {{kSteelImage, kSteelTbp, tex0(kSteelTbp, 6, 6), false}, {kMeshImage, kMeshTbp, tex0(kMeshTbp, 6, 6), true}};
        const float meshTop = kTop - kTrussDepth;
        for (int k = 0; k < g_sides; ++k)
        {
            const V3 out = {g_normal[k][0], 0.0f, g_normal[k][1]};
            const V3 a = corner(k, kFrame, kBase), b = corner(k + 1, kFrame, kBase);
            const V3 span = b - a;
            const float length = std::sqrt(dot(span, span));
            const V3 along = span * (1.0f / length);
            auto at = [&](float x, float y, float o) { return a + along * x + V3{0.0f, y - kBase, 0.0f} + out * o; };
            // the mesh: post to post, from the apron to the truss (seen from both faces)
            Piece m;
            m.kind = Kind::Mesh;
            m.image = 1;
            m.side = k;
            m.p[0] = {a.x, kBase, a.z, 0.0f, 0.0f};
            m.p[1] = {b.x, kBase, b.z, length / kTileUnits, 0.0f};
            m.p[2] = {b.x, meshTop, b.z, length / kTileUnits, (meshTop - kBase) / kTileUnits};
            m.p[3] = {a.x, meshTop, a.z, 0.0f, (meshTop - kBase) / kTileUnits};
            g_pieces.push_back(m);
            // the base on the apron, and the upright tubes between the panels
            addTube(at(0.0f, kBase + kTube, 0.0f), at(length, kBase + kTube, 0.0f), out, kTube, k, -1);
            const int panels = std::max(1, static_cast<int>(std::lround(length / kPanelWidth)));
            for (int i = 1; i < panels; ++i)
                addTube(at(length * i / panels, kBase, 0.0f), at(length * i / panels, meshTop, 0.0f), out, kTube, k, -1);
            // the truss: a top and a bottom tube on its outer and inner faces, zig-zag braces between them on each face,
            // cross tubes at the panels' uprights and the ends
            const float low = meshTop + kChord, high = kTop - kChord;
            for (const float face : {kTrussHalf, -kTrussHalf})
            {
                addTube(at(0.0f, high, face), at(length, high, face), out, kChord, k, -1);
                addTube(at(0.0f, low, face), at(length, low, face), out, kChord, k, -1);
                const int braces = std::max(2, static_cast<int>(std::lround(length / 4.5f)));
                for (int j = 0; j < braces; ++j)
                {
                    const float x0 = length * j / braces, x1 = length * (j + 1) / braces;
                    if (j % 2 == 0)
                        addTube(at(x0, low, face), at(x1, high, face), out, kBrace, k, -1);
                    else
                        addTube(at(x0, high, face), at(x1, low, face), out, kBrace, k, -1);
                }
            }
            for (int i = 0; i <= panels; ++i)
                for (const float y : {low, high})
                    addTube(at(length * i / panels, y, -kTrussHalf), at(length * i / panels, y, kTrussHalf), along, kChord, k, -1);
            if (k == doorSide)
            {
                // the door: a framed panel at one end of the side, from the apron up (only the look), with its handle
                const float x0 = kDoorFromCorner, x1 = kDoorFromCorner + kDoorWidth, yTop = kBase + kDoorHeight;
                const float o = 0.25f; // a little outside the mesh
                addTube(at(x0, kBase, o), at(x0, yTop, o), out, kTube, k, -1, true);
                addTube(at(x1, kBase, o), at(x1, yTop, o), out, kTube, k, -1, true);
                addTube(at(x0, yTop, o), at(x1, yTop, o), out, kTube, k, -1);
                addTube(at(x0, kBase + kTube, o), at(x1, kBase + kTube, o), out, kTube, k, -1);
                addTube(at(x0, kBase + kDoorHeight * 0.5f, o), at(x1, kBase + kDoorHeight * 0.5f, o), out, kBrace, k, -1);
                addTube(at(x1 - 2.6f, kBase + kDoorHeight * 0.5f, o + 0.9f), at(x1 - 1.2f, kBase + kDoorHeight * 0.5f, o + 0.9f),
                        out, 0.25f, k, -1, true);
            }
        }
        for (int k = 0; k < g_sides; ++k)
        {
            // a post at each corner, from the apron to the top of the truss, turned to the corner
            const V3 c0 = corner(k, kFrame, kBase), c1 = corner(k, kFrame, kTop + 0.6f);
            addTube(c0, c1, unit({c0.x, 0.0f, c0.z}), kCornerTube, (k + g_sides - 1) % g_sides, k, true);
        }
        std::cerr << "[cage] the cage drawn: " << g_sides << " sides, " << g_pieces.size() << " pieces, the door on side "
                  << doorSide << std::endl;
    }

    // ---- one polygon into the packet, as the post draw does it

    struct Lane
    {
        float f[12]; // s t q 0 | r g b a | x y z w
    };

    float eeDiv(float a, float b) { return b == 0.0f ? std::copysign(INFINITY, a * 0.0f) : a / b; }
    uint32_t toUnsigned(float v)
    {
        if (!(v >= 1.0f))
            return 0u;
        if (v >= 4294967296.0f)
            return 0xFFFFFFFFu;
        return static_cast<uint32_t>(v);
    }

    int clip(const Lane *in, int n, Lane *out, int lane, float plane, bool keepBelow)
    {
        int m = 0;
        for (int i = 0; i < n; ++i)
        {
            const Lane &cur = in[i], &next = in[(i + 1) % n];
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
            Lane &v = out[m++];
            for (int k = 0; k < 12; ++k)
                v.f[k] = cur.f[k] + (next.f[k] - cur.f[k]) * t;
        }
        return m;
    }

    uint32_t g_polygons = 0;

    void polygon(Packet &packet, const Piece &piece, const float colour[3], int alpha)
    {
        Lane one[24], two[24];
        for (int i = 0; i < piece.n; ++i)
        {
            const Point &p = piece.p[i];
            float c[4];
            for (int j = 0; j < 4; ++j)
                c[j] = ((p.x * g_matrix[j] + p.y * g_matrix[4 + j]) + p.z * g_matrix[8 + j]) + g_matrix[12 + j];
            Lane &l = one[i];
            l.f[0] = p.s, l.f[1] = p.t, l.f[2] = 1.0f, l.f[3] = 0.0f;
            l.f[4] = colour[0] * piece.shade, l.f[5] = colour[1] * piece.shade, l.f[6] = colour[2] * piece.shade;
            l.f[7] = static_cast<float>(alpha);
            std::copy(c, c + 4, l.f + 8);
        }
        int n = clip(one, piece.n, two, 10, 1048574.0f, true); // the near limit, z not yet divided
        for (int i = 0; i < n; ++i)
        {
            Lane &p = two[i];
            const float w = p.f[11];
            p.f[0] = eeDiv(p.f[0], w), p.f[1] = eeDiv(p.f[1], w), p.f[2] = eeDiv(p.f[2], w);
            p.f[8] = eeDiv(p.f[8], w), p.f[9] = eeDiv(p.f[9], w);
        }
        if (n)
            n = clip(two, n, one, 9, 1936.0f, false);
        if (n)
            n = clip(one, n, two, 9, 2160.0f, true);
        if (n)
            n = clip(two, n, one, 8, 1792.0f, false);
        if (n)
            n = clip(one, n, two, 8, 2304.0f, true);
        if (n < 3)
            return;
        // a fan: PRIM 0x25D (fan, Gouraud, textured, blended, context 2), PACKED, ST / RGBAQ / XYZF2
        packet.tag(uint64_t(n) | (uint64_t(0x312EC000u) << 32), 0x412ull);
        for (int i = 0; i < n; ++i)
        {
            const Lane &p = two[i];
            uint32_t s, t, q;
            std::memcpy(&s, &p.f[0], 4);
            std::memcpy(&t, &p.f[1], 4);
            std::memcpy(&q, &p.f[2], 4);
            packet.add(uint64_t(s) | (uint64_t(t) << 32), uint64_t(q));
            packet.add(uint64_t(std::min(toUnsigned(p.f[4]), 255u)) | (uint64_t(std::min(toUnsigned(p.f[5]), 255u)) << 32),
                       uint64_t(std::min(toUnsigned(p.f[6]), 255u)) | (uint64_t(std::min(toUnsigned(p.f[7]), 128u)) << 32));
            packet.add(uint64_t(toUnsigned(p.f[8] * 16.0f)) | (uint64_t(toUnsigned(p.f[9] * 16.0f)) << 32),
                       uint64_t(toUnsigned(p.f[10]) << 4));
        }
        ++g_polygons;
    }

    void draw(uint8_t *rdram)
    {
        if (!cageMatch(rdram) || !g_haveMatrix || !gpu())
            return;
        if (!g_imagesMade)
            makeImages(rdram);
        if (g_builtSides != vendetta_ring::activeSides() || g_pieces.empty())
            build();
        g_haveMatrix = false;

        // which sides face the camera (across the floor)
        const float eyeX = readF(rdram, kCameraEye), eyeZ = readF(rdram, kCameraEye + 8u);
        bool facing[16] = {};
        for (int k = 0; k < g_sides; ++k)
            facing[k] = (eyeX - g_middle[k][0]) * g_normal[k][0] + (eyeZ - g_middle[k][1]) * g_normal[k][1] > 0.0f;
        auto alphaOf = [&](const Piece &p) {
            const bool towardCamera = p.sideB < 0 ? facing[p.side] : facing[p.side] && facing[p.sideB];
            int a = towardCamera ? kNearAlpha : kSolid;
            if (p.upright) // as the game fades its ring posts near the camera: 0.4 of it at the camera, all of it from 50 away
            {
                const float dx = p.cx - eyeX, dz = p.cz - eyeZ, d2 = dx * dx + dz * dz;
                if (d2 < 2500.0f)
                    a = static_cast<int>(a * (0.4f + 0.6f * d2 / 2500.0f));
            }
            if (p.kind == Kind::Mesh && towardCamera)
            {
                // the mesh facing a camera close to it: its wires would fill the picture, so it fades away, to a tenth
                // of its strength 4 units off it, all of it from 30 away
                const float off = (eyeX - g_middle[p.side][0]) * g_normal[p.side][0] + (eyeZ - g_middle[p.side][1]) * g_normal[p.side][1];
                if (off < 30.0f)
                    a = static_cast<int>(a * std::clamp((off - 4.0f) / 26.0f, 0.1f, 1.0f));
            }
            return a;
        };
        const float c[3] = {128.0f * readF(rdram, kArenaColour), 128.0f * readF(rdram, kArenaColour + 4u),
                            128.0f * readF(rdram, kArenaColour + 8u)};

        Packet packet;
        packet.ad(0x48u, 0x5001Bull);   // TEST_2: the alpha test (the mesh's holes), the depth test
        packet.ad(0x43u, 0x44ull);      // ALPHA_2: blended by the source's alpha
        packet.ad(0x09u, 0ull);         // CLAMP_2: repeat
        packet.ad(0x15u, 0x60ull);      // TEX1_2
        g_polygons = 0;
        std::vector<int> alphas(g_pieces.size());
        for (size_t i = 0; i < g_pieces.size(); ++i)
            alphas[i] = alphaOf(g_pieces[i]);
        // far first, then near (the faded); in each the solid pictures (the depth written, far only), then the
        // see-through ones (not written, as the game's see-through parts); a picture bound once for its pieces
        for (int pass = 0; pass < 2; ++pass)
            for (const bool seeThrough : {false, true})
            {
                const bool nearPass = pass == 1;
                packet.ad(0x4Fu, nearPass || seeThrough ? 0x1'00000080ull : 0x80ull);
                for (size_t im = 0; im < g_images.size(); ++im)
                {
                    if (g_images[im].seeThrough != seeThrough)
                        continue;
                    bool bound = false;
                    for (size_t i = 0; i < g_pieces.size(); ++i)
                    {
                        const Piece &p = g_pieces[i];
                        if (p.image != static_cast<int>(im) || (alphas[i] < kSolid) != nearPass)
                            continue;
                        if (!bound)
                        {
                            packet.ad(0x07u, g_images[im].tex0);
                            packet.ad(0x71u, uint64_t(g_images[im].tbp) | (uint64_t(g_images[im].id) << 32));
                            bound = true;
                        }
                        polygon(packet, p, c, alphas[i]);
                    }
                }
            }
        // the state as the ring left it (both contexts' ZBUF not written, TEST off, ALPHA 0x44), the pictures unbound
        packet.ad(0x4Fu, 0x1'00000080ull);
        packet.ad(0x48u, 0ull);
        packet.ad(0x43u, 0x44ull);
        for (const Image &im : g_images)
            packet.ad(0x71u, uint64_t(im.tbp));
        packet.end();
        g_runtime->memory().submitChainGif(reinterpret_cast<const uint8_t *>(packet.q.data()),
                                           static_cast<uint32_t>(packet.q.size() * 8u));
        if (!g_toldMatch)
        {
            g_toldMatch = true;
            std::cerr << "[cage] the cage's first picture: " << g_polygons << " polygons, " << packet.q.size() * 8u
                      << " bytes" << std::endl;
        }
    }
}

void vendetta_cage::ringDrawing(uint8_t *rdram, uint32_t cameraMatrix)
{
    using namespace vendetta_cage_draw;
    if (!cageMatch(rdram))
        return;
    std::memcpy(g_matrix, rdram + (cameraMatrix & PS2_RAM_MASK), sizeof(g_matrix));
    g_haveMatrix = true;
}

std::filesystem::path vendetta_cage::cageFolder()
{
    const std::filesystem::path game = PS2Runtime::getIoPaths().elfDirectory;
    std::error_code ec;
    if (std::filesystem::is_directory(game / "cage", ec))
        return game / "cage";
    return game / "standard" / "cage";
}

bool vendetta_cage::screenRight(float &x, float &z)
{
    using namespace vendetta_cage_draw;
    if (!g_haveMatrix)
        return false;
    x = g_matrix[0];
    z = g_matrix[8];
    const float length = std::sqrt(x * x + z * z);
    if (length < 1e-6f)
        return false;
    x /= length;
    z /= length;
    return true;
}

void vendetta_cage::drawCage(uint8_t *rdram, PS2Runtime &runtime)
{
    vendetta_cage_draw::g_runtime = &runtime;
    vendetta_cage_draw::draw(rdram);
}

void vendetta_cage::prepareDraw(uint8_t *rdram, PS2Runtime &runtime)
{
    using namespace vendetta_cage_draw;
    g_runtime = &runtime;
    g_toldMatch = false;
    g_builtSides = -2; // built again at the first picture (the ring's shape is set later in the match's start)
    if (cageMatch(rdram))
    {
        readModels();
        makeModelImages();
        makeImages(rdram);
    }
}
