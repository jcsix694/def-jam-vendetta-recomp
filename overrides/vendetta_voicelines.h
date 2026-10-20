// Def Jam Vendetta (USA, SLUS-20639) — a mod wrestler's own intro and win lines, played from PC memory.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// wrestler.json's "profile" "voice" { "intro line": "voice/intro.wav", "win line": "voice/win.wav", ... } (a path is
// relative to the folder of that wrestler.json): the voice row's bank name becomes one of ours (VDM<row>I / E), and
// when the game loads it (FUN_001f3938: the intro lines at the match's sound setup, the winner's at ENDMATCH) the WAV
// is read and handed to the sound chip as a host sample (ps2x/iop/spu2_host_samples.h), and the game gets a stand-in
// bank: silence as long as the recording at 1,500 Hz whose first block names that host sample. The game plays,
// skips and stops it as any line (one at a time: the next line, the fight's start, the victory's end); the sound chip
// plays the recording in its place.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace vendetta_voicelines
{
    // A line from a recording, for voice row `row`: checks the file (problems named with `label`) and returns the bank
    // name the row gets (8 letters), or "" when the file can't be used.
    std::string add(uint32_t row, bool intro, const std::filesystem::path &file, const std::string &label);
}
