// Def Jam Vendetta (USA, SLUS-20639) — the mods\ folder (see vendetta_mods.h).
// Copied into ps2xRuntime/src/runner/ by tools/sync_generated.py; edit this file in the repo's overrides/ folder.
#include "vendetta_mods.h"

#include "ps2_runtime.h"
#include "runtime/ps2_host_settings.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace vendetta_mods
{
    const Json *Json::find(const std::string &key) const
    {
        for (const auto &[name, value] : members)
            if (name == key)
                return &value;
        return nullptr;
    }

    namespace
    {
        class JsonReader
        {
        public:
            explicit JsonReader(const std::string &text) : m_text(text) {}

            bool read(Json &out, std::string &error)
            {
                if (!value(out))
                {
                    error = m_error;
                    return false;
                }
                space();
                if (m_pos != m_text.size())
                {
                    error = fail("text after the end of the JSON");
                    return false;
                }
                return true;
            }

        private:
            std::string fail(const std::string &what)
            {
                const size_t line = 1u + static_cast<size_t>(std::count(m_text.begin(), m_text.begin() + static_cast<std::ptrdiff_t>(std::min(m_pos, m_text.size())), '\n'));
                m_error = what + " (line " + std::to_string(line) + ")";
                return m_error;
            }

            void space()
            {
                while (m_pos < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_pos])))
                    ++m_pos;
            }

            bool literal(const char *word)
            {
                const size_t length = std::char_traits<char>::length(word);
                if (m_text.compare(m_pos, length, word) != 0)
                    return false;
                m_pos += length;
                return true;
            }

            bool string(std::string &out)
            {
                ++m_pos; // the opening quote
                while (m_pos < m_text.size())
                {
                    const char c = m_text[m_pos++];
                    if (c == '"')
                        return true;
                    if (c != '\\')
                    {
                        out.push_back(c);
                        continue;
                    }
                    if (m_pos >= m_text.size())
                        break;
                    const char e = m_text[m_pos++];
                    switch (e)
                    {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u':
                    {
                        if (m_pos + 4u > m_text.size())
                        {
                            fail("a broken \\u escape");
                            return false;
                        }
                        const unsigned code = static_cast<unsigned>(std::strtoul(m_text.substr(m_pos, 4).c_str(), nullptr, 16));
                        m_pos += 4u;
                        // UTF-8 (surrogate pairs are not needed for names and paths)
                        if (code < 0x80u)
                            out.push_back(static_cast<char>(code));
                        else if (code < 0x800u)
                        {
                            out.push_back(static_cast<char>(0xC0u | (code >> 6u)));
                            out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
                        }
                        else
                        {
                            out.push_back(static_cast<char>(0xE0u | (code >> 12u)));
                            out.push_back(static_cast<char>(0x80u | ((code >> 6u) & 0x3Fu)));
                            out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
                        }
                        break;
                    }
                    default:
                        fail(std::string("an unknown escape \\") + e);
                        return false;
                    }
                }
                fail("a string with no closing quote");
                return false;
            }

            bool value(Json &out)
            {
                space();
                if (m_pos >= m_text.size())
                {
                    fail("the text ends where a value should be");
                    return false;
                }
                const char c = m_text[m_pos];
                if (c == '{')
                {
                    out.type = Json::Type::Object;
                    ++m_pos;
                    space();
                    if (m_pos < m_text.size() && m_text[m_pos] == '}')
                    {
                        ++m_pos;
                        return true;
                    }
                    for (;;)
                    {
                        space();
                        if (m_pos >= m_text.size() || m_text[m_pos] != '"')
                        {
                            fail("a name in quotes expected");
                            return false;
                        }
                        std::string key;
                        if (!string(key))
                            return false;
                        space();
                        if (m_pos >= m_text.size() || m_text[m_pos] != ':')
                        {
                            fail("':' expected after \"" + key + "\"");
                            return false;
                        }
                        ++m_pos;
                        Json member;
                        if (!value(member))
                            return false;
                        out.members.emplace_back(std::move(key), std::move(member));
                        space();
                        if (m_pos < m_text.size() && m_text[m_pos] == ',')
                        {
                            ++m_pos;
                            continue;
                        }
                        if (m_pos < m_text.size() && m_text[m_pos] == '}')
                        {
                            ++m_pos;
                            return true;
                        }
                        fail("',' or '}' expected");
                        return false;
                    }
                }
                if (c == '[')
                {
                    out.type = Json::Type::Array;
                    ++m_pos;
                    space();
                    if (m_pos < m_text.size() && m_text[m_pos] == ']')
                    {
                        ++m_pos;
                        return true;
                    }
                    for (;;)
                    {
                        Json item;
                        if (!value(item))
                            return false;
                        out.items.push_back(std::move(item));
                        space();
                        if (m_pos < m_text.size() && m_text[m_pos] == ',')
                        {
                            ++m_pos;
                            continue;
                        }
                        if (m_pos < m_text.size() && m_text[m_pos] == ']')
                        {
                            ++m_pos;
                            return true;
                        }
                        fail("',' or ']' expected");
                        return false;
                    }
                }
                if (c == '"')
                {
                    out.type = Json::Type::String;
                    return string(out.text);
                }
                if (literal("true") || literal("false"))
                {
                    out.type = Json::Type::Bool;
                    out.boolean = m_text[m_pos - 1] == 'e' && m_text[m_pos - 4] == 't';
                    return true;
                }
                if (literal("null"))
                    return true;
                if (c == '-' || std::isdigit(static_cast<unsigned char>(c)))
                {
                    const char *start = m_text.c_str() + m_pos;
                    char *end = nullptr;
                    out.type = Json::Type::Number;
                    out.number = std::strtod(start, &end);
                    m_pos += static_cast<size_t>(end - start);
                    return true;
                }
                fail(std::string("an unexpected '") + c + "'");
                return false;
            }

            const std::string &m_text;
            size_t m_pos = 0;
            std::string m_error;
        };

        bool modsOff()
        {
            const char *value = std::getenv("VENDETTA_MODS");
            return value && value[0] == '0';
        }

        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        std::map<std::string, std::string> g_namesById; // every readable mod in the folder, on or off

        // a mod without an "id": its folder's name in lower case, every run of other characters one '-'
        std::string idFrom(const std::string &folderName)
        {
            std::string id;
            for (const char c : lower(folderName))
            {
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '.')
                    id.push_back(c);
                else if (!id.empty() && id.back() != '-')
                    id.push_back('-');
            }
            while (!id.empty() && id.back() == '-')
                id.pop_back();
            return id;
        }

        std::vector<Mod> findMods()
        {
            std::vector<Mod> mods;
            startProblemLog();
            if (modsOff())
            {
                std::cerr << "[mods] off (VENDETTA_MODS=0)" << std::endl;
                return mods;
            }
            // VENDETTA_MODS_DIR=<folder>: every mod in another folder, no project (test runs); otherwise the mods
            // installed in the project's own mods\ that it has on, and the original game has none
            const char *dir = std::getenv("VENDETTA_MODS_DIR");
            const bool testFolder = dir && *dir;
            const Project *project = testFolder ? nullptr : currentProject();
            if (!testFolder && !project)
            {
                std::cerr << "[mods] the original game: no mods (a project plays them)" << std::endl;
                return mods;
            }
            const std::filesystem::path root = testFolder ? std::filesystem::path(dir) : project->folder / "mods";
            if (testFolder)
                std::cerr << "[mods] folder " << root.string() << " (VENDETTA_MODS_DIR)" << std::endl;
            std::error_code ec;
            if (!std::filesystem::is_directory(root, ec) && testFolder)
                return mods;

            // every folder with a readable mod.json
            std::vector<Mod> found;
            for (const auto &entry : std::filesystem::directory_iterator(root, ec))
            {
                if (!entry.is_directory())
                    continue;
                Mod mod;
                mod.folder = entry.path();
                mod.folderName = entry.path().filename().string();
                std::ifstream file(mod.folder / "mod.json", std::ios::binary);
                if (!file)
                {
                    if (testFolder)
                        problem() << "[mods] " << mod.folderName << ": no mod.json - skipped" << std::endl;
                    continue;
                }
                std::stringstream text;
                text << file.rdbuf();
                std::string error;
                if (!parseJson(text.str(), mod.manifest, error) || !mod.manifest.isObject())
                {
                    problem() << "[mods] " << mod.folderName << ": mod.json can't be read: "
                              << (error.empty() ? "it isn't a JSON object" : error) << " - skipped" << std::endl;
                    if (project && std::any_of(project->mods.begin(), project->mods.end(), [&](const std::string &on)
                                               { return lower(on) == lower(mod.folderName); }))
                        blocking(mod.folderName + "'s mod.json can't be read.");
                    continue;
                }
                const Json *name = mod.manifest.find("name");
                mod.name = name && name->isString() && !name->text.empty() ? name->text : mod.folderName;
                const Json *id = mod.manifest.find("id");
                mod.id = id && id->isString() && !id->text.empty() ? lower(id->text) : idFrom(mod.folderName);
                g_namesById.emplace(mod.id, mod.name);
                found.push_back(std::move(mod));
            }
            std::sort(found.begin(), found.end(), [](const Mod &a, const Mod &b)
                      { return lower(a.folderName) < lower(b.folderName); });

            std::vector<Mod> ordered;
            if (project)
            {
                // the mods the project has on, in its order (later wins); the others in its folder are off
                for (const std::string &want : project->mods)
                {
                    auto it = std::find_if(found.begin(), found.end(), [&](const Mod &m)
                                           { return lower(m.folderName) == lower(want) || lower(m.name) == lower(want); });
                    if (it == found.end())
                    {
                        problem() << "[mods] " << project->name << ": \"" << want
                                  << "\" is on but isn't in the project's mods folder - skipped" << std::endl;
                        blocking(want + " is on but isn't installed in this project.");
                        continue;
                    }
                    ordered.push_back(std::move(*it));
                    found.erase(it);
                }
            }
            else
            {
                // order.txt: the mods it names move to the end, in its order (later wins)
                std::ifstream order(root / "order.txt");
                std::vector<std::string> names;
                std::string line;
                while (std::getline(order, line))
                {
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                        line.pop_back();
                    if (!line.empty() && line[0] != '#')
                        names.push_back(lower(line));
                }
                std::vector<Mod> named;
                for (const std::string &want : names)
                {
                    auto it = std::find_if(found.begin(), found.end(), [&](const Mod &m)
                                           { return lower(m.folderName) == want || lower(m.name) == want; });
                    if (it == found.end())
                    {
                        problem() << "[mods] order.txt names \"" << want << "\", which isn't a mod in that folder" << std::endl;
                        continue;
                    }
                    named.push_back(std::move(*it));
                    found.erase(it);
                }
                ordered = std::move(found);
                for (Mod &m : named)
                    ordered.push_back(std::move(m));
            }

            // "needs": a mod whose needs are missing is skipped; a needed mod loads first
            auto provides = [](const Mod &m, const std::string &want)
            { return lower(m.id) == lower(want) || lower(m.folderName) == lower(want) || lower(m.name) == lower(want); };
            // (decided for every mod first, then moved: a need is looked for among all the mods on, earlier ones too)
            std::vector<char> keep(ordered.size(), 1);
            for (size_t i = 0; i < ordered.size(); ++i)
            {
                const Mod &mod = ordered[i];
                bool ok = true;
                if (const Json *needs = mod.manifest.find("needs"); needs && needs->isArray())
                    for (const Json &need : needs->items)
                    {
                        if (!need.isString())
                            continue;
                        const bool present = std::any_of(ordered.begin(), ordered.end(), [&](const Mod &m)
                                                         { return &m != &mod && provides(m, need.text); });
                        if (!present)
                        {
                            problem() << "[mods] " << mod.name << " needs \"" << need.text << "\", which "
                                      << (project ? "isn't on in the project" : "isn't in that folder") << " - skipped"
                                      << std::endl;
                            // by its name when it is in the folder (installed but off)
                            std::string needName = need.text;
                            for (const Mod &other : found)
                                if (provides(other, need.text))
                                    needName = other.name;
                            blocking(mod.name + " needs " + needName + ".");
                            ok = false;
                        }
                    }
                keep[i] = ok ? 1 : 0;
            }
            std::vector<Mod> usable;
            for (size_t i = 0; i < ordered.size(); ++i)
                if (keep[i])
                    usable.push_back(std::move(ordered[i]));
            // place each mod after everything it needs (stable otherwise; a cycle keeps the order it has)
            for (size_t pass = 0; pass < usable.size(); ++pass)
            {
                bool moved = false;
                for (size_t i = 0; i < usable.size() && !moved; ++i)
                {
                    const Json *needs = usable[i].manifest.find("needs");
                    if (!needs || !needs->isArray())
                        continue;
                    for (const Json &need : needs->items)
                    {
                        if (!need.isString())
                            continue;
                        for (size_t j = i + 1; j < usable.size(); ++j)
                            if (provides(usable[j], need.text))
                            {
                                Mod later = std::move(usable[j]);
                                usable.erase(usable.begin() + static_cast<std::ptrdiff_t>(j));
                                usable.insert(usable.begin() + static_cast<std::ptrdiff_t>(i), std::move(later));
                                moved = true;
                                break;
                            }
                        if (moved)
                            break;
                    }
                }
                if (!moved)
                    break;
            }
            std::set<std::string> ids;
            for (const Mod &mod : usable)
            {
                std::cerr << "[mods] loading " << mod.name << (mod.name != mod.folderName ? " (" + mod.folderName + ")" : "")
                          << ", id " << mod.id << std::endl;
                if (!ids.insert(mod.id).second)
                    problem() << "[mods] " << mod.name << ": its id \"" << mod.id
                              << "\" is another loaded mod's too - \"" << mod.id << "/...\" finds the first" << std::endl;
            }
            return usable;
        }
    }

    bool parseJson(const std::string &text, Json &out, std::string &error)
    {
        out = Json{};
        // a UTF-8 byte-order mark, as some editors write
        const std::string body = text.rfind("\xEF\xBB\xBF", 0) == 0 ? text.substr(3) : text;
        JsonReader reader(body);
        return reader.read(out, error);
    }

    namespace
    {
        // --project "<name>" / --project=<name> on the game's command line, else VENDETTA_PROJECT (settings.toml's
        // [play] project fills it); empty = the original game
        std::string askedProject()
        {
#if defined(_MSC_VER)
            for (int i = 1; i < __argc; ++i)
            {
                const std::string arg = __argv[i];
                if (arg == "--project" && i + 1 < __argc)
                    return __argv[i + 1];
                if (arg.rfind("--project=", 0) == 0)
                    return arg.substr(10);
            }
#endif
            const char *env = std::getenv("VENDETTA_PROJECT");
            return env ? env : "";
        }

        std::unique_ptr<Project> findProject()
        {
            const std::string name = askedProject();
            if (name.empty())
                return nullptr;
            if (name.find_first_of("\\/:") != std::string::npos || name == "." || name == "..")
            {
                problem() << "[projects] \"" << name << "\" isn't a project's name - playing the original game" << std::endl;
                return nullptr;
            }
            auto project = std::make_unique<Project>();
            project->name = name;
            project->folder = PS2Runtime::getIoPaths().elfDirectory.parent_path() / "projects" / name;
            std::ifstream file(project->folder / "project.json", std::ios::binary);
            Json manifest;
            std::string error;
            std::stringstream text;
            if (file)
                text << file.rdbuf();
            if (!file || !parseJson(text.str(), manifest, error) || !manifest.isObject())
            {
                problem() << "[projects] " << name << ": " << (project->folder / "project.json").string()
                          << (file ? " can't be read" + (error.empty() ? std::string() : ": " + error) : std::string(" is missing"))
                          << " - playing the original game" << std::endl;
                return nullptr;
            }
            if (const Json *mods = manifest.find("mods"))
            {
                if (!mods->isArray())
                    problem() << "[projects] " << name << ": \"mods\" should be a list of mod folders - no mods" << std::endl;
                else
                    for (const Json &m : mods->items)
                        if (m.isString() && !m.text.empty())
                            project->mods.push_back(m.text);
            }
            std::cerr << "[projects] playing " << name << ": " << project->mods.size() << " mod(s) on" << std::endl;
            return project;
        }
    }

    const Project *currentProject()
    {
        static const std::unique_ptr<Project> project = findProject();
        return project.get();
    }

    const std::vector<Mod> &loadedMods()
    {
        static const std::vector<Mod> mods = findMods();
        return mods;
    }

    namespace
    {
        struct Kept
        {
            std::vector<uint8_t> bytes;
            uintmax_t size = 0;
            std::filesystem::file_time_type time{};
        };
        std::mutex g_aheadLock;
        std::condition_variable g_aheadDone;
        bool g_aheadRunning = false;
        std::unordered_map<std::string, Kept> g_kept; // by the path, normalised

        std::string keyOf(const std::filesystem::path &path) { return path.lexically_normal().string(); }

        bool sizeAndTime(const std::filesystem::path &path, uintmax_t &size, std::filesystem::file_time_type &time)
        {
            std::error_code ec;
            size = std::filesystem::file_size(path, ec);
            if (ec)
                return false;
            time = std::filesystem::last_write_time(path, ec);
            return !ec;
        }

        bool readFrom(const std::filesystem::path &path, std::vector<uint8_t> &bytes)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return true;
        }

        void waitAhead(std::unique_lock<std::mutex> &lock) { g_aheadDone.wait(lock, [] { return !g_aheadRunning; }); }
    }

    void runAhead(std::function<void()> job)
    {
        {
            std::unique_lock<std::mutex> lock(g_aheadLock);
            waitAhead(lock);
            g_kept.clear();
            g_aheadRunning = true;
        }
        std::thread([job = std::move(job)]()
                    {
                        try
                        {
                            job();
                        }
                        catch (...)
                        {
                            // a guess only: whatever it didn't read, the match reads itself
                        }
                        {
                            std::lock_guard<std::mutex> lock(g_aheadLock);
                            g_aheadRunning = false;
                        }
                        g_aheadDone.notify_all(); })
            .detach();
    }

    bool readAhead(const std::filesystem::path &path, std::vector<uint8_t> &bytes)
    {
        Kept kept;
        if (!sizeAndTime(path, kept.size, kept.time) || !readFrom(path, bytes))
            return false;
        kept.bytes = bytes;
        std::lock_guard<std::mutex> lock(g_aheadLock);
        g_kept[keyOf(path)] = std::move(kept);
        return true;
    }

    void forEachAtOnce(size_t count, const std::function<void(size_t)> &work)
    {
        constexpr size_t kThreads = 16u;
        std::atomic<size_t> next{0};
        const auto run = [&]()
        {
            for (size_t i; (i = next.fetch_add(1)) < count;)
            {
                try
                {
                    work(i);
                }
                catch (...)
                {
                    // a file that can't be read is read (or reported) again by whoever needs it
                }
            }
        };
        std::vector<std::thread> threads;
        for (size_t t = 1; t < std::min(kThreads, count); ++t)
            threads.emplace_back(run);
        run();
        for (std::thread &thread : threads)
            thread.join();
    }

    void readAheadAll(const std::vector<std::filesystem::path> &paths)
    {
        forEachAtOnce(paths.size(), [&paths](size_t i)
        {
            std::vector<uint8_t> bytes;
            readAhead(paths[i], bytes);
        });
    }

    bool readWhole(const std::filesystem::path &path, std::vector<uint8_t> &bytes)
    {
        {
            std::unique_lock<std::mutex> lock(g_aheadLock);
            waitAhead(lock);
            const auto found = g_kept.find(keyOf(path));
            uintmax_t size = 0;
            std::filesystem::file_time_type time{};
            if (found != g_kept.end() && sizeAndTime(path, size, time) && size == found->second.size &&
                time == found->second.time)
            {
                bytes = found->second.bytes;
                return true;
            }
        }
        return readFrom(path, bytes);
    }

    void dropAhead()
    {
        std::unique_lock<std::mutex> lock(g_aheadLock);
        waitAhead(lock);
        g_kept.clear();
    }

    bool detailLog()
    {
        static const bool on = std::getenv("VENDETTA_MODLOG") && *std::getenv("VENDETTA_MODLOG") != '0';
        return on;
    }

    std::filesystem::path modFile(const Mod &mod, const std::string &relative, const char *what)
    {
        const std::filesystem::path path = (mod.folder / std::filesystem::path(relative)).lexically_normal();
        const std::string folder = mod.folder.lexically_normal().string();
        if (path.string().compare(0, folder.size(), folder) != 0)
        {
            problem() << "[mods] " << mod.name << ": " << what << " \"" << relative << "\" is outside the mod's folder - skipped"
                      << std::endl;
            return {};
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec))
        {
            problem() << "[mods] " << mod.name << ": " << what << " \"" << relative << "\" isn't in the mod's folder - skipped"
                      << std::endl;
            return {};
        }
        return path;
    }

    const Mod *splitId(const std::string &text, std::string &rest)
    {
        const size_t slash = text.find('/');
        if (slash == std::string::npos || slash == 0)
            return nullptr;
        const std::string id = lower(text.substr(0, slash));
        for (const Mod &mod : loadedMods())
            if (mod.id == id)
            {
                rest = text.substr(slash + 1);
                return &mod;
            }
        return nullptr;
    }

    std::string nameOfId(const std::string &id)
    {
        loadedMods();
        const auto found = g_namesById.find(lower(id));
        return found == g_namesById.end() ? id : found->second;
    }

    std::filesystem::path fileNamed(const std::filesystem::path &folder, const std::string &relative)
    {
        std::string rest;
        const Mod *other = splitId(relative, rest);
        if (!other)
            return (folder / std::filesystem::path(relative)).lexically_normal(); // the mod's own, as before ids
        const std::filesystem::path path = (other->folder / std::filesystem::path(rest)).lexically_normal();
        const std::string root = other->folder.lexically_normal().string();
        if (path.string().compare(0, root.size(), root) != 0)
            return {}; // another mod's file stays inside that mod
        return path;
    }

    namespace
    {
        std::mutex g_problemLock;
        std::ofstream g_problemFile;
        bool g_problemStarted = false;

        // mods.log: beside settings.toml, else in the current folder; VENDETTA_MODS_LOG=<file> / none
        std::filesystem::path problemLogPath()
        {
            if (const char *chosen = std::getenv("VENDETTA_MODS_LOG"); chosen && *chosen)
                return std::string(chosen) == "none" ? std::filesystem::path() : std::filesystem::path(chosen);
            const std::filesystem::path &settings = ps2_host_settings::get().file;
            std::error_code error;
            const std::filesystem::path folder = settings.empty() ? std::filesystem::current_path(error) : settings.parent_path();
            return error ? std::filesystem::path() : folder / "mods.log";
        }

        std::string clock(const char *format)
        {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            char text[32];
            std::strftime(text, sizeof(text), format, &local);
            return text;
        }

        void startProblemLogLocked()
        {
            if (g_problemStarted)
                return;
            g_problemStarted = true;
            const std::filesystem::path path = problemLogPath();
            if (path.empty())
                return;
            g_problemFile.open(path, std::ios::binary | std::ios::trunc);
            if (g_problemFile)
                g_problemFile << "Mod problems, game started " << clock("%Y-%m-%d %H:%M:%S")
                              << " (nothing below this line: no problems)\r\n" << std::flush;
        }
    }

    void startProblemLog()
    {
        std::lock_guard<std::mutex> hold(g_problemLock);
        startProblemLogLocked();
    }

    namespace
    {
        std::mutex g_blockingLock;
        std::vector<std::string> g_blocking;
    }

    void blocking(const std::string &text)
    {
        problem() << "[check] " << text << std::endl;
        std::lock_guard<std::mutex> hold(g_blockingLock);
        if (std::find(g_blocking.begin(), g_blocking.end(), text) == g_blocking.end())
            g_blocking.push_back(text);
    }

    std::vector<std::string> blockingProblems()
    {
        std::lock_guard<std::mutex> hold(g_blockingLock);
        return g_blocking;
    }

    Problem::~Problem()
    {
        const std::string text = m_text.str();
        std::cerr << text << std::endl;
        std::lock_guard<std::mutex> hold(g_problemLock);
        startProblemLogLocked();
        if (!g_problemFile)
            return;
        std::string line = text;
        if (!line.empty() && line[0] == '[')
            if (const size_t close = line.find("] "); close != std::string::npos)
                line = line.substr(close + 2);
        g_problemFile << clock("%H:%M:%S") << ' ' << line << "\r\n" << std::flush;
    }
}
