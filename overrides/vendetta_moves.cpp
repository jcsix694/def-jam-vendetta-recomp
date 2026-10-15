// Def Jam Vendetta (USA, SLUS-20639) - new moves from a mods\ folder: reading and checking (vendetta_moves.h).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// A move folder becomes the game's own records: the move record (31 bit-packed fields, FUN_0011c608) and one playback
// record per animation (30 bit-packed fields, FUN_0011c968). The files carry only the values the move's type reads; every
// other field gets what all the game's own moves of that type have there:
//  - the "nothing" codes: 0x7FE (never) for an unused hit window / damage frame, 0x7FF for every other unused frame
//    (FUN_00132158 turns an unset 0x7FF hit window or damage frame into the clip's end), 0, and an empty window
//    (0x7FF, 0x7FF, 127);
//  - move field 17 = field 18 (FUN_0011c608 overwrites one with the other); playback field 17 bits 4 + 5 exactly when
//    the animation has an eye track; a grapple's hold switches from its second stage (read from the stage); a second
//    stage's 0x200 / 0x400 from its grapple (pin / hold); a ground strike's field 19 by where it starts;
//  - each type's constants (the never-read field 21, the never-tested 0x2000, a corner move's 0x1 and 0x400000, ...).
// Every key must belong to the move's type and every word must be on its list: a mistake skips that move, with a
// message naming the mod, the file and the key.
//
// VENDETTA_MOVECHECK=<folder>: every move folder in <folder> is read the same way; where a folder holds _expected.json
// (written by the mod tool's round trip from the game's own moves), the records built here must equal it field for field.
#include "vendetta_moves.h"

#include "ps2_runtime.h"
#include "vendetta_clipsounds.h"
#include "vendetta_mods.h"
#include "vendetta_store.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace vendetta_moves
{
    namespace
    {
        using vendetta_mods::Json;

        constexpr uint32_t kNever = 0x7FEu; // playback hit window / damage frame: never
        constexpr uint32_t kNone = 0x7FFu;  // every other playback frame: none (the clip's end for some)
        constexpr uint32_t kWindows = 18u;  // bit in a playback mask for the windows
        constexpr uint32_t kSharedNames = 0x259AB8u; // the shared clips' (ids 10000 +) names
        constexpr uint32_t kLyingPoses = 0x236AA8u;  // the 28 lying poses (endings 51-78), clip ids
        constexpr uint32_t kRunningReactions = 0x28D4u; // + move field 18: the running hit's reaction clip
        constexpr uint32_t kFirstTimeOnly = 0x10000000u;

        uint8_t *g_rdram = nullptr;

        uint32_t read32(uint32_t address)
        {
            uint32_t value = 0;
            std::memcpy(&value, g_rdram + (address & PS2_RAM_MASK), 4);
            return value;
        }

        std::string guestString(uint32_t address)
        {
            std::string text;
            for (uint32_t i = 0; i < 64u; ++i)
            {
                const char c = static_cast<char>(g_rdram[(address + i) & PS2_RAM_MASK]);
                if (c == 0)
                    break;
                text += c;
            }
            return text;
        }

        std::string sharedClipName(uint32_t id) { return guestString(read32(kSharedNames + 4u * (id - 10000u))); }

        constexpr uint32_t mask(std::initializer_list<uint32_t> bits)
        {
            uint32_t m = 0;
            for (uint32_t b : bits)
                m |= 1u << b;
            return m;
        }

        // ------------------------------------------------------------------------------------------------- the words
        const std::vector<std::string> kAreas = {"head", "body", "arms", "legs", "whole body"};
        const std::vector<std::string> kHits = {"head", "body", "arms", "legs", "none"};
        const std::vector<std::string> kBodyPoints = {
            "pelvis", "hips", "belly", "chest", "neck", "head", "left collarbone", "left shoulder", "left elbow",
            "left fist", "right collarbone", "right shoulder", "right elbow", "right fist", "left hip", "left knee",
            "left ankle", "right hip", "right knee", "right ankle", "both arms", "both legs", "whole body"};
        const std::vector<std::string> kReactions = {
            "head snap", "twisting snap (hooks, chops)", "doubles over (body shots)", "knee buckles (leg kicks)",
            "low blow (sinks and falls)", "big arch back", "head snap, second kind"};
        const std::vector<std::string> kLeaves = {"where they are", "in the ring", "outside on the floor", "on the apron"};
        const std::map<uint32_t, std::string> kWindowWhat = {
            {1, "ropes, own side 1"}, {2, "ropes, own side 2"}, {3, "ropes, own side 3"}, {4, "ropes, nearest side"},
            {5, "ropes, corner"}, {6, "ring shakes"},
            {7, "top rope, right hand"}, {8, "middle rope, right hand"}, {9, "bottom rope, right hand"},
            {10, "top rope, left hand"}, {11, "middle rope, left hand"}, {12, "bottom rope, left hand"},
            {13, "top rope, both hands"}, {14, "middle rope, both hands"}, {15, "bottom rope, both hands"},
            {16, "top rope, right foot"}, {17, "middle rope, right foot"}, {18, "bottom rope, right foot"},
            {19, "top rope, left foot"}, {20, "middle rope, left foot"}, {21, "bottom rope, left foot"},
            {22, "top rope, both feet"}, {23, "middle rope, both feet"}, {24, "bottom rope, both feet"},
            {25, "screen shake"}, {26, "extra hit"}};
        const std::vector<std::string> kCorners = {"far right corner", "near right corner", "near left corner", "far left corner"};
        const std::map<std::string, uint32_t> kAerialF21 = {
            {"top rope onto a standing opponent", 3}, {"top rope onto a downed opponent", 1}, {"dive out of the ring", 1},
            {"apron to the floor", 3}, {"apron into the ring", 1}, {"springboard", 3}};
        const std::set<std::string> kTauntKinds = {"standing", "Blazin'", "crouched", "turnbuckle", "apron"};
        const std::map<std::string, uint32_t> kGroundStrikeF19 = {
            {"standing", 2}, {"running", 3}, {"standing, opponent half down", 0}, {"running, opponent half down", 3}};

        std::map<std::string, uint32_t> g_endings; // word -> playback field 11

        void buildEndings()
        {
            g_endings = {{"standing (code 0)", 0}, {"standing", 1}, {"standing, turned", 2}, {"crouched (Q601)", 3},
                         {"groggy", 4}, {"slumped in the corner", 21}, {"slumped in the corner (code 22)", 22}};
            for (uint32_t i = 0; i < 28u; ++i)
            {
                const uint32_t code = 51u + i;
                const std::string name = sharedClipName(read32(kLyingPoses + 4u * i));
                std::string word;
                if (code == 75u)
                    word = "sitting (" + name + ")";
                else if (code == 72u || code == 74u || code == 76u)
                    word = "on all fours (" + name + ")";
                else
                    word = std::string("lying face ") + (code % 2u ? "up" : "down") + " (" + name + ")";
                g_endings[word] = code;
            }
        }

        // ------------------------------------------------------------------------------------------------- the types
        // playback fields each role carries
        constexpr uint32_t kGA = mask({2, 3, 4, 7, 10, 11, 16, 17, kWindows});
        constexpr uint32_t kGRC = mask({2, 3, 4, 8, 9, 10, 11, 12, 16, 17, kWindows});
        constexpr uint32_t kSTK = mask({0, 1, 2, 3, 8, 11, 16, 17, kWindows});
        constexpr uint32_t kAIRP = mask({0, 1, 2, 3, 5, 6, 8, 10, 11, 16, 17, kWindows});
        constexpr uint32_t kLAND = mask({2, 3, 8, 10, 11, 16, 17, kWindows});
        constexpr uint32_t kTAUNT = mask({2, 3, 7, 11, 15, 16, 17, kWindows});
        constexpr uint32_t kSCENE = mask({14, 15, 16, 17, kWindows});
        constexpr uint32_t kPARRY = mask({2, 3, 4, 8, 9, 10, 11, 16, 17, kWindows});
        constexpr uint32_t kREACH = mask({0, 1, 2, 3, 11, 16, 17, kWindows});
        // move record fields each type carries
        constexpr uint32_t kDMG = mask({0, 1, 2, 3, 5, 6, 12, 13, 14, 15, 16, 26});
        constexpr uint32_t kST = kDMG | mask({7, 8, 9, 11});
        constexpr uint32_t kAIR = kDMG | mask({7, 8, 9, 10, 19, 25});

        using Switches = std::vector<std::pair<uint32_t, std::string>>;
        const Switches kCommon = {{0x08000000u, "no damage"}, {0x10000000u, "repeat penalty"}, {0x20000000u, "counts for combos"}};

        Switches plus(Switches base, std::initializer_list<std::pair<uint32_t, std::string>> more)
        {
            base.insert(base.end(), more.begin(), more.end());
            return base;
        }

        struct Role
        {
            std::string name;
            uint32_t carried;
        };

        struct Type
        {
            std::string name;
            std::vector<Role> roles;
            uint32_t fields = 0;
            Switches switches;
            int f21 = 1;          // -1: by the aerial's kind
            bool sw2000 = true;
            uint32_t fillBits = 0;
            int f10 = -1, f20 = -1;
            bool grapple = false; // "reversal" and "lead-in" are written apart from "switches"
        };

        std::vector<Type> g_types;

        void buildTypes()
        {
            const Switches grapple = plus(kCommon, {{0x800000u, "receiver counts as standing"}});
            const Switches reversible = plus(grapple, {{0x1u, "can be reversed"}});
            const Switches stage = plus(grapple, {{0x100u, "rope break"}, {0x4u, "receiver stays down"},
                                                  {0x2u, "attacker ends knocked down"}, {0x8u, "reversed facing"},
                                                  {0x200000u, "held wrestler can't crawl to the ropes"}, {0x400000u, "corner hold"}});
            const Switches aerial = plus(kCommon, {{0x8u, "backflip"}, {0x40u, "knocks them flat on the back"},
                                                   {0x100u, "never hits a lying opponent"}, {0x200u, "cover into a pin"},
                                                   {0x10u, "rolled-away landing"}, {0x100000u, "turn round into the cover"}});
            const Switches alsoHits = plus(kCommon, {{0x10000u, "also hits sitting or kneeling"}, {0x20000u, "also hits lying down"}});
            const Switches taunt = {{0x10000000u, "repeat penalty"}, {0x20000000u, "counts for combos"}};
            const std::vector<Role> pair = {{"attacker", kGA}, {"receiver", kGRC}};
            const std::vector<Role> stageRoles = {
                {"attacker loop", mask({2, 3, 4, 7, 8, 11, 16, 17, kWindows})},
                {"receiver loop", mask({2, 3, 4, 8, 11, 12, 16, 17, kWindows})},
                {"attacker released", mask({2, 3, 4, 7, 8, 9, 10, 11, 16, 17, kWindows})},
                {"receiver released", mask({2, 3, 4, 7, 8, 9, 10, 11, 16, 17, kWindows})},
                {"attacker finish", mask({2, 3, 4, 7, 8, 9, 10, 11, 16, 17, kWindows})},
                {"receiver finish", mask({2, 3, 4, 7, 8, 9, 10, 11, 16, 17, kWindows})},
                {"attacker broken up", mask({2, 3, 4, 7, 8, 9, 10, 11, 16, 17, kWindows})},
                {"receiver broken up", mask({2, 3, 4, 7, 8, 9, 10, 11, 16, 17, kWindows})}};
            const std::vector<Role> catchRoles = {{"reach", kREACH}, {"catcher", kGA}, {"runner", kGRC}};
            const uint32_t links = mask({29, 27});
            auto add = [](Type t) { g_types.push_back(std::move(t)); };
            Type t;
            t = {"front grapple", pair, kDMG | mask({22}) | links, reversible, 3}; t.grapple = true; add(t);
            t = {"back grapple", pair, kDMG | links, reversible, 1, false}; t.grapple = true; add(t);
            t = {"Blazin' from the front", pair, kDMG | mask({4}) | links, grapple, 5}; t.grapple = true; add(t);
            t = {"Blazin' from behind", pair, kDMG | mask({4}) | links, grapple, 5}; t.grapple = true; add(t);
            add({"hold", stageRoles, kDMG, stage, 1});
            add({"pin", stageRoles, kDMG, stage, 1});
            add({"submission", stageRoles, kDMG, stage, 1});
            t = {"Irish whip catch", catchRoles, kDMG | links, grapple, 3}; t.grapple = true; add(t);
            t = {"running catch at an edge", catchRoles, kDMG | mask({7, 8, 9}) | links, grapple, 1}; t.grapple = true; add(t);
            t = {"corner move", {{"attacker", kGA & ~(1u << 4)}, {"receiver", kGRC & ~(1u << 4)}}, kDMG | links,
                 plus(grapple, {{0x100000u, "lead-in onto the top turnbuckle"}, {0x4u, "receiver stays down"}, {0x8u, "reversed facing"}}),
                 1, true, 0x1u | 0x400000u};
            t.grapple = true; add(t);
            t = {"grapple across the ropes", pair, kDMG | links, reversible, 1}; t.grapple = true; add(t);
            t = {"ring-edge grapple", pair, kDMG | mask({22}) | links, reversible, 1}; t.grapple = true; add(t);
            t = {"ground grapple", pair, kDMG | links, reversible, 1}; t.grapple = true; add(t);
            add({"standing strike", {{"striker", kSTK}}, kST | mask({23, 30}), alsoHits, 1, true, 0x800000u});
            add({"combo follow-up", {{"striker", kSTK}}, kST | mask({23, 30}), alsoHits, 1, true, 0x800000u});
            add({"hard strike", {{"striker", kSTK}}, kST | mask({30}), alsoHits, 1});
            add({"parry pair", {{"parrier", kPARRY}, {"striker", kPARRY}}, kDMG, kCommon, 1});
            t = {"running attack", {{"striker", kSTK}}, kST | mask({18, 30}), alsoHits, 1}; t.f10 = 2; add(t);
            add({"strike at a runner", {{"striker", kSTK}}, kST, alsoHits, 1});
            add({"ground strike", {{"striker", kSTK}}, kDMG | mask({7, 8, 9}), kCommon, 1});
            t = {"rising strike", {{"striker", kSTK}}, kST, alsoHits, 1}; t.f20 = 2; add(t);
            add({"aerial", {{"diver", kAIRP}}, kAIR, aerial, -1});
            add({"taunt", {{"taunter", kTAUNT}}, mask({1, 2, 26}), taunt, 1});
            add({"intro", {{"wrestler", kSCENE}}, 0, {}, 1});
            add({"victory", {{"wrestler", kSCENE}}, 0, {}, 1});
        }

        const Type *typeNamed(const std::string &name)
        {
            for (const Type &t : g_types)
                if (t.name == name)
                    return &t;
            return nullptr;
        }

        const std::vector<Role> kDownedRoles = {{"dive", kAIRP}, {"landing or cover", kLAND}, {"rolled-away landing", kLAND},
                                                {"miss", kLAND}, {"landing without a cover", kLAND}};
        constexpr uint32_t kHoldBits = 0x2u | 0x4u | 0x8u | 0x80u | 0x100u | 0x100000u | 0x200000u | 0x400000u;

        // ------------------------------------------------------------------------------------------------- a move
        struct CameraKey
        {
            int32_t frame = 0;
            std::array<float, 3> eye{}, at{};
            float roll = 0.0f, fov = 0.0f;
        };

        struct Animation
        {
            std::string role;
            std::filesystem::path file;
            std::vector<uint8_t> bytes;
            std::array<uint32_t, 30> play{};
            std::vector<vendetta_clipsounds::Event> sounds;
            std::vector<std::vector<CameraKey>> cameras; // intro: the four corners in spot order; victory: one
            std::vector<uint8_t> eyes;
        };

        struct Move
        {
            std::string type, kind, name;
            std::filesystem::path folder;
            std::array<uint32_t, 31> record{};
            std::vector<Animation> animations;
            std::unique_ptr<Move> stage, next; // field 29 / field 30
        };

        // Reads one move folder; problems go to `problems` (a non-empty list = the move is not used).
        class Reader
        {
        public:
            explicit Reader(std::string mod) : m_mod(std::move(mod)) {}
            std::vector<std::string> problems;

            std::unique_ptr<Move> read(const std::filesystem::path &folder, bool isStage)
            {
                auto mv = std::make_unique<Move>();
                mv->folder = folder;
                const std::filesystem::path file = folder / "move.json";
                Json j;
                if (!load(file, j))
                    return nullptr;
                m_where = where(file);
                const Json *type = j.find("type");
                const Type *t = type && type->isString() ? typeNamed(type->text) : nullptr;
                if (!t)
                {
                    fail("\"type\": " + (type && type->isString() ? "\"" + type->text + "\"" : std::string("missing")) +
                         " is not one of the move types");
                    return nullptr;
                }
                mv->type = t->name;
                std::array<uint32_t, 31> &m = mv->record;
                const uint32_t F = t->fields;
                auto has = [F](uint32_t field) { return (F >> field) & 1u; };

                // which keys this type has
                std::set<std::string> allowed = {"type", "name", "animations"};
                if (t->name == "aerial" || t->name == "taunt")
                    allowed.insert("kind");
                if (has(0))
                    allowed.insert("damage");
                if (has(1))
                    allowed.insert("momentum");
                if (has(26))
                    allowed.insert("points");
                if (has(22) || hasSwitch(*t, 0x1u))
                    allowed.insert("reversal");
                if (has(4))
                    allowed.insert("Blazin' KO");
                if (t->name == "grapple across the ropes")
                    allowed.insert("on the apron");
                if (t->name == "ground grapple")
                    allowed.insert("position");
                if (t->name == "ground strike")
                    allowed.insert("from");
                if (t->name == "corner move")
                    allowed.insert("lead-in");
                if (has(7))
                    allowed.insert("contact points");
                if (has(10))
                    allowed.insert("aims at");
                if (has(25))
                    allowed.insert("reach");
                if (has(19))
                    allowed.insert("miss costs");
                if (has(11))
                    allowed.insert("reaction");
                if (has(18))
                    allowed.insert("running reaction");
                if (has(23))
                    allowed.insert("combo window");
                if (has(30))
                    allowed.insert(t->name == "standing strike" || t->name == "combo follow-up" ? "next hit" : "parry");
                if (!t->switches.empty())
                    allowed.insert("switches");
                if (has(29))
                    allowed.insert("then");
                for (const auto &[key, value] : j.members)
                    if (!allowed.count(key))
                        fail("\"" + key + "\" is not a key a " + t->name + " has");

                if (const Json *name = j.find("name"); name && name->isString())
                    mv->name = name->text;
                else if (t->name != "intro" && t->name != "victory")
                    fail("needs \"name\": the name on screen");

                // the kind
                if (t->name == "aerial")
                {
                    const Json *k = j.find("kind");
                    if (!k || !k->isString() || !kAerialF21.count(k->text))
                        fail("\"kind\" should be one of the six dives (\"top rope onto a downed opponent\", ...)");
                    else
                        mv->kind = k->text;
                }
                if (t->name == "taunt")
                {
                    const Json *k = j.find("kind");
                    if (!k || !k->isString() || !kTauntKinds.count(k->text))
                        fail("\"kind\" should be standing, Blazin', crouched, turnbuckle or apron");
                    else
                        mv->kind = k->text;
                }

                // ---- the move record
                if (has(0))
                    if (const Json *d = object(j, "damage"))
                    {
                        m[0] = number(*d, "hit", 0, 255, "damage");
                        m[3] = number(*d, "lasting", 0, 127, "damage");
                        m[5] = word(*d, "attacks with", kAreas, "damage");
                        m[6] = word(*d, "hits", kHits, "damage");
                        if (const Json *a = object(*d, "areas", "damage"))
                            for (size_t i = 0; i < kAreas.size(); ++i)
                                m[12 + i] = number(*a, kAreas[i], 0, 7, "damage \"areas\"");
                    }
                if (has(1))
                    if (const Json *mo = object(j, "momentum"))
                    {
                        m[1] = number(*mo, "gained", 0, 63, "momentum");
                        const int64_t o = signedNumber(*mo, "opponent", -32, 31, "momentum");
                        m[2] = static_cast<uint32_t>(o < 0 ? o + 64 : o);
                    }
                if (has(26))
                    m[26] = number(j, "points", 0, 1023, "");
                if (const Json *rv = j.find("reversal"); rv && allowed.count("reversal"))
                {
                    if (!rv->isObject())
                        fail("\"reversal\" should be { \"can be reversed\": true / false" +
                             std::string(has(22) ? ", \"set\": 0-3 }" : " }"));
                    else
                    {
                        if (flag(*rv, "can be reversed", "reversal"))
                            m[24] |= 0x1u;
                        if (has(22))
                            m[22] = number(*rv, "set", 0, 15, "reversal");
                    }
                }
                if (has(4) && optionalFlag(j, "Blazin' KO"))
                    m[4] = 5;
                if (t->name == "corner move")
                {
                    const std::string lead = text(j, "lead-in", {"standing", "onto the top turnbuckle"});
                    if (lead == "onto the top turnbuckle")
                        m[24] |= 0x100000u;
                }
                if (t->name == "grapple across the ropes")
                    text(j, "on the apron", {"opponent", "attacker"});
                if (t->name == "ground grapple")
                    if (const Json *p = object(j, "position"))
                    {
                        text(*p, "at", {"head", "feet"});
                        text(*p, "opponent", {"face up", "face down", "sitting", "on all fours"});
                    }
                if (has(7))
                {
                    const Json *cp = j.find("contact points");
                    if (cp && !cp->isArray())
                        fail("\"contact points\" should be a list of up to three body points");
                    for (size_t i = 0; cp && cp->isArray() && i < 3; ++i)
                        m[7 + i] = i < cp->items.size() ? wordOf(cp->items[i], kBodyPoints, "\"contact points\"") : 0u;
                    if (cp && cp->isArray() && cp->items.size() > 3)
                        fail("\"contact points\" has more than three");
                }
                if (has(10))
                    m[10] = word(j, "aims at", kBodyPoints, "");
                if (has(25))
                    m[25] = number(j, "reach", 0, 255, "");
                if (has(19))
                    m[19] = number(j, "miss costs", 0, 31, "");
                if (has(11))
                    m[11] = word(j, "reaction", kReactions, "");
                if (has(18))
                {
                    const Json *rr = j.find("running reaction");
                    bool ok = rr && rr->isString();
                    if (ok && rr->text == "none")
                        m[18] = 0;
                    else if (ok)
                    {
                        ok = false;
                        for (uint32_t k = 1; k < 16u && !ok; ++k)
                            if (sharedClipName(kRunningReactions + k) == rr->text)
                            {
                                m[18] = k;
                                ok = true;
                            }
                    }
                    if (!ok)
                        fail("\"running reaction\" should be \"none\" or one of the running knockdowns (R705 ... R708)");
                }
                if (has(23))
                    m[23] = number(j, "combo window", 0, 31, "");
                if (const Json *sw = j.find("switches"); sw && allowed.count("switches"))
                {
                    if (!sw->isObject())
                        fail("\"switches\" should be { \"<switch>\": true / false, ... }");
                    else
                        for (const auto &[name, value] : sw->members)
                        {
                            uint32_t bit = 0;
                            for (const auto &[b, n] : t->switches)
                                if (n == name && !(t->grapple && (b == 0x1u || b == 0x100000u)))
                                    bit = b;
                            if (bit == 0u)
                                fail("\"switches\": \"" + name + "\" is not a switch a " + t->name + " has");
                            else if (value.type != Json::Type::Bool)
                                fail("\"switches\": \"" + name + "\" should be true or false");
                            else if (value.boolean)
                                m[24] |= bit;
                        }
                }

                // ---- linked parts
                if (has(30))
                {
                    const bool strings = t->name == "standing strike" || t->name == "combo follow-up";
                    const std::string key = strings ? "next hit" : "parry";
                    const Json *v = j.find(key);
                    const std::string value = v && v->isString() ? v->text : (v ? "" : "none");
                    if (value.empty())
                        fail("\"" + key + "\" should be a folder in this one, or " +
                             (strings ? std::string("\"none\"") : std::string("\"shared, by limb\" or \"shared, running hit\"")));
                    else if (!strings && value == "shared, running hit")
                        m[24] |= 0x40000u;
                    else if (value != "none" && value != "shared, by limb")
                    {
                        mv->next = readPart(folder, value, key);
                        if (mv->next && mv->next->type != (strings ? "combo follow-up" : "parry pair"))
                            fail("\"" + key + "\": the folder \"" + value + "\" holds a " + mv->next->type + ", not a " +
                                 (strings ? "combo follow-up" : "parry pair"));
                    }
                }
                if (has(29))
                {
                    const Json *then = j.find("then");
                    if (!then || (then->isString() && then->text == "nothing"))
                    {
                    }
                    else if (then->isObject() && then->find("second stage") && then->find("second stage")->isString())
                    {
                        mv->stage = readPart(folder, then->find("second stage")->text, "then");
                        if (mv->stage)
                        {
                            const std::string &st = mv->stage->type;
                            if (st != "hold" && st != "pin" && st != "submission")
                                fail("\"then\": the second stage's folder holds a " + st + ", not a hold, pin or submission");
                            m[24] |= st == "pin" ? 0x200u : 0x400u;
                            m[24] |= mv->stage->record[24] & kHoldBits; // a hold's switches are read from its stage
                        }
                    }
                    else if (then->isObject() && then->find("chain into slot"))
                        m[27] = number(*then, "chain into slot", 0, 127, "then");
                    else
                        fail("\"then\" should be \"nothing\", { \"second stage\": \"<folder>\" } or { \"chain into slot\": <slot> }");
                }

                // ---- the fills
                m[21] = t->f21 >= 0 ? static_cast<uint32_t>(t->f21) : (kAerialF21.count(mv->kind) ? kAerialF21.at(mv->kind) : 1u);
                if (t->sw2000)
                    m[24] |= 0x2000u;
                m[24] |= t->fillBits;
                if (t->f10 >= 0)
                    m[10] = static_cast<uint32_t>(t->f10);
                if (t->f20 >= 0)
                    m[20] = static_cast<uint32_t>(t->f20);
                if (t->name == "ground strike")
                {
                    const Json *from = j.find("from");
                    if (!from || !from->isString() || !kGroundStrikeF19.count(from->text))
                        fail("\"from\" should be standing, running, \"standing, opponent half down\" or \"running, opponent half down\"");
                    else
                        m[19] = kGroundStrikeF19.at(from->text);
                }
                if (t->name == "aerial" && mv->kind == "top rope onto a downed opponent")
                    m[24] |= 0x20u;
                if (t->name == "taunt")
                    m[5] = m[6] = 4;
                if (t->name == "intro" || t->name == "victory")
                {
                    m[1] = 17;
                    m[5] = m[6] = 4;
                    if (t->name == "victory")
                        m[24] |= 0x30000000u;
                }
                if (t->name == "hold" || t->name == "pin" || t->name == "submission")
                {
                    if (t->name == "submission")
                        m[24] |= 0x80u;
                    if (isStage)
                        m[24] |= t->name == "pin" ? 0x200u : 0x400u;
                }
                m[17] = m[18];

                // ---- the animations
                const std::vector<Role> &roles =
                    t->name == "aerial" && mv->kind == "top rope onto a downed opponent" ? kDownedRoles : t->roles;
                const Json *anims = j.find("animations");
                if (!anims || !anims->isObject())
                    fail("needs \"animations\": { \"<role>\": \"<file>.bin\", ... }");
                else
                {
                    for (const auto &[role, value] : anims->members)
                        if (std::none_of(roles.begin(), roles.end(), [&](const Role &r) { return r.name == role; }))
                            fail("\"animations\": \"" + role + "\" is not a role of a " + t->name);
                    for (size_t k = 0; k < roles.size(); ++k)
                    {
                        const Json *f = anims->find(roles[k].name);
                        if (!f || !f->isString())
                        {
                            fail("\"animations\" has no \"" + roles[k].name + "\"");
                            continue;
                        }
                        mv->animations.push_back(readAnimation(folder, f->text, roles[k], *t, mv->kind, k));
                    }
                }
                return mv;
            }

        private:
            std::string m_mod;
            std::string m_where;

            std::string where(const std::filesystem::path &file) const { return m_mod + ": " + file.string(); }
            void fail(const std::string &what) { problems.push_back(m_where + ": " + what); }

            bool load(const std::filesystem::path &file, Json &out)
            {
                std::ifstream in(file, std::ios::binary);
                if (!in)
                {
                    problems.push_back(where(file) + ": can't be read (missing?)");
                    return false;
                }
                const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                std::string error;
                if (!vendetta_mods::parseJson(text, out, error) || !out.isObject())
                {
                    problems.push_back(where(file) + ": not a JSON object" + (error.empty() ? "" : ": " + error));
                    return false;
                }
                return true;
            }

            static bool hasSwitch(const Type &t, uint32_t bit)
            {
                return std::any_of(t.switches.begin(), t.switches.end(), [bit](const auto &s) { return s.first == bit; });
            }

            std::string at(const std::string &in, const std::string &key) const
            {
                return in.empty() ? "\"" + key + "\"" : in + " \"" + key + "\"";
            }

            const Json *object(const Json &j, const std::string &key, const std::string &in = "")
            {
                const Json *v = j.find(key);
                if (!v || !v->isObject())
                {
                    fail(at(in, key) + " is missing or not { ... }");
                    return nullptr;
                }
                return v;
            }

            uint32_t number(const Json &j, const std::string &key, int64_t low, int64_t high, const std::string &in)
            {
                return static_cast<uint32_t>(signedNumber(j, key, low, high, in));
            }

            int64_t signedNumber(const Json &j, const std::string &key, int64_t low, int64_t high, const std::string &in)
            {
                const Json *v = j.find(key);
                if (!v || v->type != Json::Type::Number || v->number != static_cast<double>(static_cast<int64_t>(v->number)) ||
                    v->number < static_cast<double>(low) || v->number > static_cast<double>(high))
                {
                    fail(at(in, key) + " should be a whole number from " + std::to_string(low) + " to " + std::to_string(high));
                    return low < 0 ? 0 : low;
                }
                return static_cast<int64_t>(v->number);
            }

            bool flag(const Json &j, const std::string &key, const std::string &in)
            {
                const Json *v = j.find(key);
                if (!v || v->type != Json::Type::Bool)
                {
                    fail(at(in, key) + " should be true or false");
                    return false;
                }
                return v->boolean;
            }

            bool optionalFlag(const Json &j, const std::string &key)
            {
                const Json *v = j.find(key);
                if (v && v->type != Json::Type::Bool)
                    fail("\"" + key + "\" should be true or false");
                return v && v->type == Json::Type::Bool && v->boolean;
            }

            uint32_t wordOf(const Json &v, const std::vector<std::string> &words, const std::string &what)
            {
                if (v.isString())
                    for (size_t i = 0; i < words.size(); ++i)
                        if (words[i] == v.text)
                            return static_cast<uint32_t>(i);
                fail(what + ": \"" + (v.isString() ? v.text : std::string("?")) + "\" is not one of its words");
                return 0;
            }

            uint32_t word(const Json &j, const std::string &key, const std::vector<std::string> &words, const std::string &in)
            {
                const Json *v = j.find(key);
                if (!v)
                {
                    fail(at(in, key) + " is missing");
                    return 0;
                }
                return wordOf(*v, words, at(in, key));
            }

            std::string text(const Json &j, const std::string &key, std::initializer_list<const char *> words)
            {
                const Json *v = j.find(key);
                if (v && v->isString())
                    for (const char *w : words)
                        if (v->text == w)
                            return v->text;
                std::string list;
                for (const char *w : words)
                    list += (list.empty() ? "\"" : ", \"") + std::string(w) + "\"";
                fail("\"" + key + "\" should be one of " + list);
                return {};
            }

            std::unique_ptr<Move> readPart(const std::filesystem::path &folder, const std::string &sub, const std::string &key)
            {
                const std::filesystem::path path = (folder / sub).lexically_normal();
                if (path.string().compare(0, folder.lexically_normal().string().size(), folder.lexically_normal().string()) != 0)
                {
                    fail("\"" + key + "\": \"" + sub + "\" is outside the move's folder");
                    return nullptr;
                }
                const std::string here = m_where;
                auto part = read(path, key == "then");
                m_where = here;
                return part;
            }

            // a frame value: a number, or one of the words the key knows
            uint32_t frame(const Json &v, const std::map<uint32_t, std::string> &words, const std::string &what, uint32_t last)
            {
                if (v.type == Json::Type::Number && v.number == static_cast<double>(static_cast<int64_t>(v.number)) &&
                    v.number >= 0.0 && v.number < 2046.0)
                {
                    const uint32_t f = static_cast<uint32_t>(v.number);
                    // allowed (some of the game's own moves have it: such a frame is never reached), but said
                    if (f > last)
                        std::cerr << "[moves] " << m_where << ": " << what << ": frame " << f
                                  << " is past the animation's last frame (" << last << "): it never happens" << std::endl;
                    return f;
                }
                if (v.isString())
                    for (const auto &[code, w] : words)
                        if (w == v.text)
                            return code;
                std::string list;
                for (const auto &[code, w] : words)
                    list += ", \"" + w + "\"";
                fail(what + " should be a frame" + list);
                return kNone;
            }

            Animation readAnimation(const std::filesystem::path &folder, const std::string &fileName, const Role &role,
                                    const Type &t, const std::string &kind, size_t index)
            {
                Animation a;
                a.role = role.name;
                a.file = (folder / fileName).lexically_normal();
                const std::string here = m_where;
                {
                    std::ifstream in(a.file, std::ios::binary);
                    if (!in)
                        fail("\"animations\": \"" + fileName + "\" can't be read (missing?)");
                    else
                        a.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                }
                uint32_t last = 0;
                if (a.bytes.size() >= 3u)
                    last = static_cast<uint32_t>(a.bytes[0]) | (static_cast<uint32_t>(a.bytes[1]) << 8u);
                else if (!a.bytes.empty())
                    fail("\"animations\": \"" + fileName + "\" is shorter than an animation's header");
                if (last >= 0x7FEu)
                    fail("\"animations\": \"" + fileName + "\" has more frames than the game's timings hold (2045)");

                std::filesystem::path settingsFile = a.file;
                settingsFile.replace_extension(".json");
                Json s;
                std::array<uint32_t, 30> &p = a.play;
                p = {kNever, kNever, kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone, 0, 1, kNever, 0, 0, 0, 100, 0,
                     kNone, kNone, 127, kNone, kNone, 127, kNone, kNone, 127, kNone, kNone, 127};
                if (!load(settingsFile, s))
                    return a;
                m_where = where(settingsFile);
                const uint32_t c = role.carried;
                auto carries = [c](uint32_t field) { return (c >> field) & 1u; };

                std::set<std::string> allowed = {"speed", "loop", "windows", "sounds"};
                const bool catchWindow = role.name == "reach";
                const std::string hitKey = catchWindow ? "catch window" : "hit window";
                if (carries(0))
                    allowed.insert(hitKey);
                if (carries(2))
                    allowed.insert("can't be grabbed");
                const std::vector<std::tuple<uint32_t, std::string, std::map<uint32_t, std::string>>> singles = {
                    {4, "pushed together until", {{kNone, "end"}}}, {5, "takeoff", {{kNone, "none"}}},
                    {6, "landing", {{kNone, "end"}}}, {7, "replay mark", {{kNone, "none"}}}, {8, "impact", {{kNone, "none"}}},
                    {9, "release", {{kNone, "none"}}}, {12, "damage frame", {{kNever, "never"}, {kNone, "end"}}}};
                for (const auto &[f, key, words] : singles)
                    if (carries(f))
                        allowed.insert(key);
                if (carries(10))
                    allowed.insert("leaves them");
                if (carries(11))
                    allowed.insert("ending");
                if (carries(14))
                    allowed.insert("camera");
                if (carries(15))
                    allowed.insert("eyes");
                for (const auto &[key, value] : s.members)
                    if (!allowed.count(key))
                        fail("\"" + key + "\" is not a setting a " + t.name + "'s " + role.name + " has");

                p[16] = number(s, "speed", 1, 255, "");
                if (optionalFlag(s, "loop"))
                    p[17] |= 1u;
                const std::map<uint32_t, std::string> hitWords = {{kNever, "never"}, {kNone, "end"}};
                const std::map<uint32_t, std::string> endWord = {{kNone, "end"}};
                if (carries(0))
                    if (const Json *v = s.find(hitKey); v && !(v->isString() && v->text == "never"))
                    {
                        if (!v->isObject() || !v->find("from") || !v->find("to"))
                            fail("\"" + hitKey + "\" should be \"never\" or { \"from\": <frame>, \"to\": <frame> }");
                        else
                        {
                            p[0] = frame(*v->find("from"), hitWords, "\"" + hitKey + "\" \"from\"", last);
                            p[1] = frame(*v->find("to"), hitWords, "\"" + hitKey + "\" \"to\"", last);
                        }
                    }
                if (carries(2))
                    if (const Json *v = s.find("can't be grabbed"); v && !(v->isString() && v->text == "never"))
                    {
                        if (v->isString() && v->text == "whole animation")
                        {
                            p[2] = 0;
                            p[3] = kNone;
                        }
                        else if (!v->isObject() || !v->find("from") || !v->find("to"))
                            fail("\"can't be grabbed\" should be \"never\", \"whole animation\" or { \"from\": <frame>, \"to\": <frame> }");
                        else
                        {
                            p[2] = frame(*v->find("from"), endWord, "\"can't be grabbed\" \"from\"", last);
                            p[3] = frame(*v->find("to"), endWord, "\"can't be grabbed\" \"to\"", last);
                        }
                    }
                for (const auto &[f, key, words] : singles)
                    if (carries(f))
                        if (const Json *v = s.find(key))
                            p[f] = frame(*v, words, "\"" + key + "\"", last);
                if (carries(10))
                    if (const Json *v = s.find("leaves them"))
                        p[10] = wordOf(*v, kLeaves, "\"leaves them\"");
                if (carries(11))
                {
                    const Json *v = s.find("ending");
                    if (!v || !v->isString() || !g_endings.count(v->text))
                        fail("\"ending\" should be one of the endings (\"standing\", \"lying face up (V217)\", ...)");
                    else
                        p[11] = g_endings.at(v->text);
                }
                if (const Json *w = s.find("windows"))
                {
                    if (!w->isArray() || w->items.size() > 4u)
                        fail("\"windows\" should be a list of up to four { \"from\", \"to\", \"what\" }");
                    else
                        for (size_t i = 0; i < w->items.size(); ++i)
                        {
                            const Json &win = w->items[i];
                            const Json *what = win.isObject() ? win.find("what") : nullptr;
                            if (!what || !win.find("from") || !win.find("to"))
                            {
                                fail("\"windows\" " + std::to_string(i + 1) + " needs \"from\", \"to\" and \"what\"");
                                continue;
                            }
                            p[18 + 3 * i] = frame(*win.find("from"), endWord, "\"windows\" \"from\"", last);
                            p[19 + 3 * i] = frame(*win.find("to"), endWord, "\"windows\" \"to\"", last);
                            bool known = false;
                            for (const auto &[code, name] : kWindowWhat)
                                if (what->isString() && what->text == name)
                                {
                                    p[20 + 3 * i] = code;
                                    known = true;
                                }
                            if (!known)
                                fail("\"windows\" \"what\": \"" + (what->isString() ? what->text : std::string("?")) +
                                     "\" is not one of its words (\"ring shakes\", \"extra hit\", \"top rope, right hand\", ...)");
                        }
                }
                // role fills (the game's own moves all have these)
                if (t.name == "aerial" && kind == "top rope onto a downed opponent" && index == 1)
                    p[12] = 3;
                if (t.name == "aerial" && kind == "top rope onto a downed opponent" && index == 3)
                    p[12] = 5;
                if (t.name == "intro" || t.name == "victory")
                {
                    p[2] = 0;
                    p[3] = kNone;
                }
                if (const Json *snd = s.find("sounds"))
                    a.sounds = vendetta_clipsounds::readSounds(*snd, m_where, last);
                if (carries(14))
                    readCamera(s, t, a, last);
                if (carries(15))
                    if (const Json *e = s.find("eyes"))
                    {
                        std::ifstream in(folder / (e->isString() ? e->text : std::string()), std::ios::binary);
                        if (!e->isString() || !in)
                            fail("\"eyes\": the eye track file can't be read");
                        else
                        {
                            a.eyes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                            p[17] |= 48u; // the game's own animations have these two bits exactly when they have an eye track
                        }
                    }
                m_where = here;
                return a;
            }

            bool cameraKeys(const Json &list, std::vector<CameraKey> &keys, const std::string &what, uint32_t last)
            {
                if (!list.isArray() || list.items.empty())
                {
                    fail(what + " should be a list of keys");
                    return false;
                }
                for (const Json &k : list.items)
                {
                    CameraKey key;
                    const Json *fr = k.isObject() ? k.find("frame") : nullptr;
                    const Json *eye = k.isObject() ? k.find("eye") : nullptr;
                    const Json *at = k.isObject() ? k.find("looks at") : nullptr;
                    const Json *roll = k.isObject() ? k.find("roll") : nullptr;
                    const Json *fov = k.isObject() ? k.find("field of view") : nullptr;
                    auto vec = [](const Json *v, std::array<float, 3> &out)
                    {
                        if (!v || !v->isArray() || v->items.size() != 3u)
                            return false;
                        for (size_t i = 0; i < 3; ++i)
                        {
                            if (v->items[i].type != Json::Type::Number)
                                return false;
                            out[i] = static_cast<float>(v->items[i].number);
                        }
                        return true;
                    };
                    if (!fr || fr->type != Json::Type::Number || !vec(eye, key.eye) || !vec(at, key.at) || !roll ||
                        roll->type != Json::Type::Number || !fov || fov->type != Json::Type::Number)
                    {
                        fail(what + ": each key needs \"frame\", \"eye\" [x, y, z], \"looks at\" [x, y, z], \"roll\", \"field of view\"");
                        return false;
                    }
                    key.frame = static_cast<int32_t>(fr->number);
                    key.roll = static_cast<float>(roll->number);
                    key.fov = static_cast<float>(fov->number);
                    if (keys.empty() && key.frame != 0)
                        fail(what + ": the first key must be at frame 0");
                    if (!keys.empty() && key.frame <= keys.back().frame)
                        fail(what + ": the keys' frames must go up");
                    keys.push_back(key);
                }
                if (!keys.empty() && static_cast<uint32_t>(keys.back().frame) < last)
                    std::cerr << "[moves] " << m_where << ": " << what << " ends at frame " << keys.back().frame
                              << ", before the animation (" << last << "): it holds its last picture" << std::endl;
                return true;
            }

            void readCamera(const Json &s, const Type &t, Animation &a, uint32_t last)
            {
                const Json *cam = s.find("camera");
                if (!cam)
                {
                    fail("needs \"camera\"" + std::string(t.name == "intro" ? ": the four corners' keys" : ": its keys"));
                    return;
                }
                if (t.name == "intro")
                {
                    if (!cam->isObject())
                    {
                        fail("\"camera\" should be { \"far right corner\": [keys], ... } with all four corners");
                        return;
                    }
                    for (const auto &[key, value] : cam->members)
                        if (std::find(kCorners.begin(), kCorners.end(), key) == kCorners.end())
                            fail("\"camera\": \"" + key + "\" is not a corner (far right, near right, near left, far left corner)");
                    for (const std::string &corner : kCorners)
                    {
                        std::vector<CameraKey> keys;
                        const Json *list = cam->find(corner);
                        if (!list)
                            fail("\"camera\" has no \"" + corner + "\": an intro needs all four corners");
                        else
                            cameraKeys(*list, keys, "\"camera\" \"" + corner + "\"", last);
                        a.cameras.push_back(std::move(keys));
                    }
                }
                else
                {
                    std::vector<CameraKey> keys;
                    cameraKeys(*cam, keys, "\"camera\"", last);
                    a.cameras.push_back(std::move(keys));
                }
            }
        };

        uint32_t countParts(const Move &mv)
        {
            return 1u + (mv.stage ? countParts(*mv.stage) : 0u) + (mv.next ? countParts(*mv.next) : 0u);
        }

        // ---------------------------------------------------------------------------------------- VENDETTA_MOVECHECK
        uint32_t g_checked = 0, g_compared = 0, g_differences = 0;

        void compareExpected(const Move &mv)
        {
            const std::filesystem::path file = mv.folder / "_expected.json";
            std::ifstream in(file, std::ios::binary);
            if (in)
            {
                const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                Json e;
                std::string error;
                if (vendetta_mods::parseJson(text, e, error) && e.isObject() && e.find("record") && e.find("plays"))
                {
                    ++g_compared;
                    const Json &rec = *e.find("record");
                    for (size_t f = 0; f < 31 && f < rec.items.size(); ++f)
                    {
                        if (f == 28 || f == 29 || f == 30)
                            continue;
                        const uint32_t want = static_cast<uint32_t>(rec.items[f].number);
                        if (mv.record[f] != want && ++g_differences <= 40u)
                            std::cerr << "[moves] MOVECHECK " << mv.folder.string() << ": move field " << f << " is "
                                      << mv.record[f] << ", the tool has " << want << std::endl;
                    }
                    const Json &plays = *e.find("plays");
                    for (size_t k = 0; k < plays.items.size() && k < mv.animations.size(); ++k)
                        for (size_t f = 0; f < 30 && f < plays.items[k].items.size(); ++f)
                        {
                            if (f == 14 || f == 15)
                                continue;
                            const uint32_t want = static_cast<uint32_t>(plays.items[k].items[f].number);
                            if (mv.animations[k].play[f] != want && ++g_differences <= 40u)
                                std::cerr << "[moves] MOVECHECK " << mv.folder.string() << ": " << mv.animations[k].role
                                          << " playback field " << f << " is " << mv.animations[k].play[f] << ", the tool has "
                                          << want << std::endl;
                        }
                    if (plays.items.size() != mv.animations.size() && ++g_differences <= 40u)
                        std::cerr << "[moves] MOVECHECK " << mv.folder.string() << ": " << mv.animations.size()
                                  << " animations, the tool has " << plays.items.size() << std::endl;
                }
            }
            if (mv.stage)
                compareExpected(*mv.stage);
            if (mv.next)
                compareExpected(*mv.next);
        }

        void moveCheck()
        {
            const char *folder = std::getenv("VENDETTA_MOVECHECK");
            if (!folder || !*folder)
                return;
            uint32_t failed = 0;
            std::error_code ec;
            for (const auto &entry : std::filesystem::directory_iterator(folder, ec))
            {
                if (!entry.is_directory() || !std::filesystem::is_regular_file(entry.path() / "move.json"))
                    continue;
                Reader reader("MOVECHECK");
                std::unique_ptr<Move> mv = reader.read(entry.path(), false);
                ++g_checked;
                if (!reader.problems.empty() || !mv)
                {
                    if (++failed <= 20u)
                        for (const std::string &p : reader.problems)
                            std::cerr << "[moves] " << p << std::endl;
                    continue;
                }
                compareExpected(*mv);
            }
            std::cerr << "[moves] VENDETTA_MOVECHECK " << folder << ": " << g_checked << " move folders read, " << failed
                      << " with problems, " << g_compared << " records compared with the tool's, " << g_differences
                      << " differences" << std::endl;
        }

        // ------------------------------------------------------------------------------------------ the mods' moves
        std::vector<std::pair<std::string, std::unique_ptr<Move>>> g_moves; // (mod, move)

        void readModMoves()
        {
            for (const vendetta_mods::Mod &mod : vendetta_mods::loadedMods())
            {
                const Json *section = mod.manifest.find("new");
                const Json *list = section ? section->find("moves") : nullptr;
                if (!list)
                    continue;
                if (!list->isArray())
                {
                    std::cerr << "[moves] " << mod.name << ": \"new\": \"moves\" should be a list of folders: [\"moves/<name>\"]"
                              << std::endl;
                    continue;
                }
                for (const Json &item : list->items)
                {
                    if (!item.isString())
                    {
                        std::cerr << "[moves] " << mod.name << ": new moves: each folder should be a name in quotes" << std::endl;
                        continue;
                    }
                    const std::filesystem::path moveJson = vendetta_mods::modFile(mod, item.text + "/move.json", "move");
                    if (moveJson.empty())
                        continue;
                    Reader reader(mod.name);
                    std::unique_ptr<Move> mv = reader.read(moveJson.parent_path(), false);
                    if (!reader.problems.empty() || !mv)
                    {
                        for (const std::string &p : reader.problems)
                            std::cerr << "[moves] " << p << std::endl;
                        std::cerr << "[moves] " << mod.name << ": \"" << item.text << "\" skipped (" << reader.problems.size()
                                  << " problem" << (reader.problems.size() == 1 ? "" : "s") << ")" << std::endl;
                        continue;
                    }
                    std::cerr << "[moves] " << mod.name << ": " << mv->name << " (" << mv->type
                              << (mv->kind.empty() ? "" : ", " + mv->kind) << ") read: " << countParts(*mv) << " part(s), "
                              << mv->animations.size() << " animation(s); not added to the game yet" << std::endl;
                    g_moves.push_back({mod.name, std::move(mv)});
                }
            }
        }
    }

    namespace
    {
        // ------------------------------------------------------------------------------------------ into the game
        constexpr uint32_t kWaza = 0x2331C8u;    // the parsed move data
        constexpr uint32_t kRecordArray = 0x14u; // + 0x04 stride, + 0x08 count, + 0x14 the move records
        constexpr uint32_t kNames = 0x28u;       // the move names: + 0x2C count, + 0x30 u16 offsets, + 0x34 strings
        constexpr uint32_t kTimingStride = 0x1Cu, kTimingArray = 0x24u; // the playback records (one per clip id)
        constexpr uint32_t kPak0 = 0x140FD00u;   // the camera paths: "PAK ", size, count, self-relative offsets from + 0xC
        constexpr uint32_t kPak1 = 0x1DDC580u;   // the eye tracks, the same layout
        constexpr uint32_t kMaxRecord = 4095u;   // moveset slots and links hold 12 bits
        constexpr uint32_t kMaxCamera = 1023u;   // playback field 14 holds 10 bits
        constexpr uint32_t kMaxEyes = 511u;      // playback field 15 holds 9 bits (the track + 1)
        const std::array<uint32_t, 31> kMoveBits = {8, 6, 6, 7, 4, 3, 3, 5, 5, 5, 5, 4, 3, 3, 3, 3,
                                                    3, 3, 4, 5, 4, 5, 4, 5, 32, 8, 10, 7, 12, 12, 12};
        const std::array<uint32_t, 30> kPlayBits = {11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 3, 7, 11, 4, 10, 9, 8, 8,
                                                    11, 11, 7, 11, 11, 7, 11, 11, 7, 11, 11, 7};

        uint8_t *guest(uint32_t address) { return g_rdram + (address & PS2_RAM_MASK); }
        void write32(uint32_t address, uint32_t value) { std::memcpy(guest(address), &value, 4); }

        // most significant bit first, as FUN_0011c608 / FUN_0011c968 unpack them
        template <size_t N>
        void packBits(const std::array<uint32_t, N> &values, const std::array<uint32_t, N> &bits, uint8_t *out, size_t size)
        {
            std::memset(out, 0, size);
            uint32_t at = 0;
            for (size_t f = 0; f < N; ++f)
                for (uint32_t i = 0; i < bits[f]; ++i, ++at)
                    if ((values[f] >> (bits[f] - 1u - i)) & 1u)
                        out[at >> 3u] |= static_cast<uint8_t>(0x80u >> (at & 7u));
        }

        struct Flat
        {
            Move *move;
            Animation *animation;
            std::string mod;
            uint32_t clip = 0; // given by clipNumbered (0 = not added)
        };
        std::vector<Flat> g_flat;
        std::vector<std::pair<std::string, Move *>> g_top; // (mod, move) as read
        std::map<const Move *, uint32_t> g_record;         // each part's move record number

        void flatten(Move &mv, const std::string &mod)
        {
            for (Animation &a : mv.animations)
                g_flat.push_back({&mv, &a, mod});
            if (mv.stage)
                flatten(*mv.stage, mod);
            if (mv.next)
                flatten(*mv.next, mod);
        }

        // every part numbered, each one's clips consecutive (they were added in a row)
        bool clipsOk(const Move &mv, uint32_t &first)
        {
            first = 0;
            for (size_t k = 0; k < mv.animations.size(); ++k)
            {
                const auto it = std::find_if(g_flat.begin(), g_flat.end(),
                                             [&](const Flat &f) { return f.animation == &mv.animations[k]; });
                if (it == g_flat.end() || it->clip == 0u)
                    return false;
                if (k == 0)
                    first = it->clip;
                else if (it->clip != first + k)
                    return false;
            }
            uint32_t ignore = 0;
            return (!mv.stage || clipsOk(*mv.stage, ignore)) && (!mv.next || clipsOk(*mv.next, ignore));
        }

        uint32_t firstClip(const Move &mv)
        {
            for (const Flat &f : g_flat)
                if (f.move == &mv)
                    return f.clip;
            return 0;
        }

        void numberRecords(const Move &mv, uint32_t &next)
        {
            g_record[&mv] = next++;
            if (mv.stage)
                numberRecords(*mv.stage, next);
            if (mv.next)
                numberRecords(*mv.next, next);
        }

        // PAK 0 / PAK 1 (a table of self-relative offsets, entries anywhere): every entry moved to the store and the table
        // rewritten in place with room for `extra` more, the new entries' data placed too. Returns the first new entry's
        // number, or -1. `size` measures an entry.
        int growPak(uint32_t pak, const std::vector<std::vector<uint8_t>> &extra, uint32_t limit, const char *user,
                    uint32_t (*size)(uint32_t entry, uint32_t next))
        {
            if (std::memcmp(guest(pak), "PAK ", 4) != 0)
                return -1;
            const uint32_t count = read32(pak + 8u);
            if (count + extra.size() > limit + 1u)
                return -1;
            std::vector<uint32_t> at(count);
            for (uint32_t n = 0; n < count; ++n)
                at[n] = pak + 0xCu + 4u * n + read32(pak + 0xCu + 4u * n);
            std::vector<uint32_t> sorted = at;
            std::sort(sorted.begin(), sorted.end());
            const uint32_t end = pak + read32(pak + 4u);
            std::vector<uint32_t> moved(count);
            for (uint32_t n = 0; n < count; ++n)
            {
                const auto next = std::upper_bound(sorted.begin(), sorted.end(), at[n]);
                const uint32_t bytes = size(at[n], next != sorted.end() ? *next : end);
                moved[n] = vendetta_store::allocate(vendetta_store::Area::Boot, bytes, user, 16u, 0u);
                if (moved[n] == 0u)
                    return -1;
                std::memcpy(guest(moved[n]), guest(at[n]), bytes);
            }
            std::vector<uint32_t> added(extra.size());
            for (size_t i = 0; i < extra.size(); ++i)
            {
                added[i] = vendetta_store::allocate(vendetta_store::Area::Boot, static_cast<uint32_t>(extra[i].size()), user, 16u, 0u);
                if (added[i] == 0u)
                    return -1;
                std::memcpy(guest(added[i]), extra[i].data(), extra[i].size());
            }
            const uint32_t total = count + static_cast<uint32_t>(extra.size());
            for (uint32_t n = 0; n < total; ++n)
            {
                const uint32_t slot = pak + 0xCu + 4u * n;
                write32(slot, (n < count ? moved[n] : added[n - count]) - slot); // unsigned wrap-around = signed
            }
            write32(pak + 8u, total);
            return static_cast<int>(count);
        }

        uint32_t cameraSize(uint32_t entry, uint32_t) { return 8u + 0x24u * read32(entry + 4u); }
        uint32_t toNext(uint32_t entry, uint32_t next) { return next - entry; }

        std::vector<uint8_t> cameraBytes(const std::vector<CameraKey> &keys)
        {
            std::vector<uint8_t> out(8u + 0x24u * keys.size());
            const int32_t last = keys.empty() ? 0 : keys.back().frame;
            const uint32_t count = static_cast<uint32_t>(keys.size());
            std::memcpy(out.data(), &last, 4);
            std::memcpy(out.data() + 4, &count, 4);
            for (size_t k = 0; k < keys.size(); ++k)
            {
                uint8_t *p = out.data() + 8u + 0x24u * k;
                const float v[8] = {keys[k].eye[0], keys[k].eye[1], keys[k].eye[2], keys[k].at[0], keys[k].at[1],
                                    keys[k].at[2], keys[k].roll, keys[k].fov};
                std::memcpy(p, &keys[k].frame, 4);
                std::memcpy(p + 4, v, 32);
            }
            return out;
        }

        // an intro's four paths sit at f14 + {spot 0: 1, spot 1: 3, spot 2: 0, spot 3: 2} (the camera path player, FUN_00104278)
        constexpr uint32_t kCornerOffset[4] = {1, 3, 0, 2};

        void applyMoves()
        {
            // which top-level moves made it in whole
            std::vector<std::pair<std::string, Move *>> ok;
            for (auto &[mod, mv] : g_top)
            {
                uint32_t first = 0;
                if (clipsOk(*mv, first))
                    ok.push_back({mod, mv});
                else
                    std::cerr << "[moves] " << mod << ": " << mv->name << " skipped: not all of its animations could be added"
                              << std::endl;
            }
            if (ok.empty())
                return;

            // ---- cameras and eye tracks first (their numbers go into the playback records)
            std::set<const Move *> parts;
            std::vector<const Move *> todo;
            for (auto &[mod, mv] : ok)
                todo.push_back(mv);
            while (!todo.empty())
            {
                const Move *mv = todo.back();
                todo.pop_back();
                parts.insert(mv);
                if (mv->stage)
                    todo.push_back(mv->stage.get());
                if (mv->next)
                    todo.push_back(mv->next.get());
            }
            std::vector<std::vector<uint8_t>> cameras, eyes;
            std::map<const Animation *, uint32_t> cameraIndex, eyeIndex; // index into the lists above
            for (Flat &f : g_flat)
            {
                if (!parts.count(f.move))
                    continue;
                const Animation &a = *f.animation;
                if (!a.cameras.empty())
                {
                    cameraIndex[&a] = static_cast<uint32_t>(cameras.size());
                    if (a.cameras.size() == 4u)
                    {
                        // in path order: base + 0 = spot 2, + 1 = spot 0, + 2 = spot 3, + 3 = spot 1
                        for (uint32_t path = 0; path < 4u; ++path)
                            for (uint32_t spot = 0; spot < 4u; ++spot)
                                if (kCornerOffset[spot] == path)
                                    cameras.push_back(cameraBytes(a.cameras[spot]));
                    }
                    else
                        cameras.push_back(cameraBytes(a.cameras[0]));
                }
                if (!a.eyes.empty())
                {
                    eyeIndex[&a] = static_cast<uint32_t>(eyes.size());
                    eyes.push_back(a.eyes);
                }
            }
            int firstCamera = 0, firstEyes = 0;
            if (!cameras.empty())
            {
                firstCamera = growPak(kPak0, cameras, kMaxCamera, "camera paths", cameraSize);
                if (firstCamera < 0)
                    std::cerr << "[moves] the camera paths could not be added (more than 1,024, or no room): intros and "
                                 "victories from mods use no camera" << std::endl;
            }
            if (!eyes.empty())
            {
                firstEyes = growPak(kPak1, eyes, kMaxEyes - 1u, "eye tracks", toNext);
                if (firstEyes < 0)
                    std::cerr << "[moves] the eye tracks could not be added: animations from mods move no eyes" << std::endl;
            }

            // ---- move records
            const uint32_t stride = read32(kWaza + 4u);
            const uint32_t oldArray = read32(kWaza + kRecordArray);
            const uint32_t oldCount = read32(kWaza + 8u);
            uint32_t next = oldCount;
            for (auto &[mod, mv] : ok)
                numberRecords(*mv, next);
            if (next - 1u > kMaxRecord)
            {
                std::cerr << "[moves] " << next << " moves in all, but a moveset slot holds 12 bits (4,096): new moves skipped"
                          << std::endl;
                g_record.clear();
                return;
            }
            const uint32_t array = vendetta_store::allocate(vendetta_store::Area::Boot, next * stride, "move records", 16u, 0u);
            if (array == 0u)
            {
                g_record.clear();
                return;
            }
            std::memcpy(guest(array), guest(oldArray), oldCount * stride);
            const uint32_t timing = read32(kWaza + kTimingArray);
            const uint32_t timingStride = read32(kWaza + kTimingStride);
            static const bool log = std::getenv("VENDETTA_MOVELOG") && *std::getenv("VENDETTA_MOVELOG") != '0';
            for (const auto &[part, record] : g_record)
            {
                std::array<uint32_t, 31> m = part->record;
                m[28] = firstClip(*part);
                m[29] = part->stage ? g_record.at(part->stage.get()) : 0u;
                m[30] = part->next ? g_record.at(part->next.get()) : 0u;
                packBits(m, kMoveBits, guest(array + record * stride), stride);
                for (size_t k = 0; k < part->animations.size(); ++k)
                {
                    const Animation &a = part->animations[k];
                    std::array<uint32_t, 30> p = a.play;
                    if (cameraIndex.count(&a) && firstCamera >= 0)
                        p[14] = static_cast<uint32_t>(firstCamera) + cameraIndex.at(&a);
                    if (eyeIndex.count(&a) && firstEyes >= 0)
                        p[15] = static_cast<uint32_t>(firstEyes) + eyeIndex.at(&a) + 1u;
                    else if (eyeIndex.count(&a))
                        p[17] &= ~48u;
                    packBits(p, kPlayBits, guest(timing + (m[28] + k) * timingStride), timingStride);
                    if (log)
                    {
                        std::cerr << "[moves] " << part->name << " " << a.role << " clip " << m[28] + k << " playback:";
                        for (uint32_t v : p)
                            std::cerr << " " << v;
                        std::cerr << std::endl;
                    }
                }
                if (log)
                {
                    std::cerr << "[moves] " << part->name << " (" << part->type << ") record " << record << ":";
                    for (uint32_t v : m)
                        std::cerr << " " << v;
                    std::cerr << std::endl;
                }
            }
            write32(kWaza + kRecordArray, array);
            write32(kWaza + 8u, next);

            // ---- names: the NAME block rebuilt in the store with the new ones (FUN_0011c5d0: strings + offsets[n])
            const uint32_t nameCount = read32(kWaza + kNames + 4u);
            const uint32_t offsets = read32(kWaza + kNames + 8u);
            const uint32_t strings = read32(kWaza + kNames + 0xCu);
            std::vector<std::string> names(next);
            for (uint32_t n = 0; n < nameCount && n < next; ++n)
            {
                uint16_t off = 0;
                std::memcpy(&off, guest(offsets + 2u * n), 2);
                names[n] = guestString(strings + off);
            }
            for (const auto &[part, record] : g_record)
                names[record] = part->name.empty() ? part->folder.filename().string() : part->name;
            std::vector<uint8_t> text;
            std::vector<uint16_t> at(next);
            for (uint32_t n = 0; n < next; ++n)
            {
                at[n] = static_cast<uint16_t>(text.size());
                text.insert(text.end(), names[n].begin(), names[n].end());
                text.push_back(0);
            }
            if (text.size() > 0xFFFFu)
                std::cerr << "[moves] the move names are over 64 KB in all: the new names don't show" << std::endl;
            else
            {
                const uint32_t newOffsets = vendetta_store::allocate(vendetta_store::Area::Boot, 2u * next, "move names", 16u, 0u);
                const uint32_t newStrings =
                    vendetta_store::allocate(vendetta_store::Area::Boot, static_cast<uint32_t>(text.size()), "move names", 16u, 0u);
                if (newOffsets && newStrings)
                {
                    std::memcpy(guest(newOffsets), at.data(), 2u * next);
                    std::memcpy(guest(newStrings), text.data(), text.size());
                    write32(kWaza + kNames + 8u, newOffsets);
                    write32(kWaza + kNames + 0xCu, newStrings);
                    write32(kWaza + kNames + 4u, next);
                }
            }

            // VENDETTA_ANIM_POISON=1 (the animation loader's test switch): 0xFF over what the moves made unused - the old
            // move records, the old names, the camera paths' and eye tracks' old data under their rewritten tables
            if (const char *poison = std::getenv("VENDETTA_ANIM_POISON"); poison && *poison && *poison != '0')
            {
                uint32_t bytes = 0;
                auto fill = [&bytes](uint32_t from, uint32_t to)
                {
                    if (to > from)
                    {
                        std::memset(guest(from), 0xFF, to - from);
                        bytes += to - from;
                    }
                };
                fill(oldArray, oldArray + oldCount * stride);
                if (read32(kWaza + kNames + 8u) != offsets)
                {
                    uint32_t end = strings;
                    for (uint32_t n = 0; n < nameCount; ++n)
                    {
                        uint16_t off = 0;
                        std::memcpy(&off, guest(offsets + 2u * n), 2);
                        end = std::max<uint32_t>(end, strings + off + static_cast<uint32_t>(guestString(strings + off).size()) + 1u);
                    }
                    fill(offsets, offsets + 2u * nameCount);
                    fill(strings, end);
                }
                for (const auto &[pak, grown] : {std::make_pair(kPak0, !cameras.empty() && firstCamera >= 0),
                                                 std::make_pair(kPak1, !eyes.empty() && firstEyes >= 0)})
                    if (grown)
                        fill(pak + 0xCu + 4u * read32(pak + 8u), pak + read32(pak + 4u));
                std::cerr << "[moves] VENDETTA_ANIM_POISON: " << bytes << " bytes the new moves made unused filled with 0xFF"
                          << std::endl;
            }

            for (auto &[mod, mv] : ok)
            {
                std::cerr << "[moves] " << mod << ": " << mv->name << " (" << mv->type << (mv->kind.empty() ? "" : ", " + mv->kind)
                          << ") added as move " << g_record.at(mv) << ", clips " << firstClip(*mv) << "-"
                          << firstClip(*mv) + mv->animations.size() - 1u;
                if (mv->stage)
                    std::cerr << "; " << mv->stage->type << " move " << g_record.at(mv->stage.get()) << ", clips "
                              << firstClip(*mv->stage) << "-" << firstClip(*mv->stage) + mv->stage->animations.size() - 1u;
                if (mv->next)
                    std::cerr << "; " << mv->next->type << " move " << g_record.at(mv->next.get()) << ", clips "
                              << firstClip(*mv->next) << "-" << firstClip(*mv->next) + mv->next->animations.size() - 1u;
                std::cerr << std::endl;
            }
            std::cerr << "[moves] " << ok.size() << " new move(s) in, " << next - oldCount << " move record(s) (" << oldCount
                      << "-" << next - 1u << ")";
            if (!cameras.empty() && firstCamera >= 0)
                std::cerr << ", camera paths " << firstCamera << "-" << firstCamera + cameras.size() - 1u;
            if (!eyes.empty() && firstEyes >= 0)
                std::cerr << ", eye tracks " << firstEyes << "-" << firstEyes + eyes.size() - 1u;
            std::cerr << std::endl;
        }
    }

    void prepare(uint8_t *rdram)
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        g_rdram = rdram;
        buildTypes();
        buildEndings();
        moveCheck();
        readModMoves();
        for (auto &[mod, mv] : g_moves)
        {
            g_top.push_back({mod, mv.get()});
            flatten(*mv, mod);
        }
    }

    std::vector<ClipToAdd> clipsToAdd()
    {
        std::vector<ClipToAdd> out;
        for (const Flat &f : g_flat)
            out.push_back({(f.move->name.empty() ? f.move->folder.filename().string() : f.move->name) + ": " + f.animation->role,
                           f.mod, &f.animation->bytes, &f.animation->sounds});
        return out;
    }

    void clipNumbered(size_t which, uint32_t clipId)
    {
        if (which < g_flat.size())
            g_flat[which].clip = clipId;
    }

    void apply()
    {
        if (g_rdram && !g_top.empty())
            applyMoves();
    }

    int recordOf(const std::string &name)
    {
        auto upper = [](std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return s;
        };
        for (const auto &[mod, mv] : g_top)
            if (g_record.count(mv) &&
                (upper(mv->name) == upper(name) || upper(mv->folder.filename().string()) == upper(name)))
                return static_cast<int>(g_record.at(mv));
        return -1;
    }
}
