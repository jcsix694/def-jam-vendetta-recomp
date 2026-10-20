// Def Jam Vendetta (USA, SLUS-20639) — a mod wrestler's own intro and win lines (see vendetta_voicelines.h).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
#include "vendetta_voicelines.h"
#include "vendetta_mods.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2x/iop/spu2_host_samples.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

void installVendettaVoiceLines(PS2Runtime &runtime);

namespace
{
    using Fn = PS2Runtime::RecompiledFunction;

    // FUN_001f3938(path, flags): a whole file into a heap block (the speech banks: AUDIO\SPEECH\<name>.bnk, flags 0 for
    // an intro line, 0x100 for a win line); its worker FUN_001f3880 allocates through the pointer at 0x275104
    // (FUN_001f4dc8, called (path, size, flags)), which we call the same way.
    constexpr uint32_t kLoadFile = 0x1F3938u, kAllocatorPointer = 0x275104u;
    // the stand-in's rate: 1,500 Hz plays at pitch 0x80 exactly (1,000 Hz would round to 0x55, 0.4 % slow)
    constexpr uint32_t kStandinRate = 1500u, kStandinPitch = kStandinRate * 4096u / 48000u;

    struct Line
    {
        std::filesystem::path file;
        std::string label;
        uint32_t id = 0; // its host sample's number
    };
    std::map<std::string, Line> g_lines; // by bank name (upper case)
    uint32_t g_nextId = 1;
    Fn g_load = nullptr;
    PS2Runtime *g_runtime = nullptr;

    bool logging()
    {
        static const bool on = [] { const char *v = std::getenv("VENDETTA_VOICELOG"); return v && *v && *v != '0'; }();
        return on;
    }

    // ---- a WAV: PCM 8 / 16 / 24 / 32-bit or 32-bit float (also inside WAVE_FORMAT_EXTENSIBLE), any rate, made mono ----
    bool readWav(const std::filesystem::path &file, std::vector<int16_t> &pcm, uint32_t &rate, std::string &error)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
        {
            error = "can't be opened";
            return false;
        }
        const std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), {});
        const auto u16 = [&](size_t at) { return static_cast<uint32_t>(b[at] | (b[at + 1] << 8)); };
        const auto u32 = [&](size_t at) { return u16(at) | (u16(at + 2) << 16); };
        if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0)
        {
            error = "isn't a WAV file";
            return false;
        }
        uint32_t format = 0, channels = 0, bits = 0;
        size_t data = 0, dataSize = 0;
        for (size_t at = 12; at + 8 <= b.size();)
        {
            const uint32_t size = u32(at + 4);
            if (std::memcmp(b.data() + at, "fmt ", 4) == 0 && size >= 16 && at + 8 + 16 <= b.size())
            {
                format = u16(at + 8);
                channels = u16(at + 10);
                rate = u32(at + 12);
                bits = u16(at + 22);
                if (format == 0xFFFEu && size >= 26 && at + 8 + 26 <= b.size())
                    format = u16(at + 8 + 24); // the sub-format's first two bytes
            }
            else if (std::memcmp(b.data() + at, "data", 4) == 0)
            {
                data = at + 8;
                dataSize = std::min<size_t>(size, b.size() - data);
            }
            at += 8 + size + (size & 1u);
        }
        const bool pcmFormat = format == 1u && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
        const bool floatFormat = format == 3u && bits == 32;
        if (!pcmFormat && !floatFormat)
        {
            error = "has a sound format that isn't read (use 8 / 16 / 24 / 32-bit PCM or 32-bit float)";
            return false;
        }
        if (channels == 0 || rate < 1000u || rate > 192000u || data == 0)
        {
            error = channels == 0 || rate < 1000u || rate > 192000u ? "has a wrong header" : "has no sound in it";
            return false;
        }
        const size_t bytes = bits / 8u, frames = dataSize / (bytes * channels);
        pcm.resize(frames);
        for (size_t f = 0; f < frames; ++f)
        {
            double sum = 0.0;
            for (uint32_t c = 0; c < channels; ++c)
            {
                const size_t at = data + (f * channels + c) * bytes;
                double v = 0.0;
                if (floatFormat)
                {
                    float x;
                    std::memcpy(&x, b.data() + at, 4);
                    v = x;
                }
                else if (bits == 8)
                    v = (static_cast<int>(b[at]) - 128) / 128.0;
                else if (bits == 16)
                    v = static_cast<int16_t>(u16(at)) / 32768.0;
                else if (bits == 24)
                    v = (static_cast<int32_t>((b[at] << 8) | (b[at + 1] << 16) | (b[at + 2] << 24)) >> 8) / 8388608.0;
                else
                    v = static_cast<int32_t>(u32(at)) / 2147483648.0;
                sum += v;
            }
            pcm[f] = static_cast<int16_t>(std::clamp(sum / channels * 32767.0, -32768.0, 32767.0));
        }
        if (pcm.empty())
        {
            error = "has no sound in it";
            return false;
        }
        return true;
    }

    // ---- the stand-in bank (an EA BNKl v5 with one PS-ADPCM sound): silence as long as `seconds` at 1,500 Hz, its
    // first block the host sample's marker, then the closing block {00 01} as every game line ends ----
    std::vector<uint8_t> standin(uint32_t id, double seconds)
    {
        const uint32_t blocks = std::max<uint32_t>(2u, static_cast<uint32_t>(seconds * kStandinRate / 28.0) + 2u);
        const uint32_t count = blocks * 28u;
        std::vector<uint8_t> bank(0x40u + 16u * blocks, 0);
        std::memcpy(bank.data(), "BNKl", 4);
        bank[4] = 5;
        const auto put32 = [&](size_t at, uint32_t v) { std::memcpy(bank.data() + at, &v, 4); };
        bank[6] = 1; // one sound
        put32(8, 0x40u);
        put32(0xC, 16u * blocks);
        put32(0x10, 0u);
        put32(0x14, 4u); // the sound's header at 0x18
        const uint8_t tags[] = {'P', 'T', 5, 0, 0xFD, 0x80, 1, 2,
                                0x85, 3, static_cast<uint8_t>(count >> 16), static_cast<uint8_t>(count >> 8), static_cast<uint8_t>(count),
                                0x84, 3, static_cast<uint8_t>(kStandinRate >> 16), static_cast<uint8_t>(kStandinRate >> 8),
                                static_cast<uint8_t>(kStandinRate),
                                0x88, 4, 0, 0, 0, 0x40, 0xFC, 0xFC, 0x8A, 4, 0, 0, 0, 0, 0xFF};
        std::memcpy(bank.data() + 0x18, tags, sizeof(tags));
        const std::array<uint8_t, 16> marker = ps2x::iop::hostSampleMarker(id);
        std::memcpy(bank.data() + 0x40, marker.data(), 16);
        bank[0x40 + 16u * (blocks - 1u) + 1u] = 1; // the end
        return bank;
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

    std::string guestText(uint8_t *rdram, uint32_t address)
    {
        std::string s;
        for (uint32_t i = 0; i < 160u; ++i)
        {
            const char c = static_cast<char>(rdram[(address + i) & PS2_RAM_MASK]);
            if (!c)
                break;
            s += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return s;
    }

    // FUN_001f3938: our banks made here, every other file the game's
    void loadFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const std::string path = guestText(rdram, getRegU32(ctx, 4));
        const size_t slash = path.rfind('\\');
        const std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
        const auto it = name.size() > 4 && name.ends_with(".BNK") ? g_lines.find(name.substr(0, name.size() - 4)) : g_lines.end();
        const Fn allocate = it != g_lines.end() ? runtime->lookupFunction(Ps2FastRead32(rdram, kAllocatorPointer)) : nullptr;
        if (!allocate)
        {
            g_load(rdram, ctx, runtime);
            return;
        }
        Line &line = it->second;
        // the recording, read again for each match (the mod's file as it is now); a file that can't be read gives a
        // short silent stand-in, so the match's sound setup goes on
        auto sample = std::make_shared<ps2x::iop::HostSample>();
        std::string error;
        double seconds = 0.1;
        if (readWav(line.file, sample->pcm, sample->rate, error))
        {
            sample->standinPitch = kStandinPitch;
            seconds = static_cast<double>(sample->pcm.size()) / sample->rate;
            ps2x::iop::setHostSample(line.id, std::move(sample));
        }
        else
        {
            ps2x::iop::setHostSample(line.id, nullptr);
            vendetta_mods::problem() << line.label << ": " << line.file.filename().string() << " " << error << " - silent"
                                     << std::endl;
        }
        auto bank = std::make_shared<std::vector<uint8_t>>(standin(line.id, seconds));
        if (logging())
            std::cerr << "[voicelines] " << line.label << ": " << it->first << " = " << line.file.string() << ", "
                      << seconds << " s" << std::endl;
        // allocate(path, size, flags), as FUN_001f3880 does; its block is the result FUN_001f3938's caller gets
        const uint32_t flags = getRegU32(ctx, 5);
        SET_GPR_U32(ctx, 5, static_cast<uint32_t>(bank->size()));
        SET_GPR_U32(ctx, 6, flags);
        callThen(rdram, ctx, runtime, allocate, [bank](uint8_t *rdram, R5900Context *ctx) {
            const uint32_t block = getRegU32(ctx, 2);
            if (block)
                std::memcpy(rdram + (block & PS2_RAM_MASK), bank->data(), bank->size());
        });
    }
}

namespace vendetta_voicelines
{
    std::string add(uint32_t row, bool intro, const std::filesystem::path &file, const std::string &label)
    {
        std::vector<int16_t> pcm;
        uint32_t rate = 0;
        std::string error;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec))
        {
            vendetta_mods::problem() << label << ": " << (intro ? "\"intro line\"" : "\"win line\"") << ": "
                                     << file.filename().string() << " isn't there - none" << std::endl;
            return std::string();
        }
        if (!readWav(file, pcm, rate, error))
        {
            vendetta_mods::problem() << label << ": " << (intro ? "\"intro line\"" : "\"win line\"") << ": "
                                     << file.filename().string() << " " << error << " - none" << std::endl;
            return std::string();
        }
        char name[16];
        std::snprintf(name, sizeof(name), "VDM%04u%c", row % 10000u, intro ? 'I' : 'E');
        Line &line = g_lines[name];
        line.file = file;
        line.label = label;
        if (line.id == 0)
            line.id = g_nextId++;
        return name;
    }
}

void installVendettaVoiceLines(PS2Runtime &runtime)
{
    g_runtime = &runtime;
    g_load = runtime.lookupFunction(kLoadFile);
    if (!g_load)
    {
        std::cerr << "[voicelines] the game's file loader was not found" << std::endl;
        return;
    }
    runtime.replaceFunction(kLoadFile, loadFile);
}
