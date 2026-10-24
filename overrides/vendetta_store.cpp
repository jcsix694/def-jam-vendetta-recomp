// Def Jam Vendetta (USA, SLUS-20639) — the store: guest memory above the PS2's 32 MB for data from mods.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
#include "vendetta_store.h"

#include "ps2_runtime.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace vendetta_store
{
    namespace
    {
        constexpr uint32_t kStart = PS2_RAM_SIZE;         // 0x02000000: just above the game's own RAM
        constexpr uint32_t kEnd = PS2_RAM_BACKING_SIZE;   // the end of the backed range (256 MB)
        constexpr uint32_t kCommitStep = 1u << 20;        // commit in 1 MB steps

        PS2Memory *g_memory = nullptr;
        uint32_t g_bootTop = kStart + kFixedSize;         // the boot area grows up after the fixed area
        uint32_t g_committed = kStart;                    // [kStart, g_committed) is usable
        uint32_t g_matchBottom = kEnd;                    // the match area grows down from the end: [g_matchBottom, kEnd)
        uint32_t g_matchCommitted = kEnd;                 // [g_matchCommitted, kEnd) is usable
        std::map<std::string, uint64_t> g_used;           // bytes by user (padding included)
        std::map<std::string, uint64_t> g_matchUsed;      // the same, this match
        std::vector<void (*)()> g_onMatchEmptied;

        void matchEmptied()
        {
            for (void (*callback)() : g_onMatchEmptied)
                callback();
        }

        std::string kilobytes(uint64_t bytes)
        {
            if (bytes >= (10u << 20))
                return std::to_string(bytes >> 20) + " MB";
            if (bytes >= 10240u)
                return std::to_string((bytes + 512u) >> 10) + " KB";
            return std::to_string(bytes) + " bytes";
        }
    }

    void install(PS2Runtime &runtime) { g_memory = &runtime.memory(); }

    uint32_t allocate(Area area, uint32_t size, const char *user, uint32_t alignment, uint32_t phase)
    {
        if (!g_memory || alignment == 0u || (alignment & (alignment - 1u)) != 0u || phase >= alignment)
        {
            std::cerr << "[store] " << user << ": can't place " << size << " bytes (store not ready or a bad alignment)"
                      << std::endl;
            return 0u;
        }
        if (area == Area::Match)
        {
            // down from the match area's bottom: the highest start at or below bottom - size, `phase` past a multiple
            uint64_t start = static_cast<uint64_t>(g_matchBottom) - size;
            start = ((start - phase) & ~static_cast<uint64_t>(alignment - 1u)) + phase;
            if (size > g_matchBottom - g_bootTop || start < g_bootTop || start > g_matchBottom)
            {
                std::cerr << "[store] " << user << ": no room for " << kilobytes(size) << " in the match area ("
                          << kilobytes(g_matchBottom - g_bootTop) << " left)" << std::endl;
                return 0u;
            }
            if (start < g_matchCommitted)
            {
                const uint32_t from = static_cast<uint32_t>((start / kCommitStep) * kCommitStep);
                const uint32_t to = from < g_bootTop ? g_bootTop : from;
                if (!g_memory->commitRam(to, g_matchCommitted - to))
                {
                    std::cerr << "[store] " << user << ": the PC refused " << kilobytes(g_matchCommitted - to) << " more memory"
                              << std::endl;
                    return 0u;
                }
                g_matchCommitted = to;
            }
            g_matchUsed[user] += g_matchBottom - start;
            g_matchBottom = static_cast<uint32_t>(start);
            return g_matchBottom;
        }
        const uint64_t start = ((static_cast<uint64_t>(g_bootTop) + alignment - 1u - phase) & ~static_cast<uint64_t>(alignment - 1u)) + phase;
        const uint64_t end = start + size;
        if (end > g_matchBottom)
        {
            std::cerr << "[store] " << user << ": no room for " << kilobytes(size) << " (" << kilobytes(g_matchBottom - g_bootTop)
                      << " left of " << ((kEnd - kStart) >> 20) << " MB)" << std::endl;
            return 0u;
        }
        if (end > g_committed)
        {
            const uint64_t want = ((end + kCommitStep - 1u) / kCommitStep) * kCommitStep;
            const uint32_t to = static_cast<uint32_t>(want < kEnd ? want : kEnd);
            if (!g_memory->commitRam(g_committed, to - g_committed))
            {
                std::cerr << "[store] " << user << ": the PC refused " << kilobytes(to - g_committed) << " more memory"
                          << std::endl;
                return 0u;
            }
            g_committed = to;
        }
        g_used[user] += end - g_bootTop;
        g_bootTop = static_cast<uint32_t>(end);
        return static_cast<uint32_t>(start);
    }

    uint32_t fixed(uint32_t offset, uint32_t size, const char *user)
    {
        if (!g_memory || offset + size > kFixedSize)
        {
            std::cerr << "[store] " << user << ": the fixed area has no 0x" << std::hex << offset << "+" << size << std::dec
                      << std::endl;
            return 0u;
        }
        const uint32_t to = ((kStart + offset + size + kCommitStep - 1u) / kCommitStep) * kCommitStep;
        if (g_committed < to)
        {
            if (!g_memory->commitRam(g_committed, to - g_committed))
            {
                std::cerr << "[store] " << user << ": the PC refused the fixed area's memory" << std::endl;
                return 0u;
            }
            g_committed = to;
        }
        g_used[std::string(user) + " (fixed)"] += size;
        return kFixedArea + offset;
    }

    uint8_t *host(uint32_t guestAddress) { return g_memory->getRDRAM() + (guestAddress & PS2_RAM_MASK); }

    bool readable(uint32_t guestAddress, uint32_t size)
    {
        const uint64_t end = static_cast<uint64_t>(guestAddress) + size;
        if (end <= kStart)
            return true;
        return guestAddress >= kStart && ((end <= g_committed) || (guestAddress >= g_matchCommitted && end <= kEnd));
    }

    void beginMatch()
    {
        if (!g_memory)
            return;
        matchEmptied();
        static const bool poison = std::getenv("VENDETTA_ANIM_POISON") && *std::getenv("VENDETTA_ANIM_POISON") != '0';
        if (poison && g_matchBottom < kEnd)
        {
            std::memset(host(g_matchBottom), 0xFF, kEnd - g_matchBottom);
            std::cerr << "[store] VENDETTA_ANIM_POISON: the last match's " << kilobytes(kEnd - g_matchBottom)
                      << " filled with 0xFF" << std::endl;
        }
        g_matchBottom = kEnd;
        g_matchUsed.clear();
    }

    namespace
    {
        std::map<std::string, uint64_t> g_markUsed; // g_matchUsed when the mark was taken
    }

    uint32_t matchMark()
    {
        g_markUsed = g_matchUsed;
        return g_matchBottom;
    }

    void rewindMatch(uint32_t mark)
    {
        if (!g_memory || mark < g_matchBottom || mark > kEnd)
            return;
        matchEmptied();
        static const bool poison = std::getenv("VENDETTA_ANIM_POISON") && *std::getenv("VENDETTA_ANIM_POISON") != '0';
        if (poison && mark > g_matchBottom)
            std::memset(host(g_matchBottom), 0xFF, mark - g_matchBottom);
        g_matchBottom = mark;
        g_matchUsed = g_markUsed;
    }

    void onMatchEmptied(void (*callback)()) { g_onMatchEmptied.push_back(callback); }

    void report()
    {
        if (g_used.empty())
            return;
        std::cerr << "[store] fixed area + boot area 0x" << std::hex << kStart << "-0x" << g_bootTop << std::dec << ": "
                  << kilobytes(g_bootTop - kStart - kFixedSize) << " placed (";
        bool first = true;
        for (const auto &[user, bytes] : g_used)
        {
            std::cerr << (first ? "" : ", ") << user << " " << kilobytes(bytes);
            first = false;
        }
        std::cerr << "); " << kilobytes(g_matchBottom - g_bootTop) << " free" << std::endl;
    }

    void reportMatch()
    {
        std::cerr << "[store] match area 0x" << std::hex << g_matchBottom << "-0x" << kEnd << std::dec << ": "
                  << kilobytes(kEnd - g_matchBottom) << " placed";
        if (!g_matchUsed.empty())
        {
            std::cerr << " (";
            bool first = true;
            for (const auto &[user, bytes] : g_matchUsed)
            {
                std::cerr << (first ? "" : ", ") << user << " " << kilobytes(bytes);
                first = false;
            }
            std::cerr << ")";
        }
        std::cerr << "; " << kilobytes(g_matchBottom - g_bootTop) << " free" << std::endl;
    }
}
