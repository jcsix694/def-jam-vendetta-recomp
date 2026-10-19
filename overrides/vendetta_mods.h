// Def Jam Vendetta (USA, SLUS-20639) — the mods\ folder: finding mods, reading their mod.json, dependencies, order.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// mods\<folder>\mod.json says what a mod does; its files sit in folders by type (animations\, wrestlers\, …) under any
// names. A mod tool writes this; it is plain text, so it can be edited by hand too:
//
//   {
//     "name": "Briggs Makeover", "author": "…", "version": 1, "description": "…",
//     "category": "Movesets",                                 the settings program's Mods page groups by it (not read here)
//     "needs": ["No Mercy Entrances"],                       other mods this one uses (they load first)
//     "replace": {                                           things the game already has, by the game's names:
//       "animations": { "N61A1_AP": "animations/Jericho Entrance.bin" }    changed everywhere they are used
//     },
//     "new": { "animations": ["animations/Jericho Entrance.bin"] },        things the game doesn't have
//     "wrestlers": { "Briggs": "wrestlers/Briggs.json" }                  moveset slots: { "84": "Jericho Entrance" }
//   }
//
// Projects: mods\ is the library of every installed mod; a project (projects\<name>\project.json) lists the mods ticked
// in it, in load order ("mods": ["<mod folder or name>", ...]), and keeps its own memory cards (projects\<name>\saves\mc0
// and mc1). The game plays a project when started with --project "<name>" (or VENDETTA_PROJECT=<name>, which
// settings.toml's [play] project fills); without one it is the original game: no mods, its own saves (game\mc0).
// Load order: the project's list (a later one wins when two change the same thing), and a mod always after the mods it
// needs. A mod whose needs are missing, or that isn't installed, is skipped with a message.
// VENDETTA_MODS=0: none. VENDETTA_MODS_DIR=<folder>: every mod in that folder, no project (test runs; mods\order.txt
// orders them: one mod folder per line, moved to the end in that order).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace vendetta_mods
{
    // A small JSON value (what mod.json needs: objects, arrays, strings, numbers, true / false / null).
    struct Json
    {
        enum class Type
        {
            Null,
            Bool,
            Number,
            String,
            Array,
            Object,
        };
        Type type = Type::Null;
        bool boolean = false;
        double number = 0.0;
        std::string text;
        std::vector<Json> items;                                  // an array
        std::vector<std::pair<std::string, Json>> members;        // an object, in file order

        const Json *find(const std::string &key) const;           // an object's member, or null
        bool isObject() const { return type == Type::Object; }
        bool isArray() const { return type == Type::Array; }
        bool isString() const { return type == Type::String; }
    };

    // Parses JSON text; false (and why, with the line) on an error.
    bool parseJson(const std::string &text, Json &out, std::string &error);

    struct Mod
    {
        std::string folderName;            // the folder under mods\ (what order.txt and "needs" name)
        std::string name;                  // mod.json's "name" (else the folder's)
        std::filesystem::path folder;
        Json manifest;
    };

    // The project being played (projects\<name>\): its name, folder, the mods ticked in it (as project.json names them, in
    // load order); null = the original game (no project asked for, or its project.json can't be read: then a message).
    struct Project
    {
        std::string name;
        std::filesystem::path folder;
        std::vector<std::string> mods;
    };
    const Project *currentProject();

    // The enabled mods in load order (read once; later calls return the same list).
    const std::vector<Mod> &loadedMods();

    // VENDETTA_MODLOG=1: a line for every animation, move, slot and sound list a match's mods put in (they go in at every
    // match, so without it only problems and one summary a match are printed).
    bool detailLog();

    // Reading ahead: a match's mod files are read on a background thread while its loading screen shows, so the match's
    // start finds them in memory. runAhead(job) runs `job` there (after any earlier job); inside it readAhead(path, bytes)
    // reads a file and keeps it. readWhole(path, bytes) - every read of a match's mod file - waits for the job, then takes
    // the kept bytes if the file is unchanged since (its size and time), else reads the file. dropAhead() forgets them.
    void runAhead(std::function<void()> job);
    bool readAhead(const std::filesystem::path &path, std::vector<uint8_t> &bytes);
    void readAheadAll(const std::vector<std::filesystem::path> &paths); // readAhead for each, several at once

    // Runs work(0) ... work(count - 1) on several threads and returns when all are done. For opening many files: the
    // first open of a file since the PC started costs about 11 ms (Windows checks it), reading it almost nothing, so
    // 1,000 files take 11 s one after another and 0.5 s sixteen at a time.
    void forEachAtOnce(size_t count, const std::function<void(size_t)> &work);
    bool readWhole(const std::filesystem::path &path, std::vector<uint8_t> &bytes);
    void dropAhead();

    // A file a mod.json names ("animations/x.bin", relative to the mod's folder), as a path; empty if it leaves the
    // mod's folder or doesn't exist (with a message).
    std::filesystem::path modFile(const Mod &mod, const std::string &relative, const char *what);

    // A problem with a mod, for the player: problem() << "[wrestlers] " << ... << std::endl; prints it on stderr as
    // before and adds it as a line of mods.log (the time, then the text without its "[tag] "), so a player can see what
    // was left out or used instead. mods.log sits beside settings.toml (the current folder without one;
    // VENDETTA_MODS_LOG=<file> elsewhere, =none: no file) and starts fresh at each game start, so it only holds that
    // session's problems.
    class Problem
    {
    public:
        Problem() = default;
        Problem(const Problem &) = delete;
        ~Problem();
        template <class T> Problem &operator<<(const T &value)
        {
            m_text << value;
            return *this;
        }
        Problem &operator<<(std::ostream &(*)(std::ostream &)) { return *this; } // std::endl: the line ends anyway

    private:
        std::ostringstream m_text;
    };
    inline Problem problem() { return {}; }
    void startProblemLog(); // mods.log emptied (the first look at the mods does it)
}
