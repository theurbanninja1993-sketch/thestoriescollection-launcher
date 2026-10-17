cmake_minimum_required(VERSION 3.20)
project(ps2x_launcher CXX)

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
add_executable(ps2x_launcher src/main.cpp src/builder.cpp src/builder.h)
target_link_libraries(ps2x_launcher PRIVATE raylib)

# The launcher is meant to sit in a folder next to the games it starts, so the
# executable has to find its own directory to resolve relative paths.
if(WIN32)
    target_compile_definitions(ps2x_launcher PRIVATE PS2X_LAUNCHER_WINDOWS)
endif()
