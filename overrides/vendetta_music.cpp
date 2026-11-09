// Def Jam Vendetta (USA, SLUS-20639) - music themes: a song of the game's played from an audio file (anything FFmpeg reads).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// The game has two sets of songs, numbered alike (0 to 17): the menus' one file (DJFE, an event per song) and a file per song for
// the matches. Ours is decoded whole to 48 kHz stereo when its song starts and mixed into the game's output
// (PS2AudioBackend::setPcmMixer). The game's own song keeps running, silent, so nothing around it changes:
//
// - FUN_001cc158(-1, volume) is the one way to the music's loudness (only caller FUN_001c23e8): the volume asked for (0 to 100, the
//   player's setting and every fade in it) is written down and 0 passed on while the current song is replaced.
// - FUN_001cb5e0 (send an event), by its return address: from FUN_001c2250 a menu song starts (0x29ABCC), from FUN_001c21c8 a
//   match song (0x29ABFC), from FUN_001c5ca8 the match ends. FUN_001c2078 (the song freed) stops ours.
// - FUN_001c20c8 ("is the track playing") answers for ours in the menus, so the next song comes when ours ends.
// - The menus play the file once. A match: before "loop from" once, the loop until the match ends, then the part after "loop to".
//   No marks = loop from the start to the end.
//
// Which file plays: the project's music themes, mods whose mod.json has
//   "music": { "format": 1, "list": "music/music.json" }
// and whose list says { "songs": { "<song>": { "file": "<file>", "artist": "", "song": "", "album": "", "loop from": s, "loop to": s } },
// "3-on-1": "random" | "game", "song box": true | false }. The later theme wins song by song, and its "3-on-1" and "song box" are used.
// A song's three names all empty: no song box for it. "3-on-1": "random": the story's 3-on-1 fight asks FUN_001c1e68 for 99, a random
// song of the 18 is loaded instead. Every problem is a line in mods.log and the game's song plays: a newer format (the whole theme
// skipped), a name that isn't a song, a file missing, outside the mod's folder or that FFmpeg can't read (each file is opened at start).
// With no theme replacing anything nothing is hooked.
//
// Tests: VENDETTA_MUSICTRY=<k or *>=<file>[@from,to][|artist|song|album][;...] names files without a mod (they win over the themes);
// VENDETTA_MUSICLOG=1; VENDETTA_MUSIC=0 = off; VENDETTA_MUSICTRY_HIDEBOX=1 (no song box); VENDETTA_MUSICTRY_RANDOM=1 (song 99
// becomes a random one of the 18); VENDETTA_MUSICTRY_STEP=<n> (a match not in the story treated as story step n);
// VENDETTA_MUSICTRY_MENULOOP=1 (the marks used in the menus too); VENDETTA_MUSICTRY_GAIN=<x> (a scale on the volume).
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_audio.h"
#include "vendetta_mods.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

void vendettaVideoMix(int16_t *out, size_t count, size_t queued); // vendetta_videos.cpp

namespace vendetta_music
{
    using Fn = decltype(std::declval<PS2Runtime &>().lookupFunction(0u));

    constexpr uint32_t kSetVolume = 0x1CC158u;  // FUN_001cc158(-1, volume)
    constexpr uint32_t kSendEvent = 0x1CB5E0u;  // FUN_001cb5e0(track, event, 0)
    constexpr uint32_t kFreeSong = 0x1C2078u;   // FUN_001c2078(): the song stopped and freed
    constexpr uint32_t kIsPlaying = 0x1C20C8u;  // FUN_001c20c8(): 1 while the track plays
    constexpr uint32_t kLoadSong = 0x1C1E68u;   // FUN_001c1e68(k): -1 the menus', 99 the 3-on-1's
    constexpr uint32_t kMatchSound = 0x1C0908u; // FUN_001c0908(story step or 0, ...)
    constexpr uint32_t kAskSongBox = 0x1B1218u; // FUN_001b1218: the song box asked for
    constexpr uint32_t kMenuSong = 0x29ABCCu, kMatchSong = 0x29ABFCu, kWhere = 0x29AA78u, kRecords = 0x2613A0u;
    constexpr uint32_t kRate = 48000u;

    struct Song
    {
        std::filesystem::path file;
        double from = -1.0, to = -1.0;
        std::string artist, song, album, theme;
        bool named = false, noBox = false;
    };

    struct Pcm
    {
        std::filesystem::path file;
        std::vector<int16_t> samples;
        size_t frames() const { return samples.size() / 2u; }
    };

    constexpr int kFormat = 1;
    const char *const kSongNames[18] = {"smash-sumthin", "the-streets", "nothin", "bring-the-pain", "oh-my-goodness", "fight-the-power",
                                        "intro", "throw-ya-gunz", "party-up", "uh-huh", "focus", "in-cold-blood",
                                        "do-sumptin", "x-gon-give-it-to-you", "stomp", "buck-50", "yeah-yeah-you-know-it", "stick-em"};

    std::map<int, Song> g_songs;
    Song g_every;
    bool g_hasEvery = false, g_log = false, g_menuLoop = false, g_random = false, g_hideBox = false, g_anyNoBox = false;
    int g_step = 0;
    float g_scale = 1.0f;
    std::atomic<uint32_t> g_broken{0}; // bit k: song k's file wouldn't decode when it came to play
    Fn g_setVolume, g_sendEvent, g_freeSong, g_isPlaying, g_loadSong, g_matchSound, g_askSongBox;

    // the player: everything under g_lock, the mixer holds it only for a chunk
    std::mutex g_lock;
    std::shared_ptr<Pcm> g_pcm, g_cache;
    size_t g_pos = 0, g_from = 0, g_to = 0, g_fadeOut = 0;
    bool g_looping = false, g_ending = false;
    size_t g_xfPos = 0, g_xfLeft = 0;
    float g_gain = 0.0f;
    uint64_t g_generation = 0;
    std::atomic<int> g_want{0};
    std::atomic<bool> g_on{false}, g_done{false};
    std::atomic<int> g_playing{-1};
    constexpr size_t kCross = kRate / 100u;      // 10 ms over a jump to the end part
    constexpr size_t kFadeFrames = kRate / 2u;   // no end part: out in half a second

    std::string utf8(const std::filesystem::path &p)
    {
        const auto u = p.u8string();
        return std::string(reinterpret_cast<const char *>(u.data()), u.size());
    }

    const Song *find(int k)
    {
        if (k < 0 || k > 17 || (g_broken.load() >> k & 1u))
            return nullptr;
        if (const auto it = g_songs.find(k); it != g_songs.end())
            return &it->second;
        return g_hasEvery ? &g_every : nullptr;
    }

    int read32(const uint8_t *rdram, uint32_t address) { return *reinterpret_cast<const int32_t *>(rdram + address); }

    int currentSong(const uint8_t *rdram)
    {
        const int match = read32(rdram, kMatchSong);
        return match == -1 ? read32(rdram, kMenuSong) : match;
    }

    std::shared_ptr<Pcm> decode(const std::filesystem::path &file)
    {
        AVFormatContext *format = nullptr;
        AVCodecContext *codec = nullptr;
        SwrContext *swr = nullptr;
        AVPacket *packet = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        auto pcm = std::make_shared<Pcm>();
        pcm->file = file;
        bool ok = false;
        std::vector<int16_t> &out = pcm->samples;
        const auto add = [&](const uint8_t *const *in, int count) {
            const int room = swr_get_out_samples(swr, count);
            if (room <= 0)
                return;
            const size_t had = out.size();
            out.resize(had + size_t(room) * 2u);
            uint8_t *dst[1] = {reinterpret_cast<uint8_t *>(out.data() + had)};
            const int got = swr_convert(swr, dst, room, in, count);
            out.resize(had + size_t(std::max(got, 0)) * 2u);
        };
        do
        {
            if (avformat_open_input(&format, utf8(file).c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(format, nullptr) < 0)
                break;
            const int stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            if (stream < 0)
                break;
            const AVCodecParameters *par = format->streams[stream]->codecpar;
            const AVCodec *decoder = avcodec_find_decoder(par->codec_id);
            if (!decoder || !(codec = avcodec_alloc_context3(decoder)) || avcodec_parameters_to_context(codec, par) < 0 ||
                avcodec_open2(codec, decoder, nullptr) < 0)
                break;
            AVChannelLayout stereo{};
            av_channel_layout_default(&stereo, 2);
            const bool made = swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, int(kRate), &codec->ch_layout, codec->sample_fmt,
                                                  codec->sample_rate, 0, nullptr) >= 0 && swr_init(swr) >= 0;
            av_channel_layout_uninit(&stereo);
            if (!made)
                break;
            out.reserve(size_t(std::max<int64_t>(format->duration, 0)) * 2u * kRate / AV_TIME_BASE + 1024u);
            const auto drain = [&] {
                while (avcodec_receive_frame(codec, frame) >= 0)
                {
                    add(const_cast<const uint8_t *const *>(frame->extended_data), frame->nb_samples);
                    av_frame_unref(frame);
                }
            };
            while (av_read_frame(format, packet) >= 0)
            {
                if (packet->stream_index == stream && avcodec_send_packet(codec, packet) >= 0)
                    drain();
                av_packet_unref(packet);
            }
            avcodec_send_packet(codec, nullptr);
            drain();
            add(nullptr, 0); // what the resampler still holds
            ok = out.size() >= 2u;
        } while (false);
        swr_free(&swr);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        av_frame_free(&frame);
        av_packet_free(&packet);
        return ok ? pcm : nullptr;
    }

    void writeField(uint8_t *rdram, uint32_t address, size_t size, const std::string &text)
    {
        std::memset(rdram + address, 0, size);
        std::memcpy(rdram + address, text.data(), std::min(text.size(), size - 1u));
    }

    void stop(const char *why)
    {
        std::lock_guard<std::mutex> lk(g_lock);
        ++g_generation;
        const bool was = g_on.exchange(false);
        g_pcm.reset();
        g_playing = -1;
        if (was && g_log)
            std::cerr << "[music] stopped: " << why << std::endl;
    }

    void start(uint8_t *rdram, int k, bool match)
    {
        const Song *song = find(k);
        if (!song)
        {
            stop("a song not replaced");
            return;
        }
        if (song->named)
        {
            const uint32_t record = kRecords + uint32_t(k) * 0x84u;
            writeField(rdram, record + 0x04u, 40u, song->artist);
            writeField(rdram, record + 0x2Cu, 30u, song->song);
            writeField(rdram, record + 0x4Au, 58u, song->album);
        }
        uint64_t generation;
        std::shared_ptr<Pcm> cached;
        {
            std::lock_guard<std::mutex> lk(g_lock);
            generation = ++g_generation;
            g_pcm.reset();
            g_pos = 0, g_ending = false, g_fadeOut = 0, g_xfLeft = 0;
            g_on = true, g_done = false, g_playing = k;
            if (g_cache && g_cache->file == song->file)
                cached = g_cache;
        }
        const Song copy = *song;
        const bool loops = match || g_menuLoop;
        std::thread([copy, k, match, loops, generation, cached] {
            const auto began = std::chrono::steady_clock::now();
            std::shared_ptr<Pcm> pcm = cached ? cached : decode(copy.file);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
            std::lock_guard<std::mutex> lk(g_lock);
            if (generation != g_generation)
                return;
            if (!pcm)
            {
                vendetta_mods::problem() << "[music] " << copy.theme << ": " << utf8(copy.file) << " can't be read: the game's song plays" << std::endl;
                g_broken.fetch_or(1u << k);
                g_on = false, g_done = true, g_playing = -1;
                return;
            }
            g_cache = pcm;
            const size_t total = pcm->frames();
            g_from = copy.from > 0.0 ? std::min(size_t(copy.from * kRate + 0.5), total - 1u) : 0u;
            g_to = copy.to > 0.0 ? std::min(size_t(copy.to * kRate + 0.5), total) : total;
            if (g_to <= g_from)
                g_from = 0, g_to = total;
            g_looping = loops;
            g_pcm = pcm;
            if (g_log)
                std::cerr << "[music] song " << k << (match ? " (match): " : " (menus): ") << utf8(copy.file) << ", " << double(total) / kRate
                          << " s, decoded in " << ms << " ms, loop " << double(g_from) / kRate << " to " << double(g_to) / kRate << std::endl;
        }).detach();
    }

    void ending()
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (!g_on)
            return;
        if (g_log)
            std::cerr << "[music] the match ended at " << double(g_pos) / kRate << " s" << std::endl;
        if (!g_pcm)
            return;
        g_looping = false;
        g_ending = true;
        if (g_to < g_pcm->frames())
        {
            g_xfPos = g_pos, g_xfLeft = kCross;
            g_pos = g_to;
        }
        else
            g_fadeOut = kFadeFrames;
    }

    void mix(int16_t *out, size_t count, size_t queued)
    {
        vendettaVideoMix(out, count, queued);
        std::unique_lock<std::mutex> lk(g_lock, std::try_to_lock);
        if (!lk.owns_lock() || !g_pcm)
            return;
        const int16_t *in = g_pcm->samples.data();
        const size_t total = g_pcm->frames();
        const float target = std::clamp(float(g_want.load(std::memory_order_relaxed)) / 100.0f * g_scale, 0.0f, 1.0f);
        const float step = (target - g_gain) / float(std::max<size_t>(count, 1u));
        for (size_t i = 0; i < count; ++i)
        {
            if (g_looping && g_pos >= g_to)
            {
                if (g_log)
                    std::cerr << "[music] loop back at " << double(g_pos) / kRate << " s" << std::endl;
                g_pos = g_from;
            }
            if (g_pos >= total)
            {
                g_pcm.reset();
                g_done = true;
                if (g_log)
                    std::cerr << "[music] the file ended" << std::endl;
                return;
            }
            float left = in[g_pos * 2u], right = in[g_pos * 2u + 1u];
            if (g_xfLeft && g_xfPos < total)
            {
                const float t = float(g_xfLeft) / float(kCross);
                left = left * (1.0f - t) + in[g_xfPos * 2u] * t;
                right = right * (1.0f - t) + in[g_xfPos * 2u + 1u] * t;
                ++g_xfPos, --g_xfLeft;
            }
            float fade = 1.0f;
            if (g_ending && g_fadeOut)
            {
                fade = float(g_fadeOut) / float(kFadeFrames);
                if (--g_fadeOut == 0u)
                {
                    g_pcm.reset();
                    g_done = true;
                    return;
                }
            }
            g_gain += step;
            ++g_pos;
            out[i * 2u] = int16_t(std::clamp(int(out[i * 2u]) + int(left * g_gain * fade), -32768, 32767));
            out[i * 2u + 1u] = int16_t(std::clamp(int(out[i * 2u + 1u]) + int(right * g_gain * fade), -32768, 32767));
        }
    }

    void setVolume(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int volume = int(int8_t(getRegU32(ctx, 5)));
        if (g_log && volume != g_want.load())
            std::cerr << "[music] volume " << volume << std::endl;
        g_want = volume;
        if (find(currentSong(rdram)))
            SET_GPR_U32(ctx, 5, 0u);
        g_setVolume(rdram, ctx, runtime);
    }

    void sendEvent(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        if (ra >= 0x1C2250u && ra < 0x1C2378u)
            start(rdram, read32(rdram, kMenuSong), false);
        else if (ra >= 0x1C21C8u && ra < 0x1C2250u)
            start(rdram, read32(rdram, kMatchSong), true);
        else if (ra >= 0x1C5CA8u && ra < 0x1C5D18u)
            ending();
        g_sendEvent(rdram, ctx, runtime);
    }

    void freeSong(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        stop("the song freed");
        g_freeSong(rdram, ctx, runtime);
    }

    void isPlaying(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (read32(rdram, kWhere) == -1 && g_on.load() && g_playing.load() == currentSong(rdram))
        {
            SET_GPR_U32(ctx, 2, g_done.load() ? 0u : 1u);
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
        g_isPlaying(rdram, ctx, runtime);
    }

    void loadSong(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_random && int(getRegU32(ctx, 4)) == 99)
        {
            static std::mt19937 rng{std::random_device{}()};
            const uint32_t k = rng() % 18u;
            if (g_log)
                std::cerr << "[music] the 3-on-1 fight plays song " << k << std::endl;
            SET_GPR_U32(ctx, 4, k);
        }
        g_loadSong(rdram, ctx, runtime);
    }

    void matchSound(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (int(getRegU32(ctx, 4)) <= 0)
            SET_GPR_U32(ctx, 4, uint32_t(g_step));
        g_matchSound(rdram, ctx, runtime);
    }

    void askSongBox(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const Song *song = find(currentSong(rdram));
        if (g_hideBox || (song && song->noBox))
        {
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
        g_askSongBox(rdram, ctx, runtime);
    }

    // FFmpeg can open it and finds sound it can decode
    bool canRead(const std::filesystem::path &file)
    {
        AVFormatContext *format = nullptr;
        bool ok = false;
        if (avformat_open_input(&format, utf8(file).c_str(), nullptr, nullptr) >= 0)
        {
            if (avformat_find_stream_info(format, nullptr) >= 0)
            {
                const int stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
                ok = stream >= 0 && avcodec_find_decoder(format->streams[stream]->codecpar->codec_id);
            }
            avformat_close_input(&format);
        }
        return ok;
    }

    bool readText(const std::filesystem::path &file, std::string &text)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return false;
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }

    int songNumber(const std::string &name)
    {
        for (int k = 0; k < 18; ++k)
            if (name == kSongNames[k])
                return k;
        return -1;
    }

    std::string nameField(const vendetta_mods::Mod &mod, const vendetta_mods::Json &entry, const char *key, size_t most, const std::string &song)
    {
        const vendetta_mods::Json *field = entry.find(key);
        if (!field || !field->isString())
            return {};
        if (field->text.size() <= most)
            return field->text;
        vendetta_mods::problem() << "[music] " << mod.name << ": " << song << "'s " << key << " is over " << most << " letters: cut" << std::endl;
        return field->text.substr(0, most);
    }

    // the project's music themes, in load order (the later one wins song by song)
    void readThemes(int &fight)
    {
        using vendetta_mods::Json;
        for (const vendetta_mods::Mod &mod : vendetta_mods::loadedMods())
        {
            const Json *music = mod.manifest.find("music");
            if (!music || !music->isObject())
                continue;
            const Json *format = music->find("format");
            if (format && format->number > kFormat)
            {
                vendetta_mods::problem() << "[music] " << mod.name << "'s music needs a newer game (format " << int(format->number) << "): skipped" << std::endl;
                continue;
            }
            const Json *list = music->find("list");
            if (!list || !list->isString() || list->text.empty())
            {
                vendetta_mods::problem() << "[music] " << mod.name << "'s \"music\" names no list: skipped" << std::endl;
                continue;
            }
            const std::filesystem::path listFile = vendetta_mods::modFile(mod, list->text, "music list");
            if (listFile.empty())
                continue;
            std::string text, error;
            Json json;
            if (!readText(listFile, text) || !vendetta_mods::parseJson(text, json, error))
            {
                vendetta_mods::problem() << "[music] " << mod.name << ": " << list->text << " can't be read: " << error << std::endl;
                continue;
            }
            const Json *entries = json.find("songs");
            if (!entries || !entries->isObject())
            {
                vendetta_mods::problem() << "[music] " << mod.name << ": " << list->text << " has no \"songs\"" << std::endl;
                continue;
            }
            const std::string folder = std::filesystem::path(list->text).parent_path().generic_string();
            int used = 0;
            for (const auto &[name, entry] : entries->members)
            {
                const int k = songNumber(name);
                if (k < 0)
                {
                    vendetta_mods::problem() << "[music] " << mod.name << ": \"" << name << "\" isn't one of the game's songs: ignored" << std::endl;
                    continue;
                }
                const Json *fileName = entry.isObject() ? entry.find("file") : nullptr;
                if (!fileName || !fileName->isString() || fileName->text.empty())
                {
                    vendetta_mods::problem() << "[music] " << mod.name << ": " << name << " names no file: the game's song plays" << std::endl;
                    continue;
                }
                const std::filesystem::path file = vendetta_mods::modFile(mod, folder.empty() ? fileName->text : folder + "/" + fileName->text, ("the music for " + name).c_str());
                if (file.empty())
                    continue; // (its line written: not there, or outside the mod's folder)
                if (!canRead(file))
                {
                    vendetta_mods::problem() << "[music] " << mod.name << ": " << fileName->text << " can't be read as music: left out" << std::endl;
                    continue;
                }
                Song song;
                song.theme = mod.name;
                song.file = file;
                song.artist = nameField(mod, entry, "artist", 39u, name);
                song.song = nameField(mod, entry, "song", 29u, name);
                song.album = nameField(mod, entry, "album", 57u, name);
                song.named = !song.artist.empty() || !song.song.empty() || !song.album.empty();
                song.noBox = !song.named;
                if (const Json *from = entry.find("loop from"); from && from->type == Json::Type::Number)
                    song.from = from->number;
                if (const Json *to = entry.find("loop to"); to && to->type == Json::Type::Number)
                    song.to = to->number;
                if (song.to > 0.0 && song.to <= std::max(song.from, 0.0))
                {
                    vendetta_mods::problem() << "[music] " << mod.name << ": " << name << "'s loop ends before it starts: played without marks" << std::endl;
                    song.from = song.to = -1.0;
                }
                if (const auto had = g_songs.find(k); had != g_songs.end())
                    vendetta_mods::problem() << "[music] " << name << ": " << mod.name << "'s file used, not " << had->second.theme << "'s" << std::endl;
                g_songs[k] = std::move(song);
                ++used;
            }
            const Json *three = json.find("3-on-1");
            fight = three && three->isString() && three->text == "game" ? 2 : 1;
            const Json *box = json.find("song box");
            g_hideBox = box && box->type == Json::Type::Bool && !box->boolean;
            std::cerr << "[music] " << mod.name << ": " << used << (used == 1 ? " song" : " songs") << " replaced, the 3-on-1 fight " << (fight == 1 ? "random" : "its own song")
                      << ", song box " << (g_hideBox ? "hidden" : "shown") << std::endl;
        }
    }

    void parseTry(const std::string &all)
    {
        size_t at = 0;
        while (at <= all.size())
        {
            size_t end = all.find(';', at);
            if (end == std::string::npos)
                end = all.size();
            std::string item = all.substr(at, end - at);
            at = end + 1;
            const size_t eq = item.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string which = item.substr(0, eq);
            std::string rest = item.substr(eq + 1), names;
            if (const size_t bar = rest.find('|'); bar != std::string::npos)
                names = rest.substr(bar + 1), rest.resize(bar);
            Song song;
            if (const size_t mark = rest.rfind('@'); mark != std::string::npos)
            {
                song.from = std::atof(rest.c_str() + mark + 1);
                if (const size_t comma = rest.find(',', mark); comma != std::string::npos)
                    song.to = std::atof(rest.c_str() + comma + 1);
                rest.resize(mark);
            }
            song.file = std::filesystem::u8path(rest);
            if (!names.empty())
            {
                song.named = true;
                const size_t a = names.find('|'), b = a == std::string::npos ? a : names.find('|', a + 1);
                song.artist = names.substr(0, a);
                if (a != std::string::npos)
                    song.song = names.substr(a + 1, b == std::string::npos ? b : b - a - 1);
                if (b != std::string::npos)
                    song.album = names.substr(b + 1);
            }
            if (which == "*")
                g_every = song, g_hasEvery = true;
            else
                g_songs[std::atoi(which.c_str())] = song;
        }
    }

    bool hook(PS2Runtime &runtime, uint32_t address, Fn &original, void (*replacement)(uint8_t *, R5900Context *, PS2Runtime *))
    {
        original = runtime.lookupFunction(address);
        return original && runtime.replaceFunction(address, replacement);
    }

    void install(PS2Runtime &runtime)
    {
        const auto on = [](const char *name) { const char *v = std::getenv(name); return v && *v && *v != '0'; };
        if (const char *off = std::getenv("VENDETTA_MUSIC"); off && off[0] == '0')
            return;
        g_log = on("VENDETTA_MUSICLOG");
        int fight = 0; // 0 no theme, 1 random, 2 its own song
        readThemes(fight);
        g_random = fight == 1;
        if (const char *list = std::getenv("VENDETTA_MUSICTRY"); list && *list)
            parseTry(list);
        g_menuLoop = on("VENDETTA_MUSICTRY_MENULOOP");
        g_random = g_random || on("VENDETTA_MUSICTRY_RANDOM");
        g_hideBox = g_hideBox || on("VENDETTA_MUSICTRY_HIDEBOX");
        if (const char *step = std::getenv("VENDETTA_MUSICTRY_STEP"))
            g_step = std::atoi(step);
        if (const char *scale = std::getenv("VENDETTA_MUSICTRY_GAIN"))
            g_scale = float(std::atof(scale));
        for (const auto &[k, song] : g_songs)
            g_anyNoBox = g_anyNoBox || song.noBox;
        if (g_songs.empty() && !g_hasEvery && !g_random && !g_step && !g_hideBox)
            return;
        const bool ok = hook(runtime, kSetVolume, g_setVolume, setVolume) && hook(runtime, kSendEvent, g_sendEvent, sendEvent) &&
                        hook(runtime, kFreeSong, g_freeSong, freeSong) && hook(runtime, kIsPlaying, g_isPlaying, isPlaying) &&
                        hook(runtime, kLoadSong, g_loadSong, loadSong);
        if (!ok)
        {
            std::cerr << "[music] a routine wasn't found: off" << std::endl;
            return;
        }
        if (g_step)
            hook(runtime, kMatchSound, g_matchSound, matchSound);
        if (g_hideBox || g_anyNoBox)
            hook(runtime, kAskSongBox, g_askSongBox, askSongBox);
        PS2AudioBackend::setPcmMixer(mix);
        std::cerr << "[music] " << (g_hasEvery ? 18u : g_songs.size()) << " songs replaced" << std::endl;
    }
}

void installVendettaMusic(PS2Runtime &runtime) { vendetta_music::install(runtime); }
