// Def Jam Vendetta (USA, SLUS-20639) - the check at start: a project whose mods have a problem doesn't start.
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
//
// The problems are the ones that would break the project's matches or leave something out: a mod on in project.json but
// not in its mods\ folder, a mod.json that can't be read, a need missing, two mods replacing the same attire, a move
// whose animation isn't there (vendetta_mods.cpp, vendetta_wrestlers.cpp and vendetta_moves.cpp mark those as they
// read), and, checked here, every moveset slot a mod sets naming something the project doesn't have and every moveset
// change for a wrestler the game doesn't have. With any of them the game shows them all and closes (the settings
// program shows the same list on the project, with the fixes). Test runs (VENDETTA_MODS_DIR) and
// VENDETTA_MODCHECK=log only write them to stderr and mods.log.
#include "vendetta_mods.h"
#include "vendetta_moves.h"
#include "vendetta_wrestlers.h"

#include "ps2_runtime.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace vendetta_modcheck
{
    namespace
    {
        using vendetta_mods::Json;

        std::string upper(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return s;
        }

        // "game move <number> <name>" (as vendetta_animations.cpp reads it)
        bool isGameMove(const std::string &value)
        {
            const std::string want = "GAME MOVE ";
            if (value.size() <= want.size() || upper(value.substr(0, want.size())) != want)
                return false;
            char *end = nullptr;
            const long n = std::strtol(value.c_str() + want.size(), &end, 10);
            return end != value.c_str() + want.size() && n >= 0 && n < 999;
        }

        // the mods' "new" animations a slot may name: their file's name, plain and as "<id>/<name>" (upper case)
        std::set<std::string> newAnimationNames()
        {
            std::set<std::string> names;
            for (const vendetta_mods::Mod &mod : vendetta_mods::loadedMods())
            {
                const Json *section = mod.manifest.find("new");
                const Json *list = section ? section->find("animations") : nullptr;
                if (!list || !list->isArray())
                    continue;
                for (const Json &item : list->items)
                    if (item.isString())
                    {
                        const std::string stem = std::filesystem::path(item.text).stem().string();
                        names.insert(upper(stem));
                        names.insert(upper(mod.id + "/" + stem));
                    }
            }
            return names;
        }

        // every slot of one moveset (a whole file, or its member `key`) names something the project has
        void checkSlots(const std::string &modName, const std::string &wrestler, const std::filesystem::path &file,
                        const std::string &key, const std::set<std::string> &animations)
        {
            std::vector<uint8_t> bytes;
            Json whole;
            std::string error;
            const Json *moveset = nullptr;
            if (!file.empty() && vendetta_mods::readWhole(file, bytes) &&
                vendetta_mods::parseJson(std::string(bytes.begin(), bytes.end()), whole, error) && whole.isObject())
                moveset = key.empty() ? &whole : whole.find(key);
            if (!moveset || !moveset->isObject())
            {
                vendetta_mods::blocking(wrestler + "'s moveset in " + modName + " can't be read.");
                return;
            }
            for (const auto &[slot, value] : moveset->members)
            {
                const Json *move = value.isObject() ? value.find("move") : &value;
                if (!move || !move->isString())
                    continue; // reported where the slots are read
                const std::string &name = move->text;
                if (upper(name) == "NONE" || isGameMove(name) || vendetta_moves::isMove(name) || animations.count(upper(name)))
                    continue;
                // the move's name without the id in front of it (players don't see ids)
                const size_t slash = name.find('/');
                const std::string shown = slash == std::string::npos ? name : name.substr(slash + 1);
                vendetta_mods::blocking(modName + " gives " + wrestler + " the move " + shown + ", which isn't in this project.");
            }
        }

        void check()
        {
            vendetta_moves::indexNames();
            const std::set<std::string> animations = newAnimationNames();
            // new wrestlers' (and replacements') own movesets
            for (const vendetta_wrestlers::SlotFile &own : vendetta_wrestlers::slotFiles())
                checkSlots(own.modName, own.wrestler, own.file, own.key, animations);
            // the mods' moveset changes for any wrestler
            for (const vendetta_mods::Mod &mod : vendetta_mods::loadedMods())
            {
                const Json *wrestlers = mod.manifest.find("wrestlers");
                if (!wrestlers || !wrestlers->isObject())
                    continue;
                for (const auto &[wrestler, fileValue] : wrestlers->members)
                {
                    if (vendetta_wrestlers::recordsNamed(wrestler).empty())
                    {
                        vendetta_mods::blocking(mod.name + " changes " + wrestler + "'s moves, but there's no wrestler called that.");
                        continue;
                    }
                    if (!fileValue.isString())
                        continue;
                    checkSlots(mod.name, wrestler, vendetta_mods::modFile(mod, fileValue.text, "wrestler"), std::string(), animations);
                }
            }
        }

#if defined(_WIN32)
        std::wstring wide(const std::string &text)
        {
            const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring out(static_cast<size_t>(std::max(n, 0)), L'\0');
            if (n > 0)
                MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n);
            return out;
        }

        // the settings program beside the game (a player's install), or one folder up
        std::filesystem::path settingsProgram()
        {
            wchar_t self[MAX_PATH];
            const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
            if (length == 0 || length >= MAX_PATH)
                return {};
            const std::filesystem::path here = std::filesystem::path(self).parent_path();
            for (const std::filesystem::path &folder : {here, here.parent_path()})
            {
                std::error_code ec;
                const std::filesystem::path program = folder / "Def Jam Vendetta Recompiled Settings.exe";
                if (std::filesystem::is_regular_file(program, ec))
                    return program;
            }
            return {};
        }
#endif

        // the problems shown, then the game closes (with the settings program opened if the player asks)
        [[noreturn]] void refuse(const std::string &project, const std::vector<std::string> &problems)
        {
            std::string text = "Its mods have " + std::to_string(problems.size()) + (problems.size() == 1 ? " problem:" : " problems:") + "\n";
            for (size_t i = 0; i < problems.size() && i < 12u; ++i)
                text += "\n• " + problems[i];
            if (problems.size() > 12u)
                text += "\n• and " + std::to_string(problems.size() - 12u) + " more (mods.log has them all)";
            std::cerr << "[check] " << project << " doesn't start: " << problems.size() << " problem(s)" << std::endl;
#if defined(_WIN32)
            const std::filesystem::path settings = settingsProgram();
            text += settings.empty() ? "\n\nOpen the settings app to fix them." : "\n\nOpen the settings app to fix them?";
            const int answer = MessageBoxW(nullptr, wide(text).c_str(), wide(project + " can't start").c_str(),
                                           (settings.empty() ? MB_OK : MB_YESNO) | MB_ICONWARNING | MB_SETFOREGROUND);
            if (answer == IDYES)
                ShellExecuteW(nullptr, L"open", settings.wstring().c_str(), nullptr, settings.parent_path().wstring().c_str(),
                              SW_SHOWNORMAL);
#else
            std::cerr << text << std::endl;
#endif
            std::exit(1);
        }
    }

    void install()
    {
        const char *testDir = std::getenv("VENDETTA_MODS_DIR");
        const vendetta_mods::Project *project = vendetta_mods::currentProject();
        vendetta_mods::loadedMods(); // marks the problems found while finding the mods
        check();
        const std::vector<std::string> problems = vendetta_mods::blockingProblems();
        if (problems.empty())
        {
            if (project)
                std::cerr << "[check] " << project->name << ": no problems" << std::endl;
            return;
        }
        const char *mode = std::getenv("VENDETTA_MODCHECK");
        if (!project || (testDir && *testDir) || (mode && std::string(mode) == "log"))
        {
            std::cerr << "[check] " << problems.size() << " problem(s) that would stop a project (only logged here)" << std::endl;
            return;
        }
        refuse(project->name, problems);
    }
}

void installVendettaModCheck(PS2Runtime &) { vendetta_modcheck::install(); }
