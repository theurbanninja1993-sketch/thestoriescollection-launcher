// ps2x-launcher - the front end for the ported GTA PS2 games.
//
// Phase 1: the game menu. Two cards with the player's own box art, mouse and
// controller navigation, click / Cross to launch. The games and their paths come
// from launcher.ini next to the executable, so nothing is hard-coded and a game
// marked available=0 shows a COMING SOON notice instead of launching.
//
// The menu is drawn directly with raylib (no ImGui yet); the builder wizard in
// phase 2 is where forms and widgets will actually earn their keep.
#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(PS2X_LAUNCHER_WINDOWS)
#include <windows.h>
#else
#include <unistd.h>
#include <limits.h>
#endif

namespace
{
    // ---------------------------------------------------------------- paths

    // The folder the launcher is running from, so a portable layout works
    // wherever it is dropped: launcher.ini, art/ and games/ all resolve here.
    std::string launcherDirectory()
    {
#if defined(PS2X_LAUNCHER_WINDOWS)
        char buffer[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, buffer, MAX_PATH) == 0)
            return ".";
        std::string path = buffer;
#else
        char buffer[PATH_MAX] = {};
        const ssize_t length = readlink("/proc/self/exe", buffer, sizeof buffer - 1);
        if (length <= 0)
            return ".";
        std::string path(buffer, static_cast<size_t>(length));
#endif
        const size_t slash = path.find_last_of("/\\");
        return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
    }

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
        bool available = true;
        Texture2D artTexture{};
        bool artLoaded = false;
    };

    // A deliberately small key=value reader: one "[game:<id>]" section per game,
    // in file order, so the menu order is whatever the file says.
    std::vector<Game> loadGames(const std::string &path)
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
        while (std::fgets(line, sizeof line, file) != nullptr)
        {
            std::string text = trim(line);
            if (text.empty() || text[0] == '#' || text[0] == ';')
                continue;
            if (text.front() == '[' && text.back() == ']')
            {
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
            if (current == nullptr)
                continue;
            const size_t equals = text.find('=');
            if (equals == std::string::npos)
                continue;
            const std::string key = trim(text.substr(0, equals));
            const std::string value = trim(text.substr(equals + 1));
            if (key == "title")
                current->title = value;
            else if (key == "exe")
                current->exe = value;
            else if (key == "data")
                current->data = value;
            else if (key == "art")
                current->art = value;
            else if (key == "available")
                current->available = std::atoi(value.c_str()) != 0;
        }
        std::fclose(file);
        return games;
    }

    // ---------------------------------------------------------------- launch

    void launchGame(const Game &game)
    {
        const std::string exe = game.exe;
        const std::string workingDirectory = directoryOf(exe);
        // The shell handles the quoting on both platforms (cmd and sh both
        // accept && and double quotes), and the game wants to be run from its
        // own folder so a portable config next to it is the one it reads.
        std::string command = "cd \"" + workingDirectory + "\" && \"" + exe + "\"";
        if (!game.data.empty())
            command += " \"" + game.data + "\"";
        std::fprintf(stderr, "[launcher] %s\n", command.c_str());
        const int status = std::system(command.c_str());
        if (status != 0)
            std::fprintf(stderr, "[launcher] %s exited with status %d\n", game.id.c_str(), status);
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
    const std::string directory = launcherDirectory();
    // launcher.ini normally sits beside the executable (that is the release
    // layout); falling back to the working directory keeps development simple,
    // where the binary lives in build/ but the config does not.
    std::string configPath = resolve(directory, "launcher.ini");
    if (!FileExists(configPath.c_str()))
        configPath = "launcher.ini";
    // Relative paths inside the config (art, games/) are relative to the config
    // itself, so the whole folder can be moved anywhere as one unit.
    const std::string base = directoryOf(configPath);

    std::vector<Game> games = loadGames(configPath);
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
        game.exe = resolve(base, game.exe);
        game.data = resolve(base, game.data);
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
        launchGame(games[launchIndex]);
        return 0;
    }

    SetConfigFlags(FLAG_WINDOW_HIGHDPI | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(1280, 720, "GTA PS2 - Launcher");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);

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

    int selected = startSelection >= 0 && startSelection < static_cast<int>(games.size()) ? startSelection : 0;
    std::vector<Card> cards(games.size());
    // Open with the selection already shown rather than fading in from nothing.
    cards[selected].highlight = 1.0f;

    // A mouse that is merely *resting* over a card must not keep stealing the
    // selection from the keyboard or the controller, so hovering only counts
    // when the pointer actually moves.
    Vector2 lastMouse = GetMousePosition();

    // Stick navigation needs a nudge: hold a direction and it steps every so
    // often rather than every frame.
    float stickCooldown = 0.0f;

    while (!WindowShouldClose())
    {
        const float width = static_cast<float>(GetScreenWidth());
        const float height = static_cast<float>(GetScreenHeight());
        const float delta = GetFrameTime();

        // ---------------------------------------------------------- layout
        const float totalWidth = kArtWidth * games.size() + kGap * (games.size() - 1);
        float x = (width - totalWidth) * 0.5f;
        const float artY = (height - kArtHeight) * 0.5f - 26.0f; // room for two title lines below
        for (size_t i = 0; i < games.size(); ++i)
        {
            cards[i].bounds = (Rectangle){x, artY, kArtWidth, kArtHeight};
            x += kArtWidth + kGap;
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

        const int gamepad = 0;
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
        }

        if (activate && games[selected].available)
            launchGame(games[selected]);

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

        const char *heading = "GRAND THEFT AUTO:";
        const float headingWidth = static_cast<float>(MeasureText(heading, 44));
        DrawText(heading, static_cast<int>((width - headingWidth) * 0.5f), static_cast<int>(artY - 100.0f), 44,
                 (Color){236, 240, 248, 255});
        const char *subheading = "The Stories Collection";
        const float subheadingWidth = static_cast<float>(MeasureText(subheading, 28));
        DrawText(subheading, static_cast<int>((width - subheadingWidth) * 0.5f), static_cast<int>(artY - 48.0f), 28,
                 (Color){240, 200, 90, 255});

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
                    const float spread = 6.0f + static_cast<float>(ring) * 7.0f;
                    const unsigned char alpha = static_cast<unsigned char>(14 * card.highlight);
                    DrawRectangleRounded((Rectangle){art.x - spread, art.y - spread, art.width + spread * 2,
                                                     art.height + spread * 2},
                                         0.03f, 4, (Color){90, 150, 255, alpha});
                }
            }
            const unsigned char frame = static_cast<unsigned char>(40 + 190 * card.highlight);
            DrawRectangleRounded((Rectangle){art.x - 5, art.y - 5, art.width + 10, art.height + 10}, 0.02f, 4,
                                 (Color){frame, frame, frame, 255});

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
                const float placeholderWidth = static_cast<float>(MeasureText(placeholder, 28));
                DrawText(placeholder, static_cast<int>(art.x + (art.width - placeholderWidth) * 0.5f),
                         static_cast<int>(art.y + art.height * 0.5f - 14.0f), 28, (Color){70, 78, 96, 255});
                const char *hint = TextFormat("drop %s here", game.art.c_str());
                const float hintWidth = static_cast<float>(MeasureText(hint, 16));
                DrawText(hint, static_cast<int>(art.x + (art.width - hintWidth) * 0.5f),
                         static_cast<int>(art.y + art.height * 0.5f + 22.0f), 16, (Color){58, 64, 80, 255});
            }

            // The title under the card, wrapped so a long one can never run into
            // its neighbour.
            Color titleColor = (Color){108, 114, 130, 255};
            if (game.available)
                titleColor = isSelected ? WHITE : (Color){196, 203, 216, 255};
            const std::vector<std::string> titleLines = wrapText(game.title, 20, kArtWidth + 30.0f);
            drawCenteredLines(titleLines, art.x + art.width * 0.5f, art.y + art.height + 16.0f, 20, titleColor, 24.0f);

            if (!game.available)
            {
                // A band rather than a wash: the notice stays readable whatever
                // the cover underneath happens to be.
                DrawRectangleRec(art, (Color){8, 9, 12, 150});
                const float bandHeight = 56.0f;
                const float bandY = art.y + art.height * 0.5f - bandHeight * 0.5f;
                DrawRectangle(static_cast<int>(art.x), static_cast<int>(bandY), static_cast<int>(art.width),
                              static_cast<int>(bandHeight), (Color){12, 13, 17, 255});
                DrawRectangle(static_cast<int>(art.x), static_cast<int>(bandY), static_cast<int>(art.width), 2,
                              (Color){240, 200, 90, 200});
                DrawRectangle(static_cast<int>(art.x), static_cast<int>(bandY + bandHeight - 2),
                              static_cast<int>(art.width), 2, (Color){240, 200, 90, 200});
                const char *soon = "COMING SOON!";
                const float soonSize = 28.0f;
                const float soonWidth = static_cast<float>(MeasureText(soon, static_cast<int>(soonSize)));
                DrawText(soon, static_cast<int>(art.x + (art.width - soonWidth) * 0.5f),
                         static_cast<int>(bandY + bandHeight * 0.5f - soonSize * 0.5f), static_cast<int>(soonSize),
                         (Color){240, 200, 90, 255});
            }
            else if (card.highlight < 0.99f)
            {
                // Dim whatever is not chosen, so the choice reads at a glance.
                DrawRectangleRec(art, (Color){8, 9, 12, static_cast<unsigned char>((1.0f - card.highlight) * 120.0f)});
            }
        }

        // Footer: how to drive it.
        const char *footer = games[selected].available
                                 ? "Left / Right or the stick to choose    -    Click or Cross to play    -    Esc to quit"
                                 : "Left / Right or the stick to choose    -    this one is still being built";
        const float footerWidth = static_cast<float>(MeasureText(footer, 18));
        DrawText(footer, static_cast<int>((width - footerWidth) * 0.5f), static_cast<int>(height - 46.0f), 18,
                 (Color){120, 130, 150, 255});

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

    for (Game &game : games)
        if (game.artLoaded)
            UnloadTexture(game.artTexture);
    CloseWindow();
    return 0;
}
