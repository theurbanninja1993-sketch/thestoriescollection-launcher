// The Stories Collection launcher - the front end for the ported PS2 games.
//
// Phase 1: the game menu. Two cards with the player's own box art, mouse and
// controller navigation, click / Cross to launch. The games and their paths come
// from launcher.ini next to the executable, so nothing is hard-coded and a game
// marked available=0 shows a COMING SOON notice instead of launching. Each card
// can also carry its own theme music from music/, played while it is highlighted.
//
// The menu is drawn directly with raylib (no ImGui yet); the builder wizard in
// phase 2 is where forms and widgets will actually earn their keep.
#include "raylib.h"

#include "builder.h"
#include "platform.h"

// Set by CMakeLists from VERSION; this is only for an editor or a build that
// forgot, so the string never silently goes missing.
#ifndef PS2X_LAUNCHER_VERSION
#define PS2X_LAUNCHER_VERSION "unknown"
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

// No <windows.h> here, deliberately: raylib declares Rectangle, CloseWindow and
// ShowCursor, and Windows declares all three differently. The platform bits that
// need it live in platform.cpp, which does not include raylib.

namespace
{
    // ---------------------------------------------------------------- paths

    // The folder the launcher is running from, so a portable layout works
    // wherever it is dropped: launcher.ini, art/ and games/ all resolve here.
    bool isAbsolute(const std::string &path)
    {
        if (path.empty())
            return false;
        if (path[0] == '/' || path[0] == '\\')
            return true;
        return path.size() > 1 && path[1] == ':'; // C:\...
    }

    std::string resolve(const std::string &directory, const std::string &path)
    {
        if (path.empty() || isAbsolute(path))
            return path;
        return directory + "/" + path;
    }

    std::string directoryOf(const std::string &path)
    {
        const size_t slash = path.find_last_of("/\\");
        return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
    }

    std::string trim(const std::string &text)
    {
        const size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return {};
        const size_t last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
    }

    // ---------------------------------------------------------------- games

    struct Game
    {
        std::string id;
        std::string title;
        std::string exe;   // the port's runner; relative paths resolve beside the launcher
        std::string data;  // disc image or extracted folder, passed to the game
        std::string art;   // box art png
        std::string artDisplay; // the same path as the config has it, for the card's hint
        std::string music; // theme audio, played while this card is highlighted
        bool musicNamed = false; // whether the config named the file, for warnings
        // The port's source, which the launcher fetches itself so a player only has
        // to download the launcher. Empty portUrl means nothing to fetch.
        std::string portUrl;
        std::string portDir;  // where it is kept
        std::string portRef;  // branch or tag
        std::string disc;     // an explicit disc/iso/folder, skipping the picker
        bool exeConfigured = false;  // named in the config, so never derived over
        bool dataConfigured = false;
        bool portConfigured = false;
        std::string portLocal;  // revision on disk ("" = not fetched)
        std::string portRemote; // revision the remote has ("" = not known)
        bool available = true;
        Texture2D artTexture{};
        bool artLoaded = false;
        Music theme{};           // the loaded theme, if there was one
        bool themeLoaded = false;
        float themeVolume = 0.0f; // eased 0..1 of the configured music volume
    };

    // Launcher-wide settings, from the [launcher] section.
    struct Settings
    {
        float musicVolume = 0.6f; // 0 turns menu music off entirely
        bool fullscreen = true;   // a front end is meant to sit on the screen
        int crossCompile = -1;    // -1 unset: under WSL that means yes, elsewhere no
        std::string toolchain;    // a specific llvm-mingw to cross-compile with
        // Where to ask about a newer launcher. The launcher cannot update itself,
        // so this is only ever a version to compare against and a page to open -
        // and it is a setting rather than a constant, so a fork checks its own
        // releases.
        std::string updateRepo = "https://github.com/theurbanninja1993-sketch/thestoriescollection-launcher.git";
    };

    // A deliberately small key=value reader: one "[game:<id>]" section per game,
    // in file order, so the menu order is whatever the file says.
    std::vector<Game> loadGames(const std::string &path, Settings &settings)
    {
        std::vector<Game> games;
        FILE *file = std::fopen(path.c_str(), "rb");
        if (file == nullptr)
        {
            std::fprintf(stderr, "[launcher] %s not found; using built-in defaults\n", path.c_str());
            return games;
        }

        char line[1024];
        Game *current = nullptr;
        bool inLauncher = false;
        while (std::fgets(line, sizeof line, file) != nullptr)
        {
            std::string text = trim(line);
            if (text.empty() || text[0] == '#' || text[0] == ';')
                continue;
            if (text.front() == '[' && text.back() == ']')
            {
                inLauncher = text == "[launcher]";
                if (text.rfind("[game:", 0) != 0)
                {
                    current = nullptr;
                    continue;
                }
                games.push_back(Game{});
                current = &games.back();
                current->id = text.substr(6, text.size() - 7);
                if (current->title.empty())
                    current->title = current->id;
                continue;
            }
            const size_t equals = text.find('=');
            if (equals == std::string::npos)
                continue;
            const std::string key = trim(text.substr(0, equals));
            const std::string value = trim(text.substr(equals + 1));
            if (inLauncher)
            {
                if (key == "music_volume")
                    settings.musicVolume = static_cast<float>(std::atof(value.c_str()));
                else if (key == "fullscreen")
                    settings.fullscreen = std::atoi(value.c_str()) != 0;
                else if (key == "windows")
                    settings.crossCompile = std::atoi(value.c_str()) != 0 ? 1 : 0;
                else if (key == "toolchain")
                    settings.toolchain = value;
                else if (key == "update_repo")
                    settings.updateRepo = value;
                continue;
            }
            if (current == nullptr)
                continue;
            if (key == "title")
                current->title = value;
            else if (key == "exe")
                current->exe = value;
            else if (key == "data")
                current->data = value;
            else if (key == "art")
                current->art = value;
            else if (key == "music")
                current->music = value;
            else if (key == "port")
                current->portUrl = value;
            else if (key == "port_dir")
                current->portDir = value;
            else if (key == "port_ref")
                current->portRef = value;
            else if (key == "disc")
                current->disc = value;
            else if (key == "available")
                current->available = std::atoi(value.c_str()) != 0;
        }
        std::fclose(file);
        return games;
    }

    // ---------------------------------------------------------------- music

    // A theme starts from its opening rather than resuming mid-track, so hovering
    // a card always plays the part that identifies it.
    void startTheme(Game &game)
    {
        if (!game.themeLoaded)
            return;
        StopMusicStream(game.theme); // also rewinds to the beginning
        PlayMusicStream(game.theme);
        SetMusicVolume(game.theme, game.themeVolume);
    }

    // The game brings its own music, so none of ours may play over it. Everything
    // is stopped first: the audio thread would otherwise keep feeding the buffer
    // while the launcher is blocked in the launch.
    void silenceThemes(std::vector<Game> &games)
    {
        for (Game &game : games)
        {
            if (!game.themeLoaded)
                continue;
            game.themeVolume = 0.0f;
            SetMusicVolume(game.theme, 0.0f);
            StopMusicStream(game.theme);
        }
    }

    // ------------------------------------------------------------ port source
    // The port's source is fetched by the launcher rather than cloned by hand, so a
    // player downloads one repository instead of two. It is source, plainly: what
    // makes the game is their own disc, and it is built on their own machine.

    const char *platformFolder()
    {
#if defined(PS2X_LAUNCHER_WINDOWS)
        return "windows";
#else
        return "linux";
#endif
    }

    const char *runnerName()
    {
#if defined(PS2X_LAUNCHER_WINDOWS)
        return "ps2EntryRunner.exe";
#else
        return "ps2EntryRunner";
#endif
    }

    const char *launcherExeName()
    {
#if defined(PS2X_LAUNCHER_WINDOWS)
        return "ps2x_launcher.exe";
#else
        return "ps2x_launcher";
#endif
    }

    // Runs a command, keeping the last line it printed: that is what the fetch
    // screen shows while it works. Output is captured rather than inherited so the
    // screen can report it, and stderr is folded in because git prints progress
    // and errors there.
    int runCaptured(const std::string &command, std::mutex &mutex, std::string &lastLine)
    {
#if defined(PS2X_LAUNCHER_WINDOWS)
        FILE *pipe = _popen(command.c_str(), "r");
#else
        FILE *pipe = popen(command.c_str(), "r");
#endif
        if (pipe == nullptr)
            return -1;
        char line[512];
        while (std::fgets(line, sizeof line, pipe) != nullptr)
        {
            const std::string text = trim(line);
            if (!text.empty())
            {
                std::lock_guard<std::mutex> lock(mutex);
                lastLine = text;
            }
        }
#if defined(PS2X_LAUNCHER_WINDOWS)
        return _pclose(pipe);
#else
        const int status = pclose(pipe);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    }

    // Runs a command and keeps every line it printed. runCaptured keeps only the
    // last, which is what a progress screen wants; a tag listing needs them all.
    int runCapturedAll(const std::string &command, std::vector<std::string> &lines)
    {
        lines.clear();
#if defined(PS2X_LAUNCHER_WINDOWS)
        FILE *pipe = _popen(command.c_str(), "r");
#else
        FILE *pipe = popen(command.c_str(), "r");
#endif
        if (pipe == nullptr)
            return -1;
        char line[512];
        while (std::fgets(line, sizeof line, pipe) != nullptr)
        {
            const std::string text = trim(line);
            if (!text.empty())
                lines.push_back(text);
        }
#if defined(PS2X_LAUNCHER_WINDOWS)
        return _pclose(pipe);
#else
        const int status = pclose(pipe);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    }

    // A version as numbers, so 0.1.10 is newer than 0.1.9 - which comparing the
    // strings gets wrong, and which will matter the first time there is a tenth.
    std::vector<int> versionNumbers(const std::string &text)
    {
        std::vector<int> numbers;
        size_t at = text.find_first_of("0123456789");
        while (at != std::string::npos)
        {
            const size_t end = text.find_first_not_of("0123456789", at);
            numbers.push_back(std::atoi(text.substr(at, end - at).c_str()));
            at = end == std::string::npos ? std::string::npos : text.find_first_of("0123456789", end);
        }
        return numbers;
    }

    bool versionIsNewer(const std::string &candidate, const std::string &current)
    {
        const std::vector<int> a = versionNumbers(candidate), b = versionNumbers(current);
        for (size_t i = 0; i < std::max(a.size(), b.size()); ++i)
        {
            const int left = i < a.size() ? a[i] : 0;
            const int right = i < b.size() ? b[i] : 0;
            if (left != right)
                return left > right;
        }
        return false;
    }

    // The highest "v<number>" tag a repository has, or "" if it has none. An
    // annotated tag appears twice, once as the tag and once peeled; the peeled
    // line is skipped because it is the same release.
    std::string highestVersionTag(const std::vector<std::string> &lines)
    {
        std::string best;
        for (const std::string &line : lines)
        {
            const size_t at = line.find("refs/tags/");
            if (at == std::string::npos)
                continue;
            std::string name = line.substr(at + 10);
            if (name.size() >= 3 && name.compare(name.size() - 3, 3, "^{}") == 0)
                continue;
            if (name.size() < 2 || name[0] != 'v' || !std::isdigit(static_cast<unsigned char>(name[1])))
                continue;
            name.erase(0, 1);
            if (best.empty() || versionIsNewer(name, best))
                best = name;
        }
        return best;
    }

    // What the launcher's own repository has released. Asked over git rather than
    // the GitHub API: git is already required to fetch a game's source, so this
    // needs nothing new, and it is one round trip rather than a rate-limited API.
    // "https://github.com/owner/repo.git" -> "owner/repo", which is what the API
    // and archive URLs are built from.
    std::string repositoryPath(const std::string &url)
    {
        std::string text = url;
        if (text.size() > 4 && text.compare(text.size() - 4, 4, ".git") == 0)
            text.resize(text.size() - 4);
        const size_t scheme = text.find("://");
        const size_t host = scheme == std::string::npos ? 0 : text.find('/', scheme + 3);
        return host == std::string::npos ? std::string{} : text.substr(host + 1);
    }

    // The fetched ref's revision, kept beside the source rather than inside it: the
    // source is wiped and replaced on every fetch, and a player who has only ever
    // downloaded a tarball has no git to ask.
    std::string revisionPath(const Game &game) { return game.portDir + ".revision"; }

    // The value of the first "key": "value" in a blob of JSON. Enough for the two
    // answers the launcher needs - a commit's sha and a release's tag - without
    // pulling in a JSON library to read them.
    std::string jsonString(const std::string &text, const std::string &key)
    {
        const size_t at = text.find("\"" + key + "\"");
        if (at == std::string::npos)
            return {};
        const size_t colon = text.find(':', at);
        if (colon == std::string::npos)
            return {};
        const size_t open = text.find('"', colon + 1);
        if (open == std::string::npos)
            return {};
        const size_t close = text.find('"', open + 1);
        if (close == std::string::npos)
            return {};
        return text.substr(open + 1, close - open - 1);
    }

    // The newest release's version: "v0.1.5" in the API's answer becomes "0.1.5".
    std::string newestReleaseVersion(const std::vector<std::string> &lines)
    {
        for (const std::string &line : lines)
        {
            const std::string tag = jsonString(line, "tag_name");
            if (tag.empty())
                continue;
            return tag.size() > 1 && tag[0] == 'v' ? tag.substr(1) : tag;
        }
        return {};
    }

    std::string launcherUpdateCommand(const Settings &settings)
    {
        const std::string repository = repositoryPath(settings.updateRepo);
        if (repository.empty())
            return {};
        return "curl -sL \"https://api.github.com/repos/" + repository + "/releases/latest\"";
    }

    // The page to send someone to when there is a newer release.
    std::string releasePageUrl(const std::string &repository)
    {
        std::string url = repository;
        if (url.size() > 4 && url.compare(url.size() - 4, 4, ".git") == 0)
            url.resize(url.size() - 4);
        return url + "/releases/latest";
    }

    // ------------------------------------------------------- the launcher's own update
    // The launcher can fetch a new version of itself and put it in place, which is a
    // better end to the flow than handing the player a web page and hoping. What it
    // cannot do is replace its own files while it is running - on Windows they are
    // locked, and on either platform the code that would run afterwards is not the
    // code being replaced. So the work is split: the download and the unpack happen
    // now, into a staging folder that has nothing to do with the install, and a small
    // script does the swap once the launcher has exited.

    const char *launcherArchiveExtension()
    {
#if defined(PS2X_LAUNCHER_WINDOWS)
        // The zip, because PowerShell opens one on any Windows that can run this at
        // all - and because the fetch just taught us what a command that only exists
        // on the other platform does to a player's evening.
        return ".zip";
#else
        return ".tar.gz";
#endif
    }

    // The release asset's name, which tools/make_release.sh decides and which is
    // therefore known rather than looked up. No API call, no JSON to parse, and no
    // chance of picking the other platform's file.
    std::string launcherAssetName(const std::string &repository, const std::string &version)
    {
        const std::string path = repositoryPath(repository);
        const size_t slash = path.find('/');
        const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        return name + "-" + version + "-" + platformFolder() + launcherArchiveExtension();
    }

    std::string launcherAssetUrl(const std::string &repository, const std::string &version)
    {
        return "https://github.com/" + repositoryPath(repository) + "/releases/download/v" + version + "/" +
               launcherAssetName(repository, version);
    }

    // Where a downloaded launcher waits to be put in place, and where the script that
    // puts it there is written. Both live in the system's temp folder rather than
    // beside the launcher: a download that fails half way must not leave anything
    // half-written in the one folder that has to keep working.
    std::string updateStagingDirectory()
    {
        std::error_code ec;
        const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
        const std::filesystem::path root = ec ? std::filesystem::path(".") : temp;
        return (root / "thestoriescollection-launcher-update").string();
    }

    std::string updateScriptPath()
    {
        return updateStagingDirectory() +
#if defined(PS2X_LAUNCHER_WINDOWS)
               ".cmd";
#else
               ".sh";
#endif
    }

    // A path as a shell's single-quoted string, for the script below. The POSIX one
    // is the awkward one; cmd has no quoting problem worth solving, because the paths
    // in it are set with `set "X=..."` and used as `"%X%"`.
    std::string quotedForScript(const std::string &text)
    {
        std::string out = "'";
        for (char c : text)
        {
            if (c == '\'')
                out += "'\\''";
            else
                out += c;
        }
        out += "'";
        return out;
    }

    // The script that does the swap. It waits for the launcher to exit, keeps the old
    // executable rather than deleting it - if the new one turns out to be broken,
    // renaming it back is the way out - puts the staged files in place, starts the
    // launcher again, and deletes itself.
    std::string updaterScript(const std::string &install, const std::string &staging, const std::string &exeName)
    {
        std::ostringstream script;
#if defined(PS2X_LAUNCHER_WINDOWS)
        // CRLF, because this is a batch file: cmd is written for its own line ending.
        script << "@echo off\r\n";
        script << "rem Written by The Stories Collection launcher, to put a new version of\r\n";
        script << "rem itself in place after it has exited.\r\n";
        script << "set \"HERE=" << install << "\"\r\n";
        script << "set \"STAGE=" << staging << "\"\r\n";
        script << "if not exist \"%HERE%\\" << exeName << "\" goto copy\r\n";
        script << ":wait\r\n";
        script << "rem A running executable cannot be renamed, so this failing means it is\r\n";
        script << "rem still up. Nothing else can tell us as reliably.\r\n";
        script << "move /y \"%HERE%\\" << exeName << "\" \"%HERE%\\" << exeName << ".old\" >nul 2>&1\r\n";
        script << "if errorlevel 1 (\r\n";
        script << "    timeout /t 1 /nobreak >nul\r\n";
        script << "    goto wait\r\n";
        script << ")\r\n";
        script << ":copy\r\n";
        script << "xcopy /e /y /i /q \"%STAGE%\\*\" \"%HERE%\\\" >nul 2>&1\r\n";
        script << "rmdir /s /q \"%STAGE%\" >nul 2>&1\r\n";
        script << "del /q \"%STAGE%.zip\" >nul 2>&1\r\n";
        script << "start \"\" \"%HERE%\\" << exeName << "\"\r\n";
        script << "rem A batch file that deletes itself mid-run can lose the lines after\r\n";
        script << "rem it, so another process does the deleting.\r\n";
        script << "start \"\" /min cmd /c del \"%~f0\"\r\n";
#else
        script << "#!/bin/sh\n";
        script << "# Written by The Stories Collection launcher, to put a new version of\n";
        script << "# itself in place after it has exited.\n";
        script << "HERE=" << quotedForScript(install) << "\n";
        script << "STAGE=" << quotedForScript(staging) << "\n";
        script << "sleep 1\n";
        script << "if [ -f \"$HERE/" << exeName << "\" ]; then\n";
        script << "    mv -f \"$HERE/" << exeName << "\" \"$HERE/" << exeName << ".old\" 2>/dev/null\n";
        script << "fi\n";
        script << "cp -a \"$STAGE\"/. \"$HERE\"/ 2>/dev/null\n";
        script << "rm -rf \"$STAGE\"\n";
        script << "rm -f \"$STAGE.tar.gz\"\n";
        script << "chmod +x \"$HERE/" << exeName << "\" 2>/dev/null\n";
        script << "\"$HERE/" << exeName << "\" >/dev/null 2>&1 &\n";
        script << "rm -f \"$0\"\n";
#endif
        return script.str();
    }

    // Puts the unpacked release where the swap can find it: the archive holds one
    // folder named after the release, the player's own launcher.ini is kept rather
    // than replaced by the one that ships, the executable has to actually be there,
    // and the script that does the swap is written. Returns "" when it is ready, or
    // why it could not be.
    std::string stageLauncherUpdate(const std::string &install, const std::string &staging,
                                    const std::string &exeName, const std::string &script)
    {
        std::error_code ec;
        std::filesystem::path folder;
        int folders = 0;
        for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(staging, ec))
        {
            if (!entry.is_directory())
                continue;
            folder = entry.path();
            ++folders;
        }
        if (ec)
            return "the archive did not unpack (" + ec.message() + ")";
        if (folders != 1)
            return "the archive did not unpack into one folder";
        for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(folder, ec))
        {
            std::filesystem::rename(entry.path(), std::filesystem::path(staging) / entry.path().filename(), ec);
            if (ec)
                return "the unpacked files could not be moved into place (" + ec.message() + ")";
        }
        std::filesystem::remove_all(folder, ec);

        // The one file in there that belongs to the player and not to the release.
        const std::string theirs = install + "/launcher.ini";
        if (std::filesystem::exists(theirs, ec))
        {
            std::filesystem::copy_file(theirs, staging + "/launcher.ini",
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
                return "the launcher's own settings could not be kept (" + ec.message() + ")";
        }
        if (!std::filesystem::exists(staging + "/" + exeName, ec))
            return std::string("the archive did not contain ") + exeName;

        std::ofstream out(script, std::ios::binary | std::ios::trunc);
        if (!out)
            return "the update script could not be written";
        out << updaterScript(install, staging, exeName);
        out.close();
        // A script that cannot be run is not a script. Windows has no such bit.
        std::filesystem::permissions(script, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, ec);
        return {};
    }

    // Getting the new launcher here: the asset for this platform, unpacked into the
    // staging folder. Nothing in this touches the launcher itself.
    std::string launcherInstallCommand(const std::string &url, const std::string &archive,
                                       const std::string &staging)
    {
        std::string command;
        command += "echo downloading the new launcher && ";
        command += "curl -fL --retry 3 -o \"" + archive + "\" \"" + url + "\" && ";
        command += "echo unpacking && ";
#if defined(PS2X_LAUNCHER_WINDOWS)
        // PowerShell is on every Windows that can run this, and it opens a zip with
        // nothing installed - no tar, no 7-Zip, nothing to go wrong.
        command += "powershell -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '" +
                   archive + "' -DestinationPath '" + staging + "' -Force\"";
#else
        command += "tar -xzf \"" + archive + "\" -C \"" + staging + "\"";
#endif
        command += " && echo done";
        return command;
    }

    // The whole of the update's shell work: fetch the asset for this platform and
    // unpack it into the staging folder. Shared by the button and by --self-update,
    // so there is one place to be right rather than two that can drift apart.
    std::string launcherUpdateShellCommand(const Settings &settings, const std::string &version)
    {
        const std::string staging = updateStagingDirectory();
        const std::string archive = staging + launcherArchiveExtension();
        return platform::captureEverything(
            platform::removeTreeCommand(staging) + platform::commandSeparator() +
            platform::makeDirectoryCommand(staging) + platform::commandSeparator() +
            launcherInstallCommand(launcherAssetUrl(settings.updateRepo, version), archive, staging));
    }

    // The revision of a fetched tree, or "" when there is not one to read.
    // The revision of a fetched tree, or "" when there is not one to read.
    std::string readRevision(const std::string &directory)
    {
        if (directory.empty())
            return {};
        // What the fetch wrote: the API's answer for the ref it downloaded. Read
        // here rather than through the launcher's readFile, which is declared
        // further down the file.
        std::string stored;
        {
            std::ifstream in(directory + ".revision", std::ios::binary);
            std::ostringstream text;
            text << in.rdbuf();
            stored = text.str();
        }
        const std::string sha = jsonString(stored, "sha");
        if (sha.size() >= 7)
            return sha;
        // A developer's own clone has no such file, and may have git to ask.
        if (platform::commandExists("git"))
        {
            std::mutex discard;
            std::string line;
            const std::string command = "git -C \"" + directory + "\" rev-parse HEAD 2>&1";
            if (runCaptured(command, discard, line) == 0)
                return line;
        }
        return {};
    }

    std::string portRefOf(const Game &game) { return game.portRef.empty() ? "main" : game.portRef; }

    // A source folder is safe to throw away only if it looks like one. `port_dir`
    // comes from the config, so it is checked rather than trusted: nothing here
    // should be able to delete a whole home directory because of a typo in a config.
    bool safeToWipe(const std::string &dir)
    {
        return dir.size() > 1 && dir != "/" && dir != "." && dir != ".." && dir.find("..") == std::string::npos;
    }

    // A shallow clone the first time. After that, take the ref again and reset onto
    // it: the source is not the player's to edit, and this is what makes an update
    // deterministic rather than a merge that can fail next to their changes.
    //
    // Both halves have to be able to recover from what an interrupted one leaves
    // behind. A clone that dies partway leaves a non-empty folder that `git clone`
    // then refuses, so a player could be left with a card that will not build and a
    // fetch that cannot fix it - and the only way out was to find and delete the
    // folder by hand. So: clear whatever is in the way first, and if an existing
    // checkout can no longer be updated, start it again from scratch.
    // Fetching is a download, not a clone: curl and tar ship with Windows 10 and
    // with every desktop Linux, and neither needs installing, an account, or git.
    // Git for Windows is frequently absent - its installer offers "use Git from
    // Git Bash only", and a Windows player may only ever have it inside WSL - and
    // requiring it to *download* a tarball was never reasonable.
    //
    // The revision is written next to the source rather than inside it, because
    // the source is wiped and replaced every time: it is the ref's commit, asked
    // of the API, so the card can say whether the download is the newest one.
    // Fetching is a download, not a clone: curl and tar ship with Windows 10 and
    // with every desktop Linux, and neither needs installing, an account, or git.
    //
    // Everything runs from the port's *parent* folder with relative names, which
    // is not tidiness: a Windows path passed to tar is `C:/...`, and the `C:` is
    // read as a remote host by the MSYS tar that a Git for Windows install puts on
    // the PATH, so the unpack fails on a path that looks perfectly ordinary.
    //
    // The revision is written beside the source rather than inside it, because the
    // source is replaced wholesale on every fetch: it is the ref's commit, asked
    // of the API, so the card can say whether the download is the newest one.
    std::string fetchCommand(const Game &game)
    {
        const std::string repository = repositoryPath(game.portUrl);
        if (repository.empty())
            return "echo \"the port's address in launcher.ini is not a github repository\"";
        const std::string ref = portRefOf(game);
        const std::string archive = "https://github.com/" + repository + "/archive/" + ref + ".tar.gz";
        const std::string revision = "https://api.github.com/repos/" + repository + "/commits/" + ref;

        const std::string parent = std::filesystem::path(game.portDir).parent_path().string();
        const std::string folder = std::filesystem::path(game.portDir).filename().string();
        const std::string tarball = folder + ".tar.gz";
        const std::string revisionFile = revisionPath(game);

        const std::string join = platform::commandSeparator();
        std::string command;
        command += "echo fetching " + repository + " " + ref + " && ";
        // The parent has to exist before the shell moves into it: on a first fetch it
        // does not, and a `cd` into a folder that is not there carries on regardless -
        // leaving every relative name after it pointing somewhere else. That is how a
        // download landed beside the port instead of in it, and how the unpack then
        // failed on a folder it had just been told to create.
        if (!parent.empty())
            command += platform::makeDirectoryCommand(parent) + join;
        command += platform::changeDirectoryCommand(parent) + join;
        if (safeToWipe(game.portDir))
            command += platform::removeTreeCommand(game.portDir) + join;
        command += platform::makeDirectoryCommand(game.portDir) + join;
        command += "echo downloading, about 5 MB && ";
        command += "curl -fL --retry 3 -o \"" + tarball + "\" \"" + archive + "\" && ";
        command += "echo unpacking && ";
        command += "tar -xzf \"" + tarball + "\" -C \"" + folder + "\" --strip-components=1 && ";
        command += platform::removeFileCommand(tarball) + " && ";
        command += "echo asking GitHub which revision that was && ";
        command += "curl -sL -o \"" + revisionFile + "\" \"" + revision + "\" && ";
        command += "echo done";
        return platform::captureEverything(command);
    }

    // What the remote has for the ref, without touching the working tree.
    std::string checkCommand(const Game &game)
    {
        const std::string repository = repositoryPath(game.portUrl);
        if (repository.empty())
            return {};
        return "curl -sL \"https://api.github.com/repos/" + repository + "/commits/" + portRefOf(game) + "\"";
    }

    // Where a built game ended up, so nobody has to write these into the config.
    // A portable folder (dist/, which build.sh makes) carries its own copy of the
    // game files and the runner finds them beside itself. Otherwise the wizard's own
    // build tree has both. An explicit exe= in the config still wins over either.
    void derivePaths(Game &game)
    {
        if (!game.portConfigured || !game.exe.empty())
            return;
        const std::string dist = game.portDir + "/dist/" + platformFolder();
        const std::string distRunner = dist + "/" + runnerName();
        if (FileExists(distRunner.c_str()))
        {
            game.exe = distRunner;
            return; // the game files are in that folder too, and found by name
        }
        game.exe = builder::builtRunner(game.portDir);
#if defined(PS2X_LAUNCHER_WINDOWS)
        game.exe += ".exe";
#endif
        if (game.data.empty())
        {
            const builder::Recipe recipe = builder::readRecipe(game.portDir);
            if (recipe.ok)
                game.data = builder::builtGameDir(game.portDir) + "/" + recipe.bootFile;
        }
    }

    bool gameIsBuilt(const Game &game) { return !game.exe.empty() && FileExists(game.exe.c_str()); }

    std::string shortRevision(const std::string &revision)
    {
        return revision.size() > 7 ? revision.substr(0, 7) : revision;
    }

    // What the four face buttons are called on the pad that is connected. raylib
    // reports positions rather than names, and the three families disagree about
    // them: the bottom button is Cross on a PlayStation pad, A on an Xbox one, and B
    // on a Switch one. Anything unrecognised is described the PlayStation way, which
    // is what the rest of the launcher's wording assumes.
    struct PadNames
    {
        std::string bottom, right, left, top;
        std::string select; // the small one beside the d-pad: Select, View, Minus
        std::string mute;   // clicking the right stick in
        std::string bumper; // the upper shoulder button: L1, LB, L
    };

    PadNames padNamesFor(int slot)
    {
        const char *raw = IsGamepadAvailable(slot) ? GetGamepadName(slot) : nullptr;
        std::string name = raw != nullptr ? raw : "";
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name.find("xbox") != std::string::npos || name.find("xinput") != std::string::npos ||
            name.find("steam") != std::string::npos)
            return {"A", "B", "X", "Y", "View", "R stick", "LB"};
        if (name.find("switch") != std::string::npos || name.find("nintendo") != std::string::npos)
            return {"B", "A", "Y", "X", "Minus", "R stick", "L"}; // Nintendo numbers them the other way round
        return {"Cross", "Circle", "Square", "Triangle", "Select", "R3", "L1"};
    }

    PadNames padNames() { return padNamesFor(0); }

    // The one-line summary drawn under a card, or "" when there is nothing to say.
    // `fetch` and `build` are what to tell the player to press, which depends on
    // whether they are holding a pad or sitting at a keyboard.
    std::string portSummary(const Game &game, const std::string &fetch, const std::string &build)
    {
        if (!game.portConfigured)
            return {};
        if (game.portLocal.empty())
            return "source not fetched - press " + fetch;
        if (!game.portRemote.empty() && game.portRemote != game.portLocal)
            return "source update available - press " + fetch;
        if (!gameIsBuilt(game))
            return "not built - press " + build;
        return "ready";
    }

    // ---------------------------------------------------------------- launch

    // The shell handles the quoting on both platforms (cmd and sh both accept &&
    // and double quotes), and the game is run from its own folder so a portable
    // config next to it is the one it reads.
    //
    // PS2X_LAUNCHED_FROM_LAUNCHER is what lets the game's settings say "Return to
    // game select" instead of "Quit to desktop" - which is what actually happens.
    std::string launchCommand(const Game &game)
    {
#if defined(PS2X_LAUNCHER_WINDOWS)
        std::string command =
            "set PS2X_LAUNCHED_FROM_LAUNCHER=1 && cd /d \"" + directoryOf(game.exe) + "\"";
#else
        std::string command = "export PS2X_LAUNCHED_FROM_LAUNCHER=1; cd \"" + directoryOf(game.exe) + "\"";
#endif
        command += " && \"" + game.exe + "\"";
        if (!game.data.empty())
            command += " \"" + game.data + "\"";
        return command;
    }

    // ---------------------------------------------------------------- layout

    struct Card
    {
        Rectangle bounds{}; // the box art area
        float scale = 1.0f;
        float highlight = 0.0f; // eased 0..1
    };

    constexpr float kArtWidth = 300.0f;
    constexpr float kArtHeight = 420.0f; // a PS2 cover is roughly 1:1.4
    constexpr float kGap = 90.0f;
    constexpr float kMusicFadeSpeed = 3.0f; // how fast a theme swells in or out

    bool pointIn(const Rectangle &rect, Vector2 point)
    {
        return CheckCollisionPointRec(point, rect);
    }

    // Greedy word wrap so a long title sits under its card instead of running
    // into its neighbour.
    std::vector<std::string> wrapText(const std::string &text, int fontSize, float maxWidth)
    {
        std::vector<std::string> lines;
        std::string line;
        size_t start = 0;
        while (start <= text.size())
        {
            const size_t space = text.find(' ', start);
            const std::string word = text.substr(start, space == std::string::npos ? std::string::npos : space - start);
            const std::string candidate = line.empty() ? word : line + " " + word;
            if (!line.empty() && static_cast<float>(MeasureText(candidate.c_str(), fontSize)) > maxWidth)
            {
                lines.push_back(line);
                line = word;
            }
            else
            {
                line = candidate;
            }
            if (space == std::string::npos)
                break;
            start = space + 1;
        }
        if (!line.empty())
            lines.push_back(line);
        return lines;
    }

    // The card's own copy, centered on x, one line per entry.
    void drawCenteredLines(const std::vector<std::string> &lines, float centerX, float top, int fontSize, Color color,
                           float lineHeight)
    {
        float y = top;
        for (const std::string &line : lines)
        {
            const float width = static_cast<float>(MeasureText(line.c_str(), fontSize));
            DrawText(line.c_str(), static_cast<int>(centerX - width * 0.5f), static_cast<int>(y), fontSize, color);
            y += lineHeight;
        }
    }
}

int main(int argc, char **argv)
{
    const std::string directory = platform::executableDirectory();
    // launcher.ini normally sits beside the executable (that is the release
    // layout); falling back to the working directory keeps development simple,
    // where the binary lives in build/ but the config does not.
    // --config decides which file everything else is read from, so it is picked
    // out before anything is. It is how a developer keeps their own build paths
    // while the launcher.ini that ships stays what a player actually needs.
    std::string configOverride;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            configOverride = argv[i + 1];

    std::string configPath;
    if (!configOverride.empty())
    {
        configPath = configOverride;
        if (!FileExists(configPath.c_str()))
        {
            std::fprintf(stderr, "[launcher] no config at %s\n", configPath.c_str());
            return 1;
        }
    }
    else
    {
        configPath = resolve(directory, "launcher.ini");
        if (!FileExists(configPath.c_str()))
            configPath = "launcher.ini";
    }
    // Relative paths inside the config (art, games/) are relative to the config
    // itself, so the whole folder can be moved anywhere as one unit.
    const std::string base = directoryOf(configPath);

    Settings settings;
    std::vector<Game> games = loadGames(configPath, settings);
    if (games.empty())
    {
        std::fprintf(stderr, "[launcher] no games configured in %s\n", configPath.c_str());
        return 1;
    }

    // Screenshot mode: render one frame with a given card selected and write it
    // out, so the layout can be checked without a person at the screen.
    std::string screenshotPath;
    int startSelection = -1;
    int launchIndex = -1;
    bool listPads = false;
    std::string selfUpdateVersion;
    // --build and --image: how the Windows launcher hands a build to this one under
    // WSL. It is the same config and the same port/, so the game is named by its
    // place in that list rather than by anything that could drift.
    bool buildOnStart = false;
    std::string startImage;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            screenshotPath = argv[++i];
        else if (std::strcmp(argv[i], "--select") == 0 && i + 1 < argc)
            startSelection = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--launch") == 0 && i + 1 < argc)
            launchIndex = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--pads") == 0)
            listPads = true; // answered after the window is up: pads need it
        else if (std::strcmp(argv[i], "--version") == 0)
        {
            std::printf("ps2x-launcher %s\n", PS2X_LAUNCHER_VERSION);
            return 0;
        }
        else if (std::strcmp(argv[i], "--list") == 0)
        {
            for (size_t i = 0; i < games.size(); ++i)
                std::printf("%zu\t%s\t%s\n", i, games[i].id.c_str(),
                            games[i].available ? "available" : "coming soon");
            return 0;
        }
        else if (std::strcmp(argv[i], "--build") == 0 && i + 1 < argc)
        {
            startSelection = std::atoi(argv[++i]);
            buildOnStart = true;
        }
        else if (std::strcmp(argv[i], "--image") == 0 && i + 1 < argc)
            startImage = argv[++i];
        else if (std::strcmp(argv[i], "--self-update") == 0 && i + 1 < argc)
        {
            selfUpdateVersion = argv[++i];
            // The button, without the window: how this was tested, and how a player
            // who is stuck can be walked through it.
            const std::string staging = updateStagingDirectory();
            const std::string command = launcherUpdateShellCommand(settings, selfUpdateVersion);
            std::vector<std::string> lines;
            const int status = runCapturedAll(command, lines);
            for (const std::string &line : lines)
                std::printf("  %s\n", line.c_str());
            if (status != 0)
            {
                std::printf("ps2x-launcher %s - could not fetch %s\n", PS2X_LAUNCHER_VERSION,
                            selfUpdateVersion.c_str());
                return 1;
            }
            const std::string reason = stageLauncherUpdate(directory, staging, launcherExeName(), updateScriptPath());
            if (!reason.empty())
            {
                std::printf("ps2x-launcher %s - %s\n", PS2X_LAUNCHER_VERSION, reason.c_str());
                return 1;
            }
            std::printf("ps2x-launcher %s - %s is staged in %s\n", PS2X_LAUNCHER_VERSION, selfUpdateVersion.c_str(),
                        staging.c_str());
            std::printf("  the swap runs once this has exited: %s\n", updateScriptPath().c_str());
            platform::runScriptDetached(updateScriptPath());
            return 0;
        }
        else if (std::strcmp(argv[i], "--check-updates") == 0)
        {
            // The button, without the window - so a script can ask, and so the
            // answer can be read somewhere other than a screenshot.
            std::vector<std::string> lines;
            const int status = runCapturedAll(launcherUpdateCommand(settings), lines);
            const std::string newest = status == 0 ? newestReleaseVersion(lines) : std::string{};
            if (newest.empty())
            {
                std::printf("ps2x-launcher %s - could not ask %s\n", PS2X_LAUNCHER_VERSION,
                            settings.updateRepo.c_str());
                return 1;
            }
            if (versionIsNewer(newest, PS2X_LAUNCHER_VERSION))
                std::printf("ps2x-launcher %s - %s is available: %s\n", PS2X_LAUNCHER_VERSION, newest.c_str(),
                            releasePageUrl(settings.updateRepo).c_str());
            else
                std::printf("ps2x-launcher %s - up to date (the newest release is %s)\n", PS2X_LAUNCHER_VERSION,
                            newest.c_str());
            return 0;
        }
    }

    // Run the game folders and data paths the way the config means them: a
    // relative path is beside the config, so a release folder moves as one unit.
    for (Game &game : games)
    {
        game.portConfigured = !game.portUrl.empty();
        game.portDir = resolve(base, game.portDir.empty() ? ("port/" + game.id) : game.portDir);
        game.disc = resolve(base, game.disc);
        game.exeConfigured = !game.exe.empty();
        game.dataConfigured = !game.data.empty();
        derivePaths(game);

        game.exe = resolve(base, game.exe);
        game.data = resolve(base, game.data);
        // The theme is optional. When the config does not name one, music/<id>.mp3
        // is what a drop-in would be called, so dropping the file in with the right
        // name needs no edit at all.
        game.musicNamed = !game.music.empty();
        game.music = resolve(base, game.musicNamed ? game.music : ("music/" + game.id + ".mp3"));
        // And the cover: art/<id>.png is what a drop-in would be called, so a card
        // with none still says exactly which file it is waiting for. The hint keeps
        // the config's own wording rather than the resolved path, which is absolute
        // and far too long to sit inside a card.
        const std::string artNamed = game.art;
        game.artDisplay = artNamed.empty() ? ("art/" + game.id + ".png") : artNamed;
        game.art = resolve(base, game.artDisplay);
    }

    // --launch skips the menu entirely, so a desktop shortcut can start a game
    // directly without the launcher flashing a window first.
    if (launchIndex >= 0 && launchIndex < static_cast<int>(games.size()))
    {
        if (!games[launchIndex].available)
        {
            std::fprintf(stderr, "[launcher] %s is not available yet\n", games[launchIndex].id.c_str());
            return 1;
        }
        const std::string command = launchCommand(games[launchIndex]);
        std::fprintf(stderr, "[launcher] %s\n", command.c_str());
        return std::system(command.c_str()) == 0 ? 0 : 1;
    }

    // --pads needs the window to exist, because that is what initialises the pad
    // support; it does not need to be seen.
    //
    // Deliberately *not* FLAG_WINDOW_HIGHDPI. On Windows that hint sets
    // GLFW_SCALE_TO_MONITOR, which resizes the window's content area by the
    // monitor's scale while raylib goes on reporting the size it was asked for -
    // so on a display at 125% or 150% the layout was drawn for one size inside a
    // framebuffer of another, and only came right after a fullscreen toggle
    // refreshed the size. Real pixels everywhere is what this layout wants.
    platform::becomeDpiAware();
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT | (listPads ? FLAG_WINDOW_HIDDEN : 0));
    InitWindow(1280, 720, "The Stories Collection - Launcher");
    if (listPads)
    {
        // raylib fills its gamepad state during a poll, which normally happens in the
        // frame loop. Asking before the first poll reports nothing at all - which is
        // what this did until a DualSense was finally plugged in to test with.
        PollInputEvents();
        for (int settle = 0; settle < 4; ++settle)
        {
            PollInputEvents();
            WaitTime(0.1);
        }

        // What the launcher can see, and what it would call the buttons. For working
        // out why a pad does not behave: this is the same naming the hints use.
        bool found = false;
        for (int slot = 0; slot < 4; ++slot)
        {
            if (!IsGamepadAvailable(slot))
                continue;
            found = true;
            const PadNames names = padNamesFor(slot);
            std::printf("%d: %s\n   axes=%d  bottom=%s  right=%s  left=%s  top=%s\n", slot, GetGamepadName(slot),
                        GetGamepadAxisCount(slot), names.bottom.c_str(), names.right.c_str(), names.left.c_str(),
                        names.top.c_str());
        }
        if (!found)
            std::printf("no controllers connected\n");
        CloseWindow();
        return 0;
    }
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);
    // Fullscreen by default: this is a front end meant to sit on the screen like a
    // console menu. F11 swaps at any time, and --screenshot always stays windowed
    // so a capture is the same size whatever the display.
    if (settings.fullscreen && screenshotPath.empty())
    {
        const int monitor = GetCurrentMonitor();
        SetWindowSize(GetMonitorWidth(monitor), GetMonitorHeight(monitor));
        ToggleFullscreen();
    }

    for (Game &game : games)
    {
        if (game.art.empty())
            continue;
        const std::string artPath = resolve(base, game.art);
        if (FileExists(artPath.c_str()))
        {
            game.artTexture = LoadTexture(artPath.c_str());
            game.artLoaded = game.artTexture.id != 0;
            if (game.artLoaded)
                SetTextureFilter(game.artTexture, TEXTURE_FILTER_BILINEAR);
        }
    }

    // ------------------------------------------------------------ menu music
    // Each theme is the player's own file, like the box art: nothing is shipped,
    // and having none is not an error. With nothing to play the audio device is
    // never opened at all, so a launcher with an empty music/ is completely silent
    // to the sound server rather than holding an idle output open.
    const float musicVolume = std::max(0.0f, std::min(1.0f, settings.musicVolume));
    bool anyThemeFile = false;
    for (const Game &game : games)
        if (FileExists(game.music.c_str()))
            anyThemeFile = true;

    bool audioReady = false;
    int themesLoaded = 0;
    if (musicVolume <= 0.0f)
        std::fprintf(stderr, "[launcher] menu music off (music_volume=0)\n");
    else if (!anyThemeFile)
        std::fprintf(stderr, "[launcher] no theme music in music/ yet: the menu will be silent\n");
    else
    {
        InitAudioDevice();
        audioReady = IsAudioDeviceReady();
        if (!audioReady)
            std::fprintf(stderr, "[launcher] no audio device: the menu will be silent\n");
    }

    for (Game &game : games)
    {
        if (!audioReady)
            break;
        if (!FileExists(game.music.c_str()))
        {
            // Only worth mentioning when the config asked for it by name; the
            // guess at music/<id>.mp3 being absent is the normal, quiet case.
            if (game.musicNamed)
                std::fprintf(stderr, "[launcher] music: %s: %s not found\n", game.id.c_str(), game.music.c_str());
            continue;
        }
        game.theme = LoadMusicStream(game.music.c_str());
        game.themeLoaded = IsMusicValid(game.theme);
        if (!game.themeLoaded)
        {
            std::fprintf(stderr, "[launcher] music: %s: %s would not decode\n", game.id.c_str(), game.music.c_str());
            continue;
        }
        game.theme.looping = true;
        SetMusicVolume(game.theme, 0.0f);
        ++themesLoaded;
        const float seconds = GetMusicTimeLength(game.theme);
        std::fprintf(stderr, "[launcher] music: %s <- %s (%d:%02d, loops)\n", game.id.c_str(), game.music.c_str(),
                     static_cast<int>(seconds) / 60, static_cast<int>(seconds) % 60);
    }

    int selected = startSelection >= 0 && startSelection < static_cast<int>(games.size()) ? startSelection : 0;
    std::vector<Card> cards(games.size());
    // Open with the selection already shown rather than fading in from nothing.
    cards[selected].highlight = 1.0f;

    // The theme follows the highlighted card. -1 so the first frame starts the
    // opening selection's theme: the menu has music the moment it appears.
    int previousSelected = -1;
    bool musicMuted = false;

    // A mouse that is merely *resting* over a card must not keep stealing the
    // selection from the keyboard or the controller, so hovering only counts
    // when the pointer actually moves.
    Vector2 lastMouse = GetMousePosition();

    // Stick navigation needs a nudge: hold a direction and it steps every so
    // often rather than every frame.
    float stickCooldown = 0.0f;

    // Which device was touched last. Every hint the launcher shows is worded for it,
    // so a controller player is never told to press a key. The port has done this
    // for its own prompts since it was written; the launcher had not.
    bool usingController = false;

    // What of each game's source is already here. Reading a revision is a local git
    // call, so it costs nothing to do now.
    for (Game &game : games)
        if (game.portConfigured && !game.portLocal.empty())
            game.portRemote = ""; // the check fills this in below
    for (Game &game : games)
        if (game.portConfigured)
            game.portLocal = readRevision(game.portDir);

    // Ask each remote what it has, in the background: the menu must never wait on
    // the network, and a machine that is offline simply never learns of an update.
    std::atomic<bool> checksDone{false};
    std::vector<std::string> checkedRevisions(games.size());
    std::thread checkThread;
    auto startUpdateChecks = [&]
    {
        if (checkThread.joinable())
            checkThread.join();
        checksDone = false;
        checkThread = std::thread([&, checked = &checkedRevisions]
        {
            for (size_t i = 0; i < games.size(); ++i)
            {
                if (!games[i].portConfigured)
                    continue;
                std::mutex discard;
                std::string line;
                if (runCaptured(checkCommand(games[i]), discard, line) != 0)
                    continue; // offline, or no git: there is simply nothing to report
                const size_t space = line.find_first_of(" \t");
                const std::string revision = space == std::string::npos ? line : line.substr(0, space);
                if (revision.size() >= 7 && revision.find_first_not_of("0123456789abcdef") == std::string::npos)
                    (*checked)[i] = revision;
            }
            checksDone = true;
        });
    };
    startUpdateChecks();

    // The launcher's own version, asked the same way and in the background. It
    // cannot update itself, so this is a button and a version to compare against -
    // but a player who never presses it would never learn, so it is asked once at
    // launch as well, and the button is for asking again.
    enum class LauncherCheck { Never, Running, UpToDate, Available, Failed };
    std::atomic<int> launcherCheck{static_cast<int>(LauncherCheck::Never)};
    std::mutex launcherVersionMutex;
    std::string launcherVersion; // the newest release the repository has
    bool updateOpened = false;   // the page was handed to the browser
    bool updateRefused = false;  // it was not, so the address is shown instead
    bool updateHandOver = false; // the new files are staged and the script is ready
    bool updateFailed = false;   // it could not be done, so the page was opened
    std::string updateTarget;    // the version being put in place
    std::string updateDetail;    // what the attempt last said
    std::thread launcherCheckThread;
    auto startLauncherCheck = [&]
    {
        if (launcherCheck.load() == static_cast<int>(LauncherCheck::Running))
            return;
        if (launcherCheckThread.joinable())
            launcherCheckThread.join();
        launcherCheck = static_cast<int>(LauncherCheck::Running);
        launcherCheckThread = std::thread([&]
        {
            std::vector<std::string> lines;
            const int status = runCapturedAll(launcherUpdateCommand(settings), lines);
            const std::string newest = status == 0 ? newestReleaseVersion(lines) : std::string{};
            {
                std::lock_guard<std::mutex> lock(launcherVersionMutex);
                launcherVersion = newest;
            }
            // No answer at all is not the same as "up to date": offline, or no git,
            // would otherwise be reported as being current, which is a lie a player
            // would act on.
            launcherCheck = static_cast<int>(newest.empty()
                                                 ? LauncherCheck::Failed
                                                 : (versionIsNewer(newest, PS2X_LAUNCHER_VERSION)
                                                        ? LauncherCheck::Available
                                                        : LauncherCheck::UpToDate));
        });
    };
    startLauncherCheck();

    // One background job at a time: the game, or fetching a port's source. Waiting
    // in a thread rather than blocking the loop is what lets the launcher keep
    // answering the window manager while it works, and notice the moment it ends.
    enum class Job { None, Game, Fetch, Build, Launcher };
    std::atomic<int> jobKind{static_cast<int>(Job::None)};
    std::atomic<int> jobStatus{0};
    std::thread jobThread;
    Job jobAway = Job::None; // not None while we are away from the menu
    int jobGame = -1;
    std::mutex jobMutex;   // guards jobDetail
    std::string jobDetail; // the last line the job printed
    float menuLit = 1.0f;  // ramps back up when a job finishes

    // The wizard's own state, written by the build thread and read by the menu
    // thread: which step it is on, and the process it can stop.
    std::atomic<int> buildStep{-1};
    std::atomic<int> buildPid{0};
    std::atomic<bool> buildStop{false};
    std::atomic<bool> buildOk{false};
    double buildStarted = 0.0;
    bool buildCrosses = false;       // whether this run also cross-compiles Windows
    std::vector<int> buildPlan;      // the steps it will take, in order
    int buildPlanAt = 0;             // which of them is running

    // A build that ends - finished, failed or stopped - says so on the way back to
    // the menu, and says where the whole of what it printed was written. Without
    // this a failure dropped the player back on a card that still read "not built"
    // and told them nothing, which is no way to hand someone a twenty-minute build.
    bool buildReport = false;
    bool buildReportOk = false;
    std::string buildReportDetail;
    std::string buildReportLog;

    // A fetch that fails needs saying too, and for the same reason: it can be over
    // in milliseconds, so the screen that showed what it was doing is gone before
    // anyone can read it. That is exactly what "pressing U does nothing" was.
    bool fetchReport = false;
    std::string fetchReportDetail;

    // And what the launcher says when it cannot do what was asked and no job can
    // report it: pressing B where building cannot happen, for one. Silence there is
    // the worst possible answer, because it reads as a broken key.
    bool notice = false;
    bool noticeBusy = false; // something is being waited for; say so, and keep drawing
    float noticeOpenedAt = 0.0f;
    std::string noticeTitle;
    std::string noticeText;
    // A notice can offer to put something right rather than only describe it. Both of
    // the things worth offering are Windows-side and take minutes, so each is a script
    // in its own window with the outcome left on the screen.
    enum class Offer { None, InstallWsl, InstallToolchain };
    Offer noticeOffer = Offer::None;
    const auto offerScript = [](Offer which) -> std::string
    {
        if (which == Offer::InstallWsl)
            return "@echo off\r\n"
                   "title Installing WSL\r\n"
                   "rem WSL is the Linux inside Windows that builds the port. It is a free\r\n"
                   "rem optional Windows feature; nothing else on this PC is touched.\r\n"
                   "echo Installing WSL. Windows will ask for permission first.\r\n"
                   "echo.\r\n"
                   "wsl --install\r\n"
                   "echo.\r\n"
                   "echo If Windows says a restart is needed, restart, then press B in the\r\n"
                   "echo launcher again. If it says the option is unknown, this Windows is\r\n"
                   "echo older than the one-line install and the README has the manual steps.\r\n"
                   "pause\r\n";
        if (which == Offer::InstallToolchain)
            return "@echo off\r\n"
                   "title Installing the C++ toolchain inside WSL\r\n"
                   "rem The compiler the port is built with, inside WSL. It runs as root\r\n"
                   "rem inside WSL, which is a place this launcher already owns - so it needs\r\n"
                   "rem no password and cannot reach anything outside WSL.\r\n"
                   "echo Installing cmake and a C++ toolchain inside WSL.\r\n"
                   "echo.\r\n"
                   "wsl -u root -- bash -lc \"apt-get update && apt-get install -y cmake build-essential "
                   "git curl ca-certificates python3\"\r\n"
                   "echo.\r\n"
                   "echo When that has finished, press B in the launcher again.\r\n"
                   "pause\r\n";
        return {};
    };

    const auto setNotice = [&](const std::string &title, const std::string &text, Offer offer, bool busy)
    {
        noticeTitle = title;
        noticeText = text;
        noticeOffer = offer;
        noticeBusy = busy;
        noticeOpenedAt = GetTime();
        notice = true;
    };

    // Nothing about WSL can be asked quickly the first time: answering means starting a
    // Linux kernel, which takes seconds, and three questions asked on the thread that
    // draws the menu is a window that stops answering - which is indistinguishable from
    // a crash, and was reported as one. So it is asked in the background, the notice
    // says it is being asked, and the answer is remembered so it is asked once.
    enum class Wsl { Unknown, Asking, Ready, NoWsl, NoLauncher, NoToolchain };

    // The question and the answer together, in something that joins its own thread
    // however main leaves: a thread that is joinable and never joined is a terminate().
    struct WslAsker
    {
        std::atomic<int> state{static_cast<int>(Wsl::Unknown)};
        std::string report;      // what was asked, and what came back
        bool needsRestart = false;
        std::thread thread;
        ~WslAsker()
        {
            if (thread.joinable())
                thread.join();
        }
    } wsl;

    const auto askAboutWsl = [&]
    {
        // Only a "yes" is remembered. A "no" has to be asked again on the next press,
        // because the whole point of offering to install WSL is that the player goes
        // and does it and comes back - and a remembered "no" is what made pressing B
        // after an install do nothing at all.
        const int known = wsl.state.load();
        if (known == static_cast<int>(Wsl::Ready) || known == static_cast<int>(Wsl::Asking))
            return;
        if (wsl.thread.joinable())
            wsl.thread.join();
        wsl.state = static_cast<int>(Wsl::Asking);
        wsl.thread = std::thread([&]
        {
            // Every answer is kept as what was run and what came back, not only as a
            // verdict. The launcher has now told someone the wrong thing about their own
            // machine twice, and both times the fix came from reading the output rather
            // than from reasoning about it. The report is written before the state and
            // read after it, so the atomic is what orders them.
            //
            // WSL prints UTF-16, so a captured line is every other byte a NUL. Made
            // readable, because this text is the whole point of the report.
            const auto plainText = [](const std::string &text)
            {
                const bool wide = text.size() >= 2 && text[1] == '\0';
                std::string out;
                for (size_t i = 0; i < text.size(); i += wide ? 2 : 1)
                    if (text[i] != '\0' && static_cast<unsigned char>(text[i]) >= 32)
                        out += text[i];
                return out;
            };
            const auto probe = [&](const std::string &command)
            {
                std::vector<std::string> lines;
                const int status = runCapturedAll(command, lines);
                std::string said = lines.empty() ? std::string("no output") : plainText(lines.front());
                if (said.size() > 160)
                    said.resize(160);
                wsl.report += "  " + command + "\n    exit " + std::to_string(status) + ": " + said + "\n";
                return status == 0;
            };

            wsl.report.clear();
            wsl.report += std::string("  wsl.exe found: ") + (platform::wslAvailable() ? "yes" : "no") + "\n";
            // uname answers "is there a Linux behind wsl.exe"; cmake and g++ are asked
            // directly rather than read out of a package list, because what matters is
            // whether a build can run, not how it got there.
            const bool usable = platform::wslAvailable() && probe("wsl.exe -e uname");
            const bool launcher = platform::fileIsExecutable(directory + "/ps2x_launcher");
            const bool cmake = usable && probe("wsl.exe -e cmake --version");
            const bool compiler = usable && probe("wsl.exe -e g++ --version");
            // What Windows says when the feature is enabled but has not taken effect yet.
            // That is the most likely reason for a "no" straight after an install, and
            // the one thing a player can put right in a minute.
            const std::string lower = [&]
            {
                std::string s = wsl.report;
                for (char &c : s)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return s;
            }();
            wsl.needsRestart = lower.find("restart") != std::string::npos ||
                               lower.find("virtual machine platform") != std::string::npos ||
                               lower.find("not enabled") != std::string::npos ||
                               lower.find("no installed distributions") != std::string::npos;
            wsl.state = static_cast<int>(!usable              ? Wsl::NoWsl
                                        : !launcher           ? Wsl::NoLauncher
                                        : !(cmake && compiler) ? Wsl::NoToolchain
                                                               : Wsl::Ready);
        });
    };

    // What the answer means: the build, or the one thing standing in its way, with the
    // offer that puts it right.
    const auto showWslOutcome = [&]
    {
        const std::string command =
            "wsl --cd \"" + directory + "\" ./ps2x_launcher --build " + std::to_string(selected);
        switch (static_cast<Wsl>(wsl.state.load()))
        {
        case Wsl::NoWsl:
            setNotice("WSL is not set up yet",
                      std::string("WSL is what lets this launcher build. It is Linux inside Windows, the port "
                                  "is built by Linux, and WSL is a free optional Windows feature - not part of "
                                  "Windows by default, but one command and one restart to add.\n\nPress W and "
                                  "the launcher will install it for you. Windows will ask for permission first, "
                                  "and will say if a restart is needed.") +
                          (wsl.needsRestart
                               ? "\n\nWindows is saying that the feature is enabled but not in force yet, "
                                 "which means a restart. That is the most likely reason straight after an "
                                 "install, and the whole of the fix."
                               : "") +
                          "\n\nWhat the launcher asked, and what Windows said:\n\n" + wsl.report,
                      Offer::InstallWsl, false);
            break;
        case Wsl::NoLauncher:
            setNotice("The Linux launcher is missing",
                      "WSL is ready, but the Linux launcher is not in this folder, so there is nothing to hand "
                      "the build to. It ships in the Windows release - download it again and unpack it over "
                      "this folder.",
                      Offer::None, false);
            break;
        case Wsl::NoToolchain:
            setNotice("WSL needs a compiler",
                      "WSL is running, but it has no C++ toolchain inside it, and that is what a build is made "
                      "with. Installing it is one command.\n\nPress W and the launcher will install cmake and "
                      "a toolchain inside WSL for you. It needs no password: it runs as root inside WSL, which "
                      "is a place you already own.\n\nWhat the launcher asked, and what WSL said:\n\n" +
                          wsl.report,
                      Offer::InstallToolchain, false);
            break;
        default:
        {
            // Ready. Building is done by Linux, so this starts the Linux launcher under
            // WSL on the build for this game: the same launcher.ini, the same port/, and
            // under WSL it cross-compiles the Windows port - which this one plays.
            std::vector<std::string> arguments;
            arguments.push_back("--cd");
            arguments.push_back(directory);
            arguments.push_back("./ps2x_launcher");
            arguments.push_back("--build");
            arguments.push_back(std::to_string(selected));
            if (!games[selected].disc.empty())
            {
                arguments.push_back("--image");
                arguments.push_back(platform::toWslPath(games[selected].disc));
            }
            const bool started = platform::runDetached("wsl.exe", arguments);
            std::fprintf(stderr, "[launcher] %s wsl.exe\n", started ? "started" : "could not start");
            setNotice(started ? "Building in WSL" : "Could not start WSL",
                      started ? ("A build window has opened. Press Start in it. It builds the Windows port, "
                                 "and this launcher plays it.\n\nIf no window appeared, WSL could not start "
                                 "the Linux launcher. The command to run by hand is:\n\n" + command)
                              : ("The launcher could not start WSL. The command to run by hand is:\n\n" + command),
                      Offer::None, false);
            break;
        }
        }
    };

    // Whether a run also cross-compiles Windows. WSL is a Linux machine running on a
    // Windows one, so there the Windows build is the one that is any use; anywhere
    // else it is this machine's own, unless the config says otherwise either way.
    auto willCrossCompile = [&]
    { return settings.crossCompile < 0 ? builder::runningUnderWsl() : settings.crossCompile != 0; };

    auto beginJob = [&](Job kind, int index) -> bool
    {
        if (jobKind.load() != static_cast<int>(Job::None))
            return false;
        if (jobThread.joinable())
            jobThread.join();
        jobGame = index;
        jobAway = kind;
        jobStatus = 0;
        {
            std::lock_guard<std::mutex> lock(jobMutex);
            jobDetail.clear();
        }
        jobKind = static_cast<int>(kind);
        SetTargetFPS(kind == Job::Game ? 10 : 20); // none of them needs a full frame rate
        return true;
    };

    auto startJob = [&](Job kind, int index, const std::string &command, bool captureOutput)
    {
        if (!beginJob(kind, index))
            return;
        std::fprintf(stderr, "[launcher] %s\n", command.c_str());
        jobThread = std::thread([&, kind, index, command, captureOutput]
        {
            // A fetch and the launcher's own update both keep every line they print,
            // because a failure is usually the one line a step said - and the screen
            // that showed it is gone by the time anyone can read it.
            if (kind == Job::Fetch || kind == Job::Launcher)
            {
                const bool update = kind == Job::Launcher;
                std::vector<std::string> lines;
                jobStatus = runCapturedAll(command, lines);
                std::string tail;
                for (const std::string &line : lines)
                {
                    tail += line;
                    tail += "\n";
                }
                const std::string logPath =
                    base + "/logs/" + (update ? std::string("launcher-update.log")
                                              : games[index].id + "-fetch.log");
                std::error_code logEc;
                std::filesystem::create_directories(std::filesystem::path(logPath).parent_path(), logEc);
                {
                    std::ofstream out(logPath, std::ios::binary | std::ios::trunc);
                    if (out)
                        out << tail;
                }
                // The update is not over when the download is: the archive unpacks
                // into a folder named after the release, and the swap needs a script.
                std::string reason;
                if (update && jobStatus == 0)
                {
                    reason = stageLauncherUpdate(directory, updateStagingDirectory(), launcherExeName(),
                                                 updateScriptPath());
                    if (!reason.empty())
                    {
                        std::fprintf(stderr, "[launcher] %s\n", reason.c_str());
                        jobStatus = 1;
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(jobMutex);
                    if (!reason.empty())
                        jobDetail = reason;
                    else
                    {
                        jobDetail = lines.empty() ? std::string{} : lines.back();
                        for (auto it = lines.rbegin(); it != lines.rend() && jobDetail.size() < 40; ++it)
                        {
                            if (it != lines.rbegin())
                                jobDetail = *it + " / " + jobDetail;
                        }
                    }
                }
                jobKind = static_cast<int>(Job::None);
                return;
            }
            jobStatus = captureOutput ? runCaptured(command, jobMutex, jobDetail) : std::system(command.c_str());
            jobKind = static_cast<int>(Job::None);
        });
    };

    // The launcher's own update. The asset for this platform is downloaded into a
    // staging folder and unpacked there, and when that is done a script is ready to
    // put it in place - which happens after the launcher has exited, the only time it
    // can. Everything here is on the menu thread, so a failure leaves the launcher
    // exactly as it was and the click falls back to the release page.
    auto startLauncherUpdate = [&](const std::string &version) -> bool
    {
        if (version.empty() || jobKind.load() != static_cast<int>(Job::None))
            return false;
        if (repositoryPath(settings.updateRepo).empty())
            return false;
        // Only an installed copy updates itself. A developer's build has the binary
        // in build/ and the settings somewhere else, and replacing that folder would
        // drop a release's launcher.ini where the next run would find it.
        if (base != directory)
            return false;

        const std::string command = launcherUpdateShellCommand(settings, version);
        updateTarget = version;
        updateFailed = false;
        updateOpened = false;
        updateRefused = false;
        startJob(Job::Launcher, selected, command, false);
        return true;
    };

    // A build is a sequence rather than one command, so the thread runs the sequence
    // and the step it has reached is what the screen draws.
    auto startBuild = [&](int index, const std::string &image)
    {
        if (!beginJob(Job::Build, index))
            return;
        buildCrosses = willCrossCompile();
        buildPlan = builder::plan(buildCrosses);
        buildPlanAt = 0;
        buildStep = buildPlan.empty() ? -1 : buildPlan.front();
        buildStop = false;
        buildOk = false;
        buildStarted = GetTime();
        buildReport = false;

        // Everything a build says, kept. It is the only record of why a build that
        // failed failed, so it is written before the first step rather than after
        // something goes wrong - and beside the launcher rather than in the source
        // folder, so it survives the source folder being deleted or re-fetched.
        const std::string logPath = base + "/logs/" + games[index].id + "-build.log";
        std::error_code logEc;
        std::filesystem::create_directories(std::filesystem::path(logPath).parent_path(), logEc);
        FILE *log = std::fopen(logPath.c_str(), "w");

        std::fprintf(stderr, "[launcher] building %s from %s\n", games[index].id.c_str(), image.c_str());
        jobThread = std::thread([&, index, image, log]
        {
            const auto write = [&](const char *prefix, const std::string &text)
            {
                if (log != nullptr)
                {
                    std::fprintf(log, "%s%s\n", prefix, text.c_str());
                    std::fflush(log);
                }
            };
            const auto finish = [&]
            {
                if (log != nullptr)
                    std::fclose(log);
            };

            write("", "The Stories Collection - build log");
            write("game:  ", games[index].title);
            write("from:  ", image);
            write("", "If the build failed, the last line below is the reason.");
            write("", "");

            const builder::Recipe recipe = builder::readRecipe(games[index].portDir);
            if (!recipe.ok)
            {
                write("", recipe.error);
                {
                    std::lock_guard<std::mutex> lock(jobMutex);
                    jobDetail = recipe.error;
                }
                finish();
                jobStatus = -1;
                jobKind = static_cast<int>(Job::None);
                return;
            }
            builder::Callbacks callbacks;
            callbacks.childPid = &buildPid;
            callbacks.keepGoing = [&] { return !buildStop.load(); };
            callbacks.onStep = [&](int step)
            {
                buildStep = step;
                buildPlanAt = static_cast<int>(std::find(buildPlan.begin(), buildPlan.end(), step) - buildPlan.begin());
                std::fprintf(stderr, "[build] %s\n", builder::stepName(step));
                write("", std::string("== ") + builder::stepName(step));
            };
            callbacks.onLine = [&](const std::string &line)
            {
                // Onto the console and into the log as well as the screen: a build
                // that fails needs something to read afterwards, and the screen is
                // gone by then.
                std::fprintf(stderr, "[build]   %s\n", line.c_str());
                write("   ", line);
                std::lock_guard<std::mutex> lock(jobMutex);
                jobDetail = line;
            };
            builder::Targets targets;
            targets.windows = buildCrosses;
            targets.toolchain = settings.toolchain;
            buildOk = builder::run(games[index].portDir, image, recipe, targets, callbacks);
            finish();
            jobStatus = buildOk.load() ? 0 : -1;
            jobKind = static_cast<int>(Job::None);
        });
    };

    // What is drawn while the launcher is away from the menu. A game gets a black
    // screen because the game itself is about to cover it; a fetch has nothing
    // behind it, so it says what it is doing.
    auto drawAwayScreen = [&](Job kind)
    {
        const float w = static_cast<float>(GetScreenWidth());
        const float h = static_cast<float>(GetScreenHeight());
        const float fs = std::max(0.8f, std::min(2.5f, std::min(w / 1280.0f, h / 720.0f)));
        const auto px = [fs](float v) { return static_cast<int>(std::lround(v * fs)); };
        ClearBackground(kind == Job::Game ? BLACK : (Color){12, 13, 17, 255});
        if (jobGame < 0 || jobGame >= static_cast<int>(games.size()))
            return;

        // Building. The list is the pipeline, and which line it is on is the honest
        // answer to "how long is this going to be".
        if (kind == Job::Build)
        {
            const int current = buildPlanAt;
            const float rowHeight = 32.0f * fs;
            const std::vector<std::string> title = wrapText("Building " + games[jobGame].title, px(26), w * 0.8f);
            float y = h * 0.5f - static_cast<float>(buildPlan.size()) * rowHeight * 0.5f - 96.0f * fs;
            drawCenteredLines(title, w * 0.5f, y, px(26), (Color){236, 240, 248, 255}, 34.0f * fs);
            y += static_cast<float>(title.size()) * 34.0f * fs + 8.0f * fs;
            drawCenteredLines({buildCrosses ? "for this machine, and for Windows" : "for this machine"}, w * 0.5f, y,
                              px(17), (Color){126, 134, 152, 255}, 24.0f * fs);
            y += 40.0f * fs;

            const float left = w * 0.5f - 230.0f * fs;
            for (size_t i = 0; i < buildPlan.size(); ++i)
            {
                const bool done = static_cast<int>(i) < current;
                const bool running = static_cast<int>(i) == current;
                const Color colour = done      ? (Color){116, 170, 132, 255}
                                     : running ? (Color){240, 200, 90, 255}
                                               : (Color){92, 100, 118, 255};
                const std::string text = std::string(done ? "[done] " : running ? "[>]    " : "[ ]    ") +
                                         builder::stepName(buildPlan[i]);
                DrawText(text.c_str(), static_cast<int>(left), static_cast<int>(y), px(20), colour);
                y += rowHeight;
            }

            y += 6.0f * fs;
            if (current >= 0 && current < static_cast<int>(buildPlan.size()))
            {
                drawCenteredLines(wrapText(builder::stepNote(buildPlan[current]), px(16), w * 0.7f), w * 0.5f, y,
                                  px(16), (Color){126, 134, 152, 255}, 22.0f * fs);
                y += 52.0f * fs;
            }
            std::string detail;
            {
                std::lock_guard<std::mutex> lock(jobMutex);
                detail = jobDetail;
            }
            if (!detail.empty())
            {
                const std::vector<std::string> lines = wrapText(detail, px(16), w * 0.8f);
                drawCenteredLines({lines.back()}, w * 0.5f, y, px(16), (Color){150, 158, 176, 255}, 22.0f * fs);
            }

            const int seconds = static_cast<int>(GetTime() - buildStarted);
            drawCenteredLines({std::to_string(seconds / 60) + " min " + std::to_string(seconds % 60) + " s"},
                              w * 0.5f, h - 74.0f * fs, px(16), (Color){104, 112, 130, 255}, 22.0f * fs);
            drawCenteredLines({usingController ? (padNames().right + " stops it") : "Esc stops it"}, w * 0.5f,
                              h - 44.0f * fs, px(16), (Color){104, 112, 130, 255}, 22.0f * fs);
            return;
        }

        // The launcher's own update. Short, but it is the one job that ends with the
        // launcher closing and starting itself again, so it says so rather than
        // leaving a window that vanishes and a new one that appears.
        if (kind == Job::Launcher)
        {
            const std::vector<std::string> heading =
                wrapText("Updating the launcher to " + updateTarget, px(26), w * 0.8f);
            std::string detail;
            {
                std::lock_guard<std::mutex> lock(jobMutex);
                detail = jobDetail;
            }
            float y = (h - 170.0f * fs - static_cast<float>(heading.size()) * 34.0f * fs) * 0.5f;
            drawCenteredLines(heading, w * 0.5f, y, px(26), (Color){236, 240, 248, 255}, 34.0f * fs);
            y += static_cast<float>(heading.size()) * 34.0f * fs + 20.0f * fs;
            if (!detail.empty())
                drawCenteredLines({wrapText(detail, px(18), w * 0.8f).back()}, w * 0.5f, y, px(18),
                                  (Color){150, 158, 176, 255}, 24.0f * fs);
            y += 44.0f * fs;
            drawCenteredLines(wrapText("The launcher closes and starts again by itself. Nothing in this folder is "
                                       "replaced until it has.",
                                       px(16), w * 0.8f),
                              w * 0.5f, y, px(16), (Color){104, 112, 130, 255}, 22.0f * fs);
            return;
        }

        if (kind != Job::Fetch)
            return;

        const std::vector<std::string> heading =
            wrapText("Fetching the source for " + games[jobGame].title, px(26), w * 0.8f);
        std::string detail;
        {
            std::lock_guard<std::mutex> lock(jobMutex);
            detail = jobDetail;
        }
        float y = (h - 150.0f * fs - static_cast<float>(heading.size()) * 34.0f * fs) * 0.5f;
        drawCenteredLines(heading, w * 0.5f, y, px(26), (Color){236, 240, 248, 255}, 34.0f * fs);
        y += static_cast<float>(heading.size()) * 34.0f * fs + 20.0f * fs;
        if (!detail.empty())
        {
            // The tail of what git last said: progress on the way, an error if not.
            const std::vector<std::string> lines = wrapText(detail, px(18), w * 0.8f);
            drawCenteredLines({lines.back()}, w * 0.5f, y, px(18), (Color){150, 158, 176, 255}, 24.0f * fs);
        }
        y += 44.0f * fs;
        const std::vector<std::string> note =
            wrapText("The port's own source, not the game - your disc is what makes the game.", px(16), w * 0.8f);
        drawCenteredLines(note, w * 0.5f, y, px(16), (Color){104, 112, 130, 255}, 22.0f * fs);
    };

    // The wizard: choosing where the game is read from, then confirming it. The
    // confirmation is not ceremony - a build is twenty to forty minutes, and one
    // press on a controller should not be able to start it.
    enum class Wizard { None, Pick, Confirm };
    Wizard wizard = Wizard::None;
    std::vector<builder::Source> sources;
    std::vector<Rectangle> sourceRects;
    int sourcePick = 0;
    std::string wizardImage;       // what the build would read
    std::string wizardLabel;       // what to call it on the confirmation
    bool wizardFromPicker = false; // so backing out returns to the list
    Vector2 lastWizardMouse{};

    // Which pad the player is on. It cannot simply be the first one: on a desktop a
    // gaming keyboard and a mouse dongle both claim a joystick slot - this machine's
    // keyboard takes two - and the launcher spent its life reading that as the
    // controller while a DualSense sat in slot 2 ignored. Every slot is read, and the
    // one that is actually touched becomes the one the hints describe.
    int activePad = -1;

    // Quitting is asked about rather than done. Circle sits under a thumb and Esc is
    // one key away from nothing; neither should close a launcher by accident.
    bool quitAsk = false;

    // The screens that do not reach the menu's input handling - the picker, the
    // confirmation, the build itself - read the pads for themselves, for the same
    // reason: whichever slot answers is the one in hand.
    auto padPressedAny = [&activePad](int button) -> bool
    {
        for (int slot = 0; slot < 4; ++slot)
        {
            if (IsGamepadAvailable(slot) && IsGamepadButtonPressed(slot, button))
            {
                activePad = slot;
                return true;
            }
        }
        return false;
    };

    // A stick reads as a direction once it is pushed past this, and not again until
    // it comes back.
    auto padStickX = [&activePad]() -> float
    {
        for (int slot = 0; slot < 4; ++slot)
        {
            if (!IsGamepadAvailable(slot))
                continue;
            const float x = GetGamepadAxisMovement(slot, GAMEPAD_AXIS_LEFT_X);
            if (std::fabs(x) > 0.5f)
            {
                activePad = slot;
                return x;
            }
        }
        return 0.0f;
    };

    // The two confirmation screens are answered the same way: move between the
    // options, and Cross takes the one under the highlight. That is how a console
    // dialog has always worked, and it means the button you are looking at is the
    // one that happens - rather than one button always meaning yes and another
    // always meaning no. Returns 0 for the first option, 1 for the second, -1 for
    // nothing yet.
    int dialogChoice = 0;
    float dialogStick = 0.0f;
    auto dialogPick = [&](const Rectangle &first, const Rectangle &second) -> int
    {
        const Vector2 mouse = GetMousePosition();
        if (CheckCollisionPointRec(mouse, first))
            dialogChoice = 0;
        else if (CheckCollisionPointRec(mouse, second))
            dialogChoice = 1;

        if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A) || padPressedAny(GAMEPAD_BUTTON_LEFT_FACE_LEFT))
            dialogChoice = 0;
        if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D) || padPressedAny(GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
            dialogChoice = 1;
        dialogStick -= GetFrameTime();
        const float stick = padStickX();
        if (dialogStick <= 0.0f && std::fabs(stick) > 0.5f)
        {
            dialogChoice = stick < 0.0f ? 0 : 1;
            dialogStick = 0.25f;
        }

        const Rectangle chosen = dialogChoice == 0 ? first : second;
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) ||
            (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, chosen)) ||
            padPressedAny(GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
            return dialogChoice;
        // Backing out is always the second option, which is the one that does nothing.
        if (IsKeyPressed(KEY_ESCAPE) || padPressedAny(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
            return 1;
        return -1;
    };

    // A screenshot is taken of one frame, and a background check that has not
    // answered yet would be caught mid-sentence - "checking..." instead of the
    // answer. So screenshot mode waits for them: it is the mode used to look at
    // the layout, and the layout is what the answer changes.
    if (!screenshotPath.empty())
    {
        if (checkThread.joinable())
            checkThread.join();
        if (launcherCheckThread.joinable())
            launcherCheckThread.join();
    }

    while (true)
    {
        // Which names this pad's buttons go by, read once a frame so a controller
        // plugged in or swapped mid-session is picked up. Until one has been touched,
        // the first slot is the best guess there is.
        // --build: someone else asked for this build, so it opens on it rather than on
        // the menu. Once, on the first frame, before anything is drawn.
        if (buildOnStart)
        {
            buildOnStart = false;
            if (startImage.empty())
            {
                sources = builder::findSources(base);
                sourcePick = 0;
                wizard = Wizard::Pick;
            }
            else
            {
                wizardImage = startImage;
                wizardLabel = startImage;
                wizardFromPicker = false;
                dialogChoice = 0; // Start
                wizard = Wizard::Confirm;
            }
        }

        const PadNames pad = padNamesFor(activePad >= 0 ? activePad : 0);

        // Away from the menu. Closing the window is ignored on purpose: the work is
        // still going, and quitting now would leave a game running or a clone half
        // written.
        if (jobKind.load() != static_cast<int>(Job::None))
        {
            // A build can be stopped. The step being run is killed, and the sequence
            // ends at the next boundary - re-running it is cheap, because every step
            // skips what is already done.
            const bool stopPressed =
                IsKeyPressed(KEY_ESCAPE) ||
                padPressedAny(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);
            if (jobKind.load() == static_cast<int>(Job::Build) && stopPressed)
            {
                buildStop = true;
                builder::stop(buildPid);
            }
            BeginDrawing();
            drawAwayScreen(static_cast<Job>(jobKind.load()));
            EndDrawing();
            continue;
        }
        if (jobAway != Job::None)
        {
            const Job finished = jobAway;
            const int status = jobStatus.load();
            const int index = jobGame;
            jobAway = Job::None;
            SetTargetFPS(60);
            SetWindowFocused();
            menuLit = 0.0f;
            if (finished == Job::Game)
            {
                if (status != 0)
                    std::fprintf(stderr, "[launcher] %s exited with status %d\n", games[selected].id.c_str(), status);
                if (!musicMuted)
                    startTheme(games[selected]);
            }
            else if (finished == Job::Build)
            {
                if (index >= 0 && index < static_cast<int>(games.size()))
                {
                    // What is on disk decided these paths, so redoing it is what turns
                    // the card from "not built" into "ready". Only the derived ones are
                    // dropped: an explicit exe=/data= belongs to the config.
                    if (!games[index].exeConfigured)
                        games[index].exe.clear();
                    if (!games[index].dataConfigured)
                        games[index].data.clear();
                    derivePaths(games[index]);

                    // Say how it ended, whatever the answer was, and point at the log.
                    buildReport = true;
                    buildReportOk = buildOk.load();
                    {
                        std::lock_guard<std::mutex> lock(jobMutex);
                        buildReportDetail = jobDetail;
                    }
                    buildReportLog = base + "/logs/" + games[index].id + "-build.log";
                    if (!buildOk.load())
                        std::fprintf(stderr, "[launcher] the build for %s did not finish\n",
                                     games[index].id.c_str());
                }
            }
            else if (finished == Job::Launcher)
            {
                if (status == 0)
                {
                    // The new files are staged and the script that swaps them is
                    // written. Nothing is left but to get out of the way, and that
                    // happens once the loop is over - the window has to close first.
                    updateHandOver = true;
                }
                else
                {
                    // A click that asked for an update should still leave a way to
                    // get one, so the page is opened as a fallback and the button
                    // says what happened.
                    updateFailed = true;
                    {
                        std::lock_guard<std::mutex> lock(jobMutex);
                        updateDetail = jobDetail;
                    }
                    const std::string url = releasePageUrl(settings.updateRepo);
                    updateOpened = platform::openUrl(url);
                    updateRefused = !updateOpened;
                    if (updateOpened)
                        MinimizeWindow();
                    std::fprintf(stderr, "[launcher] the launcher update failed: %s\n", updateDetail.c_str());
                }
            }
            else if (index >= 0 && index < static_cast<int>(games.size()))
            {
                games[index].portLocal = readRevision(games[index].portDir);
                if (status == 0 && !games[index].portLocal.empty())
                {
                    // Just taken from the ref, so this *is* what the remote has.
                    games[index].portRemote = games[index].portLocal;
                }
                else
                {
                    // Said on the screen, not only on a console that is not there when
                    // the launcher is started from a file manager.
                    fetchReport = true;
                    {
                        std::lock_guard<std::mutex> lock(jobMutex);
                        fetchReportDetail = jobDetail;
                    }
                    if (fetchReportDetail.empty())
                        fetchReportDetail = "nothing was reported.";
                    fetchReportDetail += "   (the whole thing is in logs/" + games[index].id + "-fetch.log)";
                    if (!platform::commandExists("curl"))
                        fetchReportDetail += "   (curl was not found, and the launcher downloads a game's source "
                                             "with it. It ships with Windows 10 and with every desktop Linux.)";
                    std::fprintf(stderr, "[launcher] fetching the %s source failed (status %d)\n",
                                 games[index].id.c_str(), status);
                }
            }
        }
        // The launcher's own update leaves by the door: the new files are staged and
        // the script that swaps them is written, so there is nothing left to draw.
        if (updateHandOver)
            break;
        // How the build ended, and where to read the rest. A build that failed used
        // to put the player back on the menu with the card unchanged and nothing
        // said at all; the reason is the most useful thing the launcher has, so it
        // goes on the screen and the log beside it is named.
        if (buildReport)
        {
            const float w = static_cast<float>(GetScreenWidth());
            const float h = static_cast<float>(GetScreenHeight());
            const float fs = std::max(0.8f, std::min(2.5f, std::min(w / 1280.0f, h / 720.0f)));
            const auto px = [fs](float v) { return static_cast<int>(std::lround(v * fs)); };

            BeginDrawing();
            ClearBackground((Color){12, 13, 17, 255});

            const bool stopped = !buildReportOk && buildStop.load();
            const std::string heading =
                buildReportOk ? "Build finished" : (stopped ? "Build stopped" : "Build did not finish");
            drawCenteredLines({heading}, w * 0.5f, h * 0.5f - 170.0f * fs, px(30),
                              buildReportOk ? (Color){140, 210, 160, 255} : (Color){240, 170, 120, 255}, 38.0f * fs);

            float y = h * 0.5f - 100.0f * fs;
            if (buildReportOk)
            {
                drawCenteredLines(wrapText("The game is ready - the card now says so, and Cross plays it.", px(18),
                                           w * 0.7f),
                                  w * 0.5f, y, px(18), (Color){200, 208, 224, 255}, 26.0f * fs);
            }
            else
            {
                const std::string reason =
                    buildReportDetail.empty() ? "nothing was reported. The log has everything it printed."
                                              : buildReportDetail;
                drawCenteredLines(wrapText("A build carries on from the step it stopped at, so trying again is "
                                           "cheap - it does not start over.",
                                           px(18), w * 0.7f),
                                  w * 0.5f, y, px(18), (Color){200, 208, 224, 255}, 26.0f * fs);
                y += 74.0f * fs;
                drawCenteredLines(wrapText(reason, px(18), w * 0.78f), w * 0.5f, y, px(18),
                                  (Color){240, 170, 120, 255}, 26.0f * fs);
            }

            if (!buildReportLog.empty())
            {
                drawCenteredLines({"the whole build is written to:"}, w * 0.5f, h - 132.0f * fs, px(15),
                                  (Color){104, 112, 130, 255}, 22.0f * fs);
                drawCenteredLines(wrapText(buildReportLog, px(15), w * 0.85f), w * 0.5f, h - 106.0f * fs, px(15),
                                  (Color){126, 134, 152, 255}, 22.0f * fs);
            }

            // A build that did not finish can be started again from here, which is
            // the whole point of saying so: the last release left a player with a
            // build that would not start and no way to find out why.
            const bool canRetry = !buildReportOk && jobGame >= 0 && jobGame < static_cast<int>(games.size()) &&
                                  !wizardImage.empty();
            if (canRetry)
                drawCenteredLines({usingController ? (pad.bottom + " tries again    -    " + pad.right + " closes this")
                                                   : "Enter tries again    -    any other key closes"},
                                  w * 0.5f, h - 54.0f * fs, px(16), (Color){120, 130, 150, 255}, 22.0f * fs);
            else
                drawCenteredLines({usingController ? (pad.bottom + " or " + pad.right + " closes this")
                                                   : "any key closes this"},
                                  w * 0.5f, h - 54.0f * fs, px(16), (Color){120, 130, 150, 255}, 22.0f * fs);
            EndDrawing();

            if (canRetry && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) ||
                             padPressedAny(GAMEPAD_BUTTON_RIGHT_FACE_DOWN)))
            {
                buildReport = false;
                startBuild(jobGame, wizardImage);
            }
            else if (GetKeyPressed() != 0 || IsMouseButtonPressed(MOUSE_BUTTON_LEFT) ||
                     padPressedAny(GAMEPAD_BUTTON_RIGHT_FACE_DOWN) || padPressedAny(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
                buildReport = false;
            continue;
        }
        // A fetch that failed, said the same way a build is. A clone that cannot
        // start is over in milliseconds, so the screen it was doing that on is gone
        // before it can be read - which is what "pressing U does nothing" was.
        // A notice: something the launcher was asked to do and cannot, with the
        // reason. Wrapped line by line, because these carry paragraphs and a command
        // that has to stand on a line of its own.
        // The answer arrived while the notice was waiting for it: say what it means.
        if (noticeBusy && wsl.state.load() != static_cast<int>(Wsl::Asking))
            showWslOutcome();

        if (notice)
        {
            const float w = static_cast<float>(GetScreenWidth());
            const float h = static_cast<float>(GetScreenHeight());
            const float fs = std::max(0.8f, std::min(2.5f, std::min(w / 1280.0f, h / 720.0f)));
            const auto px = [fs](float v) { return static_cast<int>(std::lround(v * fs)); };
            const auto wrapLines = [&](const std::string &text, int size, float width)
            {
                std::vector<std::string> out;
                size_t at = 0;
                while (true)
                {
                    const size_t end = text.find('\n', at);
                    const std::string line =
                        text.substr(at, end == std::string::npos ? std::string::npos : end - at);
                    const std::vector<std::string> wrapped = wrapText(line, size, width);
                    out.insert(out.end(), wrapped.begin(), wrapped.end());
                    if (end == std::string::npos)
                        break;
                    at = end + 1;
                }
                return out;
            };

            BeginDrawing();
            ClearBackground((Color){12, 13, 17, 255});
            drawCenteredLines({noticeTitle}, w * 0.5f, h * 0.5f - 130.0f * fs, px(28),
                              (Color){240, 200, 120, 255}, 36.0f * fs);
            drawCenteredLines(wrapLines(noticeText, px(18), w * 0.74f), w * 0.5f, h * 0.5f - 60.0f * fs, px(18),
                              (Color){200, 208, 224, 255}, 26.0f * fs);
            if (noticeBusy)
            {
                // Moving dots, because a still screen is what a crash looks like.
                const int dots = 1 + static_cast<int>(GetTime() * 2.0) % 3;
                drawCenteredLines({"waiting" + std::string(static_cast<size_t>(dots), '.')}, w * 0.5f,
                                  h - 54.0f * fs, px(18), (Color){240, 200, 120, 255}, 26.0f * fs);
            }
            else
            {
                drawCenteredLines({usingController ? (pad.bottom + " closes this") : "any key closes this"},
                                  w * 0.5f, h - 54.0f * fs, px(16), (Color){120, 130, 150, 255}, 22.0f * fs);
                if (noticeOffer != Offer::None)
                    drawCenteredLines({"W  -  let the launcher do it"}, w * 0.5f, h - 86.0f * fs, px(18),
                                      (Color){240, 200, 120, 255}, 26.0f * fs);
            }
            EndDrawing();

            if (!screenshotPath.empty())
            {
                TakeScreenshot(screenshotPath.c_str());
                std::fprintf(stderr, "[launcher] wrote %s\n", screenshotPath.c_str());
            }
            // The offer first: pressing its key must not also close the notice, because
            // what it does takes minutes and the notice is where the outcome is read.
            if (noticeOffer != Offer::None && IsKeyPressed(KEY_W))
            {
                const Offer which = noticeOffer;
                noticeOffer = Offer::None;
                const std::string script = updateStagingDirectory() +
                                           (which == Offer::InstallWsl ? "-install-wsl.cmd"
                                                                       : "-install-toolchain.cmd");
                std::ofstream out(script, std::ios::binary | std::ios::trunc);
                const bool written = static_cast<bool>(out);
                if (out)
                    out << offerScript(which);
                out.close();
                const bool started = written && platform::runInWindow(script, which == Offer::InstallWsl);
                std::fprintf(stderr, "[launcher] %s %s\n", started ? "offered" : "could not run", script.c_str());
                // Out of the way, and not merely polite: this install reports its own
                // progress, and a full screen launcher in front of the window is exactly
                // why none of it could be seen. A bar drawn here would be a guess - the
                // installer does not report a percentage - so the real one wins.
                if (started)
                    MinimizeWindow();
                noticeTitle = started ? "Running that for you" : "Could not run that";
                noticeText = started
                                 ? ("The launcher has got out of the way. Look for the window it opened - "
                                    "Windows may ask for permission first, and the window has the progress in "
                                    "it, and stays open when it finishes.\n\nWhen it is done, come back here "
                                    "and press B - the launcher works the rest out from what is there.")
                                 : ("The launcher could not start it. What it would have run is written to:\n\n" +
                                    script);
                notice = true;
                continue;
            }
            // Not within a quarter of a second of opening: the key that asked for this
            // screen is still in the queue, and a notice dismissed by the press that
            // opened it is another way of looking like nothing happened.
            const bool settled = GetTime() - noticeOpenedAt > 0.25f;
            if (settled && (GetKeyPressed() != 0 || IsMouseButtonPressed(MOUSE_BUTTON_LEFT) ||
                            padPressedAny(GAMEPAD_BUTTON_RIGHT_FACE_DOWN) ||
                            padPressedAny(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT)))
            {
                notice = false;
                noticeBusy = false;
                noticeOffer = Offer::None;
            }
            continue;
        }
        if (fetchReport)
        {
            const float w = static_cast<float>(GetScreenWidth());
            const float h = static_cast<float>(GetScreenHeight());
            const float fs = std::max(0.8f, std::min(2.5f, std::min(w / 1280.0f, h / 720.0f)));
            const auto px = [fs](float v) { return static_cast<int>(std::lround(v * fs)); };

            BeginDrawing();
            ClearBackground((Color){12, 13, 17, 255});
            drawCenteredLines({"Could not fetch the source"}, w * 0.5f, h * 0.5f - 90.0f * fs, px(28),
                              (Color){240, 170, 120, 255}, 36.0f * fs);
            drawCenteredLines(wrapText(fetchReportDetail, px(18), w * 0.78f), w * 0.5f, h * 0.5f - 20.0f * fs, px(18),
                              (Color){200, 208, 224, 255}, 26.0f * fs);
            drawCenteredLines({usingController ? (pad.bottom + " closes this") : "any key closes this"}, w * 0.5f,
                              h - 54.0f * fs, px(16), (Color){120, 130, 150, 255}, 22.0f * fs);
            EndDrawing();

            if (GetKeyPressed() != 0 || IsMouseButtonPressed(MOUSE_BUTTON_LEFT) ||
                padPressedAny(GAMEPAD_BUTTON_RIGHT_FACE_DOWN) || padPressedAny(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
                fetchReport = false;
            continue;
        }
        if (WindowShouldClose())
            break;
        if (checksDone)
        {
            // The background check has answered: fold it into the cards.
            checksDone = false;
            for (size_t i = 0; i < games.size(); ++i)
                if (!checkedRevisions[i].empty())
                    games[i].portRemote = checkedRevisions[i];
        }
        if (wizard == Wizard::Pick)
        {
            const float w = static_cast<float>(GetScreenWidth());
            const float h = static_cast<float>(GetScreenHeight());
            const float fs = std::max(0.8f, std::min(2.5f, std::min(w / 1280.0f, h / 720.0f)));
            const auto px = [fs](float v) { return static_cast<int>(std::lround(v * fs)); };

            sourceRects.assign(sources.size(), (Rectangle){0, 0, 0, 0});
            BeginDrawing();
            ClearBackground((Color){12, 13, 17, 255});

            const std::vector<std::string> heading = wrapText("Build " + games[selected].title, px(28), w * 0.8f);
            float y = h * 0.5f - 180.0f * fs;
            drawCenteredLines(heading, w * 0.5f, y, px(28), (Color){236, 240, 248, 255}, 36.0f * fs);
            y += static_cast<float>(heading.size()) * 36.0f * fs + 20.0f * fs;

            const float rowHeight = 42.0f * fs;
            if (!builder::supported())
            {
                drawCenteredLines(wrapText(builder::unsupportedNote(), px(18), w * 0.68f), w * 0.5f, y, px(18),
                                  (Color){170, 176, 192, 255}, 26.0f * fs);
            }
            else if (sources.empty())
            {
                drawCenteredLines(wrapText("No disc and no disc image found.", px(20), w * 0.7f), w * 0.5f, y, px(20),
                                  (Color){236, 240, 248, 255}, 28.0f * fs);
                y += 44.0f * fs;
                drawCenteredLines(wrapText("Put your .iso in a discs/ folder beside the launcher, or insert the "
                                           "disc, and press B again.", px(17), w * 0.7f),
                                  w * 0.5f, y, px(17), (Color){126, 134, 152, 255}, 24.0f * fs);
            }
            else
            {
                drawCenteredLines({"Where is your disc?"}, w * 0.5f, y, px(18), (Color){126, 134, 152, 255},
                                  24.0f * fs);
                y += 38.0f * fs;
                for (size_t i = 0; i < sources.size(); ++i)
                {
                    const bool picked = static_cast<int>(i) == sourcePick;
                    const float rowWidth = w * 0.56f;
                    const Rectangle row = {w * 0.5f - rowWidth * 0.5f, y - 8.0f * fs, rowWidth,
                                           rowHeight - 8.0f * fs};
                    sourceRects[i] = row;
                    if (picked)
                        DrawRectangleRounded(row, 0.35f, 4, (Color){40, 58, 92, 255});
                    const float width = static_cast<float>(MeasureText(sources[i].label.c_str(), px(19)));
                    DrawText(sources[i].label.c_str(), static_cast<int>(w * 0.5f - width * 0.5f), static_cast<int>(y),
                             px(19), picked ? WHITE : (Color){176, 184, 200, 255});
                    y += rowHeight;
                }
            }

            const bool choosable = builder::supported() && !sources.empty();
            const std::string startHint = usingController ? pad.bottom : "Cross";
            const std::string backHint = usingController ? pad.right : "Esc";
            drawCenteredLines({choosable ? ("Up / Down to choose    -    Click or " + startHint + " to start    -    " +
                                            backHint + " to go back")
                                         : (backHint + " to go back")},
                              w * 0.5f, h - 46.0f * fs, px(17), (Color){120, 130, 150, 255}, 24.0f * fs);
            EndDrawing();
            if (!screenshotPath.empty())
            {
                TakeScreenshot(screenshotPath.c_str());
                std::fprintf(stderr, "[launcher] wrote %s\n", screenshotPath.c_str());
                break;
            }

            const bool backPressed =
                IsKeyPressed(KEY_ESCAPE) ||
                padPressedAny(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);
            if (backPressed)
                wizard = Wizard::None;
            if (choosable)
            {
                const Vector2 mouse = GetMousePosition();
                if (mouse.x != lastWizardMouse.x || mouse.y != lastWizardMouse.y)
                {
                    lastWizardMouse = mouse;
                    for (size_t i = 0; i < sourceRects.size(); ++i)
                        if (CheckCollisionPointRec(mouse, sourceRects[i]))
                            sourcePick = static_cast<int>(i);
                }
                const int count = static_cast<int>(sources.size());
                if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_LEFT))
                    sourcePick = (sourcePick + count - 1) % count;
                if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_RIGHT))
                    sourcePick = (sourcePick + 1) % count;
                bool choose = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) ||
                              padPressedAny(GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && sourcePick >= 0 &&
                    sourcePick < static_cast<int>(sourceRects.size()) &&
                    CheckCollisionPointRec(mouse, sourceRects[sourcePick]))
                    choose = true;
                if (choose)
                {
                    wizardImage = sources[sourcePick].path;
                    wizardLabel = sources[sourcePick].label;
                    wizardFromPicker = true;
                    dialogChoice = 0; // Start
                    wizard = Wizard::Confirm;
                }
            }
            continue;
        }
        if (quitAsk)
        {
            const float w = static_cast<float>(GetScreenWidth());
            const float h = static_cast<float>(GetScreenHeight());
            const float fs = std::max(0.8f, std::min(2.5f, std::min(w / 1280.0f, h / 720.0f)));
            const auto px = [fs](float v) { return static_cast<int>(std::lround(v * fs)); };

            BeginDrawing();
            ClearBackground((Color){12, 13, 17, 255});

            const std::vector<std::string> heading = wrapText("Quit the launcher?", px(30), w * 0.8f);
            drawCenteredLines(heading, w * 0.5f, h * 0.5f - 150.0f * fs, px(30), (Color){236, 240, 248, 255},
                              38.0f * fs);
            drawCenteredLines(wrapText("The games stay where they are, and anything already built is kept.",
                                       px(17), w * 0.7f),
                              w * 0.5f, h * 0.5f - 90.0f * fs, px(17), (Color){126, 134, 152, 255}, 24.0f * fs);

            const Vector2 mouse = GetMousePosition();
            const float buttonW = 168.0f * fs, buttonH = 46.0f * fs, buttonGap = 34.0f * fs;
            const Rectangle yes = {w * 0.5f - buttonW - buttonGap * 0.5f, h * 0.5f + 20.0f * fs, buttonW, buttonH};
            const Rectangle no = {w * 0.5f + buttonGap * 0.5f, yes.y, buttonW, buttonH};
            const auto drawButton = [&](const Rectangle &rect, const char *text, bool highlighted)
            {
                DrawRectangleRounded(rect, 0.3f, 4, highlighted ? (Color){62, 92, 150, 255} : (Color){32, 38, 52, 255});
                const float textWidth = static_cast<float>(MeasureText(text, px(20)));
                DrawText(text, static_cast<int>(rect.x + (rect.width - textWidth) * 0.5f),
                         static_cast<int>(rect.y + (rect.height - static_cast<float>(px(20))) * 0.5f), px(20),
                         highlighted ? WHITE : (Color){186, 194, 210, 255});
            };
            drawButton(yes, "Quit", dialogChoice == 0);
            drawButton(no, "Not now", dialogChoice == 1);

            const std::string footer = "Left / Right to choose    -    " +
                                       std::string(usingController ? pad.bottom : "Cross") + " to confirm    -    " +
                                       (usingController ? pad.right : "Esc") + " to stay";
            drawCenteredLines({footer}, w * 0.5f, h - 46.0f * fs, px(17), (Color){120, 130, 150, 255}, 24.0f * fs);
            EndDrawing();

            if (!screenshotPath.empty())
            {
                TakeScreenshot(screenshotPath.c_str());
                std::fprintf(stderr, "[launcher] wrote %s\n", screenshotPath.c_str());
                break;
            }

            const int picked = dialogPick(yes, no);
            if (picked == 0)
                break;
            if (picked == 1)
                quitAsk = false;
            continue;
        }
        if (wizard == Wizard::Confirm)
        {
            const float w = static_cast<float>(GetScreenWidth());
            const float h = static_cast<float>(GetScreenHeight());
            const float fs = std::max(0.8f, std::min(2.5f, std::min(w / 1280.0f, h / 720.0f)));
            const auto px = [fs](float v) { return static_cast<int>(std::lround(v * fs)); };

            BeginDrawing();
            ClearBackground((Color){12, 13, 17, 255});

            const std::vector<std::string> heading =
                wrapText("Build " + games[selected].title + "?", px(28), w * 0.8f);
            float y = h * 0.5f - 190.0f * fs;
            drawCenteredLines(heading, w * 0.5f, y, px(28), (Color){236, 240, 248, 255}, 36.0f * fs);
            y += static_cast<float>(heading.size()) * 36.0f * fs + 30.0f * fs;

            const bool cross = willCrossCompile();
            const int minutes = builder::planMinutes(cross, wizardImage);
            struct Row
            {
                const char *label;
                std::string value;
            };
            const std::vector<Row> rows = {
                {"from", wizardLabel},
                {"for", cross ? "this machine, and for Windows" : "this machine"},
                // Rounded to five, because the estimate is not good to the minute.
                {"takes", "about " + std::to_string((minutes / 5) * 5) + " minutes"},
            };

            const float left = w * 0.5f - 260.0f * fs;
            for (const Row &row : rows)
            {
                DrawText(row.label, static_cast<int>(left), static_cast<int>(y), px(17),
                         (Color){126, 134, 152, 255});
                const std::vector<std::string> valueLines = wrapText(row.value, px(17), w * 0.52f);
                float rowY = y;
                for (const std::string &line : valueLines)
                {
                    DrawText(line.c_str(), static_cast<int>(left + 70.0f * fs), static_cast<int>(rowY), px(17),
                             (Color){196, 203, 216, 255});
                    rowY += 26.0f * fs;
                }
                y = rowY + 8.0f * fs;
            }

            y += 20.0f * fs;
            drawCenteredLines(wrapText("Nothing of the game is fetched: it is read from your disc, and built here.",
                                       px(16), w * 0.7f),
                              w * 0.5f, y, px(16), (Color){104, 112, 130, 255}, 22.0f * fs);

            // Two buttons, and only they start anything. Clicking the window at large
            // used to be enough, which is one stray click away from half an hour of
            // compiling - the exact mistake this screen exists to prevent.
            const Vector2 mouse = GetMousePosition();
            const float buttonW = 168.0f * fs, buttonH = 46.0f * fs, buttonGap = 34.0f * fs;
            const Rectangle startButton = {w * 0.5f - buttonW - buttonGap * 0.5f, h - 168.0f * fs, buttonW, buttonH};
            const Rectangle backButton = {w * 0.5f + buttonGap * 0.5f, startButton.y, buttonW, buttonH};
            const auto drawButton = [&](const Rectangle &rect, const char *text, bool highlighted)
            {
                DrawRectangleRounded(rect, 0.3f, 4, highlighted ? (Color){62, 92, 150, 255} : (Color){32, 38, 52, 255});
                const float textWidth = static_cast<float>(MeasureText(text, px(20)));
                DrawText(text, static_cast<int>(rect.x + (rect.width - textWidth) * 0.5f),
                         static_cast<int>(rect.y + (rect.height - static_cast<float>(px(20))) * 0.5f), px(20),
                         highlighted ? WHITE : (Color){186, 194, 210, 255});
            };
            drawButton(startButton, "Start", dialogChoice == 0);
            drawButton(backButton, "Not now", dialogChoice == 1);

            const std::string footer = "Left / Right to choose    -    " +
                                       std::string(usingController ? pad.bottom : "Cross") + " to confirm    -    " +
                                       (usingController ? pad.right : "Esc") + " to go back";
            drawCenteredLines({footer}, w * 0.5f, h - 46.0f * fs, px(17), (Color){120, 130, 150, 255}, 24.0f * fs);
            EndDrawing();

            if (!screenshotPath.empty())
            {
                TakeScreenshot(screenshotPath.c_str());
                std::fprintf(stderr, "[launcher] wrote %s\n", screenshotPath.c_str());
                break;
            }

            const int picked = dialogPick(startButton, backButton);
            if (picked == 0)
            {
                wizard = Wizard::None;
                startBuild(selected, wizardImage);
            }
            else if (picked == 1)
                wizard = wizardFromPicker ? Wizard::Pick : Wizard::None;
            continue;
        }

        const float width = static_cast<float>(GetScreenWidth());
        const float height = static_cast<float>(GetScreenHeight());
        const float delta = GetFrameTime();

        // ---------------------------------------------------------- layout
        // Everything scales with the display, so going fullscreen on a large monitor
        // fills it instead of leaving a small window's worth of content in the
        // middle. 1.0 at 1280x720, which is where the design was drawn.
        const float s = std::max(0.8f, std::min(2.5f, std::min(width / 1280.0f, height / 720.0f)));
        const float artWidth = kArtWidth * s;
        const float artHeight = kArtHeight * s;
        const float gap = kGap * s;
        const auto px = [s](float v) { return static_cast<int>(std::lround(v * s)); };

        const float totalWidth = artWidth * games.size() + gap * (games.size() - 1);
        float x = (width - totalWidth) * 0.5f;
        const float artY = (height - artHeight) * 0.5f - 26.0f * s; // room for two title lines below
        for (size_t i = 0; i < games.size(); ++i)
        {
            cards[i].bounds = (Rectangle){x, artY, artWidth, artHeight};
            x += artWidth + gap;
        }

        // The launcher's own update button, in the corner where the version used to
        // sit alone. It is the one control here that is not about a game: the
        // launcher cannot update itself, so it says what the repository has and
        // opens the page for you.
        const auto updateLabel = [&]() -> std::string
        {
            const std::string version = std::string("v") + PS2X_LAUNCHER_VERSION;
            switch (static_cast<LauncherCheck>(launcherCheck.load()))
            {
            case LauncherCheck::Running: return version + "  -  checking...";
            case LauncherCheck::UpToDate: return version + "  -  up to date";
            case LauncherCheck::Failed: return version + "  -  no answer, try again";
            case LauncherCheck::Available:
            {
                std::lock_guard<std::mutex> lock(launcherVersionMutex);
                const std::string offer = "launcher " + launcherVersion;
                // The click fetches the new one and puts it in place, so "opened in
                // your browser" only belongs here when that could not be done.
                if (updateFailed)
                {
                    if (updateOpened)
                        return offer + "  -  could not update it, opened the page";
                    if (updateRefused)
                        return offer + "  -  could not update it, see logs/launcher-update.log";
                    return offer + "  -  could not update it";
                }
                return offer + " is available  -  click to update";
            }
            default: return version + "  -  check for updates";
            }
        };
        // Measured from the label rather than fixed, because the label grows when
        // there is something to say. It sits in the top-right corner, level with the
        // subheading - the one row up there short enough to leave the corner free at
        // any window shape. (The title row and the footer row both reach it, so
        // neither will do.)
        const auto updateButton = [&]() -> Rectangle
        {
            const int size = px(15);
            const float boxWidth = static_cast<float>(MeasureText(updateLabel().c_str(), size)) + 28.0f * s;
            const float boxHeight = 32.0f * s;
            return {width - boxWidth - 16.0f * s, artY - 50.0f * s, boxWidth, boxHeight};
        };

        // ---------------------------------------------------------- input
        bool activate = false;
        const Vector2 mouse = GetMousePosition();
        const bool mouseMoved = mouse.x != lastMouse.x || mouse.y != lastMouse.y;
        lastMouse = mouse;

        if (mouseMoved)
            for (size_t i = 0; i < games.size(); ++i)
                if (pointIn(cards[i].bounds, mouse))
                    selected = static_cast<int>(i);

        if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A))
            selected = (selected + static_cast<int>(games.size()) - 1) % static_cast<int>(games.size());
        if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D))
            selected = (selected + 1) % static_cast<int>(games.size());
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            activate = true;
        // Typing or clicking means the keyboard and mouse are the ones in use. This
        // comes before the pad is read, so a frame with both favours the pad.
        if (mouseMoved || IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsKeyPressed(KEY_LEFT) ||
            IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_A) || IsKeyPressed(KEY_D) || IsKeyPressed(KEY_ENTER) ||
            IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_F11) || IsKeyPressed(KEY_M) ||
            IsKeyPressed(KEY_U) || IsKeyPressed(KEY_B) || IsKeyPressed(KEY_C))
            usingController = false;

        // A controller gets the same three actions as the keyboard: the bottom face
        // button plays, the left one fetches, the top one builds, and the right one
        // backs out or stops a build. Square/X and Triangle/Y are the ones the pad in
        // your hand calls them.
        // Whichever slot was touched last is the pad in hand, so a controller that is
        // not the first device on the system still drives the menu.
        for (int slot = 0; slot < 4; ++slot)
        {
            if (!IsGamepadAvailable(slot))
                continue;
            const bool touched =
                IsGamepadButtonPressed(slot, GAMEPAD_BUTTON_LEFT_FACE_LEFT) ||
                IsGamepadButtonPressed(slot, GAMEPAD_BUTTON_LEFT_FACE_RIGHT) ||
                IsGamepadButtonPressed(slot, GAMEPAD_BUTTON_RIGHT_FACE_DOWN) ||
                IsGamepadButtonPressed(slot, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT) ||
                IsGamepadButtonPressed(slot, GAMEPAD_BUTTON_RIGHT_FACE_LEFT) ||
                IsGamepadButtonPressed(slot, GAMEPAD_BUTTON_RIGHT_FACE_UP) ||
                IsGamepadButtonPressed(slot, GAMEPAD_BUTTON_MIDDLE_RIGHT) ||
                std::fabs(GetGamepadAxisMovement(slot, GAMEPAD_AXIS_LEFT_X)) > 0.5f;
            if (touched)
            {
                activePad = slot;
                usingController = true;
            }
        }

        const int gamepad = activePad;
        const bool padFetch = gamepad >= 0 && IsGamepadButtonPressed(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT);
        const bool padBuild = gamepad >= 0 && IsGamepadButtonPressed(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP);
        // A press that cannot be honoured says why. Doing nothing at all is what "B
        // does nothing" was, and it reads as a broken key rather than as a missing
        // toolchain or a source that has not been fetched.
        const auto explain = [&](const std::string &title, const std::string &text, Offer offer = Offer::None)
        { setNotice(title, text, offer, false); };
        if (IsKeyPressed(KEY_B) || padBuild)
        {
            if (!games[selected].portConfigured)
                explain("There is nothing to build from",
                        "This game has no port= line in launcher.ini, so the launcher does not know where its "
                        "source is. The README's launcher.ini section has the shape of it.");
            else if (games[selected].portLocal.empty())
                explain("Fetch the source first",
                        "Press U (or Square/X) to fetch this game's source. A build needs it, and the card "
                        "says which revision is here once it has one.");
            else if (!builder::supported())
            {
                // Building is done by Linux, and a Windows player should not have to be
                // told a command line. The Linux launcher ships in this folder, so this
                // starts it under WSL on the build for this game: the same launcher.ini,
                // the same port/, and under WSL it cross-compiles the Windows port -
                // which is the build this launcher then plays.
                // Asked in the background if it is not already known, because asking
                // means starting Linux. Nothing here may block the thread that draws.
                if (wsl.state.load() == static_cast<int>(Wsl::Ready))
                    showWslOutcome(); // known already: straight to it, no wait at all
                else
                {
                    askAboutWsl();
                    if (wsl.state.load() == static_cast<int>(Wsl::Asking))
                        setNotice("Asking WSL about this PC",
                                  "Windows has to start Linux to answer, and the first time that takes a few "
                                  "seconds. Nothing is wrong - the launcher is waiting for it, and the dots "
                                  "below are moving.\n\nIt only asks once: after this, pressing B goes "
                                  "straight to the build.",
                                  Offer::None, true);
                    else
                        showWslOutcome();
                }
            }
            // A configured disc= skips the picker entirely. Otherwise look for one and
            // say what to do when there is none, rather than opening an empty list.
            else if (!games[selected].disc.empty())
            {
                wizardImage = games[selected].disc;
                wizardLabel = games[selected].disc;
                wizardFromPicker = false;
                dialogChoice = 0; // Start
                wizard = Wizard::Confirm;
            }
            else
            {
                sources = builder::findSources(base);
                sourcePick = 0;
                wizard = Wizard::Pick;
            }
        }
        if ((IsKeyPressed(KEY_U) || padFetch) && games[selected].portConfigured)
            startJob(Job::Fetch, selected, fetchCommand(games[selected]), true);

        // The launcher's own update button. Click it, or press C / L1. When a newer
        // release is known the same action opens its page instead of asking again.
        {
            const Rectangle updateRect = updateButton();
            const bool asked = IsKeyPressed(KEY_C) || padPressedAny(GAMEPAD_BUTTON_LEFT_TRIGGER_1) ||
                               (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, updateRect));
            if (asked)
            {
                if (launcherCheck.load() == static_cast<int>(LauncherCheck::Available))
                {
                    std::string newest;
                    {
                        std::lock_guard<std::mutex> lock(launcherVersionMutex);
                        newest = launcherVersion;
                    }
                    // Fetch the new one and put it in place. Handing the player a page
                    // is only what happens when that cannot be done at all.
                    if (!startLauncherUpdate(newest))
                    {
                        const std::string url = releasePageUrl(settings.updateRepo);
                        const bool opened = platform::openUrl(url);
                        updateOpened = opened;
                        updateRefused = !opened;
                        // A browser that opens behind a fullscreen launcher is
                        // indistinguishable from a click that did nothing, so the
                        // launcher gets out of the way and says what it did either way.
                        if (opened)
                            MinimizeWindow();
                        std::fprintf(stderr, "[launcher] %s %s\n", opened ? "opened" : "could not open", url.c_str());
                    }
                }
                else
                {
                    updateOpened = false;
                    updateRefused = false;
                    startLauncherCheck();
                }
            }
        }
        if ((IsKeyPressed(KEY_F11) || padPressedAny(GAMEPAD_BUTTON_MIDDLE_LEFT)) && screenshotPath.empty())
        {
            ToggleFullscreen();
            // Leaving fullscreen leaves the window at the monitor's size, so put it
            // back to the size the layout was drawn for.
            if (!IsWindowFullscreen())
                SetWindowSize(1280, 720);
        }

        if (gamepad >= 0)
        {
            if (IsGamepadButtonPressed(gamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT))
                selected = (selected + static_cast<int>(games.size()) - 1) % static_cast<int>(games.size());
            if (IsGamepadButtonPressed(gamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
                selected = (selected + 1) % static_cast<int>(games.size());
            if (IsGamepadButtonPressed(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN) ||
                IsGamepadButtonPressed(gamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT))
                activate = true;

            stickCooldown -= delta;
            const float stick = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_X);
            if (stickCooldown <= 0.0f && std::fabs(stick) > 0.5f)
            {
                selected = (selected + (stick < 0.0f ? -1 : 1) + static_cast<int>(games.size())) %
                           static_cast<int>(games.size());
                stickCooldown = 0.25f;
            }
        }

        // A click only counts on the card itself, so clicking empty space does
        // nothing.
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            activate = pointIn(cards[selected].bounds, mouse);

        // ---------------------------------------------------------- update
        for (size_t i = 0; i < games.size(); ++i)
        {
            const bool isSelected = static_cast<int>(i) == selected;
            const float target = isSelected ? 1.0f : 0.0f;
            cards[i].highlight += (target - cards[i].highlight) * std::min(1.0f, delta * 12.0f);
            cards[i].scale = 1.0f + cards[i].highlight * 0.04f;

            // The theme swells in with the highlight and fades out the same way,
            // so a hover never cuts to silence.
            Game &game = games[i];
            if (!game.themeLoaded)
                continue;
            const float musicTarget = (isSelected && !musicMuted) ? musicVolume : 0.0f;
            game.themeVolume += (musicTarget - game.themeVolume) * std::min(1.0f, delta * kMusicFadeSpeed);
            if (musicTarget == 0.0f && game.themeVolume < 0.001f)
            {
                game.themeVolume = 0.0f;
                // Nothing left to hear: stop and rewind it, and leave it stopped,
                // so a theme nobody is hovering costs nothing at all.
                if (IsMusicStreamPlaying(game.theme))
                    StopMusicStream(game.theme);
            }
            SetMusicVolume(game.theme, game.themeVolume);
            UpdateMusicStream(game.theme);
        }

        // ---------------------------------------------------------- music
        if ((IsKeyPressed(KEY_M) || padPressedAny(GAMEPAD_BUTTON_RIGHT_THUMB)) && themesLoaded > 0)
        {
            musicMuted = !musicMuted;
            if (!musicMuted)
                startTheme(games[selected]); // silent until the fade brings it up
        }
        // Hovering the other card swaps the theme: the new one starts from its
        // opening, the old one fades away and is stopped once it is inaudible.
        if (selected != previousSelected)
        {
            if (!musicMuted)
                startTheme(games[selected]);
            previousSelected = selected;
        }

        if (activate && games[selected].available)
        {
            silenceThemes(games); // the game brings its own music
            startJob(Job::Game, selected, launchCommand(games[selected]), false);
        }

        // ---------------------------------------------------------- draw
        BeginDrawing();
        ClearBackground((Color){18, 20, 26, 255});

        // A soft vignette so the art sits in something rather than on a flat wall.
        for (int i = 0; i < 6; ++i)
        {
            const float t = static_cast<float>(i) / 6.0f;
            DrawRectangleGradientV(0, static_cast<int>(height * t), static_cast<int>(width),
                                   static_cast<int>(height / 6.0f) + 1,
                                   (Color){24, 27, 36, 255}, (Color){14, 15, 20, 255});
        }

        // The title is descriptive and carries no mark of its own: it says what
        // this is and what it launches, and nothing more. The games are named on
        // their own cards, which is the only place their names are needed.
        const char *heading = "THE STORIES COLLECTION";
        const float headingWidth = static_cast<float>(MeasureText(heading, px(44)));
        DrawText(heading, static_cast<int>((width - headingWidth) * 0.5f), static_cast<int>(artY - 100.0f * s), px(44),
                 (Color){236, 240, 248, 255});
        const char *subheading = "PlayStation 2 recompilations";
        const float subheadingWidth = static_cast<float>(MeasureText(subheading, px(28)));
        DrawText(subheading, static_cast<int>((width - subheadingWidth) * 0.5f), static_cast<int>(artY - 48.0f * s),
                 px(28), (Color){240, 200, 90, 255});

        for (size_t i = 0; i < games.size(); ++i)
        {
            const Game &game = games[i];
            Card &card = cards[i];
            const bool isSelected = static_cast<int>(i) == selected;

            const float growW = card.bounds.width * (card.scale - 1.0f);
            const float growH = card.bounds.height * (card.scale - 1.0f);
            const Rectangle art = (Rectangle){card.bounds.x - growW * 0.5f, card.bounds.y - growH * 0.5f,
                                              card.bounds.width + growW, card.bounds.height + growH};

            // A glow behind the selected card, then a frame that brightens with
            // the selection - the two together make the choice unmistakable.
            if (card.highlight > 0.01f)
            {
                for (int ring = 5; ring >= 1; --ring)
                {
                    const float spread = (6.0f + static_cast<float>(ring) * 7.0f) * s;
                    const unsigned char alpha = static_cast<unsigned char>(14 * card.highlight);
                    DrawRectangleRounded((Rectangle){art.x - spread, art.y - spread, art.width + spread * 2,
                                                     art.height + spread * 2},
                                         0.03f, 4, (Color){90, 150, 255, alpha});
                }
            }
            const unsigned char frame = static_cast<unsigned char>(40 + 190 * card.highlight);
            DrawRectangleRounded((Rectangle){art.x - 5 * s, art.y - 5 * s, art.width + 10 * s, art.height + 10 * s},
                                 0.02f, 4, (Color){frame, frame, frame, 255});

            if (game.artLoaded)
            {
                // Fit inside the card without distorting the cover's aspect.
                const float scale = std::min(art.width / game.artTexture.width,
                                             art.height / game.artTexture.height);
                const float drawWidth = game.artTexture.width * scale;
                const float drawHeight = game.artTexture.height * scale;
                const Rectangle source = {0, 0, static_cast<float>(game.artTexture.width),
                                          static_cast<float>(game.artTexture.height)};
                const Rectangle destination = {art.x + (art.width - drawWidth) * 0.5f,
                                               art.y + (art.height - drawHeight) * 0.5f, drawWidth, drawHeight};
                const Color tint = game.available
                                       ? Color{static_cast<unsigned char>(180 + 75 * card.highlight), 255, 255, 255}
                                       : Color{110, 115, 130, 255};
                DrawTexturePro(game.artTexture, source, destination, (Vector2){0, 0}, 0.0f, tint);
            }
            else
            {
                DrawRectangleRec(art, (Color){30, 34, 44, 255});
                const char *placeholder = "BOX ART";
                const float placeholderWidth = static_cast<float>(MeasureText(placeholder, px(28)));
                DrawText(placeholder, static_cast<int>(art.x + (art.width - placeholderWidth) * 0.5f),
                         static_cast<int>(art.y + art.height * 0.5f - 14.0f * s), px(28), (Color){70, 78, 96, 255});
                // Wrapped to the card and clipped to it. The path shown is the one the
                // config has rather than the resolved one, and even then a long one
                // folds: a hint that spills over its neighbour reads as a bug rather
                // than as help.
                const std::vector<std::string> hintLines =
                    wrapText("drop " + game.artDisplay + " here", px(15), art.width - 28.0f * s);
                float hintY = art.y + art.height * 0.5f + 22.0f * s;
                BeginScissorMode(static_cast<int>(art.x), static_cast<int>(art.y), static_cast<int>(art.width),
                                 static_cast<int>(art.height));
                for (const std::string &line : hintLines)
                {
                    const float lineWidth = static_cast<float>(MeasureText(line.c_str(), px(15)));
                    DrawText(line.c_str(), static_cast<int>(art.x + (art.width - lineWidth) * 0.5f),
                             static_cast<int>(hintY), px(15), (Color){58, 64, 80, 255});
                    hintY += 20.0f * s;
                }
                EndScissorMode();
            }

            // The title under the card, wrapped so a long one can never run into
            // its neighbour.
            Color titleColor = (Color){108, 114, 130, 255};
            if (game.available)
                titleColor = isSelected ? WHITE : (Color){196, 203, 216, 255};
            const std::vector<std::string> titleLines = wrapText(game.title, px(20), artWidth + 30.0f * s);
            drawCenteredLines(titleLines, art.x + art.width * 0.5f, art.y + art.height + 16.0f * s, px(20), titleColor,
                              24.0f * s);

            // Where this game's source stands, when the launcher is the one fetching
            // it. It says what to press, so the footer does not have to.
            const std::string portLine =
                portSummary(game, usingController ? pad.left : "U", usingController ? pad.top : "B");
            if (!portLine.empty())
            {
                const bool onThePlayer = game.portLocal.empty() ||
                                         (!game.portRemote.empty() && game.portRemote != game.portLocal);
                const float portY = art.y + art.height + 16.0f * s +
                                    static_cast<float>(titleLines.size()) * 24.0f * s + 8.0f * s;
                drawCenteredLines({portLine}, art.x + art.width * 0.5f, portY, px(15),
                                  onThePlayer ? (Color){240, 200, 90, 255} : (Color){104, 112, 130, 255},
                                  20.0f * s);
            }

            if (!game.available)
            {
                // A band rather than a wash: the notice stays readable whatever
                // the cover underneath happens to be.
                DrawRectangleRec(art, (Color){8, 9, 12, 150});
                const float bandHeight = 56.0f * s;
                const float bandY = art.y + art.height * 0.5f - bandHeight * 0.5f;
                DrawRectangle(static_cast<int>(art.x), static_cast<int>(bandY), static_cast<int>(art.width),
                              static_cast<int>(bandHeight), (Color){12, 13, 17, 255});
                DrawRectangle(static_cast<int>(art.x), static_cast<int>(bandY), static_cast<int>(art.width), px(2),
                              (Color){240, 200, 90, 200});
                DrawRectangle(static_cast<int>(art.x), static_cast<int>(bandY + bandHeight - px(2)),
                              static_cast<int>(art.width), px(2), (Color){240, 200, 90, 200});
                const char *soon = "COMING SOON!";
                const int soonSize = px(28);
                const float soonWidth = static_cast<float>(MeasureText(soon, soonSize));
                DrawText(soon, static_cast<int>(art.x + (art.width - soonWidth) * 0.5f),
                         static_cast<int>(bandY + bandHeight * 0.5f - soonSize * 0.5f), soonSize,
                         (Color){240, 200, 90, 255});
            }
            else if (card.highlight < 0.99f)
            {
                // Dim whatever is not chosen, so the choice reads at a glance.
                DrawRectangleRec(art, (Color){8, 9, 12, static_cast<unsigned char>((1.0f - card.highlight) * 120.0f)});
            }
        }

        // Footer: how to drive it.
        std::string footer = "Left / Right or the stick to choose    -    ";
        footer += games[selected].available ? ("Click or " + (usingController ? pad.bottom : "Enter") + " to play")
                                            : "this one is still being built";
        footer += "    -    " + std::string(usingController ? pad.select : "F11") + ": fullscreen";
        if (themesLoaded > 0)
        {
            const std::string muteKey = usingController ? pad.mute : "M";
            footer += musicMuted ? ("    -    " + muteKey + ": music off") : ("    -    " + muteKey + ": mute");
        }
        footer += "    -    " + std::string(usingController ? pad.bumper : "C") + ": updates";
        footer += "    -    " + std::string(usingController ? pad.right : "Esc") + ": quit";
        const float footerWidth = static_cast<float>(MeasureText(footer.c_str(), px(18)));
        DrawText(footer.c_str(), static_cast<int>((width - footerWidth) * 0.5f), static_cast<int>(height - 46.0f * s),
                 px(18), (Color){120, 130, 150, 255});

        // Nothing in music/ yet: say where a theme would go. It disappears for
        // good the moment there is one, so it teaches without ever nagging.
        if (musicVolume > 0.0f && themesLoaded == 0)
        {
            const char *hint = "each game's theme can play here - drop an mp3 into music/ (see music/README.txt)";
            const float hintWidth = static_cast<float>(MeasureText(hint, px(16)));
            DrawText(hint, static_cast<int>((width - hintWidth) * 0.5f), static_cast<int>(height - 22.0f * s), px(16),
                     (Color){84, 92, 110, 255});
        }

        // The update button, where the version used to sit alone: the version, and
        // the one way to ask whether there is a newer launcher. It lights up when
        // there is one, because a player who never presses it would never learn -
        // and it is quiet otherwise, because it is the last thing that should be in
        // the way.
        {
            const Rectangle rect = updateButton();
            const bool available = launcherCheck.load() == static_cast<int>(LauncherCheck::Available);
            const bool hot = CheckCollisionPointRec(GetMousePosition(), rect);
            const Color background = available ? (Color){58, 48, 20, 255}
                                               : (hot ? (Color){32, 38, 52, 255} : (Color){20, 23, 31, 255});
            DrawRectangleRounded(rect, 0.4f, 4, background);
            const std::string label = updateLabel();
            const int size = px(15);
            const float textWidth = static_cast<float>(MeasureText(label.c_str(), size));
            const Color colour = available ? (Color){240, 200, 90, 255}
                                           : (hot ? (Color){176, 184, 200, 255} : (Color){96, 104, 122, 255});
            DrawText(label.c_str(), static_cast<int>(rect.x + (rect.width - textWidth) * 0.5f),
                     static_cast<int>(rect.y + (rect.height - static_cast<float>(size)) * 0.5f), size, colour);
            if (updateRefused)
            {
                const std::string url = releasePageUrl(settings.updateRepo);
                const int urlWidth = MeasureText(url.c_str(), px(14));
                DrawText(url.c_str(), static_cast<int>(rect.x + (rect.width - urlWidth) * 0.5f),
                         static_cast<int>(rect.y + rect.height + 6.0f * s), px(14), (Color){150, 158, 176, 255});
            }
        }

        // Lights back up when a game has closed, rather than snapping to it.
        if (menuLit < 1.0f)
        {
            menuLit = std::min(1.0f, menuLit + delta * 2.5f);
            DrawRectangle(0, 0, static_cast<int>(width), static_cast<int>(height),
                          (Color){0, 0, 0, static_cast<unsigned char>((1.0f - menuLit) * 255.0f)});
        }

        EndDrawing();

        if (!screenshotPath.empty())
        {
            TakeScreenshot(screenshotPath.c_str());
            std::fprintf(stderr, "[launcher] wrote %s\n", screenshotPath.c_str());
            break;
        }
        // Esc or Circle asks rather than quits.
        if (IsKeyPressed(KEY_ESCAPE) || padPressedAny(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
        {
            dialogChoice = 1; // Not now: this screen exists to catch accidents
            quitAsk = true;
        }
    }

    if (jobThread.joinable())
        jobThread.join(); // the loop only leaves once nothing is running
    if (checkThread.joinable())
        checkThread.join();
    if (launcherCheckThread.joinable())
        launcherCheckThread.join();

    for (Game &game : games)
    {
        if (game.themeLoaded)
            UnloadMusicStream(game.theme);
        if (game.artLoaded)
            UnloadTexture(game.artTexture);
    }
    if (audioReady)
        CloseAudioDevice(); // raylib wants the audio device closed before the window
    CloseWindow();

    // The launcher's own update: the new files are staged and a script is ready to
    // put them in place, so all that is left is to get out of its way. The window is
    // closed first, because on Windows a running program's files cannot be replaced
    // at all and the script waits for this process to end.
    if (updateHandOver)
        platform::runScriptDetached(updateScriptPath());

    return 0;
}
