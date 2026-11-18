#!/usr/bin/env python3
"""A running Kilix pane, shown in this terminal and used from it.

usage: zell_attach.py [--pane ID]

Without --pane it lists the Kilix panes there are, to choose one. What the
chosen pane shows is drawn here and kept up to date; what is typed, clicked,
scrolled or pasted here goes to it. Ctrl-] leaves it (back to the list, or
out with --pane); q, Esc or Ctrl-] in the list leaves.

Zell runs it in a pane room's pane (the Kilix button) and in the Command
Center's worker screens.

How:
- The pane's output comes from its kitty-pty-broker, read-only (observe).
  The pane is never resized and never taken from the Kilix window showing
  it; the Kilix window keeps working as before.
- A terminal parser (the Neuronte display's) keeps the pane's screen at the
  pane's own size. The part that fits here is drawn, following its cursor,
  with a status line at the bottom.
- Input goes through kilix_sdk: send_bytes where Kilix has it (0.2.4), else
  send with every byte written as an escape it reads back as that byte.
- The mouse goes to the pane's program when it asked for the mouse (in the
  encoding it asked for). Otherwise the wheel scrolls back through what the
  pane showed.
"""
from __future__ import annotations

import argparse
import fcntl
import os
from pathlib import Path
import queue
import re
import select
import signal
import struct
import subprocess
import sys
import termios
import threading
import time
import tty
import unicodedata

sys.path.insert(0, str(Path(__file__).resolve().parent))

LEAVE = 0x1d                     # Ctrl-]
# How long the start of an escape waits for the rest before it is keys.
ESCAPE_SECONDS = 0.03
FRAME_SECONDS = 1 / 30
SIZE_SECONDS = 2.0
LIST_SECONDS = 2.0
# This window's title over the list; over a pane, its name (View.name).
LIST_TITLE = 'Kilix panes'
WHEEL_ROWS = 3
CELL_W, CELL_H = 8, 16
SESSION = re.compile(r'[A-Za-z0-9_.-]{1,64}')
POINTER = re.compile(rb'\x1b\[<(\d{1,4});(\d{1,5});(\d{1,5})([Mm])')
# The start of a pointer report not yet whole, held for the next read.
POINTER_START = re.compile(rb'\x1b(\[(<[\d;]*)?)?$')
# What this terminal is asked for, as the pane's program asked for it.
MIRRORED = (1, 2004)
SHIFT, ALT, CTRL = 4, 8, 16


class AttachError(RuntimeError):
    pass


def kilix_config(environ=os.environ):
    """The Kilix config folder holding kilix_sdk, as Zell's other Kilix
    callers find it."""
    import shutil
    candidates = []
    home = environ.get('KILIX_HOME')
    if home:
        candidates.append(Path(home).expanduser() / 'config')
    source = environ.get('GPU_TERMINAL_SOURCE_HOME')
    if source:
        candidates.append(Path(source).expanduser() / 'kilix' / 'config')
    program = shutil.which('kilix')
    if program:
        candidates.append(Path(os.path.realpath(program)).parent / 'config')
    for path in candidates:
        if (path / 'kilix_sdk' / 'panes.py').is_file():
            return path
    raise AttachError('kilix_sdk was not found (KILIX_HOME).')


def escaped(data):
    """`data` as text kilix_sdk.panes.send writes back as the same bytes: the
    engine reads ANSI-C escapes in it. Bytes that are not UTF-8 cannot be
    written that way and are left out."""
    text = data.decode('utf-8', 'ignore')
    out = []
    for ch in text:
        if ch == '\\':
            out.append('\\\\')
        elif ord(ch) < 32 or ord(ch) == 127:
            out.append(f'\\x{ord(ch):02x}')
        else:
            out.append(ch)
    text = ''.join(out)
    # Not taken for an option of the kitten.
    if text.startswith('-'):
        text = '\\x2d' + text[1:]
    return text


class Kilix:
    """What this needs of Kilix: its panes, a pane's broker, and a way to
    type into a pane."""

    def __init__(self, panes, executable, runtime, own_window=None):
        self.panes, self.executable, self.runtime = panes, executable, runtime
        self.own_window = own_window

    @classmethod
    def load(cls, environ=os.environ):
        executable = environ.get('KITTY_PTY_BROKER_EXECUTABLE') or ''
        runtime = environ.get('KITTY_PTY_BROKER_RUNTIME') or ''
        if not environ.get('KITTY_LISTEN_ON'):
            raise AttachError('This is not inside a Kilix window (no KITTY_LISTEN_ON).')
        if not (os.path.isfile(executable) and os.access(executable, os.X_OK)):
            raise AttachError('The Kilix pane broker was not found (KITTY_PTY_BROKER_EXECUTABLE).')
        if not os.path.isabs(runtime):
            raise AttachError('The Kilix pane broker has no runtime folder (KITTY_PTY_BROKER_RUNTIME).')
        config = str(kilix_config(environ))
        if config not in sys.path:
            sys.path.insert(0, config)
        from kilix_sdk import panes
        return cls(panes, executable, runtime, environ.get('KITTY_WINDOW_ID'))

    def listing(self):
        """(tab title, pane) for every pane but this window, with a broker."""
        workspace = self.panes.snapshot()
        out = []
        for tab in workspace.tabs():
            for pane in tab.panes:
                if not pane.broker_session or not SESSION.fullmatch(pane.broker_session):
                    continue
                # This window: by the broker session Kilix matches it with
                # (window ids change when the engine restarts), and by id.
                if getattr(pane, 'is_self', False) or (self.own_window and str(pane.id) == str(self.own_window)):
                    continue
                out.append((tab.title, pane))
        return out

    def observe(self, session):
        if not SESSION.fullmatch(session or ''):
            raise AttachError('That pane has no broker session.')
        return subprocess.Popen([self.executable, '--runtime-dir', self.runtime, 'observe', session],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

    def size(self, session):
        """The pane's (columns, rows) now, or None."""
        import json
        try:
            done = subprocess.run([self.executable, '--runtime-dir', self.runtime, '--timeout', '0.5',
                                   'status', session, '--json'], capture_output=True, timeout=3)
            if done.returncode:
                return None
            record = json.loads(done.stdout)
            columns, rows = int(record['columns']), int(record['rows'])
        except (OSError, ValueError, KeyError, TypeError, subprocess.TimeoutExpired):
            return None
        return (columns, rows) if 1 <= columns <= 1000 and 1 <= rows <= 500 else None

    def send(self, pane_id, data):
        target = f'pane:{int(pane_id)}'
        send_bytes = getattr(self.panes, 'send_bytes', None)
        if send_bytes is not None:
            send_bytes(target, data)
            return
        text = escaped(data)
        if text:
            self.panes.send(target, text)


class Sender(threading.Thread):
    """Types into the pane in order, off the drawing loop: each write is a
    kitten run. What waits is joined into one write. The last failure is
    kept for the status line."""

    def __init__(self, kilix, pane_id):
        super().__init__(daemon=True)
        self.kilix, self.pane_id = kilix, pane_id
        self.waiting, self.error = queue.Queue(), None
        self.start()

    def put(self, data):
        if data:
            self.waiting.put(bytes(data))

    def stop(self):
        self.waiting.put(None)

    def run(self):
        while True:
            data = self.waiting.get()
            if data is None:
                return
            more = [data]
            while True:
                try:
                    piece = self.waiting.get_nowait()
                except queue.Empty:
                    break
                if piece is None:
                    self.write(b''.join(more))
                    return
                more.append(piece)
            self.write(b''.join(more))

    def write(self, data):
        try:
            self.kilix.send(self.pane_id, data)
            self.error = None
        except Exception as error:  # the status line says it; typing goes on
            self.error = str(error).splitlines()[0][:120] if str(error) else type(error).__name__


def follow(offset, at, shown, total):
    """The first row (or column) to show of `total` in `shown`, kept from
    `offset` unless that leaves `at` out of view."""
    if total <= shown:
        return 0
    offset = min(max(0, offset), total - shown)
    if at < offset:
        offset = at
    elif at >= offset + shown:
        offset = at - shown + 1
    return min(max(0, offset), total - shown)


def wrap_row(texts, cols, tcols, cursor=None):
    """A row of `tcols` cells as pieces (start, width) of at most `cols`:
    broken after a space where one is, never inside a wide character; the
    blank cells at its end are not wrapped, except up to `cursor`, its
    column when the cursor is on this row. The last piece runs to the row's
    end where it fits, so a coloured background is kept."""
    used = 0
    for column in range(min(len(texts), tcols) - 1, -1, -1):
        if texts[column] not in (' ', ''):
            used = column + 1
            break
    if cursor is not None:
        used = max(used, min(cursor + 1, tcols))
    cols = max(1, cols)
    pieces, start = [], 0
    while True:
        if used - start <= cols:
            pieces.append((start, min(cols, tcols - start)))
            return pieces
        end = start + cols
        cut = next((k for k in range(end, start, -1) if k - 1 < len(texts) and texts[k - 1] == ' '), end)
        if cut < len(texts) and texts[cut] == '' and cut - 1 > start:
            cut -= 1  # the second half of a wide character goes with its first
        pieces.append((start, cut - start))
        start = cut


def fit(text, cols):
    """`text` cut to `cols` cells and padded to them: wide characters take
    two, combining marks none, so it never wraps the last row."""
    out, used = [], 0
    for ch in text:
        if unicodedata.combining(ch):
            width = 0
        elif unicodedata.east_asian_width(ch) in ('W', 'F'):
            width = 2
        elif ord(ch) < 32 or 0x7f <= ord(ch) < 0xa0:
            ch, width = ' ', 1
        else:
            width = 1
        if used + width > cols:
            break
        out.append(ch)
        used += width
    return ''.join(out) + ' ' * (cols - used)


def sgr(fg, bg, flags):
    parts = ['0']
    for bit, code in ((1, '1'), (32, '2'), (2, '3'), (4, '4'), (16, '7'), (64, '8'), (8, '9'), (128, '53')):
        if flags & bit:
            parts.append(code)
    parts.append(f'38;2;{(fg >> 16) & 255};{(fg >> 8) & 255};{fg & 255}')
    parts.append(f'48;2;{(bg >> 16) & 255};{(bg >> 8) & 255};{bg & 255}')
    return '\x1b[' + ';'.join(parts) + 'm'


def row_text(cells, texts, start, count):
    """One shown row: `count` cells from `start`, with their colours."""
    out, last = [], None
    for column in range(start, start + count):
        _code, fg, bg, flags = cells[column - start]
        text = texts[column] if column < len(texts) else ' '
        if text == '':
            if column > start:
                continue  # the second half of a wide character, drawn by the first
            text = ' '  # its first half is out of view: a blank keeps the columns
        if len(text) and column == start + count - 1 and column + 1 < len(texts) and texts[column + 1] == '':
            text = ' '  # half a wide character does not fit
        style = (fg, bg, flags)
        if style != last:
            out.append(sgr(fg, bg, flags))
            last = style
        out.append(' ' if any(ord(ch) < 32 or 0x7f <= ord(ch) < 0xa0 for ch in text) else text)
    return ''.join(out)


class View:
    """One pane being shown: its broker's stream into a terminal parser at
    the pane's size."""

    def __init__(self, display, kilix, pane, tab_title='', clock=time.monotonic):
        self.kilix, self.pane, self.clock = kilix, pane, clock
        self.title = pane.title or tab_title or pane.process or f'pane {pane.id}'
        columns, rows = max(1, pane.columns or 80), max(1, pane.lines or 24)
        self.terminal = display.Terminal(columns, rows)
        self.process = kilix.observe(pane.broker_session)
        os.set_blocking(self.process.stdout.fileno(), False)
        self.sender = Sender(kilix, pane.id)
        self.ox = self.oy = 0
        self.lines = []  # (row, start, width) of each row drawn
        # Wrapped lines past the drawn ones, and how many of them the wheel
        # has gone up through at the oldest row kept.
        self.spare = self.extra = 0
        self.named = ''
        self.next_size = clock() + SIZE_SECONDS
        self.ended = False

    def fileno(self):
        return self.process.stdout.fileno()

    def read(self):
        """True when the pane showed something new."""
        try:
            data = os.read(self.fileno(), 65536)
        except BlockingIOError:
            return False
        except OSError:
            data = b''
        if not data:
            self.ended = True
            return True
        # The parser's replies are not sent: the Kilix window answers.
        self.terminal.feed(data)
        return True

    def check_size(self):
        if self.clock() < self.next_size:
            return False
        self.next_size = self.clock() + SIZE_SECONDS
        size = self.kilix.size(self.pane.broker_session)
        if size and size != (self.terminal.cols, self.terminal.rows):
            self.terminal.resize(*size, CELL_W, CELL_H)
            return True
        return False

    def close(self):
        self.sender.stop()
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        for stream in (self.process.stdin, self.process.stdout):
            try:
                stream.close()
            except OSError:
                pass
        self.terminal.close()

    def name(self):
        """What the pane is called here: its number and the title its
        program set, else its own."""
        title = ''
        try:
            title = self.terminal.title()
        except (AttributeError, ValueError):
            pass
        return f'Kilix {self.pane.id}: {title.strip() or self.title}'

    def paint(self, cols, rows, previous):
        """Escape sequences that draw the pane in cols x rows (the last row is
        the status line), its rows wrapped to cols, the rows that changed
        since `previous` only; and the rows now drawn."""
        terminal = self.terminal
        area = max(1, rows - 1)
        cells, (cx, cy) = terminal.snapshot()
        texts = terminal.rows_text()
        tcols, trows = terminal.cols, terminal.rows
        kept, first, shown = terminal.scrollbar()
        live = first + shown >= kept
        lines, at = [], None
        if terminal.alternate_screen():
            # A full-screen program is laid out for its own width: not
            # wrapped, but shown a window's width at a time from its cursor.
            self.ox = follow(self.ox, cx, cols, tcols)
            lines = [(row, self.ox, max(0, min(cols, tcols - self.ox))) for row in range(trows)]
            if cy < trows:
                at = (cy, cx - self.ox)
        else:
            self.ox = 0
            for row in range(trows):
                on_row = cx if row == cy else None
                for start, width in wrap_row(texts[row] if row < len(texts) else [], cols, tcols, on_row):
                    if on_row is not None and start <= cx < start + max(1, width):
                        at = (len(lines), cx - start)
                    lines.append((row, start, width))
        # Live, the cursor is followed; scrolled back, the lines nearest the
        # live ones are shown, so each wheel step brings new rows in; at the
        # oldest row kept, the wheel goes on up through the wrapped lines.
        self.spare = max(0, len(lines) - area)
        self.extra = min(self.extra, self.spare)
        if live and not self.extra and at is not None:
            self.oy = follow(self.oy, at[0], area, len(lines))
        else:
            self.oy = self.spare - self.extra
        self.lines = lines[self.oy:self.oy + area]
        drawn = []
        for y in range(area):
            if y < len(self.lines):
                row, start, width = self.lines[y]
                at_cell = row * tcols + start
                line = row_text(cells[at_cell:at_cell + width], texts[row], start, width) if width > 0 else ''
            else:
                line = ''
            drawn.append(line + '\x1b[0m\x1b[K')
        back = kept - first - shown
        # What matters most first: a narrow status line keeps it.
        parts = [f'Kilix pane {self.pane.id}']
        error = self.sender.error
        if error:
            parts.append(f'not sent: {error}')
        if back:
            parts.append(f'{back} lines back')
        if self.ox:
            parts.append(f'from column {self.ox + 1}')
        parts += ['Ctrl-] leaves', self.title, f'{tcols}x{trows}']
        status = fit(' ' + ' / '.join(parts) + ' ', cols)
        drawn.append('\x1b[0;7m' + status + '\x1b[0m')
        out = []
        name = self.name()
        if name != self.named:
            self.named = name
            out.append(f'\x1b]2;{fit(name, 200).rstrip()}\x07')
        for y, line in enumerate(drawn):
            if y >= len(previous) or previous[y] != line:
                out.append(f'\x1b[{y + 1};1H' + line)
        visible = (live and at is not None and terminal.mode(25) and self.oy <= at[0] < self.oy + area
                   and 0 <= at[1] < cols)
        if visible:
            out.append(f'\x1b[{at[0] - self.oy + 1};{at[1] + 1}H\x1b[?25h')
        else:
            out.append('\x1b[?25l')
        return ''.join(out), drawn

    def modes(self):
        """{mode: on} this terminal should have for the pane's program."""
        wanted = {}
        for number in MIRRORED:
            try:
                wanted[number] = bool(self.terminal.mode(number))
            except ValueError:
                wanted[number] = False
        # The wheel is always wanted here; motion with no button only when
        # the program asked for it.
        try:
            any_motion = bool(self.terminal.mode(1003))
        except ValueError:
            any_motion = False
        wanted[1003], wanted[1002] = any_motion, not any_motion
        return wanted

    def pointer(self, buttons, x, y, release, cols, rows):
        """One pointer report from this terminal (cells counted from one)."""
        if y > rows - 1 or x < 1 or y < 1:
            return
        # The cell under it, through the wrapping; past a piece's end, its
        # last cell, so a release there still reaches the program.
        row = column = None
        if y <= len(self.lines):
            row, start, width = self.lines[y - 1]
            if width > 0:
                column = start + min(x, width) - 1
        terminal = self.terminal
        low = buttons & 3
        wheel, motion = bool(buttons & 64), bool(buttons & 32)
        mods = (1 if buttons & SHIFT else 0) | (2 if buttons & CTRL else 0) | (4 if buttons & ALT else 0)
        # The program has the mouse when it asked for it, unless Shift is held.
        if terminal.mouse_tracking() and not buttons & SHIFT and column is not None and column < terminal.cols and row < terminal.rows:
            button = 4 + low if wheel else 8 + low if buttons & 128 else (1, 3, 2, 0)[low]
            action = 1 if release else 2 if motion else 0
            self.send(terminal.mouse(action, button, mods, column * CELL_W, row * CELL_H), live=False)
            return
        if not wheel or low not in (0, 1):
            return
        if terminal.alternate_screen():
            # A program on its alternate screen that asked (1007) gets
            # arrows, as other terminals give it.
            if terminal.mode(1007):
                letter = b'A' if low == 0 else b'B'
                self.send(((b'\x1bO' if terminal.mode(1) else b'\x1b[') + letter) * WHEEL_ROWS)
            return
        up = low == 0
        _kept, first, _shown = terminal.scrollbar()
        if up and first == 0 and self.oy > 0:
            self.extra = min(self.spare, self.spare - self.oy + WHEEL_ROWS)
        elif not up and self.extra:
            self.extra = max(0, self.extra - WHEEL_ROWS)
        else:
            terminal.scroll(-WHEEL_ROWS if up else WHEEL_ROWS)

    def send(self, data, live=True):
        if not data:
            return
        if live:
            self.extra = 0
            kept, first, shown = self.terminal.scrollbar()
            if first + shown < kept:
                self.terminal.scroll_bottom()
        self.sender.put(data)


def split_input(data, held):
    """Pointer reports and the rest, in order, from what this terminal sent;
    the start of a report not yet whole is held for the next read."""
    data = held + data
    keep = POINTER_START.search(data)
    held = b''
    if keep and keep.group(0):
        held, data = data[keep.start():], data[:keep.start()]
    out, at = [], 0
    for match in POINTER.finditer(data):
        if match.start() > at:
            out.append(('keys', data[at:match.start()]))
        out.append(('pointer', (int(match[1]), int(match[2]), int(match[3]), match[4] == b'm')))
        at = match.end()
    if at < len(data):
        out.append(('keys', data[at:]))
    return out, held


def terminal_size(fd):
    try:
        rows, cols = struct.unpack('HHHH', fcntl.ioctl(fd, termios.TIOCGWINSZ, b'\0' * 8))[:2]
    except OSError:
        return 80, 24
    return max(20, cols or 80), max(4, rows or 24)


def list_lines(entries, chosen, cols, rows, note):
    lines = [fit(' Kilix panes / Enter shows one here / Up and Down choose / q leaves', cols).rstrip(), '']
    if note:
        lines += [fit(' ' + note, cols).rstrip(), '']
    if not entries:
        lines.append(' No other Kilix pane is running.')
    top = max(0, chosen - (rows - len(lines) - 1))
    for index, (tab_title, pane) in enumerate(entries[top:top + rows - len(lines)], top):
        name = pane.title or tab_title
        where = pane.cwd.replace(str(Path.home()), '~', 1) if pane.cwd else ''
        text = f'{pane.id:>5}  {pane.process[:14]:<14}  {name[:40]:<40}  {pane.columns}x{pane.lines}  {where}'
        mark = '>' if index == chosen else ' '
        line = fit(f'{mark}{text}', cols)
        lines.append(('\x1b[7m' + line + '\x1b[0m') if index == chosen else line.rstrip())
    return lines, top


class Screen:
    """This terminal: raw, on its alternate screen, asked for SGR mouse
    reports; put back as it was on leaving."""

    def __init__(self, fd_in, fd_out):
        self.fd_in, self.fd_out = fd_in, fd_out
        self.saved = termios.tcgetattr(fd_in) if os.isatty(fd_in) else None
        self.modes = {}

    def __enter__(self):
        if self.saved is not None:
            tty.setraw(self.fd_in)
        self.write('\x1b[22;0t\x1b[?1049h\x1b[?1006h\x1b[?1000h\x1b[2J')
        return self

    def __exit__(self, *_exc):
        resets = ''.join(f'\x1b[?{number}l' for number, on in self.modes.items() if on)
        self.write(resets + '\x1b[?1003l\x1b[?1002l\x1b[?1000l\x1b[?1006l\x1b[0m\x1b[?25h\x1b[?1049l\x1b[23;0t')
        if self.saved is not None:
            termios.tcsetattr(self.fd_in, termios.TCSADRAIN, self.saved)

    def write(self, text):
        data = text.encode('utf-8', 'replace')
        while data:
            try:
                data = data[os.write(self.fd_out, data):]
            except BlockingIOError:
                select.select([], [self.fd_out], [], 1)

    def mirror(self, wanted):
        out = []
        for number, on in wanted.items():
            if self.modes.get(number) != on:
                self.modes[number] = on
                out.append(f'\x1b[?{number}{"h" if on else "l"}')
        if out:
            self.write(''.join(out))


def choose(kilix, screen, note='', wait=select.select, clock=time.monotonic):
    """The pane chosen from the list, or None to leave."""
    entries, chosen, next_list, held = [], 0, 0.0, b''
    screen.mirror({1003: False, 1002: False, 1: False, 2004: False})
    screen.write(f'\x1b]2;{LIST_TITLE}\x07')
    while True:
        if clock() >= next_list:
            next_list = clock() + LIST_SECONDS
            try:
                entries = kilix.listing()
            except Exception as error:  # said on the list, tried again
                note = f'The panes could not be listed: {str(error).splitlines()[0][:100] if str(error) else error}'
            chosen = min(chosen, max(0, len(entries) - 1))
            cols, rows = terminal_size(screen.fd_out)
            lines, top = list_lines(entries, chosen, cols, rows, note)
            screen.write('\x1b[?25l\x1b[H\x1b[2J' + '\r\n'.join(lines))
        if not wait([screen.fd_in], [], [], ESCAPE_SECONDS if held else 0.25)[0]:
            if not held:
                continue
            # A lone Esc, or the start of one, that nothing followed.
            data, events, held = b'', [('keys', held)], b''
        else:
            data = os.read(screen.fd_in, 4096)
            if not data:
                return None
            events, held = split_input(data, held)
        for kind, value in events:
            if kind == 'pointer':
                buttons, _x, y, release = value
                header = 2 + (2 if note else 0)
                if not release and not buttons & (3 | 32 | 64):
                    index = top + y - 1 - header
                    if 0 <= index < len(entries):
                        return entries[index]
                elif buttons & 64:
                    chosen = max(0, min(len(entries) - 1, chosen + (1 if buttons & 1 else -1)))
                    cols, rows = terminal_size(screen.fd_out)
                    lines, top = list_lines(entries, chosen, cols, rows, note)
                    screen.write('\x1b[H\x1b[2J' + '\r\n'.join(lines))
                continue
            keys = value
            if keys in (b'q', b'\x1b', b'\x03', bytes([LEAVE])):
                return None
            if keys in (b'\r', b'\n') and entries:
                return entries[chosen]
            if keys in (b'\x1b[A', b'\x1bOA', b'k'):
                chosen = max(0, chosen - 1)
            elif keys in (b'\x1b[B', b'\x1bOB', b'j'):
                chosen = min(max(0, len(entries) - 1), chosen + 1)
            elif keys == b'r':
                pass
            else:
                continue
            # Redrawn at once, the list read again only on its timer.
            cols, rows = terminal_size(screen.fd_out)
            lines, top = list_lines(entries, chosen, cols, rows, note)
            screen.write('\x1b[H\x1b[2J' + '\r\n'.join(lines))


def show(display, kilix, entry, screen, wait=select.select, clock=time.monotonic):
    """Show one pane until Ctrl-] or until it closes. A note for the list,
    or '' when left by Ctrl-]."""
    tab_title, pane = entry
    view = View(display, kilix, pane, tab_title, clock)
    resized = []
    old = signal.signal(signal.SIGWINCH, lambda *_args: resized.append(True))
    previous, dirty, next_frame, held, held_at = [], True, 0.0, b'', 0.0
    try:
        screen.write('\x1b[2J')
        while True:
            ready = wait([screen.fd_in, view.fileno()], [], [], ESCAPE_SECONDS if held else FRAME_SECONDS)[0]
            cols, rows = terminal_size(screen.fd_out)
            if held and screen.fd_in not in ready and clock() - held_at >= ESCAPE_SECONDS:
                view.send(held)
                held = b''
            if resized:
                resized.clear()
                previous, dirty = [], True
                screen.write('\x1b[2J')
            if view.fileno() in ready:
                dirty = view.read() or dirty
                if view.ended:
                    return f'Kilix pane {pane.id} has closed.'
            if screen.fd_in in ready:
                data = os.read(screen.fd_in, 65536)
                if not data:
                    return None
                events, held = split_input(data, held)
                held_at = clock()
                for kind, value in events:
                    if kind == 'pointer':
                        view.pointer(*value, cols, rows)
                        dirty = True
                        continue
                    if LEAVE in value:
                        view.send(value[:value.index(LEAVE)])
                        return ''
                    view.send(value)
                    dirty = True
            dirty = view.check_size() or dirty
            if dirty and clock() >= next_frame:
                screen.mirror(view.modes())
                text, previous = view.paint(cols, rows, previous)
                screen.write(text)
                dirty, next_frame = False, clock() + FRAME_SECONDS
    finally:
        signal.signal(signal.SIGWINCH, old)
        view.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description='Show a running Kilix pane here and use it.')
    parser.add_argument('--pane', type=int, help='the pane to show, as kilix pane list numbers it')
    args = parser.parse_args(argv)
    try:
        kilix = Kilix.load()
        import zell_terminal
        display = zell_terminal.load_display()
    except (AttachError, ImportError, RuntimeError, OSError) as error:
        print(f'Kilix panes cannot be shown here: {error}', file=sys.stderr)
        return 1
    with Screen(sys.stdin.fileno(), sys.stdout.fileno()) as screen:
        note = ''
        while True:
            if args.pane is not None:
                try:
                    entry = next((e for e in kilix.listing() if e[1].id == args.pane), None)
                except Exception as error:
                    entry, note = None, f'The panes could not be listed: {error}'
                if entry is None:
                    note = note or f'Kilix pane {args.pane} is not running.'
                    args.pane = None
                    continue
            else:
                entry = choose(kilix, screen, note)
                if entry is None:
                    return 0
            try:
                note = show(display, kilix, entry, screen)
            except Exception as error:  # said on the list; the viewer goes on
                note = f'Kilix pane {entry[1].id} could not be shown: {type(error).__name__}: {error}'
            if note is None:
                return 0
            if args.pane is not None and not note:
                return 0
            args.pane = None


if __name__ == '__main__':
    raise SystemExit(main())
