// The few things that differ between platforms, in a file that deliberately does
// not include raylib.
//
// raylib and <windows.h> cannot be seen in the same translation unit: raylib
// declares Rectangle and CloseWindow and ShowCursor, and Windows declares all
// three as something else, so the compiler refuses. The port learned this years
// ago and keeps to it; the launcher had a <windows.h> include next to raylib.h
// from the start, which nobody noticed because it had never been built for
// Windows.
#pragma once

#include <string>
#include <vector>

namespace platform
{
    // The folder this binary is running from, so a portable layout works wherever
    // it is dropped. Empty if it cannot be worked out.
    std::string executableDirectory();

    // Optical drives that can be read as an image path: /dev/sr0, or D:.
    std::vector<std::string> opticalDrives();

    // Whether a file can be run. Windows has no execute bit, so anything there is
    // runnable as far as this is concerned.
    bool fileIsExecutable(const std::string &path);
}
