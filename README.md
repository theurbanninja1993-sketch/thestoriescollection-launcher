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
- **Fullscreen by default** (F11 swaps), and the whole menu scales to the
  display, so it fills a 1440p monitor rather than sitting small in the middle.
- Launches whatever is already built, per the paths in `launcher.ini`.
- A game marked `available=0` is dimmed, marked `COMING SOON!`, and will not
  launch.
- **Each card can carry its own theme music**, from `music/`. It starts from the
  opening when a card is picked, fades in and out rather than cutting, swaps
  smoothly when you move between the two, and loops. `M` mutes.
- **It fetches the port's source for you.** Press `U` on a card and the launcher
  downloads or updates that game's source itself, so this repository is the whole
  download. Source only — never a built game. The card says where it stands.
- **Playing a game is a round trip.** Choosing one blanks the screen while the
  game runs and hands it the foreground; when the game closes, however it was
  closed, the menu comes back lit with its theme playing again. The game's own
  settings say *"Return to game select"* rather than *"Quit to desktop"* — because
  from the launcher, that is where it goes.

## What is coming

- **Phase 2 — the builder wizard.** Fetching the source is already in, on the `U`
  key. The wizard takes it from there: point it at your own disc (read straight
  from the drive, or a `.iso` / `.bin` you already made) and it verifies the disc,
  extracts it, runs the VU1 learning passes and compiles the game, then hands
  you back to this menu. Nothing of Rockstar's is shipped — your disc produces
  it. `U` plus a disc becomes the whole install.
- **Phase 3 — the build orchestration** (bundled toolchain, progress and ETA).
- **Phase 4 — packaging per OS.**

## Where the game comes from

Nothing but **source** is ever downloaded — never a built game. What makes the game
is your own disc, recompiled on your own machine. That is deliberate: it is why
this repository holds no game files of any kind, and it is also why the result
works on machines an already-built binary would refuse to start on.

The launcher fetches that source itself, so **downloading this repository is the
whole download**. Sit on a card and press `U`. The card reports where its source
stands — `source not fetched`, `source update available`, or the revision it
already has — and an update is only ever taken when you ask for it.

Then build it, from the folder it arrived in:

```sh
cd port/vcs && ./build.sh --image /dev/sr0     # or a .iso / folder you have
```

That needs a C++ toolchain and cmake, and takes roughly 20 minutes. It produces
`port/vcs/dist/`, which is exactly what the menu launches.

## Building the launcher itself

```sh
cmake -S . -B build
cmake --build build --target ps2x_launcher -j
./build/ps2x_launcher
```

raylib is fetched automatically. There are no other dependencies.

## Configuring

Everything lives in `launcher.ini` next to the executable: the games, their source
repositories, their box art, their theme music, whether the menu fills the screen,
and whether each game is available. See the comments in the file. The runner and
the player's own game files are worked out from wherever the source was fetched
to, so there is normally nothing to set beyond the repository address.

`--config <file>` reads a different file instead, which is how a developer keeps
their own build paths — `launcher.dev.ini` here is one, and it is not tracked
because it is specific to this machine.

Box art goes in `art/` — see `art/README.txt`. Theme music goes in `music/` — see
`music/README.txt`. **Neither is shipped**: the covers and the themes are
Rockstar's, so those are the two things the launcher asks you to supply yourself.
A game with no theme is simply quiet — it is never an error.

`[launcher] music_volume` sets how loud the themes are, 0 to 1. Set it to 0 to
turn menu music off entirely.

## Handy flags

- `--config <file>` — read a different config instead of `launcher.ini`.
- `--screenshot <file.png>` — render one frame and exit. Always windowed, at
  1280x720, so a capture is the same size whatever the display. Used to check the
  layout without a person at the screen.
- `--select <n>` — start with the nth card highlighted (pairs with
  `--screenshot` to capture a specific card's state).

## Layout

    launcher.ini        the games, their source, their paths and their state
    art/                box art (supplied by you)
    music/              theme music (supplied by you)
    port/               the port source the launcher fetches (created on demand)
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
