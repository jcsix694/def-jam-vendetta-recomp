// Def Jam Vendetta (USA, SLUS-20639) - the cage match: cage.json, the cage's own values (vendetta_cage_file.h).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
#include "vendetta_cage_file.h"
#include "vendetta_cage.h"
#include "vendetta_moves.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

namespace vendetta_cage_file
{
    using vendetta_cage::CageFile;
    using vendetta_cage::CageWindow;
    using vendetta_mods::Json;

    constexpr uint32_t kToEnd = 2047u; // "to": "end" (the playback record's window fields hold 11 bits)

    // the standard cage's values: what its shipped cage.json holds, and what a cage without one plays by
    CageFile standard()
    {
        CageFile f;
        cage_rules_standard_numbers(&f.numbers);
        f.ropeBends["3203"] = {{30u, 58u, 18u}, {46u, 90u, 20u}, {70u, 132u, 16u}};
        f.ropeBends[vendetta_cage::kHangOnRopeKey] = {{0u, kToEnd, 16u}};
        f.ropeBends["3205"] = {{0u, 38u, 16u}, {20u, 52u, 20u}};
        f.loops = {"3204", "3239", "323E", "3246", "324A"};
        return f;
    }

    CageFile g_file = standard();

    bool number(const Json &j, double &out)
    {
        if (j.type != Json::Type::Number || !std::isfinite(j.number))
            return false;
        out = j.number;
        return true;
    }

    void readNumbers(const Json &numbers, vendetta_cage::ClimbNumbers &n, std::vector<std::string> &skipped)
    {
        struct Named
        {
            const char *name;
            float *value;
        };
        const Named floats[] = {{"the cage's height above the mat", &n.height},
                                {"the cage's walls from the middle", &n.walls},
                                {"a step up", &n.stepUp},
                                {"a step sideways", &n.stepSide},
                                {"kept from each corner", &n.cornerRoom},
                                {"a step's momentum", &n.stepCost},
                                {"the wires", &n.wires},
                                {"toward the cage, a picture", &n.toCage},
                                {"back from the cage, a picture", &n.offCage},
                                {"ankles above the mat, least", &n.ankleLeast},
                                {"ankles above the mat, standing", &n.ankleUp},
                                {"a foot on a rope presses it down", &n.ropePress},
                                {"room, chest", &n.room[0]},
                                {"room, head", &n.room[1]},
                                {"room, left elbow", &n.room[2]},
                                {"room, left fist", &n.room[3]},
                                {"room, right elbow", &n.room[4]},
                                {"room, right fist", &n.room[5]},
                                {"room, left knee", &n.room[6]},
                                {"room, left ankle", &n.room[7]},
                                {"room, right knee", &n.room[8]},
                                {"room, right ankle", &n.room[9]}};
        for (const auto &[key, value] : numbers.members)
        {
            double v = 0.0;
            bool used = false;
            for (const Named &f : floats)
                if (key == f.name && number(value, v))
                {
                    *f.value = static_cast<float>(v);
                    used = true;
                }
            if (key == "steps to the top hold" && number(value, v) && v >= 1.0 && v <= 20.0)
            {
                n.steps = static_cast<int>(v);
                used = true;
            }
            if (key == "dangling, pictures" && number(value, v) && v >= 1.0)
            {
                n.danglePictures = static_cast<int>(v);
                used = true;
            }
            if (key == "getting on from") // a tool's own: where its wrestler stands; in a match, wherever they stood
                used = true;
            if (!used)
                skipped.push_back("numbers \"" + key + "\"");
        }
    }

    void readRopeBends(const Json &bends, CageFile &f, std::vector<std::string> &skipped)
    {
        f.ropeBends.clear();
        for (const auto &[key, list] : bends.members)
        {
            std::vector<CageWindow> windows;
            if (list.isArray())
                for (const Json &w : list.items)
                {
                    const Json *from = w.find("from"), *to = w.find("to"), *what = w.find("what");
                    double a = 0.0, b = 0.0;
                    const uint32_t code = what && what->isString() ? vendetta_moves::windowCode(what->text) : 0u;
                    const bool toEnd = to && to->isString() && to->text == "end";
                    if (!from || !number(*from, a) || !to || (!toEnd && !number(*to, b)) || code == 0u || a < 0.0 ||
                        (!toEnd && b < a) || windows.size() >= 4u)
                    {
                        skipped.push_back("rope bends \"" + key + "\" (a bend not understood, or more than four)");
                        continue;
                    }
                    windows.push_back({static_cast<uint32_t>(a), toEnd ? kToEnd : std::min(kToEnd, static_cast<uint32_t>(b)), code});
                }
            f.ropeBends[key] = windows;
        }
    }
}

std::string vendetta_cage::clipKey(uint32_t number)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%X", number);
    return text;
}

const vendetta_cage::CageFile &vendetta_cage::cageFile() { return vendetta_cage_file::g_file; }

void vendetta_cage::readCageFile()
{
    using namespace vendetta_cage_file;
    g_file = standard();
    const std::filesystem::path path = cageFolder() / "cage.json";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
    {
        std::cerr << "[cage] no cage.json in " << cageFolder().string() << ": the standard cage's values" << std::endl;
        return;
    }
    std::ifstream in(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Json root;
    std::string why;
    if (!vendetta_mods::parseJson(text, root, why) || !root.isObject())
    {
        std::cerr << "[cage] " << path.string() << " can't be read" << (why.empty() ? "" : ": " + why)
                  << ": the standard cage's values" << std::endl;
        return;
    }
    std::vector<std::string> skipped;
    if (const Json *bends = root.find("rope bends"); bends && bends->isObject())
        readRopeBends(*bends, g_file, skipped);
    if (const Json *sounds = root.find("sounds"); sounds && sounds->isObject())
        for (const auto &[key, list] : sounds->members)
            g_file.sounds[key] = list;
    if (const Json *speeds = root.find("speeds"); speeds && speeds->isObject())
        for (const auto &[key, value] : speeds->members)
        {
            double v = 0.0;
            if (number(value, v) && v >= 1.0 && v <= 255.0)
                g_file.speeds[key] = static_cast<uint32_t>(v);
            else
                skipped.push_back("speeds \"" + key + "\" (1 to 255)");
        }
    if (const Json *loops = root.find("loops"); loops && loops->isArray())
    {
        g_file.loops.clear();
        for (const Json &key : loops->items)
            if (key.isString())
                g_file.loops.insert(key.text);
    }
    if (const Json *numbers = root.find("numbers"); numbers && numbers->isObject())
        readNumbers(*numbers, g_file.numbers, skipped);
    g_file.from = path.string();
    std::cerr << "[cage] " << path.string() << ": rope bends for " << g_file.ropeBends.size() << " clips, sounds for "
              << g_file.sounds.size() << ", " << g_file.speeds.size() << " speeds, " << g_file.loops.size() << " loops"
              << std::endl;
    for (const std::string &s : skipped)
        std::cerr << "[cage] cage.json: " << s << " not used" << std::endl;
}
