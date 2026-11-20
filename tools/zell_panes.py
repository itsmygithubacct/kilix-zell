#!/usr/bin/env python3
"""Shells for Zell's pane rooms, painted by Neuronte into a shared map.

Through the Command Center's lower corners are two rooms that are a Kilix tab
each: two panes side by side on the left, four on the right, across the
whole Zell pane. Zell draws the walkway and the panes' title bars; this
helper paints the cells under them. Zell starts it beside itself the first
time Kilix walks into one. A room's shells live until Zell closes; only the
room Kilix is in is painted.

Lines on stdin (INDEX is a pane of the shown room, from 0):
  S ROOM COLS ROWS  show a room at this size (its shells start the first time)
  H                 hide: the shells keep running, painting stops
  F INDEX           focus that pane: typing and the cursor go there
  Z INDEX           zoom that pane over the whole room; Z -1 shows them all
  A INDEX AGENT     type AGENT's command into that pane's idle shell; AGENT
                    kilix shows the Kilix panes there instead (zell_attach.py)
                    over its shell, which keeps running; leaving them (Ctrl-]
                    then q) or Restart shows the shell again
  X INDEX           end that pane's shell and start a new one
  K<hex>            bytes for the focused pane of the shown room
  M INDEX ACTION BUTTON MODS X Y
                    a pointer event over a pane's cells: ACTION 0 press,
                    1 release, 2 motion; BUTTON 1 left, 2 right, 3 middle,
                    4 to 7 the wheel, 0 none; MODS 1 Shift, 2 Ctrl, 4 Alt;
                    X and Y pixels from the pane's top left. It goes to the
                    pane's program when that asked for the mouse (Shift
                    keeps it here); else the left button selects text,
                    which is copied when it goes up, and the wheel scrolls
  W INDEX HOW       scroll a pane: up or down a page, top, bottom
  V INDEX ROW       scroll a pane so ROW (as B counts) is its first shown
  P ROOM INDEX HEX  part of a paste for that pane; P ROOM INDEX . ends it
  Q                 close every pane and stop
On stdout: "R" once ready, then "G AGENT LABEL" for each coding agent found
(on PATH or where it installs itself) and "G kilix Kilix" when Kilix panes
can be shown; "T ROOM INDEX NAME" when what runs in front in a pane changes;
"N ROOM INDEX TEXT" when a pane cannot do what was asked or text was copied;
"B ROOM INDEX KEPT FIRST SHOWN FLAGS" when a shown pane's scrollback
changes (rows kept, the first shown, how many; FLAGS 1 if its program wants
the mouse, 2 if it wants motion with no button too); "Y BASE64" pieces of
copied text, then "Y ." ; "E <text>" when something failed.

The map (its fd is in ZELL_PANES_FD) starts with 64 bytes of little-endian
words: b"ZPNE", version 1, width, height, sequence (odd while a frame is
being written), room. Then the pixels, as native ARGB.
"""
import base64
import mmap
import os
from pathlib import Path
import select
import shlex
import shutil
import signal
import struct
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import zell_providers
import zell_terminal

HEADER = 64
MAX_WIDTH, MAX_HEIGHT = 3840, 2160
MAP_BYTES = HEADER + MAX_WIDTH * MAX_HEIGHT * 4
CELL_WIDTH, CELL_HEIGHT = 8, 16
FRAME_SECONDS = 0.05
PANES = {9: 2, 10: 4}
SEPARATOR = 0x709BC5
# A shell that will not stay up is started again at most this often.
RESPAWN_SECONDS = 1.0
# How often what runs in each pane is looked at.
TITLE_SECONDS = 0.5
BUSY = 'is busy. Finish what runs there, then press again.'
VIEWING = 'shows Kilix panes. Ctrl-] then q goes back to its shell.'
NO_VIEW = 'could not show the Kilix panes.'
# What runs a pane shows over its shell to show Kilix panes, and its title.
ATTACH = Path(__file__).resolve().with_name('zell_attach.py')
VIEW_TITLE = 'Kilix panes'
# The coding agents offered, in zell_providers' order; where they install
# themselves under the home folder when not on PATH: the shared folders,
# then the agent's own (.NAME/bin, .NAME/local).
AGENT_ORDER = tuple(zell_providers.CLI_FILES)
AGENT_FOLDERS = ('.local/bin', '.npm-global/bin', '.bun/bin', 'bin')
OWN_FOLDERS = ('bin', 'local')
# Ctrl-E then Ctrl-U: the agent's command replaces the whole half-typed
# line, wherever the cursor was in it.
CLEAR_LINE = b'\x05\x15'
# Rows the wheel moves, and the time within which presses on one cell are a
# double or triple click.
WHEEL_ROWS = 3
CLICK_SECONDS = 0.4
# Copied text at most, rows read for it at most, and base64 characters a Y
# line.
COPY_MAX = 256 * 1024
COPY_ROWS = 20000
COPY_CHUNK = 3000
# A paste at most, and how much of one waits in a pane's own queue at once.
PASTE_MAX = 4 << 20
PASTE_STEP = 64 * 1024
# What Shift with Page Up, Page Down, Home and End sends a program on its
# alternate screen, which keeps no scrollback to move.
SHIFTED_KEYS = {'up': b'\x1b[5;2~', 'down': b'\x1b[6;2~', 'top': b'\x1b[1;2H', 'bottom': b'\x1b[1;2F'}
SHIFT, PRESS, RELEASE, MOTION, LEFT = 1, 0, 1, 2, 1
# Zell's own Kilix window. The shells are not in it, so an agent started in
# one must not be found, or sent text, as that window.
IDENTITY = ('KITTY_WINDOW_ID', 'WINDOWID', 'KITTY_PID', 'KITTY_LISTEN_ON', 'KITTY_PUBLIC_KEY',
            'KITTY_PTY_BROKER_SESSION')
# Drawn instead of a character the renderer cannot draw.
STAND_IN = ord('?')
PANE_ERRORS = (OSError, RuntimeError, ValueError)


def column_widths(cols, count):
    """Pane widths across `cols`, with one column between neighbours."""
    base, extra = divmod(cols - (count - 1), count)
    return [base + (index < extra) for index in range(count)]


def valid_size(cols, rows, count):
    return (2 * count - 1 <= cols and 20 <= cols <= MAX_WIDTH // CELL_WIDTH
            and 8 <= rows <= MAX_HEIGHT // CELL_HEIGHT)


def available_agents(which=shutil.which, home=None):
    """The coding agents that can run, as (name, label, what to type): on
    PATH by name, else where it installs itself under the home folder by its
    whole path, as the pane's shell may not have that folder on PATH."""
    home = Path(home) if home is not None else Path.home()
    out = []
    for name in AGENT_ORDER:
        if which(name):
            out.append((name, zell_providers.LABELS[name], name))
            continue
        folders = [home / folder for folder in AGENT_FOLDERS] + [home / f'.{name}' / own for own in OWN_FOLDERS]
        for folder in folders:
            path = folder / name
            if path.is_file() and os.access(path, os.X_OK):
                out.append((name, zell_providers.LABELS[name], shlex.quote(str(path))))
                break
    return out


def attach_ready(environ):
    """Whether Kilix panes can be shown: inside a Kilix window, with its
    pane broker, and the viewer here."""
    executable = environ.get('KITTY_PTY_BROKER_EXECUTABLE') or ''
    return bool(environ.get('KITTY_LISTEN_ON') and os.path.isfile(executable) and os.access(executable, os.X_OK)
                and os.path.isabs(environ.get('KITTY_PTY_BROKER_RUNTIME') or '') and ATTACH.is_file())


def foreground(pane):
    """The process group in front in a pane's terminal and its name."""
    if pane.fd is None:
        return None, None
    try:
        group = os.tcgetpgrp(pane.fd)
        name = Path(f'/proc/{group}/comm').read_text().strip()
    except OSError:
        return None, None
    return group, name


def title_text(name):
    """A name Zell can show: printable ASCII, at most 31 characters."""
    text = ''.join(ch for ch in (name or '') if 32 <= ord(ch) < 127).strip()[:31].strip()
    return text or 'shell'


def view_title(view):
    """A Kilix pane shown in a pane is called what the viewer called its
    window: the pane's number and name. Until it says, or over its list,
    the Kilix panes."""
    try:
        title = view.terminal.title()
    except (AttributeError, ValueError):
        title = ''
    if not any(32 < ord(ch) < 127 for ch in title):
        return VIEW_TITLE
    return title_text(title)


class Selection:
    """Text chosen with the left button: from anchor to head, each (row,
    column), rows counted as the scrollbar counts them so the text stays
    chosen as the pane scrolls. A word or line selection grows to whole
    words or lines."""

    def __init__(self, at, mode, alternate):
        self.anchor = self.head = at
        self.mode, self.alternate, self.active = mode, alternate, True

    def ends(self):
        return min(self.anchor, self.head), max(self.anchor, self.head)


def word_char(text):
    return bool(text) and (text.isalnum() or text in '_-./~:@%+=#?&')


def read_rows(terminal, first_row, last_row):
    """{row: its cells' text} for rows first_row to last_row as the scrollbar
    counts them, on screen or not: the view is moved to read them, then put
    back (to the live rows if it was there). At most COPY_ROWS rows."""
    kept, first, shown = terminal.scrollbar()
    last_row = min(last_row, first_row + COPY_ROWS - 1, kept - 1)
    out = {}
    if first <= first_row and last_row < first + shown:
        rows = terminal.rows_text()
        return {row: rows[row - first] for row in range(first_row, last_row + 1)}
    try:
        at = first_row
        while at <= last_row:
            terminal.scroll_to(at)
            now = terminal.scrollbar()[1]
            for index, row in enumerate(terminal.rows_text()):
                if first_row <= now + index <= last_row:
                    out[now + index] = row
            if now + shown > last_row or now + shown <= at:
                break
            at = now + shown
    finally:
        if first + shown >= kept:
            terminal.scroll_bottom()
        else:
            terminal.scroll_to(first)
    return out


def selection_ends(pane):
    """A pane's selection as ((first row, column), (last row, column)), rows
    as the scrollbar counts them, grown to whole words or lines; None if
    there is none."""
    selection = getattr(pane, 'selection', None)
    if selection is None:
        return None
    terminal = pane.terminal
    if selection.alternate != terminal.alternate_screen():
        pane.selection = None
        return None
    (start_row, start_col), (end_row, end_col) = selection.ends()
    if selection.mode == 'char' and (start_row, start_col) == (end_row, end_col):
        return None
    if selection.mode == 'word':
        line = read_rows(terminal, start_row, start_row).get(start_row)
        if line:
            start_col = min(start_col, len(line) - 1)
            while start_col > 0 and word_char(line[start_col]) and word_char(line[start_col - 1]):
                start_col -= 1
        line = read_rows(terminal, end_row, end_row).get(end_row)
        if line:
            end_col = min(end_col, len(line) - 1)
            while end_col + 1 < len(line) and word_char(line[end_col]) and word_char(line[end_col + 1]):
                end_col += 1
    if selection.mode == 'line':
        start_col, end_col = 0, pane.cols - 1
    return (start_row, start_col), (end_row, end_col)


def selected_spans(pane):
    """{row shown: (first column, last column)} of a pane's selection."""
    ends = selection_ends(pane)
    if ends is None:
        return {}
    (start_row, start_col), (end_row, end_col) = ends
    first = pane.terminal.scrollbar()[1]
    spans = {}
    for row in range(max(start_row, first), min(end_row, first + pane.rows - 1) + 1):
        left = start_col if row == start_row else 0
        right = end_col if row == end_row else pane.cols - 1
        spans[row - first] = (left, min(right, pane.cols - 1))
    return spans


def selected_text(pane):
    """The selection's text, the rows scrolled out of view too: rows joined
    by newlines, each with its trailing blanks gone."""
    ends = selection_ends(pane)
    if ends is None:
        return ''
    (start_row, start_col), (end_row, end_col) = ends
    rows = read_rows(pane.terminal, start_row, end_row)
    lines = []
    for row in range(start_row, end_row + 1):
        if row not in rows:
            continue
        left = start_col if row == start_row else 0
        right = end_col if row == end_row else pane.cols - 1
        lines.append(''.join(rows[row][left:right + 1]).rstrip())
    return '\n'.join(lines)


def room_grid(display, workspace):
    cols, rows = workspace.cols, workspace.rows
    records = [(32, display.FG, display.BG, 0)] * (cols * rows)
    rectangles = workspace.layout()
    for index, (pane, rectangle) in enumerate(zip(workspace.current['panes'], rectangles)):
        if rectangle is None:
            continue
        x, _y, width, height = rectangle
        cells, cursor = pane.terminal.snapshot()
        for row in range(height):
            records[row * cols + x:row * cols + x + width] = cells[row * width:(row + 1) * width]
        # Selected cells are drawn reversed, as the cursor is.
        for row, (left, right) in selected_spans(pane).items():
            if 0 <= row < height:
                for column in range(left, min(right, width - 1) + 1):
                    at = row * cols + x + column
                    code, foreground, background, flags = records[at]
                    records[at] = (code, foreground, background, flags ^ 16)
        cx, cy = cursor
        if index == workspace.focus and 0 <= cx < width and 0 <= cy < height:
            at = cy * cols + x + cx
            code, foreground, background, flags = records[at]
            records[at] = (code, foreground, background, flags ^ 16)
        if x + width < cols:
            for row in range(rows):
                records[row * cols + x + width] = (0x2502, SEPARATOR, display.BG, 0)
    return records


def room_workspace(display, count, cols, rows, command):
    class PaneRoom(display.Workspace):
        """One tab of side-by-side panes and nothing else: no tab bar, no
        help line, no pane titles. A zoomed pane has every column and the
        others are not shown; they keep their size until it is unzoomed."""
        zoom = None

        def layout(self, tab=None):
            tab = self.current if tab is None else tab
            if self.zoom is not None:
                rectangles = [None] * len(tab['panes'])
                tab['panes'][self.zoom].resize(self.cols, self.rows)
                rectangles[self.zoom] = (0, 0, self.cols, self.rows)
                return rectangles
            rectangles, x = [], 0
            for pane, width in zip(tab['panes'], column_widths(self.cols, len(tab['panes']))):
                pane.resize(width, self.rows)
                rectangles.append((x, 0, width, self.rows))
                x += width + 1
            return rectangles

        def resize(self, cols, rows):
            self.cols, self.rows = cols, rows
            for tab in self.tabs:
                self.layout(tab)

    workspace = PaneRoom(cols, rows, CELL_WIDTH, CELL_HEIGHT, command)
    for _ in range(count - 1):
        workspace.split(True)
    workspace.focus = 0
    now = time.monotonic()
    for pane in workspace.panes():
        pane.born = now
    return workspace


def poll_pane(pane, wait=select.select):
    """Read and write one pane. A pane that fails is ended, to be started
    again like a shell that exited; the other panes keep running."""
    try:
        changed = False
        if pane.fd is not None:
            ready, writes, _ = wait([pane.fd], [pane.fd] if pane.pending else [], [], 0)
            if ready:
                changed = bool(pane.read())
            if writes and pane.fd is not None:
                pane.flush()
        pane.reap()
        return changed
    except PANE_ERRORS:
        try:
            pane.eof()
        except PANE_ERRORS:
            pane.fd = None
        if pane.exit_status is None:
            pane.exit_status = -1
        return True


def drawable(cache, records, bad):
    """The records with every character the renderer cannot draw replaced."""
    out = records
    for at, (code, foreground, background, flags) in enumerate(records):
        if code in bad:
            if out is records:
                out = list(records)
            out[at] = (STAND_IN, foreground, background, flags)
            continue
        if code in cache.memory:
            continue
        try:
            cache.get(code)
        except PANE_ERRORS:
            bad.add(code)
            if out is records:
                out = list(records)
            out[at] = (STAND_IN, foreground, background, flags)
    return out


def replace_pane(display, workspace, tab, index, command, now):
    """End a pane's shell and start a new one of the same size there."""
    pane = tab['panes'][index]
    pane.close()
    fresh = display.Pane(command, pane.cols, pane.rows, workspace.cw, workspace.ch, workspace.lib)
    fresh.born = now
    tab['panes'][index] = fresh


def renew_exited(display, workspace, command, now):
    """Start a fresh shell where one has ended, so a room keeps its panes."""
    changed = False
    for tab in workspace.tabs:
        for index, pane in enumerate(tab['panes']):
            if pane.exit_status is None or pane.fd is not None:
                continue
            if now - getattr(pane, 'born', 0.0) < RESPAWN_SECONDS:
                continue
            replace_pane(display, workspace, tab, index, command, now)
            changed = True
    if changed:
        workspace.layout()
    return changed


def publish(shared, frame, room):
    height, width, channels = frame.shape
    if channels != 3 or width > MAX_WIDTH or height > MAX_HEIGHT:
        raise ValueError('A pane frame exceeds the shared map')
    import numpy as np
    pixels = np.empty((height, width, 4), dtype=np.uint8)
    pixels[:, :, :3] = frame[:, :, ::-1]
    pixels[:, :, 3] = 255
    payload = pixels.tobytes()
    sequence = (struct.unpack_from('<I', shared, 16)[0] + 1) | 1
    struct.pack_into('<I', shared, 16, sequence & 0xffffffff)
    shared[HEADER:HEADER + len(payload)] = payload
    struct.pack_into('<4sIII', shared, 0, b'ZPNE', 1, width, height)
    struct.pack_into('<I', shared, 20, room)
    struct.pack_into('<I', shared, 16, (sequence + 1) & 0xffffffff)


class Rooms:
    """The pane rooms and which one is shown. Kept apart from the pipes and
    the renderer so the protocol can be tested without either."""

    def __init__(self, display, command, make=room_workspace, wait=select.select, front=foreground,
                 agents=(), say=lambda text: None, clock=time.monotonic, hang_up=os.killpg, viewer=None):
        self.display, self.command, self.make, self.wait = display, command, make, wait
        # Agent name -> what is typed to start it; kilix -> None (the viewer).
        self.agents = {name: typed for name, _label, typed in agents}
        self.front, self.say, self.clock = front, say, clock
        # viewer(workspace, cols, rows): a pane showing the Kilix panes.
        self.viewer = viewer
        self.hang_up = hang_up
        self.rooms, self.shown, self.dirty, self.quit = {}, None, False, False
        self.titles, self.next_titles = {}, 0.0
        # Scrollback last said per (room, pane); pastes coming in, and ones
        # waiting to go to a pane; the last left press (pane, cell, time).
        self.bars, self.pastes, self.backlog = {}, {}, {}
        self.last_press, self.clicks = None, 0

    def pane(self, word):
        """The shown room and its pane numbered `word`, or None."""
        workspace = self.rooms.get(self.shown)
        try:
            index = int(word)
        except ValueError:
            return None
        if workspace is None or not 0 <= index < len(workspace.current['panes']):
            return None
        return workspace, index

    def line(self, text):
        """One line from Zell. A pane's terminal failing on it changes
        nothing else: the line is dropped."""
        try:
            self.take(text)
        except PANE_ERRORS:
            self.dirty = True

    def take(self, text):
        if text.startswith('K'):
            workspace = self.rooms.get(self.shown)
            try:
                data = bytes.fromhex(text[1:])
            except ValueError:
                return
            if workspace is not None and data:
                pane = workspace.selected
                # Typing shows the live rows again and ends a selection.
                self.to_live(pane)
                # A shell that has not read a megabyte of input loses what
                # is typed until it reads again.
                self.send(pane, data)
            return
        words = text.split()
        if not words:
            return
        if words[0] == 'Q':
            self.quit = True
        elif words[0] == 'H':
            self.shown = None
        elif words[0] == 'S' and len(words) == 4:
            try:
                room, cols, rows = (int(word) for word in words[1:])
            except ValueError:
                return
            if room not in PANES or not valid_size(cols, rows, PANES[room]):
                return
            workspace = self.rooms.get(room)
            if workspace is None:
                self.rooms[room] = self.make(self.display, PANES[room], cols, rows, self.command)
            elif (workspace.cols, workspace.rows) != (cols, rows):
                workspace.resize(cols, rows)
            self.shown, self.dirty = room, True
        elif words[0] == 'F' and len(words) == 2:
            found = self.pane(words[1])
            # Not a pane hidden by another one's zoom.
            if found and found[0].zoom in (None, found[1]):
                found[0].focus, self.dirty = found[1], True
        elif words[0] == 'Z' and len(words) == 2:
            workspace = self.rooms.get(self.shown)
            if workspace is not None and words[1] == '-1':
                workspace.zoom = None
            else:
                found = self.pane(words[1])
                if not found:
                    return
                workspace, index = found
                workspace.zoom = workspace.focus = index
            workspace.layout()
            self.dirty = True
        elif words[0] == 'X' and len(words) == 2:
            found = self.pane(words[1])
            if found:
                workspace, index = found
                pane = workspace.current['panes'][index]
                # Over a shell, the Kilix panes go and the shell is shown.
                if getattr(pane, 'shell', None) is not None:
                    self.unview(workspace.current, index)
                    workspace.layout()
                    self.dirty = True
                    return
                # What runs in front has a group of its own: it is hung up as
                # a closed terminal would hang it up, before the shell goes.
                group = self.front(pane)[0]
                if group is not None and group != pane.pid:
                    try:
                        self.hang_up(group, signal.SIGHUP)
                    except OSError:
                        pass
                replace_pane(self.display, workspace, workspace.current, index, self.command, self.clock())
                workspace.layout()
                self.dirty = True
        elif words[0] == 'M' and len(words) == 7:
            found = self.pane(words[1])
            try:
                action, button, mods, x, y = (int(word) for word in words[2:])
            except ValueError:
                return
            if found and action in (PRESS, RELEASE, MOTION) and 0 <= button <= 11 and 0 <= mods <= 7:
                workspace, index = found
                self.pointer(workspace.current['panes'][index], index, action, button, mods, max(0, x), max(0, y))
        elif words[0] == 'W' and len(words) == 3:
            found = self.pane(words[1])
            if found and words[2] in SHIFTED_KEYS:
                self.scroll(found[0].current['panes'][found[1]], words[2])
        elif words[0] == 'V' and len(words) == 3:
            found = self.pane(words[1])
            if found and words[2].isdigit():
                found[0].current['panes'][found[1]].terminal.scroll_to(int(words[2]))
                self.dirty = True
        elif words[0] == 'P' and len(words) == 4:
            self.paste_line(words[1], words[2], words[3])
        elif words[0] == 'A' and len(words) == 3:
            found = self.pane(words[1])
            if not found or words[2] not in self.agents:
                return
            workspace, index = found
            pane = workspace.current['panes'][index]
            if getattr(pane, 'shell', None) is not None:
                self.say(f'N {self.shown} {index} {VIEWING}')
                return
            if words[2] == 'kilix':
                self.view(workspace, index)
                return
            group, name = self.front(pane)
            # Typed only at the shell's own prompt, never into what it runs,
            # nor into a program the shell has become (exec).
            shell = getattr(pane, 'name', None)
            if pane.fd is None or group is None or group != pane.pid or (shell and name != shell):
                self.say(f'N {self.shown} {index} {BUSY}')
                return
            try:
                pane.send(CLEAR_LINE + self.agents[words[2]].encode() + b'\r')
            except PANE_ERRORS:
                pass

    def view(self, workspace, index):
        """The Kilix panes shown in pane `index`, over its shell."""
        pane = workspace.current['panes'][index]
        if self.viewer is None:
            return
        try:
            view = self.viewer(workspace, pane.cols, pane.rows)
        except PANE_ERRORS:
            self.say(f'N {self.shown} {index} {NO_VIEW}')
            return
        view.born, view.shell = self.clock(), pane
        workspace.current['panes'][index] = view
        workspace.layout()
        self.dirty = True

    def unview(self, tab, index):
        """Pane `index` of `tab` shows its shell again."""
        view = tab['panes'][index]
        tab['panes'][index] = view.shell
        view.shell = None
        self.backlog.pop(view, None)
        try:
            view.close()
        except PANE_ERRORS:
            pass

    def hidden(self):
        """The shells under the Kilix panes, still running."""
        return [pane.shell for workspace in self.rooms.values() for pane in workspace.panes()
                if getattr(pane, 'shell', None) is not None]

    def to_live(self, pane):
        """Back to the live rows, with no selection."""
        terminal = pane.terminal
        kept, first, shown = terminal.scrollbar()
        if first + shown < kept:
            terminal.scroll_bottom()
            self.dirty = True
        if getattr(pane, 'selection', None) is not None:
            pane.selection = None
            self.dirty = True

    def send(self, pane, data):
        """Bytes for a pane's program; after the rest of a paste going to it,
        so nothing lands inside the paste's frame."""
        if not data:
            return
        if pane in self.backlog:
            self.backlog[pane].extend(data)
            return
        self.write(pane, data)

    def write(self, pane, data):
        try:
            pane.send(data)
        except PANE_ERRORS:
            pass

    def scroll(self, pane, how):
        terminal = pane.terminal
        if terminal.alternate_screen():
            self.send(pane, SHIFTED_KEYS[how])
            return
        page = max(1, pane.rows - 1)
        if how == 'up':
            terminal.scroll(-page)
        elif how == 'down':
            terminal.scroll(page)
        elif how == 'top':
            terminal.scroll_top()
        else:
            terminal.scroll_bottom()
        self.dirty = True

    def pointer(self, pane, index, action, button, mods, x, y):
        terminal = pane.terminal
        # The program has the mouse when it asked for it, unless Shift is held.
        program = terminal.mouse_tracking() and not mods & SHIFT
        if button in (4, 5):
            if program:
                self.send(pane, terminal.mouse(action, button, mods, x, y))
            elif terminal.alternate_screen():
                # A program on its alternate screen that asked (1007) gets
                # arrows, as other terminals give it.
                if terminal.mode(1007):
                    arrow = (b'\x1bOA' if terminal.mode(1) else b'\x1b[A') if button == 4 else \
                        (b'\x1bOB' if terminal.mode(1) else b'\x1b[B')
                    self.send(pane, arrow * WHEEL_ROWS)
            else:
                terminal.scroll(-WHEEL_ROWS if button == 4 else WHEEL_ROWS)
                self.dirty = True
            return
        if program:
            data = terminal.mouse(action, button, mods, x, y)
            if data:
                self.send(pane, data)
            return
        if button != LEFT:
            return
        column = min(max(0, x // CELL_WIDTH), pane.cols - 1)
        row = min(max(0, y // CELL_HEIGHT), pane.rows - 1) + terminal.scrollbar()[1]
        selection = getattr(pane, 'selection', None)
        if action == PRESS:
            now = self.clock()
            same = self.last_press is not None and self.last_press[:2] == (index, (row, column)) and \
                now - self.last_press[2] < CLICK_SECONDS
            self.clicks = self.clicks % 3 + 1 if same else 1
            self.last_press = (index, (row, column), now)
            pane.selection = Selection((row, column), ('char', 'word', 'line')[self.clicks - 1],
                                       terminal.alternate_screen())
            self.dirty = True
        elif selection is not None and selection.active:
            selection.head = (row, column)
            self.dirty = True
            if action == RELEASE:
                selection.active = False
                text = selected_text(pane)
                if text:
                    self.copy(index, text)
                else:
                    pane.selection = None

    def copy(self, index, text):
        data = text.encode('utf-8')[:COPY_MAX].decode('utf-8', 'ignore').encode('utf-8')
        encoded = base64.b64encode(data).decode('ascii')
        for at in range(0, len(encoded), COPY_CHUNK):
            self.say('Y ' + encoded[at:at + COPY_CHUNK])
        self.say('Y .')
        lines = text.count('\n') + 1
        self.say(f'N {self.shown} {index} Copied {lines} line{"s" if lines != 1 else ""}.')

    def paste_line(self, room_word, index_word, piece):
        try:
            room, index = int(room_word), int(index_word)
        except ValueError:
            return
        workspace = self.rooms.get(room)
        if workspace is None or not 0 <= index < len(workspace.current['panes']):
            return
        key = (room, index)
        if piece != '.':
            try:
                data = bytes.fromhex(piece)
            except ValueError:
                # A bad piece spoils the paste: none of it is sent.
                self.pastes[key] = None
                return
            held = self.pastes.setdefault(key, bytearray())
            if held is not None:
                if len(held) + len(data) > PASTE_MAX:
                    self.pastes[key] = None
                else:
                    held.extend(data)
            return
        held = self.pastes.pop(key, None)
        if not held:
            return
        pane = workspace.current['panes'][index]
        self.to_live(pane)
        self.backlog.setdefault(pane, bytearray()).extend(pane.terminal.paste(bytes(held)))
        self.feed_backlog()

    def feed_backlog(self):
        """A long paste goes to its pane a step at a time, as it reads."""
        for pane, data in list(self.backlog.items()):
            while data and pane.fd is not None and len(getattr(pane, 'pending', b'')) < PASTE_STEP:
                step = bytes(data[:PASTE_STEP])
                del data[:PASTE_STEP]
                self.write(pane, step)
            if not data or pane.fd is None:
                del self.backlog[pane]

    def report_bars(self):
        """Say how far back each shown pane is, when it changes."""
        workspace = self.rooms.get(self.shown)
        if workspace is None:
            return
        for index, pane in enumerate(workspace.current['panes']):
            terminal = pane.terminal
            try:
                kept, first, shown = terminal.scrollbar()
                flags = 0
                if terminal.mouse_tracking():
                    flags = 1 | (2 if terminal.mode(1003) else 0)
            except PANE_ERRORS:
                continue
            state = (kept, first, shown, flags)
            if shown and self.bars.get((self.shown, index)) != state:
                self.bars[self.shown, index] = state
                self.say(f'B {self.shown} {index} {kept} {first} {shown} {flags}')

    def poll(self, now):
        self.feed_backlog()
        for room, workspace in self.rooms.items():
            changed = False
            for pane in workspace.panes():
                changed = poll_pane(pane, self.wait) or changed
                shell = getattr(pane, 'shell', None)
                if shell is not None:
                    poll_pane(shell, self.wait)
            # The Kilix panes were left: the shell is shown again.
            restored = False
            for tab in workspace.tabs:
                for index, pane in enumerate(tab['panes']):
                    if getattr(pane, 'shell', None) is not None and pane.exit_status is not None and pane.fd is None:
                        # Ended with an error (it could not start): its words
                        # went with it, so the pane says so.
                        failed = pane.exit_status != 0
                        self.unview(tab, index)
                        restored = True
                        if failed:
                            self.say(f'N {room} {index} {NO_VIEW}')
            if restored:
                workspace.layout()
                changed = True
            changed = renew_exited(self.display, workspace, self.command, now) or changed
            if room == self.shown and changed:
                self.dirty = True
        self.report_bars()
        if now >= self.next_titles:
            self.next_titles = now + TITLE_SECONDS
            for room, workspace in self.rooms.items():
                for index, pane in enumerate(workspace.current['panes']):
                    if getattr(pane, 'shell', None) is not None:
                        title = view_title(pane)
                    else:
                        title = title_text(self.front(pane)[1] or getattr(pane, 'name', None))
                    if self.titles.get((room, index)) != title:
                        self.titles[room, index] = title
                        self.say(f'T {room} {index} {title}')

    def descriptors(self):
        return [pane.fd for workspace in self.rooms.values() for pane in workspace.panes() if pane.fd is not None] + \
            [shell.fd for shell in self.hidden() if shell.fd is not None]

    def close(self):
        for shell in self.hidden():
            try:
                shell.close()
            except PANE_ERRORS:
                pass
        for workspace in self.rooms.values():
            workspace.close()
        self.rooms = {}


def say(text):
    sys.stdout.write(text + '\n')
    sys.stdout.flush()


def main():
    fd = int(os.environ.pop('ZELL_PANES_FD'))
    # The viewer of Kilix panes is Zell's, in Zell's Kilix window: it keeps
    # that window's identity, which kilix_sdk checks.
    kilix_env = dict(os.environ)
    # The shells are in these panes, not in the Kilix window Zell runs in.
    for name in IDENTITY:
        os.environ.pop(name, None)
    # Python ignores these; the shells must get them as any shell does.
    for sig in (signal.SIGPIPE, signal.SIGXFSZ):
        signal.signal(sig, signal.SIG_DFL)
    shared = mmap.mmap(fd, MAP_BYTES)
    os.close(fd)
    cache = rooms = None
    stdin = sys.stdin.fileno()
    pending = bytearray()
    next_frame = 0.0
    stopping = []
    bad = set()

    def stop(_signum, _frame):
        stopping.append(True)
        if rooms is not None:
            rooms.quit = True
    for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(sig, stop)
    try:
        display = zell_terminal.load_display()
        if not hasattr(display.Terminal, 'scrollbar'):
            say('E The panes need the Neuronte display from 0.2.4, with scrollback and the mouse.')
            return 1
        weights, library = zell_terminal.renderer_paths()
        cache_dir = Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache')) / 'neuronte-display'
        cache = display.MaskCache(weights, library, cache_dir, CELL_WIDTH, CELL_HEIGHT)
        renderer = display.Renderer(cache)
        # Relative renderer overrides belong to the launch directory. Only
        # the shells start in the user's home.
        os.chdir(Path.home())
        agents = available_agents()
        viewer = None
        if attach_ready(kilix_env):
            agents.append(('kilix', 'Kilix', None))

            def viewer(workspace, cols, rows):
                return display.Pane([sys.executable, str(ATTACH)], cols, rows, workspace.cw, workspace.ch,
                                    workspace.lib, env=kilix_env)
        rooms = Rooms(display, [os.environ.get('SHELL') or '/bin/sh'], agents=agents, say=say, viewer=viewer)
        rooms.quit = bool(stopping)
        say('R')
        for name, label, _typed in agents:
            say(f'G {name} {label}')
        while not rooms.quit:
            ready = select.select([stdin, *rooms.descriptors()], [], [], FRAME_SECONDS)[0]
            if stdin in ready:
                data = os.read(stdin, 65536)
                if not data:
                    break
                pending.extend(data)
                while b'\n' in pending:
                    line, _, rest = pending.partition(b'\n')
                    pending[:] = rest
                    rooms.line(line.decode('ascii', 'replace'))
                if len(pending) > 1 << 20:
                    raise RuntimeError('A pane line was too long')
            now = time.monotonic()
            rooms.poll(now)
            workspace = rooms.rooms.get(rooms.shown)
            if workspace is not None and rooms.dirty and now >= next_frame:
                records = drawable(cache, room_grid(display, workspace), bad)
                frame = renderer.render(records, workspace.cols, workspace.rows)
                publish(shared, frame, rooms.shown)
                rooms.dirty = False
                next_frame = time.monotonic() + FRAME_SECONDS
    except (OSError, ValueError, RuntimeError, ImportError, KeyError):
        # Terminal text never goes into the reply.
        say('E The panes stopped. Check the Neuronte display library and weights.')
        return 1
    finally:
        if rooms is not None:
            rooms.close()
        if cache is not None:
            cache.close()
        shared.close()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
