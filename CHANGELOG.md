# Changelog

## Unreleased

These changes are not in the 0.1.1 runtime archive that Plebian-OS 0.2.3 pins.

- Keep the health tests independent of the machine they run on. The combined health test read the live restart check and failed wherever a newer kernel or driver was waiting. A second test covers the restart cap.
- Compile the Neuronte objects again when a build names another Neuronte checkout or weights file. The earlier build kept the objects from the previous checkout.
- Name the Neuronte commit 0.1.1 was reviewed against, and say where the release carries Zell.
- Take soft-raster out of the Neuronte programs. Their pixel buffer, frame clear, flat fill, RGBA packing and PPM writer were first moved to `src/canvas.c`, adapted from soft-raster with its notice (a later entry says where they are now), which gives the same bytes as soft-raster did. The raster build is unchanged.
- Draw the stair marker and the floating window's close button from Neuronte characters in the Neuronte build. Both were drawn by soft-raster's triangle and line. The marker sits within a pixel of where it was. The cross is slightly bolder and can sit half a pixel off the button's middle, because its cell is placed on whole pixels.
- Show the later staff the way the first six are shown: no name until Kilix stands beside one, then the glow and the name. The ten operating-system staff, and the Git Wizard, Pilot and Key Master downstairs, carried a name tag all the time. The three who stand in the cave also get the floor shadow the first six have.
- Move the Neuronte programs' canvas into the `neuronte-sdk` module, for the 0.2.4 line. `src/canvas.c` now only carries Zell's canvas struct across to `nsdk_canvas_*`; the build compiles that module's one canvas source (`NSDK_DIR`), and the runtime gains `licenses/neuronte-sdk`. Frames are the same bytes as before. A release that pins Zell after this change must pin a `neuronte-sdk` that has the canvas and add the licence file to its package list. The build records which `neuronte-sdk` checkout the canvas objects came from and compiles them again when another is named.
- Take the stairs out of the Command Center. Kilix leaves by walking off the bottom of its floor, anywhere along it, and a row of arrows marks the edge. The Key Master moves to the bottom right corner, with his name and hint above his head so the status bar does not cover them. The other downstairs rooms keep their stairs.
- Add the Command computer in the bottom left corner of the Command Center. Enter beside it opens a shell in the floating terminal, started in `tools/mana`, where `set-mana`, `show-mana` and `clear-mana` tell Zell by hand how much each provider has left. A value set by hand stands until it is cleared, set again, or a good automatic reading is taken after it; the Mana console and the MANA bar show it. The runtime archive gains five files: the three scripts, their notes and `tools/zell_mana_manual.py`.

## 0.1.1 — 2026-11-13

Rooms:

- Add the Vault and the Workshop downstairs, beside the Command Center and the Laundry. The four cave hatches match in size and spacing.
- Add a game room, a movie theater, a meditation room and a library through a second row of hatches. Each fits the pane between the top bar and the status line, and has a foreground layer Kilix walks behind.
- Fit every room on smaller panes with one transform shared by drawing, collision and clicking.
- Walk onto the stairs and behind standing things.
- Replace the coded room placeholders with generated backgrounds, doors, stairs, monitor frames, drive bins and floor panels. Git Wizard, Pilot and Key Master have their own four-frame animations.

Movie theater:

- Open a video file, a folder of videos, or an http or https page on the theater screen.
- Choose Chrome, Chromium or Firefox ESR for a page. A missing browser is installed in the floating terminal after a confirmation, and only the one chosen.
- Play, pause and queue videos. Each queued video keeps its own position; one source plays at a time.
- Show captions from a matching .srt or .vtt file in a separate caption bar. Embedded subtitle tracks are not read.
- Toggle fullscreen with F11 and leave it with Escape.
- Pop the active video out into the floating window and dock it back.

Staff and tasks:

- Add ten operating-system staff: Gatekeeper, Signalman, Porter, Laundress, Merchant, Herald, Timekeeper, Sentinel, Vault Keeper and Tinkerer. Their tasks run in private floating terminals and ask for a typed word before a change.
- Add Key Master, backed by kilix-secrets: set up, unlock, recover and lock the vault, save keys and logins, copy a field to the clipboard for 20 seconds, and launch a coding client with a saved key.
- Search every room's actions by name from the Tab launcher.
- Add a task board and opt-in conversation history on the Noticeboard. Key Master and terminal contents are never saved.
- Show each new notice on screen for eight seconds.
- Add a Workshop computer that opens a shell in the floating window, with a stool Kilix hops onto.

Command Center:

- Enlarge the five screens and show bounded text previews of their coding sessions, taken only from verified panes.
- Add the YOLO switch, which reads and saves Kilix's shared setting.
- Add the agent-updates switch: choose installed clients and update them in the floating terminal.
- Add the Mana console, which shows what each coding provider has left and reads again every 30 minutes unless that is turned off.
- Save Pilot fleets with their launch settings and startup instructions sealed in the vault. The saved file keeps references only, and older plaintext files are refused.

Cave:

- Show illustrated weather on the wall map from the weather widget's cached reading.
- Add three levers below the map for voice, dictation and music. DJ playback and the background track pause each other.
- Mount the Neuronte shell lever on the east wall. The shell follows later pane resizes and ends when its program exits.
- Give the DJ a text browser in the floating window for finding playlists and creators.
- Add minimize and maximize buttons to the floating window, with a bottom-row button that reopens it.

Fixes:

- Keep menu choices intact when a Mana reading arrives. Both used the same message letter, so a reading could select the wrong action.
- Keep the theater picture when folder browsing or a rejected file leaves the current video unchanged.
- Route automatic local-model questions to model names that are installed and supported.
- Make archive-only folders a per-machine setting. Nothing is reserved by default.

Packaging and tools:

- `make install` installs the Neuronte desktop, its helper scripts, font weights, terminal renderer, vault binaries, user service units and the `zell` command.
- `make runtime` writes a relocatable archive with an exact file manifest and dependency notices.
- Snapshots are written as PNG beside every PPM.
- The generated art is compiled as one file per source sheet.

## 0.1.0 — 2026-11-11

- Add Health/Mana/Fatigue bars, health-hover reasons, OS-age and opt-in dirty-file penalties, and resource-sensitive movement.
- Add an interactive Neuronte PTY update window; token usage refresh remains explicit and shares Claudex's hourly cache evidence.
- Add downstairs Command Center and Laundry, five coding-tab screens, disk-capacity bins, Git Wizard and Pilot.
- Continue release integration on RC9 without changing frozen RC8 refs.

- Add Shift-held, boots-style running with a short charge, straight dash, dust, faster steps and collision stops.

- Add in-game disk-library guidance, file/folder indexing, search and refresh for music, books and documents.
- Install `zell` as the stable Neuronte launcher with helper scripts and bundled font weights.

- Add a reachable wall lever behind the shell computer that opens a Neuronte-rendered shell in a Kilix tab.

Station windows:

- Keep station menus, text input, confirmations, reports and file previews inside the cave.
- Add paged, filterable choices, scrollable reports and cancellable background helpers.
- Open interactive terminal tools in separate Kilix tabs, preserving the cave.
- Test helper protocol, keyboard round trips, report routing and tab-launch errors.

DJ improvements:

- Open music in a separate Kilix Amp window, including cached audio from URLs.
- Follow creators and playlists, refresh videos/Shorts/streams, and track unseen uploads.
- Transcribe selected media alongside playback with CPU Whisper jobs, progress, cancellation and saved text.

Sprite corrections:

- Import PNGs as premultiplied ARGB to eliminate colored backgrounds and halos around transparent cutouts.
- Filter once at the fixed world display size, preserving antialiased edges without a second nearest-neighbor resize.
- Add staggered staff and hero idle motion, match the seven-tick walk cadence, and stop the walk cycle when movement is blocked.
- Verify premultiplied pixels and visible idle motion in both renderers.


- Newly illustrated cave desktop, green-clad Kilix firekitten, four-direction walk cycle and six staff characters.
- Work areas and two computers along the perimeter, with an open central floor.
- Fixed 1536×1024 world and following camera: resizing changes the view, not object sizes.
- Minimap, clock, interaction labels, scrolling station menus and Tab quick launcher.
- Archivist log search; Administrator settings, packages and services; Housekeeper health and cache cleanup; DJ media/playlists; Librarian books, OCR, documents and audiobook export.
- Separate Kilix TUI and shell computers that return to the cave. Wall map, notebook, voice control and clock.
- Preserve Ask-first one-shot local models, voice, reply scrolling and history. Detach question processes from the controlling terminal so replies stay inside Zell.
- Keep Neuronte split-v4. No Plebian-OS release manifest change.
