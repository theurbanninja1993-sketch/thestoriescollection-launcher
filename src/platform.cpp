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
}
