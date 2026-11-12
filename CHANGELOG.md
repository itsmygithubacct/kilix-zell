# Changelog

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
