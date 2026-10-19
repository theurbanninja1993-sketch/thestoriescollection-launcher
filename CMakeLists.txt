cmake_minimum_required(VERSION 3.20)
# C as well as C++: raylib is C, and src/glibc_compat.c - the maths wrappers that keep
# the binary from asking for a glibc newer than any player has - is C too.
project(ps2x_launcher C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# raylib only: the menu is simple enough to draw directly, and ImGui can come in
# with the builder wizard (phase 2) where forms actually need it.
include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
)
FetchContent_MakeAvailable(raylib)

# builder.cpp is the build wizard's engine: the steps that turn a fetched source
# plus the player's disc into a game.
# platform.cpp holds the handful of things that differ per platform. It exists
# because raylib and <windows.h> cannot share a translation unit, and the
# compiler says so the moment anyone tries to build this for Windows.
add_executable(ps2x_launcher src/main.cpp src/builder.cpp src/builder.h
                              src/platform.cpp src/platform.h src/glibc_compat.c)

# One version, in one file, so a release and the binary cannot disagree about which
# one it is.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PS2X_LAUNCHER_VERSION)
string(STRIP "${PS2X_LAUNCHER_VERSION}" PS2X_LAUNCHER_VERSION)
message(STATUS "ps2x-launcher version ${PS2X_LAUNCHER_VERSION}")
target_compile_definitions(ps2x_launcher PRIVATE PS2X_LAUNCHER_VERSION="${PS2X_LAUNCHER_VERSION}")
target_link_libraries(ps2x_launcher PRIVATE raylib)

# Built on a rolling-release distribution, where libm's float maths functions carry
# the newest symbol version - so the binary asks for GLIBC_2.43 and refuses to start
# on any Ubuntu LTS. --wrap catches every reference, raylib's included, and
# src/glibc_compat.c implements each one on top of the double-precision version that
# every glibc has had for twenty years. A launcher has to run on the player's
# machine, and the player's machine is not this one.
if(NOT WIN32)
    target_link_options(ps2x_launcher PRIVATE
        "-Wl,--wrap=sqrtf" "-Wl,--wrap=acosf" "-Wl,--wrap=asinf" "-Wl,--wrap=atan2f"
        "-Wl,--wrap=fmod" "-Wl,--wrap=fmodf")
endif()

# The launcher is meant to sit in a folder next to the games it starts, so the
# executable has to find its own directory to resolve relative paths.
if(WIN32)
    target_compile_definitions(ps2x_launcher PRIVATE PS2X_LAUNCHER_WINDOWS)
endif()
