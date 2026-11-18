#!/usr/bin/env python3
"""Shells for Zell's pane rooms, painted by Neuronte into a shared map.

Through the Command Center's top corners are two rooms that are a Kilix tab
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
  A INDEX AGENT     type AGENT's command into that pane's idle shell
  X INDEX           end that pane's shell and start a new one
  K<hex>            bytes for the focused pane of the shown room
  Q                 close every pane and stop
On stdout: "R" once ready, then "G AGENT LABEL" for each coding agent found
on PATH; "T ROOM INDEX NAME" when what runs in front in a pane changes;
"N ROOM INDEX TEXT" when a pane cannot do what was asked; "E <text>" when
something failed.

The map (its fd is in ZELL_PANES_FD) starts with 64 bytes of little-endian
words: b"ZPNE", version 1, width, height, sequence (odd while a frame is
being written), room. Then the pixels, as native ARGB.
"""
import mmap
import os
from pathlib import Path
import select
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
# Ctrl-E then Ctrl-U: the agent's command replaces the whole half-typed
# line, wherever the cursor was in it.
CLEAR_LINE = b'\x05\x15'
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


def available_agents(which=shutil.which):
    """The coding agents with a command on PATH, as (command, label)."""
    return [(name, zell_providers.LABELS[name]) for name in zell_providers.CLI_FILES if which(name)]


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
                 agents=(), say=lambda text: None, clock=time.monotonic, hang_up=os.killpg):
        self.display, self.command, self.make, self.wait = display, command, make, wait
        self.front, self.agents, self.say, self.clock = front, dict(agents), say, clock
        self.hang_up = hang_up
        self.rooms, self.shown, self.dirty, self.quit = {}, None, False, False
        self.titles, self.next_titles = {}, 0.0

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
        if text.startswith('K'):
            workspace = self.rooms.get(self.shown)
            try:
                data = bytes.fromhex(text[1:])
            except ValueError:
                return
            if workspace is not None and data:
                try:
                    workspace.selected.send(data)
                except PANE_ERRORS:
                    # A shell that has not read a megabyte of input loses
                    # what is typed until it reads again.
                    pass
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
                # What runs in front has a group of its own: it is hung up as
                # a closed terminal would hang it up, before the shell goes.
                pane = workspace.current['panes'][index]
                group = self.front(pane)[0]
                if group is not None and group != pane.pid:
                    try:
                        self.hang_up(group, signal.SIGHUP)
                    except OSError:
                        pass
                replace_pane(self.display, workspace, workspace.current, index, self.command, self.clock())
                workspace.layout()
                self.dirty = True
        elif words[0] == 'A' and len(words) == 3:
            found = self.pane(words[1])
            if not found or words[2] not in self.agents:
                return
            workspace, index = found
            pane = workspace.current['panes'][index]
            group, name = self.front(pane)
            # Typed only at the shell's own prompt, never into what it runs,
            # nor into a program the shell has become (exec).
            shell = getattr(pane, 'name', None)
            if pane.fd is None or group is None or group != pane.pid or (shell and name != shell):
                self.say(f'N {self.shown} {index} {BUSY}')
                return
            try:
                pane.send(CLEAR_LINE + words[2].encode() + b'\r')
            except PANE_ERRORS:
                pass

    def poll(self, now):
        for room, workspace in self.rooms.items():
            changed = False
            for pane in workspace.panes():
                changed = poll_pane(pane, self.wait) or changed
            changed = renew_exited(self.display, workspace, self.command, now) or changed
            if room == self.shown and changed:
                self.dirty = True
        if now >= self.next_titles:
            self.next_titles = now + TITLE_SECONDS
            for room, workspace in self.rooms.items():
                for index, pane in enumerate(workspace.current['panes']):
                    title = title_text(self.front(pane)[1] or getattr(pane, 'name', None))
                    if self.titles.get((room, index)) != title:
                        self.titles[room, index] = title
                        self.say(f'T {room} {index} {title}')

    def descriptors(self):
        return [pane.fd for workspace in self.rooms.values() for pane in workspace.panes() if pane.fd is not None]

    def close(self):
        for workspace in self.rooms.values():
            workspace.close()
        self.rooms = {}


def say(text):
    sys.stdout.write(text + '\n')
    sys.stdout.flush()


def main():
    fd = int(os.environ.pop('ZELL_PANES_FD'))
    # The shells are in these panes, not in the Kilix window Zell runs in.
    for name in IDENTITY:
        os.environ.pop(name, None)
    # Python ignores these; the shells must get them as any shell does.
    for sig in (signal.SIGPIPE, signal.SIGXFSZ):
        signal.signal(sig, signal.SIG_DFL)
    os.chdir(Path.home())
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
        weights, library = zell_terminal.renderer_paths()
        cache_dir = Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache')) / 'neuronte-display'
        cache = display.MaskCache(weights, library, cache_dir, CELL_WIDTH, CELL_HEIGHT)
        renderer = display.Renderer(cache)
        agents = available_agents()
        rooms = Rooms(display, [os.environ.get('SHELL') or '/bin/sh'], agents=agents, say=say)
        rooms.quit = bool(stopping)
        say('R')
        for name, label in agents:
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
