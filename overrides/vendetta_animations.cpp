// Def Jam Vendetta (USA, SLUS-20639) - body animations and moves from a mods\ folder, loaded per match.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// The game keeps every body animation ("clip") in two banks loaded whole at boot: bank 0 (0x1431580, 821 clips, ids
// 10000 + i) and bank 1 (0x1625580, 2,003 clips, ids i). Each bank is "PAK ", a size, a count, then an index of signed
// 32-bit offsets: clip i is at bank + 0xC + 4i + offset[i]. The code that plays clips (FUN_001b33b8, FUN_001b2620,
// FUN_001b2848, FUN_001b3460) builds the bank address from a literal and reads the index without checking the count,
// so a clip can be replaced by pointing its index entry at new bytes anywhere in memory.
//
// Per match: the game's own clips and moves stay as the boot loaded them; a mod's only go in for the match whose
// wrestlers use them, read from the mod's files at the match's start, so any number of mods can be installed (new clip
// ids stop at 4,095 because a move record holds its clip in 12 bits, FUN_0011c608: per match that is room for 2,093).
//  - Every match, in every mode (Battle, Survival, each story fight, REMATCH), starts with the match init
//    FUN_00119d70(rematch count), which wipes everything the game keeps about the last match (the four fighters, the
//    replay, the camera, the intro scenes, the sound engine's cache) and then reads the fighters' movesets itself. So
//    the match's mods go in just before it (hooked; the work is done before calling it, as it can wait). The records
//    are the match block's 0x236208 + 4p (-1 = no fighter); a story hero (records 0-11) fights with any of its three
//    records' movesets (picked inside the init from its stats), so all three are loaded.
//  - Each match load (FUN_00165838, hooked the same way) empties the store's match area (vendetta_store.h).
//  - A REMATCH (or another init with the same wrestlers in the same load) keeps what is loaded, unless one of the files
//    it came from changed: then it is read again (so a mod tool's test match shows an edit at the next rematch).
//  - Once, at the first match: the game's tables are made full size in the store's boot area: bank 1's index grows in
//    place to 4,096 entries (the game clips under it moved to the store), the timing records (FZUI: 38 bytes per clip,
//    the array at *(0x2331C8 + 0x24), stride *(0x2331C8 + 0x1C), read when a clip starts) copied with room for 4,096,
//    and the move records, names, camera paths and eye tracks likewise (vendetta_moves.cpp). Each match then only
//    changes their contents: put back to the game's own (a spare id points at the game's clip 0 and a copy of its
//    timing record, harmless if anything ever read one), then the match's mods written in.
// What a match's mods are (mod.json, see vendetta_mods.h):
//  - "wrestlers": a game wrestler's moveset slots ({ "84": "Jericho Entrance" }) for the match's wrestlers: a slot
//    naming a new move (vendetta_moves.h) gets that move; a slot naming a new animation gets a move of its own (a copy
//    of the slot's move with the clip changed, written after the new moves' records).
//  - "new": "animations": clips the game doesn't have, named by their file: those a match's slots name become bank 1
//    clips 2003 and up, then the new moves' animations. A new clip's timing record starts empty.
//  - "replace": "animations": game clips (by the names in the tables at 0x25A790 / 0x259AB8) get new bytes; the index
//    entry points at them. A replacement changes the clip everywhere the game uses it, so every replacement goes in at
//    every match; mods apply in load order, so a later mod's replacement of the same clip wins.
//  - sounds: an animation's settings file beside it (the same name, .json) lists its sound events: { "sounds": [
//    { "frame": 12, "sound": "Swing (whoosh)", "value": 3 } ] } (vendetta_clipsounds.cpp). A mod's animation, new or
//    replacing a game clip, plays exactly that list: without one it is silent.
// A mistake in a file leaves that one thing out of the match (a slot then keeps the game's move), with a message.
//
// VENDETTA_MODS=0        the plain game (mods\ ignored; the hooks pass straight through, nothing is changed)
// VENDETTA_MODLOG=1      every animation, move, slot and sound list each match puts in (vendetta_mods.h)
// VENDETTA_ANIMLOG=1     each clip a fighter starts (FUN_00132158: fighter in a0, clip id in a1), with its name
// VENDETTA_ANIMLIST=1    every clip at each match: id, bank, name, size, where it comes from
// VENDETTA_ANIMDUMP=a,b  those clips' bytes to logs\anim\<name>.bin (to make test mods)
// VENDETTA_ANIM_POISON=1 fills with 0xFF what nothing should read: the game data moved at boot, every spare id's
//                        stand-in, and at each match load the last match's mod data: if anything still read them, it
//                        would show (a test switch)
#include "ps2_runtime.h"
#include "game_overrides.h"
#include "runtime/ee_scheduler.h"
#include "vendetta_clipsounds.h"
#include "vendetta_mods.h"
#include "vendetta_moves.h"
#include "vendetta_store.h"
#include "vendetta_wrestlers.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace vendetta_animations
{
    constexpr uint32_t kBank0 = 0x1431580u;    // PAK 2: 821 clips, ids 10000 + i
    constexpr uint32_t kBank1 = 0x1625580u;    // PAK 3: 2,003 clips, ids i
    constexpr uint32_t kNames0 = 0x259AB8u;    // bank 0's clip names
    constexpr uint32_t kNames1 = 0x25A790u;    // bank 1's clip names
    constexpr uint32_t kWaza = vendetta_store::kMoveData; // the parsed move data (+0x08 = 999 move records), moved into the
                                                          // store with room for new wrestlers (vendetta_wrestlers.cpp)
    constexpr uint32_t kFzuiStride = 0x1Cu;    // ... + 0x1C: bytes per clip timing record (38)
    constexpr uint32_t kFzuiBank1 = 0x24u;     // ... + 0x24: bank 1's timing records (one per clip id)
    constexpr uint32_t kFzuiSize = 38u;
    constexpr uint32_t kMaxClipId = 4095u;     // a move record's clip field is 12 bits (FUN_0011c608)
    constexpr uint32_t kClipIds = kMaxClipId + 1u; // bank 1's index at its full size
    constexpr uint32_t kMaxRecord = 4095u;     // a moveset slot holds its move record in 12 bits
    constexpr uint32_t kRecords = 0x14u;       // ... + 0x14: the move records (stride + 0x04, count + 0x08)
    constexpr uint32_t kMovesets = 0x7Cu;      // ... + 0x7C + 16 x attire: that attire's moveset (128 u16 slots)
    constexpr uint32_t kCharacters = vendetta_store::kCharacterTable; // the character table: one 0x20 row per wrestler
                                                                     // attire (+0 / +4 names); new wrestlers from 63
    constexpr uint32_t kAttires = 59u;         // rows with a moveset (59-62 are the empty "Character A-D")
    constexpr uint32_t kHeroes = 12u;          // records 0-11: the story heroes, three records each (FUN_0013e958)
    constexpr uint32_t kSlots = 128u;
    constexpr uint32_t kReadSlots = 0x1E25F00u; // the game's 32 disc reads in flight (0 = free)
    constexpr uint32_t kPlayClip = 0x132158u;
    constexpr uint32_t kLoadMatch = 0x165838u;  // the match loader ("LoadGameData"): Battle, Survival, Story
    constexpr uint32_t kMatchInit = 0x119D70u;  // the match init (a0 = the rematch count): every match
    constexpr uint32_t kMatchRecords = 0x236208u; // the match block's four records (-1 = no fighter)
    constexpr uint32_t kExpectedCount0 = 821u;
    constexpr uint32_t kExpectedCount1 = 2003u;

    struct Clip
    {
        int bank = 0;
        uint32_t index = 0;
        uint32_t address = 0;
        uint32_t size = 0; // to the next clip (or the bank's end)
        std::string name;
        std::string from = "game";
    };

    uint8_t *g_rdram = nullptr;
    std::vector<Clip> g_clips[2];     // as this match has them
    std::vector<Clip> g_gameClips[2]; // the game's own, where they sit after the boot's moves
    PS2Runtime::RecompiledFunction g_originalPlayClip = nullptr;
    PS2Runtime::RecompiledFunction g_originalLoadMatch = nullptr;
    PS2Runtime::RecompiledFunction g_originalMatchInit = nullptr;

    uint32_t read32(uint32_t address)
    {
        uint32_t value = 0;
        std::memcpy(&value, g_rdram + (address & PS2_RAM_MASK), 4);
        return value;
    }
    uint16_t read16(uint32_t address)
    {
        uint16_t value = 0;
        std::memcpy(&value, g_rdram + (address & PS2_RAM_MASK), 2);
        return value;
    }
    void write32(uint32_t address, uint32_t value) { std::memcpy(g_rdram + (address & PS2_RAM_MASK), &value, 4); }
    uint8_t *guest(uint32_t address) { return g_rdram + (address & PS2_RAM_MASK); }

    std::string upper(std::string text)
    {
        for (char &c : text)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return text;
    }

    bool envOn(const char *name)
    {
        const char *value = std::getenv(name);
        return value && *value && *value != '0';
    }

    bool poisonOn()
    {
        static const bool on = envOn("VENDETTA_ANIM_POISON");
        return on;
    }

    // A guest C string (a clip name, a wrestler's name), or "" when the pointer doesn't lead to one.
    std::string guestString(uint32_t address)
    {
        if (address < 0x100000u || !vendetta_store::readable(address, 64u))
            return {};
        std::string text;
        for (uint32_t i = 0; i < 64u; ++i)
        {
            const char c = static_cast<char>(g_rdram[(address + i) & PS2_RAM_MASK]);
            if (c == 0)
                return text;
            if (static_cast<unsigned char>(c) < 0x20u || static_cast<unsigned char>(c) >= 0x7Fu)
                return {};
            text.push_back(c);
        }
        return {};
    }

    uint32_t bankBase(int bank) { return bank == 0 ? kBank0 : kBank1; }
    uint32_t clipAddress(int bank, uint32_t index)
    {
        const uint32_t entry = bankBase(bank) + 0xCu + 4u * index;
        return entry + read32(entry); // a signed offset: unsigned wrap-around does the subtraction
    }

    // Reads both banks' index and names (the game's own state; called after the boot loads).
    void readClips()
    {
        for (int bank = 0; bank < 2; ++bank)
        {
            const uint32_t base = bankBase(bank);
            const uint32_t count = read32(base + 8u);
            const uint32_t end = base + 8u + read32(base + 4u); // the size field counts from after the 8-byte "PAK " + size header
            std::vector<Clip> &clips = g_clips[bank];
            clips.assign(count, Clip{});
            for (uint32_t i = 0; i < count; ++i)
            {
                Clip &clip = clips[i];
                clip.bank = bank;
                clip.index = i;
                clip.address = clipAddress(bank, i);
                clip.name = guestString(read32((bank == 0 ? kNames0 : kNames1) + 4u * i));
            }
            // sizes: the distance to the next clip in memory (the last one to the bank's end)
            std::vector<uint32_t> starts;
            for (const Clip &clip : clips)
                starts.push_back(clip.address);
            std::sort(starts.begin(), starts.end());
            for (Clip &clip : clips)
            {
                const auto next = std::upper_bound(starts.begin(), starts.end(), clip.address);
                clip.size = (next != starts.end() ? *next : end) - clip.address;
            }
        }
    }

    // The banks and the move data look like the original disc's (mods are for the original disc only).
    bool looksOriginal(std::string &why)
    {
        if (std::memcmp(g_rdram + kBank0, "PAK ", 4) != 0 || std::memcmp(g_rdram + kBank1, "PAK ", 4) != 0)
            why = "an animation bank is missing";
        else if (read32(kBank0 + 8u) != kExpectedCount0 || read32(kBank1 + 8u) != kExpectedCount1)
            why = "the animation banks hold " + std::to_string(read32(kBank0 + 8u)) + " / " +
                  std::to_string(read32(kBank1 + 8u)) + " clips, not 821 / 2003";
        else if (read32(kWaza + 8u) != 999u)
            why = "the move data holds " + std::to_string(read32(kWaza + 8u)) + " moves, not 999";
        else if (read32(kWaza + kFzuiStride) != kFzuiSize || read32(kWaza + kFzuiBank1) - 0x100000u >= 0x1F00000u)
            why = "the clip timing records aren't laid out as in the original game";
        else if (upper(guestString(read32(kCharacters))) != "BRIGGS" || read32(kWaza + 4u) != 25u)
            why = "the character table or the move records aren't where the original game keeps them";
        else if (guestString(read32(kNames1)).empty() || guestString(read32(kNames0)).empty())
            why = "the clip name tables are not where the original game keeps them";
        else
            return true;
        return false;
    }

    std::filesystem::path projectRoot() { return PS2Runtime::getIoPaths().elfDirectory.parent_path(); }

    // ------------------------------------------------------------------------------------- the files a match read
    // Each file a match's mods came from, as it was then: a rematch reads them again only if one changed.
    struct Stamp
    {
        std::filesystem::path path;
        bool exists = false;
        uintmax_t size = 0;
        std::filesystem::file_time_type time{};
    };
    std::vector<Stamp> g_stamps;

    Stamp stampOf(const std::filesystem::path &path)
    {
        Stamp stamp;
        stamp.path = path;
        std::error_code ec;
        stamp.exists = std::filesystem::is_regular_file(path, ec);
        if (stamp.exists)
        {
            stamp.size = std::filesystem::file_size(path, ec);
            stamp.time = std::filesystem::last_write_time(path, ec);
        }
        return stamp;
    }

    void track(const std::filesystem::path &path) { g_stamps.push_back(stampOf(path)); }

    // the first file that changed since the match read it, or an empty path
    std::filesystem::path changedFile()
    {
        for (const Stamp &was : g_stamps)
        {
            const Stamp now = stampOf(was.path);
            if (now.exists != was.exists || (now.exists && (now.size != was.size || now.time != was.time)))
                return was.path;
        }
        return {};
    }

    bool readFile(const std::filesystem::path &path, std::vector<uint8_t> &bytes)
    {
        track(path);
        return vendetta_mods::readWhole(path, bytes);
    }

    // The checks a clip file must pass (the full layout needs the BODY skeleton: that stays the converter's job).
    bool clipLooksValid(const std::vector<uint8_t> &bytes, std::string &why)
    {
        if (bytes.size() < 3u)
        {
            why = "shorter than a clip header";
            return false;
        }
        const uint32_t lastFrame = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8u);
        const uint32_t poseWords = bytes[2];
        if (lastFrame >= 0x7FFu)
        {
            why = "its last frame (" + std::to_string(lastFrame) + ") is past what the game's timing fields hold (2046)";
            return false;
        }
        if (3u + poseWords * 6u > bytes.size())
        {
            why = "its pose words run past the end of the file";
            return false;
        }
        return true;
    }

    uint32_t lastFrameOf(const std::vector<uint8_t> &bytes)
    {
        return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8u);
    }

    // A clip file from a mod, read and checked once a match however many times it's named; its bytes go into the store's
    // match area on first use (placed 8 bytes past a 16-byte boundary, like the game's own clips).
    struct ModClip
    {
        std::string modName;
        std::string fileName;
        std::vector<uint8_t> bytes;
        uint32_t address = 0; // in the store, once placed
        bool hasSounds = false; // its settings file has a "sounds" list
        std::vector<vendetta_clipsounds::Event> sounds;
    };

    std::map<std::filesystem::path, ModClip> g_modClips; // this match's

    ModClip *loadModClip(const vendetta_mods::Mod &mod, const std::string &relative, const std::string &what)
    {
        const std::filesystem::path file = vendetta_mods::modFile(mod, relative, "animation");
        if (file.empty())
            return nullptr;
        const auto known = g_modClips.find(file);
        if (known != g_modClips.end())
            return &known->second;
        ModClip clip;
        clip.modName = mod.name;
        clip.fileName = file.filename().string();
        std::string why;
        if (!readFile(file, clip.bytes) || !clipLooksValid(clip.bytes, why))
        {
            vendetta_mods::problem() << "[mods] " << mod.name << ": " << what << " " << relative << " left out: "
                      << (why.empty() ? "can't be read" : why) << std::endl;
            return nullptr;
        }
        // its settings beside it: the same name, .json
        std::filesystem::path settingsFile = file;
        settingsFile.replace_extension(".json");
        track(settingsFile);
        std::error_code ec;
        if (std::filesystem::is_regular_file(settingsFile, ec))
        {
            std::vector<uint8_t> text;
            vendetta_mods::Json settings;
            const std::string where = mod.name + ": " + settingsFile.filename().string();
            if (!readFile(settingsFile, text) ||
                !vendetta_mods::parseJson(std::string(text.begin(), text.end()), settings, why) || !settings.isObject())
                vendetta_mods::problem() << "[mods] " << where << " can't be read" << (why.empty() ? "" : ": " + why)
                          << " - the animation is used without it" << std::endl;
            else if (const vendetta_mods::Json *sounds = settings.find("sounds"))
            {
                clip.hasSounds = true;
                clip.sounds = vendetta_clipsounds::readSounds(*sounds, where, lastFrameOf(clip.bytes));
            }
        }
        return &g_modClips.emplace(file, std::move(clip)).first->second;
    }

    bool placeModClip(ModClip &clip)
    {
        if (clip.address == 0u)
        {
            clip.address = vendetta_store::allocate(vendetta_store::Area::Match, static_cast<uint32_t>(clip.bytes.size()),
                                                    "animations", 16u, 8u);
            if (clip.address != 0u)
                std::memcpy(vendetta_store::host(clip.address), clip.bytes.data(), clip.bytes.size());
        }
        return clip.address != 0u;
    }

    void pointIndexAt(int bank, uint32_t index, uint32_t address)
    {
        const uint32_t entry = bankBase(bank) + 0xCu + 4u * index;
        write32(entry, address - entry); // self-relative; unsigned wrap-around makes it signed
    }

    // ------------------------------------------------------------------------------------------- once, at boot
    // Byte ranges no index entry uses any more, for VENDETTA_ANIM_POISON.
    std::vector<std::pair<uint32_t, uint32_t>> g_unused;

    // Bank 1's index grows in place by 4 bytes per new clip: the game clips it would cover move to the store first.
    // Returns false (nothing changed) if they can't.
    bool growBank1Index(uint32_t newCount, uint32_t &moved)
    {
        std::vector<Clip> &clips = g_clips[1];
        const uint32_t indexEnd = kBank1 + 0xCu + 4u * newCount;
        std::vector<std::pair<Clip *, uint32_t>> moves;
        for (Clip &clip : clips)
        {
            if (clip.address >= indexEnd || clip.address < kBank1)
                continue;
            const uint32_t to = vendetta_store::allocate(vendetta_store::Area::Boot, clip.size, "game clips moved for the index",
                                                         16u, 8u);
            if (to == 0u)
                return false;
            moves.push_back({&clip, to});
        }
        for (auto &[clip, to] : moves)
        {
            std::memcpy(vendetta_store::host(to), g_rdram + clip->address, clip->size);
            pointIndexAt(1, clip->index, to);
            if (clip->address + clip->size > indexEnd)
                g_unused.push_back({indexEnd, clip->address + clip->size});
            clip->address = to;
        }
        moved = static_cast<uint32_t>(moves.size());
        return true;
    }

    // Bank 1's timing records (FZUI), copied to the store with room for the new clips.
    bool growBank1Timing(uint32_t newCount)
    {
        const uint32_t oldArray = read32(kWaza + kFzuiBank1);
        const uint32_t oldBytes = kExpectedCount1 * kFzuiSize;
        const uint32_t array = vendetta_store::allocate(vendetta_store::Area::Boot, newCount * kFzuiSize,
                                                        "animation timing records", 16u, 0u);
        if (array == 0u)
            return false;
        uint8_t *to = vendetta_store::host(array);
        std::memcpy(to, g_rdram + oldArray, oldBytes);
        std::memset(to + oldBytes, 0, (newCount - kExpectedCount1) * kFzuiSize);
        write32(kWaza + kFzuiBank1, array);
        g_unused.push_back({oldArray, oldArray + oldBytes});
        return true;
    }

    // A wrestler attire's moveset: 128 u16 slots (flags in bits 12-15, the move record in bits 0-11; 0xFFFF = empty),
    // pointed to by the WAZA struct at +0x7C + 16 x record (record = the attire's row in the character table).
    uint32_t movesetAddress(uint32_t attire)
    {
        uint32_t address = read32(kWaza + kMovesets + 16u * attire);
        if (std::memcmp(g_rdram + (address & PS2_RAM_MASK), "WWAZ", 4) == 0)
            address += 8u; // the block's tag and size
        return address;
    }

    // The attires with a moveset: the game's 0-58 and the new wrestlers' from 63 (vendetta_wrestlers.cpp).
    bool hasMoveset(uint32_t attire)
    {
        return attire < kAttires || (attire >= vendetta_wrestlers::kFirstNewRecord && attire < vendetta_wrestlers::endRecord());
    }

    std::vector<std::array<uint8_t, 2u * kSlots>> g_gameMovesets; // every attire's slots as the game has them (a new
                                                                   // attire's: as its moveset.json gives them at start-up)
    uint32_t g_standIn = 0;                                        // where a spare clip id points
    std::array<uint8_t, kFzuiSize> g_standInTiming{};              // and its timing record
    uint32_t g_poisonClip = 0;                                     // VENDETTA_ANIM_POISON: 64 bytes of 0xFF

    // VENDETTA_ANIM_POISON=1: the bytes the boot's moves made unused are filled with 0xFF - unless a clip still in use
    // overlaps them (two names can share one clip's bytes).
    void poisonUnused()
    {
        if (!poisonOn())
            return;
        uint32_t bytes = 0;
        for (const auto &[from, to] : g_unused)
        {
            bool inUse = false;
            for (const std::vector<Clip> &clips : g_clips)
                for (const Clip &clip : clips)
                    if (clip.address < to && clip.address + clip.size > from)
                        inUse = true;
            if (inUse)
                continue;
            std::memset(g_rdram + from, 0xFF, to - from);
            bytes += to - from;
        }
        std::cerr << "[anim] VENDETTA_ANIM_POISON: " << bytes << " unused bytes filled with 0xFF (" << g_unused.size()
                  << " ranges)" << std::endl;
    }

    void dumpClips()
    {
        const char *list = std::getenv("VENDETTA_ANIMDUMP");
        if (!list || !*list)
            return;
        const std::filesystem::path folder = projectRoot() / "logs" / "anim";
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        std::string names = list;
        size_t start = 0;
        while (start <= names.size())
        {
            const size_t comma = names.find(',', start);
            const std::string want = upper(names.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            start = comma == std::string::npos ? names.size() + 1 : comma + 1;
            bool found = false;
            for (const std::vector<Clip> &clips : g_clips)
                for (const Clip &clip : clips)
                    if (!found && upper(clip.name) == want)
                    {
                        found = true;
                        std::ofstream out(folder / (clip.name + ".bin"), std::ios::binary);
                        out.write(reinterpret_cast<const char *>(g_rdram + clip.address), clip.size);
                        std::cerr << "[anim] dumped " << clip.name << " (" << clip.size << " bytes) to logs\\anim\\" << std::endl;
                    }
            if (!found && !want.empty())
                std::cerr << "[anim] VENDETTA_ANIMDUMP: no clip named " << want << std::endl;
        }
    }

    void listClips()
    {
        for (const std::vector<Clip> &clips : g_clips)
            for (const Clip &clip : clips)
                std::cerr << "[anim] id " << (clip.bank == 0 ? 10000u + clip.index : clip.index) << " bank " << clip.bank
                          << " " << (clip.name.empty() ? "?" : clip.name) << " " << clip.size << " bytes, "
                          << read16(clip.address) + 1u << " frames, from " << clip.from << std::endl;
    }

    // Both banks loaded; `strict`: and no disc read of the game's in flight (so neither bank is half-read).
    bool banksReady(bool strict)
    {
        if (std::memcmp(g_rdram + kBank0, "PAK ", 4) != 0 || std::memcmp(g_rdram + kBank1, "PAK ", 4) != 0)
            return false;
        if (strict)
            for (uint32_t slot = 0; slot < 32u; ++slot)
                if (read32(kReadSlots + 4u * slot) != 0u)
                    return false;
        return true;
    }

    enum class State
    {
        Waiting, // the tables not made full size yet
        Ready,
        Off, // no mods, or not the original disc's data
    };
    State g_state = State::Waiting;

    // Once, at the first match load (or init): check the data is the original disc's, then make the tables full size.
    bool ready(bool strict)
    {
        if (g_state != State::Waiting)
            return g_state == State::Ready;
        if (vendetta_mods::loadedMods().empty())
        {
            g_state = State::Off; // the plain game: nothing touched
            return false;
        }
        if (!banksReady(strict))
        {
            if (!strict)
                std::cerr << "[mods] the animation banks aren't in memory at the match's start: no mods in this match" << std::endl;
            return false;
        }
        std::string why;
        if (!looksOriginal(why))
        {
            vendetta_mods::problem() << "[mods] the game data doesn't look like the original disc's (" << why
                      << "); mods are off: they need the original Def Jam Vendetta (USA) disc" << std::endl;
            g_state = State::Off;
            return false;
        }
        readClips();
        dumpClips(); // the game's own bytes, before any mod
        uint32_t moved = 0;
        if (!growBank1Index(kClipIds, moved) || !growBank1Timing(kClipIds))
        {
            vendetta_mods::problem() << "[mods] no room in the store for the animation tables: mods are off" << std::endl;
            g_state = State::Off;
            return false;
        }
        g_gameClips[0] = g_clips[0];
        g_gameClips[1] = g_clips[1];
        g_standIn = g_clips[1][0].address;
        std::memcpy(g_standInTiming.data(), guest(read32(kWaza + kFzuiBank1)), kFzuiSize);
        if (poisonOn())
        {
            g_poisonClip = vendetta_store::allocate(vendetta_store::Area::Boot, 64u, "stand-ins (poison test)", 16u, 8u);
            if (g_poisonClip)
                std::memset(guest(g_poisonClip), 0xFF, 64u);
        }
        g_gameMovesets.resize(vendetta_wrestlers::endRecord());
        for (uint32_t attire = 0; attire < vendetta_wrestlers::endRecord(); ++attire)
            if (hasMoveset(attire))
                std::memcpy(g_gameMovesets[attire].data(), guest(movesetAddress(attire)), 2u * kSlots);
        vendetta_moves::prepare(g_rdram); // the move records, names, camera paths and eye tracks at full size
        poisonUnused();
        g_state = State::Ready;
        std::cerr << "[mods] " << vendetta_mods::loadedMods().size() << " mod(s); their animations and moves go in at each "
                  << "match, for its wrestlers (" << moved << " game clips moved for bank 1's index of " << kClipIds << ")"
                  << std::endl;
        vendetta_store::report();
        return true;
    }

    // --------------------------------------------------------------------------------------------- each match
    // Everything back to the game's own: the clip index, the spare ids, the timing records, the movesets, the moves'
    // tables, the sounds.
    void resetToGame()
    {
        for (int bank = 0; bank < 2; ++bank)
        {
            for (const Clip &clip : g_gameClips[bank])
                pointIndexAt(bank, clip.index, clip.address);
            g_clips[bank] = g_gameClips[bank];
        }
        uint8_t *fzui = guest(read32(kWaza + kFzuiBank1));
        for (uint32_t id = kExpectedCount1; id < kClipIds; ++id)
        {
            const bool poison = poisonOn() && g_poisonClip;
            pointIndexAt(1, id, poison ? g_poisonClip : g_standIn);
            if (poison)
                std::memset(fzui + id * kFzuiSize, 0xFF, kFzuiSize);
            else
                std::memcpy(fzui + id * kFzuiSize, g_standInTiming.data(), kFzuiSize);
        }
        write32(kBank1 + 8u, kExpectedCount1);
        for (uint32_t attire = 0; attire < vendetta_wrestlers::endRecord(); ++attire)
            if (hasMoveset(attire))
                std::memcpy(guest(movesetAddress(attire)), g_gameMovesets[attire].data(), 2u * kSlots);
        vendetta_moves::reset();
        vendetta_clipsounds::reset();
        g_modClips.clear();
        g_stamps.clear();
    }

    // The rows (one per wrestler attire, with a moveset) of the wrestler called `name` (any case): a game wrestler by
    // the game's own names, a new one by its folder's or any attire's names (vendetta_wrestlers::recordsNamed).
    std::vector<uint32_t> attiresOf(const std::string &name)
    {
        std::vector<uint32_t> rows;
        for (uint32_t row : vendetta_wrestlers::recordsNamed(name))
            if (hasMoveset(row))
                rows.push_back(row);
        return rows;
    }

    // A slot a mod's "wrestlers" file sets: { "84": "Jericho Entrance" } - a new move or a new animation by its name,
    // "game move <number> <name>", "none", or { "move": ..., "flags": 0-15 } (the slot's bits 12-15; else kept).
    struct SlotChange
    {
        std::string value;
        std::string modName;
        std::string wrestler;
        int flags = -1;
        bool base = false; // from a new wrestler's own moveset.json (edits go on top of it)
    };

    // The number in "game move 56 CEMENT HANDS" (the name after it is for people), or -1.
    int gameMoveNumber(const std::string &value)
    {
        const std::string want = "GAME MOVE ";
        if (value.size() <= want.size() || upper(value.substr(0, want.size())) != want)
            return -1;
        char *end = nullptr;
        const long n = std::strtol(value.c_str() + want.size(), &end, 10);
        return end != value.c_str() + want.size() && n >= 0 && n < 999 ? static_cast<int>(n) : -1;
    }
    using SlotChanges = std::map<std::pair<uint32_t, uint32_t>, SlotChange>; // (attire, slot) -> what goes there

    // The match's attires' slots: first each new wrestler's own moveset.json (all its slots: the base), then every mod's
    // "wrestlers": { "<wrestler>": "<file>" } edits on top (only the listed slots change; a later mod's win).
    SlotChanges readSlotChanges(const std::set<uint32_t> &matchAttires)
    {
        SlotChanges changes;
        std::map<std::tuple<std::string, std::string, std::string>, uint32_t> clashes; // (mod, wrestler, earlier mod) -> slots
        // One slot file for one wrestler: its slots for the attires in this match.
        const auto readSlotFile = [&](const std::string &modName, const std::string &wrestler, const std::filesystem::path &file,
                                      const std::vector<uint32_t> &all, bool base)
        {
            std::vector<uint32_t> attires;
            for (uint32_t attire : all)
                if (matchAttires.count(attire))
                    attires.push_back(attire);
            if (attires.empty())
                return; // not in this match
            std::vector<uint8_t> bytes;
            vendetta_mods::Json moveset;
            std::string error;
            if (file.empty() || !readFile(file, bytes) ||
                !vendetta_mods::parseJson(std::string(bytes.begin(), bytes.end()), moveset, error) || !moveset.isObject())
            {
                vendetta_mods::problem() << "[mods] " << modName << ": " << wrestler << "'s file can't be read"
                          << (error.empty() ? "" : ": " + error) << " - left out" << std::endl;
                return;
            }
            for (const auto &[slotText, value] : moveset.members)
            {
                char *end = nullptr;
                const long slot = std::strtol(slotText.c_str(), &end, 10);
                if (slotText.empty() || *end != '\0' || slot < 0 || slot >= static_cast<long>(kSlots))
                {
                    vendetta_mods::problem() << "[mods] " << modName << ": " << wrestler << ": \"" << slotText
                              << "\" isn't a moveset slot (0-127) - skipped" << std::endl;
                    continue;
                }
                const vendetta_mods::Json *move = value.isObject() ? value.find("move") : &value;
                const vendetta_mods::Json *flags = value.isObject() ? value.find("flags") : nullptr;
                if (!move || !move->isString() ||
                    (flags && (flags->type != vendetta_mods::Json::Type::Number || flags->number < 0 || flags->number > 15)))
                {
                    vendetta_mods::problem() << "[mods] " << modName << ": " << wrestler << " slot " << slot
                              << ": the move should be a name in quotes (or { \"move\": ..., \"flags\": 0-15 }) - skipped" << std::endl;
                    continue;
                }
                for (uint32_t attire : attires)
                {
                    SlotChange &change = changes[{attire, static_cast<uint32_t>(slot)}];
                    if (!change.modName.empty() && !change.base && change.modName != modName && attire == attires.front())
                        ++clashes[{modName, wrestler, change.modName}];
                    change = {move->text, modName, wrestler, flags ? static_cast<int>(flags->number) : -1, base};
                }
            }
        };
        // the base: each new wrestler's own moveset.json
        for (const vendetta_wrestlers::SlotFile &own : vendetta_wrestlers::slotFiles())
            readSlotFile(own.modName, own.wrestler, own.file, own.records, true);
        // the edits on top, in load order
        for (const vendetta_mods::Mod &mod : vendetta_mods::loadedMods())
        {
            const vendetta_mods::Json *wrestlers = mod.manifest.find("wrestlers");
            if (!wrestlers)
                continue;
            if (!wrestlers->isObject())
            {
                vendetta_mods::problem() << "[mods] " << mod.name << ": \"wrestlers\" should be { \"<wrestler>\": \"<file>\" }" << std::endl;
                continue;
            }
            for (const auto &[wrestler, fileValue] : wrestlers->members)
            {
                const std::vector<uint32_t> all = attiresOf(wrestler);
                if (all.empty())
                {
                    vendetta_mods::problem() << "[mods] " << mod.name << ": the game has no wrestler called \"" << wrestler << "\" - skipped"
                              << std::endl;
                    continue;
                }
                const std::filesystem::path file =
                    fileValue.isString() ? vendetta_mods::modFile(mod, fileValue.text, "wrestler") : std::filesystem::path();
                readSlotFile(mod.name, wrestler, file, all, false);
            }
        }
        for (const auto &[who, slots] : clashes)
            vendetta_mods::problem() << "[mods] " << std::get<0>(who) << ": " << std::get<1>(who) << ": " << slots << " slot(s) also set by "
                      << std::get<2>(who) << " - " << std::get<0>(who) << "'s are used (later in the load order)" << std::endl;
        return changes;
    }

    // A new clip from a mod: its name (the file's), the file.
    struct NewClip
    {
        std::string name;
        ModClip *clip = nullptr;
        int moveClip = -1; // one of a new move's animations (vendetta_moves), its place in that list
    };

    // The mods' "new" animations the match's slots name (upper-case names in `wanted`), in load order; a name added again
    // by a later mod replaces the earlier one.
    std::vector<NewClip> collectNewClips(const std::unordered_map<std::string, std::vector<Clip *>> &gameNames,
                                         const std::set<std::string> &wanted)
    {
        std::vector<NewClip> added;
        std::unordered_map<std::string, size_t> byName;
        for (const vendetta_mods::Mod &mod : vendetta_mods::loadedMods())
        {
            const vendetta_mods::Json *section = mod.manifest.find("new");
            const vendetta_mods::Json *list = section ? section->find("animations") : nullptr;
            if (!list)
                continue;
            if (!list->isArray())
            {
                vendetta_mods::problem() << "[mods] " << mod.name << ": \"new\": \"animations\" should be a list of files: [\"animations/<name>.bin\"]"
                          << std::endl;
                continue;
            }
            for (const vendetta_mods::Json &item : list->items)
            {
                if (!item.isString())
                {
                    vendetta_mods::problem() << "[mods] " << mod.name << ": new animations: each file should be a name in quotes" << std::endl;
                    continue;
                }
                const std::string name = std::filesystem::path(item.text).stem().string();
                const std::string key = upper(name);
                if (!wanted.count(key))
                    continue;
                ModClip *clip = loadModClip(mod, item.text, "new animation");
                if (!clip)
                    continue;
                if (gameNames.count(key))
                    vendetta_mods::problem() << "[mods] " << mod.name << ": new animation \"" << name
                              << "\" has the name of one of the game's own clips - to change the game's, use \"replace\""
                              << std::endl;
                const auto earlier = byName.find(key);
                if (earlier != byName.end())
                {
                    NewClip &was = added[earlier->second];
                    if (was.clip != clip)
                        vendetta_mods::problem() << "[mods] " << mod.name << ": new animation \"" << name << "\" was also added by "
                                  << was.clip->modName << " - " << mod.name << "'s is used (later in the load order)" << std::endl;
                    was.clip = clip;
                    continue;
                }
                byName.emplace(key, added.size());
                added.push_back({name, clip});
            }
        }
        return added;
    }

    struct Counts
    {
        uint32_t added = 0, replaced = 0, skipped = 0, slots = 0;
    };

    // The match's clips: its new animations and its new moves' animations as clips 2003 and up, then every replacement.
    void applyClips(const std::set<std::string> &moveNames, const std::set<std::string> &animationNames, Counts &counts)
    {
        // the game's own clips by name (only those: "replace" is for the game's clips)
        std::unordered_map<std::string, std::vector<Clip *>> byName;
        const auto indexGameNames = [&byName]()
        {
            byName.clear();
            for (int bank = 0; bank < 2; ++bank)
            {
                const uint32_t count = bank == 0 ? kExpectedCount0 : kExpectedCount1;
                for (uint32_t i = 0; i < count && i < g_clips[bank].size(); ++i)
                    if (!g_clips[bank][i].name.empty())
                        byName[upper(g_clips[bank][i].name)].push_back(&g_clips[bank][i]);
            }
        };
        indexGameNames();

        const uint32_t room = kClipIds - kExpectedCount1;
        std::vector<NewClip> added = collectNewClips(byName, animationNames);
        if (added.size() > room)
        {
            vendetta_mods::problem() << "[mods] new animations: " << added.size() << " in this match, " << room
                      << " fit (the numbers stop at 4,095) - the rest left out" << std::endl;
            counts.skipped += static_cast<uint32_t>(added.size() - room);
            added.resize(room);
        }
        // the new moves this match uses, as many as the room left holds; their animations after the new ones, each
        // move's in a row (vendetta_moves.h)
        vendetta_moves::load(moveNames, room - static_cast<uint32_t>(added.size()));
        const std::vector<vendetta_moves::ClipToAdd> moveClips = vendetta_moves::clipsToAdd();
        for (const std::filesystem::path &file : vendetta_moves::filesRead())
            track(file);
        for (size_t i = 0; i < moveClips.size(); ++i)
        {
            ModClip clip;
            clip.modName = moveClips[i].from;
            clip.fileName = moveClips[i].name;
            clip.bytes = *moveClips[i].bytes;
            clip.hasSounds = true; // a move's animation plays exactly its settings file's sounds (none = silent)
            clip.sounds = *moveClips[i].sounds;
            ModClip &kept = g_modClips[std::filesystem::path("move clip " + std::to_string(i))] = std::move(clip);
            added.push_back({moveClips[i].name, &kept, static_cast<int>(i)});
        }
        uint8_t *fzui = guest(read32(kWaza + kFzuiBank1));
        std::vector<Clip> &clips = g_clips[1];
        for (const NewClip &item : added)
        {
            if (clips.size() >= kClipIds || !placeModClip(*item.clip))
            {
                ++counts.skipped;
                continue;
            }
            Clip clip;
            clip.bank = 1;
            clip.index = static_cast<uint32_t>(clips.size());
            clip.address = item.clip->address;
            clip.size = static_cast<uint32_t>(item.clip->bytes.size());
            clip.name = item.name;
            clip.from = item.clip->modName;
            pointIndexAt(1, clip.index, clip.address);
            std::memset(fzui + clip.index * kFzuiSize, 0, kFzuiSize); // empty until a move sets it
            const std::string soundName = vendetta_clipsounds::nameNewClip(clip.index);
            if (vendetta_mods::detailLog())
                std::cerr << "[mods] " << clip.from << ": added " << clip.name << " as clip " << clip.index << " ("
                          << lastFrameOf(item.clip->bytes) + 1u << " frames, " << clip.size << " bytes, sounds as " << soundName
                          << ")" << std::endl;
            if (item.clip->hasSounds)
                vendetta_clipsounds::setSounds(soundName, item.clip->sounds, clip.from + " (" + clip.name + ")");
            if (item.moveClip >= 0)
                vendetta_moves::clipNumbered(static_cast<size_t>(item.moveClip), clip.index);
            clips.push_back(clip);
            ++counts.added;
        }
        write32(kBank1 + 8u, static_cast<uint32_t>(clips.size()));
        indexGameNames(); // adding can move the list

        for (const vendetta_mods::Mod &mod : vendetta_mods::loadedMods())
        {
            const vendetta_mods::Json *replace = mod.manifest.find("replace");
            const vendetta_mods::Json *animations = replace ? replace->find("animations") : nullptr;
            if (!animations)
                continue;
            if (!animations->isObject())
            {
                vendetta_mods::problem() << "[mods] " << mod.name << ": \"replace\": \"animations\" should be { \"<game clip>\": \"<file>\" }"
                          << std::endl;
                continue;
            }
            for (const auto &[gameName, fileValue] : animations->members)
            {
                const auto found = byName.find(upper(gameName));
                if (found == byName.end())
                {
                    vendetta_mods::problem() << "[mods] " << mod.name << ": replace: the game has no animation called \"" << gameName
                              << "\" - skipped" << std::endl;
                    ++counts.skipped;
                    continue;
                }
                if (!fileValue.isString())
                {
                    vendetta_mods::problem() << "[mods] " << mod.name << ": replace " << gameName << ": the file should be a name in quotes"
                              << std::endl;
                    ++counts.skipped;
                    continue;
                }
                ModClip *modClip = loadModClip(mod, fileValue.text, "replace " + gameName + ":");
                if (!modClip || !placeModClip(*modClip))
                {
                    ++counts.skipped;
                    continue;
                }
                for (Clip *clip : found->second)
                {
                    const uint32_t oldLastFrame = read16(clip->address);
                    if (clip->from != "game")
                        std::cerr << "[mods] " << mod.name << ": " << clip->name << " was already replaced by " << clip->from
                                  << " - " << mod.name << "'s is used (later in the load order)" << std::endl;
                    pointIndexAt(clip->bank, clip->index, modClip->address);
                    if (vendetta_mods::detailLog())
                        std::cerr << "[mods] " << mod.name << ": replaced " << clip->name << " (bank " << clip->bank << " clip "
                                  << clip->index << ", id " << (clip->bank == 0 ? 10000u + clip->index : clip->index) << ", "
                                  << oldLastFrame + 1u << " frames) with " << modClip->fileName << " ("
                                  << lastFrameOf(modClip->bytes) + 1u << " frames, " << modClip->bytes.size() << " bytes)"
                                  << std::endl;
                    // only the file's sounds: none listed = silent
                    vendetta_clipsounds::setSounds(clip->name, modClip->sounds, mod.name + " (" + modClip->fileName + ")");
                    clip->address = modClip->address;
                    clip->size = static_cast<uint32_t>(modClip->bytes.size());
                    clip->from = mod.name;
                    ++counts.replaced;
                }
            }
        }
    }

    // A move record field (25 bytes, bit-packed MSB-first, FUN_0011c608): field 28 = the clip, at bit 163, 12 bits.
    constexpr uint32_t kClipBit = 163u, kClipBits = 12u;

    uint32_t recordField(const uint8_t *record, uint32_t bit, uint32_t bits)
    {
        uint32_t value = 0;
        for (uint32_t i = 0; i < bits; ++i, ++bit)
            value = (value << 1u) | ((record[bit >> 3u] >> (7u - (bit & 7u))) & 1u);
        return value;
    }
    void setRecordField(uint8_t *record, uint32_t bit, uint32_t bits, uint32_t value)
    {
        for (uint32_t i = 0; i < bits; ++i, ++bit)
        {
            const uint8_t mask = static_cast<uint8_t>(1u << (7u - (bit & 7u)));
            if ((value >> (bits - 1u - i)) & 1u)
                record[bit >> 3u] |= mask;
            else
                record[bit >> 3u] &= static_cast<uint8_t>(~mask);
        }
    }

    // The slots: a new move goes straight in (a slot's top four bits are never read; kept as they were); a new animation
    // gets its own move, a copy of the slot's move with the clip changed (so it keeps the slot's behaviour - an intro
    // stays an intro), written after the new moves' records, and the clip takes the replaced clip's timing record
    // (camera, frame markers) unless it already has one.
    void applySlots(const SlotChanges &changes, Counts &counts)
    {
        std::unordered_map<std::string, uint32_t> newClips; // a new animation's name -> its clip id
        for (uint32_t i = kExpectedCount1; i < g_clips[1].size(); ++i)
            newClips[upper(g_clips[1][i].name)] = i;

        const uint32_t stride = read32(kWaza + 4u);
        const uint32_t records = read32(kWaza + kRecords); // the store's full-size array (vendetta_moves.cpp)
        uint8_t *fzui = guest(read32(kWaza + kFzuiBank1));
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> copies; // (base record, clip) -> its new record
        for (const auto &[where, change] : changes)
        {
            const uint32_t slotAddress = movesetAddress(where.first) + 2u * where.second;
            const uint16_t value = read16(slotAddress);
            const std::string label = "[mods] " + change.modName + ": " + change.wrestler + " (attire row " +
                                      std::to_string(where.first) + ") slot " + std::to_string(where.second);
            const uint32_t high = change.flags >= 0 ? static_cast<uint32_t>(change.flags) << 12
                                                    : (value == 0xFFFFu ? 0u : (value & 0xF000u));
            if (upper(change.value) == "NONE" || gameMoveNumber(change.value) >= 0)
            {
                // a game move, or an empty slot: written as it is
                const uint16_t newValue = upper(change.value) == "NONE"
                                              ? uint16_t(0xFFFFu)
                                              : static_cast<uint16_t>(high | static_cast<uint32_t>(gameMoveNumber(change.value)));
                std::memcpy(guest(slotAddress), &newValue, 2);
                if (vendetta_mods::detailLog() && !change.base)
                    std::cerr << label << ": " << change.value << " (" << change.modName << ")" << std::endl;
                ++counts.slots;
                continue;
            }
            if (const int moveRecord = vendetta_moves::recordOf(change.value); moveRecord >= 0)
            {
                const uint16_t newValue = static_cast<uint16_t>(high | static_cast<uint32_t>(moveRecord));
                std::memcpy(guest(slotAddress), &newValue, 2);
                if (vendetta_mods::detailLog())
                    std::cerr << label << ": the new move " << change.value << " (move " << moveRecord << ")" << std::endl;
                ++counts.slots;
                continue;
            }
            const auto clip = newClips.find(upper(change.value));
            if (clip == newClips.end())
            {
                std::cerr << label << ": no new move or new animation called \"" << change.value
                          << "\" in this match (a mod's \"new\" list names them; or it was left out above) - the game's move stays"
                          << std::endl;
                continue;
            }
            const uint32_t count = read32(kWaza + 8u);
            if (value == 0xFFFFu || (value & 0xFFFu) >= count)
            {
                vendetta_mods::problem() << label << " is empty - a new animation needs a move to copy there - skipped" << std::endl;
                continue;
            }
            const uint32_t base = value & 0xFFFu;
            auto made = copies.find({base, clip->second});
            if (made == copies.end())
            {
                if (count > kMaxRecord)
                {
                    vendetta_mods::problem() << label << ": no room for another move this match (moveset slots hold 12 bits, 4,096) - the game's move stays"
                              << std::endl;
                    continue;
                }
                uint8_t *record = guest(records + count * stride);
                std::memcpy(record, guest(records + base * stride), stride);
                const uint32_t baseClip = recordField(record, kClipBit, kClipBits);
                setRecordField(record, kClipBit, kClipBits, clip->second);
                // the new clip's timing record: the one of the clip it stands in for, the first time it's used
                static const uint8_t empty[kFzuiSize] = {};
                if (std::memcmp(fzui + clip->second * kFzuiSize, empty, kFzuiSize) == 0 && baseClip < g_clips[1].size())
                    std::memcpy(fzui + clip->second * kFzuiSize, fzui + baseClip * kFzuiSize, kFzuiSize);
                write32(kWaza + 8u, count + 1u);
                made = copies.emplace(std::make_pair(base, clip->second), count).first;
            }
            const uint8_t *baseRecord = guest(records + base * stride);
            const uint32_t baseClip = recordField(baseRecord, kClipBit, kClipBits);
            const uint16_t newValue = static_cast<uint16_t>((value & 0xF000u) | made->second);
            std::memcpy(guest(slotAddress), &newValue, 2);
            if (vendetta_mods::detailLog())
                std::cerr << label << ": move " << base << " (clip " << baseClip << " "
                          << (baseClip < g_clips[1].size() ? g_clips[1][baseClip].name : "?") << ") -> move " << made->second
                          << " (clip " << clip->second << " " << g_clips[1][clip->second].name << ")" << std::endl;
            ++counts.slots;
        }
    }

    std::string characterName(int32_t record)
    {
        if (record < 0 || static_cast<uint32_t>(record) >= vendetta_wrestlers::endRecord())
            return "?";
        const std::string name = guestString(read32(kCharacters + 0x20u * static_cast<uint32_t>(record)));
        return name.empty() ? "?" : name;
    }

    // The character table rows a match's records stand for: a hero (records 0-11) all three of its records.
    std::set<uint32_t> attiresFor(const std::vector<int32_t> &records)
    {
        std::set<uint32_t> attires;
        for (int32_t record : records)
        {
            if (record < 0)
                continue;
            const uint32_t r = static_cast<uint32_t>(record);
            if (r < kHeroes)
                for (uint32_t build = 0; build < 3u; ++build)
                    attires.insert(3u * (r / 3u) + build); // a hero fights with any of its three records' movesets
            else if (hasMoveset(r))
                attires.insert(r);
        }
        return attires;
    }

    // During the loading screen (the match loader's list, records per model buffer at + 4 ... + 0x10): the files the
    // match's mods will need, read on a background thread (vendetta_mods.h), so the match init finds them in memory:
    // its wrestlers' moveset files, every file in the folders of the moves they name, the new animations they name, and
    // every replacement. A guess only: the init reads whatever it needs itself, taking what was read ahead when it is
    // unchanged (a REMATCH or a second match in the same load has no loading screen: it reads as before).
    void readAhead(uint32_t list)
    {
        std::vector<int32_t> records;
        for (uint32_t p = 0; p < 4u; ++p)
            records.push_back(static_cast<int32_t>(read32(list + 4u + 4u * p)));
        std::set<uint32_t> attires; // the match's wrestlers' attires (records)
        for (uint32_t attire : attiresFor(records))
            attires.insert(attire);
        const auto inMatch = [attires](const std::vector<uint32_t> &rows)
        {
            for (uint32_t row : rows)
                if (attires.count(row))
                    return true;
            return false;
        };
        vendetta_mods::runAhead([inMatch]()
        {
            std::vector<uint8_t> bytes;
            std::vector<std::filesystem::path> files; // read at the end, several at once
            const auto inMod = [](const vendetta_mods::Mod &mod, const std::string &relative)
            { return (mod.folder / std::filesystem::path(relative)).lexically_normal(); };
            const auto withSettings = [&files](const std::filesystem::path &file)
            {
                files.push_back(file);
                files.push_back(std::filesystem::path(file).replace_extension(".json"));
            };
            std::set<std::string> named; // what the slots name, upper case
            std::set<std::filesystem::path> folders;
            for (const vendetta_mods::Mod &mod : vendetta_mods::loadedMods())
            {
                const vendetta_mods::Json *wrestlers = mod.manifest.find("wrestlers");
                if (wrestlers && wrestlers->isObject())
                    for (const auto &[wrestler, file] : wrestlers->members)
                    {
                        vendetta_mods::Json moveset;
                        std::string error;
                        if (!inMatch(vendetta_wrestlers::recordsNamed(wrestler)) || !file.isString() ||
                            !vendetta_mods::readAhead(inMod(mod, file.text), bytes) ||
                            !vendetta_mods::parseJson(std::string(bytes.begin(), bytes.end()), moveset, error))
                            continue;
                        for (const auto &[slot, value] : moveset.members)
                            if (value.isString() && named.insert(upper(value.text)).second)
                                for (const std::filesystem::path &folder : vendetta_moves::foldersNamed(value.text))
                                    folders.insert(folder);
                    }
                const vendetta_mods::Json *replace = mod.manifest.find("replace");
                const vendetta_mods::Json *replaced = replace ? replace->find("animations") : nullptr;
                if (replaced && replaced->isObject())
                    for (const auto &[gameName, file] : replaced->members)
                        if (file.isString())
                            withSettings(inMod(mod, file.text));
            }
            for (const vendetta_wrestlers::SlotFile &own : vendetta_wrestlers::slotFiles())
            {
                // a new wrestler's moveset.json: the mod moves it names
                vendetta_mods::Json moveset;
                std::string error;
                if (!inMatch(own.records) || !vendetta_mods::readAhead(own.file, bytes) ||
                    !vendetta_mods::parseJson(std::string(bytes.begin(), bytes.end()), moveset, error))
                    continue;
                for (const auto &[slot, value] : moveset.members)
                {
                    const vendetta_mods::Json *move = value.isObject() ? value.find("move") : &value;
                    if (move && move->isString() && named.insert(upper(move->text)).second)
                        for (const std::filesystem::path &folder : vendetta_moves::foldersNamed(move->text))
                            folders.insert(folder);
                }
            }
            for (const vendetta_mods::Mod &mod : vendetta_mods::loadedMods())
            {
                const vendetta_mods::Json *section = mod.manifest.find("new");
                const vendetta_mods::Json *added = section ? section->find("animations") : nullptr;
                if (added && added->isArray())
                    for (const vendetta_mods::Json &file : added->items)
                        if (file.isString() && named.count(upper(std::filesystem::path(file.text).stem().string())))
                            withSettings(inMod(mod, file.text));
            }
            std::error_code ec;
            for (const std::filesystem::path &folder : folders)
                for (const auto &entry : std::filesystem::recursive_directory_iterator(folder, ec))
                    if (entry.is_regular_file(ec))
                        files.push_back(entry.path());
            vendetta_mods::readAheadAll(files);
        });
    }

    // The match's mods in: everything back to the game's own, then what the match's wrestlers use.
    void loadMatch(const std::array<int32_t, 4> &records, uint32_t rematch)
    {
        const auto start = std::chrono::steady_clock::now();
        resetToGame();
        const SlotChanges changes = readSlotChanges(attiresFor(std::vector<int32_t>(records.begin(), records.end())));
        std::set<std::string> moveNames, animationNames;
        for (const auto &[where, change] : changes)
        {
            if (vendetta_moves::isMove(change.value))
                moveNames.insert(change.value);
            else
                animationNames.insert(upper(change.value));
        }
        Counts counts;
        applyClips(moveNames, animationNames, counts);
        vendetta_moves::apply(); // the new moves' records, names, cameras (their clips were added just above)
        applySlots(changes, counts);
        vendetta_clipsounds::place();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::cerr << "[match] " << (rematch ? "rematch " + std::to_string(rematch) + ": " : "");
        bool first = true;
        for (int32_t record : records)
            if (record >= 0)
            {
                std::cerr << (first ? "" : " v ") << characterName(record) << " (" << record << ")";
                first = false;
            }
        std::cerr << ": " << counts.added << " clips added, " << counts.replaced << " replaced, " << counts.slots
                  << " slots set" << (counts.skipped ? ", " + std::to_string(counts.skipped) + " left out" : "") << "; "
                  << g_stamps.size() << " files read in " << static_cast<int>(ms + 0.5) << " ms" << std::endl;
        vendetta_store::reportMatch();
        if (envOn("VENDETTA_ANIMLIST"))
            listClips();
    }

    std::string clipName(uint32_t id)
    {
        const int bank = id >= 10000u ? 0 : 1;
        const uint32_t index = bank == 0 ? id - 10000u : id;
        if (index < g_clips[bank].size())
            return g_clips[bank][index].name;
        return "?";
    }

    // ------------------------------------------------------------------------------------------------- the hooks
    bool g_newLoad = true;         // a match load since the last build
    bool g_built = false;
    bool g_marked = false;         // g_mark taken in this load
    uint32_t g_mark = 0;           // the match area before this load's first build
    std::array<int32_t, 4> g_builtFor{};

    // The match loader (FUN_00165838; it can wait, so the work comes first): a new match, its area emptied, its mods'
    // files read ahead while the loading screen shows.
    void loadMatchHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_rdram = rdram;
        if (ready(true))
        {
            vendetta_store::beginMatch();
            g_newLoad = true;
            g_marked = false;
            readAhead(getRegU32(ctx, 4));
        }
        g_originalLoadMatch(rdram, ctx, runtime);
    }

    // The match init (FUN_00119d70(rematch count); every match passes it, it reads the movesets itself and can wait):
    // the match's mods in first, unless they already are (the same wrestlers, the same load, no file changed).
    void matchInitHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_rdram = rdram;
        if (ready(false))
        {
            const uint32_t rematch = getRegU32(ctx, 4);
            std::array<int32_t, 4> records{};
            for (uint32_t p = 0; p < 4u; ++p)
                records[p] = static_cast<int32_t>(read32(kMatchRecords + 4u * p));
            const std::filesystem::path changed = g_built && !g_newLoad && records == g_builtFor ? changedFile()
                                                                                                 : std::filesystem::path();
            if (!g_built || g_newLoad || records != g_builtFor || !changed.empty())
            {
                if (!changed.empty())
                    std::cerr << "[match] " << changed.string() << " changed: the match's mods read again" << std::endl;
                if (g_marked)
                    vendetta_store::rewindMatch(g_mark); // built again in the same load: its last build given back
                else
                {
                    g_mark = vendetta_store::matchMark();
                    g_marked = true;
                }
                loadMatch(records, rematch);
                vendetta_mods::dropAhead();
                g_builtFor = records;
                g_newLoad = false;
                g_built = true;
            }
            else
            {
                vendetta_mods::dropAhead();
                std::cerr << "[match] rematch " << rematch << ": the same wrestlers and files: the mods stay as they are"
                          << std::endl;
            }
        }
        g_originalMatchInit(rdram, ctx, runtime);
    }

    // VENDETTA_ANIMLOG: each new clip a fighter starts (FUN_00132158; it skips the work when the clip is already playing).
    void playClip(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const auto start = std::chrono::steady_clock::now();
        const uint32_t fighter = getRegU32(ctx, 4);
        const uint32_t id = getRegU32(ctx, 5);
        g_rdram = rdram;
        if (read32(fighter + 0xFCu) != id)
        {
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::cerr << "[anim] " << std::fixed;
            std::cerr.precision(2);
            std::cerr << seconds << "s fighter 0x" << std::hex << fighter << std::dec << " clip " << id << " "
                      << clipName(id) << " (ra 0x" << std::hex << getRegU32(ctx, 31) << std::dec << ")" << std::endl;
            std::cerr.unsetf(std::ios::fixed);
        }
        g_originalPlayClip(rdram, ctx, runtime);
    }

    void install(PS2Runtime &runtime)
    {
        g_originalLoadMatch = runtime.lookupFunction(kLoadMatch);
        g_originalMatchInit = runtime.lookupFunction(kMatchInit);
        if (!g_originalLoadMatch || !g_originalMatchInit)
        {
            vendetta_mods::problem() << "[mods] the match loader / init isn't where the original program has it: mods are off" << std::endl;
            return;
        }
        runtime.replaceFunction(kLoadMatch, loadMatchHook);
        runtime.replaceFunction(kMatchInit, matchInitHook);
        if (envOn("VENDETTA_ANIMLOG"))
        {
            g_originalPlayClip = runtime.lookupFunction(kPlayClip);
            if (g_originalPlayClip)
                runtime.replaceFunction(kPlayClip, playClip);
        }
    }
}

void installVendettaAnimations(PS2Runtime &runtime) { vendetta_animations::install(runtime); }
