// Def Jam Vendetta (USA, SLUS-20639) - a test match: the game goes straight from the title into a match set in a file
// (--test-match "<file>" on the command line, or VENDETTA_TESTMATCH=<file>), for trying a move: the wrestlers, the arena,
// who plays each side, a computer player that throws one chosen moveset slot again and again, health kept full, and the
// match starting again by itself. Off (nothing hooked) without the file.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// The file:
//   { "match": "1-on-1", "arena": 7,
//     "fighters": [ { "record": 0, "attire": 0, "control": "human" },
//                   { "record": 9, "attire": 0, "control": "cpu", "level": 1 } ],
//     "repeat": { "fighter": 1, "slot": 12 },
//     "keep health": [ true, true ], "loop": true, "skip movies": true }
// "match": "1-on-1" (two fighters) or "4-way" (four, all against all: the block as Battle's Free For All leaves it,
// +0x10 = 1, +0x14 = +0x18 = 4, the pads 0 / 4 / 5 / 6).
//
// VENDETTA_TESTWIN=1 (a test, with or without the file, in any match, Story's included): while a match runs, every
// human fighter (in the match, +0x5BC bit 1, and not the computer's, bit 0, when the fight starts) is played by the
// computer with its momentum held full (it goes for its Blazin' move: a knockout) and every other fighter's health is
// held at 3 % of its full, so the human side wins in moments; VENDETTA_PADSCRIPT's clock stands still while the fight
// runs, so the script's presses after it land the same way however long it took: a test run reaches the screens after a won fight (a story fight's summary,
// The Ladder, "You've unlocked ...").
// "record" = a character record (0-62, or a record a mod adds), "attire" = added to it (the menus give the grid slot's
// first record + the attire); "control" = human / cpu / dummy (a computer player that never presses anything: no
// attacks, blocks or reversals); "level" = the computer's skill 0-2; "arena" = the stage 0-11 as Choose Stage numbers
// them.
//
// How (the game's own way in, nothing of the menus touched):
// - The front end FUN_0013fa98 runs one screen routine a picture by the screen number 0x29E46C (block 0x29E468 + 4).
//   On the title's first picture (screen 5, FUN_00141bd0) the block gets what Battle's menus leave behind for a 1-on-1
//   (+0x10 match type 0, +0x14 / +0x18 = 2, +0x20 the computer players' bits, +0x24 / +0x28 the pads, +0x34 / +0x44
//   records and attires, +0x54 the stage, no profiles at +0x770 + 0x4C n: nothing is ever saved) and the screen
//   becomes 0x19, which Choose Stage sets on its last picture: the match setup FUN_00140298 copies it into the match
//   block 0x2361C8, the VS screen loads the match, the match runs. The computer's skill is the options byte 0x240F87.
// - The two boot movies (FUN_001bb3b8) are skipped with "skip movies".
// - The computer player (FUN_00107470(player), once a picture; its brain 0x22BDE8 + 0x3C0 n): when it is free to think
//   (action +0x0C = 0, standing, both in the ring), the action for the slot's kind is started the way FUN_0010fcf0 does
//   and the slot is its plan (+0xD8, flag 0x10 in +0xB6): a grab (action 10 weak, 11 strong), whose lock-up FUN_0010c518
//   throws the planned slot first; a hard strike (action 9, it reads the plan). Standing strikes (8), running strikes
//   (13) and taunts (15) pick from a list (FUN_00112f90(brain, list, flags)): answered with the slot when the list
//   holds it. A dummy's brain is not run: its outputs (+0x1C, +0x20, +0x30, +0x34) stay 0.
// - After the match logic (FUN_0011a600, once a picture): health kept full (+0x3A8 = +0x3B0 = +0x3AC), the thrower's
//   momentum kept under the Blazin' line (a Blazin' slot: full); at the end (phases 7-10) no results screen and no
//   GAME OVER menu, REMATCH chosen (0x22FDE9 |= 2 | 8): phase 10 then runs the match init FUN_00119d70 again with the
//   rematch count 0x22FDFC + 1, the same wrestlers, arena and moves, no reload.
// - The count: the clip starter FUN_00132158(fighter, clip, flags) is watched for the thrower: the first move clip (an
//   id below 10000) he starts is the move's, and each start of it is a throw. "<file>.status" (beside the file) says
//   "throws N" and "match M" for the tool, rewritten when either changes.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "vendetta_mods.h"
#include "vendetta_wrestlers.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace ps2_stubs
{
    void padScriptHold(bool held); // VENDETTA_PADSCRIPT's clock held or running (the runtime's pad code)
}

namespace vendetta_testmatch
{
    constexpr uint32_t kMoviePlayer = 0x1BB3B8u;  // FUN_001bb3b8(name, flags): a movie, to its end
    constexpr uint32_t kTitle = 0x141BD0u;        // FUN_00141bd0(block): the title, screen 5
    constexpr uint32_t kCpuBrain = 0x107470u;     // FUN_00107470(player): the computer player, once a picture
    constexpr uint32_t kListPick = 0x112F90u;     // FUN_00112f90(brain, list, flags): a random slot of a brain list
    constexpr uint32_t kMatchLogic = 0x11A600u;   // FUN_0011a600: the match's logic, once a picture
    constexpr uint32_t kClipStart = 0x132158u;    // FUN_00132158(fighter, clip, flags): a fighter starts a clip

    constexpr uint32_t kFrontEnd = 0x29E468u;
    constexpr uint32_t kCpuLevel = 0x240F87u;     // options byte: the computer's skill 0-2
    constexpr uint32_t kBrains = 0x22BDE8u, kBrainSize = 0x3C0u;
    constexpr uint32_t kFighters = 0x22FE08u, kFighterSize = 0xCD8u;
    constexpr uint32_t kPhase = 0x22FBE4u;
    constexpr uint32_t kEndResults = 0x22FDE8u;   // byte: 1 run the results screen, 2 done
    constexpr uint32_t kEndMenu = 0x22FDE9u;      // byte: 1 run GAME OVER, 2 done, 4 quit, 8 rematch
    constexpr uint32_t kRematches = 0x22FDFCu;

    enum class Control { Human, Cpu, Dummy };

    struct Fighter
    {
        int record = 0;
        int attire = 0;
        Control control = Control::Cpu;
        int level = 1;
        bool keepHealth = false;
    };

    struct Setup
    {
        std::array<Fighter, 4> fighters{};
        int count = 2; // 2 (1-on-1) or 4 (4-way)
        int arena = 0;
        int repeatFighter = -1;
        int repeatSlot = -1;
        bool loop = true;
        bool skipMovies = true;
    };

    Setup g_setup;
    bool g_testWin = false;     // VENDETTA_TESTWIN
    uint32_t g_winHumans = 0;   // ... the human fighters of the fight going on (bits)
    bool g_winKnown = false;
    bool g_armed = false;       // the title's first picture not reached yet
    bool g_started = false;
    int g_repeatAction = 0;     // the brain action that leads to the slot (0 = none)
    bool g_repeatBlazin = false;
    bool g_repeatFromList = false;
    uint32_t g_lastRematch = 0;
    unsigned g_throws = 0;      // the computer's tries (the log)
    unsigned g_thrown = 0;      // the move's clip started
    int32_t g_moveClip = -1;
    std::string g_statusPath;

    PS2Runtime::RecompiledFunction g_originalMovie = nullptr;
    PS2Runtime::RecompiledFunction g_originalTitle = nullptr;
    PS2Runtime::RecompiledFunction g_originalBrain = nullptr;
    PS2Runtime::RecompiledFunction g_originalListPick = nullptr;
    PS2Runtime::RecompiledFunction g_originalMatchLogic = nullptr;
    PS2Runtime::RecompiledFunction g_originalClipStart = nullptr;

    void writeStatus()
    {
        if (g_statusPath.empty())
            return;
        std::ofstream out(g_statusPath, std::ios::binary | std::ios::trunc);
        out << "throws " << g_thrown << '\n' << "match " << (g_lastRematch + 1) << '\n';
    }

    void returnToCaller(R5900Context *ctx) { ctx->pc = getRegU32(ctx, 31); }
    float readF(uint8_t *rdram, uint32_t a) { return std::bit_cast<float>(Ps2FastRead32(rdram, a)); }
    void writeF(uint8_t *rdram, uint32_t a, float v) { Ps2FastWrite32(rdram, a, std::bit_cast<uint32_t>(v)); }

    // the brain action that leads to a slot, and whether it picks from a list (the slots RE 8.42 names)
    void actionFor(int slot)
    {
        g_repeatAction = 0;
        g_repeatBlazin = slot == 17 || slot == 25;
        g_repeatFromList = false;
        if ((slot >= 1 && slot <= 8) || slot == 21 || slot == 22)
            g_repeatAction = 10;                  // a weak grab: the lock-up throws the plan
        else if ((slot >= 9 && slot <= 17) || (slot >= 23 && slot <= 25))
            g_repeatAction = 11;                  // a strong grab (17 / 25 Blazin')
        else if (slot >= 50 && slot <= 53)
            g_repeatAction = 9;                   // a hard strike reads the plan
        else if (slot >= 46 && slot <= 49)
            g_repeatAction = 8, g_repeatFromList = true;
        else if (slot >= 54 && slot <= 57)
            g_repeatAction = 13, g_repeatFromList = true;
        else if ((slot >= 86 && slot <= 88) || slot == 90)
            g_repeatAction = 15, g_repeatFromList = true;
    }

    bool readSetup(const std::string &path, Setup &out)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            std::cerr << "[testmatch] " << path << " can't be read: the game starts as usual" << std::endl;
            return false;
        }
        std::stringstream text;
        text << in.rdbuf();
        vendetta_mods::Json json;
        std::string error;
        if (!vendetta_mods::parseJson(text.str(), json, error) || !json.isObject())
        {
            std::cerr << "[testmatch] " << path << ": " << (error.empty() ? "not a JSON object" : error)
                      << ": the game starts as usual" << std::endl;
            return false;
        }
        auto number = [](const vendetta_mods::Json *j, int fallback) {
            return j && j->type == vendetta_mods::Json::Type::Number ? static_cast<int>(j->number) : fallback;
        };
        auto flag = [](const vendetta_mods::Json *j, bool fallback) {
            return j && j->type == vendetta_mods::Json::Type::Bool ? j->boolean : fallback;
        };
        out.count = 2;
        if (const auto *m = json.find("match"); m && m->isString() && m->text == "4-way")
            out.count = 4;
        else if (m && m->isString() && m->text != "1-on-1")
            std::cerr << "[testmatch] \"match\" is \"1-on-1\" or \"4-way\": a 1-on-1" << std::endl;
        out.arena = std::clamp(number(json.find("arena"), 0), 0, 11);
        const auto *fighters = json.find("fighters");
        if (!fighters || !fighters->isArray() || fighters->items.size() != size_t(out.count))
        {
            std::cerr << "[testmatch] \"fighters\" should list " << out.count << " fighters: the game starts as usual"
                      << std::endl;
            return false;
        }
        const auto *keep = json.find("keep health");
        for (size_t i = 0; i < size_t(out.count); ++i)
        {
            const auto &f = fighters->items[i];
            Fighter &o = out.fighters[i];
            o.record = number(f.find("record"), 0);
            o.attire = number(f.find("attire"), 0);
            o.level = std::clamp(number(f.find("level"), 1), 0, 2);
            const auto *control = f.find("control");
            const std::string c = control && control->isString() ? control->text : "cpu";
            o.control = c == "human" ? Control::Human : c == "dummy" ? Control::Dummy : Control::Cpu;
            o.keepHealth = keep && keep->isArray() && i < keep->items.size() && flag(&keep->items[i], false);
        }
        if (const auto *repeat = json.find("repeat"); repeat && repeat->isObject())
        {
            out.repeatFighter = std::clamp(number(repeat->find("fighter"), -1), -1, out.count - 1);
            out.repeatSlot = number(repeat->find("slot"), -1);
        }
        out.loop = flag(json.find("loop"), true);
        out.skipMovies = flag(json.find("skip movies"), true);
        return true;
    }

    std::string askedFile()
    {
#if defined(_MSC_VER)
        for (int i = 1; i < __argc; ++i)
        {
            const std::string arg = __argv[i];
            if (arg == "--test-match" && i + 1 < __argc)
                return __argv[i + 1];
            if (arg.rfind("--test-match=", 0) == 0)
                return arg.substr(13);
        }
#endif
        const char *env = std::getenv("VENDETTA_TESTMATCH");
        return env ? env : "";
    }

    // ---- the boot

    void movie(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_armed && g_setup.skipMovies)
        {
            setReturnU32(ctx, 0u);
            returnToCaller(ctx);
            return;
        }
        g_originalMovie(rdram, ctx, runtime);
    }

    void title(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!g_armed || getRegU32(ctx, 4) != kFrontEnd)
        {
            g_originalTitle(rdram, ctx, runtime);
            return;
        }
        g_armed = false;
        const uint32_t fe = kFrontEnd;
        uint32_t cpuBits = 0;
        int level = 1;
        const int count = g_setup.count;
        for (int p = 0; p < count; ++p)
            if (g_setup.fighters[p].control != Control::Human)
            {
                cpuBits |= 1u << p;
                // one skill for every computer player (the options byte): the thrower's, else the opponent's
                if (g_setup.fighters[p].control == Control::Cpu && (p == g_setup.repeatFighter || g_setup.repeatFighter < 0))
                    level = g_setup.fighters[p].level;
            }
        Ps2FastWrite32(rdram, fe + 0x10, count == 4 ? 1u : 0u); // FREE FOR ALL / 1-ON-1 BATTLE
        Ps2FastWrite32(rdram, fe + 0x14, uint32_t(count));
        Ps2FastWrite32(rdram, fe + 0x18, uint32_t(count));
        Ps2FastWrite32(rdram, fe + 0x1C, 0u);
        Ps2FastWrite32(rdram, fe + 0x20, cpuBits);
        // the pads: port 1 plays the first human (port 2 the other side)
        const bool humanRight = count == 2 && g_setup.fighters[0].control != Control::Human &&
                                g_setup.fighters[1].control == Control::Human;
        Ps2FastWrite32(rdram, fe + 0x24, humanRight ? 4u : 0u);
        Ps2FastWrite32(rdram, fe + 0x28, humanRight ? 0u : 4u);
        Ps2FastWrite32(rdram, fe + 0x2C, 5u);
        Ps2FastWrite32(rdram, fe + 0x30, 6u);
        for (int p = 0; p < 4; ++p)
        {
            Ps2FastWrite32(rdram, fe + 0x34 + 4u * p,
                           p < count ? vendetta_wrestlers::choiceBase(uint32_t(g_setup.fighters[p].record)) : 0xFFFFFFFFu);
            Ps2FastWrite32(rdram, fe + 0x44 + 4u * p, p < count ? uint32_t(g_setup.fighters[p].attire) : 0u);
            Ps2FastWrite32(rdram, fe + 0x770 + 0x4Cu * p, 0u); // no profile: nothing is saved
        }
        Ps2FastWrite32(rdram, fe + 0x54, uint32_t(g_setup.arena));
        Ps2FastWrite8(rdram, kCpuLevel, uint8_t(level));
        Ps2FastWrite32(rdram, fe + 0x60, 0u);             // a fresh slide-in for the VS screen
        Ps2FastWrite32(rdram, fe + 0x08, 0x19u);
        Ps2FastWrite32(rdram, fe + 0x04, 0x19u);          // what Choose Stage leaves: the match setup next
        g_started = true;
        std::cerr << "[testmatch] straight into the match (" << (count == 4 ? "4-way" : "1-on-1") << "): records";
        for (int p = 0; p < count; ++p)
            std::cerr << (p ? ", " : " ") << g_setup.fighters[p].record << " + " << g_setup.fighters[p].attire;
        std::cerr << ", stage " << g_setup.arena << ", computer bits " << cpuBits << ", level " << level << std::endl;
        returnToCaller(ctx);
    }

    // ---- the computer players

    void brain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t player = getRegU32(ctx, 4);
        if (!g_started || player >= uint32_t(g_setup.count))
        {
            g_originalBrain(rdram, ctx, runtime);
            return;
        }
        const uint32_t b = kBrains + kBrainSize * player;
        if (g_setup.fighters[player].control == Control::Dummy)
        {
            // never presses: no attacks, no blocks, no reversals
            for (uint32_t off : {0x1Cu, 0x20u, 0x30u, 0x34u})
                Ps2FastWrite32(rdram, b + off, 0u);
            returnToCaller(ctx);
            return;
        }
        if (int(player) == g_setup.repeatFighter && g_repeatAction != 0)
        {
            const uint32_t action = Ps2FastRead32(rdram, b + 0x0C);
            const uint32_t cls = Ps2FastRead32(rdram, b + 0x44);
            const uint16_t where = Ps2FastRead16(rdram, b + 0xB4);
            if (action == 0u && (cls == 1u || cls == 10u) && (where & 1u) != 0u)
            {
                // FUN_0010fcf0's writes, then the plan
                const uint32_t f = kFighters + kFighterSize * player;
                const uint32_t opponent = Ps2FastRead32(rdram, f);
                Ps2FastWrite32(rdram, b + 0x0C, uint32_t(g_repeatAction));
                Ps2FastWrite32(rdram, b + 0x10, 0u);
                Ps2FastWrite32(rdram, b + 0x14, 0u);
                Ps2FastWrite32(rdram, b + 0x54, Ps2FastRead32(rdram, f + 0x40));
                if (opponent != 0u)
                    Ps2FastWrite32(rdram, b + 0x84, Ps2FastRead32(rdram, opponent + 0x40));
                const uint16_t flags = Ps2FastRead16(rdram, b + 0xB6);
                Ps2FastWrite16(rdram, b + 0xB6, uint16_t((flags & 0xFFECu) | 0x1u | 0x10u));
                Ps2FastWrite32(rdram, b + 0xD8, uint32_t(g_setup.repeatSlot));
                if (++g_throws <= 3 || g_throws % 20 == 0)
                    std::cerr << "[testmatch] player " << player << " goes for slot " << g_setup.repeatSlot << " (action "
                              << g_repeatAction << ", try " << g_throws << ")" << std::endl;
            }
        }
        g_originalBrain(rdram, ctx, runtime);
    }

    void listPick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t b = getRegU32(ctx, 4), list = getRegU32(ctx, 5), flags = getRegU32(ctx, 6);
        if (g_started && g_repeatFromList && g_setup.repeatFighter >= 0 && b == kBrains + kBrainSize * uint32_t(g_setup.repeatFighter)
            && (flags & 2u) == 0u && list < 16u)
        {
            const uint32_t at = b + 0x114u + 0x24u * list;
            const uint32_t count = std::min(Ps2FastRead32(rdram, at), 8u);
            for (uint32_t i = 0; i < count; ++i)
                if (int(Ps2FastRead32(rdram, at + 4u + 4u * i)) == g_setup.repeatSlot)
                {
                    setReturnU32(ctx, uint32_t(g_setup.repeatSlot));
                    returnToCaller(ctx);
                    return;
                }
        }
        g_originalListPick(rdram, ctx, runtime);
    }

    void clipStart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_started && g_setup.repeatFighter >= 0 && getRegU32(ctx, 4) == kFighters + kFighterSize * uint32_t(g_setup.repeatFighter))
        {
            const int32_t clip = static_cast<int32_t>(getRegU32(ctx, 5));
            // only in the fight (phases 1-3) and once the computer has gone for the slot: the intro's clips are move
            // clips too
            const uint32_t phase = Ps2FastRead32(rdram, kPhase);
            if (phase >= 1u && phase <= 3u && g_throws > 0 && clip >= 0 && clip < 10000 && (g_moveClip < 0 || clip == g_moveClip))
            {
                g_moveClip = clip;
                ++g_thrown;
                writeStatus();
                if (g_thrown <= 3 || g_thrown % 10 == 0)
                    std::cerr << "[testmatch] thrown " << g_thrown << " (clip " << clip << ")" << std::endl;
            }
        }
        g_originalClipStart(rdram, ctx, runtime);
    }

    // ---- the match: health, momentum, the loop

    // VENDETTA_TESTWIN: the human side played by the computer, the others held at a sliver of health (phases 1-3: the fight)
    void testWin(uint8_t *rdram)
    {
        const uint32_t phase = Ps2FastRead32(rdram, kPhase);
        if (phase < 1u || phase > 3u)
        {
            if (g_winKnown)
                ps2_stubs::padScriptHold(false);
            g_winKnown = false;
            return;
        }
        if (!g_winKnown)
        {
            g_winKnown = true;
            ps2_stubs::padScriptHold(true);
            g_winHumans = 0;
            for (uint32_t p = 0; p < 4u; ++p)
                if ((Ps2FastRead8(rdram, kFighters + kFighterSize * p + 0x5BC) & 3u) == 2u)
                    g_winHumans |= 1u << p;
        }
        for (uint32_t p = 0; p < 4u; ++p)
        {
            const uint32_t f = kFighters + kFighterSize * p;
            const uint8_t control = Ps2FastRead8(rdram, f + 0x5BC);
            if (!(control & 2u))
                continue;
            if (g_winHumans & (1u << p))
            {
                Ps2FastWrite8(rdram, f + 0x5BC, uint8_t(control | 1u));
                writeF(rdram, f + 0x3B8, 100.0f); // momentum full: the Blazin' move
            }
            else
            {
                const float full = readF(rdram, f + 0x3AC), sliver = full * 0.03f;
                if (full > 0.0f && readF(rdram, f + 0x3A8) > sliver)
                {
                    writeF(rdram, f + 0x3A8, sliver);
                    writeF(rdram, f + 0x3B0, sliver);
                }
            }
        }
    }

    void matchLogic(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_originalMatchLogic(rdram, ctx, runtime);
        if (g_testWin)
            testWin(rdram);
        if (!g_started)
            return;
        for (uint32_t p = 0; p < uint32_t(g_setup.count); ++p)
        {
            const uint32_t f = kFighters + kFighterSize * p;
            if (g_setup.fighters[p].keepHealth)
            {
                const float full = readF(rdram, f + 0x3AC);
                writeF(rdram, f + 0x3B0, full);
                writeF(rdram, f + 0x3A8, full);
            }
            if (int(p) == g_setup.repeatFighter)
            {
                const float momentum = readF(rdram, f + 0x3B8);
                if (g_repeatBlazin)
                    writeF(rdram, f + 0x3B8, 100.0f);
                else if (momentum > 90.0f)
                    writeF(rdram, f + 0x3B8, 90.0f);
            }
        }
        const uint32_t phase = Ps2FastRead32(rdram, kPhase);
        if (g_setup.loop && phase >= 7u && phase <= 10u)
        {
            // REMATCH, the GAME OVER menu's first item, without the menu (and no results screen)
            Ps2FastWrite8(rdram, kEndResults, uint8_t(Ps2FastRead8(rdram, kEndResults) & ~1u));
            Ps2FastWrite8(rdram, kEndMenu, uint8_t((Ps2FastRead8(rdram, kEndMenu) & ~1u) | 0x2u | 0x8u));
            if (phase == 8u)
                Ps2FastWrite32(rdram, kPhase, 9u);
        }
        const uint32_t rematches = Ps2FastRead32(rdram, kRematches);
        if (rematches != g_lastRematch)
        {
            g_lastRematch = rematches;
            g_moveClip = -1; // a mod move's clip id is given out per match: learned again
            writeStatus();
            std::cerr << "[testmatch] the match again (" << rematches << ")" << std::endl;
        }
    }

    template <typename Fn>
    bool hook(PS2Runtime &runtime, uint32_t address, PS2Runtime::RecompiledFunction &original, Fn replacement, const char *name)
    {
        original = runtime.lookupFunction(address);
        if (!original)
        {
            std::cerr << "[testmatch] " << name << " not found: off" << std::endl;
            return false;
        }
        runtime.replaceFunction(address, replacement);
        return true;
    }

    void install(PS2Runtime &runtime)
    {
        const char *win = std::getenv("VENDETTA_TESTWIN");
        g_testWin = win && win[0] == '1';
        const std::string file = askedFile();
        if (file.empty() || !readSetup(file, g_setup))
        {
            if (g_testWin && hook(runtime, kMatchLogic, g_originalMatchLogic, matchLogic, "FUN_0011a600"))
                std::cerr << "[testmatch] VENDETTA_TESTWIN: the human side played by the computer at full momentum, the others at 3 % health"
                          << std::endl;
            return;
        }
        if (!hook(runtime, kMoviePlayer, g_originalMovie, movie, "FUN_001bb3b8") ||
            !hook(runtime, kTitle, g_originalTitle, title, "FUN_00141bd0") ||
            !hook(runtime, kCpuBrain, g_originalBrain, brain, "FUN_00107470") ||
            !hook(runtime, kListPick, g_originalListPick, listPick, "FUN_00112f90") ||
            !hook(runtime, kMatchLogic, g_originalMatchLogic, matchLogic, "FUN_0011a600") ||
            !hook(runtime, kClipStart, g_originalClipStart, clipStart, "FUN_00132158"))
            return;
        g_statusPath = file + ".status";
        writeStatus();
        if (g_setup.repeatFighter >= 0)
        {
            actionFor(g_setup.repeatSlot);
            if (g_setup.fighters[g_setup.repeatFighter].control != Control::Cpu)
                std::cerr << "[testmatch] the repeat needs a computer player (\"control\": \"cpu\"): off" << std::endl, g_repeatAction = 0;
            else if (g_repeatAction == 0)
                std::cerr << "[testmatch] slot " << g_setup.repeatSlot
                          << " can't be thrown on repeat yet (grapples 1-17 / 21-25, strikes 46-57, taunts 86-88 / 90): the computer plays as usual"
                          << std::endl;
        }
        g_armed = true;
        std::cerr << "[testmatch] on: " << file << std::endl;
    }
}

void installVendettaTestMatch(PS2Runtime &runtime) { vendetta_testmatch::install(runtime); }
