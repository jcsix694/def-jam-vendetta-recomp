// Def Jam Vendetta (USA, SLUS-20639) - ring shapes: a mod arena's ring read from its arena.json.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// The fight's ring is fixed numbers in the game's code (four sides, four corners, twelve ropes). This file runs another
// shape from a description. An arena with no description gets the game's own ring: every hook below then does exactly
// what the instruction it stands in for did, and the moved tables hold the game's own values.
//
// A hook in the middle of a routine: config/patches.toml replaces one instruction with a jal to a guest address no
// recompiled function covers, and registerFunction answers that address with C++ here (the recompiled code looks up
// every jal when it runs). The patched routine must save $ra (it does, if it calls anything), and the instruction after
// the patch runs first, as the jal's delay slot.
//
// The old code keeps its side number 0 to 3: FUN_00130438 answers in four groups of the ring's sides (back, front, both
// right, both left), so a table of four entries can't be read past its end. Exact answers come from sideOf / cornerOf.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "vendetta_mods.h"
#include "vendetta_ring.h"
#include "vendetta_store.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vendetta_ring
{
    namespace
    {
        // 0x1DF448-0x1DF6A7: code range no recompiled function covers, 152 addresses for the hooks
        constexpr uint32_t kHookFirst = 0x1DF448u, kHookEnd = 0x1DF6A8u;
        constexpr uint32_t kHookWallSearch = kHookFirst;      // jal from 0x12AFBC in FUN_0012acc8, the wall search
        constexpr uint32_t kHookMatchStart = kHookFirst + 4u; // jal from 0x119E4C in FUN_00119d70, each match or round's start
        constexpr uint32_t kHookWallCheck = kHookFirst + 8u;  // jal from 0x12B714 in FUN_0012b600, the other wall search
        static_assert(kRopeDrawReturn == kHookFirst + 12u);   // the rope draw's calls return here

        constexpr uint32_t kLinkWallLists = 0x12B550u; // FUN_0012b550: the call the match-start hook stands in for
        constexpr uint32_t kNearestSide = 0x130438u;   // FUN_00130438: which side a point is nearest
        constexpr uint32_t kAreaScale = 0x294A84u;     // the float (0.1) a wall's +0x3C is scaled by

        // The ropes
        constexpr uint32_t kRingDraw = 0x1A2DE8u;    // FUN_001a2de8: the posts and ropes drawn
        constexpr uint32_t kPullRope = 0x1A2BC8u;    // FUN_001a2bc8(half width f12, rope, point)
        constexpr uint32_t kRopeSpring = 0x1A2CF8u;  // FUN_001a2cf8(step f12): every rope springs back
        constexpr uint32_t kClearRopes = 0x1A2B80u;  // FUN_001a2b80: at the arena's load
        constexpr uint32_t kRopeWindows = 0x131848u; // FUN_00131848(body points, window type): what a move's window pulls
        constexpr uint32_t kSetRenderState = 0x1B8998u, kLoadRopeProgram = 0x1B8D90u, kPoseRope = 0x1B8F70u, kSetMatrix = 0x1B9348u,
                           kDrawPart = 0x1B93A0u;
        constexpr uint32_t kRopeBones = 0x2588A0u, kRopeTemplate = 0x2586F0u, kViewA = 0x258000u, kViewB = 0x258040u,
                           kPartLight = 0x258590u, kPartColours = 0x258550u;
        constexpr uint32_t kSceneHeader = 0x29A930u;  // the arena scene's pointer
        constexpr uint32_t kPullHeights = 0x236B20u;  // the three heights a pulled rope is taken to
        constexpr uint32_t kGp = 0x29C570u;
        constexpr uint32_t kVif1Status = 0x10009000u; // the wait after each rope: the chain still reading its bones
        constexpr int kRopeBendPoints = 11;
        constexpr uint32_t kRopeState = 0x033D0000u, kRopeSpeed = 0x033D0200u, kRopeMatrix = 0x033D0400u;

        // Corner moves
        constexpr uint32_t kHookSetCorner = kHookFirst + 16u; // jal from 0x1150BC and 0x13C05C, in place of jal FUN_00101f88
        constexpr uint32_t kWhip = 0x121FE8u;                 // FUN_00121fe8: where a whip sends the opponent
        constexpr uint32_t kHookPostLoop = kHookFirst + 28u;  // jal from 0x114F94 in FUN_00114e68
        constexpr uint32_t kHookInRing = kHookFirst + 32u;    // jal from 0x1197D4 (FUN_001193e8) and 0x137184 (FUN_00136db8)
        constexpr uint32_t kStartMove = 0x101F88u;            // FUN_00101f88: what the corner hook stands in for
        // The apron: a side number no case matches leaves the game's fixed-place code doing nothing, and the hook places
        // the wrestler along the ring's own side
        constexpr uint32_t kHookApronPlace = kHookFirst + 36u;   // jal from 0x132210 in FUN_00132158
        constexpr uint32_t kHookApronFacing = kHookFirst + 40u;  // jal from 0x1322AC in FUN_00132158
        constexpr uint32_t kHookApronStance = kHookFirst + 44u;  // jal from 0x13D91C in FUN_0013d8a0
        constexpr uint32_t kHookApronLook = kHookFirst + 48u;    // jal from 0x102BA8 in FUN_00102b08
        constexpr uint32_t kHookApronProbe = kHookFirst + 52u;   // jal from 0x12AD50 in FUN_0012acc8
        constexpr uint32_t kHookStartExit = kHookFirst + 56u;    // j from 0x13EF40 and 0x13EF70 in FUN_0013eaa8
        constexpr uint32_t kHookApronProbe2 = kHookFirst + 60u;  // jal from 0x12AD78 in FUN_0012acc8
        // Lining up a pair for a move: cases of FUN_00133be0's switch, which reads them from a table in memory
        constexpr uint32_t kHookPairFirst = kHookFirst + 64u;    // 9 addresses, one a case
        constexpr uint32_t kPairMerge = 0x134770u, kCornerCaseResume = 0x133DBCu;
        constexpr uint32_t kHookAngleF0A0 = kHookFirst + 100u;    // jal from 0x115AC8
        constexpr uint32_t kHookAngleF0V0 = kHookFirst + 104u;    // jal from 0x115C24 and 0x115E0C
        constexpr uint32_t kHookAngleThenTurn = kHookFirst + 108u; // jal from 0x115DB4, in place of jal FUN_00101928
        constexpr uint32_t kHookAngleAdd = kHookFirst + 112u;     // jal from 0x123DA8
        constexpr uint32_t kHookAngleF12 = kHookFirst + 116u;     // jal from 0x1374D4
        constexpr uint32_t kHookAngleF20 = kHookFirst + 120u;     // jal from 0x138DCC
        constexpr uint32_t kHookApronWalk = kHookFirst + 124u;    // jal from 0x13DA54, in place of jal FUN_0013e130
        constexpr uint32_t kTurnTo = 0x101928u, kMoveBy = 0x13E130u, kApronStick = 0x13E0E8u;
        // Getting in and out of the ring (FUN_00123fb0), and which way a wrestler faces at the start (FUN_0013ef78)
        constexpr uint32_t kHookStepTurn = kHookFirst + 128u;    // jal from 0x124020, in place of jal FUN_0012f220
        constexpr uint32_t kHookStepAngle = kHookFirst + 132u;   // jal from 0x12415C
        constexpr uint32_t kHookStepMove = kHookFirst + 136u;    // jal from 0x124170
        constexpr uint32_t kHookStartFacing = kHookFirst + 140u; // jal from 0x13F000, in place of jal FUN_00130438
        constexpr uint32_t kHookPushAxes = kHookFirst + 144u;    // jal from 0x12B160 in FUN_0012acc8, the push's axes
        // A run's start (FUN_00114140): the corner the stick names, the corner already at, the rope point ahead, the run's
        // angle, and the run along the apron
        constexpr uint32_t kHookRunCorner = kHookFirst + 148u;   // jal from 0x114178, in place of jal FUN_00132d30
        // Going round a corner on the apron (FUN_0013c388; the apron moves in FUN_00114e68)
        constexpr uint32_t kHookApronEnd = kHookFirst + 152u;    // jal from 0x13C3B4 and 0x115094, in place of jal FUN_0013e0e8
        constexpr uint32_t kHookApronAlong = kHookFirst + 156u;  // jal from 0x13C3E4
        constexpr uint32_t kHookApronCorner = kHookFirst + 172u; // jal from 0x13C438
        constexpr uint32_t kHookRunAngle = kHookFirst + 160u;    // jal from 0x114280
        constexpr uint32_t kHookApronRun = kHookFirst + 164u;    // jal from 0x11424C and 0x1143A8
        constexpr uint32_t kHookApronRunWay = kHookFirst + 168u; // jal from 0x114368, in place of jal FUN_0012f220
        constexpr uint32_t kStickCorner = 0x132D30u, kCornerAt = 0x12A5B8u, kRopeAhead = 0x12A698u;
        constexpr uint32_t kNearRope = 0x1144C0u;                // FUN_001144c0: already close to the rope a run would go to
        constexpr uint32_t kRoundCorner = 0x113BF0u;             // FUN_00113bf0: state 3, round a corner on the apron
        // The computer's choices: its distances to the corners, the rope behind it, "beyond a corner"
        constexpr uint32_t kHookCpuCorner1 = kHookFirst + 176u;  // jal from 0x10BB88, in place of sll $v0, $v0, 2
        constexpr uint32_t kHookCpuCorner2 = kHookFirst + 180u;  // jal from 0x10BBD0, the same
        constexpr uint32_t kHookCpuCorner3 = kHookFirst + 184u;  // jal from 0x10CFBC, in place of sll $v0, $v1, 2
        constexpr uint32_t kHookCpuCorner4 = kHookFirst + 188u;  // jal from 0x10E8F0, in place of sll $v0, $v0, 2
        constexpr uint32_t kHookCpuRopeFoot = kHookFirst + 192u; // jal from 0x11276C, in place of jal FUN_00130578
        constexpr uint32_t kHookCpuRopeAngle = kHookFirst + 196u; // jal from 0x112788, in place of jal FUN_0012f220
        constexpr uint32_t kHookCpuBeyond1 = kHookFirst + 200u;  // jal from 0x11299C, in place of lui $at, 0x420C
        constexpr uint32_t kHookCpuBeyond2 = kHookFirst + 204u;  // jal from 0x112B2C, the same
        constexpr uint32_t kHookOutsideCorner = kHookFirst + 208u; // jal from 0x1142B0, in place of addu $v0, $v0, $v1
        constexpr uint32_t kCpuSees = 0x10F5D0u, kRopeDistanceOf = 0x1100B0u, kAngleTo = 0x130578u;
        constexpr uint32_t kCpuCornerExtra = 0x033D0A00u;        // 4 computer records x 12 corners past the fourth
        constexpr uint32_t kCornerReach = 0x294A04u;             // FUN_0012a5b8's four distances, by its second argument
        constexpr uint32_t kGameInAngles = 0x235C30u;            // the game's inward angle by its 0 to 3 side number
        constexpr uint32_t kAngleApart = 0x12F220u;              // FUN_0012f220: the difference of two angles
        constexpr uint32_t kFacingBySide = 0x033D0900u;          // each side's inward angle, read through the game's own sum
        constexpr uint32_t kNoSide = 99u;                        // a side number none of the game's cases match
        constexpr uint32_t kMatchKind = 0x2361CEu;               // the bits of what kind of match this is
        constexpr uint32_t kMatchFlags = 0x2361CCu;              // more of the match setup (0x20: tag partners beside)
        constexpr uint32_t kPosts = 0x033D0800u;              // the floor posts {x, y, z}

        // Fighter fields
        constexpr uint32_t kPlayer = 0x30u, kEdgeTouched = 0x7A0u, kHipsX = 0x15Cu, kHipsZ = 0x164u;

        bool g_log = false;
        uint64_t g_wallSearches = 0;
        PS2Runtime::RecompiledFunction g_linkWallLists = nullptr, g_nearestSide = nullptr, g_ringDraw = nullptr, g_pullRope = nullptr,
                                       g_ropeSpring = nullptr, g_clearRopes = nullptr, g_ropeWindows = nullptr, g_startMove = nullptr, g_whipDirection = nullptr, g_turnTo = nullptr, g_moveBy = nullptr,
                                       g_apronStick = nullptr, g_angleApart = nullptr, g_stickCorner = nullptr, g_cornerAt = nullptr,
                                       g_ropeAhead = nullptr, g_nearRope = nullptr, g_roundCorner = nullptr, g_cpuSees = nullptr,
                                       g_ropeDistanceOf = nullptr, g_angleTo = nullptr;

        // The tables the fight reads the ring's corners from, moved to the store's fixed area where they can hold more
        // entries (config/patches.toml moves the reads; each copy keeps its original's low 16 bits).
        struct Table
        {
            uint32_t game, store, bytes;
        };
        constexpr Table kTables[] = {
            {0x235D00u, 0x03365D00u, 0x30u}, // the corners' directions
            {0x235D30u, 0x03375D30u, 0x30u}, // the corner points {x, y, z}
            {0x235D60u, 0x03385D60u, 0x30u}, // the slump spots {x, y, z}
            {0x235C40u, 0x03395C40u, 0x10u}, // the corners' angles 135 45 -45 -135
            {0x235C50u, 0x033A5C50u, 0x10u}, // the corners' angles -45 -135 135 45
            {0x235CA0u, 0x033B5CA0u, 0x10u}, // the stick bits by corner 3 6 12 9
            {0x235C60u, 0x033E5C60u, 0x20u}, // the directions a whip is sent in: even the sides, odd the corners
        };
        enum { kCornerDirections, kCornerPoints, kSlumpSpots, kCornerOut, kCornerIn, kCornerStick, kWhipDirections };
        constexpr size_t kTableCount = std::size(kTables);
        uint8_t g_gameTables[kTableCount][0x30] = {};
        bool g_tablesCommitted = false, g_gameTablesRead = false;
        // VENDETTA_RINGPOISON=<mask>: those of the game's originals (bit 0 = kTables[0] ...) overwritten, so a read still
        // going there shows
        uint32_t g_poison = 0u;

        // A ring's walls, in the game's own record format, and the lists the wall searches walk
        constexpr uint32_t kWallLists = 0x033C0000u, kWallRecords = 0x033C0400u;
        constexpr uint32_t kRecordBytes = 0x50u;
        constexpr int kMaxSides = 16;

        // The game's own lists and the outside barrier's records (kept: every arena has the same barrier)
        constexpr uint32_t kGameRingLists = 0x295540u, kGameApronLists = 0x2955A0u, kGameFloorLists = 0x235BA8u;
        constexpr uint32_t kGameApronBySide = 0x235DA0u; // the apron's wall list by its side, 0 to 3
        constexpr uint32_t kGameEnd35A = 0x295570u, kGameEnd35B = 0x295578u, kGameEnd33A = 0x2955D0u, kGameEnd33B = 0x2955D8u;
        constexpr uint32_t kBarrierFirst = 0x29B5D0u, kBarrierLast = 0x29B4E0u; // edges 30 and 33, linked 30 31 32 33
        constexpr uint32_t kGameRecordsFirst = 0x29B260u, kGameRecordsCount = 41u;
        constexpr uint32_t kGameDataRecords = 0x235A30u, kGameDataCount = 4u;

        const float kPi = 3.14159265358979f;

        struct Vec
        {
            float x, y, z;
        };

        Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
        Vec cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
        float length(Vec a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

        Ring g_pendingRing, g_ring;
        bool g_hasPending = false, g_active = false, g_testRing = false;

        // Worked out from the description at the match's start
        int g_sides = 0;
        float g_cornerAngle[kMaxSides] = {};       // degrees, atan2(x, z) of each corner; side k runs from corner k to k + 1
        float g_sideNormal[kMaxSides][2] = {};     // each side's outward {x, z}
        float g_sideTangent[kMaxSides][2] = {};    // and its direction along the ropes
        int g_sideBand[kMaxSides] = {};            // the old code's side number for it (0 back, 1 right, 2 front, 3 left)
        uint32_t g_ropes = 0, g_apron = 0, g_outer = 0, g_ends = 0, g_wallsEnd = 0;
        uint32_t g_ringList = 0, g_floorList = 0, g_endLists = 0;
        uint32_t g_apronLists[kMaxSides] = {}; // each apron side's walls, as the game has a list for each of its four
        uint8_t g_direction[4] = {0xFFu, 0xFFu, 0xFFu, 0xFFu}; // each player's rope direction, kept while near that rope

        // VENDETTA_RINGTRACE=<file>: each wall search's fighter, its place, facing, state and ring numbers, for comparing runs
        std::ofstream g_trace;
        uint64_t g_traced = 0;
        constexpr uint64_t kTraceLines = 40000u;

        float readFloat(uint8_t *rdram, uint32_t address) { return std::bit_cast<float>(Ps2FastRead32(rdram, address)); }

        float wrapDegrees(float a)
        {
            while (a > 180.0f)
                a -= 360.0f;
            while (a <= -180.0f)
                a += 360.0f;
            return a;
        }

        // The old code's four groups of directions, as FUN_00130438 gives them on another shape of ring
        int bandOfAngle(float degrees)
        {
            if (degrees > 150.0f)
                return 0;
            if (degrees > 30.0f)
                return 1;
            if (degrees > -30.0f)
                return 2;
            if (degrees > -150.0f)
                return 3;
            return 0;
        }

        int stickBit(int band) { return 1 << (band & 3); }

        int sideOf(float x, float z)
        {
            int best = 0;
            float bestDot = -1e30f;
            for (int k = 0; k < g_sides; ++k)
            {
                const float dot = g_sideNormal[k][0] * x + g_sideNormal[k][1] * z;
                if (dot > bestDot)
                {
                    bestDot = dot;
                    best = k;
                }
            }
            return best;
        }

        int cornerOf(float x, float z)
        {
            int best = 0;
            float bestDot = -1e30f;
            for (int k = 0; k < g_sides; ++k)
            {
                const float rad = g_cornerAngle[k] * kPi / 180.0f;
                const float dot = std::sin(rad) * x + std::cos(rad) * z;
                if (dot > bestDot)
                {
                    bestDot = dot;
                    best = k;
                }
            }
            return best;
        }

        // The corner most in the direction {wayX, wayZ} seen from {x, z}: the one whose own direction from there is
        // closest to it
        int cornerToward(float x, float z, float wayX, float wayZ)
        {
            const float toCorner = g_ring.ropeDistance / std::cos(kPi / g_sides);
            int best = 0;
            float bestDot = -2.0f;
            for (int k = 0; k < g_sides; ++k)
            {
                const float rad = g_cornerAngle[k] * kPi / 180.0f;
                const float dx = std::sin(rad) * toCorner - x, dz = std::cos(rad) * toCorner - z;
                const float dot = (dx * wayX + dz * wayZ) / (std::sqrt(dx * dx + dz * dz) + 0.0001f);
                if (dot > bestDot)
                {
                    bestDot = dot;
                    best = k;
                }
            }
            return best;
        }

        // How far a line from {px, pz} going {wx, wz} runs before it meets the ropes (0 if it never does)
        float ropeHit(float px, float pz, float wx, float wz)
        {
            float nearest = 0.0f;
            bool hit = false;
            for (int k = 0; k < g_sides; ++k)
            {
                const float nx = g_sideNormal[k][0], nz = g_sideNormal[k][1], toward = nx * wx + nz * wz;
                if (toward <= 1e-4f)
                    continue;
                const float t = (g_ring.ropeDistance - (nx * px + nz * pz)) / toward;
                if (t >= 0.0f && (!hit || t < nearest))
                {
                    nearest = t;
                    hit = true;
                }
            }
            return nearest;
        }

        // A wall's +0x30 normal and +0x3C size, as FUN_00130ba8 works them out
        void finishWall(uint8_t *rdram, uint8_t *record)
        {
            Vec c[4];
            std::memcpy(c, record, sizeof(c));
            const float scale = readFloat(rdram, kAreaScale);
            Vec n = cross(c[3] - c[0], c[2] - c[0]);
            const float size = length(n);
            if (size > 0.0f)
                n = {n.x / size, n.y / size, n.z / size};
            const float area = (length(cross(c[1] - c[0], c[2] - c[0])) + length(cross(c[2] - c[0], c[3] - c[0]))) * scale;
            std::memcpy(record + 0x30u, &n, sizeof(n));
            std::memcpy(record + 0x3Cu, &area, sizeof(area));
        }

        // One wall from a to b between two heights, facing `facing`: top(a), top(b), bottom(b), bottom(a), the game's order
        void putWall(uint8_t *rdram, uint32_t at, Vec a, Vec b, float top, float bottom, float facingX, float facingZ, int number,
                     int flags, int more)
        {
            uint8_t *record = vendetta_store::host(at);
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                const Vec c[4] = {{a.x, top, a.z}, {b.x, top, b.z}, {b.x, bottom, b.z}, {a.x, bottom, a.z}};
                std::memcpy(record, c, sizeof(c));
                finishWall(rdram, record);
                Vec n;
                std::memcpy(&n, record + 0x30u, sizeof(n));
                if (n.x * facingX + n.z * facingZ >= 0.0f)
                    break;
                std::swap(a, b);
            }
            const uint16_t numberWord = static_cast<uint16_t>(number), flagsWord = static_cast<uint16_t>(flags);
            const uint32_t moreWord = static_cast<uint32_t>(more), next = 0u, tag = 0u;
            std::memcpy(record + 0x40u, &numberWord, 2);
            std::memcpy(record + 0x42u, &flagsWord, 2);
            std::memcpy(record + 0x44u, &moreWord, 4);
            std::memcpy(record + 0x48u, &next, 4);
            std::memcpy(record + 0x4Cu, &tag, 4);
        }

        void link(uint32_t first, int count)
        {
            for (int i = 0; i + 1 < count; ++i)
            {
                const uint32_t next = first + (i + 1) * kRecordBytes;
                std::memcpy(vendetta_store::host(first + i * kRecordBytes) + 0x48u, &next, 4);
            }
        }

        // A list: { pointer to {first, last}, ..., 0 }. Returns the list's address and moves `at` past it.
        uint32_t putList(uint32_t &at, std::initializer_list<std::pair<uint32_t, uint32_t>> ranges)
        {
            const uint32_t list = at;
            uint32_t pair = list + static_cast<uint32_t>(ranges.size() + 1u) * 4u;
            uint32_t *words = reinterpret_cast<uint32_t *>(vendetta_store::host(list));
            size_t i = 0;
            for (const auto &r : ranges)
            {
                words[i] = pair + static_cast<uint32_t>(i) * 8u;
                uint32_t *p = reinterpret_cast<uint32_t *>(vendetta_store::host(words[i]));
                p[0] = r.first;
                p[1] = r.second;
                ++i;
            }
            words[i] = 0u;
            at = pair + static_cast<uint32_t>(ranges.size()) * 8u;
            return list;
        }

        // The corner tables worked out from the shape: writes `sides` entries of each to `out`[table]
        void makeTables(int sides, float firstAngle, float distance, float slump, float height, uint8_t *out[kTableCount])
        {
            const float toCorner = 1.0f / std::cos(kPi / sides);
            for (int k = 0; k < sides; ++k)
            {
                const float degrees = wrapDegrees(firstAngle - 360.0f * k / sides);
                const float s = std::sin(degrees * kPi / 180.0f), c = std::cos(degrees * kPi / 180.0f);
                const float near0x = std::fabs(s) < 1e-4f ? 0.0f : s, near0z = std::fabs(c) < 1e-4f ? 0.0f : c;
                const Vec direction = {-near0x, 0.0f, -near0z};
                const Vec point = {near0x * distance * toCorner, height, near0z * distance * toCorner};
                const Vec spot = {near0x * slump * toCorner, height, near0z * slump * toCorner};
                const float inward = wrapDegrees(degrees - 180.0f);
                int bits = 0;
                if (near0z < 0.0f)
                    bits |= 1;
                if (near0x > 0.0f)
                    bits |= 2;
                if (near0z > 0.0f)
                    bits |= 4;
                if (near0x < 0.0f)
                    bits |= 8;
                std::memcpy(out[kCornerDirections] + 12 * k, &direction, 12);
                std::memcpy(out[kCornerPoints] + 12 * k, &point, 12);
                std::memcpy(out[kSlumpSpots] + 12 * k, &spot, 12);
                std::memcpy(out[kCornerOut] + 4 * k, &degrees, 4);
                std::memcpy(out[kCornerIn] + 4 * k, &inward, 4);
                std::memcpy(out[kCornerStick] + 4 * k, &bits, 4);
            }
        }

        void buildRing(uint8_t *rdram)
        {
            const Ring &r = g_ring;
            g_sides = r.sides;
            uint8_t *tables[kTableCount];
            for (size_t i = 0; i < kTableCount; ++i)
                tables[i] = vendetta_store::host(kTables[i].store);
            const float firstAngle = 90.0f;
            makeTables(g_sides, firstAngle, r.ropeDistance, r.slumpDistance, r.matHeight, tables);
            // a whip's directions: the game's own for the sides (even), each corner's angle for the corners (odd)
            float *whip = reinterpret_cast<float *>(tables[kWhipDirections]);
            for (int k = 0; k < g_sides; ++k)
                whip[2 * k + 1] = wrapDegrees(firstAngle - 360.0f * k / g_sides);

            const float toCorner = 1.0f / std::cos(kPi / g_sides);
            auto corner = [&](int k, float distance) {
                const float degrees = firstAngle - 360.0f * k / g_sides;
                const float rad = degrees * kPi / 180.0f;
                return Vec{std::sin(rad) * distance * toCorner, 0.0f, std::cos(rad) * distance * toCorner};
            };
            for (int k = 0; k < g_sides; ++k) // the posts on the floor, at the corners of the ring's outer side
            {
                const Vec post = corner(k, r.ringSide);
                std::memcpy(vendetta_store::host(kPosts + k * 12u), &post, sizeof(post));
                const float inward = wrapDegrees(firstAngle - 360.0f * (k + 0.5f) / g_sides + 180.0f);
                std::memcpy(vendetta_store::host(kFacingBySide + k * 4u), &inward, 4);
            }
            for (int k = 0; k < g_sides; ++k)
            {
                g_cornerAngle[k] = wrapDegrees(firstAngle - 360.0f * k / g_sides);
                const float degrees = firstAngle - 360.0f * (k + 0.5f) / g_sides;
                const float rad = degrees * kPi / 180.0f;
                g_sideNormal[k][0] = std::sin(rad);
                g_sideNormal[k][1] = std::cos(rad);
                g_sideTangent[k][0] = -std::cos(rad);
                g_sideTangent[k][1] = std::sin(rad);
                g_sideBand[k] = bandOfAngle(wrapDegrees(degrees));
            }

            // the ropes, the apron's strips, the ring's outer side, and two end walls for each direction of rope
            const float top = r.matHeight + 14.0f;
            uint32_t at = kWallRecords;
            g_ropes = at;
            for (int k = 0; k < g_sides; ++k, at += kRecordBytes)
                putWall(rdram, at, corner(k, r.ropeDistance), corner(k + 1, r.ropeDistance), top, r.matHeight, -g_sideNormal[k][0],
                        -g_sideNormal[k][1], g_sideBand[k] + 1, 0x20 | stickBit(g_sideBand[k]), 0x1A);
            link(g_ropes, g_sides);
            // each apron side a strip as the square's: the ring's side (more 4), its outer edge (2), and a wall at each end (0)
            // where the ring's side ends, so a wrestler on it stops there; four walls a side, in a list of their own
            g_apron = at;
            const float width = r.apronOuter - r.apronInner;
            for (int k = 0; k < g_sides; ++k)
            {
                const float nx = g_sideNormal[k][0], nz = g_sideNormal[k][1], tx = g_sideTangent[k][0], tz = g_sideTangent[k][1];
                const Vec a = corner(k, r.apronInner), b = corner(k + 1, r.apronInner);
                const Vec aOut = {a.x + nx * width, 0.0f, a.z + nz * width}, bOut = {b.x + nx * width, 0.0f, b.z + nz * width};
                const int back = (g_sideBand[k] + 2) & 3, number = 10 + 4 * k;
                const int towardA = bandOfAngle(std::atan2(-tx, -tz) * 180.0f / kPi), towardB = bandOfAngle(std::atan2(tx, tz) * 180.0f / kPi);
                putWall(rdram, at, a, b, top, r.matHeight, nx, nz, number, 0x40 | stickBit(back), 4);
                at += kRecordBytes;
                putWall(rdram, at, aOut, bOut, top, r.matHeight, -nx, -nz, number + 1, 0x40 | stickBit(g_sideBand[k]), 2);
                at += kRecordBytes;
                putWall(rdram, at, a, aOut, top, r.matHeight, tx, tz, number + 2, 0x40 | stickBit(towardA), 0);
                at += kRecordBytes;
                putWall(rdram, at, b, bOut, top, r.matHeight, -tx, -tz, number + 3, 0x40 | stickBit(towardB), 0);
                at += kRecordBytes;
                link(at - 4u * kRecordBytes, 4);
            }
            g_outer = at;
            for (int k = 0; k < g_sides; ++k, at += kRecordBytes)
                putWall(rdram, at, corner(k, r.ringSide), corner(k + 1, r.ringSide), r.matHeight, 0.0f, g_sideNormal[k][0], g_sideNormal[k][1],
                        34 + k, 0x100 | stickBit((g_sideBand[k] + 2) & 3), 0x3D);
            link(g_outer, g_sides);

            const int directions = g_sides / 2;
            const float halfSide = r.ropeDistance * std::tan(kPi / g_sides);
            g_ends = at;
            for (int variant = 0; variant < 2; ++variant) // 0 at the rope's own length, 1 a little in (the game's 35 and 33)
            {
                const float half = variant == 0 ? halfSide : halfSide * 33.0f / 35.0f;
                for (int j = 0; j < directions; ++j)
                    for (int end = 0; end < 2; ++end, at += kRecordBytes)
                    {
                        const float sign = end == 0 ? 1.0f : -1.0f;
                        const float tx = g_sideTangent[j][0] * sign, tz = g_sideTangent[j][1] * sign;
                        const float nx = g_sideNormal[j][0], nz = g_sideNormal[j][1];
                        const Vec a = {tx * half - nx * 1000.0f, 0.0f, tz * half - nz * 1000.0f};
                        const Vec b = {tx * half + nx * 1000.0f, 0.0f, tz * half + nz * 1000.0f};
                        putWall(rdram, at, a, b, top, 0.0f, -tx, -tz, 5 + end, 0xF, 0);
                    }
                for (int j = 0; j < directions; ++j)
                    link(at - (directions - j) * 2 * kRecordBytes, 2);
            }
            g_wallsEnd = at;

            uint32_t list = kWallLists;
            g_ringList = putList(list, {{g_ropes, g_ropes + (g_sides - 1) * kRecordBytes}});
            for (int k = 0; k < g_sides; ++k)
                g_apronLists[k] = putList(list, {{g_apron + 4u * k * kRecordBytes, g_apron + (4u * k + 3u) * kRecordBytes}});
            g_floorList = putList(list, {{g_outer, g_outer + (g_sides - 1) * kRecordBytes}, {kBarrierFirst, kBarrierLast}});
            g_endLists = list;
            for (int variant = 0; variant < 2; ++variant)
                for (int j = 0; j < directions; ++j)
                {
                    const uint32_t first = g_ends + (variant * directions + j) * 2 * kRecordBytes;
                    uint32_t slot = g_endLists + (variant * directions + j) * 32u;
                    putList(slot, {{first, first + kRecordBytes}});
                }
            std::memset(g_direction, 0xFF, sizeof(g_direction));
            if (g_log)
                std::cerr << "[ring] " << g_sides << " sides: " << (g_wallsEnd - kWallRecords) / kRecordBytes << " walls built" << std::endl;
        }

        void fillTables(uint8_t *rdram)
        {
            if (!g_tablesCommitted)
                return;
            if (!g_gameTablesRead) // once: the game builds them at start-up and never changes them
            {
                for (size_t i = 0; i < kTableCount; ++i)
                {
                    std::memcpy(g_gameTables[i], rdram + kTables[i].game, kTables[i].bytes);
                    if (g_poison & (1u << i))
                        std::memset(rdram + kTables[i].game, 0xFF, kTables[i].bytes);
                }
                g_gameTablesRead = true;
                if (g_log)
                {
                    float corner[3];
                    std::memcpy(corner, g_gameTables[kCornerPoints], sizeof(corner));
                    std::cerr << "[ring] the game's first corner: " << corner[0] << ", " << corner[1] << ", " << corner[2]
                              << (g_poison ? " (originals poisoned, mask " + std::to_string(g_poison) + ")" : std::string()) << std::endl;
                }
            }
            for (size_t i = 0; i < kTableCount; ++i)
                std::memcpy(vendetta_store::host(kTables[i].store), g_gameTables[i], kTables[i].bytes);
        }

        // VENDETTA_RINGLOG: the builder's sums against the game's own: every table for a square of the game's numbers, and
        // every one of its wall records' normal and size worked out from its corners
        void checkBuilder(uint8_t *rdram)
        {
            static bool done = false;
            if (done)
                return;
            done = true;

            uint8_t made[kTableCount][0x60] = {};
            uint8_t *out[kTableCount];
            for (size_t i = 0; i < kTableCount; ++i)
                out[i] = made[i];
            makeTables(4, 135.0f, 35.0f, 31.0f, 10.5f, out);
            int tableBad = 0;
            for (size_t i = 0; i < kTableCount; ++i)
            {
                if (i == kCornerDirections || i == kWhipDirections) // not worked out by makeTables
                    continue;
                const uint32_t words = kTables[i].bytes / 4u;
                for (uint32_t w = 0; w < words; ++w)
                {
                    uint32_t a, b;
                    std::memcpy(&a, made[i] + w * 4u, 4);
                    std::memcpy(&b, g_gameTables[i] + w * 4u, 4);
                    const bool same = i == kCornerStick ? a == b
                                                        : std::fabs(std::bit_cast<float>(a) - std::bit_cast<float>(b)) < 0.002f;
                    if (!same)
                    {
                        ++tableBad;
                        std::cerr << "[ring] check: table " << i << " word " << w << ": made " << std::bit_cast<float>(a) << " (" << a
                                  << "), the game's " << std::bit_cast<float>(b) << " (" << b << ")" << std::endl;
                    }
                }
            }

            int walls = 0, wallBad = 0;
            auto checkRecord = [&](uint32_t at) {
                uint8_t copy[kRecordBytes];
                std::memcpy(copy, rdram + at, sizeof(copy));
                Vec gameNormal;
                float gameArea;
                std::memcpy(&gameNormal, copy + 0x30u, 12);
                std::memcpy(&gameArea, copy + 0x3Cu, 4);
                finishWall(rdram, copy);
                Vec normal;
                float area;
                std::memcpy(&normal, copy + 0x30u, 12);
                std::memcpy(&area, copy + 0x3Cu, 4);
                ++walls;
                if (std::fabs(normal.x - gameNormal.x) > 0.0005f || std::fabs(normal.y - gameNormal.y) > 0.0005f ||
                    std::fabs(normal.z - gameNormal.z) > 0.0005f || std::fabs(area - gameArea) > 0.05f)
                {
                    ++wallBad;
                    std::cerr << "[ring] check: wall at " << std::hex << at << std::dec << ": normal " << normal.x << " " << normal.y << " "
                              << normal.z << ", the game's " << gameNormal.x << " " << gameNormal.y << " " << gameNormal.z << "; size " << area
                              << ", the game's " << gameArea << std::endl;
                }
            };
            for (uint32_t i = 0; i < kGameRecordsCount; ++i)
                checkRecord(kGameRecordsFirst + i * kRecordBytes);
            for (uint32_t i = 0; i < kGameDataCount; ++i)
                checkRecord(kGameDataRecords + i * kRecordBytes);
            std::cerr << "[ring] check: tables " << tableBad << " different, walls " << wallBad << " different of " << walls << std::endl;

            // the game's own apron walls, by the apron's side (the lists at 0x235DA0): number, flags, the word after, corners
            for (uint32_t side = 0; side < 4u; ++side)
            {
                const uint32_t list = Ps2FastRead32(rdram, kGameApronBySide + side * 4u);
                for (uint32_t p = list; list != 0u && Ps2FastRead32(rdram, p) != 0u; p += 4u)
                {
                    const uint32_t pair = Ps2FastRead32(rdram, p), last = Ps2FastRead32(rdram, pair + 4u);
                    int count = 0;
                    for (uint32_t r = Ps2FastRead32(rdram, pair); r != 0u && count < 24; ++count,
                                  r = r == last ? 0u : Ps2FastRead32(rdram, r + 0x48u))
                    {
                        const uint32_t numberFlags = Ps2FastRead32(rdram, r + 0x40u);
                        std::cerr << "[ring] the game's apron, side " << side << ": wall " << (numberFlags & 0xFFFFu) << " flags 0x" << std::hex
                                  << (numberFlags >> 16) << " more 0x" << Ps2FastRead32(rdram, r + 0x44u) << std::dec;
                        for (uint32_t c = 0; c < 4u; ++c)
                            std::cerr << " (" << readFloat(rdram, r + c * 12u) << " " << readFloat(rdram, r + c * 12u + 4u) << " "
                                      << readFloat(rdram, r + c * 12u + 8u) << ")";
                        std::cerr << std::endl;
                    }
                }
            }
        }

        void trace(uint8_t *rdram, uint32_t fighter)
        {
            if (g_log && g_poison != 0u) // with the originals poisoned: the first time a fighter's place stops being a number
            {
                static bool told[4] = {};
                const uint32_t player = Ps2FastRead32(rdram, fighter + kPlayer) & 3u;
                if (!told[player] && std::isnan(readFloat(rdram, fighter + 0xCCu)))
                {
                    told[player] = true;
                    std::cerr << "[ring] player " << player << "'s place is not a number: state " << Ps2FastRead32(rdram, fighter + 0x40u)
                              << " step " << Ps2FastRead32(rdram, fighter + 0x44u) << ", the states before";
                    for (uint32_t i = 0; i < 6u; ++i) // the history FUN_00102c20 keeps (30 entries from + 0x54, the newest first)
                        std::cerr << " " << static_cast<int32_t>(Ps2FastRead32(rdram, fighter + 0x54u + 4u * i));
                    std::cerr << ", +0x414 " << static_cast<int32_t>(Ps2FastRead32(rdram, fighter + 0x414u)) << ", +0x41C "
                              << Ps2FastRead32(rdram, fighter + 0x41Cu) << ", +0x5AC 0x" << std::hex
                              << (Ps2FastRead32(rdram, fighter + 0x5ACu) & 0xFFFFu) << std::dec << std::endl;
                }
            }
            if (!g_trace.is_open() || g_traced >= kTraceLines)
                return;
            char line[160];
            std::snprintf(line, sizeof(line), "%08x %08x %08x %08x %08x %08x %08x %u %u %u\n", fighter,
                          Ps2FastRead32(rdram, fighter + 0xCCu), Ps2FastRead32(rdram, fighter + 0xD0u),
                          Ps2FastRead32(rdram, fighter + 0xD4u), Ps2FastRead32(rdram, fighter + 0xDCu),
                          Ps2FastRead32(rdram, fighter + 0x44u), Ps2FastRead32(rdram, fighter + 0x414u),
                          Ps2FastRead32(rdram, fighter + 0x418u), Ps2FastRead32(rdram, fighter + 0x41Cu),
                          Ps2FastRead32(rdram, fighter + 0x420u));
            g_trace << line;
            if (++g_traced % 256u == 0u || g_traced == kTraceLines)
                g_trace.flush();
        }

        // Which direction of rope a fighter is at, for the walls at its two ends: from the wall touched, else the one it
        // was at while still near that rope, else the nearest side
        int ropeDirection(uint8_t *rdram, uint32_t fighter)
        {
            const int directions = g_sides / 2;
            const uint32_t edge = Ps2FastRead32(rdram, fighter + kEdgeTouched);
            const uint32_t player = Ps2FastRead32(rdram, fighter + kPlayer) & 3u;
            if (edge >= g_ropes && edge < g_ends)
            {
                const int side = sideOf(readFloat(rdram, edge + 0x30u), readFloat(rdram, edge + 0x38u));
                return g_direction[player] = static_cast<uint8_t>(side % directions);
            }
            if (edge >= g_ends && edge < g_wallsEnd)
            {
                const int plane = static_cast<int>((edge - g_ends) / kRecordBytes);
                return g_direction[player] = static_cast<uint8_t>((plane / 2) % directions);
            }
            const float x = readFloat(rdram, fighter + kHipsX), z = readFloat(rdram, fighter + kHipsZ);
            const int kept = g_direction[player];
            if (kept < directions)
            {
                const float out = std::fabs(g_sideNormal[kept][0] * x + g_sideNormal[kept][1] * z);
                const float along = std::fabs(g_sideTangent[kept][0] * x + g_sideTangent[kept][1] * z);
                if (along <= out * 0.84f)
                    return kept;
            }
            return g_direction[player] = static_cast<uint8_t>(sideOf(x, z) % directions);
        }

        uint32_t mapList(uint8_t *rdram, uint32_t fighter, uint32_t list)
        {
            if (!g_active)
                return list;
            if (list - kGameRingLists < 0x20u)
                return g_ringList;
            if (list - kGameApronLists < 0x20u)
                return g_apronLists[sideOf(readFloat(rdram, fighter + 0xCCu), readFloat(rdram, fighter + 0xD4u))];
            if (list - kGameFloorLists < 0x40u)
                return g_floorList;
            const int directions = g_sides / 2;
            if (list == kGameEnd35A || list == kGameEnd35B)
                return g_endLists + ropeDirection(rdram, fighter) * 32u;
            if (list == kGameEnd33A || list == kGameEnd33B)
                return g_endLists + (directions + ropeDirection(rdram, fighter)) * 32u;
            return list;
        }

        // ------------------------------------------------------------------ corner moves

        // 0x1150BC and 0x13C05C: the move about to start at a corner gets the real corner in +0x414
        void setCorner(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_active)
            {
                const uint32_t fighter = getRegU32(ctx, 4);
                Ps2FastWrite32(rdram, fighter + 0x414u,
                               static_cast<uint32_t>(cornerOf(readFloat(rdram, fighter + kHipsX), readFloat(rdram, fighter + kHipsZ))));
            }
            g_startMove(rdram, ctx, runtime);
        }

        // FUN_00121fe8(fighter): where a whip sends the opponent: a direction number, even towards a side (the game's own
        // four), odd towards a corner (2 x the corner + 1). The same choices as the game's, over the ring's corners; a corner
        // picked with the stick (one of the stick's eight directions) becomes the corner most that way from the wrestler.
        void whipDirection(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_active)
            {
                g_whipDirection(rdram, ctx, runtime);
                return;
            }
            const uint32_t fighter = getRegU32(ctx, 4);
            const float facing = readFloat(rdram, fighter + 0xDCu), behind = wrapDegrees(facing + 180.0f);
            const float x = readFloat(rdram, fighter + kHipsX), z = readFloat(rdram, fighter + kHipsZ);
            auto gameDirection = [&](uint32_t i) { return readFloat(rdram, kTables[kWhipDirections].game + i * 4u); };
            auto apart = [](float a, float b) { return std::fabs(wrapDegrees(a - b)); };

            uint32_t bestSide = 0u;
            float nearest = 180.0f;
            for (uint32_t i = 0u; i < 8u; i += 2u)
                if (const float d = apart(behind, gameDirection(i)); d < nearest)
                {
                    nearest = d;
                    bestSide = i;
                }

            const float toCorner = g_ring.ropeDistance / std::cos(kPi / g_sides);
            auto cornerX = [&](int k) { return std::sin(g_cornerAngle[k] * kPi / 180.0f) * toCorner; };
            auto cornerZ = [&](int k) { return std::cos(g_cornerAngle[k] * kPi / 180.0f) * toCorner; };
            uint32_t bestCorner = 0u;
            nearest = 180.0f;
            for (int k = 0; k < g_sides; ++k)
            {
                const float angle = std::atan2(cornerX(k) - x, cornerZ(k) - z) * 180.0f / kPi;
                if (const float d = apart(behind, angle); d < nearest)
                {
                    nearest = d;
                    bestCorner = static_cast<uint32_t>(k) * 2u + 1u;
                }
            }

            static const int kStickIndex[16] = {0, 1, 3, 2, 5, 0, 4, 0, 7, 8, 0, 0, 6, 0, 0, 0};
            const int stick = kStickIndex[Ps2FastRead32(rdram, fighter + 0x734u) & 0xFu];
            uint32_t direction = stick == 0 ? bestSide : static_cast<uint32_t>(stick - 1);
            const bool odd = (direction & 1u) != 0u;
            if (apart(gameDirection(direction), facing) < 67.5f)
                direction = odd ? bestCorner : bestSide;
            else if (odd)
            {
                static const float kStick[8][2] = {{0.0f, -1.0f}, {0.7071f, -0.7071f}, {1.0f, 0.0f},  {0.7071f, 0.7071f},
                                                   {0.0f, 1.0f},  {-0.7071f, 0.7071f}, {-1.0f, 0.0f}, {-0.7071f, -0.7071f}};
                const float *way = kStick[direction & 7u];
                direction = static_cast<uint32_t>(cornerToward(x, z, way[0], way[1])) * 2u + 1u;
            }
            // a whip toward a side whose way meets the ropes at one of the ring's corners (within the reach a run's corner
            // is judged by, 0x294A0C) goes into that corner: on a hexagon the game's left and right go straight at its points
            if ((direction & 1u) == 0u)
            {
                const float rad = gameDirection(direction) * kPi / 180.0f, wx = std::sin(rad), wz = std::cos(rad);
                const float t = ropeHit(x, z, wx, wz), hx = x + wx * t, hz = z + wz * t;
                const float reach = readFloat(rdram, kCornerReach + 8u);
                for (int k = 0; k < g_sides; ++k)
                    if (std::hypot(cornerX(k) - hx, cornerZ(k) - hz) <= reach)
                        direction = static_cast<uint32_t>(k) * 2u + 1u;
            }
            SET_GPR_U32(ctx, 2, direction);
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x114F94: addiu $s3, $s0, 0x30: the posts tried for a run into one
        void postLoop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_active)
            {
                SET_GPR_U32(ctx, 16, kPosts);
                SET_GPR_U32(ctx, 19, kPosts + static_cast<uint32_t>(g_sides) * 12u);
            }
            else
                SET_GPR_U32(ctx, 19, getRegU32(ctx, 16) + 0x30u);
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x1197D4 and 0x137184: lui $at, 0x4204, then mtc1 $at, $f1 (run first, as this jal's delay slot): 33, the limit for
        // being inside the ring, tested against |x| and then |z|. On another shape the first test takes the furthest out along
        // any side, so the point has to be inside them all.
        void inRing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ctx->f[1] = 33.0f;
            SET_GPR_U32(ctx, 1, 0x42040000u);
            if (g_active)
            {
                const float x = readFloat(rdram, getRegU32(ctx, 3)), z = readFloat(rdram, getRegU32(ctx, 4));
                float furthest = 0.0f;
                for (int k = 0; k < g_sides; ++k)
                {
                    const float out = g_sideNormal[k][0] * x + g_sideNormal[k][1] * z;
                    if (out > furthest)
                        furthest = out;
                }
                ctx->f[0] = furthest;
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        // ------------------------------------------------------------------ the apron and the start places

        float inwardAngle(int side) { return wrapDegrees(std::atan2(g_sideNormal[side][0], g_sideNormal[side][1]) * 180.0f / kPi + 180.0f); }

        // A point moved straight out or in to the apron's line on its nearest side
        void toApronLine(float &x, float &z)
        {
            const int side = sideOf(x, z);
            const float nx = g_sideNormal[side][0], nz = g_sideNormal[side][1];
            const float by = g_ring.apronLine - (x * nx + z * nz);
            x += nx * by;
            z += nz * by;
        }

        // 0x132210: lw $v1, 0x41C($s0), with swc1 $f0, 0xCC($s0) run first as the delay slot and swc1 $f1, 0xD4($s0) still to come
        void apronPlace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t fighter = getRegU32(ctx, 16);
            if (!g_active)
                SET_GPR_S32(ctx, 3, static_cast<int32_t>(Ps2FastRead32(rdram, fighter + 0x41Cu)));
            else
            {
                float x = ctx->f[0], z = ctx->f[1];
                toApronLine(x, z);
                Ps2FastWrite32(rdram, fighter + 0xCCu, std::bit_cast<uint32_t>(x));
                ctx->f[1] = z;
                SET_GPR_U32(ctx, 3, kNoSide);
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x1322AC: lw $v1, 0x41C($s0): which way a wrestler on the apron faces
        void apronFacing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t fighter = getRegU32(ctx, 16);
            if (!g_active)
                SET_GPR_S32(ctx, 3, static_cast<int32_t>(Ps2FastRead32(rdram, fighter + 0x41Cu)));
            else
            {
                const int side = sideOf(readFloat(rdram, fighter + kHipsX), readFloat(rdram, fighter + kHipsZ));
                Ps2FastWrite32(rdram, fighter + 0xDCu, std::bit_cast<uint32_t>(inwardAngle(side)));
                SET_GPR_U32(ctx, 3, kNoSide);
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x13D91C: swc1 $f1, 0xD0($s0), with lw $v1, 0x41C($s0) run first as the delay slot: standing on the apron
        void apronStance(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t fighter = getRegU32(ctx, 16);
            Ps2FastWrite32(rdram, fighter + 0xD0u, std::bit_cast<uint32_t>(ctx->f[1]));
            if (g_active)
            {
                float x = readFloat(rdram, fighter + kHipsX), z = readFloat(rdram, fighter + kHipsZ);
                Ps2FastWrite32(rdram, fighter + 0xDCu, std::bit_cast<uint32_t>(inwardAngle(sideOf(x, z))));
                toApronLine(x, z);
                Ps2FastWrite32(rdram, fighter + 0xCCu, std::bit_cast<uint32_t>(x));
                Ps2FastWrite32(rdram, fighter + 0xD4u, std::bit_cast<uint32_t>(z));
                SET_GPR_U32(ctx, 3, kNoSide);
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x102BA8: addu $v0, $v0, $s5, with lwc1 $f0, 0($v0) run first as the delay slot (so done again here): the angle by side
        void apronLook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t at = getRegU32(ctx, 2) + getRegU32(ctx, 21);
            SET_GPR_U32(ctx, 2, at);
            ctx->f[0] = readFloat(rdram, at);
            if (g_active)
            {
                const uint32_t fighter = getRegU32(ctx, 16);
                ctx->f[0] = inwardAngle(sideOf(readFloat(rdram, fighter + kHipsX), readFloat(rdram, fighter + kHipsZ)));
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x12AD50: lw $v0, 0x41C($s4): the wall search's point for a fighter on the apron, kept before the game moves one
        // axis of it to the hips; 0x12AD78 (lui $at, 0x4090) then puts it on the side, level with the hips
        float g_probeX = 0.0f, g_probeZ = 0.0f;
        void apronProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            SET_GPR_S32(ctx, 2, static_cast<int32_t>(Ps2FastRead32(rdram, getRegU32(ctx, 20) + 0x41Cu)));
            if (g_active)
            {
                const uint32_t sp = getRegU32(ctx, 29);
                g_probeX = readFloat(rdram, sp);
                g_probeZ = readFloat(rdram, sp + 8u);
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        void apronProbe2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            SET_GPR_U32(ctx, 1, 0x40900000u);
            ctx->f[21] = 4.5f;
            if (g_active)
            {
                const uint32_t sp = getRegU32(ctx, 29), fighter = getRegU32(ctx, 20);
                const int side = sideOf(g_probeX, g_probeZ);
                const float tx = g_sideTangent[side][0], tz = g_sideTangent[side][1];
                const float along = (readFloat(rdram, fighter + kHipsX) - g_probeX) * tx + (readFloat(rdram, fighter + kHipsZ) - g_probeZ) * tz;
                Ps2FastWrite32(rdram, sp, std::bit_cast<uint32_t>(g_probeX + tx * along));
                Ps2FastWrite32(rdram, sp + 8u, std::bit_cast<uint32_t>(g_probeZ + tz * along));
                ctx->f[21] = 4.4375f;
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        // ------------------------------------------------------------------ the start places and the intros

        // A place by one of the square's corners, moved to the ring: to the ring's corner nearest that corner's angle (the
        // near-left, near-right, far-left and far-right of a hexagon), the same distance in from that corner's two ropes
        // (each of the square's ropes standing for the ring's side beside the corner whose normal is nearest its own), and
        // the turn for what is done round it: that of the rope it is nearer to, in between by the square of the distances
        // (on a hexagon 0 by a front or back rope, 30 by a slanted one, 15 in the corner's middle)
        struct PlaceMove
        {
            float squareX, squareZ, ringX, ringZ, c, s, turn;
            bool ok;
        };

        float normalAngle(float x, float z) { return std::atan2(x, z) * 180.0f / kPi; }

        PlaceMove placeMoveFor(float x, float z)
        {
            PlaceMove m{x, z, x, z, 1.0f, 0.0f, 0.0f, false};
            const float sx = x < 0.0f ? -1.0f : 1.0f, sz = z < 0.0f ? -1.0f : 1.0f, squareAngle = normalAngle(sx, sz);
            auto apart = [](float a, float b) { return std::fabs(wrapDegrees(a - b)); };
            int corner = 0;
            for (int k = 1; k < g_sides; ++k)
                if (apart(g_cornerAngle[k], squareAngle) < apart(g_cornerAngle[corner], squareAngle))
                    corner = k;
            // the square's two ropes there: across x (normal {sx, 0}) and across z ({0, sz}); how far in from each
            const float inX = 35.0f - x * sx, inZ = 35.0f - z * sz, R = g_ring.ropeDistance;
            const int before = (corner + g_sides - 1) % g_sides, after = corner; // the ring's sides ending and starting there
            const float xAngle = normalAngle(sx, 0.0f), zAngle = normalAngle(0.0f, sz);
            const float beforeAngle = normalAngle(g_sideNormal[before][0], g_sideNormal[before][1]);
            const float afterAngle = normalAngle(g_sideNormal[after][0], g_sideNormal[after][1]);
            const bool xIsBefore = apart(beforeAngle, xAngle) <= apart(afterAngle, xAngle);
            const int sideX = xIsBefore ? before : after, sideZ = xIsBefore ? after : before;
            // n1 . q = R - inX, n2 . q = R - inZ
            const float a = g_sideNormal[sideX][0], b = g_sideNormal[sideX][1], c = g_sideNormal[sideZ][0], d = g_sideNormal[sideZ][1];
            const float e = R - inX, f = R - inZ, det = a * d - b * c;
            if (std::fabs(det) <= 1e-4f)
                return m;
            m.ringX = (e * d - b * f) / det;
            m.ringZ = (a * f - e * c) / det;
            const float turnX = wrapDegrees(normalAngle(a, b) - xAngle), turnZ = wrapDegrees(normalAngle(c, d) - zAngle);
            const float nearX = inZ * inZ, nearZ = inX * inX; // the nearer the rope, the more its turn counts
            m.turn = nearX + nearZ > 0.0f ? (turnX * nearX + turnZ * nearZ) / (nearX + nearZ) : 0.5f * (turnX + turnZ);
            m.c = std::cos(m.turn * kPi / 180.0f);
            m.s = std::sin(m.turn * kPi / 180.0f);
            m.ok = true;
            return m;
        }

        // A point near the place: the place itself goes to its ring place, the rest turned with it
        void applyMove(const PlaceMove &m, float &x, float &z)
        {
            const float dx = x - m.squareX, dz = z - m.squareZ;
            x = m.ringX + dx * m.c + dz * m.s;
            z = m.ringZ + dz * m.c - dx * m.s;
        }

        // The end of FUN_0013eaa8 (a wrestler's start place, $a2 the fighter): its tables (0x29E348 on, filled at its first
        // use; y 10.5 in all) by the match kind 0x2361CE and the player, every place by one of the square's corners: a 1-on-1
        // in the near-left and far-right corners 25 in on each axis, tag's legal men the same with the partners on the apron
        // behind them (38.5) or (0x2361CC 0x20) beside them along their corner's ropes, handicap's pair or three round the
        // far-right corner, 3-way and 4-way one in each corner
        void startExit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_active)
            {
                const uint32_t fighter = getRegU32(ctx, 6);
                const PlaceMove m = placeMoveFor(readFloat(rdram, fighter + 0xCCu), readFloat(rdram, fighter + 0xD4u));
                if (m.ok)
                {
                    Ps2FastWrite32(rdram, fighter + 0xCCu, std::bit_cast<uint32_t>(m.ringX));
                    Ps2FastWrite32(rdram, fighter + 0xD4u, std::bit_cast<uint32_t>(m.ringZ));
                }
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        // The square's start place of a player in this kind of match, as FUN_0013eaa8 picks it (a later kind wins, as there)
        void squarePlaceOf(uint8_t *rdram, uint32_t player, float &x, float &z)
        {
            static const float kOne[4][2] = {{-25.0f, 25.0f}, {25.0f, -25.0f}, {25.0f, 25.0f}, {-25.0f, -25.0f}};
            static const float kAll[4][2] = {{-25.0f, 25.0f}, {25.0f, 25.0f}, {-25.0f, -25.0f}, {25.0f, -25.0f}};
            static const float kTagBeside[4][2] = {{-17.0f, 27.0f}, {-27.0f, 17.0f}, {27.0f, -17.0f}, {17.0f, -27.0f}};
            static const float kTagApron[4][2] = {{-25.0f, 25.0f}, {-25.0f, 38.5f}, {25.0f, -25.0f}, {25.0f, -38.5f}};
            static const float kTwoOnOne[4][2] = {{-25.0f, 25.0f}, {27.0f, -17.0f}, {17.0f, -27.0f}, {17.0f, -27.0f}};
            static const float kThreeOnOne[4][2] = {{-25.0f, 25.0f}, {15.0f, -30.0f}, {21.0f, -21.0f}, {30.0f, -15.0f}};
            const uint8_t kind = rdram[kMatchKind];
            const float *pick = kOne[player & 3u];
            if (kind & 0x0Cu)
                pick = kAll[player & 3u];
            if (kind & 2u)
                pick = (rdram[kMatchFlags] & 0x20u) ? kTagBeside[player & 3u] : kTagApron[player & 3u];
            if (kind & 0x10u)
                pick = kTwoOnOne[player & 3u];
            if (kind & 0x20u)
                pick = kThreeOnOne[player & 3u];
            x = pick[0];
            z = pick[1];
        }

        // An intro's scene (FUN_001245c8 builds them all at 0x29AF48 on: FUN_00124ae0 one wrestler, FUN_00124cc0 a tag pair,
        // FUN_00124fd8 a handicap pair, FUN_00125158 a trio; entries of 0x20 bytes {player, place x y z, angles x y z, clip},
        // the camera path at + 0x80, its flags at + 0x84) puts each wrestler at the middle facing by its spot, and the clip
        // carries him to his corner of the square, filmed by a camera path fixed in the world (PAK 0). On the ring each
        // wrestler's place and facing in the scene move by his start place's move (above), the scene's camera path by its
        // first wrestler's; the path is put back at the next scene and at each match's start.
        constexpr uint32_t kIntroScenes = 0x29AF48u, kIntroScenesBytes = 700u, kSceneStart = 0x1252A0u;
        constexpr uint32_t kCameraTable = 0x140FD0Cu; // path n's self-relative offset at + 4n; {last frame, key count, keys}

        uint32_t g_movedPath = 0u;            // the camera path whose keys are moved (its data's address; 0: none)
        std::vector<uint32_t> g_movedPathKeys; // their eye x, z and target x, z as they were
        std::unordered_map<uint32_t, std::array<uint32_t, 3>> g_movedEntries; // a scene entry: what was written (x, z, facing)
        PS2Runtime::RecompiledFunction g_sceneStart = nullptr;

        void putPathBack(uint8_t *rdram)
        {
            for (size_t k = 0; g_movedPath != 0u && 4u * k + 3u < g_movedPathKeys.size(); ++k)
            {
                const uint32_t key = g_movedPath + 8u + 0x24u * static_cast<uint32_t>(k);
                Ps2FastWrite32(rdram, key + 0x04u, g_movedPathKeys[4 * k]);
                Ps2FastWrite32(rdram, key + 0x0Cu, g_movedPathKeys[4 * k + 1]);
                Ps2FastWrite32(rdram, key + 0x10u, g_movedPathKeys[4 * k + 2]);
                Ps2FastWrite32(rdram, key + 0x18u, g_movedPathKeys[4 * k + 3]);
            }
            g_movedPath = 0u;
            g_movedPathKeys.clear();
        }

        void movePath(uint8_t *rdram, uint32_t path, const PlaceMove &m)
        {
            if (path >= 1024u)
                return;
            const uint32_t slot = kCameraTable + path * 4u, data = slot + Ps2FastRead32(rdram, slot);
            const uint32_t count = Ps2FastRead32(rdram, data + 4u);
            if (count == 0u || count > 64u)
                return;
            g_movedPath = data;
            for (uint32_t k = 0; k < count; ++k)
            {
                const uint32_t key = data + 8u + 0x24u * k;
                for (const uint32_t at : {0x04u, 0x0Cu, 0x10u, 0x18u})
                    g_movedPathKeys.push_back(Ps2FastRead32(rdram, key + at));
                for (const uint32_t at : {0x04u, 0x10u}) // the eye, the target
                {
                    float x = readFloat(rdram, key + at), z = readFloat(rdram, key + at + 8u);
                    applyMove(m, x, z);
                    Ps2FastWrite32(rdram, key + at, std::bit_cast<uint32_t>(x));
                    Ps2FastWrite32(rdram, key + at + 8u, std::bit_cast<uint32_t>(z));
                }
            }
        }

        // FUN_001252a0(scene): a scene's start (its camera path into 0x22B50C, then each wrestler in it placed by
        // FUN_00125568, the others moved away)
        void sceneStart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            putPathBack(rdram);
            const uint32_t scene = getRegU32(ctx, 4);
            // the arena's fly-in is built in the same place before the intros, but places no one (flags + 0x84 bit 1)
            const bool placesWrestlers = (Ps2FastRead32(rdram, scene + 0x84u) & 1u) == 0u;
            if (g_active && scene - kIntroScenes < kIntroScenesBytes && placesWrestlers)
            {
                bool camera = true;
                for (uint32_t i = 0; i < 4u; ++i)
                {
                    const uint32_t entry = scene + i * 0x20u, player = Ps2FastRead32(rdram, entry);
                    if (player > 3u)
                        continue;
                    float px, pz;
                    squarePlaceOf(rdram, player, px, pz);
                    const PlaceMove m = placeMoveFor(px, pz);
                    if (!m.ok)
                        continue;
                    if (camera)
                    {
                        movePath(rdram, Ps2FastRead32(rdram, scene + 0x80u), m);
                        camera = false;
                    }
                    const std::array<uint32_t, 3> now = {Ps2FastRead32(rdram, entry + 0x04u), Ps2FastRead32(rdram, entry + 0x0Cu),
                                                         Ps2FastRead32(rdram, entry + 0x14u)};
                    if (const auto it = g_movedEntries.find(entry); it != g_movedEntries.end() && it->second == now)
                        continue; // this scene's entry already moved
                    float x = std::bit_cast<float>(now[0]), z = std::bit_cast<float>(now[1]);
                    applyMove(m, x, z);
                    const float facing = wrapDegrees(std::bit_cast<float>(now[2]) + m.turn);
                    Ps2FastWrite32(rdram, entry + 0x04u, std::bit_cast<uint32_t>(x));
                    Ps2FastWrite32(rdram, entry + 0x0Cu, std::bit_cast<uint32_t>(z));
                    Ps2FastWrite32(rdram, entry + 0x14u, std::bit_cast<uint32_t>(facing));
                    g_movedEntries[entry] = {std::bit_cast<uint32_t>(x), std::bit_cast<uint32_t>(z), std::bit_cast<uint32_t>(facing)};
                    if (g_log)
                        std::cerr << "[ring] intro scene " << std::hex << scene << std::dec << ", player " << player << " (start "
                                  << px << ", " << pz << " -> " << m.ringX << ", " << m.ringZ << ", turned " << m.turn
                                  << "): placed at " << x << ", " << z << " facing " << facing << ", camera path "
                                  << Ps2FastRead32(rdram, scene + 0x80u) << std::endl;
                }
            }
            g_sceneStart(rdram, ctx, runtime);
        }

        // ------------------------------------------------------------------ angles by side

        // The game's two tables of angles by side (0x235C20, 0x235C30): on six sides the old 0 to 3 number covers up to two real
        // sides, so the angle also goes by which quarter the wrestler is in
        const float kAngleBySideAndQuarter[16] = {180.0f, 60.0f,  0.0f, -120.0f, 0.0f, -120.0f, 180.0f, 60.0f,
                                                  180.0f, 120.0f, 0.0f, -60.0f,  0.0f, -60.0f,  180.0f, 120.0f};

        float angleFor(uint8_t *rdram, uint32_t address, uint32_t fighter)
        {
            if (!g_active)
                return readFloat(rdram, address);
            uint32_t at = address & 0x1Fu;
            if (Ps2FastRead32(rdram, fighter + kHipsX) >> 31)
                at |= 32u;
            if (Ps2FastRead32(rdram, fighter + kHipsZ) >> 31)
                at ^= 32u;
            return kAngleBySideAndQuarter[at / 4u];
        }

        // 0x115AC8: lwc1 $f0, 0($a0)
        void angleF0A0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ctx->f[0] = angleFor(rdram, getRegU32(ctx, 4), getRegU32(ctx, 16));
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x115C24 and 0x115E0C: lwc1 $f0, 0($v0)
        void angleF0V0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ctx->f[0] = angleFor(rdram, getRegU32(ctx, 2), getRegU32(ctx, 16));
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x115DB4: jal FUN_00101928, with lwc1 $f12, 0($v0) as its delay slot
        void angleThenTurn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_active)
                ctx->f[12] = angleFor(rdram, getRegU32(ctx, 2), getRegU32(ctx, 16));
            g_turnTo(rdram, ctx, runtime);
        }

        // 0x123DA8: addu $v0, $v0, $v1, with lwc1 $f0, 0($v0) as this jal's delay slot, so done after the sum here
        void angleAdd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t at = getRegU32(ctx, 2) + getRegU32(ctx, 3);
            SET_GPR_U32(ctx, 2, at);
            ctx->f[0] = angleFor(rdram, at, getRegU32(ctx, 16));
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x1374D4: the read of $f12 moved here, the instructions after it each moved up one (config/patches.toml)
        void angleF12(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ctx->f[12] = angleFor(rdram, getRegU32(ctx, 2), getRegU32(ctx, 16));
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x138DCC: the read of $f20, with addu $v0, $v0, $v1 moved into this jal's delay slot
        void angleF20(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ctx->f[20] = angleFor(rdram, getRegU32(ctx, 2), getRegU32(ctx, 17));
            ctx->pc = getRegU32(ctx, 31);
        }

        // ------------------------------------------------------------------ lining up a pair for a move

        // The side a pair is at: the old 0 to 3 number, and which half of the ring the point is in
        int sideFromBand(uint32_t band, float z)
        {
            static const int kSides[4][2] = {{4, 4}, {0, 5}, {1, 1}, {2, 3}};
            return kSides[band][z < 0.0f ? 1 : 0];
        }

        float outAngle(int side) { return wrapDegrees(std::atan2(g_sideNormal[side][0], g_sideNormal[side][1]) * 180.0f / kPi); }

        void placeTarget(uint8_t *rdram, uint32_t sp, int side, uint32_t point, float distance, bool straightOut)
        {
            const float nx = g_sideNormal[side][0], nz = g_sideNormal[side][1];
            const float x = readFloat(rdram, point), z = readFloat(rdram, point + 8u);
            float tx, tz;
            if (straightOut)
            {
                tx = x + nx * distance;
                tz = z + nz * distance;
            }
            else
            {
                const float reach = std::tan(kPi / g_sides) * distance;
                float across = z * nx - x * nz;
                across = across > reach ? reach : (across < -reach ? -reach : across);
                tx = distance * nx - across * nz;
                tz = distance * nz + across * nx;
            }
            Ps2FastWrite32(rdram, sp, std::bit_cast<uint32_t>(tx));
            Ps2FastWrite32(rdram, sp + 8u, std::bit_cast<uint32_t>(tz));
        }

        struct PairTableWord
        {
            uint32_t at, original;
        };
        const PairTableWord kPairTable[9] = {{0x27BC38u, 0x134060u}, {0x27BC3Cu, 0x13410Cu}, {0x27BC40u, 0x1341C4u},
                                             {0x27BC44u, 0x134280u}, {0x27BBF4u, 0x1344C0u}, {0x27BBBCu, 0x1342BCu},
                                             {0x27BBC0u, 0x134384u}, {0x27BCE4u, 0x1345F4u}, {0x27BBD0u, 0x133D38u}};

        void setPairTable(uint8_t *rdram, bool active)
        {
            for (uint32_t i = 0; i < 9u; ++i)
                Ps2FastWrite32(rdram, kPairTable[i].at, active ? kHookPairFirst + i * 4u : kPairTable[i].original);
        }

        void runPairCase(uint8_t *rdram, R5900Context *ctx, int id)
        {
            const uint32_t a = getRegU32(ctx, 16), b = getRegU32(ctx, 17), sp = getRegU32(ctx, 29); // $s0, $s1
            auto word = [&](uint32_t address) { return Ps2FastRead32(rdram, address); };
            auto half = [&](uint32_t address) { return word(address & ~3u) >> ((address & 2u) * 8u) & 0xFFFFu; };
            auto finish = [&](float facing, int frames) {
                ctx->f[20] = facing;
                SET_GPR_U64(ctx, 19, 0u);
                SET_GPR_U64(ctx, 18, static_cast<uint64_t>(frames));
                ctx->pc = kPairMerge;
            };
            auto zOf = [&](uint32_t point) { return readFloat(rdram, point + 8u); };
            auto bad = [&]() { ctx->pc = kPairTable[id].original; };
            switch (id)
            {
            case 0:
            {
                const uint32_t band = word(b + 0x41Cu), point = a + 0x15Cu;
                if (band >= 4u) return bad();
                const int side = sideFromBand(band, zOf(point));
                placeTarget(rdram, sp, side, point, 46.5f, false);
                return finish(inwardAngle(side), 30);
            }
            case 1:
            {
                const uint32_t band = word(a + 0x41Cu), point = b + 0x15Cu;
                if (band >= 4u) return bad();
                const int side = sideFromBand(band, zOf(point));
                placeTarget(rdram, sp, side, point, 31.0f, false);
                return finish((half(a + 0x5A6u) & 0x200u) ? inwardAngle(side) : outAngle(side), 30);
            }
            case 2:
            {
                const uint32_t band = word(b + 0x41Cu), point = a + 0xCCu;
                if (band >= 4u) return bad();
                const int side = sideFromBand(band, zOf(point));
                placeTarget(rdram, sp, side, point, 9.0f, true);
                return finish(inwardAngle(side), 30);
            }
            case 3:
            {
                const uint32_t band = word(a + 0x41Cu), point = b + 0xCCu;
                if (band >= 4u) return bad();
                const int side = sideFromBand(band, zOf(point));
                placeTarget(rdram, sp, side, point, 0.0f, true);
                return finish(inwardAngle(side), 30);
            }
            case 4:
            {
                const uint32_t band = word(a + 0x41Cu), point = a + 0xCCu;
                if (band >= 4u) return bad();
                const int side = sideFromBand(band, zOf(point));
                placeTarget(rdram, sp, side, point, -5.5f, true);
                return finish(inwardAngle(side), 20);
            }
            case 5:
            {
                const bool held = (half(b + 0x5A6u) & 0x20u) != 0u;
                const uint32_t band = word(b + 0x414u), point = held ? b + 0xCCu : a + 0x138u;
                if (band >= 4u) return bad();
                const int side = sideFromBand(band, zOf(point));
                placeTarget(rdram, sp, side, point, held ? 0.0f : 30.0f, held);
                return finish(held ? inwardAngle(side) : outAngle(side), 12);
            }
            case 6:
            {
                const bool held = (half(a + 0x5A6u) & 0x20u) != 0u;
                const uint32_t band = word(a + 0x414u), point = held ? a + 0xCCu : b + 0xCCu;
                if (band >= 4u) return bad();
                const int side = sideFromBand(band, zOf(point));
                placeTarget(rdram, sp, side, point, held ? -8.5f : 0.0f, true);
                return finish(held ? outAngle(side) : inwardAngle(side), 12);
            }
            case 7:
            {
                const uint32_t band = word(a + 0x41Cu), point = a + 0xCCu;
                if (static_cast<int32_t>(word(b + 0x44u)) >= 2 || band >= 4u) return bad();
                const int side = sideFromBand(band, zOf(point));
                Ps2FastWrite32(rdram, sp + 0x28u, std::bit_cast<uint32_t>(8.0f));
                Ps2FastWrite32(rdram, sp + 0x10u, word(a + 0xCCu));
                Ps2FastWrite32(rdram, sp + 0x14u, word(a + 0xD0u));
                Ps2FastWrite32(rdram, sp + 0x18u, word(a + 0xD4u));
                ctx->f[21] = inwardAngle(side);
                ctx->f[20] = outAngle(side);
                SET_GPR_U64(ctx, 18, 15u);
                ctx->pc = kPairMerge;
                return;
            }
            default: // the corner: a spot just inside it
            {
                const uint32_t corner = word(b + 0x414u);
                SET_GPR_U64(ctx, 19, 0u);
                SET_GPR_U64(ctx, 18, 12u);
                if (corner < static_cast<uint32_t>(g_sides))
                {
                    const float rad = g_cornerAngle[corner] * kPi / 180.0f, radius = 34.3f / std::cos(kPi / g_sides);
                    Ps2FastWrite32(rdram, sp, std::bit_cast<uint32_t>(std::sin(rad) * radius));
                    Ps2FastWrite32(rdram, sp + 8u, std::bit_cast<uint32_t>(std::cos(rad) * radius));
                    ctx->pc = kCornerCaseResume;
                }
                else
                    bad();
                return;
            }
            }
        }

        template <int Id>
        void pairCase(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            runPairCase(rdram, ctx, Id);
        }

        // ------------------------------------------------------------------ getting in and out of the ring

        int hipsSide(uint8_t *rdram, uint32_t fighter)
        {
            return sideOf(readFloat(rdram, fighter + kHipsX), readFloat(rdram, fighter + kHipsZ));
        }

        // 0x124020: jal FUN_0012f220, with lwc1 $f13, 0($v0) as its delay slot: the turn to make, against the ring's own side
        void stepTurn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_active)
                ctx->f[13] = outAngle(hipsSide(rdram, getRegU32(ctx, 17)));
            g_angleApart(rdram, ctx, runtime);
        }

        // 0x12415C: lwc1 $f12, 0($v0)
        void stepAngle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_active)
                ctx->f[12] = readFloat(rdram, getRegU32(ctx, 2));
            else
            {
                const float out = outAngle(hipsSide(rdram, getRegU32(ctx, 17)));
                ctx->f[12] = wrapDegrees(static_cast<int32_t>(getRegU32(ctx, 16)) > 0 ? out - 90.0f : out + 90.0f);
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x124170: lw $v1, 0x414($s1): the step over the ropes, straight in along the ring's own side
        void stepMove(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t fighter = getRegU32(ctx, 17);
            if (!g_active)
                SET_GPR_S32(ctx, 3, static_cast<int32_t>(Ps2FastRead32(rdram, fighter + 0x414u)));
            else
            {
                const float x = readFloat(rdram, fighter + kHipsX), z = readFloat(rdram, fighter + kHipsZ);
                const int side = sideOf(x, z);
                const float nx = g_sideNormal[side][0], nz = g_sideNormal[side][1];
                const int frames = static_cast<int32_t>(getRegU32(ctx, 16));
                const float each = (30.5f - (x * nx + z * nz)) / static_cast<float>(frames != 0 ? frames : 1);
                Ps2FastWrite32(rdram, fighter + 0x588u, std::bit_cast<uint32_t>(nx * each));
                Ps2FastWrite32(rdram, fighter + 0x590u, std::bit_cast<uint32_t>(nz * each));
                SET_GPR_U32(ctx, 3, kNoSide);
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x13F000: jal FUN_00130438: the code after it adds the number x 4 to $s5 and reads an angle there, so on another
        // shape the number is whatever lands on this side's inward angle
        void startFacing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_active)
            {
                g_nearestSide(rdram, ctx, runtime);
                return;
            }
            const uint32_t point = getRegU32(ctx, 4);
            const int side = sideOf(readFloat(rdram, point), readFloat(rdram, point + 8u));
            SET_GPR_U32(ctx, 2, (kFacingBySide + static_cast<uint32_t>(side) * 4u - getRegU32(ctx, 21)) >> 2);
            ctx->pc = getRegU32(ctx, 31);
        }

        // ------------------------------------------------------------------ walking along the apron

        // The stick's bits as a direction {x, z}
        void stickDirection(uint32_t bits, float &x, float &z)
        {
            x = static_cast<float>(static_cast<int>((bits >> 1) & 1u) - static_cast<int>((bits >> 3) & 1u));
            z = static_cast<float>(static_cast<int>((bits >> 2) & 1u) - static_cast<int>(bits & 1u));
        }

        // FUN_0013e0e8(fighter, stick): 1 or 2 for the stick along the apron's side, either way; 0 otherwise
        void apronStick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_active)
            {
                g_apronStick(rdram, ctx, runtime);
                return;
            }
            const uint32_t fighter = getRegU32(ctx, 4);
            const int side = sideOf(readFloat(rdram, fighter + 0xCCu), readFloat(rdram, fighter + 0xD4u));
            float x, z;
            stickDirection(getRegU32(ctx, 5) & 0xFu, x, z);
            const float along = x * g_sideTangent[side][0] + z * g_sideTangent[side][1];
            SET_GPR_U64(ctx, 2, along > 0.25f ? 2u : (along < -0.25f ? 1u : 0u));
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x13DA54: jal FUN_0013e130(fighter, step): a step along the apron follows the ring's side, its end walls stopping it
        void apronWalk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_active)
            {
                const uint32_t fighter = getRegU32(ctx, 16), step = getRegU32(ctx, 5);
                const float px = readFloat(rdram, fighter + 0xCCu), pz = readFloat(rdram, fighter + 0xD4u);
                const int side = sideOf(px, pz);
                float x, z;
                stickDirection(getRegU32(ctx, 17) & 0xFu, x, z);
                const float tx = g_sideTangent[side][0], tz = g_sideTangent[side][1];
                const float nx = g_sideNormal[side][0], nz = g_sideNormal[side][1];
                const float dotT = x * tx + z * tz, dotN = x * nx + z * nz;
                // nothing stops the step at the side's end: its end wall does, as on the square, and the wrestler held
                // against it is what lets the move round the corner start (FUN_0013c388 reads the walls touched)
                const float along = dotT > 0.25f ? 1.0f : (dotT < -0.25f ? -1.0f : 0.0f);
                const float away = dotN > 0.25f ? 1.0f : (dotN < -0.25f ? -1.0f : 0.0f);
                const float speed = ctx->f[1];
                Ps2FastWrite32(rdram, step, std::bit_cast<uint32_t>((tx * along + nx * away) * speed));
                Ps2FastWrite32(rdram, step + 8u, std::bit_cast<uint32_t>((tz * along + nz * away) * speed));
            }
            g_moveBy(rdram, ctx, runtime);
        }

        // ------------------------------------------------------------------ going round a corner on the apron

        // 0x13C3B4 and 0x115094: jal FUN_0013e0e8(fighter, the bits of the walls touched, +0x79C): which way along the apron's
        // side those walls lie, 1 or 2, else 0: on the square only its end walls lie along a side. On the ring a slanted
        // side's own walls have bits along it too, so it is read from the walls touched: an end wall of the side, 1 at its
        // start (corner k, the -1 way), 2 at its end.
        void apronEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_active)
            {
                g_apronStick(rdram, ctx, runtime);
                return;
            }
            const uint32_t fighter = getRegU32(ctx, 4);
            uint64_t way = 0u;
            if ((getRegU32(ctx, 5) & 0xFFFFu) != 0u)
                for (const uint32_t edge : {Ps2FastRead32(rdram, fighter + 0x7A0u), Ps2FastRead32(rdram, fighter + 0x7A4u)})
                    if (edge >= g_apron && edge < g_apron + 4u * static_cast<uint32_t>(g_sides) * kRecordBytes)
                    {
                        const uint32_t wall = ((edge - g_apron) / kRecordBytes) % 4u;
                        if (wall == 2u)
                            way = 1u;
                        else if (wall == 3u)
                            way = 2u;
                    }
            SET_GPR_U64(ctx, 2, way);
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x13C3E4: or $v1, $v1, $a1, the stick bits along the apron's side either way (0x295658 / 0x295660 by its old side
        // number), with and $v0, $v0, $v1 run first as this jal's delay slot: $v0 = the stick held along the side, else 0.
        // On the ring: along the ring's own side.
        void apronAlong(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t fighter = getRegU32(ctx, 16), stick = Ps2FastRead32(rdram, fighter + 0x668u);
            if (!g_active)
            {
                const uint32_t mask = getRegU32(ctx, 3) | getRegU32(ctx, 5);
                SET_GPR_U32(ctx, 3, mask);
                SET_GPR_U32(ctx, 2, stick & mask);
            }
            else
            {
                const int side = sideOf(readFloat(rdram, fighter + 0xCCu), readFloat(rdram, fighter + 0xD4u));
                float x, z;
                stickDirection(stick & 0xFu, x, z);
                const float size = std::sqrt(x * x + z * z);
                const bool along = size > 0.0f && std::fabs(x * g_sideTangent[side][0] + z * g_sideTangent[side][1]) / size > 0.3f;
                SET_GPR_U32(ctx, 3, along ? 1u : 0u);
                SET_GPR_U32(ctx, 2, along ? 1u : 0u);
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x13C438: lui $v0, 0x23 (done here), after $a1 got the corner to go round (the old side number, -1 the 1 way, & 3):
        // on the ring, the corner at the end touched ($s2 the way)
        void apronCorner(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_active)
            {
                const uint32_t fighter = getRegU32(ctx, 16);
                const int side = sideOf(readFloat(rdram, fighter + 0xCCu), readFloat(rdram, fighter + 0xD4u));
                SET_GPR_U32(ctx, 5, static_cast<uint32_t>(getRegU32(ctx, 18) == 1u ? side : (side + 1) % g_sides));
            }
            SET_GPR_U32(ctx, 2, 0x00230000u);
            ctx->pc = getRegU32(ctx, 31);
        }

        // FUN_00113bf0(fighter): state 3, round a corner on the apron: picture 0 starts clip 0x2741 / 0x2742, which carries
        // the wrestler round the post through the square's 90 degrees; after it, the next clip starts where it left him,
        // facing as it turned (+0x148). On the ring the corner turns 360 / sides, so while the clip plays the wrestler's
        // whole frame (the anchor, the place and facing it ends with) is turned about the corner of the two apron lines
        // by the clip's turn so far (+0x130) x (360 / sides / 90 - 1): a third back on six sides, nothing on four. The
        // clip's own motion is kept and it ends on the next side.
        float g_roundTurned[4] = {};

        void turnAbout(uint8_t *rdram, uint32_t xAt, uint32_t zAt, float px, float pz, float c, float s)
        {
            const float x = readFloat(rdram, xAt) - px, z = readFloat(rdram, zAt) - pz;
            Ps2FastWrite32(rdram, xAt, std::bit_cast<uint32_t>(px + x * c + z * s));
            Ps2FastWrite32(rdram, zAt, std::bit_cast<uint32_t>(pz + z * c - x * s));
        }

        void roundCorner(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_active)
            {
                const uint32_t fighter = getRegU32(ctx, 4), player = Ps2FastRead32(rdram, fighter + kPlayer) & 3u;
                const uint32_t step = Ps2FastRead32(rdram, fighter + 0x44u);
                const int corner = static_cast<int32_t>(Ps2FastRead32(rdram, fighter + 0x414u));
                if (step == 0u)
                    g_roundTurned[player] = 0.0f;
                else if (step == 1u && corner >= 0 && corner < g_sides)
                {
                    const float want = readFloat(rdram, fighter + 0x130u) * (360.0f / static_cast<float>(g_sides) / 90.0f - 1.0f);
                    const float by = want - g_roundTurned[player];
                    if (std::fabs(by) > 1e-4f)
                    {
                        g_roundTurned[player] = want;
                        const float toCorner = g_ring.apronLine / std::cos(kPi / g_sides), rad = g_cornerAngle[corner] * kPi / 180.0f;
                        const float px = std::sin(rad) * toCorner, pz = std::cos(rad) * toCorner;
                        const float c = std::cos(by * kPi / 180.0f), s = std::sin(by * kPi / 180.0f);
                        turnAbout(rdram, fighter + 0xCCu, fighter + 0xD4u, px, pz, c, s);   // the anchor
                        turnAbout(rdram, fighter + 0x138u, fighter + 0x140u, px, pz, c, s); // where the clip has him
                        for (const uint32_t angle : {0xDCu, 0x148u})                       // the facing, and after the clip
                            Ps2FastWrite32(rdram, fighter + angle,
                                           std::bit_cast<uint32_t>(wrapDegrees(readFloat(rdram, fighter + angle) + by)));
                    }
                }
            }
            g_roundCorner(rdram, ctx, runtime);
        }

        // ------------------------------------------------------------------ a run's start (FUN_00114140)

        // 0x114178: jal FUN_00132d30(stick): the corner a diagonal on the stick names (the game's four, by the diagonal alone),
        // or -1. On the ring: the corner most that way from the wrestler, any of its corners, as a whip judges it.
        void runCorner(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // outside the ring (+0x5AC bit 4 clear) a run goes the stick's own way, as on the square: the game's corner
            if (!g_active || !(Ps2FastRead32(rdram, getRegU32(ctx, 17) + 0x5ACu) & 4u))
            {
                g_stickCorner(rdram, ctx, runtime);
                return;
            }
            const uint32_t bits = getRegU32(ctx, 4) & 0xFu;
            if (bits != 3u && bits != 6u && bits != 0xCu && bits != 9u)
                SET_GPR_S32(ctx, 2, -1);
            else
            {
                const uint32_t fighter = getRegU32(ctx, 17);
                float x, z;
                stickDirection(bits, x, z);
                SET_GPR_S32(ctx, 2, cornerToward(readFloat(rdram, fighter + kHipsX), readFloat(rdram, fighter + kHipsZ),
                                                 x * 0.70710678f, z * 0.70710678f));
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        // FUN_0012a5b8(point, reach): the first corner within one of four distances of a point (x and z), or -1; the
        // game's loop stops after four corners, here all the ring's
        void cornerAt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_active)
            {
                g_cornerAt(rdram, ctx, runtime);
                return;
            }
            const uint32_t point = getRegU32(ctx, 4);
            const int32_t reach = static_cast<int32_t>(getRegU32(ctx, 5));
            const float limit = reach >= 0 && reach <= 3 ? readFloat(rdram, kCornerReach + static_cast<uint32_t>(reach) * 4u) : 0.0f;
            const float px = readFloat(rdram, point), pz = readFloat(rdram, point + 8u);
            int found = -1;
            for (int k = 0; k < g_sides && found < 0; ++k)
            {
                const uint32_t corner = kTables[kCornerPoints].store + static_cast<uint32_t>(k) * 12u;
                const float dx = readFloat(rdram, corner) - px, dz = readFloat(rdram, corner + 8u) - pz;
                if (std::sqrt(dx * dx + dz * dz) <= limit)
                    found = k;
            }
            SET_GPR_S32(ctx, 2, found);
            ctx->pc = getRegU32(ctx, 31);
        }

        // FUN_0012a698(angle $f12, point, out): where a line from the point at that angle meets the ropes, at the height 10.5
        // (the game's works on the square's ropes at 35); $v0 is left as the side that point is nearest (the game's last
        // call, FUN_00130438), which FUN_001155e8 keeps
        void ropeAhead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_active)
            {
                g_ropeAhead(rdram, ctx, runtime);
                return;
            }
            const uint32_t point = getRegU32(ctx, 4), out = getRegU32(ctx, 5);
            const float rad = ctx->f[12] * kPi / 180.0f, wx = std::sin(rad), wz = std::cos(rad);
            const float px = readFloat(rdram, point), pz = readFloat(rdram, point + 8u);
            const float t = ropeHit(px, pz, wx, wz), hx = px + wx * t, hz = pz + wz * t;
            if (out != 0u)
            {
                Ps2FastWrite32(rdram, out, std::bit_cast<uint32_t>(hx));
                Ps2FastWrite32(rdram, out + 4u, std::bit_cast<uint32_t>(10.5f));
                Ps2FastWrite32(rdram, out + 8u, std::bit_cast<uint32_t>(hz));
            }
            SET_GPR_U32(ctx, 2, static_cast<uint32_t>(bandOfAngle(std::atan2(hx, hz) * 180.0f / kPi)));
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x114280: addu $a0, $a0, $v1, with lwc1 $f0, 0($a0) (the run's angle, toward the rope the stick names) run first
        // as this jal's delay slot, so done after the sum here
        void runAngle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t at = getRegU32(ctx, 4) + getRegU32(ctx, 3), fighter = getRegU32(ctx, 17);
            SET_GPR_U32(ctx, 4, at);
            // outside the ring the run goes the stick's way: the game's own angle for that side (its table 0x235C20)
            ctx->f[0] = (Ps2FastRead32(rdram, fighter + 0x5ACu) & 4u) ? angleFor(rdram, at, fighter) : readFloat(rdram, at);
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x1142B0: addu $v0, $v0, $v1 (the corner angles' table + 4 x the corner), with lwc1 $f0, 0($v0) run first as this
        // jal's delay slot, so done after the sum here: a diagonal run outside the ring, toward the corner the stick names
        // (runCorner gave the game's corner there). On the ring the table holds the ring's corners: outside, the square's
        // diagonals are the stick's way (the game's own table 0x235C40: 135, 45, -45, -135).
        void outsideCornerAngle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            static constexpr float kSquareCorners[4] = {135.0f, 45.0f, -45.0f, -135.0f};
            const uint32_t offset = getRegU32(ctx, 2), at = offset + getRegU32(ctx, 3);
            SET_GPR_U32(ctx, 2, at);
            ctx->f[0] = g_active && offset < 16u ? kSquareCorners[offset / 4u] : readFloat(rdram, at);
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x11424C and 0x1143A8: sw $a0, 0x414($s1) (the side next along, -1 or +1 from the apron's own), with lwc1 $f0, 0($v1)
        // (that side's angle) run first as this jal's delay slot. On the ring the run goes along the apron's side, the way
        // the game chose (+0x5B2 bit 4: the -1 way)
        void apronRun(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t fighter = getRegU32(ctx, 17);
            Ps2FastWrite32(rdram, fighter + 0x414u, getRegU32(ctx, 4));
            if (g_active)
            {
                const int side = sideOf(readFloat(rdram, fighter + 0xCCu), readFloat(rdram, fighter + 0xD4u));
                const float sign = (Ps2FastRead32(rdram, fighter + 0x5B0u) >> 16) & 4u ? -1.0f : 1.0f;
                ctx->f[0] = std::atan2(g_sideTangent[side][0] * sign, g_sideTangent[side][1] * sign) * 180.0f / kPi;
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x114368: jal FUN_0012f220($f12 the apron side's inward angle, $f13 the facing): with no stick, its sign says which
        // way along the apron to run (above 0: the -1 way). On the ring: the way the wrestler faces along its side.
        void apronRunWay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_active)
            {
                g_angleApart(rdram, ctx, runtime);
                return;
            }
            const uint32_t fighter = getRegU32(ctx, 17);
            const int side = sideOf(readFloat(rdram, fighter + 0xCCu), readFloat(rdram, fighter + 0xD4u));
            const float rad = ctx->f[13] * kPi / 180.0f;
            const float along = std::sin(rad) * g_sideTangent[side][0] + std::cos(rad) * g_sideTangent[side][1];
            ctx->f[0] = along < 0.0f ? 1.0f : -1.0f;
            ctx->pc = getRegU32(ctx, 31);
        }

        // FUN_001144c0(fighter): 1 when a run to the rope +0x414 names would start too close to it (past 25 from the middle
        // towards it, facing it within 85 degrees); the game measures along x or z, here along the ring's real side
        void nearRope(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_active)
            {
                g_nearRope(rdram, ctx, runtime);
                return;
            }
            const uint32_t fighter = getRegU32(ctx, 4);
            const int32_t band = static_cast<int32_t>(Ps2FastRead32(rdram, fighter + 0x414u));
            uint32_t result = 0u;
            if ((Ps2FastRead32(rdram, fighter + 0x5ACu) & 4u) != 0u && band >= 0 && band <= 3)
            {
                const float inward = angleFor(rdram, kGameInAngles + static_cast<uint32_t>(band) * 4u, fighter);
                const float rad = (inward + 180.0f) * kPi / 180.0f;
                const float out = readFloat(rdram, fighter + kHipsX) * std::sin(rad) + readFloat(rdram, fighter + kHipsZ) * std::cos(rad);
                if (out > 25.0f && std::fabs(wrapDegrees(inward - readFloat(rdram, fighter + 0x148u))) < 85.0f)
                    result = 1u;
            }
            SET_GPR_U32(ctx, 2, result);
            ctx->pc = getRegU32(ctx, 31);
        }

        // ------------------------------------------------------------------ the computer's choices

        // The side with the game's 0 to 3 number `band` nearest a point (on a hexagon a number covers one or two sides)
        int sideForBand(uint32_t band, float x, float z)
        {
            int best = -1;
            float bestDot = -1e30f;
            for (int k = 0; k < g_sides; ++k)
                if (static_cast<uint32_t>(g_sideBand[k]) == band)
                    if (const float dot = g_sideNormal[k][0] * x + g_sideNormal[k][1] * z; dot > bestDot)
                    {
                        bestDot = dot;
                        best = k;
                    }
            return best >= 0 ? best : sideOf(x, z);
        }

        // FUN_0010f5d0(record, fighter): what the computer sees each picture, among it its distance to each corner (the
        // corner points' table, now the ring's), four floats at + 0xA4 (+ 0xB4 is the next field: no room for more). Four
        // readers index them by an opponent's corner (+0x414): 0x10BB94 and 0x10BBDC (FUN_0010ba78), 0x10CFC4
        // (FUN_0010cf68), 0x10E8F8 (FUN_0010e868). On the ring the distances to the corners past the fourth are kept in
        // the store, and those readers pointed there for them.
        uint32_t g_cpuRecords[4] = {};

        uint32_t cpuExtra(uint32_t record)
        {
            for (uint32_t i = 0; i < 4u; ++i)
                if (g_cpuRecords[i] == record || g_cpuRecords[i] == 0u)
                {
                    g_cpuRecords[i] = record;
                    return kCpuCornerExtra + i * 4u * (kMaxSides - 4);
                }
            return kCpuCornerExtra; // more than four computer records: never seen
        }

        void cpuSees(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_active && g_sides > 4)
            {
                const uint32_t record = getRegU32(ctx, 4), fighter = getRegU32(ctx, 5), extra = cpuExtra(record);
                const float x = readFloat(rdram, fighter + kHipsX), z = readFloat(rdram, fighter + kHipsZ);
                for (int k = 4; k < g_sides; ++k)
                {
                    const uint32_t corner = kTables[kCornerPoints].store + static_cast<uint32_t>(k) * 12u;
                    const float dx = readFloat(rdram, corner) - x, dz = readFloat(rdram, corner + 8u) - z;
                    Ps2FastWrite32(rdram, extra + 4u * static_cast<uint32_t>(k - 4), std::bit_cast<uint32_t>(std::sqrt(dx * dx + dz * dz)));
                }
            }
            g_cpuSees(rdram, ctx, runtime);
        }

        // 0x10BB88, 0x10BBD0, 0x10E8F0: sll $v0, $v0, 2, with addu $v0, $base, $v0 run first as this jal's delay slot (so $v0
        // = the record + the corner); 0x10CFBC: sll $v0, $v1, 2 (the corner in $v1). $v0 left for the lwc1 0xA4($v0) after it:
        // the record's own float for corners 0 to 3, the kept one past them
        template <int Base, bool CornerInV1>
        void cpuCornerRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t record = getRegU32(ctx, Base);
            const uint32_t corner = CornerInV1 ? getRegU32(ctx, 3) : getRegU32(ctx, 2) - record;
            uint32_t at = record + corner * 4u;
            if (g_active && corner >= 4u && corner < static_cast<uint32_t>(g_sides))
                at = cpuExtra(record) + (corner - 4u) * 4u - 0xA4u;
            SET_GPR_U32(ctx, 2, at);
            ctx->pc = getRegU32(ctx, 31);
        }

        // FUN_00112648(record, fighter): is the rope behind the computer close enough for the rope move: the side behind
        // by its old number ($s0), the rope point there (its hips with x or z put on the square's +-35), the angle from that
        // point to the opponent against the side's inward angle (within 20), the distance. On the ring:
        // 0x11276C: jal FUN_00130578(the rope point, his hips): the point becomes the foot of its hips ($s2 the fighter) on
        // the rope of the side with that number nearest
        void cpuRopeFoot(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_active)
            {
                const uint32_t point = getRegU32(ctx, 4), fighter = getRegU32(ctx, 18);
                const float x = readFloat(rdram, fighter + kHipsX), z = readFloat(rdram, fighter + kHipsZ);
                const int side = sideForBand(getRegU32(ctx, 16) & 3u, x, z);
                const float nx = g_sideNormal[side][0], nz = g_sideNormal[side][1], out = x * nx + z * nz - g_ring.ropeDistance;
                Ps2FastWrite32(rdram, point, std::bit_cast<uint32_t>(x - nx * out));
                Ps2FastWrite32(rdram, point + 8u, std::bit_cast<uint32_t>(z - nz * out));
            }
            g_angleTo(rdram, ctx, runtime);
        }

        // 0x112788: jal FUN_0012f220, with lwc1 $f12, 0($v1) (the side's inward angle by its old number) as its delay slot:
        // on the ring, that side's own
        void cpuRopeAngle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_active)
            {
                const uint32_t fighter = getRegU32(ctx, 18);
                ctx->f[12] = inwardAngle(sideForBand(getRegU32(ctx, 16) & 3u, readFloat(rdram, fighter + kHipsX),
                                                     readFloat(rdram, fighter + kHipsZ)));
            }
            g_angleApart(rdram, ctx, runtime);
        }

        // FUN_001100b0(side's old number, point): how far the point is from that side's rope (|35 - x or z| on the square,
        // 35 for another number); on the ring, from the rope of the side with that number nearest the point
        void ropeDistanceOf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_active)
            {
                g_ropeDistanceOf(rdram, ctx, runtime);
                return;
            }
            const uint32_t band = getRegU32(ctx, 4), point = getRegU32(ctx, 5);
            float distance = 35.0f;
            if (band <= 3u)
            {
                const float x = readFloat(rdram, point), z = readFloat(rdram, point + 8u);
                const int side = sideForBand(band, x, z);
                distance = std::fabs(g_ring.ropeDistance - (g_sideNormal[side][0] * x + g_sideNormal[side][1] * z));
            }
            ctx->f[0] = distance;
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x11299C (FUN_00112978, the dives' start: the opponent's hips) and 0x112B2C (FUN_00112ad8, Action 51's: his anchor):
        // lui $at, 0x420C (35.0, done here), with mtc1 $at, $f1 run first as this jal's delay slot; then |x| ($f0) < $f1, else
        // |z| < $f1, says he is not diagonally beyond a corner of the square. On the ring the answer is the ring's (his foot
        // on his nearest side past the side's end): not beyond, $f0 = 0; beyond, the limit $f1 = -1 so both tests fail
        template <int FighterReg, uint32_t Place>
        void cpuBeyondCorner(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            SET_GPR_U32(ctx, 1, 0x420C0000u);
            ctx->f[1] = 35.0f;
            if (g_active)
            {
                const uint32_t fighter = getRegU32(ctx, FighterReg);
                const float x = readFloat(rdram, fighter + Place), z = readFloat(rdram, fighter + Place + 8u);
                const int side = sideOf(x, z);
                const float along = x * g_sideTangent[side][0] + z * g_sideTangent[side][1];
                if (std::fabs(along) > g_ring.ropeDistance * std::tan(kPi / g_sides))
                {
                    ctx->f[0] = 100.0f;
                    ctx->f[1] = -1.0f;
                }
                else
                    ctx->f[0] = 0.0f;
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        // ------------------------------------------------------------------ the ropes
        // Each side has three ropes, top to bottom: {along the rope in the model's units (-35 to 35), up, out, half width}
        // and a speed for up and out. The rope model is 70 long; drawn on a shorter side it is scaled along its length, so
        // positions along a rope are multiplied by 35 / the half side.

        float g_ropeScale = 1.0f; // the rope model's units per game unit along a side
        uint64_t g_windowsSeen[25] = {}; // VENDETTA_RINGLOG: how often each kind of rope window ran
        float g_furthestOut = 0.0f;      // and the furthest any rope has been pulled out

        void setRope(int side, int level, float along, float up, float out, float halfWidth)
        {
            const float state[4] = {along, up, out, halfWidth};
            std::memcpy(vendetta_store::host(kRopeState + (side * 3 + level) * 16u), state, sizeof(state));
            if (std::fabs(out) > g_furthestOut)
                g_furthestOut = std::fabs(out);
        }

        void clearRopeState()
        {
            std::memset(vendetta_store::host(kRopeState), 0, static_cast<size_t>(g_sides) * 3u * 16u);
            std::memset(vendetta_store::host(kRopeSpeed), 0, static_cast<size_t>(g_sides) * 3u * 8u);
        }

        // A rope pulled to a point. `flat`: not out at all (the bottom rope lifted); `extraOut`: this far past the point.
        void pullTo(int side, int level, float x, float y, float z, float halfWidth, float extraOut, bool flat)
        {
            const float nx = g_sideNormal[side][0], nz = g_sideNormal[side][1];
            const float rx = -g_sideTangent[side][0], rz = -g_sideTangent[side][1];
            float along = (x * rx + z * rz) * g_ropeScale;
            along = along > 35.0f ? 35.0f : (along < -35.0f ? -35.0f : along);
            const float out = flat ? 0.0f : x * nx + z * nz - g_ring.ropeDistance + extraOut;
            setRope(side, level, along, y - g_ring.ropeHeights[level], out, halfWidth * g_ropeScale);
        }

        // FUN_001a2bc8(half width f12, rope a0, point a1): on another shape, the side nearest the point
        void pullRope(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_active)
            {
                g_pullRope(rdram, ctx, runtime);
                return;
            }
            const uint32_t point = getRegU32(ctx, 5);
            const float x = readFloat(rdram, point), y = readFloat(rdram, point + 4u), z = readFloat(rdram, point + 8u);
            pullTo(sideOf(x, z), static_cast<int>(getRegU32(ctx, 4)) % 3, x, y, z, ctx->f[12], 0.0f, false);
            ctx->pc = getRegU32(ctx, 31);
        }

        // FUN_001a2cf8(step f12): up and out spring back towards rest
        void ropeSpring(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_active)
            {
                g_ropeSpring(rdram, ctx, runtime);
                return;
            }
            const float step = ctx->f[12];
            const float k = readFloat(rdram, Ps2FastRead32(rdram, kSceneHeader) + 0x10u);
            const float small = readFloat(rdram, kGp - 0x72E0u);
            float *state = reinterpret_cast<float *>(vendetta_store::host(kRopeState));
            float *speed = reinterpret_cast<float *>(vendetta_store::host(kRopeSpeed));
            for (int rope = 0; rope < g_sides * 3; ++rope)
                for (int j = 0; j < 2; ++j)
                {
                    float &p = state[rope * 4 + 1 + j], &v = speed[rope * 2 + j];
                    const float moved = p + v * step;
                    const float faster = v + (k - 1.0f) * (v + moved) * step;
                    p = moved;
                    v = faster;
                    if (std::fabs(v) < small && std::fabs(p) < small)
                        p = v = 0.0f;
                }
            ctx->pc = getRegU32(ctx, 31);
        }

        // FUN_001a2b80, at the arena's load
        void clearRopes(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_active)
            {
                g_clearRopes(rdram, ctx, runtime);
                return;
            }
            clearRopeState();
            ctx->pc = getRegU32(ctx, 31);
        }

        // FUN_00131848(body points, window type): what a move's window does to the ropes, by the ring's own sides. The
        // heights are the game's: the three a pulled rope is taken to, its other numbers beside them.
        void ropeWindows(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_active)
            {
                g_ropeWindows(rdram, ctx, runtime);
                return;
            }
            const uint32_t b = getRegU32(ctx, 4);
            const int type = static_cast<int>(getRegU32(ctx, 5));
            if (type >= 0 && type < 25)
                ++g_windowsSeen[type];
            auto f = [&](uint32_t offset) { return readFloat(rdram, b + offset); };
            auto gp = [&](uint32_t back) { return readFloat(rdram, kGp - back); };
            const float hipsX = f(0x15Cu), hipsZ = f(0x164u);
            const int side = sideOf(hipsX, hipsZ);
            const float heights[3] = {readFloat(rdram, kPullHeights), readFloat(rdram, kPullHeights + 4u), readFloat(rdram, kPullHeights + 8u)};
            const float low = 15.25f;
            switch (type)
            {
            case 1:
                pullTo(side, 2, hipsX, gp(0x7ADCu), hipsZ, 0.0f, 0.0f, true);
                break;
            case 2:
                pullTo(side, 0, hipsX, gp(0x7AD8u), hipsZ, 4.0f, 3.0f, false);
                pullTo(side, 1, hipsX, gp(0x7AD4u), hipsZ, 4.0f, 3.0f, false);
                pullTo(side, 2, hipsX, low, hipsZ, 4.0f, 3.0f, false);
                break;
            case 3:
                pullTo(side, 0, f(0x180u), gp(0x7AD0u), f(0x188u), 0.0f, 1.5f, false);
                pullTo(side, 1, hipsX, gp(0x7ACCu), hipsZ, 0.0f, 1.5f, false);
                pullTo(side, 2, hipsX, low, hipsZ, 0.0f, 0.0f, false);
                break;
            case 4:
                pullTo(side, 0, hipsX, gp(0x7AC8u), hipsZ, 0.0f, 4.0f, false);
                pullTo(side, 1, hipsX, gp(0x7AC4u), hipsZ, 0.0f, 4.0f, false);
                pullTo(side, 2, hipsX, low, hipsZ, 0.0f, 4.0f, false);
                break;
            case 5: // the corner the hips are at: the two ropes meeting there, pulled towards it
            {
                const float along = hipsX * -g_sideTangent[side][0] + hipsZ * -g_sideTangent[side][1];
                const int first = along > 0.0f ? side : (side + 1) % g_sides;
                const int second = (first + g_sides - 1) % g_sides;
                const float drop = gp(0x7AB0u), out = std::fabs(gp(0x7AC0u)) - 35.0f;
                for (int level = 0; level < 3; ++level)
                {
                    const float up = heights[level] - drop - g_ring.ropeHeights[level];
                    setRope(first, level, 29.0f, up, out, 0.0f);
                    setRope(second, level, -29.0f, up, out, 0.0f);
                }
                break;
            }
            case 6: // the ring shakes: every rope at its middle
            {
                const float drop = gp(0x7A98u);
                for (int s = 0; s < g_sides; ++s)
                    for (int level = 0; level < 3; ++level)
                        setRope(s, level, 0.0f, heights[level] - drop - g_ring.ropeHeights[level], 1.0f, 0.0f);
                break;
            }
            default: // 7 to 24: one rope of the side nearest a hand, a foot or the midpoint of two
                if (type >= 7 && type <= 24)
                {
                    static const uint32_t points[6][2] = {{0x1ECu, 0u},     {0x1BCu, 0u},     {0x1ECu, 0x1BCu},
                                                          {0x234u, 0u},     {0x210u, 0u},     {0x234u, 0x210u}};
                    const int group = (type - 7) / 3, level = (type - 7) % 3;
                    const uint32_t a = points[group][0], c = points[group][1];
                    float x = f(a), y = f(a + 4u), z = f(a + 8u);
                    if (c)
                    {
                        x = (x + f(c)) * 0.5f;
                        y = (y + f(c + 4u)) * 0.5f;
                        z = (z + f(c + 8u)) * 0.5f;
                    }
                    pullTo(sideOf(x, z), level, x, y, z, 0.0f, 0.0f, false);
                }
                break;
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        // The bend, as FUN_001a3950 works it out: each of the eleven points along the rope takes a share of the pull
        void bendRope(uint8_t *rdram, int rope)
        {
            float state[4];
            std::memcpy(state, vendetta_store::host(kRopeState + rope * 16u), sizeof(state));
            const float from = state[0] - state[3], to = state[0] + state[3];
            for (int i = 0; i < kRopeBendPoints; ++i)
            {
                const uint32_t at = kRopeBones + i * 16u;
                const float x = readFloat(rdram, at);
                float share = 1.0f;
                if (x < from)
                {
                    const float span = std::fabs(from + 35.0f);
                    share = span > 0.0f ? std::fabs(x + 35.0f) / span : 0.0f;
                }
                else if (x > to)
                {
                    const float span = std::fabs(to - 35.0f);
                    share = span > 0.0f ? std::fabs(x - 35.0f) / span : 0.0f;
                }
                Ps2FastWrite32(rdram, at + 4u, std::bit_cast<uint32_t>(state[1] * share));
                Ps2FastWrite32(rdram, at + 8u, std::bit_cast<uint32_t>(state[2] * share));
            }
        }

        // FUN_001a2de8 on another shape: only the ropes, the posts being parts of the arena's own model. A run of calls to
        // the game's routines, each returning to kRopeDrawReturn for the next, so the game's scheduler can run between them.
        const uint32_t kBeforeRopes[10][3] = {{0x4Eu, 0u, 0x80u},    {0x4Fu, 0u, 0x80u},    {0x47u, 0u, 0x5001Bu}, {0x48u, 0u, 0x5001Bu}, {0x14u, 0u, 0x60u},
                                              {0x15u, 0u, 0x60u},    {0x42u, 0x80u, 0xA8u}, {0x43u, 0x80u, 0xA8u}, {8u, 0u, 0u},         {9u, 0u, 0u}};
        const uint32_t kAfterRopes[6][3] = {{0x4Eu, 1u, 0x80u}, {0x4Fu, 1u, 0x80u}, {0x47u, 0u, 0u}, {0x48u, 0u, 0u}, {0x42u, 0u, 0x44u}, {0x43u, 0u, 0x44u}};
        enum DrawStep { kBefore, kPieces, kLoadProgram, kNextRope, kMatrix, kDraw, kWait, kFaded, kAfter };
        struct RopeDraw
        {
            DrawStep step = kBefore;
            int index = 0, rope = 0, fadeStep = 0;
            size_t piece = 0;
            uint32_t part = 0, returnTo = 0, matrix = 0;
        } g_draw;

        // The ring's own corner pieces (setCornerSpots), drawn as the game's ring draw draws 81-92 on High Poly: the main
        // processor's post draw FUN_001b3820(piece, M, the arena colour 0x1430D60), the solid ones at once; one within 50
        // of the camera across the floor fades (FIX = d^2 / 2500 x 76.8 + 51.2, truncated, into ALPHA_1 with 0x64, then
        // sceGsSyncPath(0, 0) before its draw), the faded ones after the ropes, far to near. A corner's pad and turnbuckle
        // take its post's fade, as the game's 85-92 take 81-84's.
        constexpr uint32_t kDrawPost = 0x1B3820u, kSyncPath = 0x1F7930u, kArenaColour = 0x1430D60u, kCamera = 0x258090u;
        std::vector<CornerSpot> g_cornerSpots;
        struct PieceDraw
        {
            uint32_t object = 0, fix = 0x80u;
            float distance = 0.0f;
        };
        std::vector<PieceDraw> g_pieces; // this picture's: the solid ones, then the faded far to near
        size_t g_solidPieces = 0;

        // A part's object in the arena's scene (the first directory entry with its number; 0 = not there)
        uint32_t findPart(uint8_t *rdram, int32_t id)
        {
            const uint32_t scene = Ps2FastRead32(rdram, kSceneHeader);
            const uint32_t count = std::min<uint32_t>(Ps2FastRead32(rdram, scene), 4096u);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t entry = scene + 0x14u + 8u * i;
                if (static_cast<int32_t>(Ps2FastRead32(rdram, entry)) == id)
                    return entry + 4u + Ps2FastRead32(rdram, entry + 4u);
            }
            return 0u;
        }

        void listPieces(uint8_t *rdram)
        {
            g_pieces.clear();
            const float cameraX = readFloat(rdram, kCamera), cameraZ = readFloat(rdram, kCamera + 8u);
            uint32_t postFix[kCornerRoom] = {};
            for (const int pass : {0, 1}) // the posts first: their corner's pad and turnbuckle follow them
                for (const CornerSpot &s : g_cornerSpots)
                {
                    const bool post = s.part >= kCornerPost && s.part < kCornerPost + kCornerRoom;
                    if (post != (pass == 0))
                        continue;
                    const uint32_t object = findPart(rdram, s.part);
                    if (!object)
                        continue;
                    const float dx = s.x - cameraX, dz = s.z - cameraZ, d2 = dx * dx + dz * dz;
                    uint32_t fix = d2 < 2500.0f ? static_cast<uint32_t>(static_cast<int32_t>(d2 / 2500.0f * 76.8f + 51.2f)) : 0x80u;
                    const int at = (s.part - kCornerPost) % 20;
                    if (post)
                        postFix[at] = fix;
                    else if (at >= 0 && at < kCornerRoom && postFix[at])
                        fix = postFix[at];
                    g_pieces.push_back({object, fix, d2});
                }
            auto faded = std::stable_partition(g_pieces.begin(), g_pieces.end(), [](const PieceDraw &p) { return p.fix == 0x80u; });
            std::stable_sort(faded, g_pieces.end(), [](const PieceDraw &a, const PieceDraw &b) { return a.distance > b.distance; });
            g_solidPieces = static_cast<size_t>(faded - g_pieces.begin());
        }

        uint32_t findRopePart(uint8_t *rdram)
        {
            const uint32_t scene = Ps2FastRead32(rdram, kSceneHeader);
            const uint32_t count = Ps2FastRead32(rdram, scene);
            uint32_t entry = scene + 0x14u;
            for (uint32_t i = 0; i < count; ++i, entry += 8u)
                if (Ps2FastRead32(rdram, entry) == 2u)
                    return entry + 4u + Ps2FastRead32(rdram, entry + 4u);
            return 0u;
        }

        void callGuest(R5900Context *ctx, uint32_t function, uint32_t a0 = 0u, uint32_t a1 = 0u, uint32_t a2 = 0u, uint32_t a3 = 0u)
        {
            SET_GPR_U32(ctx, 4, a0);
            SET_GPR_U32(ctx, 5, a1);
            SET_GPR_U32(ctx, 6, a2);
            SET_GPR_U32(ctx, 7, a3);
            SET_GPR_U32(ctx, 31, kRopeDrawReturn);
            ctx->pc = function;
        }

        void writeRopeMatrix(int rope)
        {
            const int side = rope / 3, level = rope % 3;
            const float nx = g_sideNormal[side][0], nz = g_sideNormal[side][1];
            const float rx = -g_sideTangent[side][0] / g_ropeScale, rz = -g_sideTangent[side][1] / g_ropeScale;
            const float d = g_ring.ropeDistance;
            const float matrix[16] = {rx, 0.0f, rz, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, nx, 0.0f, nz, 0.0f, nx * d, g_ring.ropeHeights[level], nz * d, 1.0f};
            std::memcpy(vendetta_store::host(kRopeMatrix), matrix, sizeof(matrix));
        }

        void continueRopeDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            for (;;)
                switch (g_draw.step)
                {
                case kBefore:
                    if (g_draw.index < 10)
                    {
                        const uint32_t *c = kBeforeRopes[g_draw.index++];
                        callGuest(ctx, kSetRenderState, c[0], c[1], c[2]);
                        return;
                    }
                    g_draw.step = kPieces;
                    g_draw.piece = 0;
                    break;
                case kPieces:
                    if (g_draw.piece < g_solidPieces)
                    {
                        callGuest(ctx, kDrawPost, g_pieces[g_draw.piece++].object, g_draw.matrix, kArenaColour);
                        return;
                    }
                    g_draw.step = kLoadProgram;
                    break;
                case kLoadProgram:
                    g_draw.step = kNextRope;
                    g_draw.rope = 0;
                    callGuest(ctx, kLoadRopeProgram);
                    return;
                case kNextRope:
                    if (g_draw.part == 0u || g_draw.rope >= g_sides * 3)
                    {
                        g_draw.step = kFaded;
                        g_draw.piece = g_solidPieces;
                        g_draw.fadeStep = 0;
                        break;
                    }
                    bendRope(rdram, g_draw.rope);
                    g_draw.step = kMatrix;
                    callGuest(ctx, kPoseRope, g_draw.part, kRopeTemplate, kRopeBones);
                    return;
                case kMatrix:
                    writeRopeMatrix(g_draw.rope);
                    g_draw.step = kDraw;
                    callGuest(ctx, kSetMatrix, g_draw.part, kRopeMatrix, kViewA, kViewB);
                    return;
                case kDraw:
                    g_draw.step = kWait;
                    callGuest(ctx, kDrawPart, g_draw.part, kPartLight, kPartColours);
                    return;
                case kWait:
                    if (READ32(kVif1Status) & 0x100u)
                    {
                        ctx->pc = kRopeDrawReturn; // the chain still reading this rope: asked again after the scheduler has run
                        return;
                    }
                    ++g_draw.rope;
                    g_draw.step = kNextRope;
                    break;
                case kFaded:
                    if (g_draw.piece >= g_pieces.size())
                    {
                        g_draw.step = kAfter;
                        g_draw.index = 0;
                        break;
                    }
                    {
                        const PieceDraw &p = g_pieces[g_draw.piece];
                        switch (g_draw.fadeStep++)
                        {
                        case 0:
                            callGuest(ctx, kSetRenderState, 0x42u, p.fix, 0x64u); // ALPHA_1
                            return;
                        case 1:
                            callGuest(ctx, kSyncPath, 0u, 0u);
                            return;
                        default:
                            g_draw.fadeStep = 0;
                            ++g_draw.piece;
                            callGuest(ctx, kDrawPost, p.object, g_draw.matrix, kArenaColour);
                            return;
                        }
                    }
                case kAfter:
                    if (g_draw.index < 6)
                    {
                        const uint32_t *c = kAfterRopes[g_draw.index++];
                        callGuest(ctx, kSetRenderState, c[0], c[1], c[2]);
                        return;
                    }
                    g_draw.step = kBefore;
                    SET_GPR_U32(ctx, 31, g_draw.returnTo);
                    ctx->pc = g_draw.returnTo;
                    return;
                }
        }

        void ringDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_active)
            {
                g_ringDraw(rdram, ctx, runtime);
                return;
            }
            g_draw = RopeDraw{};
            g_draw.returnTo = getRegU32(ctx, 31);
            g_draw.matrix = getRegU32(ctx, 4);
            g_draw.part = findRopePart(rdram);
            listPieces(rdram);
            continueRopeDraw(rdram, ctx, runtime);
        }

        // 0x119E4C: jal FUN_0012b550 at each match or round's start
        void matchStart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            putPathBack(rdram); // an intro's camera path left moved (a match ended in its intro)
            g_movedEntries.clear();
            fillTables(rdram);
            g_active = g_hasPending && g_tablesCommitted;
            setPairTable(rdram, g_active);
            if (g_active)
            {
                g_ring = g_pendingRing;
                buildRing(rdram);
                g_ropeScale = 35.0f / (g_ring.ropeDistance * std::tan(kPi / g_sides));
                clearRopeState();
            }
            g_linkWallLists(rdram, ctx, runtime);
        }

        // 0x12AFBC: daddu $v0, $s5, $zero: the list of walls this fighter ($s4) is tested against
        void wallSearch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t fighter = getRegU32(ctx, 20);
            SET_GPR_U32(ctx, 21, mapList(rdram, fighter, getRegU32(ctx, 21)));
            SET_GPR_U64(ctx, 2, GPR_U64(ctx, 21));
            trace(rdram, fighter);
            if (g_log)
            {
                checkBuilder(rdram);
                if (++g_wallSearches == 1u || g_wallSearches % 3600u == 0u)
                {
                    std::cerr << "[ring] wall searches so far: " << g_wallSearches;
                    if (g_active)
                    {
                        std::cerr << "; rope windows";
                        for (int i = 0; i < 25; ++i)
                            if (g_windowsSeen[i])
                                std::cerr << " " << i << ":" << g_windowsSeen[i];
                        std::cerr << "; furthest pulled out " << g_furthestOut;
                    }
                    std::cerr << std::endl;
                }
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x12B160: and $v1, $t5, $v1, the direction bits of the walls touched ($t5) kept to the axes the push may use ($v1),
        // with andi $v0, $v1, 0xA (x is pushed) run first as this jal's delay slot and done again here; 5 then says whether z
        // is (0x12B188). A wall carries one direction bit, so a slanted side of the ring would be pushed along one axis only
        // and the wrestler slid along it: on the ring a slanted wall touched ($s7 the first, $fp the second, 0 if none)
        // pushes along both, straight off it.
        void pushAxes(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            uint64_t bits = GPR_U64(ctx, 13);
            if (g_active)
                for (const uint32_t wall : {getRegU32(ctx, 23), getRegU32(ctx, 30)})
                    if (wall >= kWallRecords && wall < g_wallsEnd && std::fabs(readFloat(rdram, wall + 0x30u)) > 0.01f &&
                        std::fabs(readFloat(rdram, wall + 0x38u)) > 0.01f)
                        bits |= 0xFu;
            const uint64_t allowed = bits & GPR_U64(ctx, 3);
            SET_GPR_U64(ctx, 3, allowed);
            SET_GPR_U64(ctx, 2, allowed & 0xAu);
            ctx->pc = getRegU32(ctx, 31);
        }

        // 0x12B714: daddu $s4, $zero, $zero, after $s2 got the list and with the first read from it still to come
        // (0x12B718, lw $s1, 0($s2), runs first as this jal's delay slot, so it is done again here)
        void wallCheck(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            SET_GPR_U64(ctx, 20, 0u);
            const uint32_t list = mapList(rdram, getRegU32(ctx, 19), getRegU32(ctx, 18));
            SET_GPR_U32(ctx, 18, list);
            SET_GPR_U32(ctx, 17, Ps2FastRead32(rdram, list));
            ctx->pc = getRegU32(ctx, 31);
        }

        // FUN_00130438(point): the side a point is nearest
        void nearestSide(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_active)
            {
                g_nearestSide(rdram, ctx, runtime);
                return;
            }
            const uint32_t point = getRegU32(ctx, 4);
            const float degrees = std::atan2(readFloat(rdram, point), readFloat(rdram, point + 8u)) * 180.0f / kPi;
            SET_GPR_U32(ctx, 2, static_cast<uint32_t>(bandOfAngle(degrees)));
            ctx->pc = getRegU32(ctx, 31);
        }
    }

    bool parse(const vendetta_mods::Json &json, const std::string &what, Ring &out)
    {
        auto bad = [&](const char *why) {
            vendetta_mods::problem() << "[arenas] " << what << ": arena.json's \"ring\" " << why << ": the game's own ring" << std::endl;
            return false;
        };
        if (!json.isObject())
            return bad("is not a list of the ring's numbers");
        auto number = [&](const vendetta_mods::Json *j, float &to) {
            if (!j || j->type != vendetta_mods::Json::Type::Number)
                return false;
            to = static_cast<float>(j->number);
            return true;
        };
        Ring r;
        float sides = 0.0f;
        const vendetta_mods::Json *heights = json.find("rope heights"), *apron = json.find("apron");
        if (!number(json.find("sides"), sides) || !number(json.find("rope distance"), r.ropeDistance) ||
            !number(json.find("mat height"), r.matHeight) || !number(json.find("slump distance"), r.slumpDistance) ||
            !number(json.find("ring side"), r.ringSide) || !number(json.find("outside wall"), r.outsideWall) || !heights ||
            !heights->isArray() || heights->items.size() != 3u || !apron || !apron->isObject() ||
            !number(&heights->items[0], r.ropeHeights[0]) || !number(&heights->items[1], r.ropeHeights[1]) ||
            !number(&heights->items[2], r.ropeHeights[2]) || !number(apron->find("inner"), r.apronInner) ||
            !number(apron->find("line"), r.apronLine) || !number(apron->find("outer"), r.apronOuter))
            return bad("is missing some of its numbers");
        r.sides = static_cast<int>(sides);
        auto is = [](float a, float b) { return std::fabs(a - b) < 0.001f; };
        if (r.sides != 6)
            return bad("has a number of sides this version can't run (only 6)");
        if (!is(r.ropeDistance, 35.0f) || !is(r.matHeight, 10.5f) || !is(r.ropeHeights[0], 24.1f) || !is(r.ropeHeights[1], 19.3f) ||
            !is(r.ropeHeights[2], 14.5f) || !is(r.slumpDistance, 31.0f) || !is(r.apronInner, 34.0f) || !is(r.apronLine, 38.5f) ||
            !is(r.apronOuter, 43.0f) || !is(r.ringSide, 41.0f) || !is(r.outsideWall, 77.0f))
            return bad("has sizes this version can't run (only the game's own distances)");
        out = r;
        return true;
    }

    void setArena(const Ring *ring)
    {
        g_hasPending = ring != nullptr;
        if (ring)
            g_pendingRing = *ring;
    }

    void setCornerSpots(const std::vector<CornerSpot> &spots)
    {
        g_cornerSpots = spots;
        if (g_log && !spots.empty())
            std::cerr << "[ring] the arena's own corner pieces: " << spots.size() << std::endl;
    }

    void install(PS2Runtime &runtime)
    {
        const char *log = std::getenv("VENDETTA_RINGLOG");
        g_log = log && *log && *log != '0';
        if (const char *file = std::getenv("VENDETTA_RINGTRACE"); file && *file)
            g_trace.open(file, std::ios::trunc);
        if (const char *poison = std::getenv("VENDETTA_RINGPOISON"); poison && *poison)
            g_poison = static_cast<uint32_t>(std::strtoul(poison, nullptr, 0));
        g_tablesCommitted = vendetta_store::fixed(vendetta_store::kRingTables - vendetta_store::kFixedArea,
                                                  vendetta_store::kRingRoom, "ring tables") != 0u;
        g_linkWallLists = runtime.lookupFunction(kLinkWallLists);
        g_nearestSide = runtime.lookupFunction(kNearestSide);
        if (runtime.hasFunction(kHookWallSearch) || runtime.hasFunction(kHookMatchStart) || runtime.hasFunction(kHookWallCheck) ||
            !runtime.registerFunction(kHookWallSearch, wallSearch) || !runtime.registerFunction(kHookMatchStart, matchStart) ||
            !runtime.registerFunction(kHookWallCheck, wallCheck) || !runtime.registerFunction(kRopeDrawReturn, continueRopeDraw) ||
            !runtime.registerFunction(kHookSetCorner, setCorner) || !runtime.registerFunction(kHookPostLoop, postLoop) ||
            !runtime.registerFunction(kHookInRing, inRing) || !runtime.registerFunction(kHookApronPlace, apronPlace) ||
            !runtime.registerFunction(kHookApronFacing, apronFacing) || !runtime.registerFunction(kHookApronStance, apronStance) ||
            !runtime.registerFunction(kHookApronLook, apronLook) || !runtime.registerFunction(kHookApronProbe, apronProbe) ||
            !runtime.registerFunction(kHookApronProbe2, apronProbe2) || !runtime.registerFunction(kHookStartExit, startExit))
            std::cerr << "[ring] the hook addresses are taken: the matches will stop at the first wall" << std::endl;
        static constexpr PS2Runtime::RecompiledFunction kPairCases[9] = {pairCase<0>, pairCase<1>, pairCase<2>, pairCase<3>, pairCase<4>,
                                                                         pairCase<5>, pairCase<6>, pairCase<7>, pairCase<8>};
        for (uint32_t i = 0; i < 9u; ++i)
            runtime.registerFunction(kHookPairFirst + i * 4u, kPairCases[i]);
        runtime.registerFunction(kHookAngleF0A0, angleF0A0);
        runtime.registerFunction(kHookAngleF0V0, angleF0V0);
        runtime.registerFunction(kHookAngleThenTurn, angleThenTurn);
        runtime.registerFunction(kHookAngleAdd, angleAdd);
        runtime.registerFunction(kHookAngleF12, angleF12);
        runtime.registerFunction(kHookAngleF20, angleF20);
        runtime.registerFunction(kHookApronWalk, apronWalk);
        runtime.registerFunction(kHookStepTurn, stepTurn);
        runtime.registerFunction(kHookStepAngle, stepAngle);
        runtime.registerFunction(kHookStepMove, stepMove);
        runtime.registerFunction(kHookStartFacing, startFacing);
        runtime.registerFunction(kHookPushAxes, pushAxes);
        g_stickCorner = runtime.lookupFunction(kStickCorner);
        g_cornerAt = runtime.lookupFunction(kCornerAt);
        g_ropeAhead = runtime.lookupFunction(kRopeAhead);
        g_nearRope = runtime.lookupFunction(kNearRope);
        runtime.registerFunction(kHookRunCorner, runCorner);
        runtime.replaceFunction(kCornerAt, cornerAt);   // every caller: FUN_00114140, FUN_00115130, FUN_00128238, FUN_00137b38, FUN_0013bf18
        runtime.replaceFunction(kRopeAhead, ropeAhead);
        runtime.registerFunction(kHookRunAngle, runAngle);
        runtime.registerFunction(kHookApronRun, apronRun);
        runtime.registerFunction(kHookApronRunWay, apronRunWay);
        runtime.replaceFunction(kNearRope, nearRope);
        runtime.registerFunction(kHookApronEnd, apronEnd);
        runtime.registerFunction(kHookApronAlong, apronAlong);
        runtime.registerFunction(kHookApronCorner, apronCorner);
        g_roundCorner = runtime.lookupFunction(kRoundCorner);
        runtime.replaceFunction(kRoundCorner, roundCorner);
        g_sceneStart = runtime.lookupFunction(kSceneStart);
        runtime.replaceFunction(kSceneStart, sceneStart);
        g_cpuSees = runtime.lookupFunction(kCpuSees);
        runtime.replaceFunction(kCpuSees, cpuSees);
        g_ropeDistanceOf = runtime.lookupFunction(kRopeDistanceOf);
        runtime.replaceFunction(kRopeDistanceOf, ropeDistanceOf);
        g_angleTo = runtime.lookupFunction(kAngleTo);
        runtime.registerFunction(kHookCpuCorner1, cpuCornerRead<17, false>);
        runtime.registerFunction(kHookCpuCorner2, cpuCornerRead<17, false>);
        runtime.registerFunction(kHookCpuCorner3, cpuCornerRead<16, true>);
        runtime.registerFunction(kHookCpuCorner4, cpuCornerRead<16, false>);
        runtime.registerFunction(kHookCpuRopeFoot, cpuRopeFoot);
        runtime.registerFunction(kHookCpuRopeAngle, cpuRopeAngle);
        runtime.registerFunction(kHookCpuBeyond1, cpuBeyondCorner<2, 0x15Cu>);
        runtime.registerFunction(kHookCpuBeyond2, cpuBeyondCorner<6, 0xCCu>);
        runtime.registerFunction(kHookOutsideCorner, outsideCornerAngle);
        g_angleApart = runtime.lookupFunction(kAngleApart);
        g_turnTo = runtime.lookupFunction(kTurnTo);
        g_moveBy = runtime.lookupFunction(kMoveBy);
        g_apronStick = runtime.lookupFunction(kApronStick);
        runtime.replaceFunction(kApronStick, apronStick);
        g_startMove = runtime.lookupFunction(kStartMove);
        g_whipDirection = runtime.lookupFunction(kWhip);
        runtime.replaceFunction(kWhip, whipDirection);
        runtime.replaceFunction(kNearestSide, nearestSide);
        g_ringDraw = runtime.lookupFunction(kRingDraw);
        g_pullRope = runtime.lookupFunction(kPullRope);
        g_ropeSpring = runtime.lookupFunction(kRopeSpring);
        g_clearRopes = runtime.lookupFunction(kClearRopes);
        g_ropeWindows = runtime.lookupFunction(kRopeWindows);
        runtime.replaceFunction(kRingDraw, ringDraw);
        runtime.replaceFunction(kPullRope, pullRope);
        runtime.replaceFunction(kRopeSpring, ropeSpring);
        runtime.replaceFunction(kClearRopes, clearRopes);
        runtime.replaceFunction(kRopeWindows, ropeWindows);
    }
}

void installVendettaRing(PS2Runtime &runtime) { vendetta_ring::install(runtime); }
