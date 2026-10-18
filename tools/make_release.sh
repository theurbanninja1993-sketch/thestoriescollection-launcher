#!/usr/bin/env bash
# Assembles a release folder for the launcher.
#
#   tools/make_release.sh [options]
#
#     --out DIR       where the folder goes (default: ./dist)
#     --linux         build the Linux launcher (the default if no platform is given)
#     --windows       also cross-compile the Windows launcher
#     --toolchain DIR use this llvm-mingw instead of looking for one
#
# A release is the launcher and nothing else. The game's source is fetched by the
# launcher itself, and the game comes off the player's own disc, so there is no game
# in here and cannot be: shipping one would mean shipping the recompiled game code.
#
# The folder is portable: everything the launcher writes stays beside it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/dist"
DO_LINUX=0
DO_WINDOWS=0
TOOLCHAIN=""

while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="$2"; shift ;;
        --linux) DO_LINUX=1 ;;
        --windows) DO_WINDOWS=1 ;;
        --toolchain) TOOLCHAIN="$2"; shift ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done
[ $DO_LINUX -eq 0 ] && [ $DO_WINDOWS -eq 0 ] && DO_LINUX=1

say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf '\n\033[31mFAILED: %s\033[0m\n' "$*" >&2; exit 1; }
run()  { "$@"; }

VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"
[ -n "$VERSION" ] || die "VERSION is empty"
NAME="thestoriescollection-launcher-$VERSION"

# ---------------------------------------------------------------- finding a toolchain
# Same order as the port's build.sh: one named, one the environment points at, one
# this repository already fetched, then fetch one. Only needed for --windows.
llvm_mingw_version="20241217"
llvm_mingw_asset="llvm-mingw-${llvm_mingw_version}-ucrt-ubuntu-20.04-x86_64"
llvm_mingw_url="https://github.com/mstorsjo/llvm-mingw/releases/download/${llvm_mingw_version}/${llvm_mingw_asset}.tar.xz"
toolchain_dir="$ROOT/toolchains"

toolchain_works() { [ -n "${1:-}" ] && [ -x "$1/bin/x86_64-w64-mingw32-clang++" ]; }

ensure_toolchain() {
    if toolchain_works "$TOOLCHAIN"; then
        note "llvm-mingw: $TOOLCHAIN (given)"
        return 0
    fi
    if toolchain_works "${LLVM_MINGW_ROOT:-}"; then
        TOOLCHAIN="$LLVM_MINGW_ROOT"
        note "llvm-mingw: $TOOLCHAIN (from LLVM_MINGW_ROOT)"
        return 0
    fi
    if toolchain_works "$toolchain_dir/$llvm_mingw_asset"; then
        TOOLCHAIN="$toolchain_dir/$llvm_mingw_asset"
        note "llvm-mingw: $TOOLCHAIN (already fetched)"
        return 0
    fi
    note "fetching llvm-mingw, 88 MB, once"
    run mkdir -p "$toolchain_dir"
    run curl -fL --retry 3 -o "$toolchain_dir/$llvm_mingw_asset.tar.xz" "$llvm_mingw_url"
    run tar -xJf "$toolchain_dir/$llvm_mingw_asset.tar.xz" -C "$toolchain_dir"
    TOOLCHAIN="$toolchain_dir/$llvm_mingw_asset"
    toolchain_works "$TOOLCHAIN" || die "llvm-mingw did not unpack where expected ($TOOLCHAIN)"
}

# ------------------------------------------------------------------- the folder
# Everything a player sees, in one place, with nothing that only matters to whoever
# built it.
assemble() {
    local platform="$1" binary="$2" binary_name="$3"
    local dest="$OUT/$NAME-$platform"
    rm -rf "$dest"
    mkdir -p "$dest/art" "$dest/music" "$dest/discs"

    cp "$binary" "$dest/$binary_name"
    cp "$ROOT/launcher.ini" "$ROOT/LICENSE" "$ROOT/README.md" "$dest/"
    cp "$ROOT/art/README.txt" "$dest/art/"
    cp "$ROOT/music/README.txt" "$dest/music/"

    cat > "$dest/START-HERE.txt" <<EOF
The Stories Collection - launcher $VERSION
$(printf '=%.0s' $(seq 1 $((38 + ${#VERSION}))))

This folder holds the launcher and nothing else. The games are not in it, and
cannot be: what makes them is your own PlayStation 2 disc.

    1. Run the launcher.       Linux: ./ps2x_launcher     Windows: ps2x_launcher.exe
    2. Press U on a game.      Fetches that game's source. Source only, never a build.
    3. Press B on that game.   Builds it from your disc, and shows where it is up to.
    4. Click it, or press Cross, to play.

Step 3 wants a C++ toolchain and cmake and takes about twenty minutes. If the
wizard cannot find your disc, put the image in the discs/ folder beside this file
(an .iso, a .bin, a .cue sheet or a .chd all read), or insert the disc and press B
again. A build can be stopped with Esc and started again, and whatever it printed
is kept in logs/ beside this file.

Everything the launcher writes stays in this folder, so it can live on a USB stick.

Not affiliated with Rockstar Games or Take-Two Interactive. It contains no game
code and no game data of any kind.
EOF

    [ "$platform" = "linux" ] && chmod +x "$dest/$binary_name"

    say "checksums"
    ( cd "$dest" && find . -type f ! -name CHECKSUMS.txt -print0 | sort -z | xargs -0 sha256sum > CHECKSUMS.txt )
    note "$dest"

    # And an archive of it, because a release attaches files rather than folders.
    # Both formats for both platforms: a tar.gz is what a Linux player expects and a
    # zip is what a Windows one can open without being told how.
    ( cd "$OUT" && tar -czf "$NAME-$platform.tar.gz" "$NAME-$platform" )
    note "$OUT/$NAME-$platform.tar.gz"
    if command -v zip >/dev/null 2>&1; then
        ( cd "$OUT" && zip -qr "$NAME-$platform.zip" "$NAME-$platform" )
        note "$OUT/$NAME-$platform.zip"
    elif command -v 7z >/dev/null 2>&1; then
        ( cd "$OUT" && 7z a -tzip -bso0 -bsp0 "$NAME-$platform.zip" "$NAME-$platform" >/dev/null )
        note "$OUT/$NAME-$platform.zip"
    else
        note "neither zip nor 7z here, so no .zip for $platform"
    fi
}

# ------------------------------------------------------------------- platforms
if [ $DO_LINUX -eq 1 ]; then
    say "building the launcher for Linux"
    run cmake -S "$ROOT" -B "$ROOT/build-release" -DCMAKE_BUILD_TYPE=Release
    run cmake --build "$ROOT/build-release" --target ps2x_launcher -j "$(nproc)"
    [ -x "$ROOT/build-release/ps2x_launcher" ] || die "no binary was produced"
    run strip "$ROOT/build-release/ps2x_launcher" 2>/dev/null || true
    assemble linux "$ROOT/build-release/ps2x_launcher" ps2x_launcher
fi

if [ $DO_WINDOWS -eq 1 ]; then
    say "cross-compiling the launcher for Windows"
    ensure_toolchain
    note "llvm-mingw: $TOOLCHAIN"
    run cmake -S "$ROOT" -B "$ROOT/build-win" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchains/llvm-mingw-x86_64.cmake" \
        -DLLVM_MINGW_ROOT="$TOOLCHAIN"
    run cmake --build "$ROOT/build-win" --target ps2x_launcher -j "$(nproc)"
    [ -f "$ROOT/build-win/ps2x_launcher.exe" ] || die "no .exe was produced"
    assemble windows "$ROOT/build-win/ps2x_launcher.exe" ps2x_launcher.exe
fi

say "done"
note "folders in $OUT, each self-contained and portable"
note "attach them to a release; the launcher's own version is in VERSION"
