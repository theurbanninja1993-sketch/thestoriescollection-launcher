# Cross-compile for 64-bit Windows from Linux with llvm-mingw
# (https://github.com/mstorsjo/llvm-mingw). Pass the toolchain root:
#   cmake -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/llvm-mingw-x86_64.cmake \
#     -DLLVM_MINGW_ROOT=/path/to/llvm-mingw-20241217-ucrt-ubuntu-20.04-x86_64
# The result runs on Windows or under Wine / Proton.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT LLVM_MINGW_ROOT AND DEFINED ENV{LLVM_MINGW_ROOT})
    set(LLVM_MINGW_ROOT "$ENV{LLVM_MINGW_ROOT}")
endif()
if(NOT LLVM_MINGW_ROOT)
    message(FATAL_ERROR "Set LLVM_MINGW_ROOT to the llvm-mingw toolchain directory")
endif()
set(LLVM_MINGW_ROOT "${LLVM_MINGW_ROOT}" CACHE PATH "llvm-mingw toolchain root")
# try_compile projects re-read this file: hand them the root too.
set(ENV{LLVM_MINGW_ROOT} "${LLVM_MINGW_ROOT}")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LLVM_MINGW_ROOT)

set(CMAKE_C_COMPILER "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-windres")
set(CMAKE_AR "${LLVM_MINGW_ROOT}/bin/llvm-ar")
set(CMAKE_RANLIB "${LLVM_MINGW_ROOT}/bin/llvm-ranlib")

set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW_ROOT}/x86_64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Self-contained exe: no libc++ / unwinder DLLs to ship.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
