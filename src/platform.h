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

    // Opens a URL in whatever this desktop uses for one. False only when the
    // launcher could not even start it, which is all a caller can act on.
    bool openUrl(const std::string &url);

    // Commands that make a directory, remove a tree and remove a file, in whatever
    // shell popen hands the string to. Windows has no `mkdir -p` and no `rm`, so
    // these are different commands rather than the same one under another name.
    std::string makeDirectoryCommand(const std::string &path);
    std::string removeTreeCommand(const std::string &path);
    std::string removeFileCommand(const std::string &path);

    // What joins two commands here. cmd has no `;` and a POSIX shell treats `&` as
    // "run this in the background", so there is no one string that means "and then
    // do this regardless" to both.
    const char *commandSeparator();

    // A command that puts the shell in a directory, so everything after it can use
    // plain relative names. Worth having: a Windows path in a tar command is
    // `C:/...`, and the `C:` is read as a remote host by the MSYS tar that a Git
    // install puts on the PATH.
    std::string changeDirectoryCommand(const std::string &path);

    // Wraps a whole command so that everything it prints - including what the shell
    // itself says when a program cannot be found - is captured. Without it, `popen`
    // takes stdout only and the reason a step failed goes to a console that a GUI
    // player does not have.
    std::string captureEverything(const std::string &command);

    // Whether a tool can be run, so a failure can name what is missing instead of
    // showing a player a raw shell error.
    bool commandExists(const std::string &command);

    // Starts a script and returns at once, leaving it to outlive this program. The
    // launcher's own update needs exactly that: on Windows a running program's files
    // cannot be replaced, so a script has to do the replacing after the launcher has
    // gone - and it has to still be running when the launcher exits.
    bool runScriptDetached(const std::string &scriptPath);

    // Real pixels instead of a DPI-scaled window, which is what makes the layout
    // the size it thinks it is on a display at 125% or 150%. A no-op away from
    // Windows, and it has to be called before the window exists.
    void becomeDpiAware();
}
