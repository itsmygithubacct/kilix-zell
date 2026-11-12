# Zell Neuronte 0.1.1

A walkable Kilix desktop inside a terminal pane. Kilix is an orange firekitten
in a green adventurer outfit. Six staff members help with questions, logs,
settings, system health, media, and documents.

The cave is a fixed **1536×1024 world**. Characters and furniture keep their
size. The camera follows Kilix; smaller panes show less of the room. Work
areas sit along the walls, leaving the central floor open.

## Start

```sh
make neuronte
./zell-neuronte
./zell-neuronte --version
make install
zell --version
```

Requires Kitty graphics, C11, zlib, libm and Python 3. The default Neuronte
dependency is `~/projects/neuronte`, with its library already built and
`weights/neuronte-split-v4.nrt` present. `NEURONTE` and `NEURONTE_WEIGHTS`
override build paths. `ZELL_NEURONTE_WEIGHTS` overrides the runtime weight file.
The build does not modify that dependency.

`make` also builds the original `zell` scene. The new desktop is in
`zell-neuronte`. This is a local RC9 candidate, not a Plebian-OS release pin.

## Controls

| Key | Action |
| --- | --- |
| Arrow keys / WASD | Walk; Up/Down select menu rows |
| Hold Shift | Charge briefly, then dash straight ahead; release to stop |
| Enter | Interact with a nearby character or object; open an action |
| Tab | Open/close the quick launcher |
| Esc | Back from menus; cancel questions; open launcher while walking |
| Up / Down in an answer | Scroll the reply |
| Ctrl-C | Exit Zell |
| South doorway | Walk back to the launching shell |

The minimap shows Kilix, stations and viewport. The launcher reaches all
stations without walking. Menus scroll on smaller panes.

## Work areas

| Station | Actions |
| --- | --- |
| Wizard | Kilix Ask, then a local model; typed questions and ranked history |
| Archivist | System/user journals, boot warnings, agent rollouts, Kilix transcripts, literal log search and other log files |
| Administrator | Kilix settings, preview/install Debian packages, installed packages, user-service status/start/restart/stop, network status, GPU/model selection |
| Housekeeper | Memory/load/uptime/GPU reports, free space, home-folder usage, processes, old thumbnail cleanup and package-cache cleanup |
| DJ | Kilix Amp, files/URLs, creator feeds, playlists, transcription and volume controls |
| Librarian | Books/documents, filename search, image OCR, searchable PDFs, read aloud, document conversion and text audiobook export |

The **TUI computer** runs `kilix-tui`; the **shell computer** runs `kilix shell`.
Station dialogues stay inside the cave: choices, file browsing, text entry,
confirmation prompts, reports, log previews, creator feeds and transcription jobs.
Use arrows and Enter to choose, type into input fields, and Escape to go back or
cancel work. Reports scroll with Up/Down. The helper runs separately so the cave
keeps rendering while commands work. Long output is a bounded preview.

Full interactive tools (shell, TUI, editors, process monitor, volume mixer and
privileged installation) open a separate Kilix tab. Return to the cave tab when
ready; position and station context are preserved. Amp/video use their own windows.
A failed tab launch is reported in the station window.

The **wall map** opens applications, home, Downloads or a web URL. The
**noticeboard** stores timestamped notes and opens an editor. The **voice
lever** toggles wizard speech. The **wall clock** shows the calendar and health.
The top bar shows local time and presentation FPS.

## Existing tools

Stations invoke programs with argument lists; user input is not inserted into
shell commands. Missing tools are reported when selected.

- Logs: `journalctl`, `less`; searches stop after 500 matching lines.
- Administration: Kilix, `apt-get`, `sudo`, `dpkg-query`, `systemctl`, `ip`.
- Media: `kilix-amp`, `kilix-mpv` or `mpv`, `yt-dlp`, `ffmpeg`, `alsamixer`.
  Refresh followed sources to fetch uploads; following does not schedule downloads.
- Documents: `kilix-pdf-conversion` for image OCR/conversion, `ocrmypdf` for
  searchable PDFs, `kilix-tts` for reading/export and `xdg-open` for readers.
- Applications: `kilix-launcher`, or XDG entries opened with `gio`.

Optional OCR engines/readers may need installation. Package installation shows
an apt simulation and asks for confirmation. Service changes and cleanup also
require an explicit choice. Thumbnail cleanup selects regular files older than
30 days under `~/.cache/thumbnails`, excluding symlinks. Package-cache cleanup
uses `apt-get clean`, preserving installed software. Existing OCR/audiobook
outputs are not overwritten.

## State and models

Zell stores state in `~/.local/gpu_terminal/kilix-zell/state`:

- `model.json`: GPU/model selection, schema `kilix.zell.model/v1`, shared with Kilix 95.
- `voice.json`: wizard speech toggle.
- `history.json`: question history/ranking.
- `media.json`: followed creators/playlists, cached uploads and seen markers (imports legacy `playlists.json`).
- `transcripts/`: background job status and saved text.
- `media-cache/`: downloaded MP3 audio for Kilix Amp; removable when no longer needed.
- `notebook.txt`: notes.

`KILIX_ZELL_STATE` overrides the state directory; `GPU_TERMINAL_HOME` overrides
the shared root. Existing tools retain their own configuration locations.

The wizard tries `kilix ask` first. Automatic routing tries a larger installed
model if it starts answering within 10 seconds, otherwise Qwen 3.5 0.8B. A
saved model is used by itself. Each call uses `kilix-llm run` and unloads after
the answer; no resident server is started. Esc cancels. Paradee voice is optional.

## Tests and artwork

Sibling libraries: `kitty-framebuffer`, `kitty-input`, `soft-raster`. Neuronte
provides text, shape and sprite rendering. The background is cached after its
first Neuronte render.

```sh
make all neuronte
make test test-neuronte
make snap
build/snap-neuronte /tmp/zell-preview 960 640
```

Tests cover scene behavior, routing, output capture under a controlling
terminal, cancellation, camera/resize, launcher, model schema, command/URL
validation and cleanup boundaries. Tests do not install packages, delete real
caches, play media or run OCR.

Original PNGs are in `assets/v0.1.0/`; prompts are in
`assets/v0.1.0/prompts.json`. `tools/compile_desktop.py` uses Pillow to compile
compressed embedded premultiplied ARGB data in `src/desktop_assets.c`. Normal builds do not
regenerate assets or call an image service. Cutouts are filtered once to their
fixed world size. Staff and Kilix animate while idle, with staggered timing. Legacy notices: `THIRD-PARTY.md`.

### DJ feeds, playback and transcripts

Open music launches a separate Kilix Amp window. Listening to a URL first
caches its audio as MP3; local audio opens directly. Video opens in Kilix MPV.
Follow YouTube creator URLs, @handles or playlists, then use **Refresh followed
sources** to fetch recent videos, Shorts and streams. Cached items remain new
until opened or marked seen. Subscriptions are local to Zell.

Choose **Watch and transcribe** or **Listen and transcribe** to start a background
transcription alongside playback. **Transcripts and jobs** shows progress, partial
text, completed transcripts and cancellation. Transcription processes the selected
file or URL, not microphone/system audio, using the installed Kilix Whisper English
model on CPU in one-minute chunks. Each chunk unloads its model; temporary audio
is removed after completion or cancellation. Remote downloads require yt-dlp;
conversion requires ffmpeg. Runtime/model overrides are `KILIX_ZELL_WHISPER` and
`KILIX_ZELL_WHISPER_MODEL`. Live streams are not a continuous captioning service.

The brass-and-teal **Neuronte shell switch** sits on the wall behind (above) the
shell computer. Walk up to it and press Enter, or select it from Tab's station
list. It opens the existing `neuronte-display` terminal with your interactive
shell in a separate Kilix tab. Ctrl-A then q exits the Neuronte terminal.
The normal shell computer remains available. The display module must be built;
`KILIX_ZELL_NEURONTE_DISPLAY` can override the path to `neuronte_display.py`.

### Local books, documents and music

Librarian > Books and documents library and DJ > Music library on disk provide
in-game instructions, file/folder import, search, refresh and removal. Adding a
folder indexes supported files recursively without moving originals. Default
places include Books, Documents, Downloads and Music under your home directory;
you can also specify an external drive path. Scans skip symlinks, hidden folders
and build/cache directories, and report a 10,000-entry scan limit. Saved folders
refresh on request. Missing files stay listed so disconnected disks are visible.
Removing a library reference never deletes its file; refreshing a saved folder
can rediscover that file. Metadata lives in `library-documents.json` and
`library-music.json` under Zell's state directory.

`make install` installs the Neuronte executable, helper scripts, font weights,
terminal renderer, vault binaries, user service units and `~/.local/bin/zell`.
Build `kilix-modules/kilix-secrets` first; packaging also needs Neuronte's shared
library and the Ghostty prefix used by `neuronte-display`. Runtime dependencies
include Python NumPy, libsodium, libsystemd, zlib and glibc. The renderer's shared
libraries use paths relative to the application, so source checkouts are not
needed after installation. Run `systemctl --user daemon-reload` after installing
updated units. Key Master starts the vault socket when opened; initialization
remains an explicit user action.

`ZELL_NEURONTE_HOME` overrides the application path. `DESTDIR`,
`ZELL_INSTALL_DIR` and `ZELL_BIN_DIR` support packaging/staging. `make runtime`
creates a relocatable archive under `build/`, with an exact file manifest and
dependency notices. The RC9 OS carrier pins its archive digest, verifies all
files before installation, and carries it through the offline installer.

### Running

Hold either Shift key to charge a dash in place (24 simulation ticks, about
0.4 seconds at 60 fps), then run at 9 world pixels per tick versus 3.5 walking.
Choose a direction when charging, or use the direction Kilix is facing. The dash
commits to that cardinal direction; release Shift to stop or turn. Walls and
staff stop the dash with a brief bump; release Shift before charging again.
A small bar shows the charge, and faster steps plus dust indicate running.
Opening a station/menu cancels the dash. Shift still types normally in text fields.

### Vitals and health hover

The HUD shows Health, Mana and Fatigue. Hover the health bar for the OS-age and
watched-project reasons behind the current value. A completed OS upgrade keeps
OS health at 100% for 24 hours, then costs 10 percentage points per elapsed day,
with a 10% floor. Successful upgrade history/stamps and Zell's completed-update
record are local evidence; missing history is shown as unknown. `apt update`
alone does not reset the age.

Git Wizard in the Command Center registers project folders. Health watching is
separate and off by default. Each staged, unstaged or untracked dirty file in a
watched repository costs one percentage point, down to a combined 1% floor.
Staged-plus-unstaged changes to one path count once; a rename counts once. Local
Git scans refresh at most once per minute; missing repositories show errors.
Nothing is automatically staged, committed, reset, cleaned or pushed.

Fatigue represents remaining resource headroom: the smaller of CPU idle percent
and available-memory percent, smoothed over samples. Lower health or headroom
reduces walking and running speed, with a 35% speed floor. Mana averages the
known Codex/Claude/Grok remaining percentages, using each provider's most
restrictive unexpired window. Unknown providers remain unknown; stale values
are marked. Wall clock > Check coding-token mana is the explicit refresh action.
It reuses Claudex's shared `.loop/usage-pages.json`; a provider checked within the
last hour is not contacted again. Opening Zell and its local monitor never
initiates a provider check. Zell requests serialize; independent external
Claudex writers retain their existing cache behavior.

Administrator > Update OS in floating terminal runs exactly
`sudo apt update && sudo apt -y upgrade` after an in-game confirmation. A real
PTY renders through Neuronte in a floating panel above the room, including sudo's
interactive prompt. Password input goes to the PTY; frames stay in anonymous
shared memory. The completion record updates only when both commands succeed.
Ctrl-C interrupts the command; press Enter at its final prompt to return. The
terminal requires the existing neuronte-display module and Neuronte shared
library; `KILIX_ZELL_NEURONTE_DISPLAY` and `ZELL_NEURONTE_LIBRARY` can select them.

### Downstairs rooms

Two floor hatches lead to the **Command Center** and **Laundry**. Enter interacts;
the downstairs stair hatch returns to the cave. Tab lists the current room's
stations. Five command-center screens bind to real Kilix coding tabs, with exact
broker identities saved after successful creation. Interacting opens/focuses the
session. The larger screens show bounded text previews of their assigned Codex
sessions, refreshed about every five seconds. Exact pane, tab and broker
identities are checked before and after capture; changed sessions lose their
preview. Preview text stays in memory. Click a screen from anywhere in the room
to open its menu. The controller can explicitly dispatch tasks to verified
workers and open the task board.

**Git Wizard** can register projects, inspect changes, enable health watches and
open project coding sessions on a screen or regular tab. **Pilot** previews
project session arrangements (single, review pair, or coordinator and workers),
then creates GPT-6 Astra sessions after your Launch choice. Successful receipts
are saved individually; partial failure stops without retrying uncertain launches.
Reassigning occupied screens retains earlier identities and asks first.

Laundry bins represent mounted disk filesystems with source/mount labels and
fill levels. Enter shows capacity and a read-only file browser. The eighth bin
opens overflow disks if needed. Disconnected or inaccessible disks are reported;
no cleanup or deletion is performed by digging through a bin.

New work is on RC9 branches. Frozen RC8 release refs are unchanged.

### Key Master

The Command Center's locksmith manages `kilix-secrets`: initialize and unlock
the encrypted vault, confirm its recovery code, lock it, save API keys, save
logins or other secrets, browse metadata, copy a field, and delete a record.
First-time initialization shows the recovery code in a private floating
terminal. Keep it privately, then choose **Confirm recovery code / recover
access** to activate the vault. Zell never saves the passphrase or recovery
code. The same recovery action restores access when the passphrase is lost.

Secret entry has terminal echo disabled. The normal room interface shows only
record labels, identities and field names. Values travel through descriptors;
they are excluded from room reports, conversation history, command arguments
and debug logging. Private terminal frames are cleared when the helper exits,
including after minimizing and changing rooms. Core dumps are disabled.
Logins are one encrypted `login-json` field containing username and password.
Clipboard retrieval uses the vault's explicit confirmation and clears the
clipboard after 20 seconds if it still holds that value. Clipboard managers
may retain their own history.

Select **Launch a client with a saved key** to use a record for Codex, Claude,
Grok or Qwen. The client must already be installed. Codex uses a command-backed
provider and ephemeral credential storage. Claude uses its explicit API-key
helper in bare mode, which avoids reading its saved account credentials.
Grok and Qwen accept their API keys through the launched process's environment;
their child processes inherit it. Qwen's default endpoint is DashScope
International. Keys for other services can be stored with **Other API key**.
API use can incur separate billing from an existing subscription.

The encrypted vault is the persistent secret store. Once launched, a client
must handle its received credential securely; its own storage/logging policy
still applies. The vault is not a boundary against hostile software running as
the same OS user. `KILIX_ZELL_SECRETS` and `KILIX_ZELL_SECRETS_SOCKET` select an
alternate CLI/socket for isolated testing; there is no plaintext fallback.

### Tasks, conversations and shell resizing

The Noticeboard and controller offer a private local task board. Add tasks,
mark them done, reopen or remove them. The Noticeboard also enables, reads and
clears conversation history. Saving full station reports and Wizard replies
is off by default and requires a separate opt-in; Key Master and terminal
contents are always excluded. Tasks and history use owner-only files in Zell's
state directory. Reports can include personal information.

The Neuronte shell follows later pane resizes and preserves its running PTYs.
Generated Command Center/Laundry backgrounds, hatches, stairs, monitor frames,
drive bins and animated Git Wizard, Pilot and Key Master sprites are under
`assets/v0.1.1/`, with their image-generation prompts. `tools/compile_desktop.py`
compiles premultiplied cutouts for both renderers.

### Weather on the wall map

When the Kilix weather widget has a location, the cave's world map shows an
illustrated weather emblem, temperature and conditions from the same reading.
The overlay follows the widget's Celsius/Fahrenheit setting and refreshes from
its cache every five seconds. It makes no additional network requests. Remove
the widget's location to restore the plain map. A missing, mismatched or stale
reading shows a refresh notice rather than claiming current conditions.
The map's **Weather on the map** menu gives the location, conditions and source
attribution: Open-Meteo (CC BY 4.0), location data from GeoNames.

Automatic local-model routing uses installed supported model IDs. The current
runtime's Qwen 0.8B fallback is `qwen-0.8b-no-loop-02`; the retired stock 0.8B
alias is not selected. An installed 4B model remains available when that
fallback is absent. Install additional models through the Kilix model wizard.
