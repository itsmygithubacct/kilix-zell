# Third-party notices

`kilix-zell` is MIT licensed. See [LICENSE](LICENSE).

## Shellda sprites

`SPR_EN_WIZARD` in `src/sprites.c` is copied from Shellda's
`src/generated_sprites.h`.

Shellda is MIT licensed:

```text
Copyright (c) 2026 shellda contributors
```

The permission notice is the standard MIT notice in Shellda's `LICENSE`.
That frame was cropped from the Gemini-generated sheet
`enemies_gemini_sheet.png`. The bytes are stored with the alpha in the
high byte. Every copied wizard pixel is fully transparent or fully opaque.

Kilix's eight walk frames are cropped from Kilix Land
`assets/graphics/casts/legend-player.png` (the same bytes as that game's
`assets/graphics/kilix.png`). The cells are columns 0 and 4 of rows 0, 2,
4, and 6, which is the pair Kilix Land alternates while walking. Kilix
Land is MIT licensed:

```text
Copyright (c) 2026 itsmygithubacct
```

Magenta left on the wizard's outline by the sheet key is dropped when that
frame is copied. The hero frames are not a byte copy of Shellda's Clink sheet.

`TILE_STONE`, `TILE_WALL`, `TILE_DARK`, and `TILE_STAIRS` in `src/room.c`
are Shellda's `SPR_TILE_STONE`, `SPR_TILE_ROCK_WALL`, `SPR_TILE_DARK`, and
`SPR_TILE_STAIRS_UP`. Those are the tiles Shellda uses for the starting cave.
`HEART` is Shellda's `SPR_DROP_HEART`. A full heart is that sprite. An
empty heart is the same sprite drawn lighter.

## Legend of Kilix campfire

`CAMPFIRE` in `src/room.c` is one cell of Legend of Kilix
`assets/graphics/effects/vfx.png` (column 4, row 1).

Legend of Kilix is MIT licensed:

```text
Copyright (c) 2026 itsmygithubacct
```

The fires are that sprite, placed on fixed floor cells. Three more frames
mirror its flame and slide its sparks so the fire flickers. The log ring
is the same pixels in every frame. They are not a redraw of the sheet.

The module does not need the Shellda checkout at build time or at run time.

## Libraries

The build links sibling checkouts by path. They are not copied into this
repository. `MODULES` overrides the default `../../kilix-modules`.

| Library | Role |
| --- | --- |
| `kitty-framebuffer` | Alternate-screen RGBA frame in the current pane |
| `kitty_keyboard` (inside `kitty-input`) | Key state and typed text |
| `soft-raster` | Black frame, scaled sprites, and menu text |

Each library keeps its own MIT notice.

`zell-neuronte`, the optional build, also links Neuronte (`libneuronte.a`)
and loads its weight file at run time. Neuronte is MIT licensed, and its
weights and shapes were made for that project. Neither is copied into
this repository.

## Paradee

Answers are spoken by Paradee 8M, voice `bm_fable`, when
`/usr/lib/kilix/paradee` and its Python are installed.
`tools/paradee_speak.py` calls that ONNX. The weights are not in this
repository. Paradee is Apache-2.0.

## Zell Neuronte 0.1.0 artwork

The cave, cast, workstation sheet and walk atlas were generated for this
version with the built-in image generation tool. Source PNGs and production
prompts live under `assets/v0.1.0`. They are compiled separately from the legacy
Shellda and Kilix Land assets. Kilix wears a green fantasy adventurer outfit.
