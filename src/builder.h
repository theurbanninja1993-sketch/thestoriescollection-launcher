// The build wizard's engine: what turns a game's fetched source plus the player's
// own disc into something the menu can launch.
//
// It runs the same steps build.sh runs, in the same order, against the same paths.
// The script and the wizard are two front ends to one sequence, so a change to one
// is a change to the other.
//
// The steps are invoked directly rather than through a shell, and the file work is
// done with std::filesystem rather than cp/rm/mkdir. That is what makes it possible
// to control the environment of a child process exactly (the VU1 pass needs four
// variables) and to stop a running step cleanly.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace builder
{
    // -------------------------------------------------------------- the source

    // One place the game can be read from.
    struct Source
    {
        std::string path;    // what the build reads
        std::string label;   // what the picker shows
        bool folder = false; // already extracted, so nothing to pull off a disc
    };

    // Optical drives first, then .iso/.bin files: a discs/ folder beside the config,
    // then the config's own folder.
    std::vector<Source> findSources(const std::string &configDir);

    // -------------------------------------------------------------- the recipe

    // What the port's own manifest (games/<id>/game.toml) says about building it.
    // Read rather than assumed, so a second game needs no code.
    struct Recipe
    {
        bool ok = false;
        std::string error;
        std::string bootFile;    // the ELF on the disc, and its hash
        std::string bootHash;
        std::string directory;   // the game's own folder (games/<id>/)
        std::string config;      // recompiler config
        std::string functionMap;
        int learnPasses = 2;
        std::string learnPadScript;
        std::string learnFrames;
    };

    // games/<gameId>/game.toml when the source has one by that name (a source can
    // hold both Stories games); otherwise the source's only game.
    Recipe readRecipe(const std::string &portDir, const std::string &gameId = {});

    // --------------------------------------------------------------- the steps

    enum Step
    {
        StepTools,     // configure, and build the tools every later step calls
        StepToolchain, // fetch and configure the Windows cross-compiler
        StepCheck,     // is this the right disc?
        StepExtract,   // pull the files off it
        StepRecompile, // the game's ELF, into C++
        StepInstall,   // that C++ into the runtime
        StepCompile,   // compile the port for this machine
        StepVu1,       // learn the VU1 microprograms by running the game, twice
        StepWindows,   // cross-compile Windows, and package it to hand over
        StepCount
    };

    const char *stepName(int step);
    const char *stepNote(int step);

    // The steps a run will actually take, in order: the Windows ones only when it is
    // asked for, so the list on screen is exactly what is going to happen.
    std::vector<int> plan(bool windows);

    // The Windows build is a cross-compile, so its compiler runs on this machine
    // rather than that one: llvm-mingw, fetched once, ~88 MB.
    struct Targets
    {
        bool windows = false;
        std::string toolchain; // a specific llvm-mingw to use, if one is named
    };

    // True under WSL, where the point of building anything is the Windows binary.
    bool runningUnderWsl();

    // How long the steps take, roughly, so a player can decide before committing to
    // them. Honest about the order of magnitude rather than precise: it is the
    // difference between "a moment" and "go and make tea".
    int planMinutes(bool windows, const std::string &image);

    // Where a cross-build leaves something that can be copied to a Windows machine:
    // the exe, its DLLs and the game's files, all in one folder.
    std::string windowsOutDir(const std::string &portDir);

    // Moves a freshly unpacked source (`incoming`) into the port folder, keeping
    // what an update does not change. Files whose content is the same are left
    // alone, timestamps and all, so the next build recompiles only what changed;
    // build/ and dist/ are never touched; files the last source had and this one
    // does not are removed. Returns an empty string, or what went wrong.
    std::string syncSource(const std::string &incoming, const std::string &portDir, size_t &changed,
                           size_t &kept, size_t &removed);

    // VU programs the game recorded while being played: it records every one it has
    // no compiled version of, and those run interpreted, about ten times slower.
    // optimize() moves the recordings into the build, compiles them with the rest
    // and relinks the game - about a minute. False means the game is unchanged.
    bool vuRecordingsPending(const std::string &portDir);
    bool optimize(const std::string &portDir, const struct Callbacks &cb);

    // Whether this platform can run a build at all.
    bool supported();
    const char *unsupportedNote();

    // Where a finished build leaves the runner, and the disc files it reads.
    std::string builtRunner(const std::string &portDir);
    std::string builtGameDir(const std::string &portDir);

    // ------------------------------------------------------------- the running

    // Called from the build thread. `keepGoing` is asked between steps; `childPid`
    // carries the process that is running so the menu thread can stop it.
    struct Callbacks
    {
        std::function<void(int)> onStep;                 // a step is starting
        std::function<void(const std::string &)> onLine; // output, as it arrives
        std::function<bool()> keepGoing;                 // false stops it
        std::atomic<int> *childPid = nullptr;
    };

    // Runs every step in order. False means it did not finish: either it failed or
    // it was stopped. The last line reported says which.
    bool run(const std::string &portDir, const std::string &image, const Recipe &recipe, const Targets &targets,
             const Callbacks &cb);

    // Stops whatever is running, taking its whole process tree: a compiler tree is
    // not one process.
    void stop(std::atomic<int> &childPid);
}
