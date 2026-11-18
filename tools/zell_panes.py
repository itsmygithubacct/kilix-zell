#!/usr/bin/env python3
"""Shells for Zell's pane rooms, painted by Neuronte into a shared map.

Through the Command Center's top corners are two rooms that are a Kilix tab
each: two panes side by side on the left, four on the right, filling the
whole Zell pane with no border. Zell starts this helper beside itself the
first time Kilix walks into one. A room's shells live until Zell closes;
only the room Kilix is in is painted.

Lines on stdin:
  S ROOM COLS ROWS  show a room at this size (its shells start the first time)
  H                 hide: the shells keep running, painting stops
  C COL             focus the pane that column COL of the shown room is in
  K<hex>            bytes for the focused pane of the shown room
  Q                 close every pane and stop
On stdout: "R" once ready, "E <text>" when something failed.

The map (its fd is in ZELL_PANES_FD) starts with 64 bytes of little-endian
words: b"ZPNE", version 1, width, height, sequence (odd while a frame is
being written), room. Then the pixels, as native ARGB.
"""
import mmap
import os
from pathlib import Path
import select
import signal
import struct
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import zell_terminal

HEADER = 64
MAX_WIDTH = MAX_HEIGHT = 1920
MAP_BYTES = HEADER + MAX_WIDTH * MAX_HEIGHT * 4
CELL_WIDTH, CELL_HEIGHT = 8, 16
FRAME_SECONDS = 0.05
PANES = {9: 2, 10: 4}
SEPARATOR = 0x709BC5
# A shell that will not stay up is started again at most this often.
RESPAWN_SECONDS = 1.0
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


def pane_at(cols, count, column):
    """The pane a column is in. A separator belongs to the pane on its left."""
    x = 0
    for index, width in enumerate(column_widths(cols, count)):
        if column < x + width + 1:
            return index
        x += width + 1
    return count - 1


def valid_size(cols, rows, count):
    return (2 * count - 1 <= cols and 20 <= cols <= MAX_WIDTH // CELL_WIDTH
            and 8 <= rows <= MAX_HEIGHT // CELL_HEIGHT)


def room_grid(display, workspace):
    cols, rows = workspace.cols, workspace.rows
    records = [(32, display.FG, display.BG, 0)] * (cols * rows)
    rectangles = workspace.layout()
    for index, (pane, (x, _y, width, height)) in enumerate(zip(workspace.current['panes'], rectangles)):
        cells, cursor = pane.terminal.snapshot()
        for row in range(height):
            records[row * cols + x:row * cols + x + width] = cells[row * width:(row + 1) * width]
        cx, cy = cursor
        if index == workspace.focus and 0 <= cx < width and 0 <= cy < height:
            at = cy * cols + x + cx
            code, foreground, background, flags = records[at]
            records[at] = (code, foreground, background, flags ^ 16)
        if index < len(rectangles) - 1:
            for row in range(rows):
                records[row * cols + x + width] = (0x2502, SEPARATOR, display.BG, 0)
    return records


def room_workspace(display, count, cols, rows, command):
    class PaneRoom(display.Workspace):
        """One tab of side-by-side panes and nothing else: no tab bar, no
        help line, no pane titles."""

        def layout(self, tab=None):
            tab = self.current if tab is None else tab
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


def renew_exited(display, workspace, command, now):
    """Start a fresh shell where one has ended, so a room keeps its panes."""
    changed = False
    for tab in workspace.tabs:
        for index, pane in enumerate(tab['panes']):
            if pane.exit_status is None or pane.fd is not None:
                continue
            if now - getattr(pane, 'born', 0.0) < RESPAWN_SECONDS:
                continue
            pane.close()
            fresh = display.Pane(command, pane.cols, pane.rows, workspace.cw, workspace.ch, workspace.lib)
            fresh.born = now
            tab['panes'][index] = fresh
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

    def __init__(self, display, command, make=room_workspace, wait=select.select):
        self.display, self.command, self.make, self.wait = display, command, make, wait
        self.rooms, self.shown, self.dirty, self.quit = {}, None, False, False

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
        elif words[0] == 'C' and len(words) == 2:
            workspace = self.rooms.get(self.shown)
            try:
                column = int(words[1])
            except ValueError:
                return
            if workspace is not None and 0 <= column < workspace.cols:
                workspace.focus = pane_at(workspace.cols, len(workspace.current['panes']), column)
                self.dirty = True

    def poll(self, now):
        for room, workspace in self.rooms.items():
            changed = False
            for pane in workspace.panes():
                changed = poll_pane(pane, self.wait) or changed
            changed = renew_exited(self.display, workspace, self.command, now) or changed
            if room == self.shown and changed:
                self.dirty = True

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
        rooms = Rooms(display, [os.environ.get('SHELL') or '/bin/sh'])
        rooms.quit = bool(stopping)
        say('R')
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
