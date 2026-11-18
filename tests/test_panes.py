"""The pane rooms' helper: layout, protocol, respawn, the map, and a run
through its real pipes and map with a stand-in for the Neuronte display."""
import mmap
import os
from pathlib import Path
import select
import signal
import struct
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import zell_panes as panes

FG, BG = 0xD0D0D0, 0x101418


class Terminal:
    def __init__(self, owner):
        self.owner = owner

    def snapshot(self):
        pane = self.owner
        cell = (pane.letter, FG, BG, 0)
        return [cell] * (pane.cols * pane.rows), pane.cursor


class Pane:
    made = []

    def __init__(self, command, cols, rows, cw, ch, lib):
        self.command, self.cols, self.rows = command, cols, rows
        self.sent, self.closed, self.exit_status, self.fd = [], False, None, 1000 + len(Pane.made)
        self.letter, self.cursor = ord('A') + len(Pane.made) % 26, (0, 0)
        self.terminal = Terminal(self)
        self.resizes, self.pending, self.output, self.explode, self.ended = [], bytearray(), False, None, 0
        Pane.made.append(self)

    def resize(self, cols, rows):
        self.resizes.append((cols, rows))
        self.cols, self.rows = cols, rows

    def send(self, data):
        if self.explode == 'send':
            raise RuntimeError('PTY input queue exceeds one MiB')
        self.sent.append(bytes(data))

    def read(self):
        if self.explode == 'read':
            raise RuntimeError('Parser reply queue overflow')
        output, self.output = self.output, False
        return output

    def flush(self):
        self.pending.clear()

    def reap(self):
        pass

    def eof(self):
        self.ended += 1
        self.fd = None

    def close(self):
        self.closed = True


def ready(readable, writable, _errors, timeout):
    """select() over the fake panes: readable when they have output."""
    assert timeout == 0
    owners = {pane.fd: pane for pane in Pane.made}
    return [fd for fd in readable if owners[fd].output or owners[fd].explode == 'read'], list(writable), []


class Workspace:
    """The parts of neuronte_display.Workspace the helper builds on, kept to
    its behaviour: one tab, splits of at most four, layout on each change."""

    def __init__(self, cols=80, rows=25, cw=8, ch=16, command=None):
        self.lib, self.cols, self.rows, self.cw, self.ch = 'lib', cols, rows, cw, ch
        self.command = command
        self.tabs, self.tab, self.focus = [], 0, 0
        self.closed = False
        self.new_tab()

    @property
    def current(self):
        return self.tabs[self.tab]

    @property
    def selected(self):
        return self.current['panes'][self.focus]

    def panes(self):
        return [pane for tab in self.tabs for pane in tab['panes']]

    def new_tab(self):
        pane = Pane(self.command, self.cols, max(1, self.rows - 3), self.cw, self.ch, self.lib)
        self.tabs.append({'panes': [pane], 'vertical': True, 'bias': 0})
        self.tab, self.focus = len(self.tabs) - 1, 0
        self.layout()

    def split(self, vertical=True):
        if len(self.current['panes']) >= 4:
            return
        self.current['panes'].append(Pane(self.command, 8, 3, self.cw, self.ch, self.lib))
        self.focus = len(self.current['panes']) - 1
        self.layout()

    def layout(self, tab=None):
        raise AssertionError('the pane room replaces the layout')

    def poll(self, timeout=0.05, keyboard=False):
        raise AssertionError('each pane is polled on its own')

    def close(self):
        self.closed = True


DISPLAY = SimpleNamespace(FG=FG, BG=BG, Pane=Pane, Workspace=Workspace)


class LayoutTests(unittest.TestCase):
    def test_widths_fill_the_row_with_one_column_between_panes(self):
        for cols in range(20, 241):
            for count in (2, 4):
                widths = panes.column_widths(cols, count)
                self.assertEqual(sum(widths) + count - 1, cols)
                self.assertLessEqual(max(widths) - min(widths), 1)
                self.assertEqual(widths, sorted(widths, reverse=True))
        self.assertEqual(panes.column_widths(240, 4), [60, 59, 59, 59])

    def test_every_column_is_in_one_pane_and_a_separator_in_the_left_one(self):
        for cols in (20, 37, 191, 240):
            for count in (2, 4):
                x, owner = 0, []
                for index, width in enumerate(panes.column_widths(cols, count)):
                    owner += [index] * width + ([index] if index < count - 1 else [])
                    x += width + 1
                self.assertEqual(len(owner), cols)
                self.assertEqual([panes.pane_at(cols, count, column) for column in range(cols)], owner)
        self.assertEqual(panes.pane_at(20, 2, 10), 0)
        self.assertEqual(panes.pane_at(20, 2, 11), 1)

    def test_sizes_the_map_and_renderer_can_hold(self):
        self.assertTrue(panes.valid_size(20, 8, 2))
        self.assertTrue(panes.valid_size(240, 120, 4))
        for cols, rows in ((19, 8), (241, 8), (20, 7), (20, 121)):
            self.assertFalse(panes.valid_size(cols, rows, 2))
        self.assertEqual(panes.MAP_BYTES, 64 + 1920 * 1920 * 4)

    def test_a_room_is_one_row_of_panes_with_no_titles(self):
        Pane.made.clear()
        for count in (2, 4):
            workspace = panes.room_workspace(DISPLAY, count, 41, 12, ['/bin/sh'])
            self.assertEqual(len(workspace.tabs), 1)
            self.assertEqual(len(workspace.current['panes']), count)
            self.assertEqual(workspace.focus, 0)
            rectangles = workspace.layout()
            x = 0
            for pane, (left, top, width, height) in zip(workspace.current['panes'], rectangles):
                self.assertEqual((left, top, height), (x, 0, 12))
                # Every row is the pane's: no title row, no tab bar, no help line.
                self.assertEqual((pane.cols, pane.rows), (width, 12))
                self.assertTrue(hasattr(pane, 'born'))
                x += width + 1
            self.assertEqual(x - 1, 41)
            workspace.resize(60, 20)
            self.assertEqual(sum(pane.cols for pane in workspace.current['panes']) + count - 1, 60)
            self.assertTrue(all(pane.rows == 20 for pane in workspace.current['panes']))

    def test_the_grid_is_the_panes_and_the_lines_between_them(self):
        Pane.made.clear()
        workspace = panes.room_workspace(DISPLAY, 4, 23, 3, ['/bin/sh'])
        for pane in workspace.current['panes']:
            pane.cursor = (1, 2)
        workspace.focus = 2
        records = panes.room_grid(DISPLAY, workspace)
        self.assertEqual(len(records), 23 * 3)
        widths = panes.column_widths(23, 4)
        for row in range(3):
            line = records[row * 23:(row + 1) * 23]
            x = 0
            for index, width in enumerate(widths):
                for column in range(x, x + width):
                    self.assertEqual(line[column][0], ord('A') + index)
                if index < 3:
                    self.assertEqual(line[x + width], (0x2502, panes.SEPARATOR, BG, 0))
                x += width + 1
        # The cursor shows in the focused pane only.
        flagged = [at for at, record in enumerate(records) if record[3] & 16]
        start = sum(widths[:2]) + 2
        self.assertEqual(flagged, [2 * 23 + start + 1])


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        Pane.made.clear()
        self.rooms = panes.Rooms(DISPLAY, ['/bin/sh'], wait=ready)

    def test_a_room_starts_once_and_keeps_its_shells_while_hidden(self):
        self.rooms.line('S 9 40 20')
        first = self.rooms.rooms[9]
        self.assertEqual((self.rooms.shown, self.rooms.dirty), (9, True))
        self.assertEqual(len(first.current['panes']), 2)
        self.rooms.dirty = False
        resizes = sum(len(p.resizes) for p in first.panes())
        self.rooms.line('S 9 40 20')
        self.assertIs(self.rooms.rooms[9], first)
        self.assertEqual(sum(len(p.resizes) for p in first.panes()), resizes)
        self.assertTrue(self.rooms.dirty)
        self.rooms.line('S 9 50 22')
        self.assertIs(self.rooms.rooms[9], first)
        self.assertEqual((first.cols, first.rows), (50, 22))
        self.rooms.line('H')
        self.assertIsNone(self.rooms.shown)
        self.assertIn(9, self.rooms.rooms)
        self.assertFalse(any(p.closed for p in first.panes()))
        self.rooms.line('S 10 40 20')
        self.assertEqual(len(self.rooms.rooms[10].current['panes']), 4)
        self.assertEqual(self.rooms.shown, 10)
        self.rooms.line('S 9 50 22')
        self.assertIs(self.rooms.rooms[9], first)
        self.rooms.close()
        self.assertTrue(first.closed and self.rooms.rooms == {})

    def test_bad_lines_change_nothing(self):
        for text in ('S 11 40 20', 'S 0 40 20', 'S 9 19 20', 'S 9 241 20', 'S 9 40 7', 'S 9 40 121',
                     'S 9 40', 'S nine 40 20', 'S 9 40 20 1', 'C 3', 'Kzz', 'K6c', 'X', '', 'Q9'):
            self.rooms.line(text)
        self.assertEqual((self.rooms.rooms, self.rooms.shown, self.rooms.dirty, self.rooms.quit), ({}, None, False, False))

    def test_keys_go_to_the_clicked_pane_of_the_shown_room_only(self):
        self.rooms.line('S 10 40 20')
        room = self.rooms.rooms[10]
        first, second, third, fourth = room.current['panes']
        self.rooms.line('K6c730d')
        self.assertEqual(first.sent, [b'ls\r'])
        widths = panes.column_widths(40, 4)
        self.rooms.line('C %d' % (widths[0] + 1 + widths[1] + 1))
        self.assertEqual(room.focus, 2)
        self.rooms.line('K71')
        self.assertEqual(third.sent, [b'q'])
        # Forty columns are panes of 10, 9, 9 and 9 with lines at 10, 20
        # and 30; a line belongs to the pane on its left.
        self.assertEqual(widths, [10, 9, 9, 9])
        for column, pane in ((0, 0), (9, 0), (10, 0), (11, 1), (19, 1), (20, 1), (21, 2), (30, 2), (31, 3), (39, 3), (21, 2)):
            self.rooms.line('C %d' % column)
            self.assertEqual(room.focus, pane, column)
        for text in ('C -1', 'C 40', 'C x'):
            self.rooms.line(text)
            self.assertEqual(room.focus, 2)
        self.rooms.line('Knothex')
        self.rooms.line('K')
        self.assertEqual(third.sent, [b'q'])
        # Hidden, nothing is typed into it.
        self.rooms.line('H')
        self.rooms.line('K71')
        self.rooms.line('C 0')
        self.assertEqual([p.sent for p in room.current['panes']], [[b'ls\r'], [], [b'q'], []])
        self.assertEqual(room.focus, 2)
        # The other room has its own focus.
        self.rooms.line('S 9 40 20')
        self.rooms.line('K79')
        self.assertEqual(self.rooms.rooms[9].current['panes'][0].sent, [b'y'])
        self.rooms.line('Q')
        self.assertTrue(self.rooms.quit)

    def test_only_the_shown_room_asks_for_a_frame(self):
        self.rooms.line('S 9 40 20')
        self.rooms.line('S 10 40 20')
        self.rooms.dirty = False
        hidden = self.rooms.rooms[9].current['panes'][1]
        hidden.output = True
        self.rooms.poll(time.monotonic())
        self.assertFalse(self.rooms.dirty)
        # The hidden room's shell was still read.
        self.assertFalse(hidden.output)
        shown = self.rooms.rooms[10].current['panes'][3]
        shown.output = True
        self.rooms.poll(time.monotonic())
        self.assertTrue(self.rooms.dirty)
        self.assertFalse(shown.output)
        # Pending input is written when the pane can take it.
        shown.pending.extend(b'ls')
        self.rooms.poll(time.monotonic())
        self.assertEqual(shown.pending, b'')
        self.assertEqual(sorted(self.rooms.descriptors()), sorted(p.fd for r in self.rooms.rooms.values() for p in r.panes()))

    def test_an_ended_shell_is_started_again_but_not_in_a_loop(self):
        self.rooms.line('S 9 40 20')
        room = self.rooms.rooms[9]
        old = room.current['panes'][1]
        old.exit_status, old.fd = 0, None
        self.rooms.dirty = False
        self.rooms.poll(old.born + 0.2)
        self.assertIs(room.current['panes'][1], old)
        self.assertNotIn(None, [fd for fd in self.rooms.descriptors()])
        self.rooms.poll(old.born + panes.RESPAWN_SECONDS)
        fresh = room.current['panes'][1]
        self.assertIsNot(fresh, old)
        self.assertTrue(old.closed)
        self.assertEqual((fresh.cols, fresh.rows, fresh.command), (old.cols, old.rows, ['/bin/sh']))
        self.assertEqual(fresh.born, old.born + panes.RESPAWN_SECONDS)
        self.assertTrue(self.rooms.dirty)
        # A shell still running is left alone.
        first = room.current['panes'][0]
        first.exit_status = 0
        self.rooms.poll(old.born + 10)
        self.assertIs(room.current['panes'][0], first)


class FailureTests(unittest.TestCase):
    def setUp(self):
        Pane.made.clear()
        self.rooms = panes.Rooms(DISPLAY, ['/bin/sh'], wait=ready)
        self.rooms.line('S 10 40 20')
        self.rooms.line('S 9 40 20')
        self.room = self.rooms.rooms[9]

    def test_one_failing_pane_ends_only_itself(self):
        broken, other = self.room.current['panes']
        broken.explode = 'read'
        now = broken.born
        self.rooms.dirty = False
        self.rooms.poll(now)
        self.assertEqual((broken.ended, broken.fd, broken.exit_status), (1, None, -1))
        self.assertTrue(self.rooms.dirty)
        self.assertIsNone(other.exit_status)
        self.assertEqual(len(self.rooms.rooms), 2)
        self.assertTrue(all(pane.exit_status is None for pane in self.rooms.rooms[10].panes()))
        # It is started again like a shell that exited.
        self.rooms.poll(now + panes.RESPAWN_SECONDS)
        self.assertIsNot(self.room.current['panes'][0], broken)
        self.assertTrue(broken.closed)
        self.assertIs(self.room.current['panes'][1], other)

    def test_a_pane_that_will_not_take_input_loses_it(self):
        pane = self.room.selected
        pane.explode = 'send'
        self.rooms.line('K6869')
        self.assertEqual((pane.sent, pane.exit_status, self.rooms.quit), ([], None, False))
        pane.explode = None
        self.rooms.line('K6869')
        self.assertEqual(pane.sent, [b'hi'])

    def test_characters_the_renderer_cannot_draw_are_stood_in_for(self):
        class Cache:
            def __init__(self):
                self.memory, self.asked = {65: 'mask'}, []

            def get(self, code):
                self.asked.append(code)
                if code == 0x10FFFD:
                    raise RuntimeError('Cannot render U+10FFFD')
                self.memory[code] = 'mask'
        cache, bad = Cache(), set()
        records = [(65, 1, 2, 0), (0x10FFFD, 3, 4, 16), (66, 5, 6, 1), (0x10FFFD, 7, 8, 0)]
        out = panes.drawable(cache, records, bad)
        self.assertEqual(out, [(65, 1, 2, 0), (63, 3, 4, 16), (66, 5, 6, 1), (63, 7, 8, 0)])
        self.assertEqual(records[1][0], 0x10FFFD)
        self.assertEqual((cache.asked, bad), ([0x10FFFD, 66], {0x10FFFD}))
        # Known characters are not asked about again, and good grids are kept.
        good = [(65, 1, 2, 0), (66, 1, 2, 0)]
        self.assertIs(panes.drawable(cache, good, bad), good)
        self.assertEqual(panes.drawable(cache, records, bad)[3], (63, 7, 8, 0))
        self.assertEqual(cache.asked, [0x10FFFD, 66])


class MapTests(unittest.TestCase):
    def test_a_frame_is_written_between_odd_and_even(self):
        import numpy as np
        shared = mmap.mmap(-1, 64 + 4 * 3 * 4)
        frame = np.array([[[1, 2, 3], [4, 5, 6], [7, 8, 9], [10, 11, 12]]] * 3, dtype=np.uint8)
        for expected in (2, 4):
            panes.publish(shared, frame, 10)
            magic, version, width, height, sequence, room = struct.unpack_from('<4sIIIII', shared, 0)
            self.assertEqual((magic, version, width, height, sequence, room), (b'ZPNE', 1, 4, 3, expected, 10))
            self.assertEqual(shared[64:72], bytes([3, 2, 1, 255, 6, 5, 4, 255]))
        with self.assertRaises(ValueError):
            panes.publish(shared, np.zeros((1, 1921, 3), dtype=np.uint8), 9)
        self.assertEqual(struct.unpack_from('<I', shared, 16)[0], 4)

    def test_the_pixels_are_written_while_the_sequence_is_odd(self):
        import numpy as np
        seen = []

        class Shared(bytearray):
            def __setitem__(self, key, value):
                seen.append(struct.unpack_from('<I', self, 16)[0])
                super().__setitem__(key, value)
        shared = Shared(64 + 2 * 2 * 4)
        struct.pack_into('<I', shared, 16, 6)
        panes.publish(shared, np.zeros((2, 2, 3), dtype=np.uint8), 9)
        self.assertEqual(seen, [7])
        self.assertEqual(struct.unpack_from('<I', shared, 16)[0], 8)


FAKE_DISPLAY = r'''
import os, json
from pathlib import Path
FG, BG = 0xD0D0D0, 0x101418
LOG = Path(os.environ['PANES_TEST_LOG'])
def note(**row):
    with LOG.open('a') as f:
        f.write(json.dumps(row) + '\n')
class Terminal:
    def __init__(self, pane): self.pane = pane
    def snapshot(self): return [(self.pane.code, FG, BG, 0)] * (self.pane.cols * self.pane.rows), (0, 0)
class Pane:
    def __init__(self, command, cols, rows, cw, ch, lib):
        self.cols, self.rows, self.exit_status, self.fd = cols, rows, None, None
        # The second pane shows a character the renderer cannot draw.
        Pane.count = getattr(Pane, 'count', 0) + 1
        self.code = 0x10FFFD if Pane.count == 2 else 65
        self.terminal = Terminal(self)
        ignored = int(next(line for line in open('/proc/self/status') if line.startswith('SigIgn:')).split()[1], 16)
        note(event='pane', cwd=os.getcwd(), window=os.environ.get('KITTY_WINDOW_ID'),
             fd=os.environ.get('ZELL_PANES_FD'), command=command, ignored=ignored,
             kitty=sorted(k for k in os.environ if k.startswith('KITTY') or k == 'WINDOWID'))
    def resize(self, cols, rows): self.cols, self.rows = cols, rows
    def send(self, data): note(event='send', data=data.decode())
    def reap(self): pass
    def close(self): note(event='close')
class Workspace:
    def __init__(self, cols, rows, cw, ch, command):
        self.lib, self.cols, self.rows, self.cw, self.ch, self.command = None, cols, rows, cw, ch, command
        self.tabs, self.tab, self.focus = [], 0, 0
        self.tabs.append({'panes': [Pane(command, cols, rows, cw, ch, None)]})
        self.layout()
    @property
    def current(self): return self.tabs[self.tab]
    @property
    def selected(self): return self.current['panes'][self.focus]
    def panes(self): return [p for t in self.tabs for p in t['panes']]
    def split(self, vertical=True):
        self.current['panes'].append(Pane(self.command, 8, 3, self.cw, self.ch, None)); self.layout()
    def poll(self, timeout=0.05, keyboard=False): return False
    def close(self):
        for pane in self.panes(): pane.close()
class MaskCache:
    def __init__(self, *args):
        if os.environ.get('PANES_TEST_BROKEN'): raise RuntimeError('no weights')
        self.memory = {}
    def get(self, scalar):
        if scalar == 0x10FFFD: raise RuntimeError('Cannot render U+10FFFD')
        self.memory[scalar] = b''
        return b''
    def close(self): note(event='cache-closed')
class Renderer:
    def __init__(self, cache): pass
    def render(self, records, cols, rows):
        import numpy as np
        note(event='render', cols=cols, rows=rows, cells=len(records),
             odd=sum(r[0] == 0x10FFFD for r in records), stand=sum(r[0] == 63 for r in records))
        return np.full((rows * 16, cols * 8, 3), 7, dtype=np.uint8)
'''


class HelperRunTests(unittest.TestCase):
    """tools/zell_panes.py as Zell runs it: fd in the environment, lines on
    stdin, "R"/"E" on stdout, frames in the map."""

    def run_helper(self, lines, broken=False):
        folder = Path(self.temp.name)
        display = folder / 'neuronte_display.py'
        display.write_text(FAKE_DISPLAY)
        log = folder / 'log.jsonl'
        fd = os.memfd_create('zell-panes-test', 0)
        os.ftruncate(fd, panes.MAP_BYTES)
        self.addCleanup(os.close, fd)
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(('ZELL_', 'KITTY')) and key != 'WINDOWID'}
        env.update(ZELL_PANES_FD=str(fd), KILIX_ZELL_NEURONTE_DISPLAY=str(display), PANES_TEST_LOG=str(log),
                   KITTY_WINDOW_ID='77', HOME=str(folder), SHELL='/bin/true', PYTHONDONTWRITEBYTECODE='1',
                   WINDOWID='5', KITTY_PID='9', KITTY_LISTEN_ON='unix:@x', KITTY_PUBLIC_KEY='1:x',
                   KITTY_PTY_BROKER_SESSION='s', KITTY_INSTALLATION_DIR='/opt/kilix')
        if broken:
            env['PANES_TEST_BROKEN'] = '1'
        child = subprocess.Popen([sys.executable, str(TOOLS / 'zell_panes.py')], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, pass_fds=(fd,), env=env, cwd='/')
        first = child.stdout.readline()
        for text, pause in lines:
            child.stdin.write(text.encode() + b'\n')
            child.stdin.flush()
            time.sleep(pause)
        child.stdin.close()
        rest = child.stdout.read()
        child.stdout.close()
        code = child.wait(timeout=20)
        shared = mmap.mmap(fd, 64 + 4096)
        header = struct.unpack_from('<4sIIIII', shared, 0)
        shared.close()
        rows = [__import__('json').loads(line) for line in log.read_text().splitlines()] if log.exists() else []
        return first, rest, code, header, rows

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)

    def test_shows_a_room_and_types_into_it(self):
        first, rest, code, header, rows = self.run_helper([('S 9 40 20', 0.4), ('K6869', 0.2), ('Q', 0.0)])
        self.assertEqual((first, rest, code), (b'R\n', b'', 0))
        magic, version, width, height, sequence, room = header
        self.assertEqual((magic, version, width, height, room), (b'ZPNE', 1, 320, 320, 9))
        self.assertTrue(sequence and not sequence & 1)
        made = [row for row in rows if row['event'] == 'pane']
        self.assertEqual(len(made), 2)
        for row in made:
            # The shells start in the home folder, not in the Kilix window.
            self.assertEqual((row['cwd'], row['window'], row['fd'], row['command']),
                             (str(Path(self.temp.name).resolve()), None, None, ['/bin/true']))
            self.assertEqual(row['kitty'], ['KITTY_INSTALLATION_DIR'])
            # Broken pipes and file size limits stop a shell's commands as usual.
            self.assertEqual(row['ignored'] & ((1 << (signal.SIGPIPE - 1)) | (1 << (signal.SIGXFSZ - 1))), 0)
        self.assertIn({'event': 'send', 'data': 'hi'}, rows)
        # The second pane's 19 columns by 20 rows are drawn as stand-ins.
        self.assertIn({'event': 'render', 'cols': 40, 'rows': 20, 'cells': 800, 'odd': 0, 'stand': 380}, rows)
        self.assertEqual([row['event'] for row in rows[-3:]], ['close', 'close', 'cache-closed'])

    def test_stops_when_zell_goes(self):
        first, rest, code, header, rows = self.run_helper([('S 10 24 8', 0.3)])
        self.assertEqual((first, code), (b'R\n', 0))
        self.assertEqual(header[5], 10)
        self.assertEqual(sum(row['event'] == 'close' for row in rows), 4)

    def test_hidden_rooms_are_not_painted(self):
        first, rest, code, header, rows = self.run_helper([('S 9 40 20', 0.3), ('H', 0.1), ('K6869', 0.3), ('Q', 0.0)])
        self.assertEqual(sum(row['event'] == 'render' for row in rows), 1)
        self.assertNotIn({'event': 'send', 'data': 'hi'}, rows)

    def test_a_broken_display_says_so_without_terminal_text(self):
        first, rest, code, header, rows = self.run_helper([], broken=True)
        self.assertEqual(code, 1)
        self.assertEqual(first, b'E The panes stopped. Check the Neuronte display library and weights.\n')
        self.assertEqual(header[0], b'\0\0\0\0')


if __name__ == '__main__':
    unittest.main()
