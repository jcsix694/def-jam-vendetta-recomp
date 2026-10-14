// Def Jam Vendetta (USA, SLUS-20639) - the sound events of animations from mods.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// Once a picture the sound step FUN_001b05e0 names the clip each fighter plays and runs that name's events
// (FUN_001c4720): a bank 1 clip below 2003 (slti 0x7D3 at 0x1B06C8) is named from the table at 0x25A790, a bank 0 clip
// from 0x259AB8; any other clip gets no events at all. FUN_001c6098 packs the name's first 8 characters into a u64
// (the first highest), and FUN_001c60d8 finds that key in DEFJAM.AAD, loaded once at boot into a heap block at
// *(0x29A9B8): a binary search over the u32 entry offsets at +0x108 (the entry count at +4). An entry is the name (8
// bytes, stored backwards), a u32 event count, then the events {s16 frame, s16 value count, u32 id, u32 values},
// in frame order.
//
// The game's file and its block are never changed:
//  - config/patches.toml points the sound step at a table in the store's fixed area (0x02001000, 4,096 entries) and
//    lets it name clips up to 4095: the game's 2,003 names copied as they are, a new clip (2003 and up) named
//    ZNEW<clip> (adjmk's names) at 0x02005000 + 16 x (clip - 2003), the rest "error" (the game's default name at
//    0x29A298, which has no entry).
//  - FUN_001c60d8 is hooked: a key a mod gave events answers with the mod's entry (in the store, the file's layout),
//    every other key goes to the game's own search. So the game's sounds come from its own code and file, and a game
//    clip a mod replaces plays only the events the mod lists (none = an entry of 0: silent).
// The first lookup checks the loaded file (sorted, every event's value count as in the table below) and prints it.
#include "vendetta_clipsounds.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "vendetta_mods.h"
#include "vendetta_store.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <unordered_map>

namespace vendetta_clipsounds
{
    constexpr uint32_t kLookup = 0x1C60D8u;      // FUN_001c60d8(key): DEFJAM.AAD's entry for a clip name, or 0
    constexpr uint32_t kAad = 0x29A9B8u;         // the loaded file
    constexpr uint32_t kGameNames = 0x25A790u;   // bank 1's clip names (2,003)
    constexpr uint32_t kDefaultName = 0x29A298u; // "error"
    constexpr uint32_t kGameClips = 2003u;
    constexpr uint32_t kTableOffset = 0x1000u;   // in the store's fixed area: config/patches.toml at 0x1B06D0 / 0x1B06D8
    constexpr uint32_t kTableEntries = 0x1000u;  // config/patches.toml at 0x1B06C8 (slti 0x1000)
    constexpr uint32_t kNamesOffset = 0x5000u;   // the new clips' names, 16 bytes each
    constexpr uint32_t kFirstTimeOnly = 0x10000000u;

    // The events the game's own animations use (DEFJAM.AAD's 2,602 entries), by name (adjmk's names), with the number
    // of values each one always carries.
    struct Kind
    {
        uint32_t id;
        const char *name;
        uint32_t values;
    };
    constexpr Kind kKinds[] = {
        {0x09, "Footstep", 1}, {0x1F, "Footstep, running", 1}, {0x0B, "Swing (whoosh)", 1}, {0x04, "Grab", 1},
        {0x00, "Handling", 1}, {0x37, "Struggle", 1}, {0x1D, "Slap", 1}, {0x03, "Body hit", 1},
        {0x06, "Impact on the floor", 1}, {0x08, "Block", 0}, {0x38, "Body on the mat", 1}, {0x07, "Heavy fall", 0},
        {0x05, "Stomp", 0}, {0x0A, "Ropes", 0}, {0x1C, "Clap", 1}, {0x0D, "Grunt", 1}, {0x49, "Hurt cry", 1},
        {0x10013, "Voice: intro line", 0}, {0x10014, "Voice: win line", 0}, {0x3A, "Special punch", 0},
        {0x44, "Special kick", 0}, {0x39, "Special corkscrew", 0}, {0x47, "Special chest hit", 0},
        {0x45, "Special (chest, women)", 1}, {0x46, "Special (behind)", 0}, {0x11, "Crowd: knockdown cheer", 1},
        {0x10012, "Music stinger", 2}, {0x10002, "Music event", 1}, {0x10005, "Crowd bed: start", 0},
        {0x10006, "Crowd bed: stop", 0}, {0x10009, "Quieten the game", 0}, {0x10008, "Sound back to normal", 0},
        {0x1000B, "Blazin' sound: start", 0}, {0x1000C, "Blazin' sound: stop", 0}, {0x10017, "Controller vibration", 1},
    };

    const Kind *kindOf(uint32_t id)
    {
        for (const Kind &kind : kKinds)
            if (kind.id == (id & ~kFirstTimeOnly))
                return &kind;
        return nullptr;
    }

    PS2Runtime::RecompiledFunction g_originalLookup = nullptr;
    uint32_t g_table = 0;   // the name table's guest address (0: not there)
    uint32_t g_names = 0;   // the new clips' names

    struct Sounds
    {
        std::vector<Event> events;
        std::string from;
        uint32_t entry = 0; // in the store, once placed (0 = silent)
    };
    std::map<std::string, Sounds> g_sounds;              // by the name the sound step looks up
    std::unordered_map<uint64_t, uint32_t> g_entries;    // key -> the mod's entry (0 = silent)
    bool g_checked = false;

    uint8_t *guest(uint32_t address) { return vendetta_store::host(address); }
    uint32_t read32(uint32_t address)
    {
        uint32_t value = 0;
        std::memcpy(&value, guest(address), 4);
        return value;
    }
    void write32(uint32_t address, uint32_t value) { std::memcpy(guest(address), &value, 4); }

    // FUN_001c6098: up to 8 characters, the first highest, each added as a signed byte.
    uint64_t keyOf(const std::string &name)
    {
        uint64_t key = 0;
        size_t at = 0;
        for (int i = 0; i < 8; ++i)
        {
            key <<= 8u;
            if (at < name.size() && name[at] != 0)
                key += static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(name[at++])));
        }
        return key;
    }

    // An entry's key as FUN_001c60d8 reads it: its 8 name bytes from the last to the first.
    uint64_t entryKey(uint32_t entry)
    {
        uint64_t key = 0;
        for (int i = 7; i >= 0; --i)
            key = (key << 8u) + static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(*guest(entry + i))));
        return key;
    }

    std::string lower(std::string text)
    {
        for (char &c : text)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    std::string hex(uint32_t value)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "0x%X", value);
        return text;
    }

    std::string describe(uint32_t id)
    {
        const Kind *kind = kindOf(id);
        std::string text = kind ? kind->name : hex(id & ~kFirstTimeOnly);
        return (id & kFirstTimeOnly) ? text + " (first time only)" : text;
    }

    bool wholeNumber(const vendetta_mods::Json &value, double low, double high, int64_t &out)
    {
        if (value.type != vendetta_mods::Json::Type::Number || value.number != std::floor(value.number) ||
            value.number < low || value.number > high)
            return false;
        out = static_cast<int64_t>(value.number);
        return true;
    }

    // "sound": a name from the table (any case), a number, or a number written "0x37".
    bool soundId(const vendetta_mods::Json &value, uint32_t &id)
    {
        int64_t number = 0;
        if (wholeNumber(value, 0, 0xFFFFFFFF, number))
        {
            id = static_cast<uint32_t>(number);
            return true;
        }
        if (!value.isString())
            return false;
        for (const Kind &kind : kKinds)
            if (lower(kind.name) == lower(value.text))
            {
                id = kind.id;
                return true;
            }
        char *end = nullptr;
        const unsigned long parsed = std::strtoul(value.text.c_str(), &end, 0);
        if (value.text.empty() || *end != '\0')
            return false;
        id = static_cast<uint32_t>(parsed);
        return true;
    }

    std::vector<Event> readSounds(const vendetta_mods::Json &list, const std::string &where, uint32_t lastFrame)
    {
        std::vector<Event> events;
        if (!list.isArray())
        {
            std::cerr << "[mods] " << where << ": \"sounds\" should be a list: [{ \"frame\": 12, \"sound\": \"Swing (whoosh)\", "
                      << "\"value\": 3 }] - no sounds" << std::endl;
            return events;
        }
        for (size_t n = 0; n < list.items.size(); ++n)
        {
            const vendetta_mods::Json &item = list.items[n];
            const std::string which = where + ": sound " + std::to_string(n + 1);
            const vendetta_mods::Json *frame = item.isObject() ? item.find("frame") : nullptr;
            const vendetta_mods::Json *sound = item.isObject() ? item.find("sound") : nullptr;
            int64_t frameNumber = 0;
            uint32_t id = 0;
            if (!frame || !wholeNumber(*frame, 0, 32767, frameNumber))
            {
                std::cerr << "[mods] " << which << ": needs \"frame\": a whole number from 0 - skipped" << std::endl;
                continue;
            }
            if (!sound || !soundId(*sound, id))
            {
                std::cerr << "[mods] " << which << ": \"sound\" should be one of the game's sound names (as \"Grunt\") or a "
                          << "number - skipped" << std::endl;
                continue;
            }
            const vendetta_mods::Json *firstTime = item.find("first_time_only");
            if (firstTime && firstTime->type == vendetta_mods::Json::Type::Bool && firstTime->boolean)
                id |= kFirstTimeOnly;
            Event event;
            event.frame = static_cast<int16_t>(frameNumber);
            event.id = id;
            bool valuesOk = true;
            int64_t number = 0;
            if (const vendetta_mods::Json *value = item.find("value"))
            {
                valuesOk = wholeNumber(*value, -2147483648.0, 4294967295.0, number);
                event.values.push_back(static_cast<uint32_t>(number));
            }
            else if (const vendetta_mods::Json *values = item.find("values"))
            {
                valuesOk = values->isArray();
                for (const vendetta_mods::Json &each : values->items)
                {
                    valuesOk = valuesOk && wholeNumber(each, -2147483648.0, 4294967295.0, number);
                    event.values.push_back(static_cast<uint32_t>(number));
                }
            }
            if (!valuesOk)
            {
                std::cerr << "[mods] " << which << ": \"value\" should be a whole number (\"values\": a list of them) - skipped"
                          << std::endl;
                continue;
            }
            const uint32_t plain = id & ~kFirstTimeOnly;
            if (plain == 0x10018u)
            {
                std::cerr << "[mods] " << which << ": 0x10018 (the per-voice switch) would silence the animation - skipped"
                          << std::endl;
                continue;
            }
            if (const Kind *kind = kindOf(id))
            {
                if (event.values.size() != kind->values)
                {
                    std::cerr << "[mods] " << which << ": " << kind->name << " takes " << kind->values
                              << (kind->values == 1 ? " value" : " values") << ", not " << event.values.size() << " - skipped"
                              << std::endl;
                    continue;
                }
            }
            else
                std::cerr << "[mods] " << which << ": " << hex(plain) << " is not one the game's own animations use: "
                          << "kept as written" << std::endl;
            if (static_cast<uint32_t>(frameNumber) > lastFrame)
                std::cerr << "[mods] " << which << ": frame " << frameNumber << " is after the animation's last frame ("
                          << lastFrame << "): a sound there never plays" << std::endl;
            events.push_back(std::move(event));
        }
        std::stable_sort(events.begin(), events.end(), [](const Event &a, const Event &b) { return a.frame < b.frame; });
        return events;
    }

    std::string nameNewClip(uint32_t clip)
    {
        char name[16] = {};
        std::snprintf(name, sizeof(name), "ZNEW%04u", clip);
        if (g_table == 0u || clip < kGameClips || clip >= kTableEntries)
            return name;
        const uint32_t at = g_names + 16u * (clip - kGameClips);
        std::memcpy(guest(at), name, sizeof(name));
        write32(g_table + 4u * clip, at);
        return name;
    }

    void setSounds(const std::string &soundName, const std::vector<Event> &events, const std::string &from)
    {
        Sounds &sounds = g_sounds[soundName];
        if (!sounds.from.empty())
            std::cerr << "[mods] " << from << ": the sounds of " << soundName << " were also set by " << sounds.from << " - "
                      << from << "'s are used (later in the load order)" << std::endl;
        sounds.events = events;
        sounds.from = from;
    }

    void place()
    {
        for (auto &[name, sounds] : g_sounds)
        {
            uint32_t size = 12u;
            for (const Event &event : sounds.events)
                size += 8u + 4u * static_cast<uint32_t>(event.values.size());
            if (!sounds.events.empty())
            {
                sounds.entry = vendetta_store::allocate(vendetta_store::Area::Boot, size, "animation sounds", 16u, 0u);
                if (sounds.entry == 0u)
                    continue;
                uint8_t *to = guest(sounds.entry);
                std::memset(to, 0, size);
                for (size_t i = 0; i < 8u && i < name.size(); ++i)
                    to[7u - i] = static_cast<uint8_t>(name[i]); // stored backwards, as in the file
                const uint32_t count = static_cast<uint32_t>(sounds.events.size());
                std::memcpy(to + 8u, &count, 4u);
                uint8_t *at = to + 12u;
                for (const Event &event : sounds.events)
                {
                    const int16_t values = static_cast<int16_t>(event.values.size());
                    std::memcpy(at, &event.frame, 2u);
                    std::memcpy(at + 2u, &values, 2u);
                    std::memcpy(at + 4u, &event.id, 4u);
                    std::memcpy(at + 8u, event.values.data(), 4u * event.values.size());
                    at += 8u + 4u * event.values.size();
                }
            }
            g_entries[keyOf(name)] = sounds.entry;
            std::cerr << "[mods] " << sounds.from << ": " << name << " plays " << sounds.events.size() << " sound events"
                      << (sounds.events.empty() ? " (silent)" : ":");
            for (const Event &event : sounds.events)
            {
                std::cerr << " " << event.frame << " " << describe(event.id);
                for (uint32_t value : event.values)
                    std::cerr << " " << static_cast<int32_t>(value);
                std::cerr << ";";
            }
            std::cerr << std::endl;
        }
    }

    // The first lookup (DEFJAM.AAD is in): the file in order, every event's value count as the table says, and which
    // of the mods' names the game also has.
    void check()
    {
        g_checked = true;
        const uint32_t aad = read32(kAad);
        if (aad == 0u || std::memcmp(guest(aad), "AASD", 4) != 0)
        {
            std::cerr << "[sounds] DEFJAM.AAD isn't loaded at the first lookup: not checked" << std::endl;
            return;
        }
        const uint32_t count = read32(aad + 4u);
        std::set<uint64_t> keys;
        uint64_t previous = 0;
        uint32_t unsorted = 0, events = 0, unknown = 0, wrongCount = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t entry = aad + read32(aad + 0x108u + 4u * i);
            const uint64_t key = entryKey(entry);
            if (i && key < previous)
                ++unsorted;
            previous = key;
            keys.insert(key);
            uint32_t at = entry + 12u;
            for (uint32_t e = read32(entry + 8u); e; --e, ++events)
            {
                const uint32_t id = read32(at + 4u);
                const int16_t values = static_cast<int16_t>(read32(at) >> 16u);
                if (const Kind *kind = kindOf(id))
                    wrongCount += static_cast<uint32_t>(values) != kind->values;
                else
                    ++unknown;
                at += 8u + 4u * static_cast<uint32_t>(std::max<int16_t>(values, 0));
            }
        }
        std::cerr << "[sounds] DEFJAM.AAD: " << count << " entries, " << events << " events; "
                  << (unsorted ? std::to_string(unsorted) + " out of order, " : "in order, ")
                  << (unknown + wrongCount ? std::to_string(unknown) + " events of other kinds, " + std::to_string(wrongCount) +
                                                 " with another value count than the table's"
                                           : "every event as the table says")
                  << std::endl;
        for (const auto &[name, sounds] : g_sounds)
            std::cerr << "[sounds] " << name << " (" << sounds.from << "): "
                      << (keys.count(keyOf(name)) ? "in place of the game's events" : "the game has no events for it") << std::endl;
    }

    void lookup(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!g_entries.empty())
        {
            if (!g_checked)
                check();
            const auto found = g_entries.find(GPR_U64(ctx, 4));
            if (found != g_entries.end())
            {
                setReturnU32(ctx, found->second);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
        }
        g_originalLookup(rdram, ctx, runtime);
    }

    void install(PS2Runtime &runtime)
    {
        g_table = vendetta_store::fixed(kTableOffset, kTableEntries * 4u, "clip names for sounds");
        g_names = vendetta_store::fixed(kNamesOffset, (kTableEntries - kGameClips) * 16u, "new clip names for sounds");
        if (g_table == 0u || g_names == 0u)
        {
            std::cerr << "[sounds] the store's fixed area is missing: the sound step's clip name table can't be filled"
                      << std::endl;
            g_table = 0u;
            return;
        }
        // the game's names as they are (the table is in the program's data, there before it runs)
        for (uint32_t clip = 0; clip < kTableEntries; ++clip)
            write32(g_table + 4u * clip, clip < kGameClips ? read32(kGameNames + 4u * clip) : kDefaultName);
        if (std::memcmp(guest(kDefaultName), "error", 6) != 0)
            std::cerr << "[sounds] the default clip name at 0x29A298 isn't \"error\": not the original program?" << std::endl;
        g_originalLookup = runtime.lookupFunction(kLookup);
        if (g_originalLookup)
            runtime.replaceFunction(kLookup, lookup);
    }
}

void installVendettaClipSounds(PS2Runtime &runtime) { vendetta_clipsounds::install(runtime); }
