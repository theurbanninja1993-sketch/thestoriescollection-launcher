# Theme music

Drop each game's theme here, named to match `music=` in `launcher.ini`:

    music/vcs.mp3    Grand Theft Auto: Vice City Stories
    music/lcs.mp3    Grand Theft Auto: Liberty City Stories

**Nothing is shipped here on purpose** — the themes are Rockstar's music, the
same as the box art, so they are yours to supply. Use a copy you own.

A theme plays while its card is highlighted. It starts from the beginning each
time the card is picked, loops, and fades in and out rather than cutting, so
moving between the two swaps one for the other smoothly. `M` mutes, and
`music_volume` under `[launcher]` in `launcher.ini` sets how loud it is (0 turns
menu music off entirely).

`.mp3`, `.ogg`, `.wav` and `.qoa` all work — those are the formats raylib
decodes. The examples above use mp3 because that is the common one.

A file that will not decode is reported on the console and simply leaves that
card quiet; nothing breaks, and the rest of the menu carries on. The same is
true of a missing file: no theme is not an error.

Once *any* theme is found, the one-line hint the menu shows at the bottom saying
where to put one disappears for good. Until then it is how you find this folder.
