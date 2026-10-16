# ps2x-launcher

The front end for the recompiled PS2 GTA games. One window, one menu, and each
game is a card you click.

![menu](docs/menu.png)

Two ports share it — **Liberty City Stories** and **Vice City Stories** — because
they are the same runtime underneath; only the generated game code differs. LCS
is still in progress, so its card says `COMING SOON!` until it is ready.

## Built with AI

This launcher was written with substantial help from AI: the code came from
coding assistants under human direction, and every change was built and run
before it was kept.

It is said plainly, and early, because some people would rather not use software
made this way. That is a fair position to hold, and you have found out before
spending an evening on it rather than after.

## What it does today (phase 1)

- The menu: two cards with **your own box art**, mouse hover and d-pad / stick
  navigation, click or Cross to play, Esc to quit.
- Launches whatever is already built, per the paths in `launcher.ini`.
- A game marked `available=0` is dimmed, marked `COMING SOON!`, and will not
  launch.

## What is coming

- **Phase 2 — the builder wizard.** Point it at your own disc (read straight
  from the drive, or a `.iso` / `.bin` you already made): it verifies the disc,
  extracts it, runs the VU1 learning passes and compiles the game, then hands
  you back to this menu. Nothing of Rockstar's is shipped — your disc produces
  it.
- **Phase 3 — the build orchestration** (bundled toolchain, progress and ETA).
- **Phase 4 — packaging per OS.**

## Building it

```sh
cmake -S . -B build
cmake --build build --target ps2x_launcher -j
./build/ps2x_launcher
```

raylib is fetched automatically. There are no other dependencies.

## Configuring

Everything lives in `launcher.ini` next to the executable — the games, their
paths, their box art and whether they are available. See the comments in the
file; the one thing to change first is the paths, which start out pointing at
this machine's development builds.

Box art goes in `art/` — see `art/README.txt`.

## Handy flags

- `--screenshot <file.png>` — render one frame and exit. Used to check the
  layout without a person at the screen.
- `--select <n>` — start with the nth card highlighted (pairs with
  `--screenshot` to capture a specific card's state).

## Layout

    launcher.ini        the games, their paths and their state
    art/                box art (supplied by you)
    src/main.cpp        the menu
    games/              (release layout) one folder per game, beside the launcher

A release is a folder the player can move anywhere: the launcher, `launcher.ini`
and the `games/` folders travel together, and relative paths in the config
resolve beside the executable.

## Licence

GPL-3.0 — the full text is in `LICENSE`. The same licence as the port it
launches, so the whole project is under one licence.

The box art in `art/` is yours, not part of this repository, and is not covered
by it. Neither is the game itself: the launcher only starts a build you made
from your own disc.
