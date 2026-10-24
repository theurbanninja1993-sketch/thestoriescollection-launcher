#include "builder.h"

#include "platform.h"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>

#if !defined(_WIN32)
#include <poll.h>
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
                    const std::vector<std::string> &environment, OnLine onLine, std::atomic<int> &childPid,
                    int limitSeconds)
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

            // Read to the child's exit, not to the pipe's end: a helper it started (an X server)
            // can keep the pipe open after the child is gone, and a blocking read then never
            // returns. The limit kills the whole group when it runs out.
            const auto started = std::chrono::steady_clock::now();
            std::string pending;
            int status = 0;
            bool exited = false, eof = false;
            std::chrono::steady_clock::time_point exitedAt{};
            auto emitLines = [&]()
            {
                size_t nl;
                while ((nl = pending.find('\n')) != std::string::npos)
                {
                    const std::string text = trim(pending.substr(0, nl));
                    pending.erase(0, nl + 1);
                    if (!text.empty() && onLine)
                        onLine(text);
                }
            };
            for (;;)
            {
                if (!exited && waitpid(pid, &status, WNOHANG) == pid)
                {
                    exited = true;
                    exitedAt = std::chrono::steady_clock::now();
                }
                if (!exited && limitSeconds > 0 && std::chrono::steady_clock::now() - started > std::chrono::seconds(limitSeconds))
                {
                    kill(-pid, SIGKILL);
                    waitpid(pid, &status, 0);
                    exited = true;
                    exitedAt = std::chrono::steady_clock::now();
                }
                if (exited && (eof || std::chrono::steady_clock::now() - exitedAt > std::chrono::seconds(2)))
                    break;
                if (eof)
                {
                    poll(nullptr, 0, 200);
                    continue;
                }
                pollfd pfd{fds[0], POLLIN, 0};
                const int ready = poll(&pfd, 1, 200);
                if (ready > 0)
                {
                    char buf[4096];
                    const ssize_t n = read(fds[0], buf, sizeof buf);
                    if (n > 0)
                    {
                        pending.append(buf, static_cast<size_t>(n));
                        emitLines();
                    }
                    else if (n == 0)
                        eof = true;
                    else if (errno != EINTR)
                        eof = true;
                }
            }
            close(fds[0]);
            emitLines();
            if (exited)
                kill(-pid, SIGKILL); // a helper it left behind (an X server) is not wanted after the step
            if (!pending.empty())
            {
                const std::string text = trim(pending);
                if (!text.empty() && onLine)
                    onLine(text);
            }
            if (!exited)
                waitpid(pid, &status, 0);
            childPid = 0;
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
#else
        // Windows: a job object holds every process a step starts, so one TerminateJobObject
        // takes the whole tree - the compiler, the game, anything they launched - as the
        // process group does on Linux. The output is read on its own thread, and the wait
        // for the child is on this one, so the limit still applies while a helper holds
        // the pipe open.
        std::mutex jobMutex;
        std::map<int, HANDLE> jobsByPid;

        std::string quoteArgument(const std::string &arg)
        {
            // The rules the C runtime uses to split a command line back into arguments.
            if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos)
                return arg;
            std::string out = "\"";
            size_t backslashes = 0;
            for (const char c : arg)
            {
                if (c == '\\')
                    ++backslashes;
                else if (c == '"')
                {
                    out.append(backslashes * 2 + 1, '\\');
                    out.push_back('"');
                    backslashes = 0;
                }
                else
                {
                    out.append(backslashes, '\\');
                    out.push_back(c);
                    backslashes = 0;
                }
            }
            out.append(backslashes * 2, '\\');
            out.push_back('"');
            return out;
        }

        // This process's environment with the caller's entries replacing any of the same name,
        // as the block CreateProcess takes: NUL-separated, ending in an empty entry.
        std::string environmentBlock(const std::vector<std::string> &overrides)
        {
            std::vector<std::string> entries;
            if (char *current = GetEnvironmentStringsA())
            {
                for (const char *p = current; *p != '\0'; p += std::strlen(p) + 1)
                    entries.push_back(p);
                FreeEnvironmentStringsA(current);
            }
            auto keyOf = [](const std::string &entry)
            { return entry.substr(0, entry.find('=', 1)); };
            std::vector<std::string> merged;
            for (const std::string &entry : entries)
            {
                const std::string key = keyOf(entry);
                bool replaced = false;
                for (const std::string &over : overrides)
                    if (_stricmp(keyOf(over).c_str(), key.c_str()) == 0)
                        replaced = true;
                if (!replaced)
                    merged.push_back(entry);
            }
            merged.insert(merged.end(), overrides.begin(), overrides.end());
            std::string block;
            for (const std::string &entry : merged)
            {
                block += entry;
                block.push_back('\0');
            }
            block.push_back('\0');
            return block;
        }

        int runTool(const std::vector<std::string> &argv, const std::string &cwd,
                    const std::vector<std::string> &environment, OnLine onLine, std::atomic<int> &childPid,
                    int limitSeconds)
        {
            if (argv.empty())
                return -1;

            SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            HANDLE readEnd = nullptr, writeEnd = nullptr;
            if (!CreatePipe(&readEnd, &writeEnd, &inherit, 0))
                return -1;
            SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
            HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                     OPEN_EXISTING, 0, nullptr);

            std::string commandLine;
            for (size_t i = 0; i < argv.size(); ++i)
            {
                if (i > 0)
                    commandLine.push_back(' ');
                commandLine += quoteArgument(argv[i]);
            }

            HANDLE job = CreateJobObjectA(nullptr, nullptr);
            if (job != nullptr)
            {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
            }

            const std::string block = environmentBlock(environment);
            STARTUPINFOA startup{};
            startup.cb = sizeof startup;
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = nul;
            startup.hStdOutput = writeEnd;
            startup.hStdError = writeEnd;
            PROCESS_INFORMATION process{};
            std::vector<char> line(commandLine.begin(), commandLine.end());
            line.push_back('\0');
            // Suspended, so the job has it before it can start anything of its own.
            const BOOL started = CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE,
                                                CREATE_SUSPENDED | CREATE_NO_WINDOW, const_cast<char *>(block.data()),
                                                cwd.empty() ? nullptr : cwd.c_str(), &startup, &process);
            const DWORD error = GetLastError();
            if (nul != nullptr)
                CloseHandle(nul);
            CloseHandle(writeEnd);
            if (!started)
            {
                CloseHandle(readEnd);
                if (job != nullptr)
                    CloseHandle(job);
                if (onLine)
                    onLine("could not start " + argv[0] + " (Windows error " + std::to_string(error) + ")");
                return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 127 : 126;
            }
            if (job != nullptr)
                AssignProcessToJobObject(job, process.hProcess);
            ResumeThread(process.hThread);
            CloseHandle(process.hThread);
            childPid = static_cast<int>(process.dwProcessId);
            if (job != nullptr)
            {
                std::lock_guard<std::mutex> lock(jobMutex);
                jobsByPid[childPid] = job;
            }

            // Output ends once nothing holds the pipe. The reader stops when that is so, or
            // when the step has ended and nothing is left to read.
            std::atomic<bool> stepEnded{false};
            std::thread reader([readEnd, onLine, &stepEnded]()
            {
                std::string pending;
                char buffer[4096];
                auto emitLines = [&]()
                {
                    size_t nl;
                    while ((nl = pending.find('\n')) != std::string::npos)
                    {
                        const std::string text = trim(pending.substr(0, nl));
                        pending.erase(0, nl + 1);
                        if (!text.empty() && onLine)
                            onLine(text);
                    }
                };
                for (;;)
                {
                    DWORD available = 0;
                    if (!PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr))
                        break; // the write ends are all closed
                    if (available == 0)
                    {
                        if (stepEnded.load())
                            break;
                        Sleep(50);
                        continue;
                    }
                    DWORD n = 0;
                    if (!ReadFile(readEnd, buffer, sizeof buffer, &n, nullptr) || n == 0)
                        break;
                    pending.append(buffer, n);
                    emitLines();
                }
                const std::string text = trim(pending);
                if (!text.empty() && onLine)
                    onLine(text);
            });

            bool killed = false;
            const DWORD waitMs = limitSeconds > 0 ? static_cast<DWORD>(limitSeconds) * 1000u : INFINITE;
            if (WaitForSingleObject(process.hProcess, waitMs) == WAIT_TIMEOUT)
            {
                killed = true;
                if (job != nullptr)
                    TerminateJobObject(job, 1);
                else
                    TerminateProcess(process.hProcess, 1);
                WaitForSingleObject(process.hProcess, INFINITE);
            }
            DWORD code = 1;
            GetExitCodeProcess(process.hProcess, &code);
            // Whatever it left running (a helper, an X server) is not wanted after the step.
            if (job != nullptr)
                TerminateJobObject(job, 1);
            stepEnded.store(true);
            reader.join();

            CloseHandle(readEnd);
            CloseHandle(process.hProcess);
            if (job != nullptr)
            {
                std::lock_guard<std::mutex> lock(jobMutex);
                jobsByPid.erase(childPid);
                CloseHandle(job);
            }
            childPid = 0;
            return killed ? -1 : static_cast<int>(code);
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
                           childPid, 0);
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
            // hashfile prints "<hash>  <path>"; anything else is not a hash, and taking
            // its first word as one reported "ps2x_iso" as the disc's hash.
            const size_t end = output.find_first_of(" \t\r\n");
            const std::string first = end == std::string::npos ? trim(output) : output.substr(0, end);
            return first.size() == 32 && std::all_of(first.begin(), first.end(), [](unsigned char c) {
                       return std::isxdigit(c) != 0;
                   })
                       ? first
                       : std::string{};
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
                    // Forward slashes, which Windows takes as well: in a TOML string a
                    // backslash starts an escape, and C:\port\games is not a valid one.
                    std::string value = pair.second.first;
                    std::replace(value.begin(), value.end(), '\\', '/');
                    result << key << " = \"" << value << "\"\n";
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

    // ------------------------------------------------------------------ update

    std::string syncSource(const std::string &incoming, const std::string &portDir, size_t &changed,
                           size_t &kept, size_t &removed)
    {
        changed = kept = removed = 0;
        std::error_code ec;
        if (!fs::is_directory(incoming, ec))
            return "nothing was unpacked to " + incoming;
        const fs::path port(portDir);
        // The files the last update put here, so a file the source has since dropped
        // can be told apart from one the build made.
        const fs::path manifest = port / ".ps2x-source-files";
        const bool knowsOld = fs::exists(manifest, ec);
        auto owned = [](const fs::path &relative)
        {
            const std::string first = relative.begin() == relative.end() ? std::string{} : relative.begin()->string();
            return first != "build" && first != "dist" && first != ".ps2x-source-files";
        };

        // A folder from before the manifest: its source goes, and the build stays.
        if (!knowsOld && fs::exists(port, ec))
            for (const fs::directory_entry &entry : fs::directory_iterator(port, ec))
                if (owned(entry.path().filename()))
                    fs::remove_all(entry.path(), ec);

        // Code the build installs into the source tree. The source's placeholder
        // for it is not copied over it: the build would only copy its own back, and
        // a changed timestamp on it means recompiling it.
        const fs::path generated = port / "build" / "generated";
        auto installedByBuild = [&](const fs::path &relative)
        {
            const std::string parent = relative.parent_path().generic_string();
            return (parent == "ps2xRuntime/src/runner" || parent == "ps2xRuntime/include") &&
                   fs::exists(generated / relative.filename(), ec);
        };

        std::vector<std::string> now;
        for (const fs::directory_entry &entry : fs::recursive_directory_iterator(incoming, ec))
        {
            if (!entry.is_regular_file())
                continue;
            const fs::path relative = fs::relative(entry.path(), incoming, ec);
            if (!owned(relative))
                continue;
            now.push_back(relative.generic_string());
            const fs::path target = port / relative;
            if (installedByBuild(relative) && fs::exists(target, ec))
            {
                ++kept;
                continue;
            }
            std::error_code same;
            if (fs::exists(target, same) && fs::file_size(target, same) == entry.file_size(same) &&
                readFile(target.string()) == readFile(entry.path().string()))
            {
                ++kept;
                continue;
            }
            fs::create_directories(target.parent_path(), ec);
            std::error_code copyEc;
            fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, copyEc);
            if (copyEc)
                return "could not write " + target.string() + ": " + copyEc.message();
            // Stamped now. Unpacking dates every file to its commit, and Windows'
            // copy keeps that, so a changed file could look older than the build
            // made from the version before it - and never be compiled.
            fs::last_write_time(target, fs::file_time_type::clock::now(), ec);
            ++changed;
        }

        if (knowsOld)
        {
            std::istringstream old(readFile(manifest.string()));
            std::vector<std::string> sorted = now;
            std::sort(sorted.begin(), sorted.end());
            std::string line;
            while (std::getline(old, line))
            {
                line = trim(line);
                if (line.empty() || !owned(fs::path(line)) || std::binary_search(sorted.begin(), sorted.end(), line))
                    continue;
                if (installedByBuild(fs::path(line)))
                    continue;
                if (fs::remove(port / line, ec))
                    ++removed;
            }
        }

        std::string list;
        for (const std::string &name : now)
            list += name + "\n";
        if (!writeFile(manifest.string(), list))
            return "could not write " + manifest.string();
        fs::remove_all(incoming, ec);
        return {};
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
        // Every shape a disc arrives in: a plain .iso, a raw .bin, a .cue sheet that
        // names one of those, or a .chd - which is what a lot of people keep their
        // library in, and what the port reads directly now.
        const std::vector<fs::path> where = {fs::path(configDir) / "discs", fs::path(configDir)};
        for (const fs::path &folder : where)
        {
            if (!fs::is_directory(folder, ec))
                continue;
            std::vector<fs::path> images;
            for (const fs::directory_entry &entry : fs::directory_iterator(folder, ec))
            {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (entry.is_regular_file() && (ext == ".iso" || ext == ".bin" || ext == ".cue" || ext == ".chd"))
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

        recipe.directory = manifest.parent_path().string();
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
#if defined(_WIN32)
        case StepToolchain: return "Compiler";
#else
        case StepToolchain: return "Windows cross-compiler";
#endif
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
#if defined(_WIN32)
            return "A compiler, CMake and Ninja, fetched once (about 200 MB) into port\\toolchains. Nothing is "
                   "installed on the system, and the next build reuses them.";
#else
            return "llvm-mingw, fetched once (88 MB). The Windows build is cross-compiled, so its compiler runs here.";
#endif
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

    int planMinutes(bool windows, const std::string &image)
    {
        std::error_code ec;
        // Measured on the machine this was written on: the tools 161 s, the port's
        // own compile 426 s, and the two VU1 passes about 800 s.
        const bool folder = fs::is_directory(image, ec);
#if defined(_WIN32)
        // Built here, natively: one compile and the passes, with no cross-compile after.
        // Measured under Wine on a 20-thread desktop: the compiler fetch and the tools
        // 1.5 minutes, the port 3.5, the two passes and their relinks 5. A laptop is slower.
        (void)windows;
        return 4 + (folder ? 0 : 17) + 8 + 10;
#endif
        int minutes = 3;             // configuring and building the tools
        if (windows)
            minutes += 3;            // llvm-mingw, first time only
        if (!folder)
            minutes += 17;           // reading a disc, which is the slow part
        minutes += 7;                // compiling the port
        minutes += 14;               // learning and compiling the microprograms
        if (windows)
            minutes += 15;           // the cross-compile and its copy of the game
        return minutes;
    }

    std::vector<int> plan(bool windows)
    {
#if defined(_WIN32)
        // The compiler first, because on Windows the tools are built with it too.
        (void)windows;
        return {StepToolchain, StepTools, StepCheck, StepExtract, StepRecompile, StepInstall, StepCompile, StepVu1};
#endif
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

    bool supported() { return true; }

    const char *unsupportedNote()
    {
        return "This launcher cannot build the port on this machine.";
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
#if defined(_WIN32)
        const std::string exe = ".exe";
#else
        const std::string exe;
#endif
        const std::string ps2Recomp = join(buildDir, "ps2xRecomp/ps2_recomp") + exe;
        const std::string ps2Iso = join(buildDir, "ps2xBuilder/ps2x_iso") + exe;
        const std::string vu1Recomp = join(buildDir, "ps2xRuntime/vu1_recomp") + exe;
        const std::string runner = builtRunner(portDir) + exe;
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
        { return runTool(argv, cwd, {}, cb.onLine, pid, 0); };

        // Every step that can fail says what failed. This line is what the player
        // ends up reading on the build-ended screen, and "nothing was reported"
        // is a poor thing to leave them with - a missing cmake was the worst case,
        // where the child never even started and so printed nothing at all.
        auto failed = [&](const std::string &what, int status) -> bool
        {
            if (cb.onLine)
            {
                std::string line = what + " failed";
                if (status == 127)
                    line += " - the program is not installed, or not on PATH";
                else if (status > 0)
                    line += " (exit " + std::to_string(status) + ")";
                line += ". Everything it printed is in the log.";
                cb.onLine(line);
            }
            return false;
        };

        // How the port is configured. The studio, the tests and the analyzer are
        // developer tools nothing here runs, and leaving them out saves fetching and
        // building their dependencies. Older sources ignore the switches.
        std::string cmake = "cmake";
        std::vector<std::string> hostFlags = {"-DCMAKE_BUILD_TYPE=Release", "-DPS2X_BUILD_STUDIO=OFF",
                                              "-DPS2X_BUILD_TEST=OFF", "-DPS2X_BUILD_ANALYZER=OFF"};
        auto configureArgs = [&](const std::vector<std::string> &extra)
        {
            std::vector<std::string> argv = {cmake, "-S", portDir, "-B", buildDir};
            argv.insert(argv.end(), hostFlags.begin(), hostFlags.end());
            argv.insert(argv.end(), extra.begin(), extra.end());
            return argv;
        };

#if defined(_WIN32)
        // ------------------------------------------------- toolchain (Windows)
        // Built natively: a compiler, CMake and Ninja, each fetched once into a folder
        // beside the sources rather than inside them, because fetching a source wipes
        // its folder and these are the same for every game. curl and tar ship with
        // Windows 10 and 11, so there is nothing to install and no WSL.
        if (!step(StepToolchain))
            return false;
        {
            const std::string toolchains = join(fs::path(portDir).parent_path().generic_string(), "toolchains");
            fs::create_directories(toolchains, ec);
            struct Tool
            {
                const char *what, *url, *archive, *into, *probe;
            };
            const Tool tools[] = {
                {"the compiler (llvm-mingw, 150 MB)",
                 "https://github.com/mstorsjo/llvm-mingw/releases/download/20241217/llvm-mingw-20241217-ucrt-x86_64.zip",
                 "llvm-mingw-20241217-ucrt-x86_64.zip", "", "llvm-mingw-20241217-ucrt-x86_64/bin/x86_64-w64-mingw32-clang++.exe"},
                {"CMake (45 MB)",
                 "https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-windows-x86_64.zip",
                 "cmake-3.31.6-windows-x86_64.zip", "", "cmake-3.31.6-windows-x86_64/bin/cmake.exe"},
                {"Ninja", "https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip",
                 "ninja-1.12.1-win.zip", "ninja-1.12.1", "ninja-1.12.1/ninja.exe"},
            };
            for (const Tool &tool : tools)
            {
                if (fs::exists(join(toolchains, tool.probe), ec))
                    continue;
                const std::string archive = join(toolchains, tool.archive);
                // Downloaded under another name and renamed once whole, so a download that
                // was stopped is never mistaken for a finished one.
                if (!fs::exists(archive, ec))
                {
                    if (cb.onLine)
                        cb.onLine(std::string("fetching ") + tool.what + ", once");
                    const std::string partial = archive + ".part";
                    fs::remove(partial, ec);
                    if (shell({"curl", "-fL", "--retry", "3", "-o", partial, tool.url}, "") != 0)
                    {
                        if (cb.onLine)
                            cb.onLine(std::string("could not download ") + tool.what +
                                      " - check the connection, then press B again");
                        return false;
                    }
                    fs::rename(partial, archive, ec);
                }
                if (cb.onLine)
                    cb.onLine(std::string("unpacking ") + tool.what);
                const std::string into = join(toolchains, tool.into);
                fs::create_directories(into, ec);
                // Windows' own tar, by its full name: the MSYS tar a Git install puts on PATH
                // reads no zips, and takes the C: in a path for a remote host.
                std::string tar = "tar";
                if (const char *root = std::getenv("SystemRoot"))
                    if (fs::exists(std::string(root) + "\\System32\\tar.exe", ec))
                        tar = std::string(root) + "\\System32\\tar.exe";
                int status = shell({tar, "-xf", archive, "-C", into}, "");
                if (status != 0)
                    status = shell({"powershell", "-NoProfile", "-Command",
                                    "Expand-Archive -Force -LiteralPath '" + archive + "' -DestinationPath '" + into +
                                        "'"},
                                   "");
                if (status != 0 || !fs::exists(join(toolchains, tool.probe), ec))
                {
                    // A damaged download is the likely reason, so it goes and the next try fetches it again.
                    fs::remove(archive, ec);
                    return failed(std::string("unpacking ") + tool.what, status);
                }
                fs::remove(archive, ec);
            }

            const std::string llvm = join(toolchains, "llvm-mingw-20241217-ucrt-x86_64/bin");
            cmake = join(toolchains, "cmake-3.31.6-windows-x86_64/bin/cmake.exe");
            hostFlags.insert(hostFlags.begin(),
                             {"-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + join(toolchains, "ninja-1.12.1/ninja.exe"),
                              "-DCMAKE_C_COMPILER=" + join(llvm, "x86_64-w64-mingw32-clang.exe"),
                              "-DCMAKE_CXX_COMPILER=" + join(llvm, "x86_64-w64-mingw32-clang++.exe"),
                              "-DCMAKE_RC_COMPILER=" + join(llvm, "x86_64-w64-mingw32-windres.exe"),
                              // Self-contained: no libc++ or unwinder DLLs to carry around.
                              "-DCMAKE_EXE_LINKER_FLAGS=-static"});
            if (cb.onLine)
                cb.onLine("compiler: " + llvm);

            // A build folder another configure made - WSL's, from before the launcher
            // built natively - names a different generator and compiler, and CMake
            // refuses to reuse it. Its cache goes; the disc it extracted stays.
            const std::string cache = readFile(join(buildDir, "CMakeCache.txt"));
            if (!cache.empty() && (cache.find("CMAKE_GENERATOR:INTERNAL=Ninja") == std::string::npos ||
                                   cache.find("x86_64-w64-mingw32-clang++.exe") == std::string::npos))
            {
                if (cb.onLine)
                    cb.onLine("starting the build folder again: another kind of build made it");
                fs::remove(join(buildDir, "CMakeCache.txt"), ec);
                fs::remove_all(join(buildDir, "CMakeFiles"), ec);
            }
        }
#endif

        // ---------------------------------------------------------------- tools
        if (!step(StepTools))
            return false;
        fs::create_directories(buildDir, ec);
        {
            const int status = shell(configureArgs({}), "");
            if (status != 0)
                return failed(status == 127 ? "cmake" : "configuring the build", status);
        }
        {
            std::vector<std::string> argv = {cmake, "--build", buildDir, "--target",
                                             "ps2_recomp", "ps2x_iso", "vu1_recomp", "-j", std::to_string(cores())};
            const int status = shell(argv, "");
            if (status != 0)
                return failed("building the tools", status);
        }
        for (const std::string &tool : {ps2Recomp, ps2Iso, vu1Recomp})
            if (!need(tool))
                return false;

        // ---------------------------------------------------------------- check
        // Before the toolchain and the Windows configure, which take minutes: this only
        // needs ps2x_iso, which the tools step has just built, and a wrong disc is the
        // one failure that needs nothing else.
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
            status = runCapturing({ps2Iso, "check", image, "--boot", recipe.bootFile}, "", cb.onLine, pid, output);
        }
        if (status != 0)
            return failed("reading the disc", status);
        const std::string hash = parseHash(output);
        if (hash.empty())
        {
            if (cb.onLine)
                cb.onLine("could not read this disc. The reason is in the lines above; the whole check is in the log.");
            return false;
        }
        if (hash != recipe.bootHash)
        {
            if (cb.onLine)
                cb.onLine("this is a PlayStation 2 disc, but not the one this source was built for: its boot file (" +
                          recipe.bootFile + ") hashes to " + hash + ", and this source expects " + recipe.bootHash +
                          ". A different region, a different pressing, or an image a tool has edited will do this.");
            return false;
        }

        // ------------------------------------------------------------ toolchain
        // A Windows build needs a compiler that runs here and targets there, and one
        // is fetched once into the source and reused. Anything already on this machine
        // wins over fetching: one named, one the environment points at, the one an
        // existing build-win was configured with, then our own copy.
#if defined(_WIN32)
        const bool cross = false; // this machine's own build is the Windows one
#else
        const bool cross = targets.windows;
#endif
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
                        cb.onLine("could not download the cross-compiler - check the connection, then try again");
                    return false;
                }
                if (cb.onLine)
                    cb.onLine("unpacking the cross-compiler, which takes a moment");
                {
                    const int status = shell({"tar", "-xJf", archive, "-C", toolchains}, "");
                    if (status != 0)
                        return failed("unpacking the cross-compiler", status);
                }
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
            {
                const int status = shell({"cmake", "-S", portDir, "-B", buildWin, "-DCMAKE_BUILD_TYPE=Release",
                                          "-DCMAKE_TOOLCHAIN_FILE=" + join(portDir, "cmake/toolchains/llvm-mingw-x86_64.cmake"),
                                          "-DLLVM_MINGW_ROOT=" + toolchainRoot},
                                         "");
                if (status != 0)
                    return failed("configuring the Windows build", status);
            }
        }

        // -------------------------------------------------------------- extract
        if (!step(StepExtract))
            return false;
        if (!imageIsFolder)
        {
            // The same image as last time is not read again: the marker names it by
            // path, size and date, and is written only once an extraction finished.
            const std::string extracted = join(discDir, ".ps2x-extracted");
            std::string identity = image;
            {
                std::error_code idEc;
                const auto size = fs::is_regular_file(image, idEc) ? fs::file_size(image, idEc) : 0;
                const auto when = fs::last_write_time(image, idEc).time_since_epoch().count();
                identity += "|" + std::to_string(size) + "|" + std::to_string(static_cast<long long>(when));
            }
            if (readFile(extracted) == identity && fs::exists(bootPath, ec))
            {
                if (cb.onLine)
                    cb.onLine("this disc is already extracted");
            }
            else
            {
                fs::remove_all(discDir, ec);
                fs::create_directories(discDir, ec);
                const int status = shell({ps2Iso, "extract", image, discDir}, "");
                if (status != 0)
                    return failed("extracting the disc", status);
                writeFile(extracted, identity);
            }
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
            {
                const int status = shell({ps2Recomp, workConfig}, portDir);
                if (status != 0)
                    return failed("the recompiler", status);
            }
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
            // The achievement set travels with the game, not with the source: it is
            // read from beside the runner, so a built game has one and the engine
            // finds it without being told where to look.
            std::error_code setEc;
            const std::string setSource = join(recipe.directory, "achievements.toml");
            if (fs::exists(setSource, setEc))
            {
                fs::copy_file(setSource, join(portDir, "build/ps2xRuntime/achievements.toml"),
                              fs::copy_options::overwrite_existing, setEc);
                if (cb.onLine)
                    cb.onLine("the achievement set travels with the game");
            }
            if (cb.onLine)
                cb.onLine("copied " + std::to_string(copied) + ", left " + std::to_string(kept) + " alone");
        }

        // -------------------------------------------------------------- compile
        if (!step(StepCompile))
            return false;
        // An earlier build's recordings, if it finished its passes. They come from the
        // disc, not from the source, so an update can reuse them: the programs are
        // compiled again from them with this source's compiler, and the passes - the
        // longest part of a rebuild - are skipped. If that fails, the passes run.
        const std::string learned = join(learn, ".ps2x-learned");
        bool reuseCaptures = false;
        if (fs::exists(learned, ec))
        {
            std::vector<std::string> captures;
            for (const fs::directory_entry &entry : fs::directory_iterator(learn, ec))
            {
                const std::string name = entry.path().filename().string();
                if (entry.is_regular_file() && name.rfind("vu1_learn_", 0) == 0 && entry.path().extension() == ".bin")
                    captures.push_back(entry.path().string());
            }
            std::sort(captures.begin(), captures.end());
            if (!captures.empty())
            {
                if (cb.onLine)
                    cb.onLine("reusing " + std::to_string(captures.size()) +
                              " VU1 recording(s) from the last build, so the passes can be skipped");
                fs::remove_all(vu1Dir, ec);
                fs::create_directories(vu1Dir, ec);
                std::vector<std::string> recompiler = {vu1Recomp, vu1Dir};
                recompiler.insert(recompiler.end(), captures.begin(), captures.end());
                reuseCaptures = shell(recompiler, "") == 0 && fs::exists(join(vu1Dir, "vu1_programs.cpp"), ec);
                if (!reuseCaptures)
                {
                    if (cb.onLine)
                        cb.onLine("those recordings did not compile with this source; the passes will run");
                    fs::remove_all(vu1Dir, ec);
                    fs::remove(learned, ec);
                }
            }
        }
        {
            std::vector<std::string> configure = configureArgs({"-DPS2X_VU1_PROGRAMS=" + vu1Dir});
            {
                const int status = shell(configure, "");
                if (status != 0)
                    return failed("configuring the port", status);
            }
            std::vector<std::string> build = {cmake, "--build", buildDir, "--target", "ps2EntryRunner", "-j",
                                              std::to_string(cores())};
            if (interrupted)
                build.push_back("--clean-first");
            {
                const int status = shell(build, "");
                if (status != 0)
                    return failed("compiling the port", status);
            }
        }

        // ------------------------------------------------------------------ vu1
        if (!step(StepVu1))
            return false;
        if (reuseCaptures)
        {
            if (cb.onLine)
                cb.onLine("the microprograms were compiled from the last build's recordings");
        }
        else
        {
            fs::remove(learned, ec);
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

                // The pass runs the game for its recordings, with its window hidden so it stays
                // off the desktop. Some graphics stacks refuse a hidden window (GLXBadFBConfig
                // under WSL), and then nothing is recorded, so a pass that records nothing is run
                // once more with a visible window before the build gives up.
                // Linux helpers. Never on Windows, where Wine's view of / would otherwise
                // find the Linux machine's copies and hand the game to them.
                auto hostHas = [&](const char *path)
                {
    #if defined(_WIN32)
                    (void)path;
                    return false;
    #else
                    return fs::exists(path, ec);
    #endif
                };
                auto learnPass = [&](bool hidden, bool virtualDisplay) -> int
                {
                    std::vector<std::string> environment = {"PS2X_NO_LAUNCH_MENU=1",
                                                            "PS2X_VU1_LEARN=" + learn,
                                                            "PS2X_PAD_SCRIPT=" + recipe.learnPadScript};
                    if (hidden)
                        environment.insert(environment.begin(), "PS2X_HIDDEN_WINDOW=1");
                    if (!recipe.learnFrames.empty())
                        environment.push_back("PS2X_EXIT_AFTER_FRAME=" + recipe.learnFrames);
                    // The first pass must start from nothing, or it would only record what is
                    // already compiled and never find the rest.
                    if (pass == 1)
                        environment.push_back("PS2X_VU1_NO_COMPILED=1");

                    // The pass exits on the manifest's frame count; the timeout is only there
                    // in case a game never reaches it.
                    // A private X server, when the xvfb package is installed: the game then talks to
                    // that instead of the desktop, which is where a display can refuse the visual. It is
                    // software rendering, so the pass gets longer. The timeout wraps the whole thing: an
                    // X server that never starts once held a build on this step for hours.
                    const bool useVirtual = virtualDisplay && hostHas("/usr/bin/xvfb-run");
                    std::vector<std::string> learner = {runner, bootPath};
                    if (useVirtual)
                        learner = {"xvfb-run", "-a", runner, bootPath};
                    if (hostHas("/usr/bin/timeout"))
                        learner.insert(learner.begin(), {"/usr/bin/timeout", useVirtual ? "1800" : "400"});
                    // The timeout above is the normal limit; this is the backstop if it never ends the tree.
    #if defined(_WIN32)
                    // Nothing wraps it on Windows, so this is the limit. A pass is about four minutes
                    // on a laptop; the margin is for slower machines.
                    return runTool(learner, learn, environment, cb.onLine, pid, 1200);
    #else
                    return runTool(learner, learn, environment, cb.onLine, pid, (useVirtual ? 1800 : 400) + 60);
    #endif
                };
                auto collectCaptures = [&]()
                {
                    std::vector<std::string> found;
                    for (const fs::directory_entry &entry : fs::directory_iterator(learn, ec))
                    {
                        const std::string name = entry.path().filename().string();
                        if (entry.is_regular_file() && name.rfind("vu1_learn_", 0) == 0 && entry.path().extension() == ".bin")
                            found.push_back(entry.path().string());
                    }
                    return found;
                };

                int learnStatus = learnPass(true, false);
                if (learnStatus != 0 && cb.onLine)
                    cb.onLine("that pass ended early; that is fine if it recorded something");
                std::vector<std::string> captures = collectCaptures();
                if (captures.empty())
                {
                    if (cb.onLine)
                        cb.onLine("the pass recorded nothing with a hidden window; running it once more with a visible one");
                    learnStatus = learnPass(false, false);
                    if (learnStatus != 0 && cb.onLine)
                        cb.onLine("that visible pass ended early too");
                    captures = collectCaptures();
                }
                if (captures.empty() && hostHas("/usr/bin/xvfb-run"))
                {
                    if (cb.onLine)
                        cb.onLine("still nothing; running it once more under a private virtual display");
                    learnStatus = learnPass(true, true);
                    if (learnStatus != 0 && cb.onLine)
                        cb.onLine("that virtual-display pass ended early too");
                    captures = collectCaptures();
                }

                fs::remove_all(vu1Dir, ec);
                fs::create_directories(vu1Dir, ec);
                if (captures.empty())
                {
                    if (cb.onLine)
                        cb.onLine("no microprograms were recorded; the game did not reach gameplay");
                    return false;
                }
                std::sort(captures.begin(), captures.end());

                std::vector<std::string> recompiler = {vu1Recomp, vu1Dir};
                recompiler.insert(recompiler.end(), captures.begin(), captures.end());
                {
                    const int status = shell(recompiler, "");
                    if (status != 0)
                        return failed("the VU1 compiler", status);
                }
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
                std::vector<std::string> configure = configureArgs({"-DPS2X_VU1_PROGRAMS=" + vu1Dir});
                {
                    const int status = shell(configure, "");
                    if (status != 0)
                        return failed("configuring the port with the microprograms", status);
                }
                std::vector<std::string> rebuild = {cmake, "--build", buildDir, "--target", "ps2EntryRunner", "-j",
                                                    std::to_string(cores())};
                if (interrupted)
                    rebuild.push_back("--clean-first");
                {
                    const int status = shell(rebuild, "");
                    if (status != 0)
                        return failed("compiling the port with the microprograms", status);
                }
            }
            // Both passes done: the next build can reuse what they recorded.
            writeFile(learned, "complete\n");
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
            {
                const int status = shell({"cmake", "-S", portDir, "-B", buildWin, "-DCMAKE_BUILD_TYPE=Release",
                                          "-DCMAKE_TOOLCHAIN_FILE=" + join(portDir, "cmake/toolchains/llvm-mingw-x86_64.cmake"),
                                          "-DLLVM_MINGW_ROOT=" + toolchainRoot, "-DPS2X_VU1_PROGRAMS=" + vu1Dir},
                                         "");
                if (status != 0)
                    return failed("configuring the Windows build", status);
            }
            {
                const int status = shell({"cmake", "--build", buildWin, "--target", "ps2EntryRunner", "-j",
                                          std::to_string(cores())},
                                         "");
                if (status != 0)
                    return failed("cross-compiling for Windows", status);
            }

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
            const std::string setSource = join(recipe.directory, "achievements.toml");
            if (fs::exists(setSource, ec))
                fs::copy_file(setSource, join(out, "achievements.toml"), fs::copy_options::overwrite_existing, ec);
            const std::string readme = readFile(join(portDir, "docs/PORTABLE.txt"));
            if (!readme.empty())
                writeFile(join(out, "README.txt"), readme);

            if (cb.onLine)
                cb.onLine("copying the game's files, which is most of the wait");
            copyTree(discDir, join(out, "game"));
            if (cb.onLine)
                cb.onLine("the Windows build is in " + out);
        }

#if defined(_WIN32)
        // A folder the WSL build packaged would be played instead of this build, so it goes.
        if (fs::exists(windowsOutDir(portDir), ec))
            fs::remove_all(windowsOutDir(portDir), ec);
#endif
        return true;
    }

    void stop(std::atomic<int> &childPid)
    {
        const int pid = childPid.load();
        if (pid <= 0)
            return;
#if defined(_WIN32)
        std::lock_guard<std::mutex> lock(jobMutex);
        const auto found = jobsByPid.find(pid);
        if (found != jobsByPid.end())
            TerminateJobObject(found->second, 1);
#else
        kill(-pid, SIGTERM);
        kill(pid, SIGTERM);
#endif
    }
}
