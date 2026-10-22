// Def Jam Vendetta (USA, SLUS-20639) — the store: guest memory above the PS2's 32 MB for data from mods.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// The game's own code can only read guest memory, and the PS2's 32 MB is full of the game. The runtime backs guest
// RAM with PS2_RAM_BACKING_SIZE (256 MB) but commits only the first 32 MB; the store hands out pieces of the rest
// (from 0x02000000 up), committing memory as it goes, so a mod's animations, timing records - later models, arenas,
// sounds - can sit where the game's code reads them like any other RAM. Only data the game's own code reads belongs
// here; data our own C++ reads stays in ordinary PC memory.
//
// Areas: the boot area (from the bottom up) holds what is placed once and kept; the match area (from the top down) is
// emptied at each match load and refilled with only what that match uses, so the number of mods installed is no longer
// limited by guest memory. The two grow towards each other.
//
// Each run prints what the store holds ([store] ...), by user; each match what its area holds.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vendetta_store
{
    enum class Area
    {
        Boot,  // placed once, kept for the whole run
        Match, // emptied at each match load (beginMatch)
    };

    // Called once at start (game_overrides): the runtime's memory.
    void install(PS2Runtime &runtime);

    // A piece of guest memory of `size` bytes in `area`, starting `phase` bytes past a multiple of `alignment` (a power
    // of two; the game's clips sit 8 past a 16-byte boundary: alignment 16, phase 8). `user` names what it holds, for
    // the report. Returns the guest address (>= 0x02000000), or 0 with a message when the store is full.
    uint32_t allocate(Area area, uint32_t size, const char *user, uint32_t alignment = 16, uint32_t phase = 0);

    // The fixed area: the store's first 20 MB (0x02000000-0x033FFFFF), for data the game's code finds at an address
    // patched into it before translation (config: [patches]), so the address can't move. Each user keeps to its own
    // offsets, listed here: 0x0000-0x01FF the main menu's tables and texts (vendetta_menu.cpp); 0x1000-0xD2CF the clip
    // names the sound step reads, and the new clips' names (vendetta_clipsounds.cpp); the roster tables below
    // (vendetta_wrestlers.cpp); from 0x905500 the arena's crowd and stand-in scene below (vendetta_arenas.cpp). Returns the guest address of `offset`, its memory committed, or 0.
    constexpr uint32_t kFixedArea = 0x02000000u;
    constexpr uint32_t kFixedSize = 0x01400000u;
    uint32_t fixed(uint32_t offset, uint32_t size, const char *user);

    // The roster tables, moved here so new wrestlers can follow the game's own (vendetta_wrestlers.cpp fills them at
    // start-up). Each copy keeps its original's low 16 bits, so only the game's luis are patched (every addiu / load
    // offset stays); each has room for far more entries than any roster needs.
    constexpr uint32_t kCharacterTable = 0x0201E100u; // was 0x27E100: one 0x20 record per attire (4 MB: 131,072)
    constexpr uint32_t kSelectGrid = 0x02429EF0u;     // was 0x239EF0: Choose Fighter's slots {base record, attires} (1 MB)
    constexpr uint32_t kMoveData = 0x025331C8u;       // was 0x2331C8: the WAZA struct, +0x70 + 16 x record = a record's
                                                      // {?, WPAR, ?, WWAZ} (2 MB); the boot reads it straight here
    constexpr uint32_t kSizeClasses = 0x02744910u;    // was 0x234910: one float per record (512 KB)
    constexpr uint32_t kVoiceMap = 0x027C99B8u;       // was 0x2599B8: a record's voice row (512 KB)
    constexpr uint32_t kVoiceTable = 0x0284F938u;     // was 0x25F938: the voice rows, 0x34 bytes each (room for 8,192)

    // The arena (vendetta_arenas.cpp): the 3-D crowd the game's crowd code reads (the people (AUD) and their clips (PAK);
    // its luis patched, each copy keeping the original's low 16 bits) and a mod arena's stand-in scene (its parts
    // without their triangles: the game reads the scene through FUN_001a1c38's argument), 3 / 3 / 4 MB of room.
    constexpr uint32_t kCrowdPeople = 0x02905500u; // was 0xBD5500 (1.3 MB)
    constexpr uint32_t kCrowdClips = 0x02C0A500u;  // was 0xD1A500 (800 KB)
    constexpr uint32_t kArenaScene = 0x02F10000u;  // the game's own scene stays in its buffer at 0xB1C500
    constexpr uint32_t kCrowdRoom = 0x00300000u, kArenaSceneRoom = 0x00400000u;
    constexpr uint32_t kArenaFlyIn = 0x03310000u;  // a mod arena's fly-in camera path (the game's path entry pointed here)
    constexpr uint32_t kArenaFlyInRoom = 0x00010000u;
    // Choose Stage's places (patched luis): the stage at each place, each stage's picture id and thumbnail, each stage's
    // unlock flag (copied from the game's 12 before each Choose Stage frame, the mods' new ones always on), 64 KB each
    constexpr uint32_t kStageOrder = 0x03320000u, kStagePictures = 0x03330000u, kStageThumbs = 0x03340000u,
                       kStageFlags = 0x03350000u, kStageRoom = 0x00010000u;

    // Host pointer for a guest address the store handed out.
    uint8_t *host(uint32_t guestAddress);

    // Whether `size` bytes at a guest address can be read: the game's 32 MB or memory the store has committed (a
    // pointer read from a table, checked before following it).
    bool readable(uint32_t guestAddress, uint32_t size = 1u);

    // A new match: the match area emptied (VENDETTA_ANIM_POISON=1: its old bytes filled with 0xFF first, so anything that
    // still read the last match's data would show).
    void beginMatch();

    // A point in the match area: rewindMatch(mark) gives back everything placed since (VENDETTA_ANIM_POISON=1: filled with
    // 0xFF first), for a user that places its part last and builds it again within the same match load (one mark at a
    // time).
    uint32_t matchMark();
    void rewindMatch(uint32_t mark);

    // Prints what the store holds, by user (the boot area once the mods are in; reportMatch the match area).
    void report();
    void reportMatch();
}
