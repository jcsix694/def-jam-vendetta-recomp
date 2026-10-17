// Def Jam Vendetta (USA, SLUS-20639) — pictures from mods' files as host images (see vendetta_pictures.h).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
#include "vendetta_pictures.h"

#include "ps2_runtime.h"
#include "runtime/gs/gs_gpu.h"
#include "vendetta_mods.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace vendetta_pictures
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr uint32_t kBlank = 1u;                      // a transparent pixel: shown while a picture is read
        constexpr uint64_t kBudget = uint64_t(256) << 20u;   // graphics card memory kept for pictures
        constexpr auto kRecent = std::chrono::seconds(10);   // a picture shown this recently is never dropped

        enum class State
        {
            Idle,    // not read (or dropped)
            Queued,  // being read
            Ready,   // on its way to / on the graphics card
            Failed,  // the file can't be read (said once)
        };

        struct Picture
        {
            std::filesystem::path file;
            uint32_t width = 0, height = 0, scale = 1;
            Kind kind = Kind::Plain;
            State state = State::Idle;
            uint64_t bytes = 0;
            Clock::time_point shown{};
        };

        PS2Runtime *g_runtime = nullptr;
        std::mutex g_lock;
        std::condition_variable g_wake;
        std::vector<Picture> g_pictures{Picture{}, Picture{}}; // ids 0 (none) and 1 (the blank)
        std::vector<uint32_t> g_queue;
        uint64_t g_bytes = 0;
        bool g_blankSent = false;
        bool g_saidNoGpu = false;

        // VENDETTA_PICTURELOG=1: each batch read, and each picture shown before it was read (the "no waiting" check)
        bool logOn()
        {
            static const bool on = std::getenv("VENDETTA_PICTURELOG") && *std::getenv("VENDETTA_PICTURELOG") != '0';
            return on;
        }

        GSGpuRenderer *gpu() { return g_runtime ? g_runtime->gs().gpuRendererForSettings() : nullptr; }

        // One picture read, decoded, sized and converted (the GS's alpha: 0x80 = opaque), then sent to the renderer.
        bool load(const Picture &p, uint32_t id, uint64_t &bytes)
        {
            std::vector<uint8_t> data;
            std::ifstream in(p.file, std::ios::binary);
            if (!in)
                return false;
            data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            std::vector<uint32_t> rgba;
            uint32_t w = 0, h = 0;
            if (!gsDecodePicture(data.data(), data.size(), 0u, 0u, rgba, w, h))
                return false;
            // the game's shape, at the largest whole multiple the file has detail for (at most `scale`)
            const uint32_t k = std::clamp<uint32_t>(std::min(w / p.width, h / p.height), 1u, p.scale);
            gsResizePicture(rgba, w, h, p.width * k, p.height * k);
            w = p.width * k;
            h = p.height * k;
            for (uint32_t &pixel : rgba)
            {
                const uint32_t a = pixel >> 24;
                const uint32_t gsAlpha = (a * 128u + 127u) / 255u;
                pixel = (p.kind == Kind::Shadow ? 0u : (pixel & 0x00FFFFFFu)) | (gsAlpha << 24);
            }
            bytes = uint64_t(w) * h * 4u * 4u / 3u; // with its smaller copies
            GSGpuRenderer *renderer = gpu();
            if (!renderer)
                return false;
            renderer->setHostImage(id, w, h, std::move(rgba));
            return true;
        }

        // Over the budget: the least recently shown pictures (not shown in the last seconds) are dropped.
        void keepBudget()
        {
            GSGpuRenderer *renderer = gpu();
            const auto old = Clock::now() - kRecent;
            while (g_bytes > kBudget && renderer)
            {
                uint32_t oldest = 0;
                for (uint32_t id = 2; id < g_pictures.size(); ++id)
                    if (g_pictures[id].state == State::Ready && g_pictures[id].shown < old &&
                        (!oldest || g_pictures[id].shown < g_pictures[oldest].shown))
                        oldest = id;
                if (!oldest)
                    return;
                renderer->dropHostImage(oldest);
                g_bytes -= g_pictures[oldest].bytes;
                g_pictures[oldest].state = State::Idle;
            }
        }

        // The background reader: everything queued, several files at once (vendetta_mods::forEachAtOnce).
        void reader()
        {
            for (;;)
            {
                std::vector<uint32_t> batch;
                std::vector<Picture> copies;
                {
                    std::unique_lock<std::mutex> lock(g_lock);
                    g_wake.wait(lock, [] { return !g_queue.empty(); });
                    batch.swap(g_queue);
                    for (uint32_t id : batch)
                        copies.push_back(g_pictures[id]);
                }
                std::vector<char> good(batch.size(), 0);
                std::vector<uint64_t> bytes(batch.size(), 0);
                const auto start = Clock::now();
                vendetta_mods::forEachAtOnce(batch.size(), [&](size_t i)
                {
                    good[i] = load(copies[i], batch[i], bytes[i]) ? 1 : 0;
                });
                std::lock_guard<std::mutex> lock(g_lock);
                if (logOn())
                    std::cerr << "[pictures] " << batch.size() << " read in "
                              << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count() << " ms ("
                              << (g_bytes >> 20) << " MB kept before them)" << std::endl;
                for (size_t i = 0; i < batch.size(); ++i)
                {
                    Picture &p = g_pictures[batch[i]];
                    if (good[i])
                    {
                        p.state = State::Ready;
                        p.bytes = bytes[i];
                        g_bytes += bytes[i];
                        continue;
                    }
                    p.state = State::Failed;
                    std::cerr << "[pictures] " << p.file.string() << " can't be read as a picture" << std::endl;
                }
                keepBudget();
            }
        }

        // The register 0x70 into the GS stream, in order with the game's own GIF transfers.
        void arm(uint32_t tbp, uint32_t id)
        {
            alignas(16) uint64_t packet[4] = {
                0x1000000000008001ull,                     // GIFtag: NLOOP 1, EOP, PACKED, NREG 1
                0xEull,                                    // REGS: A+D
                uint64_t(tbp & 0x3FFFu) | (uint64_t(id) << 32),
                0x70ull,                                   // the host-image register (paraLLEl-GS, a local addition)
            };
            g_runtime->memory().submitChainGif(reinterpret_cast<const uint8_t *>(packet), sizeof(packet));
        }

        void queue(uint32_t id)
        {
            Picture &p = g_pictures[id];
            if (p.state != State::Idle)
                return;
            p.state = State::Queued;
            g_queue.push_back(id);
            g_wake.notify_one();
        }
    }

    bool available()
    {
        const bool yes = gpu() != nullptr;
        if (!yes && !g_saidNoGpu)
        {
            g_saidNoGpu = true;
            std::cerr << "[pictures] the CPU renderer draws no pictures from files: the game's own are shown" << std::endl;
        }
        return yes;
    }

    uint32_t add(const std::filesystem::path &file, uint32_t width, uint32_t height, uint32_t scale, Kind kind)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        Picture p;
        p.file = file;
        p.width = std::max(1u, width);
        p.height = std::max(1u, height);
        p.scale = std::max(1u, scale);
        p.kind = kind;
        g_pictures.push_back(p);
        return static_cast<uint32_t>(g_pictures.size() - 1u);
    }

    void want(uint32_t id)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (id >= 2u && id < g_pictures.size())
        {
            g_pictures[id].shown = Clock::now(); // wanted soon: kept like a picture just shown
            queue(id);
        }
    }

    bool show(uint32_t tbp, uint32_t id)
    {
        if (!available())
            return false;
        bool ready = false;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            if (id < 2u || id >= g_pictures.size() || g_pictures[id].state == State::Failed)
                return false;
            g_pictures[id].shown = Clock::now();
            queue(id);
            ready = g_pictures[id].state == State::Ready;
            if (!ready && logOn())
                std::cerr << "[pictures] " << g_pictures[id].file.string() << " shown before it was read (nothing drawn)" << std::endl;
            if (!g_blankSent)
            {
                if (GSGpuRenderer *renderer = gpu())
                {
                    renderer->setHostImage(kBlank, 1u, 1u, std::vector<uint32_t>(1u, 0u));
                    g_blankSent = true;
                }
            }
        }
        arm(tbp, ready ? id : kBlank);
        return true;
    }

    void hide(uint32_t tbp)
    {
        if (!available())
            return;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            if (!g_blankSent)
            {
                gpu()->setHostImage(kBlank, 1u, 1u, std::vector<uint32_t>(1u, 0u));
                g_blankSent = true;
            }
        }
        arm(tbp, kBlank);
    }

    void install(PS2Runtime &runtime)
    {
        g_runtime = &runtime;
        std::thread(reader).detach();
    }
}

void installVendettaPictures(PS2Runtime &runtime) { vendetta_pictures::install(runtime); }
