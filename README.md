# kilix-zell

Walk Kilix, the firekitten, up to the Old Wizard in the current Kilix pane
and ask a question. Kilix Ask answers when it can. Otherwise an installed
model does. Kilix is the legend-cast walk pair from Kilix Land. Walking off
the bottom of the frame returns to the shell.

This is an unpinned RC8 candidate. It is not in a Plebian-OS release
manifest, and it does not add a `kilix` launcher verb.

## Run

```sh
make
~/gpu_terminal/kilix-apps/kilix-zell/zell
```

An interactive shell can use:

```sh
alias zell="$HOME/gpu_terminal/kilix-apps/kilix-zell/zell"
```

The program must be started on a terminal. Arrow keys or WASD walk. Enter
beside the wizard opens Ask. A blank field sits under that word. Type a
question there, or choose one with the arrow keys. Enter asks, and Esc steps
back. Esc does not leave the walk. Ctrl-C does. Rock walls and the
dark alcove block the walk. Walking through a campfire sets Kilix on fire for
a moment and spends hearts. At zero hearts Kilix returns to the start of the
cave. The documented way back to the shell is to walk off the bottom through
the stairs.

Kilix Ask is tried first: `kilix ask -- <question>` with no `--yes`. If that
does not answer, a larger installed model is used when it starts to respond
within 10 seconds. Otherwise Qwen3.5 0.8B answers. The model command is
`kilix-llm run --model-id <id> -n 160`. Ask waits up to 45 seconds. A model
waits up to 180 seconds. Esc during the wait cancels it.

The answer fills the pane. Up and Down scroll it when the reply is longer
than the box, and Enter closes it. Paradee 8M reads the whole reply, in
`bm_fable`.

A switch on the back-left wall turns that voice on or off. A wooden sign
above it reads "voice". Stand in front of the switch and press Enter.
Voice starts on. The switch is saved at
`~/.local/gpu_terminal/kilix-zell/state/voice.json`, or under
`GPU_TERMINAL_HOME` when that is set. A missing Paradee runtime leaves the
switch working and stays quiet.

Preset questions:

- What time is it?
- How much disk is free?
- What is three times five?
- How do I list panes?
- What's the weather in Tokyo?

Your own question goes in the blank field under Ask.

## Build

C11, `libz`, and `libm`. The default `MODULES` path is the sibling
`kilix-modules` tree (`kitty-framebuffer`, `kitty_keyboard`, and
`soft-raster`).

```sh
make test
make
```

### The Neuronte renderer

`make RENDER=neuronte` builds a second program, `zell-neuronte`, beside
`zell`. In it Neuronte nets draw the frame: the text, the outlines (thin
block characters), the switch knob and Kilix's shadow (the circle
character), and the sprites and tiles, whose every pixel goes through
2 by 4 octant cells. Flat rectangle fills stay plain code. The layout is
shared with `zell`: `src/draw_neuronte.c` gives the soft-raster calls a
Neuronte body and includes `src/draw.c`.

It needs a Neuronte checkout with `make` run in it, at `NEURONTE`
(default `~/projects/neuronte`), and loads `neuronte-split-v4.nrt` from
there. `ZELL_NEURONTE_WEIGHTS` names another weight file at run time.
`make test-neuronte` runs the scene tests against this renderer and its
own checks, which are skipped when the weight file is absent. `make snap`
builds `build/snap` and `build/snap-neuronte`, which write a picture of
each screen without opening a pane.

`make test` checks the walk, the questions, the command arguments, the
model router, the voice switch, and that the cave tiles, both sprites, and
the campfires land on the frame. It does not open a pane, launch a model,
or play audio. `src/sprites.c` and `src/room.c` are generated input.
From `kilix-apps/kilix-zell`:

```sh
python3 tools/paint_kilix.py
python3 tools/paint_room.py
```

The first crops Kilix Land's `legend-player.png` and refreshes the wizard
from Shellda. The second copies Shellda's cave tiles and Legend of Kilix's
campfire. A normal build does not run either command.
`tools/extract_sprites.py` writes Clink's old frames and an API this tree
no longer uses.
