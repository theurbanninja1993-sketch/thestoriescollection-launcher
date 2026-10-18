#include "platform.h"

#include <filesystem>

#if defined(_WIN32)
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

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
