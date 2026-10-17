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
#include <mutex>
#include <string>
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

    // The revision of a fetched tree, or "" when there is not one to read.
    std::string readRevision(const std::string &directory)
    {
        if (directory.empty())
            return {};
        std::mutex discard;
        std::string line;
        const std::string command = "git -C \"" + directory + "\" rev-parse HEAD 2>&1";
        return runCaptured(command, discard, line) == 0 ? line : std::string{};
    }

    std::string portRefOf(const Game &game) { return game.portRef.empty() ? "main" : game.portRef; }

    // A shallow clone the first time. After that, take the ref again and reset onto
    // it: the source is not the player's to edit, and this is what makes an update
    // deterministic rather than a merge that can fail next to their changes.
    std::string fetchCommand(const Game &game)
    {
        if (game.portLocal.empty())
            return "git clone --depth 1 --branch \"" + portRefOf(game) + "\" \"" + game.portUrl + "\" \"" +
                   game.portDir + "\" 2>&1";
        return "git -C \"" + game.portDir + "\" fetch --depth 1 origin \"" + portRefOf(game) + "\" 2>&1 && "
               "git -C \"" + game.portDir + "\" reset --hard FETCH_HEAD 2>&1";
    }

    // What the remote has for the ref, without touching the working tree.
    std::string checkCommand(const Game &game)
    {
        return "git ls-remote \"" + game.portUrl + "\" \"" + portRefOf(game) + "\" 2>&1";
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
    };

    PadNames padNames()
    {
        const char *raw = IsGamepadAvailable(0) ? GetGamepadName(0) : nullptr;
        std::string name = raw != nullptr ? raw : "";
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name.find("xbox") != std::string::npos || name.find("xinput") != std::string::npos ||
            name.find("steam") != std::string::npos)
            return {"A", "B", "X", "Y"};
        if (name.find("switch") != std::string::npos || name.find("nintendo") != std::string::npos)
            return {"B", "A", "Y", "X"}; // Nintendo numbers them the other way round
        return {"Cross", "Circle", "Square", "Triangle"};
    }

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
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            screenshotPath = argv[++i];
        else if (std::strcmp(argv[i], "--select") == 0 && i + 1 < argc)
            startSelection = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--launch") == 0 && i + 1 < argc)
            launchIndex = std::atoi(argv[++i]);
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
        // with none still says exactly which file it is waiting for.
        game.art = resolve(base, game.art.empty() ? ("art/" + game.id + ".png") : game.art);
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

    SetConfigFlags(FLAG_WINDOW_HIGHDPI | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(1280, 720, "The Stories Collection - Launcher");
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

    // One background job at a time: the game, or fetching a port's source. Waiting
    // in a thread rather than blocking the loop is what lets the launcher keep
    // answering the window manager while it works, and notice the moment it ends.
    enum class Job { None, Game, Fetch, Build };
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
        jobThread = std::thread([&, kind, command, captureOutput]
        {
            jobStatus = captureOutput ? runCaptured(command, jobMutex, jobDetail) : std::system(command.c_str());
            jobKind = static_cast<int>(Job::None);
        });
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
        std::fprintf(stderr, "[launcher] building %s from %s\n", games[index].id.c_str(), image.c_str());
        jobThread = std::thread([&, index, image]
        {
            const builder::Recipe recipe = builder::readRecipe(games[index].portDir);
            if (!recipe.ok)
            {
                {
                    std::lock_guard<std::mutex> lock(jobMutex);
                    jobDetail = recipe.error;
                }
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
            };
            callbacks.onLine = [&](const std::string &line)
            {
                // Onto the console as well as the screen: a build that fails needs
                // something to read afterwards, and the screen is gone by then.
                std::fprintf(stderr, "[build]   %s\n", line.c_str());
                std::lock_guard<std::mutex> lock(jobMutex);
                jobDetail = line;
            };
            builder::Targets targets;
            targets.windows = buildCrosses;
            targets.toolchain = settings.toolchain;
            buildOk = builder::run(games[index].portDir, image, recipe, targets, callbacks);
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

    const int gamepad = 0; // the pad the launcher reads; it only ever uses one

    while (true)
    {
        // Which names this pad's buttons go by, read once a frame so a controller
        // plugged in or swapped mid-session is picked up.
        const PadNames pad = padNames();

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
                (IsGamepadAvailable(gamepad) && IsGamepadButtonPressed(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT));
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
                    if (!buildOk.load())
                        std::fprintf(stderr, "[launcher] the build for %s did not finish\n",
                                     games[index].id.c_str());
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
                    std::fprintf(stderr, "[launcher] fetching the %s source failed (status %d)\n",
                                 games[index].id.c_str(), status);
                }
            }
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
                (IsGamepadAvailable(gamepad) && IsGamepadButtonPressed(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT));
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
                              (IsGamepadAvailable(gamepad) &&
                               IsGamepadButtonPressed(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN));
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && sourcePick >= 0 &&
                    sourcePick < static_cast<int>(sourceRects.size()) &&
                    CheckCollisionPointRec(mouse, sourceRects[sourcePick]))
                    choose = true;
                if (choose)
                {
                    wizardImage = sources[sourcePick].path;
                    wizardLabel = sources[sourcePick].label;
                    wizardFromPicker = true;
                    wizard = Wizard::Confirm;
                }
            }
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
            const auto drawButton = [&](const Rectangle &rect, const char *text)
            {
                const bool over = CheckCollisionPointRec(mouse, rect);
                DrawRectangleRounded(rect, 0.3f, 4, over ? (Color){62, 92, 150, 255} : (Color){32, 38, 52, 255});
                const float textWidth = static_cast<float>(MeasureText(text, px(20)));
                DrawText(text, static_cast<int>(rect.x + (rect.width - textWidth) * 0.5f),
                         static_cast<int>(rect.y + (rect.height - static_cast<float>(px(20))) * 0.5f), px(20),
                         over ? WHITE : (Color){186, 194, 210, 255});
            };
            drawButton(startButton, "Start");
            drawButton(backButton, "Not now");

            const std::string footer = (usingController ? pad.bottom : "Cross") + std::string(" to start    -    ") +
                                       (usingController ? pad.right : "Esc") + " to go back";
            drawCenteredLines({footer}, w * 0.5f, h - 46.0f * fs, px(17), (Color){120, 130, 150, 255}, 24.0f * fs);
            EndDrawing();

            if (!screenshotPath.empty())
            {
                TakeScreenshot(screenshotPath.c_str());
                std::fprintf(stderr, "[launcher] wrote %s\n", screenshotPath.c_str());
                break;
            }

            const bool start =
                IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) ||
                (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, startButton)) ||
                (IsGamepadAvailable(gamepad) && IsGamepadButtonPressed(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN));
            const bool back =
                IsKeyPressed(KEY_ESCAPE) ||
                (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, backButton)) ||
                (IsGamepadAvailable(gamepad) && IsGamepadButtonPressed(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT));
            if (back)
                wizard = wizardFromPicker ? Wizard::Pick : Wizard::None;
            else if (start)
            {
                wizard = Wizard::None;
                startBuild(selected, wizardImage);
            }
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
            IsKeyPressed(KEY_U) || IsKeyPressed(KEY_B))
            usingController = false;

        // A controller gets the same three actions as the keyboard: the bottom face
        // button plays, the left one fetches, the top one builds, and the right one
        // backs out or stops a build. Square/X and Triangle/Y are the ones the pad in
        // your hand calls them.
        bool padFetch = false;
        bool padBuild = false;
        if (IsGamepadAvailable(gamepad))
        {
            padFetch = IsGamepadButtonPressed(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT);
            padBuild = IsGamepadButtonPressed(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP);
            if (padFetch || padBuild || IsGamepadButtonPressed(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN) ||
                IsGamepadButtonPressed(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT) ||
                IsGamepadButtonPressed(gamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT) ||
                IsGamepadButtonPressed(gamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT) ||
                IsGamepadButtonPressed(gamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT) ||
                std::fabs(GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_X)) > 0.5f)
                usingController = true;
        }
        if (IsKeyPressed(KEY_B) || padBuild)
        {
            if (!games[selected].portConfigured || games[selected].portLocal.empty() || !builder::supported())
            {
                // Nothing to build from yet: the card already says so.
            }
            // A configured disc= skips the picker entirely. Otherwise look for one and
            // say what to do when there is none, rather than opening an empty list.
            else if (!games[selected].disc.empty())
            {
                wizardImage = games[selected].disc;
                wizardLabel = games[selected].disc;
                wizardFromPicker = false;
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
        if (IsKeyPressed(KEY_F11) && screenshotPath.empty())
        {
            ToggleFullscreen();
            // Leaving fullscreen leaves the window at the monitor's size, so put it
            // back to the size the layout was drawn for.
            if (!IsWindowFullscreen())
                SetWindowSize(1280, 720);
        }

        if (IsGamepadAvailable(gamepad))
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
        if (IsKeyPressed(KEY_M) && themesLoaded > 0)
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
                const char *hint = TextFormat("drop %s here", game.art.c_str());
                const float hintWidth = static_cast<float>(MeasureText(hint, px(16)));
                DrawText(hint, static_cast<int>(art.x + (art.width - hintWidth) * 0.5f),
                         static_cast<int>(art.y + art.height * 0.5f + 22.0f * s), px(16), (Color){58, 64, 80, 255});
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
        footer += "    -    F11: fullscreen";
        if (themesLoaded > 0)
            footer += musicMuted ? "    -    M: music off" : "    -    M: mute";
        footer += "    -    Esc: quit";
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

        // The version, quiet in the corner. It is the first thing anyone needs when
        // they ask about the launcher, and the last thing that should be in the way.
        {
            const std::string version = std::string("v") + PS2X_LAUNCHER_VERSION;
            const float versionWidth = static_cast<float>(MeasureText(version.c_str(), px(14)));
            DrawText(version.c_str(), static_cast<int>(width - versionWidth - 20.0f * s),
                     static_cast<int>(height - 22.0f * s), px(14), (Color){70, 78, 96, 255});
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
        if (IsKeyPressed(KEY_ESCAPE))
            break;
    }

    if (jobThread.joinable())
        jobThread.join(); // the loop only leaves once nothing is running
    if (checkThread.joinable())
        checkThread.join();

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
    return 0;
}
