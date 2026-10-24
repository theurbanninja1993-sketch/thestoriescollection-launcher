#include "platform.h"

#include <filesystem>

#if defined(_WIN32)
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace
{
    // A POSIX shell's single-quoted string: everything inside is literal, and the one
    // character that cannot appear is escaped by closing, escaping and reopening.
    std::string quotedForShell(const std::string &text)
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
}

namespace platform
{
    std::string executableDirectory()
    {
#if defined(_WIN32)
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

    std::vector<std::string> opticalDrives()
    {
        std::vector<std::string> drives;
#if defined(_WIN32)
        for (char letter = 'D'; letter <= 'Z'; ++letter)
        {
            const std::string root = std::string(1, letter) + ":\\";
            if (GetDriveTypeA(root.c_str()) == DRIVE_CDROM)
                drives.push_back(std::string(1, letter) + ":");
        }
#else
        std::error_code ec;
        for (int i = 0; i < 8; ++i)
        {
            const std::string drive = "/dev/sr" + std::to_string(i);
            if (fs::exists(drive, ec))
                drives.push_back(drive);
        }
#endif
        return drives;
    }

    bool fileIsExecutable(const std::string &path)
    {
#if defined(_WIN32)
        std::error_code ec;
        return fs::exists(path, ec);
#else
        return access(path.c_str(), X_OK) == 0;
#endif
    }

    bool openUrl(const std::string &url)
    {
#if defined(_WIN32)
        // ShellExecute is the Windows way, and it wants wide characters. The
        // launcher's own strings are narrow, so the URL is widened here.
        const std::wstring wide(url.begin(), url.end());
        const HINSTANCE result = ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(result) > 32; // 32 and below are error codes
#else
        // xdg-open is the desktop's own dispatcher; it is not universal, but
        // nothing better is, and a failure just means the player opens it by hand.
        const std::string command = "xdg-open \"" + url + "\" >/dev/null 2>&1 &";
        return std::system(command.c_str()) == 0;
#endif
    }

    std::string makeDirectoryCommand(const std::string &path)
    {
        if (path.empty())
            return {};
#if defined(_WIN32)
        // cmd's mkdir makes the whole path, which is what -p is for, and it is
        // called after the wipe, so the directory cannot be there already. Not
        // guarded by `if`: cmd's `if` takes the *rest of the line* as its command,
        // so `if not exist X mkdir X && next` runs `next` only when X was already
        // there - the opposite of what it looks like, and a silent nothing when it
        // matters.
        return "mkdir \"" + path + "\" >nul 2>&1";
#else
        return "mkdir -p \"" + path + "\"";
#endif
    }

    bool commandExists(const std::string &command)
    {
#if defined(_WIN32)
        const std::string probe = command + " --version >nul 2>&1";
#else
        const std::string probe = command + " --version >/dev/null 2>&1";
#endif
        return std::system(probe.c_str()) == 0;
    }

    std::string removeTreeCommand(const std::string &path)
    {
        if (path.empty())
            return {};
#if defined(_WIN32)
        // cmd has no rm, and rmdir *refuses* a tree with read-only files in it -
        // which is exactly what a source folder left by a git clone is full of,
        // since git marks its objects read-only. So the attributes go first, and
        // both halves are silenced: neither has anything to say when the folder is
        // simply not there, which is the normal case for a first fetch.
        return "attrib -h -r -s /s /d \"" + path + "\" >nul 2>&1 & rmdir /s /q \"" + path + "\" >nul 2>&1";
#else
        return "rm -rf \"" + path + "\"";
#endif
    }

    std::string removeFileCommand(const std::string &path)
    {
        if (path.empty())
            return {};
#if defined(_WIN32)
        // cmd has no rm. This one cost a real player a real fetch: the whole chain
        // stopped on "'rm' is not recognized as an internal or external command",
        // which reads as a download that failed. Quiet, because the file is always
        // there by this point - the download that wrote it succeeded, or the chain
        // would never have reached here.
        return "del /q \"" + path + "\" >nul 2>&1";
#else
        return "rm -f \"" + path + "\"";
#endif
    }

    const char *commandSeparator()
    {
#if defined(_WIN32)
        return " & ";
#else
        return " ; ";
#endif
    }

    std::string changeDirectoryCommand(const std::string &path)
    {
        if (path.empty())
            return {};
#if defined(_WIN32)
        // /d so a different drive is allowed.
        return "cd /d \"" + path + "\"";
#else
        return "cd \"" + path + "\"";
#endif
    }

    std::string captureEverything(const std::string &command)
    {
        if (command.empty())
            return command;
#if defined(_WIN32)
        // cmd has no braces; a parenthesised group is the equivalent.
        return "(" + command + ") 2>&1";
#else
        return "{ " + command + " ; } 2>&1";
#endif
    }

    bool runDetached(const std::string &program, const std::vector<std::string> &arguments)
    {
        if (program.empty())
            return false;
#if defined(_WIN32)
        // `start` gives it its own window and returns at once. The window is left
        // visible on purpose: if WSL cannot run what it was given, the reason is on
        // that screen rather than nowhere, and a launcher that fails silently has cost
        // this project two rounds already.
        std::string command = "start \"\" \"" + program + "\"";
        for (const std::string &argument : arguments)
            command += " \"" + argument + "\"";
        return std::system(command.c_str()) == 0;
#else
        std::string command = quotedForShell(program);
        for (const std::string &argument : arguments)
            command += " " + quotedForShell(argument);
        return std::system((command + " >/dev/null 2>&1 &").c_str()) == 0;
#endif
    }

    void allowOthersToTakeFront()
    {
#if defined(_WIN32)
        AllowSetForegroundWindow(ASFW_ANY);
#endif
    }

    bool wslAvailable()
    {
#if defined(_WIN32)
        // wsl.exe ships with Windows and sits in System32 whether or not a
        // distribution is installed. If there is none, running it says so in the
        // window the launcher deliberately leaves visible.
        std::error_code ec;
        return fs::exists("C:\\Windows\\System32\\wsl.exe", ec);
#else
        return false;
#endif
    }

    std::string toWslPath(const std::string &path)
    {
        if (path.size() < 2 || path[1] != ':')
            return path;
        std::string out = "/mnt/";
        out += path[0] >= 'A' && path[0] <= 'Z' ? static_cast<char>(path[0] + 32) : path[0];
        for (size_t i = 2; i < path.size(); ++i)
            out += path[i] == '\\' ? '/' : path[i];
        return out;
    }

    bool runInWindow(const std::string &scriptPath, bool elevated)
    {
        if (scriptPath.empty())
            return false;
#if defined(_WIN32)
        const std::string parameters = "/c \"" + scriptPath + "\"";
        if (elevated)
        {
            // "runas" is what raises the consent prompt. A refusal comes back as
            // SE_ERR_ACCESSDENIED, which is a player saying no rather than a failure.
            const std::wstring wideParameters(parameters.begin(), parameters.end());
            const HINSTANCE result =
                ShellExecuteW(nullptr, L"runas", L"cmd.exe", wideParameters.c_str(), nullptr, SW_SHOWNORMAL);
            return reinterpret_cast<INT_PTR>(result) > 32;
        }
        return std::system(("start \"\" cmd.exe " + parameters).c_str()) == 0;
#else
        (void)scriptPath;
        (void)elevated;
        return false;
#endif
    }

    bool bringToFront(const std::string &title)
    {
        if (title.empty())
            return false;
#if defined(_WIN32)
        const std::wstring wide(title.begin(), title.end());
        HWND window = FindWindowW(nullptr, wide.c_str());
        if (window == nullptr)
            return false; // not created yet, or not ours
        ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
        return true;
#else
        return false;
#endif
    }

    bool runScriptDetached(const std::string &scriptPath)
    {
        if (scriptPath.empty())
            return false;
#if defined(_WIN32)
        // `start` returns at once, so the launcher is free to exit while the script
        // waits for it. /min keeps the console window it opens out of the way.
        return std::system(("start \"\" /min cmd /c \"" + scriptPath + "\"").c_str()) == 0;
#else
        // The shell backgrounds the script and exits, so the script outlives the
        // launcher - which is the whole point of it.
        return std::system(("sh " + quotedForShell(scriptPath) + " >/dev/null 2>&1 &").c_str()) == 0;
#endif
    }

    void becomeDpiAware()
    {
#if defined(_WIN32)
        // Per-monitor v2 where the OS has it (Windows 10 1703+), system-wide DPI
        // awareness where it does not. Either way the window is real pixels, so the
        // layout is the size the launcher thinks it is: without this, a display at
        // 150% hands the launcher a window that is 150% of what it asked for, and
        // everything drawn for a 1280x720 layout lands in the wrong place.
        using SetContextFn = BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT);
        if (HMODULE user32 = LoadLibraryW(L"user32.dll"))
        {
            const auto setContext = reinterpret_cast<SetContextFn>(
                reinterpret_cast<void *>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
            if (setContext != nullptr && setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
                return;
            SetProcessDPIAware();
        }
#endif
    }
}

#if defined(_WIN32)
// Laptops with two GPUs - an Optimus one, or an AMD PowerXpress one - run an
// OpenGL process on the integrated GPU unless the executable asks otherwise, and
// the GPU renderer wants the discrete one. These two exported variables are how an
// application asks. Without them the player has to find the vendor's control panel
// and add the game to it by hand, which is not something a player should have to
// know about. (NVIDIA's Optimus documentation names the first; the second is AMD's
// equivalent and is harmless where it does not apply.)
extern "C"
{
    __declspec(dllexport) unsigned long NvOptimusEnablement = 1;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
