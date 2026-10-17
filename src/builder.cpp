#include "builder.h"

#include "platform.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#if !defined(_WIN32)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

// The process environment, for building a child's own. Has to be at file scope:
// declared inside the anonymous namespace below it names a different symbol.
extern char **environ;
#endif

namespace fs = std::filesystem;

namespace builder
{
    namespace
    {
        // Runs something however the scope is left, including every early return.
        struct OnExit
        {
            std::function<void()> work;
            ~OnExit()
            {
                if (work)
                    work();
            }
        };

        // ------------------------------------------------------------ small things

        std::string trim(const std::string &text)
        {
            const size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
                return {};
            const size_t last = text.find_last_not_of(" \t\r\n");
            return text.substr(first, last - first + 1);
        }

        std::string join(const std::string &a, const std::string &b) { return a + "/" + b; }

        int cores()
        {
            const unsigned int n = std::thread::hardware_concurrency();
            return n == 0 ? 4 : static_cast<int>(n);
        }

        std::string readFile(const std::string &path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return {};
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        }

        bool writeFile(const std::string &path, const std::string &text)
        {
            std::ofstream out(path, std::ios::binary);
            if (!out)
                return false;
            out << text;
            return out.good();
        }

        // The step runner. Everything each step needs is an argument vector and an
        // environment, and it is invoked directly - no shell, so nothing has to be
        // quoted and the environment of the child is exactly what is asked for.
        using OnLine = std::function<void(const std::string &)>;

#if !defined(_WIN32)
        // execve does not search PATH and cmake is not at a fixed place.
        std::string onPath(const std::string &name)
        {
            if (name.find('/') != std::string::npos)
                return name;
            const char *path = std::getenv("PATH");
            if (path == nullptr)
                return name;
            std::stringstream parts(path);
            std::string dir;
            while (std::getline(parts, dir, ':'))
            {
                const std::string candidate = join(dir.empty() ? "." : dir, name);
                if (access(candidate.c_str(), X_OK) == 0)
                    return candidate;
            }
            return name;
        }

        int runTool(const std::vector<std::string> &argv, const std::string &cwd,
                    const std::vector<std::string> &environment, OnLine onLine, std::atomic<int> &childPid)
        {
            if (argv.empty())
                return -1;

            int fds[2];
            if (pipe(fds) != 0)
                return -1;

            const pid_t pid = fork();
            if (pid < 0)
            {
                close(fds[0]);
                close(fds[1]);
                return -1;
            }

            if (pid == 0)
            {
                // Its own process group, so stopping it takes the whole tree: a
                // compiler is not one process.
                setpgid(0, 0);
                if (!cwd.empty() && chdir(cwd.c_str()) != 0)
                    _exit(126);
                dup2(fds[1], STDOUT_FILENO);
                dup2(fds[1], STDERR_FILENO);
                close(fds[0]);
                close(fds[1]);

                std::vector<char *> args;
                for (const std::string &a : argv)
                    args.push_back(const_cast<char *>(a.c_str()));
                args.push_back(nullptr);

                // The environment is the parent's with ours laid over the top. Ours
                // are removed from the inherited copy rather than appended, because
                // a duplicate is read unpredictably.
                std::vector<std::string> merged;
                if (!environment.empty())
                {
                    std::vector<std::string> ours;
                    for (const std::string &kv : environment)
                        ours.push_back(kv.substr(0, kv.find('=')));
                    for (char **e = environ; e != nullptr && *e != nullptr; ++e)
                    {
                        const std::string entry = *e;
                        const std::string key = entry.substr(0, entry.find('='));
                        if (std::find(ours.begin(), ours.end(), key) == ours.end())
                            merged.push_back(entry);
                    }
                    merged.insert(merged.end(), environment.begin(), environment.end());
                }
                std::vector<char *> env;
                for (std::string &e : merged)
                    env.push_back(const_cast<char *>(e.c_str()));
                env.push_back(nullptr);

                const std::string tool = onPath(argv[0]);
                if (merged.empty())
                    execv(tool.c_str(), args.data());
                else
                    execve(tool.c_str(), args.data(), env.data());
                _exit(127);
            }

            setpgid(pid, pid);
            childPid = static_cast<int>(pid);
            close(fds[1]);

            FILE *stream = fdopen(fds[0], "r");
            if (stream != nullptr)
            {
                char line[4096];
                while (std::fgets(line, sizeof line, stream) != nullptr)
                {
                    const std::string text = trim(line);
                    if (!text.empty() && onLine)
                        onLine(text);
                }
                fclose(stream);
            }

            int status = 0;
            waitpid(pid, &status, 0);
            childPid = 0;
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
#else
        int runTool(const std::vector<std::string> &, const std::string &, const std::vector<std::string> &, OnLine,
                    std::atomic<int> &)
        {
            return -1; // there is no Windows build to drive yet; see supported()
        }
#endif

        // Runs a tool and keeps everything it printed, so the result can be read as
        // well as shown.
        int runCapturing(const std::vector<std::string> &argv, const std::string &cwd, const OnLine &onLine,
                         std::atomic<int> &childPid, std::string &output)
        {
            output.clear();
            return runTool(argv, cwd, {},
                           [&](const std::string &line)
                           {
                               output += line;
                               output += '\n';
                               if (onLine)
                                   onLine(line);
                           },
                           childPid);
        }

        // The hash out of ps2x_iso: "    boot hash: <hash>" from a disc, or
        // "<hash>  <path>" from hashfile.
        std::string parseHash(const std::string &output)
        {
            const std::string marker = "boot hash:";
            const size_t at = output.find(marker);
            if (at != std::string::npos)
            {
                size_t i = at + marker.size();
                while (i < output.size() && std::isspace(static_cast<unsigned char>(output[i])))
                    ++i;
                size_t j = i;
                while (j < output.size() && std::isxdigit(static_cast<unsigned char>(output[j])))
                    ++j;
                return output.substr(i, j - i);
            }
            const size_t end = output.find_first_of(" \t\r\n");
            return end == std::string::npos ? trim(output) : output.substr(0, end);
        }

        // The manifest names an input and an output; the player's machine decides
        // what they are, so they are rewritten rather than hand-edited. Replacing
        // only the first of each keeps this to the keys that mean it.
        std::string rewriteConfig(const std::string &text, const std::string &elf, const std::string &outDir,
                                  const std::string &csv)
        {
            std::istringstream in(text);
            std::ostringstream result;
            std::string line;
            bool didInput = false, didOutput = false, didMap = false;
            while (std::getline(in, line))
            {
                const std::string body = trim(line);
                bool replaced = false;
                for (const auto &pair : {std::make_pair(std::string("input"), std::make_pair(elf, &didInput)),
                                         std::make_pair(std::string("output"), std::make_pair(outDir + "/", &didOutput)),
                                         std::make_pair(std::string("ghidra_output"), std::make_pair(csv, &didMap))})
                {
                    const std::string &key = pair.first;
                    if (*pair.second.second || body.rfind(key, 0) != 0)
                        continue;
                    const size_t eq = body.find('=');
                    if (eq == std::string::npos || !trim(body.substr(key.size(), eq - key.size())).empty())
                        continue;
                    result << key << " = \"" << pair.second.first << "\"\n";
                    *pair.second.second = true;
                    replaced = true;
                    break;
                }
                if (!replaced)
                    result << line << "\n";
            }
            return result.str();
        }

        // Copies a whole tree. std::filesystem::copy's directory semantics are
        // fiddly about whether it copies into or over, so this walks it instead.
        void copyTree(const std::string &from, const std::string &to)
        {
            std::error_code ec;
            fs::create_directories(to, ec);
            for (const fs::directory_entry &entry : fs::recursive_directory_iterator(from, ec))
            {
                const fs::path target = fs::path(to) / fs::relative(entry.path(), from, ec);
                if (entry.is_directory())
                    fs::create_directories(target, ec);
                else if (entry.is_regular_file())
                {
                    fs::create_directories(target.parent_path(), ec);
                    fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, ec);
                }
            }
        }

        // The generated code is installed by content, not by timestamp: the build
        // keys off timestamps, so rewriting 10,000 identical files would force a
        // full rebuild every single time.
        void installDirectory(const std::string &from, const std::string &to, const char *extension, size_t &copied,
                              size_t &kept)
        {
            std::error_code ec;
            fs::create_directories(to, ec);
            for (const fs::directory_entry &entry : fs::directory_iterator(from, ec))
            {
                if (!entry.is_regular_file() || entry.path().extension() != extension)
                    continue;
                const fs::path target = fs::path(to) / entry.path().filename();
                std::error_code compare;
                if (fs::exists(target, compare) && fs::file_size(target, compare) == fs::file_size(entry.path(), compare)
                    && readFile(target.string()) == readFile(entry.path().string()))
                {
                    ++kept;
                    continue;
                }
                fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, ec);
                ++copied;
            }
        }
    }

    // ------------------------------------------------------------------ sources

    std::vector<Source> findSources(const std::string &configDir)
    {
        std::vector<Source> sources;
        std::error_code ec;

        // Optical drives. Found here rather than by asking ps2x_iso, because the
        // tools are built by the first step of the build: the picker has to work
        // before anything exists.
        for (const std::string &drive : platform::opticalDrives())
            sources.push_back({drive, drive + "  (disc drive)", false});

        // Disc images, in the place a player would put them and beside the launcher.
        const std::vector<fs::path> where = {fs::path(configDir) / "discs", fs::path(configDir)};
        for (const fs::path &folder : where)
        {
            if (!fs::is_directory(folder, ec))
                continue;
            std::vector<fs::path> images;
            for (const fs::directory_entry &entry : fs::directory_iterator(folder, ec))
            {
                const std::string ext = entry.path().extension().string();
                if (entry.is_regular_file()
                    && (ext == ".iso" || ext == ".ISO" || ext == ".bin" || ext == ".BIN"))
                    images.push_back(entry.path());
            }
            std::sort(images.begin(), images.end());
            for (const fs::path &image : images)
                sources.push_back({image.string(), image.filename().string(), false});
        }

        return sources;
    }

    // ------------------------------------------------------------------- recipe

    Recipe readRecipe(const std::string &portDir)
    {
        Recipe recipe;
        std::error_code ec;

        // games/<id>/game.toml - one game to a port repository.
        std::vector<fs::path> manifests;
        const fs::path games = fs::path(portDir) / "games";
        if (fs::is_directory(games, ec))
            for (const fs::directory_entry &entry : fs::directory_iterator(games, ec))
                if (fs::exists(entry.path() / "game.toml", ec))
                    manifests.push_back(entry.path() / "game.toml");
        if (manifests.empty())
        {
            recipe.error = "no game.toml under " + games.string() + " - is this the port's source?";
            return recipe;
        }
        if (manifests.size() > 1)
        {
            recipe.error = "more than one game in " + games.string() + "; the wizard expects one";
            return recipe;
        }
        const fs::path manifest = manifests.front();

        // Flat sections of key = value, which is all the manifest uses.
        std::istringstream in(readFile(manifest.string()));
        std::string section;
        std::string line;
        while (std::getline(in, line))
        {
            const std::string body = trim(line);
            if (body.empty() || body[0] == '#')
                continue;
            if (body.front() == '[' && body.back() == ']')
            {
                section = trim(body.substr(1, body.size() - 2));
                continue;
            }
            const size_t eq = body.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = trim(body.substr(0, eq));
            std::string value = trim(body.substr(eq + 1));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                value = value.substr(1, value.size() - 2);

            if (section == "game" && key == "boot_file")
                recipe.bootFile = value;
            else if (section == "game" && key == "boot_hash_xxh3_128")
                recipe.bootHash = value;
            else if (section == "recompiler" && key == "config")
                recipe.config = (manifest.parent_path() / value).string();
            else if (section == "recompiler" && key == "function_map")
                recipe.functionMap = (manifest.parent_path() / value).string();
            else if (section == "vu1" && key == "learn_passes")
                recipe.learnPasses = std::max(1, std::atoi(value.c_str()));
            else if (section == "vu1" && key == "learn_pad_script")
                recipe.learnPadScript = value;
            else if (section == "vu1" && key == "learn_frames")
                recipe.learnFrames = value;
        }

        if (recipe.bootFile.empty() || recipe.bootHash.empty())
        {
            recipe.error = manifest.string() + " does not say which file on the disc is the game";
            return recipe;
        }
        if (!fs::exists(recipe.config, ec) || !fs::exists(recipe.functionMap, ec))
        {
            recipe.error = "the manifest's config or function map is missing from the source";
            return recipe;
        }
        recipe.ok = true;
        return recipe;
    }

    // -------------------------------------------------------------------- steps

    const char *stepName(int step)
    {
        switch (step)
        {
        case StepTools: return "Tools";
        case StepToolchain: return "Windows cross-compiler";
        case StepCheck: return "Check the disc";
        case StepExtract: return "Extract";
        case StepRecompile: return "Recompile";
        case StepInstall: return "Install";
        case StepCompile: return "Compile the port";
        case StepVu1: return "Learn the VU1 microprograms";
        case StepWindows: return "Build and package Windows";
        default: return "";
        }
    }

    const char *stepNote(int step)
    {
        switch (step)
        {
        case StepTools: return "The first three stages call tools that a fresh download does not have yet.";
        case StepToolchain:
            return "llvm-mingw, fetched once (88 MB). The Windows build is cross-compiled, so its compiler runs here.";
        case StepCheck: return "The disc's own boot file is hashed and compared with the one the source expects.";
        case StepExtract: return "Every file is pulled off the disc. This is the long one.";
        case StepRecompile: return "The game's ELF becomes C++, which is what the port is: a translation of it.";
        case StepInstall: return "The generated code is copied into the runtime, without rewriting what has not changed.";
        case StepCompile: return "The port itself is built. This is the other long one.";
        case StepVu1: return "The VU1 microprograms live on the disc, so the game is run to record which ones it asks for.";
        case StepWindows:
            return "After the microprograms, and never before them - the Windows link needs them. Then everything the "
                   "game needs is gathered into one folder to copy across.";
        default: return "";
        }
    }

    std::vector<int> plan(bool windows)
    {
        std::vector<int> steps = {StepTools};
        if (windows)
            steps.push_back(StepToolchain);
        for (int step : {StepCheck, StepExtract, StepRecompile, StepInstall, StepCompile, StepVu1})
            steps.push_back(step);
        if (windows)
            steps.push_back(StepWindows);
        return steps;
    }

    bool runningUnderWsl()
    {
#if defined(_WIN32)
        return false;
#else
        // WSL says so in the kernel release string, and nothing else does.
        const std::string release = readFile("/proc/sys/kernel/osrelease");
        std::string lower = release;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find("microsoft") != std::string::npos || lower.find("wsl") != std::string::npos;
#endif
    }

    std::string windowsOutDir(const std::string &portDir) { return join(portDir, "dist/windows"); }

    bool supported()
    {
#if defined(_WIN32)
        return false;
#else
        return true;
#endif
    }

    const char *unsupportedNote()
    {
        return "Building is not set up on Windows yet. The Windows port is cross-compiled from Linux and has "
               "never been built on Windows, so there is no build to drive from here. Fetching and playing "
               "both work; see the README.";
    }

    std::string builtRunner(const std::string &portDir)
    {
        return join(portDir, "build/ps2xRuntime/ps2EntryRunner");
    }

    std::string builtGameDir(const std::string &portDir) { return join(portDir, "build/disc"); }

    // ------------------------------------------------------------------ running

    bool run(const std::string &portDir, const std::string &image, const Recipe &recipe, const Targets &targets,
             const Callbacks &cb)
    {
        const std::string buildDir = join(portDir, "build"); // the same tree build.sh uses
        const std::string buildWin = join(portDir, "build-win"); // the cross-compile's own tree
        const std::string work = buildDir;
        const bool imageIsFolder = fs::is_directory(image);
        const std::string discDir = imageIsFolder ? image : join(work, "disc");
        const std::string generated = join(work, "generated");
        const std::string learn = join(work, "vu1_learn");
        const std::string vu1Dir = join(work, "vu1");
        const std::string ps2Recomp = join(buildDir, "ps2xRecomp/ps2_recomp");
        const std::string ps2Iso = join(buildDir, "ps2xBuilder/ps2x_iso");
        const std::string vu1Recomp = join(buildDir, "ps2xRuntime/vu1_recomp");
        const std::string runner = builtRunner(portDir);
        const std::string bootPath = join(discDir, recipe.bootFile);

        std::atomic<int> owned{0};
        std::atomic<int> &pid = cb.childPid != nullptr ? *cb.childPid : owned;

        // Stopping a build can catch a compiler or the linker mid-write, and cmake
        // judges by timestamps: a half-written output is *newer* than its inputs, so
        // it is never rebuilt and the build quietly stays broken. Two things say
        // that happened - a marker this file left behind, or a runner that is not
        // executable - and either way the next build cleans the port's targets
        // first rather than trusting what is there.
        const std::string marker = join(buildDir, ".ps2x-build-interrupted");
        std::error_code probe;
        bool interrupted = fs::exists(marker, probe);
        if (!interrupted && fs::exists(runner, probe) && !platform::fileIsExecutable(runner))
            interrupted = true;
        if (interrupted)
        {
            fs::remove(marker, probe);
            fs::remove(runner, probe);
            if (cb.onLine)
                cb.onLine("an earlier build was interrupted; rebuilding what it touched");
        }
        const auto wasCancelled = [&] { return cb.keepGoing != nullptr && !cb.keepGoing(); };
        OnExit markIfStopped{[&]
                             {
                                 if (wasCancelled())
                                     writeFile(marker, "interrupted\n");
                             }};

        std::error_code ec;
        // The tools a step calls have to be runnable, and the message when they are
        // not is much clearer than what the platform says about it.
        auto need = [&](const std::string &path) -> bool
        {
            if (fs::exists(path, ec))
                return true;
            if (cb.onLine)
                cb.onLine("missing " + path + " - the tools step should have built it");
            return false;
        };
        auto step = [&](int id) -> bool
        {
            if (cb.onStep)
                cb.onStep(id);
            return cb.keepGoing == nullptr || cb.keepGoing();
        };
        auto shell = [&](const std::vector<std::string> &argv, const std::string &cwd) -> int
        { return runTool(argv, cwd, {}, cb.onLine, pid); };

        // ---------------------------------------------------------------- tools
        if (!step(StepTools))
            return false;
        fs::create_directories(buildDir, ec);
        if (shell({"cmake", "-S", portDir, "-B", buildDir, "-DCMAKE_BUILD_TYPE=Release"}, "") != 0)
            return false;
        {
            std::vector<std::string> argv = {"cmake", "--build", buildDir, "--target",
                                             "ps2_recomp", "ps2x_iso", "vu1_recomp", "-j", std::to_string(cores())};
            if (shell(argv, "") != 0)
                return false;
        }
        for (const std::string &tool : {ps2Recomp, ps2Iso, vu1Recomp})
            if (!need(tool))
                return false;

        // ------------------------------------------------------------ toolchain
        // A Windows build needs a compiler that runs here and targets there, and one
        // is fetched once into the source and reused. Anything already on this machine
        // wins over fetching: one named, one the environment points at, the one an
        // existing build-win was configured with, then our own copy.
        const bool cross = targets.windows;
        std::string toolchainRoot = targets.toolchain;
        if (cross)
        {
            if (!step(StepToolchain))
                return false;

            const std::string asset = "llvm-mingw-20241217-ucrt-ubuntu-20.04-x86_64";
            const std::string toolchains = join(portDir, "toolchains");
            auto works = [&](const std::string &root)
            { return !root.empty() && fs::exists(join(root, "bin/x86_64-w64-mingw32-clang++"), ec); };

            if (!works(toolchainRoot))
            {
                const char *environment = std::getenv("LLVM_MINGW_ROOT");
                if (environment != nullptr && works(environment))
                    toolchainRoot = environment;
            }
            if (!works(toolchainRoot))
            {
                const std::string cache = readFile(join(buildWin, "CMakeCache.txt"));
                const size_t at = cache.find("LLVM_MINGW_ROOT:");
                const size_t equals = at == std::string::npos ? std::string::npos : cache.find('=', at);
                if (equals != std::string::npos)
                {
                    const size_t end = cache.find('\n', equals);
                    const std::string cached =
                        trim(cache.substr(equals + 1, end == std::string::npos ? std::string::npos : end - equals - 1));
                    if (works(cached))
                        toolchainRoot = cached;
                }
            }
            if (!works(toolchainRoot) && works(join(toolchains, asset)))
                toolchainRoot = join(toolchains, asset);

            if (!works(toolchainRoot))
            {
                if (cb.onLine)
                    cb.onLine("fetching the cross-compiler, 88 MB, once");
                fs::create_directories(toolchains, ec);
                const std::string archive = join(toolchains, asset + ".tar.xz");
                const std::string url =
                    "https://github.com/mstorsjo/llvm-mingw/releases/download/20241217/" + asset + ".tar.xz";
                if ((!fs::exists(archive, ec) && shell({"curl", "-fL", "--retry", "3", "-o", archive, url}, "") != 0))
                {
                    if (cb.onLine)
                        cb.onLine("could not download the cross-compiler");
                    return false;
                }
                if (cb.onLine)
                    cb.onLine("unpacking the cross-compiler, which takes a moment");
                if (shell({"tar", "-xJf", archive, "-C", toolchains}, "") != 0)
                    return false;
                toolchainRoot = join(toolchains, asset);
                if (!works(toolchainRoot))
                {
                    if (cb.onLine)
                        cb.onLine("the cross-compiler did not unpack where expected");
                    return false;
                }
            }
            if (cb.onLine)
                cb.onLine("cross-compiler: " + toolchainRoot);

            // Configured here rather than at the end because it takes minutes and
            // nothing about it depends on the game having been compiled.
            if (shell({"cmake", "-S", portDir, "-B", buildWin, "-DCMAKE_BUILD_TYPE=Release",
                       "-DCMAKE_TOOLCHAIN_FILE=" + join(portDir, "cmake/toolchains/llvm-mingw-x86_64.cmake"),
                       "-DLLVM_MINGW_ROOT=" + toolchainRoot},
                      "") != 0)
                return false;
        }

        // ---------------------------------------------------------------- check
        if (!step(StepCheck))
            return false;
        std::string output;
        int status = 0;
        if (imageIsFolder)
        {
            if (!fs::exists(bootPath, ec))
            {
                if (cb.onLine)
                    cb.onLine("no " + recipe.bootFile + " in " + image);
                return false;
            }
            status = runCapturing({ps2Iso, "hashfile", bootPath}, "", cb.onLine, pid, output);
        }
        else
        {
            status = runCapturing({ps2Iso, "check", image}, "", cb.onLine, pid, output);
        }
        if (status != 0)
            return false;
        const std::string hash = parseHash(output);
        if (hash != recipe.bootHash)
        {
            if (cb.onLine)
                cb.onLine("this disc is not the one this source is for");
            return false;
        }

        // -------------------------------------------------------------- extract
        if (!step(StepExtract))
            return false;
        if (!imageIsFolder)
        {
            fs::remove_all(discDir, ec);
            fs::create_directories(discDir, ec);
            if (shell({ps2Iso, "extract", image, discDir}, "") != 0)
                return false;
        }

        // ------------------------------------------------------------ recompile
        if (!step(StepRecompile))
            return false;
        {
            const std::string shipConfig = readFile(recipe.config);
            if (shipConfig.empty())
            {
                if (cb.onLine)
                    cb.onLine("cannot read " + recipe.config);
                return false;
            }
            const std::string workConfig = join(work, "vcs_clean.toml");
            if (!writeFile(workConfig, rewriteConfig(shipConfig, bootPath, generated, recipe.functionMap)))
            {
                if (cb.onLine)
                    cb.onLine("cannot write " + workConfig);
                return false;
            }
            fs::remove_all(generated, ec);
            fs::create_directories(generated, ec);
            if (shell({ps2Recomp, workConfig}, portDir) != 0)
                return false;
            size_t files = 0;
            for (const fs::directory_entry &entry : fs::directory_iterator(generated, ec))
                if (entry.is_regular_file() && entry.path().extension() == ".cpp")
                    ++files;
            if (files == 0)
            {
                if (cb.onLine)
                    cb.onLine("the recompiler produced nothing");
                return false;
            }
            if (cb.onLine)
                cb.onLine("generated " + std::to_string(files) + " files");
        }

        // -------------------------------------------------------------- install
        if (!step(StepInstall))
            return false;
        {
            size_t copied = 0, kept = 0;
            installDirectory(generated, join(portDir, "ps2xRuntime/include"), ".h", copied, kept);
            installDirectory(generated, join(portDir, "ps2xRuntime/src/runner"), ".cpp", copied, kept);
            if (cb.onLine)
                cb.onLine("copied " + std::to_string(copied) + ", left " + std::to_string(kept) + " alone");
        }

        // -------------------------------------------------------------- compile
        if (!step(StepCompile))
            return false;
        {
            std::vector<std::string> configure = {"cmake", "-S", portDir, "-B", buildDir, "-DCMAKE_BUILD_TYPE=Release",
                                                  "-DPS2X_VU1_PROGRAMS=" + vu1Dir};
            if (shell(configure, "") != 0)
                return false;
            std::vector<std::string> build = {"cmake", "--build", buildDir, "--target", "ps2EntryRunner", "-j",
                                              std::to_string(cores())};
            if (interrupted)
                build.push_back("--clean-first");
            if (shell(build, "") != 0)
                return false;
        }

        // ------------------------------------------------------------------ vu1
        if (!step(StepVu1))
            return false;
        // The capture folder is cleared once, before the passes, so that they
        // accumulate: the second pass starts from what the first recorded and asks
        // for programs it never reached, which is the whole point of running twice.
        // Clearing it per pass would throw away everything the earlier pass found.
        fs::remove_all(learn, ec);
        fs::create_directories(learn, ec);
        for (int pass = 1; pass <= recipe.learnPasses; ++pass)
        {
            if (!(cb.keepGoing == nullptr || cb.keepGoing()))
                return false;
            if (cb.onLine)
                cb.onLine("pass " + std::to_string(pass) + " of " + std::to_string(recipe.learnPasses));

            std::vector<std::string> environment = {"PS2X_HIDDEN_WINDOW=1", "PS2X_NO_LAUNCH_MENU=1",
                                                    "PS2X_VU1_LEARN=" + learn,
                                                    "PS2X_PAD_SCRIPT=" + recipe.learnPadScript};
            if (!recipe.learnFrames.empty())
                environment.push_back("PS2X_EXIT_AFTER_FRAME=" + recipe.learnFrames);
            // The first pass must start from nothing, or it would only record what is
            // already compiled and never find the rest.
            if (pass == 1)
                environment.push_back("PS2X_VU1_NO_COMPILED=1");

            // The pass exits on the manifest's frame count; the timeout is only there
            // in case a game never reaches it.
            std::vector<std::string> learner = {runner, bootPath};
            if (fs::exists("/usr/bin/timeout", ec))
                learner = {"/usr/bin/timeout", "400", runner, bootPath};
            const int status = runTool(learner, learn, environment, cb.onLine, pid);
            if (status != 0 && cb.onLine)
                cb.onLine("that pass ended early; that is fine if it recorded something");

            fs::remove_all(vu1Dir, ec);
            fs::create_directories(vu1Dir, ec);
            std::vector<std::string> captures;
            for (const fs::directory_entry &entry : fs::directory_iterator(learn, ec))
            {
                const std::string name = entry.path().filename().string();
                if (entry.is_regular_file() && name.rfind("vu1_learn_", 0) == 0 && entry.path().extension() == ".bin")
                    captures.push_back(entry.path().string());
            }
            if (captures.empty())
            {
                if (cb.onLine)
                    cb.onLine("no microprograms were recorded; the game did not reach gameplay");
                return false;
            }
            std::sort(captures.begin(), captures.end());

            std::vector<std::string> recompiler = {vu1Recomp, vu1Dir};
            recompiler.insert(recompiler.end(), captures.begin(), captures.end());
            if (shell(recompiler, "") != 0)
                return false;
            if (!fs::exists(join(vu1Dir, "vu1_programs.cpp"), ec))
            {
                if (cb.onLine)
                    cb.onLine("the VU1 compiler wrote no registry");
                return false;
            }
            size_t programs = 0;
            for (const fs::directory_entry &entry : fs::directory_iterator(vu1Dir, ec))
            {
                const std::string name = entry.path().filename().string();
                if (name.rfind("vu1p_", 0) == 0 && entry.path().extension() == ".cpp")
                    ++programs;
            }
            if (cb.onLine)
            {
                size_t capturesTotal = 0;
                for (const fs::directory_entry &entry : fs::directory_iterator(learn, ec))
                {
                    const std::string name = entry.path().filename().string();
                    if (entry.is_regular_file() && name.rfind("vu1_learn_", 0) == 0)
                        ++capturesTotal;
                }
                // Both numbers, because they should grow from pass to pass.
                cb.onLine(std::to_string(capturesTotal) + " capture(s), " + std::to_string(programs) +
                          " microprogram(s) so far");
            }

            // Re-configure as well as build: the CMake only takes the VU1 sources if
            // the registry existed when it last configured.
            std::vector<std::string> configure = {"cmake", "-S", portDir, "-B", buildDir, "-DCMAKE_BUILD_TYPE=Release",
                                                  "-DPS2X_VU1_PROGRAMS=" + vu1Dir};
            if (shell(configure, "") != 0)
                return false;
            std::vector<std::string> rebuild = {"cmake", "--build", buildDir, "--target", "ps2EntryRunner", "-j",
                                                std::to_string(cores())};
            if (interrupted)
                rebuild.push_back("--clean-first");
            if (shell(rebuild, "") != 0)
                return false;
        }

        // -------------------------------------------------------------- windows
        if (cross)
        {
            if (!step(StepWindows))
                return false;

            // Configured *with the VU1 directory*, and after the passes rather than
            // before them. That is not tidiness: a Windows link without the VU1
            // programs on the link line resolves raylib's CloseWindow and ShowCursor
            // against USER32's copies and stops with duplicate symbols.
            if (shell({"cmake", "-S", portDir, "-B", buildWin, "-DCMAKE_BUILD_TYPE=Release",
                       "-DCMAKE_TOOLCHAIN_FILE=" + join(portDir, "cmake/toolchains/llvm-mingw-x86_64.cmake"),
                       "-DLLVM_MINGW_ROOT=" + toolchainRoot, "-DPS2X_VU1_PROGRAMS=" + vu1Dir},
                      "") != 0)
                return false;
            if (shell({"cmake", "--build", buildWin, "--target", "ps2EntryRunner", "-j", std::to_string(cores())},
                      "") != 0)
                return false;

            // Then gather everything a Windows machine needs into one folder, because
            // the runner looks for the game's files beside itself. This copy is why
            // the step takes a while: the game's own files are most of two gigabytes.
            const std::string out = windowsOutDir(portDir);
            const std::string from = join(buildWin, "ps2xRuntime");
            if (!fs::exists(join(from, "ps2EntryRunner.exe"), ec))
            {
                if (cb.onLine)
                    cb.onLine("the Windows build produced no exe");
                return false;
            }
            fs::remove_all(out, ec);
            fs::create_directories(out, ec);
            fs::copy_file(join(from, "ps2EntryRunner.exe"), join(out, "ps2EntryRunner.exe"),
                          fs::copy_options::overwrite_existing, ec);
            for (const fs::directory_entry &entry : fs::directory_iterator(from, ec))
            {
                const std::string name = entry.path().filename().string();
                const bool wanted = entry.path().extension() == ".dll" || name == "custom_logo.png";
                if (entry.is_regular_file() && wanted)
                    fs::copy_file(entry.path(), fs::path(out) / name, fs::copy_options::overwrite_existing, ec);
            }
            for (const std::string &asset : {"prompts", "music", "mods"})
                if (fs::exists(join(from, asset), ec))
                    copyTree(join(from, asset), join(out, asset));
            writeFile(join(out, "portable.txt"), "");
            writeFile(join(out, "play.cmd"),
                      "@echo off\r\n"
                      "rem Runs the port from this folder, whatever the current directory is.\r\n"
                      "cd /d \"%~dp0\"\r\n"
                      "ps2EntryRunner.exe %*\r\n");
            const std::string readme = readFile(join(portDir, "docs/PORTABLE.txt"));
            if (!readme.empty())
                writeFile(join(out, "README.txt"), readme);

            if (cb.onLine)
                cb.onLine("copying the game's files, which is most of the wait");
            copyTree(discDir, join(out, "game"));
            if (cb.onLine)
                cb.onLine("the Windows build is in " + out);
        }

        return true;
    }

    void stop(std::atomic<int> &childPid)
    {
        const int pid = childPid.load();
        if (pid <= 0)
            return;
#if !defined(_WIN32)
        kill(-pid, SIGTERM);
        kill(pid, SIGTERM);
#endif
    }
}
