// Def Jam Vendetta (USA, SLUS-20639) — files of the recomp's own that the game opens by name as if they were on its
// disc, and the standard songs they bring.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// The game never asks for a file by name at the time it wants it: at start its file code reads the disc's folders
// into one sorted table (the pointer at 0x26FFC8, the count at 0x26FF94; an entry is 0xC bytes + [0x26FF84] (256) of
// name: +0 the name's key, +4 the file's first sector, +8 its size in bytes, +0xC its name), and
// FUN_001f0aa0(path, mode, &handle) looks a name up in it (handle = the entry's number + 1). Everything after that
// goes by the handle: FUN_001f0c08 reads from the entry's sector, FUN_001f0e98 gives its size and name. A name not on
// the disc is then looked for in the VIV archives.
//
// So a file the disc doesn't have is given to the game here: FUN_001f0aa0 is answered for its name with an entry of
// ours, placed after the table's own (the table is moved into the store with room; its count stays, so the game's own
// search never meets our entries), whose sector is one the runtime's disc reader serves from the file on the PC
// (sceCdSearchFile gives every file under the game's folder such sectors). Ours are asked first, so a name the disc or
// an archive also has is ours.
//
// The files: every .mpf and .mus (but menu.mus) in game\standard\music\<a song's folder>\, answered as AUDIO\MUSIC\<its file name>
// (the names the game's music loader FUN_001c1e68 builds: <code>.MPF, MAI_<code>.MUS, OVL_<code>.MUS).
// VENDETTA_STANDARD=0: none. VENDETTA_DISCFILELOG=1: a line for each of ours opened; =2 every name the game looks up.
//
// Each song folder's song.json ({ "key", "code", "artist", "song", "album" }) and menu.mus make it a standard song
// (vendetta_discfiles.h); vendetta_music.cpp plays them: in a match the game's own music player, the picked game
// song's three names answered with the standard song's files (answerAs).
#include "vendetta_discfiles.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "vendetta_store.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace vendetta_discfiles
{
    namespace
    {
        using Fn = PS2Runtime::RecompiledFunction;

        constexpr uint32_t kFindOnDisc = 0x1F0AA0u; // FUN_001f0aa0(path, mode, &handle): 1 = found
        constexpr uint32_t kTable = 0x26FFC8u, kCount = 0x26FF94u, kNameBytes = 0x26FF84u;
        constexpr uint32_t kSearchFile = 0x1FD120u; // sceCdSearchFile(sceCdlFILE *, path)
        constexpr uint32_t kRoom = 64u;             // entries of ours the moved table has room for

        struct File
        {
            std::string name;     // as the game asks for it: "AUDIO/MUSIC/KROS.MPF" (upper case, forward slashes)
            std::string discPath; // for the runtime's disc reader: from the game's folder, "cdrom0:\standard\music\...;1"
            uint32_t entry = ~0u; // its entry's number in the moved table
        };

        uint8_t *g_rdram = nullptr;
        std::vector<File> g_files;
        std::vector<Song> g_songs;
        std::vector<std::pair<std::string, std::string>> g_aliases; // a name the game asks for -> the name answered
        Fn g_findOnDisc = nullptr, g_searchFile = nullptr;
        uint32_t g_table = 0, g_next = 0, g_scratch = 0; // the moved table, its next free entry, guest scratch memory
        int g_log = 0;

        uint32_t read32(uint32_t address)
        {
            uint32_t value = 0;
            std::memcpy(&value, g_rdram + (address & PS2_RAM_MASK), 4);
            return value;
        }
        void write32(uint32_t address, uint32_t value) { std::memcpy(g_rdram + (address & PS2_RAM_MASK), &value, 4); }

        // A path as the game's lookup compares it: upper case, forward slashes, no slash in front
        std::string plain(std::string text)
        {
            for (char &c : text)
                c = c == '\\' ? '/' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            while (!text.empty() && text.front() == '/')
                text.erase(text.begin());
            return text;
        }

        std::string guestString(uint32_t address)
        {
            std::string text;
            for (uint32_t i = 0; i < 255u; ++i)
            {
                const char c = static_cast<char>(g_rdram[(address + i) & PS2_RAM_MASK]);
                if (!c)
                    break;
                text.push_back(c);
            }
            return text;
        }

        // The game's table moved into the store with room after it (again if the game ever makes a new one)
        bool tableReady()
        {
            const uint32_t now = read32(kTable), count = read32(kCount), size = read32(kNameBytes) + 0xCu;
            if (now == 0u || count == 0u || count > 0x10000u || size > 0x400u)
                return false;
            if (now == g_table)
                return true;
            const uint32_t moved = vendetta_store::allocate(vendetta_store::Area::Boot, (count + kRoom) * size, "the disc's file table");
            if (!moved)
                return false;
            std::memcpy(g_rdram + (moved & PS2_RAM_MASK), g_rdram + (now & PS2_RAM_MASK), count * size);
            std::memset(g_rdram + ((moved + count * size) & PS2_RAM_MASK), 0, kRoom * size);
            write32(kTable, moved);
            g_table = moved;
            g_next = count;
            for (File &file : g_files)
                file.entry = ~0u;
            return true;
        }

        // The file's entry in the moved table (made at its first use): false when it can't be given
        bool place(File &file, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!tableReady())
            {
                if (g_log)
                    std::cerr << "[discfiles] " << file.name << ": the game's table can't be moved (at 0x" << std::hex << read32(kTable)
                              << std::dec << ", " << read32(kCount) << " entries of " << read32(kNameBytes) + 0xCu << " bytes)" << std::endl;
                return false;
            }
            if (file.entry != ~0u)
                return true;
            if (g_next - read32(kCount) >= kRoom)
                return false;
            if (!g_scratch)
                g_scratch = vendetta_store::allocate(vendetta_store::Area::Boot, 0x200u, "the disc's file table");
            if (!g_scratch || file.discPath.size() > 0x180u)
                return false;
            // the runtime's disc reader gives the file on the PC a run of sectors (sceCdlFILE: +0 the first, +4 the size)
            std::memset(g_rdram + (g_scratch & PS2_RAM_MASK), 0, 0x200u);
            std::memcpy(g_rdram + ((g_scratch + 0x40u) & PS2_RAM_MASK), file.discPath.c_str(), file.discPath.size() + 1u);
            const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5), v0 = getRegU32(ctx, 2), pc = ctx->pc;
            SET_GPR_U32(ctx, 4, g_scratch);
            SET_GPR_U32(ctx, 5, g_scratch + 0x40u);
            g_searchFile(g_rdram, ctx, runtime);
            const bool found = getRegU32(ctx, 2) != 0u;
            SET_GPR_U32(ctx, 4, a0);
            SET_GPR_U32(ctx, 5, a1);
            SET_GPR_U32(ctx, 2, v0);
            ctx->pc = pc;
            if (!found)
            {
                std::cerr << "[discfiles] " << file.name << ": the file isn't there any more (" << file.discPath << ")" << std::endl;
                return false;
            }
            const uint32_t size = read32(kNameBytes) + 0xCu, at = g_table + g_next * size;
            write32(at + 0x4u, read32(g_scratch));
            write32(at + 0x8u, read32(g_scratch + 4u));
            // (+0xC: its name as the table keeps one, "/AUDIO/MUSIC/KROS.MPF")
            const std::string kept = "/" + file.name;
            std::memcpy(g_rdram + ((at + 0xCu) & PS2_RAM_MASK), kept.c_str(), std::min<size_t>(kept.size(), size - 0xDu));
            file.entry = g_next++;
            if (g_log)
                std::cerr << "[discfiles] " << file.name << " = " << file.discPath << " (entry " << file.entry << ", sector 0x"
                          << std::hex << read32(g_scratch) << std::dec << ", " << read32(g_scratch + 4u) << " bytes)" << std::endl;
            return true;
        }

        void findOnDisc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            std::string asked = plain(guestString(getRegU32(ctx, 4)));
            if (g_log >= 2)
                std::cerr << "[discfiles] the game looks up " << asked << std::endl;
            // (a standard song loaded in a game song's place: that song's names mean the standard song's files)
            for (const auto &[from, to] : g_aliases)
                if (asked.size() >= from.size() && asked.compare(asked.size() - from.size(), std::string::npos, from) == 0)
                {
                    asked = to;
                    break;
                }
            for (File &file : g_files)
            {
                const bool same = asked == file.name ||
                                  (asked.size() > file.name.size() && asked.compare(asked.size() - file.name.size() - 1u, std::string::npos, "/" + file.name) == 0);
                if (!same || !place(file, ctx, runtime))
                    continue;
                if (const uint32_t out = getRegU32(ctx, 6))
                    write32(out, file.entry + 1u);
                setReturnU32(ctx, 1u);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            g_findOnDisc(rdram, ctx, runtime);
        }

        // "key": "value" out of a small JSON file's text
        std::string field(const std::string &json, const char *key)
        {
            const std::string want = std::string("\"") + key + "\"";
            size_t at = json.find(want);
            if (at == std::string::npos || (at = json.find(':', at + want.size())) == std::string::npos ||
                (at = json.find('"', at)) == std::string::npos)
                return {};
            std::string text;
            for (++at; at < json.size() && json[at] != '"'; ++at)
                text.push_back(json[at] == '\\' && at + 1 < json.size() ? json[++at] : json[at]);
            return text;
        }
    }

    const std::vector<Song> &songs() { return g_songs; }

    void answerAs(const std::string &gameCode, const std::string &ourCode)
    {
        g_aliases.clear();
        for (const char *form : {"%s.MPF", "MAI_%s.MUS", "OVL_%s.MUS"})
        {
            char from[64], to[64];
            std::snprintf(from, sizeof(from), form, plain(gameCode).c_str());
            std::snprintf(to, sizeof(to), form, plain(ourCode).c_str());
            g_aliases.emplace_back(std::string("AUDIO/MUSIC/") + from, std::string("AUDIO/MUSIC/") + to);
        }
    }

    void answerOwn() { g_aliases.clear(); }

    void install(PS2Runtime &runtime)
    {
        if (const char *off = std::getenv("VENDETTA_STANDARD"); off && off[0] == '0')
            return;
        if (const char *log = std::getenv("VENDETTA_DISCFILELOG"); log && *log)
            g_log = std::atoi(log);
        g_rdram = runtime.memory().getRDRAM();
        const std::filesystem::path game = PS2Runtime::getIoPaths().elfDirectory;
        std::error_code ec;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(game / "standard" / "music", std::filesystem::directory_options::skip_permission_denied, ec))
        {
            if (ec)
                break;
            const std::string extension = plain(entry.path().extension().string());
            if (!entry.is_regular_file() || (extension != ".MPF" && extension != ".MUS"))
                continue;
            if (plain(entry.path().filename().string()) == "MENU.MUS")
                continue; // the song's menu version: our player's, never asked for by the game
            File file;
            file.name = "AUDIO/MUSIC/" + plain(entry.path().filename().string());
            std::string relative = std::filesystem::relative(entry.path(), game, ec).string();
            std::replace(relative.begin(), relative.end(), '/', '\\');
            file.discPath = "cdrom0:\\" + relative + ";1";
            g_files.push_back(std::move(file));
            if (extension != ".MPF")
                continue;
            // its song.json beside it: the song box's words, the key a test names it by
            std::ifstream in(entry.path().parent_path() / "song.json", std::ios::binary);
            const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            Song song;
            song.code = plain(entry.path().stem().string());
            song.key = field(json, "key");
            song.artist = field(json, "artist");
            song.title = field(json, "song");
            song.album = field(json, "album");
            if (std::filesystem::is_regular_file(entry.path().parent_path() / "menu.mus", ec))
                song.menu = entry.path().parent_path() / "menu.mus";
            if (song.key.empty() || song.title.empty())
                std::cerr << "[songs] " << entry.path().parent_path().filename().string()
                          << ": no song.json with \"key\" and \"song\": not among the match songs" << std::endl;
            else
                g_songs.push_back(std::move(song));
        }
        if (g_files.empty())
            return;
        g_findOnDisc = runtime.lookupFunction(kFindOnDisc);
        g_searchFile = runtime.lookupFunction(kSearchFile);
        if (!g_findOnDisc || !g_searchFile || !runtime.replaceFunction(kFindOnDisc, findOnDisc))
        {
            std::cerr << "[discfiles] the game's routines were not found: the standard songs' files aren't given" << std::endl;
            g_files.clear();
            return;
        }
        std::cerr << "[discfiles] " << g_files.size() << " file(s) of the standard songs are answered as the disc's own" << std::endl;
        std::sort(g_songs.begin(), g_songs.end(), [](const Song &a, const Song &b) { return a.key < b.key; });
    }
}

void installVendettaDiscFiles(PS2Runtime &runtime) { vendetta_discfiles::install(runtime); }
