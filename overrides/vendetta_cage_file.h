// Def Jam Vendetta (USA, SLUS-20639) - the cage match: the cage's own values, from cage.json in the cage's folder.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// <the cage's folder>\cage.json (vendetta_cage::cageFolder(): game\standard\cage\, which the setup copies from the
// repository's standard\cage\), read afresh at each cage match's load. A clip goes by the number its
// file name starts with ("3203"); the hang's copy at the rope hold is "3204 at the rope hold". A file or a key that isn't
// there = the values below (the standard cage's, the same as its shipped cage.json), so an older or smaller file works:
//   "rope bends": { "<clip>": [ { "from": 30, "to": 58 | "end", "what": "bottom rope, right foot" }, ... ] }
//                 the game's windows of a playback record (up to four a clip; "what" a mod move's window word)
//   "sounds":     { "<clip>": [ the sounds list a mod move's animation settings file has ] } ("3204" for both hangs)
//   "speeds":     { "<clip>": 120 } (100 = the clip's own speed, the default)
//   "loops":      [ "<clip>", ... ] (the clips that loop)
//   "numbers":    { "a step up": 4.8, ... } (the climb's numbers by name: CageNumbers, vendetta_cage_rules.h)
//   "custom sounds": { "<NAME>": { "file": "sounds/<NAME>.wav", "volume": 1 } } (recordings of the cage's own, the file
//                 beside cage.json, volume 0 to 4; a clip's "sounds" entry names one by "sound", vendetta_cage_sounds.cpp)
// Anything else (a tool's own notes) is left alone.
#pragma once

#include "vendetta_cage_rules.h"
#include "vendetta_mods.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace vendetta_cage
{
    struct CageWindow
    {
        uint32_t from = 0, to = 0, what = 0; // frames; what = the window's type (1-26)
    };

    // The cage's and the climb's numbers ("numbers"): the climb's rules' own struct (vendetta_cage_rules.h, its fields
    // in the file's order), the standard cage's values from cage_rules_standard_numbers
    using ClimbNumbers = CageNumbers;
    // the body points the rooms belong to (chest, head, left elbow, left fist, right elbow, right fist, left knee, left
    // ankle, right knee, right ankle)
    constexpr uint32_t kRoomPoints[CAGE_ROOMS] = CAGE_ROOM_POINTS;

    struct CustomSound
    {
        std::filesystem::path file; // the recording, beside cage.json
        float volume = 1.0f;        // 1 = as recorded, 0 to 4
    };

    // The numbers of leaving by the door ("numbers" too, the game's own: the climb's rules don't use them): tag and
    // handicap are last man wins, and someone out by a fall who is not the team's last walks out by the cage's door
    struct LeaveNumbers
    {
        float knockoutWait = 600.0f; // "out by a knockout, pictures down before leaving"
        float doorPictures = 30.0f;  // "the door, pictures to swing open"
        float walkOutside = 0.25f;   // "walking beside the cage, a picture"
        float tapsEmpty = 1.5f;      // "taps with the momentum meter empty, times": the bars' taps scaled by momentum
        float tapsFull = 0.5f;       // "taps with the momentum meter full, times" (in between: in a straight line)
    };

    // The computer's numbers in a cage match ("numbers" too, the game's own; vendetta_cage_cpu.cpp)
    struct CpuNumbers
    {
        float climb = 0.02f;        // "computer: a climb's chance a second": with ESCAPE on, standing in the ring
        float climbMore = 0.01f;    // "computer: more chance a minute of the fight"
        float climbMost = 0.08f;    // "computer: a climb's chance a second, most"
        float climbDown = 0.6f;     // "computer: a climb's chance a second, every opponent down and hurt"
        float downHurt = 0.6f;      // "computer: an opponent hurt, health below": a share of full health
        float climbAgain = 600.0f;  // "computer: pictures before trying a climb again"
        float climbGiveUp = 300.0f; // "computer: pictures to reach the ropes to climb"
        float shake = 0.35f;        // "computer: a shake's chance": a second, times the game's own answer to a climber
        float shakeGiveUp = 300.0f; // "computer: pictures to reach a climber's side"
        float diveMomentum = 20.0f; // "computer: momentum to climb to dive" (Blazin' climbs with any)
        float dive = 0.25f;         // "computer: a dive's chance a second": with ESCAPE off, an opponent down, the climb to dive
    };

    struct CageFile
    {
        std::map<std::string, CustomSound> customSounds; // by name (capitals)
        std::map<std::string, std::vector<CageWindow>> ropeBends;
        std::map<std::string, vendetta_mods::Json> sounds; // each clip's list, read with the clip (its last frame known)
        std::map<std::string, uint32_t> speeds;
        std::set<std::string> loops;
        ClimbNumbers numbers;
        LeaveNumbers leave;
        CpuNumbers cpu;
        std::string from; // the file read, or empty: the values built in
    };

    // the clip key for a cage clip's number ("3203"); the hang at the rope hold has its own
    std::string clipKey(uint32_t number);
    constexpr const char *kHangOnRopeKey = "3204 at the rope hold";

    // Read <the cage's folder>\cage.json (a cage match's load, before its clips are placed); a line on stderr says what
    // was read, and any key it couldn't use.
    void readCageFile();
    const CageFile &cageFile();

    // The custom sounds (vendetta_cage_sounds.cpp): each read and made 48 kHz when cage.json is read (one that can't be
    // is left out with a line on stderr); the numbers the ones read go by, by name, for the clips' sounds lists.
    void loadCustomSounds(const CageFile &file);
    const std::map<std::string, uint32_t> &customSoundIds();
}
