// Def Jam Vendetta (USA, SLUS-20639) - video themes: a movie of the game's played from a file of the mod's own
// (MP4 and anything else FFmpeg reads, any size and shape), in place of the game's .MPC.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// Every movie goes through FUN_001bb3b8(name, skip mask): it plays the movie to its end or until a button of the mask
// is newly pressed on the first controller, and returns 1; no caller reads a length, each waits for it to return. The
// hook plays the mod's file instead when there is one for the name, and does per picture what the game's run loop
// FUN_001bb7b0 does, so the game around it carries on as during its own movie: the pads read (FUN_001afc50(1)), the
// periodic tasks (FUN_001f6cf8(0)), the skip (FUN_001aff70(0, 1) & mask), then the wait for the next VBlank. The
// game's thread waits there (EeScheduler::waitVSync) and the others run; the game's calls are nested guest calls
// (GuestInvocationKind::HleCall), each picture's next step their completion, so no C++ frame of ours is ever left
// half way by a scheduler checkpoint.
//
// The picture: decoded by FFmpeg on a thread of its own, a few pictures ahead; the one due made RGBA at the file's size
// (at most 2048 wide) and drawn by the window over the game's picture (gsSetPresentOverlay: the window's own image,
// filled again each picture; the first way tried, a host image made for each picture inside paraLLEl-GS, couldn't keep
// up with 1080p at 60 a second). While it is shown the window draws it alone, black around it, and shows each new
// one even though the game draws nothing meanwhile (the display is still set up as the game's movie sets it,
// FUN_001f7690 and FUN_001b5a50, for what comes after). A video wider than 4:3 with widescreen on: each picture marked
// wide (the window shows it at the target shape, as a match) and the video fitted inside that; otherwise fitted
// inside the 4:3 picture; bars black.
// The sound: resampled to 48 kHz stereo and mixed into the game's own output (vendetta_mixer.h; the
// caller has paused the game's music, as around its own movies); the picture follows the sound (the frames heard),
// the clock drawn to it slowly. The video ends with its last picture (sound past it cut, as the game's player does).
// After the last picture (or the skip) the hook returns 1 as the game's routine does; the caller puts the menus'
// display and GS registers back (FUN_001a13e0, FUN_001a14c8), as after its own movies.
//
// Which file plays: the project's video themes, mods whose mod.json has
//   "videos": { "format": 1, "list": "videos/videos.json" }
// and whose list says { "videos": { "<movie>": { "video": "<file>", "shape": "4:3" | "16:9" | "other" } } }: the movie
// by the game's own name (lower case, '/', no ".mpc": eabig, intro, tutorial/t01 ... t10, cinema/credits,
// cinema/heroH/scHNN and gfHNN for heroes 0-3 and scenes 00-09; 93 in all), the file relative to the list's folder.
// The later theme wins movie by movie. Every problem is a line in mods.log: a newer format (the whole theme skipped), a
// name that isn't a movie (ignored), a file missing or outside the mod's folder (that entry left out), a file FFmpeg
// can't open (found when it would play: the theme before it plays that movie, or the game), the window not shown
// through Vulkan (the game's movies). The game goes by the file's own shape; a
// list's "shape" that disagrees is a line (4:3 and 16:9 = within 0.05 of 1.33 and 1.78, anything else "other").
// With no theme replacing anything the routine isn't hooked at all.
// Tests: VENDETTA_VIDEOTRY=<movie>=<file>[;<movie>=<file>...] names files without a mod (they win over the themes);
// VENDETTA_VIDEOS=0 = off; VENDETTA_VIDEOLOG=1 = each replaced movie at start, and a line every 60 pictures while one
// plays. A line per movie played on stderr. vendettaVideoPlaying(): one of ours is on screen (the freeze watch).
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/gs/gs_gpu.h"
#include "runtime/ps2_audio.h"
#include "runtime/ps2_widescreen.h"
#include "vendetta_mixer.h"
#include "vendetta_mods.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace vendetta_videos
{
    constexpr uint32_t kPlayMovie = 0x1BB3B8u;  // FUN_001bb3b8(name, skip mask): a movie, to its end; returns 1
    constexpr uint32_t kResetGraph = 0x1F7690u; // FUN_001f7690(0, 1, 2, 0): the display as for a movie (NTSC, field)
    constexpr uint32_t kShowBuffer = 0x1B5A50u; // FUN_001b5a50(fbp, 640, 480, 0): the buffer the display shows
    constexpr uint32_t kReadPads = 0x1AFC50u;   // FUN_001afc50(1): the pads read, once a picture
    constexpr uint32_t kPeriodic = 0x1F6CF8u;   // FUN_001f6cf8(0): the periodic tasks, once a picture
    constexpr uint32_t kPressed = 0x1AFF70u;    // FUN_001aff70(0, 1): the first controller's newly pressed buttons
    constexpr uint32_t kDrawInto = 0x1B5D78u;   // FUN_001b5d78(fbp, w, h, psm, 0, 0, 0, 0): the buffer drawn into
    constexpr uint32_t kFillBuffer = 0x1B6190u; // FUN_001b6190(w, h, r, g, b, a): that buffer filled with one colour

    constexpr int kAheadFrames = 6;             // pictures decoded ahead
    constexpr uint32_t kMaxWidth = 2048u;
    constexpr uint32_t kRate = 48000u;          // the game's output: 48 kHz stereo

    using Clock = std::chrono::steady_clock;

    struct FrameFree
    {
        void operator()(AVFrame *f) const { av_frame_free(&f); }
    };

    // a picture as decoded (its own pixel format: converted to RGBA only if it is shown)
    struct Frame
    {
        double pts = 0.0;
        std::unique_ptr<AVFrame, FrameFree> frame;
    };

    // One video playing: opened on the game's thread, decoded on its own.
    struct Player
    {
        std::string name;
        std::filesystem::path file;
        AVFormatContext *format = nullptr;
        AVCodecContext *video = nullptr, *audio = nullptr;
        int videoStream = -1, audioStream = -1;
        SwsContext *scale = nullptr; // the game's thread: a shown picture to RGBA
        SwrContext *resample = nullptr;
        uint32_t outW = 0, outH = 0;
        double aspect = 4.0 / 3.0; // the picture's shape as shown (its pixels' own shape counted)
        double fps = 30.0, length = 0.0;

        std::thread thread;
        std::atomic<bool> stop{false};
        std::mutex lock;
        std::condition_variable wake;
        std::deque<Frame> frames;
        std::vector<int16_t> sound; // decoded, not mixed yet (stereo pairs)
        size_t soundRead = 0;
        bool ended = false;         // the decoder has read the whole file

        // the sound mixed into the game's output (the mixer's thread), and what of it has been heard
        std::atomic<uint64_t> mixed{0};
        std::atomic<int64_t> heard{0};      // frames heard by the last mix (mixed less those still queued)
        std::atomic<int64_t> lastMixUs{0};
        Clock::time_point start;

        ~Player()
        {
            sws_freeContext(scale);
            swr_free(&resample);
            avcodec_free_context(&video);
            avcodec_free_context(&audio);
            avformat_close_input(&format);
        }
    };

    struct Session
    {
        bool active = false;
        std::unique_ptr<Player> player;
        uint32_t ra = 0, sp = 0, mask = 0;
        uint8_t *rdram = nullptr;
        PS2Runtime *runtime = nullptr;
        bool wide = false;
        bool skipped = false;
        uint64_t pictures = 0, shown = 0, dropped = 0;
        double lastPts = -1.0;
        double offset = 0.0; // the clock = the time since it began + offset, drawn slowly to the sound heard
        bool offsetSet = false;
        bool soundless = false; // no sound came: the time alone
        Clock::time_point began;
    };

    constexpr int kFormat = 1; // the newest "videos" format this game reads

    // a movie replaced: its file, the theme it comes from, the shape its list says ("" = none)
    struct Replacement
    {
        std::string theme, shape;
        std::filesystem::path file;
        bool checked = false; // the shape compared (and the window's line written) once
        bool broken = false;  // FFmpeg couldn't open it (said once; the one before it plays from then on)
    };

    PS2Runtime::RecompiledFunction g_original = nullptr;
    // movie -> every theme's file for it, in load order: the last one plays; one that can't be opened when the movie
    // comes gives way to the one before it, then to the game's movie
    std::map<std::string, std::vector<Replacement>> g_videos;
    std::atomic<int64_t> g_lastPictureMs{0};     // when ours last made a picture (the freeze watch)
    bool g_log = false;
    Session g_session;                         // the game's thread only
    std::atomic<Player *> g_mixing{nullptr};   // the player whose sound the mixer adds

    int64_t nowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count(); }

    // the movie as the game names it: "INTRO.MPC" / "cdrom0:\CINEMA\HERO0\SC000.MPC;1" -> "intro", "cinema/hero0/sc000"
    std::string movieName(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(c == '\\' ? '/' : std::tolower(c)); });
        if (const size_t colon = s.find(':'); colon != std::string::npos)
            s = s.substr(colon + 1);
        if (const size_t semi = s.find(';'); semi != std::string::npos)
            s.resize(semi);
        while (!s.empty() && s.front() == '/')
            s.erase(s.begin());
        if (s.size() > 4 && s.compare(s.size() - 4, 4, ".mpc") == 0)
            s.resize(s.size() - 4);
        return s;
    }

    // the game's 93 movies by name
    bool isMovie(const std::string &name)
    {
        static const std::set<std::string> names = []
        {
            std::set<std::string> n{"eabig", "intro", "cinema/credits"};
            char text[40];
            for (int t = 1; t <= 10; ++t)
            {
                std::snprintf(text, sizeof(text), "tutorial/t%02d", t);
                n.insert(text);
            }
            for (int hero = 0; hero < 4; ++hero)
                for (const char *kind : {"sc", "gf"})
                    for (int scene = 0; scene < 10; ++scene)
                    {
                        std::snprintf(text, sizeof(text), "cinema/hero%d/%s%d%02d", hero, kind, hero, scene);
                        n.insert(text);
                    }
            return n;
        }();
        return names.count(name) != 0;
    }

    std::string utf8(const std::filesystem::path &path)
    {
        const std::u8string u8 = path.u8string();
        return std::string(u8.begin(), u8.end());
    }

    bool readText(const std::filesystem::path &file, std::string &text)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return false;
        std::ostringstream all;
        all << in.rdbuf();
        text = all.str();
        return true;
    }

    // the shape a picture of this width / height is called by: "4:3", "16:9" or "other"
    const char *shapeName(double aspect)
    {
        if (std::abs(aspect - 4.0 / 3.0) <= 0.05)
            return "4:3";
        if (std::abs(aspect - 16.0 / 9.0) <= 0.05)
            return "16:9";
        return "other";
    }

    std::string guestString(uint8_t *rdram, uint32_t address)
    {
        std::string s;
        for (uint32_t i = 0; i < 256u; ++i)
        {
            const char c = char(rdram[(address + i) & 0x1FFFFFFu]);
            if (!c)
                break;
            s.push_back(c);
        }
        return s;
    }

    std::string avError(int code)
    {
        char text[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(code, text, sizeof(text));
        return text;
    }

    // ---------------------------------------------------------------- the file: opened, then decoded on a thread

    std::unique_ptr<Player> open(const std::string &name, const std::filesystem::path &file, std::string &why)
    {
        auto p = std::make_unique<Player>();
        p->name = name;
        p->file = file;
        const std::string path = utf8(file); // FFmpeg takes UTF-8 paths
        if (int e = avformat_open_input(&p->format, path.c_str(), nullptr, nullptr); e < 0)
        {
            why = "can't be opened (" + avError(e) + ")";
            return nullptr;
        }
        if (int e = avformat_find_stream_info(p->format, nullptr); e < 0)
        {
            why = "can't be read (" + avError(e) + ")";
            return nullptr;
        }
        p->videoStream = av_find_best_stream(p->format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (p->videoStream < 0)
        {
            why = "has no picture";
            return nullptr;
        }
        auto openCodec = [&](int stream, AVCodecContext *&out) -> bool
        {
            const AVCodecParameters *par = p->format->streams[stream]->codecpar;
            const AVCodec *codec = avcodec_find_decoder(par->codec_id);
            if (!codec)
                return false;
            out = avcodec_alloc_context3(codec);
            if (!out || avcodec_parameters_to_context(out, par) < 0)
                return false;
            out->thread_count = 0; // as many as FFmpeg likes
            return avcodec_open2(out, codec, nullptr) >= 0;
        };
        if (!openCodec(p->videoStream, p->video))
        {
            why = "has a picture this game can't decode (" + std::string(avcodec_get_name(p->format->streams[p->videoStream]->codecpar->codec_id)) + ")";
            return nullptr;
        }
        p->audioStream = av_find_best_stream(p->format, AVMEDIA_TYPE_AUDIO, -1, p->videoStream, nullptr, 0);
        if (p->audioStream >= 0 && !openCodec(p->audioStream, p->audio))
        {
            avcodec_free_context(&p->audio);
            p->audioStream = -1; // the picture without its sound
        }
        if (p->audio)
        {
            AVChannelLayout stereo{};
            av_channel_layout_default(&stereo, 2);
            if (swr_alloc_set_opts2(&p->resample, &stereo, AV_SAMPLE_FMT_S16, int(kRate), &p->audio->ch_layout,
                                    p->audio->sample_fmt, p->audio->sample_rate, 0, nullptr) < 0 ||
                swr_init(p->resample) < 0)
            {
                swr_free(&p->resample);
                avcodec_free_context(&p->audio);
                p->audioStream = -1;
            }
            av_channel_layout_uninit(&stereo);
        }
        const AVStream *vs = p->format->streams[p->videoStream];
        const uint32_t w = uint32_t(std::max(1, p->video->width)), h = uint32_t(std::max(1, p->video->height));
        p->outW = std::min(w, kMaxWidth);
        p->outH = std::max(1u, uint32_t(uint64_t(h) * p->outW / w));
        AVRational sar = av_guess_sample_aspect_ratio(p->format, const_cast<AVStream *>(vs), nullptr);
        const double pixel = sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0;
        p->aspect = double(w) * pixel / double(h);
        const AVRational rate = av_guess_frame_rate(p->format, const_cast<AVStream *>(vs), nullptr);
        p->fps = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 30.0;
        p->length = p->format->duration > 0 ? double(p->format->duration) / AV_TIME_BASE : 0.0;
        return p;
    }

    void decode(Player *p)
    {
        AVPacket *packet = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        const AVStream *vs = p->format->streams[p->videoStream];
        const double vbase = av_q2d(vs->time_base);
        const int64_t vstart = vs->start_time != AV_NOPTS_VALUE ? vs->start_time : 0;
        double nextPts = 0.0;
        bool atEnd = false;

        auto takePictures = [&]()
        {
            while (avcodec_receive_frame(p->video, frame) >= 0)
            {
                Frame f;
                const int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
                f.pts = ts != AV_NOPTS_VALUE ? double(ts - vstart) * vbase : nextPts;
                nextPts = f.pts + 1.0 / p->fps;
                f.frame.reset(av_frame_alloc());
                av_frame_move_ref(f.frame.get(), frame);
                // at most two seconds of pictures ahead (a file whose sound lags its pictures still plays)
                std::unique_lock<std::mutex> lk(p->lock);
                p->wake.wait(lk, [&] { return p->stop.load() || double(p->frames.size()) < p->fps * 2.0; });
                if (p->stop)
                    return;
                p->frames.push_back(std::move(f));
            }
        };
        auto takeSound = [&]()
        {
            while (avcodec_receive_frame(p->audio, frame) >= 0)
            {
                const int room = swr_get_out_samples(p->resample, frame->nb_samples);
                std::vector<int16_t> out(size_t(std::max(0, room)) * 2u);
                uint8_t *dst[1] = {reinterpret_cast<uint8_t *>(out.data())};
                const int got = swr_convert(p->resample, dst, room, const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
                av_frame_unref(frame);
                if (got <= 0)
                    continue;
                std::lock_guard<std::mutex> lk(p->lock);
                if (p->soundRead > 0 && p->soundRead * 2 > p->sound.size())
                {
                    p->sound.erase(p->sound.begin(), p->sound.begin() + std::ptrdiff_t(p->soundRead));
                    p->soundRead = 0;
                }
                p->sound.insert(p->sound.end(), out.begin(), out.begin() + std::ptrdiff_t(got) * 2);
            }
        };

        while (!p->stop)
        {
            {
                // read on while pictures or sound are wanted: up to kAheadFrames pictures, a second of sound
                std::unique_lock<std::mutex> lk(p->lock);
                p->wake.wait(lk, [&] {
                    const size_t soundAhead = (p->sound.size() - p->soundRead) / 2u;
                    return p->stop.load() || int(p->frames.size()) < kAheadFrames || (p->audio && soundAhead < kRate);
                });
            }
            if (p->stop)
                break;
            const int e = av_read_frame(p->format, packet);
            if (e < 0)
            {
                // the end: what the decoders still hold
                avcodec_send_packet(p->video, nullptr);
                takePictures();
                if (p->audio)
                {
                    avcodec_send_packet(p->audio, nullptr);
                    takeSound();
                }
                atEnd = !p->stop;
                break;
            }
            if (packet->stream_index == p->videoStream && avcodec_send_packet(p->video, packet) >= 0)
                takePictures();
            else if (p->audio && packet->stream_index == p->audioStream && avcodec_send_packet(p->audio, packet) >= 0)
                takeSound();
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        av_frame_free(&frame);
        std::lock_guard<std::mutex> lk(p->lock);
        p->ended = true;
        if (g_log && atEnd)
            std::cerr << "[videos] " << p->name << ": the file read to its end" << std::endl;
    }

    // ---------------------------------------------------------------- the sound: into the game's own output

    void mix(int16_t *out, size_t count, size_t queued)
    {
        Player *p = g_mixing.load(std::memory_order_acquire);
        if (!p)
            return;
        size_t took = 0;
        {
            std::lock_guard<std::mutex> lk(p->lock);
            const size_t have = (p->sound.size() - p->soundRead) / 2u;
            took = std::min(count, have);
            const int16_t *in = p->sound.data() + p->soundRead;
            for (size_t i = 0; i < took * 2u; ++i)
                out[i] = int16_t(std::clamp(int(out[i]) + int(in[i]), -32768, 32767));
            p->soundRead += took * 2u;
        }
        p->wake.notify_one();
        if (took == 0)
            return; // none of ours in this chunk (all of it mixed, or the decoder behind): the clock runs on by the time
        const uint64_t mixed = p->mixed.fetch_add(took) + took;
        // what is being heard now: the mixed frames less those still queued before this chunk and this chunk itself
        p->heard.store(int64_t(mixed) - int64_t(queued) - int64_t(count), std::memory_order_relaxed);
        p->lastMixUs.store(nowUs(), std::memory_order_relaxed);
    }

    // where the sound is (seconds): what has been heard, run on by the time since the last mix; < 0 = none yet
    double soundClock(Player &p)
    {
        const int64_t last = p.lastMixUs.load(std::memory_order_relaxed);
        if (last == 0 || p.mixed.load(std::memory_order_relaxed) == 0)
            return -1.0;
        const double heard = std::max<int64_t>(0, p.heard.load(std::memory_order_relaxed)) / double(kRate);
        return heard + (nowUs() - last) / 1e6;
    }

    // the video's clock in seconds: the time since it began, drawn slowly to where the sound is (the picture follows
    // the sound, without the sound's own jitter: its chunks come in bursts); the time alone with no sound
    double clock(Session &s)
    {
        Player &p = *s.player;
        const double since = std::chrono::duration<double>(Clock::now() - s.began).count();
        if (!p.audio || s.soundless)
            return since;
        const double sound = soundClock(p);
        if (sound < 0.0)
        {
            if (since > 0.5)
                s.soundless = true; // no sound in half a second: it isn't coming
            return 0.0;
        }
        const double wanted = sound - since;
        if (!s.offsetSet || std::abs(wanted - s.offset) > 0.1)
            s.offset = wanted; // far off (the start, a stall): at once
        else
            s.offset += (wanted - s.offset) * 0.05;
        s.offsetSet = true;
        return std::max(0.0, since + s.offset);
    }

    // ---------------------------------------------------------------- the picture: to the window

    void draw(Session &s, const Frame *picture)
    {
        Player &p = *s.player;
        if (!picture)
            return;
        const AVFrame *f = picture->frame.get();
        p.scale = sws_getCachedContext(p.scale, f->width, f->height, AVPixelFormat(f->format), int(p.outW), int(p.outH),
                                       AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!p.scale)
            return;
        std::vector<uint32_t> rgba(size_t(p.outW) * p.outH);
        uint8_t *dst[1] = {reinterpret_cast<uint8_t *>(rgba.data())};
        const int stride[1] = {int(p.outW * 4u)};
        sws_scale(p.scale, f->data, f->linesize, 0, f->height, dst, stride);
        gsSetPresentOverlay(p.outW, p.outH, std::move(rgba), p.aspect);
        ++s.shown;
    }

    // ---------------------------------------------------------------- the game's thread: one step a picture

    GuestInvocation guestCall(const R5900Context &parent, uint32_t function, std::initializer_list<uint32_t> args,
                              std::function<void(const R5900Context &, R5900Context &)> done = {})
    {
        GuestInvocation call{};
        call.kind = GuestInvocationKind::HleCall;
        call.context = parent; // the game's gp and stack: a call made from the movie routine's own frame
        call.context.pc = function;
        int r = 4;
        for (uint32_t a : args)
        {
            SET_GPR_U32(&call.context, r, a); // (the macro reads its register twice: no r++ inside)
            ++r;
        }
        SET_GPR_U32(&call.context, 31, 0u); // returns to pc 0: the call's end
        call.onComplete = std::move(done);
        return call;
    }

    void picture(R5900Context &ctx);

    void finish(R5900Context &ctx, const char *how)
    {
        Session &s = g_session;
        g_mixing.store(nullptr, std::memory_order_release);
        Player *p = s.player.get();
        p->stop = true;
        p->wake.notify_all();
        if (p->thread.joinable())
            p->thread.join();
        gsClearPresentOverlay();
        const double took = std::chrono::duration<double>(Clock::now() - s.began).count();
        std::cerr << "[videos] " << s.player->name << ": " << how << " after " << took << " s (" << s.pictures << " pictures, "
                  << s.shown << " of the video's shown, " << s.dropped << " skipped as late; sound mixed "
                  << double(p->mixed.load()) / kRate << " s)" << std::endl;
        s.player.reset();
        s.active = false;
        setReturnU32(&ctx, 1u);
        ctx.pc = s.ra; // back to the caller, as the game's routine returns
    }

    void picture(R5900Context &ctx)
    {
        Session &s = g_session;
        Player &p = *s.player;
        ++s.pictures;
        g_lastPictureMs.store(nowUs() / 1000, std::memory_order_relaxed);
        // the picture due now: the newest one whose time has come (any older ones skipped)
        const double t = clock(s);
        std::unique_ptr<Frame> due;
        bool over = false;
        {
            std::lock_guard<std::mutex> lk(p.lock);
            while (!p.frames.empty() && p.frames.front().pts <= t)
            {
                if (due)
                    ++s.dropped;
                due = std::make_unique<Frame>(std::move(p.frames.front()));
                p.frames.pop_front();
            }
            // the end: the last picture's time over; or, whatever the clock says, a second past the file's length
            const double since = std::chrono::duration<double>(Clock::now() - s.began).count();
            over = p.ended && p.frames.empty() && (t >= s.lastPts + 1.0 / p.fps || (p.length > 0.0 && since > p.length + 1.0));
            if (g_log && s.pictures % 60u == 1u)
                std::cerr << "[videos] " << p.name << " picture " << s.pictures << ": clock " << t << " s, sound at "
                          << soundClock(p) << " s (mixed " << double(p.mixed.load()) / kRate << " s, last mix "
                          << (p.lastMixUs.load() ? (nowUs() - p.lastMixUs.load()) / 1000 : -1) << " ms ago), "
                          << p.frames.size() << " pictures and " << double(p.sound.size() - p.soundRead) / 2.0 / kRate
                          << " s of sound decoded ahead" << (p.ended ? ", the file read to its end" : "") << "; "
                          << s.shown << " shown, " << s.dropped << " skipped" << std::endl;
        }
        p.wake.notify_one();
        if (due)
            s.lastPts = due->pts;
        if (s.wide)
            ps2_widescreen::markWideFrame();
        draw(s, due.get());
        if (over)
        {
            finish(ctx, "ended");
            return;
        }
        PS2Runtime *runtime = s.runtime;
        std::vector<GuestInvocation> calls;
        calls.push_back(guestCall(ctx, kReadPads, {1u}));
        calls.push_back(guestCall(ctx, kPeriodic, {0u}));
        calls.push_back(guestCall(ctx, kPressed, {0u, 1u}, [runtime](const R5900Context &done, R5900Context &parent) {
            if (getRegU32(&done, 2) & g_session.mask)
            {
                finish(parent, "skipped");
                return;
            }
            // the next picture: after the next VBlank, as the game's movie loop waits for its flip
            EeScheduler &ee = runtime->eeScheduler();
            ee.waitVSync(ee.currentVSyncTick(), -1, [](R5900Context &resumed) { picture(resumed); });
        }));
        runtime->eeScheduler().invokeCurrentSequence(std::move(calls));
    }

    // ---------------------------------------------------------------- the hook

    void playMovie(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_session.active)
        {
            // the movie routine entered again while ours plays: only the game's thread resumed at the routine's
            // entry, which our steps never leave it at; carry on with the next picture
            if (getRegU32(ctx, 29) == g_session.sp)
            {
                picture(*ctx);
                return;
            }
            g_original(rdram, ctx, runtime);
            return;
        }
        const std::string asked = guestString(rdram, getRegU32(ctx, 4));
        const std::string name = movieName(asked);
        const auto it = g_videos.find(name);
        if (it == g_videos.end())
        {
            g_original(rdram, ctx, runtime);
            return;
        }
        if (!gsPresentOverlayAvailable())
        {
            Replacement &last = it->second.back();
            if (!last.checked)
                vendetta_mods::problem() << "[videos] " << name << ": " << last.theme << "'s video needs the window shown through Vulkan "
                                         << "(the graphics card renderer): the game's movie plays" << std::endl;
            last.checked = true;
            g_original(rdram, ctx, runtime);
            return;
        }
        // the last theme's file that opens (one that doesn't: said once, the one before it tried)
        Replacement *chosen = nullptr;
        std::unique_ptr<Player> player;
        std::vector<Replacement> &all = it->second;
        for (size_t i = all.size(); i-- > 0 && !player;)
        {
            Replacement &c = all[i];
            if (c.broken)
                continue;
            std::string why;
            player = open(name, c.file, why);
            if (player)
            {
                chosen = &c;
                break;
            }
            c.broken = true;
            const Replacement *before = nullptr;
            for (size_t j = i; j-- > 0;)
                if (!all[j].broken)
                {
                    before = &all[j];
                    break;
                }
            vendetta_mods::problem() << "[videos] " << c.theme << ": " << name << "'s " << utf8(c.file.filename()) << " " << why << ": "
                                     << (before ? before->theme + "'s video plays" : std::string("the game's movie plays")) << std::endl;
        }
        if (!player)
        {
            g_original(rdram, ctx, runtime);
            return;
        }
        Replacement &r = *chosen;
        if (!r.checked && !r.shape.empty() && r.shape != shapeName(player->aspect))
            vendetta_mods::problem() << "[videos] " << r.theme << ": " << name << "'s " << utf8(r.file.filename()) << " is "
                                     << shapeName(player->aspect) << " (" << player->video->width << " x " << player->video->height
                                     << "), not the " << r.shape << " its list says: shown as the file is" << std::endl;
        r.checked = true;
        Session &s = g_session;
        s = Session{};
        s.active = true;
        s.player = std::move(player);
        s.ra = getRegU32(ctx, 31);
        s.sp = getRegU32(ctx, 29);
        s.mask = getRegU32(ctx, 5);
        s.rdram = rdram;
        s.runtime = runtime;
        s.wide = ps2_widescreen::targetAspect() > 0.0f && s.player->aspect > 1.4;
        Player *p = s.player.get();
        std::cerr << "[videos] " << name << " (the game asked for " << asked << "): " << r.theme << "'s " << utf8(p->file) << ", "
                  << p->video->width << " x " << p->video->height << " (" << p->outW << " x " << p->outH << " shown), "
                  << p->fps << " a second, " << p->length << " s, " << (p->audio ? "with sound" : "no sound")
                  << (s.wide ? ", shown wide" : ", shown in 4:3") << ", skip mask 0x" << std::hex << s.mask << std::dec << std::endl;
        p->thread = std::thread(decode, p);
        s.began = Clock::now();
        p->start = s.began;
        g_mixing.store(p, std::memory_order_release);
        // black in the window until the first picture is decoded
        gsSetPresentOverlay(1u, 1u, std::vector<uint32_t>{0xFF000000u}, p->aspect);
        // the display as the game's movie sets it: 640 x 480, NTSC, field; buffer FBP 0 shown. The movie's two
        // buffers (FBP 0 and 0x96, where the game's player puts its pictures) filled with black by the game's own
        // routines, as a movie ending on black leaves them: what the display shows after (the caller clears only
        // 512 x 256 of them before its next screen draws), never what was there before the movie. Then the first picture.
        std::vector<GuestInvocation> calls;
        calls.push_back(guestCall(*ctx, kResetGraph, {0u, 1u, 2u, 0u}));
        calls.push_back(guestCall(*ctx, kShowBuffer, {0u, 640u, 480u, 0u}));
        for (uint32_t fbp : {0x96u, 0u})
        {
            calls.push_back(guestCall(*ctx, kDrawInto, {fbp, 640u, 480u, 0u, 0u, 0u, 0u, 0u}));
            calls.push_back(guestCall(*ctx, kFillBuffer, {640u, 480u, 0u, 0u, 0u, 0x80u}));
        }
        calls.back().onComplete = [](const R5900Context &, R5900Context &parent) { picture(parent); };
        runtime->eeScheduler().invokeCurrentSequence(std::move(calls));
    }

    // the project's video themes, in load order (the later one wins movie by movie)
    void readThemes()
    {
        for (const vendetta_mods::Mod &mod : vendetta_mods::loadedMods())
        {
            const vendetta_mods::Json *videos = mod.manifest.find("videos");
            if (!videos || !videos->isObject())
                continue;
            if (!vendetta_mods::themeOn(mod, "videos"))
                continue; // the project chose another video theme, or the game's
            const vendetta_mods::Json *format = videos->find("format");
            if (format && format->number > kFormat)
            {
                vendetta_mods::problem() << "[videos] " << mod.name << "'s videos need a newer game (format " << int(format->number)
                                         << "): skipped" << std::endl;
                continue;
            }
            const vendetta_mods::Json *list = videos->find("list");
            if (!list || !list->isString() || list->text.empty())
            {
                vendetta_mods::problem() << "[videos] " << mod.name << "'s \"videos\" names no list: skipped" << std::endl;
                continue;
            }
            const std::filesystem::path listFile = vendetta_mods::modFile(mod, list->text, "video list");
            if (listFile.empty())
                continue;
            std::string text, error;
            vendetta_mods::Json json;
            if (!readText(listFile, text) || !vendetta_mods::parseJson(text, json, error))
            {
                vendetta_mods::problem() << "[videos] " << mod.name << ": " << list->text << " can't be read: " << error << std::endl;
                continue;
            }
            const vendetta_mods::Json *entries = json.find("videos");
            if (!entries || !entries->isObject())
            {
                vendetta_mods::problem() << "[videos] " << mod.name << ": " << list->text << " has no \"videos\"" << std::endl;
                continue;
            }
            const std::string folder = std::filesystem::path(list->text).parent_path().generic_string();
            int used = 0;
            for (const auto &[asked, entry] : entries->members)
            {
                const std::string name = movieName(asked);
                if (!isMovie(name))
                {
                    vendetta_mods::problem() << "[videos] " << mod.name << ": \"" << asked << "\" isn't one of the game's movies: ignored" << std::endl;
                    continue;
                }
                const vendetta_mods::Json *video = entry.isObject() ? entry.find("video") : nullptr;
                if (!video || !video->isString() || video->text.empty())
                {
                    vendetta_mods::problem() << "[videos] " << mod.name << ": " << name << " names no video: the game's movie plays" << std::endl;
                    continue;
                }
                const std::filesystem::path file = vendetta_mods::modFile(mod, folder.empty() ? video->text : folder + "/" + video->text,
                                                                          ("the video for " + name).c_str());
                if (file.empty())
                    continue; // (its line written: not there, or outside the mod's folder)
                Replacement r;
                r.theme = mod.name;
                r.file = file;
                if (const vendetta_mods::Json *shape = entry.find("shape"); shape && shape->isString())
                    r.shape = shape->text;
                if (const auto had = g_videos.find(name); had != g_videos.end())
                    vendetta_mods::problem() << "[videos] " << name << ": " << mod.name << "'s video used, not " << had->second.back().theme << "'s" << std::endl;
                g_videos[name].push_back(std::move(r));
                ++used;
            }
            std::cerr << "[videos] " << mod.name << ": " << used << (used == 1 ? " movie" : " movies") << " replaced" << std::endl;
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *off = std::getenv("VENDETTA_VIDEOS"); off && off[0] == '0')
            return;
        g_log = std::getenv("VENDETTA_VIDEOLOG") && *std::getenv("VENDETTA_VIDEOLOG") != '0';
        readThemes();
        if (const char *list = std::getenv("VENDETTA_VIDEOTRY"); list && *list)
        {
            std::string all = list;
            size_t at = 0;
            while (at <= all.size())
            {
                size_t end = all.find(';', at);
                if (end == std::string::npos)
                    end = all.size();
                const std::string item = all.substr(at, end - at);
                if (const size_t eq = item.find('='); eq != std::string::npos)
                {
                    Replacement r;
                    r.theme = "VENDETTA_VIDEOTRY";
                    r.file = std::filesystem::path(item.substr(eq + 1));
                    g_videos[movieName(item.substr(0, eq))].push_back(std::move(r));
                }
                at = end + 1;
            }
        }
        if (g_videos.empty())
            return; // nothing replaced: the game's player as it is, not hooked
        g_original = runtime.lookupFunction(kPlayMovie);
        if (!g_original)
        {
            std::cerr << "[videos] FUN_001bb3b8 not found: off" << std::endl;
            return;
        }
        runtime.replaceFunction(kPlayMovie, playMovie);
        vendetta_mixer::add(mix);
        if (g_log)
            for (const auto &[movie, files] : g_videos)
                std::cerr << "[videos] " << movie << " plays " << files.back().theme << "'s " << utf8(files.back().file)
                          << (files.size() > 1 ? " (" + std::to_string(files.size() - 1) + " more before it)" : std::string()) << std::endl;
    }
}

void installVendettaVideos(PS2Runtime &runtime) { vendetta_videos::install(runtime); }

// One of our videos made a picture in the last second (the freeze watch: the game's frame counter stands still meanwhile).
bool vendettaVideoPlaying()
{
    const int64_t last = vendetta_videos::g_lastPictureMs.load(std::memory_order_relaxed);
    return last != 0 && vendetta_videos::nowUs() / 1000 - last < 1000;
}
