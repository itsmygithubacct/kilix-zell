# Zell Neuronte 0.1.1

A walkable Kilix desktop inside a terminal pane. Kilix is an orange firekitten
in a green adventurer outfit. Six staff members in the cave help with
questions, logs, settings, system health, media, and documents. More staff
work in the rooms below it.

The cave and eight downstairs rooms use a fixed **1536×1024 world**. Each
view stays in place as Kilix walks and scales down to fit a smaller pane.
The upper row of hatches leads to the Game Room, Movie Theater, Meditation
Room, and Library. The lower row leads to the Command Center, Vault,
Workshop, and Laundry. The nearest hatch lifts its lid as Kilix approaches
and closes as he walks away. Press Enter beside a hatch or click it to go
downstairs. Walk south to return to the cave.
Work areas sit along the walls, leaving the central floor open.

In the Workshop, the dice-marked trapdoor to the right of the entrance stairs
opens PuzzLLM in a separate Kilix tab. Walk near it and press Enter, or click
the lid. Zell keeps your place in the Workshop.

The hatch checks the `work/puzzllm` branch of `kilix-games` on each visit to
PuzzLLM. It builds the active development checkout when it contains the
published revision. Otherwise it downloads and builds that revision in the
user cache. An existing build can be used offline. Set `PUZZLLM_ROOT` to the
game's source directory to play a specific local checkout. [PuzzLLM source](https://github.com/itsmygithubacct/kilix-games/tree/work/puzzllm/puzzllm)

## Start

```sh
make neuronte
./zell-neuronte
./zell-neuronte --version
make install
zell --version
```

Requires Kitty graphics, C11, zlib, libm and Python 3. The default Neuronte
dependency is a neuronte-v2 checkout (2.5.1 or later) at
`~/projects/neuronte-v2`, with its library already built and
`weights/neuronte-split-v9.nrt` present. Text the split-v9 net was not
trained on is drawn from the listed `neuronte-term-*` and `neuronte-kilix-*`
bundles beside it (each with its `NAME.list.txt`), else as the untrained
mark, in Zell's own drawing and in its terminals alike; the install copies
those bundles and their lists. `NEURONTE` and `NEURONTE_WEIGHTS`
override build paths. `ZELL_NEURONTE_WEIGHTS` overrides the runtime weight file.
Source builds also pass the selected weights, shared library and
`DISPLAY_DIR` renderer to Zell's terminals. Installed games use the copies
in their runtime. `ZELL_NEURONTE_LIBRARY` and `KILIX_ZELL_NEURONTE_DISPLAY`
override those two files. The build records the checkout, weights and
display paths in `build/neuronte.ident` and recompiles when any changes.

The frozen 0.1.1 archive for Plebian-OS 0.2.3 used the earlier Neuronte
commit `4134bf909594cf5c774f49677405fd1e22c4e2e4`. The current source
build uses neuronte-v2 as described above.

`make` also builds the original `zell` scene. The new desktop is in
`zell-neuronte`. Plebian-OS 0.2.3 carries Zell 0.1.1 as an offline runtime
archive pinned by its digest; see `make runtime` below. Changes made after
that archive was built are listed under Unreleased in `CHANGELOG.md`.

## Controls

| Key | Action |
| --- | --- |
| Arrow keys / WASD | Walk; Up/Down select menu rows |
| Hold Shift while moving | Run immediately and steer with the movement keys |
| Enter | Interact with a nearby character or object; open an action |
| Tab | Open/close the quick launcher |
| Esc | Back from menus; cancel questions; open launcher while walking |
| Up / Down in an answer | Scroll the reply |
| Ctrl-C | Exit Zell |
| South doorway | Walk back to the launching shell |

The minimap shows Kilix, stations and viewport. The launcher reaches all
stations without walking. Menus scroll on smaller panes.

## Movie theater

Click the remote to open the picture controls. Escape leaves a text field
first, then closes the remote. Click the theater floor to return keyboard
control to Kilix after interacting with a web page.

- Open a video file, a folder of videos, or an http or https URL.
- Choose Chrome, Chromium, or Firefox ESR for a URL. The page stays in the
  theater. If that browser is missing, the floating terminal installs only the
  one you chose, after you confirm. Chromium is already provided. Chrome uses
  Google's official package. Firefox ESR installs when you choose it.
- Play starts the selected video. Pause holds it. The queue shows file names
  and pages when the window is narrow.
- Captions read a matching .srt or .vtt file beside the video when captions
  are on. Embedded subtitle tracks are not read.
- F11 toggles fullscreen. Escape leaves fullscreen. Help lists those keys.
- Pop out moves the active video into the floating window. Dock returns that
  video to the theater screen. One source is active. Queue buttons keep each
  video's own position.

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

To keep a disk for archives only, list its folder in `archive-only-paths` in
the state directory, one absolute path per line (or set
`KILIX_ZELL_ARCHIVE_ONLY`, colon-separated). Zell then refuses to start coding
sessions or copy, move and back up active files there. Nothing is reserved by default.

## Tests and artwork

Sibling libraries: `kitty-framebuffer`, `kitty-input`, `soft-raster`, and
`neuronte-sdk` for the Neuronte build and for `make test`, which runs that
build's tests too. Neuronte
provides text, shape and sprite rendering. The background is cached after its
first Neuronte render. The Command Center embeds the same Kilix NeuroTerm
core used by PuzzLLM. Set `NEUROTERM_DIR` to a NeuroTerm checkout containing
`embed.mk` and `include/neuroterm_terminal.h`. It defaults to the
`kilix-apps/neuroterm` sibling of the modules directory.

The Neuronte programs do not link soft-raster. Their pixel buffer, frame
clear, flat fill, RGBA packing and PPM writer are the canvas of
`neuronte-sdk` (`neuronte_sdk_canvas.h`), which is adapted from soft-raster
and draws no other shape. `src/canvas.c` only carries Zell's canvas struct
across to it. The build compiles the SDK's one canvas source and nothing
else of the SDK. `NSDK_DIR` names the checkout, by default
`neuronte-sdk` under `MODULES`; it must be one that has
`src/neuronte_sdk_canvas.c`, and the build stops and says so if it has
not. A build that names another checkout, or the same one after a change
to the canvas, compiles the canvas again. The programs read soft-raster's
header for the canvas struct, so the build still needs that checkout too.
`make test` runs the canvas beside soft-raster and compares the bytes;
`make test-neuronte` checks that no Neuronte program holds a soft-raster
symbol, and that each holds the SDK's canvas and no other part of the SDK.

```sh
make all neuronte
make test test-neuronte
make snap
build/snap-neuronte /tmp/zell-preview 960 640
```

Each snapshot is written twice, as `NAME.ppm` and `NAME.png` with the same
pixels. Open the PNG in viewers that do not read PPM.

Tests cover scene behavior, routing, output capture under a controlling
terminal, cancellation, camera/resize, launcher, model schema, command/URL
validation and cleanup boundaries. Tests do not install packages, delete real
caches, play media or run OCR.

Original PNGs are in `assets/v0.1.0/`; prompts are in
`assets/v0.1.0/prompts.json`. `tools/compile_desktop.py` uses Pillow to compile
compressed embedded premultiplied ARGB data in `src/desktop_art/`, one generated file per
source sheet plus `index.c`, which holds `zell_art_get()`. Normal builds do not
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

`ZELL_NEURONTE_HOME` overrides the application path. `ZELL_INSTALL_DIR`
selects the game directory and `ZELL_BIN_DIR` selects the command directory.
The installed launcher links back to its game, including when a `DESTDIR`
staging tree moves. Install into a new or empty directory, or upgrade an
existing Zell runtime in place. `make runtime`
creates a relocatable archive under `build/`, with an exact file manifest and
dependency notices. Plebian-OS 0.2.3 pins that archive's digest, verifies all
files before installation, and carries it through the offline installer.

### Running

Hold Shift while moving to run in any room. Change direction while running.
Movement still responds to health and stamina and stops at walls and furniture.

### Vitals and health hover

The HUD shows Health, Mana and Stamina. Hover the health bar for the OS-age and
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

Stamina shows how much CPU and memory the system has to spare. Zell uses the
lower of the two percentages and smooths it over successive readings. Lower health or headroom
reduces walking and running speed, with a 35% speed floor. Mana averages the
known Codex/Claude/Grok remaining percentages, using each provider's most
restrictive unexpired window. Unknown providers remain unknown; stale values
are marked. Wall clock > Check coding-token mana is the explicit refresh action.
It reuses Claudex's shared `.loop/usage-pages.json`; a provider checked within the
last hour is not contacted again. The vitals snapshot itself never
initiates a provider check; the Mana console's automatic reading, described
below, is the one background check and can be turned off. Zell requests serialize; independent external
Claudex writers retain their existing cache behavior.

The DJ's *Find a playlist or creator in a browser window* opens the Chawan
text browser inside Zell's floating window, on a search for the words given,
an `@handle`, or a full address. Like the floating terminal it can be
minimized to a button on the bottom row and maximized; minimized, the page
stays open. Zell holds one floating window at a time.

**Messages** at the bottom right collects Kilix's speech, Zell notices,
desktop notification history and recent system journal messages. When CPU or memory
headroom is low, Kilix occasionally names the busiest processes in a speech
bubble. The bubble fades automatically. Choose **Stamina** in Messages to read
Kilix's last 80 remarks, including their process names, across game restarts.
Choose **All** for the combined history. Older and Newer scroll either list.

**Updates** shows health, the elapsed days and hours since the last successful
OS upgrade, the restart cap and the watched Git penalty. Expand **OS upgrade**
to start `sudo apt update && sudo apt -y upgrade` in a NeuroTerm terminal.
Other rows open separate terminals for APT clean, APT autoclean, rotating due
logs, and browsing the user cache. Collapsing a row keeps its job running.
Each terminal accepts normal shell input, including sudo prompts.

The **Mana console** sits above the center shell. Its screen
shows what Codex, Claude and Grok each have left. Its tasks zoom into the
screen and open the usage dashboard or a provider's own usage page in a
browser drawn inside a Kilix tab; that browser keeps its own profile, so a
sign-in is made once. *Read mana now* reads all three at once and *What needs
me* lists any provider that is waiting on the person, with the command to
run. A provider shown as `ASK` is waiting on a sign-in or a folder-trust
answer. While Zell runs, readings older than 30 minutes are read again
automatically; this reads balances only and spends no tokens. *Automatic
readings on / off* turns that off. The console needs Claudex, which is
installed on first use: when *Read mana now*, a usage page or the wall clock's
*Check coding-token mana* finds no copy, Zell asks Kilix for the catalog's
pinned one (`kilix app install kilix-claudex`) and carries on. Looking at
readings and the automatic reading never install anything, and
`KILIX_APP_AUTO_INSTALL=0` refuses the install.

The **Shell** under Mana starts in the folder `tools/mana`. Click its black
screen to type there, or its **New window** header for a floating shell.
The three scripts there tell Zell by hand how much a
provider has left, for when no automatic reading can be taken:

```
set-mana codex 62           Codex has 62% left
set-mana grok 40 codex 100  several at once
set-mana all 50             all three
set-mana                    be asked for each one
show-mana                   what is set by hand now
clear-mana [provider ...]   forget the values, or only the ones named
```

The number is the percentage left, from 0 to 100. A value set by hand is kept
in Zell's state folder (`mana-manual.json`). It stands until it is cleared,
set again, or a good automatic reading is taken after it (which retires it
for good; the reading that was there when the value was set never does), and the Mana console
and the MANA bar show it within a few seconds. The scripts do not need
Claudex. Type `exit` to leave the shell.

The MANA bar is the mean of the percentages known. For each provider, a value
set by hand comes first. Once a provider snapshot publishes any percentage,
published percentages take the place of Claudex's readings for every provider;
until then Claudex's readings are used. The Mana console shows its three
providers by the same rule, and the bar also counts published providers the
console has no place for.

Administrator > Update OS in floating terminal runs exactly
`sudo apt update && sudo apt -y upgrade` after an in-game confirmation. A real
PTY renders through Neuronte in a floating panel above the room, including sudo's
interactive prompt. Password input goes to the PTY; frames stay in anonymous
shared memory. The completion record updates only when both commands succeed.
Ctrl-C interrupts the command; press Enter at its final prompt to return. The
terminal requires the existing neuronte-display module and Neuronte shared
library; `KILIX_ZELL_NEURONTE_DISPLAY` and `ZELL_NEURONTE_LIBRARY` can select them.

### Downstairs rooms

The lower row of floor hatches leads to the **Command Center**, **Vault**,
**Workshop**, and **Laundry**, from left to right. Walk off the bottom of
the Laundry or Command Center to return to its hatch in the cave. In the
Command Center, Git Wizard, Pilot and Key Master stand along the south edge.
Above them, four solid black-glass floor panels show YOLO, Spare, agent
updates and saved fleets. Walk around either end to get behind them.
From the landing, walk straight left into the amber-lit tunnel for two panes,
or straight right into the turquoise-lit tunnel for four. Both entrances are
in the lower corners.
A pane room is one Kilix tab as wide and high as the Zell pane, past the
1600x1000 cap the other rooms keep. Kilix can walk along the top. Above each
pane is a button for every coding agent found on PATH. When the shell is at
its prompt, pressing a button types that agent's command and gives the pane
keyboard focus. Under the walkway each pane has a title bar: what it runs,
Zoom (the pane over the whole room, and back), and Restart (click twice for a
new shell). Click a pane or its title bar to type. Use Shift+Page Up or
Shift+Page Down for scrollback and Ctrl+Shift+V to paste. Mouse input works
in terminal apps that request it. Click the walkway to move Kilix with the
arrows. Enter or Space presses the button he stands on. Walk into the thin
green line at the end where he arrived to return. The shells are started by
`tools/zell_panes.py` the first
time he enters and keep running, unpainted, while he is elsewhere, until Zell
closes. They start in your home folder. They are Zell's own shells, not
Kilix windows: Kilix does not list them, and they cannot be sent text as Zell's
window. Tab lists the current room's
stations.

A bronze frame spans the full width of the Command Center above the tunnels.
It holds six independent NeuroTerm shells. Worker 1, Controller and Worker 2
fill the upper row. Worker 3 and Worker 4 flank Mana and Shell in the lower
row. The first visit opens real interactive shells.
The five coding screens start in your home folder. Their prompts and running
programs stay on the wall when you visit another room. Click a screen to type,
press **F6** or **Shift+F6** to cycle screens, and press **Alt+Esc** or click
the floor to walk again. Select an exited shell to start a new one.

The **Sessions** headers open the existing Kilix coding-session menus. The
controller can dispatch tasks to verified workers and open the task board.

The large **YOLO** switch reads Kilix's shared `KILIX_CODING_YOLO` setting on
room entry and follows changes made elsewhere. Clicking it, or walking up and
pressing Enter, saves through the shared settings writer. New screen, project
and Pilot launches request that setting through Kilix's provider-specific
launch flags. Existing sessions keep the mode they started with.

The **Agent updates** switch remembers a selection of installed clients, with
controls to select some or all. Run the selection in Zell's floating terminal;
it shows each client's result and accepts any sudo prompt there. Updates use
`kilix install --update`, including npm-installed Codex and Grok. Ctrl-C stops
the batch; an individual failure is reported while the other selected clients
continue. Missing clients are shown but cannot be selected for an update.

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
state directory. Changing the conversation setting also affects stations
already open in other tabs. Reports can include personal information.

The Neuronte shell follows later pane resizes and preserves its running PTYs.
Generated room backgrounds, hatches, stairs, monitor frames, drive bins and
four-frame character animations are under
`assets/v0.1.1/`, with their image-generation prompts. `tools/compile_desktop.py`
compiles premultiplied cutouts for both renderers.

### Operating-system staff

| Character | Tasks |
|---|---|
| Gatekeeper | Lock, suspend, restart, shutdown, logout, battery, inhibitors, restart checks and power profiles |
| Signalman | Wi-Fi, VPN, Bluetooth, connection tests, radio control, listening ports and saved SSH hosts |
| Porter | Copy/move, rename, folders, trash/restore, archives, file searches, duplicates, permissions and default applications |
| Laundress | Attached drives, mount/unmount/eject, SMART health, removable image writing, formatting and folder usage |
| Merchant | Debian packages, previews, holds, Kilix applications, model inventory/download/removal and package history |
| Herald | Notices, running jobs, cancellation, attention reports and quiet hours |
| Timekeeper | Timers, alarms, reminders, persistent user timers, time zones and clock synchronization |
| Sentinel | Password changes, logins, security updates, automatic upgrades, firewall reports, public SSH keys and exposed files |
| Vault Keeper | Folder backups, checksum verification, restore, settings export/import and scheduled backups |
| Tinkerer | Displays, sound devices, microphone checks, printers, keyboard, sensors, webcam, screenshots and brightness |

Administrator owns system and user services, language/region and font settings;
Housekeeper also handles process stopping and startup applications. Noticeboard
offers explicit clipboard capture into the encrypted vault. It does not watch
or retain the clipboard automatically. Wizard offers a first-run tour.

Press Tab and type an action name to search across every room. New system tasks
run in private floating terminals. Commands use argument lists; secrets stay
out of station history. Changes show their targets and require a typed word.
Missing programs name the package to install using Merchant. Password and
privileged tools keep their normal interactive authentication prompts.

Wi-Fi credentials are saved through Key Master, then supplied to NetworkManager
in a sealed memory descriptor. Zell-created profiles do not save passwords;
reconnect through Signalman with a vault record. The Wi-Fi form supports WPA2,
WPA3 personal and open networks; enterprise authentication needs NetworkManager.
The protocol follows the [NetworkManager CLI](https://networkmanager.pages.freedesktop.org/NetworkManager/NetworkManager/nmcli.html).

Backups create a new snapshot on each run, keep previous copies, and verify
source/destination contents with checksums. Schedules record the source and
destination filesystem identities and refuse a missing or replaced drive.
Restore uses a new destination. Archive extraction refuses traversal,
links and special files, and only writes into a new folder. Drive destruction
requires an unmounted removable/USB disk with a stable serial identity; the
device is rechecked in the privileged worker. Display changes revert after 20
seconds unless kept. The Health bar flags pending kernel or NVIDIA-module restarts.

Timekeeper uses persistent [systemd user timers](https://www.freedesktop.org/software/systemd/man/latest/systemd.timer.html).
They run while the user service manager is available and catch up missed times
at login. Scheduled command arguments are explicit; shell expansion is disabled.
Herald retains 200 notices and verifies process identity before cancelling a job.
The Plebian-OS 0.2.3 image includes the station tools; additional direct dependencies are
`upower`, `bluez`, `scrot`, `brightnessctl`, `power-profiles-daemon`, `nftables`
and `wtmpdb`. Existing audio uses PulseAudio-compatible device controls.

### Three audio switches

Click the three levers below the map: **Voice**, **Dictation**, **Music**.
Voice enables Wizard speech. Dictation records one utterance through Kilix;
click again to finish, then edit the recognized words and press Enter to send.
It never submits automatically. Green indicators show the active states.

The music switch pauses/resumes music. DJ playback pauses Zell's background
track and clears mute, including Play in the DJ's player window. Muting pauses
the DJ; unmuting resumes it. When the DJ stops, background music resumes unless
muted. The DJ has a dedicated Amp service, so these controls do not affect other
players or system volume. Choose a local background track under DJ > Zell
background music; no track is selected by default. Speech and dictation are
independent of music mute.

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
