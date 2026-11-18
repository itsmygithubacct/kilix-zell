"""The pane rooms' helper: layout and zoom, protocol, agents and titles,
respawn, the map, and a run through its real pipes and map with a stand-in
for the Neuronte display."""
import mmap
import os
from pathlib import Path
import pty
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
import zell_providers

FG, BG = 0xD0D0D0, 0x101418


class Terminal:
    """The parts of neuronte_display.Terminal the helper uses. It keeps
    `kept` rows, shows the `rows` from `first`; its program's modes are in
    `modes`, its text in `text` (one string a row shown)."""

    def __init__(self, owner):
        self.owner = owner
        self.kept, self.first, self.modes, self.alternate, self.text = None, 0, set(), False, None
        self.calls = []

    def snapshot(self):
        pane = self.owner
        cell = (pane.letter, FG, BG, 0)
        return [cell] * (pane.cols * pane.rows), pane.cursor

    def scrollbar(self):
        rows = self.owner.rows
        kept = rows if self.kept is None or self.alternate else self.kept
        return kept, min(self.first, kept - rows) if not self.alternate else 0, rows

    def last(self):
        return self.scrollbar()[0] - self.owner.rows

    def scroll(self, rows):
        self.calls.append(('scroll', rows))
        self.first = min(max(0, self.first + rows), self.last())

    def scroll_to(self, row):
        self.calls.append(('scroll_to', row))
        self.first = min(max(0, row), self.last())

    def scroll_top(self):
        self.calls.append(('top',))
        self.first = 0

    def scroll_bottom(self):
        self.calls.append(('bottom',))
        self.first = self.last()

    def mode(self, number, ansi=False):
        return number in self.modes

    def mouse_tracking(self):
        return bool(self.modes & {9, 1000, 1002, 1003})

    def alternate_screen(self):
        return self.alternate

    def mouse(self, action, button, mods, x, y):
        self.calls.append(('mouse', action, button, mods, x, y))
        if not self.mouse_tracking():
            return b''
        return f'<{action};{button};{mods};{x};{y}>'.encode()

    def paste(self, data):
        self.calls.append(('paste', data))
        return b'[' + data + b']'

    def rows_text(self):
        rows = self.text or [''] * self.owner.rows
        return [list(row.ljust(self.owner.cols)[:self.owner.cols]) for row in rows]


class Pane:
    made = []

    def __init__(self, command, cols, rows, cw, ch, lib):
        self.command, self.cols, self.rows = command, cols, rows
        self.sent, self.closed, self.exit_status, self.fd = [], False, None, 1000 + len(Pane.made)
        self.letter, self.cursor = ord('A') + len(Pane.made) % 26, (0, 0)
        # What front() reports: by default the shell itself, at its prompt.
        self.pid = 5000 + len(Pane.made)
        self.group, self.title = self.pid, 'sh'

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
        # Until flushed, as a pane that has not written it yet holds it.
        self.pending.extend(data)

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


def front(pane):
    """foreground() over the fake panes."""
    return (pane.group, pane.title) if pane.fd is not None else (None, None)


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

        # The split Zell draws the title bars over (tests/test_desktop.c).
        self.assertEqual(panes.column_widths(120, 2), [60, 59])
        self.assertEqual(panes.column_widths(238, 4), [59, 59, 59, 58])

    def test_sizes_the_map_and_renderer_can_hold(self):
        self.assertTrue(panes.valid_size(20, 8, 2))
        self.assertTrue(panes.valid_size(480, 135, 4))
        for cols, rows in ((19, 8), (481, 8), (20, 7), (20, 136)):
            self.assertFalse(panes.valid_size(cols, rows, 2))
        self.assertEqual(panes.MAP_BYTES, 64 + 3840 * 2160 * 4)

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

    def test_a_zoomed_pane_has_the_room_and_the_others_keep_their_size(self):
        Pane.made.clear()
        workspace = panes.room_workspace(DISPLAY, 4, 23, 3, ['/bin/sh'])
        sizes = [(pane.cols, pane.rows) for pane in workspace.current['panes']]
        workspace.zoom = 2
        self.assertEqual(workspace.layout(), [None, None, (0, 0, 23, 3), None])
        third = workspace.current['panes'][2]
        self.assertEqual((third.cols, third.rows), (23, 3))
        self.assertEqual([(p.cols, p.rows) for i, p in enumerate(workspace.current['panes']) if i != 2],
                         [size for i, size in enumerate(sizes) if i != 2])
        third.cursor = (4, 1)
        workspace.focus = 2
        records = panes.room_grid(DISPLAY, workspace)
        # Every cell is the zoomed pane's: no other pane and no lines.
        self.assertEqual({record[0] for record in records}, {third.letter})
        self.assertEqual([at for at, record in enumerate(records) if record[3] & 16], [23 + 4])
        workspace.resize(30, 5)
        self.assertEqual((third.cols, third.rows), (30, 5))
        workspace.zoom = None
        workspace.layout()
        self.assertEqual([p.cols for p in workspace.current['panes']], panes.column_widths(30, 4))

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
        self.said = []
        self.rooms = panes.Rooms(DISPLAY, ['/bin/sh'], wait=ready, front=front, agents=[('codex', 'Codex')],
                                 say=self.said.append, clock=lambda: 50.0)

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
        for text in ('S 11 40 20', 'S 0 40 20', 'S 9 19 20', 'S 9 481 20', 'S 9 40 7', 'S 9 40 136',
                     'S 9 40', 'S nine 40 20', 'S 9 40 20 1', 'C 3', 'Kzz', 'K6c', 'X', '', 'Q9',
                     'F 0', 'Z 0', 'Z -1', 'X 0', 'A 0 codex', 'F', 'Z'):
            self.rooms.line(text)
        self.assertEqual((self.rooms.rooms, self.rooms.shown, self.rooms.dirty, self.rooms.quit), ({}, None, False, False))
        self.assertEqual(self.said, [])
        self.rooms.line('S 10 40 20')
        room, self.rooms.dirty = self.rooms.rooms[10], False
        for text in ('F 4', 'F -1', 'F x', 'F 1 2', 'Z 4', 'Z -2', 'Z x', 'X 4', 'X -1', 'X x',
                     'A 4 codex', 'A 1 nosuch', 'A 1', 'A x codex', 'C 30'):
            self.rooms.line(text)
        self.assertEqual((room.focus, room.zoom, self.rooms.dirty, self.said), (0, None, False, []))
        self.assertFalse(any(p.closed or p.sent for p in room.panes()))

    def test_keys_go_to_the_focused_pane_of_the_shown_room_only(self):
        self.rooms.line('S 10 40 20')
        room = self.rooms.rooms[10]
        first, second, third, fourth = room.current['panes']
        self.rooms.line('K6c730d')
        self.assertEqual(first.sent, [b'ls\r'])
        self.rooms.dirty = False
        self.rooms.line('F 2')
        self.assertEqual((room.focus, self.rooms.dirty), (2, True))
        self.rooms.line('K71')
        self.assertEqual(third.sent, [b'q'])
        for index in (3, 1, 0, 2):
            self.rooms.line('F %d' % index)
            self.assertEqual(room.focus, index)
        self.rooms.line('Knothex')
        self.rooms.line('K')
        self.assertEqual(third.sent, [b'q'])
        # Hidden, nothing is typed into it.
        self.rooms.line('H')
        self.rooms.line('K71')
        self.rooms.line('F 0')
        self.assertEqual([p.sent for p in room.current['panes']], [[b'ls\r'], [], [b'q'], []])
        self.assertEqual(room.focus, 2)
        # The other room has its own focus.
        self.rooms.line('S 9 40 20')
        self.rooms.line('K79')
        self.assertEqual(self.rooms.rooms[9].current['panes'][0].sent, [b'y'])
        self.rooms.line('Q')
        self.assertTrue(self.rooms.quit)

    def test_zoom_shows_one_pane_and_gives_it_the_focus(self):
        self.rooms.line('S 10 40 20')
        room = self.rooms.rooms[10]
        self.rooms.dirty = False
        self.rooms.line('Z 2')
        self.assertEqual((room.zoom, room.focus, self.rooms.dirty), (2, 2, True))
        third = room.current['panes'][2]
        self.assertEqual((third.cols, third.rows), (40, 20))
        self.rooms.line('K71')
        self.assertEqual(third.sent, [b'q'])
        # Resized while zoomed, the zoomed pane follows.
        self.rooms.line('S 10 50 22')
        self.assertEqual((third.cols, third.rows), (50, 22))
        self.rooms.dirty = False
        self.rooms.line('Z -1')
        self.assertEqual((room.zoom, room.focus, self.rooms.dirty), (None, 2, True))
        self.assertEqual([p.cols for p in room.current['panes']], panes.column_widths(50, 4))
        # A pane hidden by a zoom does not take the focus.
        self.rooms.line('Z 1')
        self.rooms.line('F 3')
        self.assertEqual(room.focus, 1)
        self.rooms.line('F 1')
        self.assertEqual(room.focus, 1)
        self.rooms.line('Z -1')
        # Each room keeps its own.
        self.rooms.line('Z 3')
        self.rooms.line('S 9 40 20')
        self.assertIsNone(self.rooms.rooms[9].zoom)
        self.assertEqual(room.zoom, 3)

    def test_restart_hangs_up_what_runs_in_front(self):
        hung = []
        self.rooms.hang_up = lambda group, sig: hung.append((group, sig))
        self.rooms.line('S 10 40 20')
        first, second = self.rooms.rooms[10].current['panes'][:2]
        self.rooms.line('X 0')
        self.assertEqual(hung, [])
        second.group = second.pid + 3
        self.rooms.line('X 1')
        self.assertEqual(hung, [(second.pid + 3, signal.SIGHUP)])
        self.assertTrue(second.closed)
        # A group already gone does not stop the restart.
        third = self.rooms.rooms[10].current['panes'][2]
        third.group = 1

        def gone(group, sig):
            raise ProcessLookupError
        self.rooms.hang_up = gone
        self.rooms.line('X 2')
        self.assertTrue(third.closed)

    def test_a_clear_line_empties_a_real_prompt(self):
        """CLEAR_LINE in a real interactive bash with the cursor mid-line."""
        bash = '/bin/bash'
        if not os.path.exists(bash):
            self.skipTest('no bash')
        pid, fd = pty.fork()
        if pid == 0:
            os.execve(bash, [bash, '--norc', '--noprofile', '-i'], {'PS1': '$ ', 'TERM': 'xterm', 'PATH': '/usr/bin:/bin'})
        output = bytearray()

        def read_for(seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                if select.select([fd], [], [], 0.05)[0]:
                    try:
                        output.extend(os.read(fd, 4096))
                    except OSError:
                        return
        try:
            read_for(0.5)
            os.write(fd, b'echo ab\x1b[D')
            read_for(0.3)
            os.write(fd, panes.CLEAR_LINE + b'echo MARK-$((6*7))\r')
            read_for(1.0)
        finally:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
            os.close(fd)
        text = output.decode('utf-8', 'replace')
        self.assertIn('MARK-42\r\n', text)
        self.assertNotIn('MARK-42b', text)

    def test_restart_puts_a_new_shell_in_the_pane(self):
        self.rooms.line('S 10 40 20')
        room = self.rooms.rooms[10]
        old, other = room.current['panes'][1], room.current['panes'][0]
        self.rooms.dirty = False
        self.rooms.line('X 1')
        fresh = room.current['panes'][1]
        self.assertIsNot(fresh, old)
        self.assertTrue(old.closed and not other.closed)
        self.assertEqual((fresh.cols, fresh.rows, fresh.command, fresh.born), (old.cols, old.rows, ['/bin/sh'], 50.0))
        self.assertIs(room.current['panes'][0], other)
        self.assertTrue(self.rooms.dirty)

    def test_an_agent_is_typed_at_an_idle_prompt_only(self):
        self.assertEqual(panes.CLEAR_LINE, b'\x05\x15')
        self.rooms.line('S 10 40 20')
        room = self.rooms.rooms[10]
        first, second, third, fourth = room.current['panes']
        self.rooms.line('A 1 codex')
        self.assertEqual(second.sent, [b'\x05\x15codex\r'])
        self.assertEqual((first.sent, third.sent, self.said), ([], [], []))
        # Something else is in front: nothing is typed, the pane says why.
        third.group = third.pid + 7
        self.rooms.line('A 2 codex')
        self.assertEqual(third.sent, [])
        self.assertEqual(self.said, ['N 10 2 ' + panes.BUSY])
        # So does a pane whose shell has ended, or whose front is unknown.
        fourth.fd = None
        first.group = None
        self.rooms.line('A 3 codex')
        self.rooms.line('A 0 codex')
        self.assertEqual(self.said[1:], ['N 10 3 ' + panes.BUSY, 'N 10 0 ' + panes.BUSY])
        self.assertEqual(first.sent, [])
        # A pane that will not take input loses it, and the helper goes on.
        second.explode = 'send'
        self.rooms.line('A 1 codex')
        self.assertEqual(second.sent, [b'\x05\x15codex\r'])
        second.explode = None
        # A shell that has become another program (exec) is not a prompt.
        second.name, second.title = 'sh', 'vim'
        self.rooms.line('A 1 codex')
        self.assertEqual(self.said[-1], 'N 10 1 ' + panes.BUSY)
        second.title = 'sh'
        self.rooms.line('A 1 codex')
        self.assertEqual(second.sent, [b'\x05\x15codex\r'] * 2)
        # Hidden, nothing is started.
        self.rooms.line('H')
        self.rooms.line('A 1 codex')
        self.assertEqual(second.sent, [b'\x05\x15codex\r'] * 2)

    def test_what_each_pane_runs_is_said_when_it_changes(self):
        self.rooms.line('S 9 40 20')
        self.rooms.line('S 10 40 20')
        self.rooms.line('H')
        self.rooms.poll(100.0)
        self.assertEqual(sorted(self.said), sorted(['T 9 0 sh', 'T 9 1 sh', 'T 10 0 sh', 'T 10 1 sh', 'T 10 2 sh', 'T 10 3 sh']))
        del self.said[:]
        third = self.rooms.rooms[10].current['panes'][2]
        third.title = 'vim'
        self.rooms.poll(100.0 + panes.TITLE_SECONDS / 2)
        self.assertEqual(self.said, [])
        self.rooms.poll(100.0 + panes.TITLE_SECONDS)
        self.assertEqual(self.said, ['T 10 2 vim'])
        # An ended shell with no name of its own is a shell.
        third.fd = None
        self.rooms.poll(101.0 + panes.TITLE_SECONDS)
        self.assertEqual(self.said[1:], ['T 10 2 shell'])

    def test_titles_are_what_zell_can_show(self):
        self.assertEqual(panes.title_text('vim'), 'vim')
        self.assertEqual(panes.title_text(None), 'shell')
        self.assertEqual(panes.title_text(''), 'shell')
        self.assertEqual(panes.title_text(' \u00e9 b\t'), 'b')
        self.assertEqual(panes.title_text('tmux: server'), 'tmux: server')
        self.assertEqual(panes.title_text('x' * 40), 'x' * 31)

    def test_agents_are_the_ones_on_path(self):
        names = list(zell_providers.CLI_FILES)
        self.assertGreaterEqual(len(names), 2)
        found = panes.available_agents(which=lambda name: name == names[1])
        self.assertEqual(found, [(names[1], zell_providers.LABELS[names[1]])])
        self.assertEqual(panes.available_agents(which=lambda name: True),
                         [(name, zell_providers.LABELS[name]) for name in names])
        self.assertEqual(panes.available_agents(which=lambda name: None), [])

    def test_the_front_of_a_real_terminal(self):
        pid, fd = pty.fork()
        if pid == 0:
            time.sleep(5)
            os._exit(0)
        try:
            pane = SimpleNamespace(fd=fd, pid=pid)
            deadline = time.monotonic() + 3
            while panes.foreground(pane)[0] != pid and time.monotonic() < deadline:
                time.sleep(0.01)
            group, name = panes.foreground(pane)
            self.assertEqual(group, pid)
            self.assertEqual(name, Path(f'/proc/{pid}/comm').read_text().strip())
        finally:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
            os.close(fd)
        self.assertEqual(panes.foreground(SimpleNamespace(fd=None, pid=1)), (None, None))
        self.assertEqual(panes.foreground(SimpleNamespace(fd=fd, pid=1)), (None, None))

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


def copied(said):
    """The text in the Y lines said, and how many pieces it came in."""
    pieces = [line[2:] for line in said if line.startswith('Y ')]
    assert pieces and pieces[-1] == '.', said
    import base64
    return base64.b64decode(''.join(pieces[:-1])).decode(), len(pieces) - 1


class ScrollMouseTests(unittest.TestCase):
    """Scrollback, the pointer, selection and copy, and pastes, over the
    stand-in terminal."""

    def setUp(self):
        Pane.made.clear()
        self.said, self.now = [], 50.0
        self.rooms = panes.Rooms(DISPLAY, ['/bin/sh'], wait=ready, front=front, say=self.said.append,
                                 clock=lambda: self.now)
        self.rooms.line('S 9 40 20')
        self.first, self.second = self.rooms.rooms[9].current['panes']
        self.term = self.first.terminal
        self.term.kept = 120
        self.term.first = 100
        self.rooms.poll(0.0)
        self.said.clear()

    def bars(self):
        self.rooms.poll(1.0)
        out = [line for line in self.said if line.startswith('B ')]
        self.said[:] = [line for line in self.said if not line.startswith('B ')]
        return out

    def test_the_scrollback_is_said_when_it_changes(self):
        rooms = panes.Rooms(DISPLAY, ['/bin/sh'], wait=ready, front=front, say=self.said.append)
        rooms.line('S 9 40 20')
        rooms.rooms[9].current['panes'][0].terminal.kept = 50
        rooms.poll(0.0)
        self.assertEqual([line for line in self.said if line.startswith('B ')], ['B 9 0 50 0 20 0', 'B 9 1 20 0 20 0'])
        self.said.clear()
        rooms.poll(0.1)
        self.assertEqual(self.said, [])
        terminal = rooms.rooms[9].current['panes'][1].terminal
        terminal.modes = {1000}
        rooms.poll(0.2)
        terminal.modes = {1000, 1003}
        rooms.poll(0.3)
        self.assertEqual(self.said, ['B 9 1 20 0 20 1', 'B 9 1 20 0 20 3'])
        # A hidden room says nothing, and its change is said when shown.
        self.said.clear()
        rooms.line('H')
        terminal.modes = set()
        rooms.poll(0.4)
        self.assertEqual(self.said, [])
        rooms.line('S 9 40 20')
        rooms.poll(0.5)
        self.assertEqual(self.said, ['B 9 1 20 0 20 0'])

    def test_the_wheel_scrolls_back_and_typing_comes_back(self):
        self.rooms.line('M 0 0 4 0 10 10')
        self.assertEqual(self.term.calls, [('scroll', -3)])
        self.assertEqual(self.bars(), ['B 9 0 120 97 20 0'])
        self.rooms.line('M 0 0 5 0 10 10')
        self.rooms.line('M 0 0 4 0 10 10')
        self.rooms.line('M 0 0 4 0 10 10')
        self.assertEqual(self.bars(), ['B 9 0 120 94 20 0'])
        self.rooms.dirty = False
        self.rooms.line('K68')
        self.assertEqual(self.first.sent, [b'h'])
        self.assertEqual(self.term.calls[-1], ('bottom',))
        self.assertTrue(self.rooms.dirty)
        self.assertEqual(self.bars(), ['B 9 0 120 100 20 0'])
        # At the live rows already, typing does not scroll.
        self.rooms.line('K69')
        self.assertEqual(self.term.calls[-1], ('bottom',))
        self.assertEqual(len([c for c in self.term.calls if c == ('bottom',)]), 1)

    def test_the_wheel_goes_to_a_program_that_asked_unless_shift(self):
        self.term.modes = {1000, 1006}
        self.rooms.line('M 0 0 4 0 17 33')
        self.assertEqual(self.first.sent, [b'<0;4;0;17;33>'])
        self.rooms.line('M 0 0 5 1 17 33')
        self.assertEqual(self.term.calls[-1], ('scroll', 3))
        self.assertEqual(len(self.first.sent), 1)

    def test_the_wheel_on_an_alternate_screen_sends_arrows_if_asked(self):
        self.term.alternate = True
        self.rooms.line('M 0 0 4 0 0 0')
        self.assertEqual(self.first.sent, [])
        self.term.modes = {1007}
        self.rooms.line('M 0 0 4 0 0 0')
        self.rooms.line('M 0 0 5 0 0 0')
        self.term.modes = {1007, 1}
        self.rooms.line('M 0 0 5 0 0 0')
        self.assertEqual(self.first.sent, [b'\x1b[A' * 3, b'\x1b[B' * 3, b'\x1bOB' * 3])
        self.assertFalse([c for c in self.term.calls if c[0] == 'scroll'])

    def test_presses_and_drags_go_to_a_program_that_asked(self):
        self.term.modes = {1002}
        for line in ('M 0 0 1 0 9 17', 'M 0 2 1 0 30 17', 'M 0 1 1 0 30 17', 'M 1 0 2 2 0 0'):
            self.rooms.line(line)
        self.assertEqual(self.first.sent, [b'<0;1;0;9;17>', b'<2;1;0;30;17>', b'<1;1;0;30;17>'])
        self.assertEqual(self.second.sent, [])
        self.assertIsNone(getattr(self.first, 'selection', None))
        # The right button does not select.
        self.assertIsNone(getattr(self.second, 'selection', None))
        # Lines out of range change nothing.
        for line in ('M 0 3 1 0 9 17', 'M 0 0 12 0 9 17', 'M 0 0 1 8 9 17', 'M 0 0 1 0 x 17', 'M 4 0 1 0 9 17', 'M 0 0 1 0 9'):
            self.rooms.line(line)
        self.assertEqual(len(self.first.sent), 3)
        # With Shift held the left button selects instead.
        self.rooms.line('M 0 0 1 1 9 17')
        self.assertEqual(len(self.first.sent), 3)
        self.assertIsNotNone(self.first.selection)

    def test_a_drag_selects_and_the_release_copies(self):
        self.term.text = ['', 'hello world   ', 'second line', 'third']
        # Row 1 column 6 to row 2 column 5, counted from the first row shown.
        self.rooms.line('M 0 0 1 0 50 20')
        self.assertTrue(self.rooms.dirty)
        self.assertEqual(self.first.selection.anchor, (101, 6))
        self.rooms.line('M 0 2 1 0 44 40')
        self.assertEqual(panes.selected_spans(self.first), {1: (6, 19), 2: (0, 5)})
        self.assertEqual(self.said, [])
        self.rooms.line('M 0 1 1 0 44 40')
        self.assertEqual(copied(self.said), ('world\nsecond', 1))
        self.assertEqual(self.said[-1], 'N 9 0 Copied 2 lines.')
        # The text stays chosen and is drawn reversed.
        records = panes.room_grid(DISPLAY, self.rooms.rooms[9])
        reversed_at = [at for at, record in enumerate(records) if record[3] & 16]
        self.assertEqual(reversed_at, [0] + [40 + c for c in range(6, 20)] + [80 + c for c in range(0, 6)])
        # Scrolled back three rows, it is three rows lower.
        self.term.first = 97
        self.assertEqual(panes.selected_spans(self.first), {4: (6, 19), 5: (0, 5)})
        # Typing ends it.
        self.rooms.line('K61')
        self.assertIsNone(self.first.selection)

    def test_a_click_without_a_drag_selects_nothing(self):
        self.term.text = ['abc']
        self.rooms.line('M 0 0 1 0 0 0')
        self.rooms.line('M 0 1 1 0 0 0')
        self.assertIsNone(self.first.selection)
        self.assertEqual(self.said, [])

    def test_double_and_triple_clicks_take_words_and_lines(self):
        self.first.cols = 40
        self.term.text = ['run ./tools/zell_panes.py now  ']
        self.rooms.line('M 0 0 1 0 72 0')
        self.rooms.line('M 0 1 1 0 72 0')
        self.now += 0.1
        self.rooms.line('M 0 0 1 0 72 0')
        self.rooms.line('M 0 1 1 0 72 0')
        self.assertEqual(copied(self.said)[0], './tools/zell_panes.py')
        self.said.clear()
        self.now += 0.1
        self.rooms.line('M 0 0 1 0 72 0')
        self.rooms.line('M 0 1 1 0 72 0')
        self.assertEqual(copied(self.said)[0], 'run ./tools/zell_panes.py now')
        # A fourth is a single click again; so is one too late or elsewhere.
        self.said.clear()
        self.now += 0.1
        self.rooms.line('M 0 0 1 0 72 0')
        self.rooms.line('M 0 1 1 0 72 0')
        self.assertEqual(self.said, [])
        self.now += 0.1
        self.rooms.line('M 0 0 1 0 72 0')
        self.now += 0.5
        self.rooms.line('M 0 0 1 0 72 0')
        self.rooms.line('M 0 1 1 0 72 0')
        self.assertEqual(self.said, [])
        self.now += 0.1
        self.rooms.line('M 0 0 1 0 80 0')
        self.rooms.line('M 0 1 1 0 80 0')
        self.assertEqual(self.said, [])

    def test_a_long_copy_comes_in_pieces_and_is_capped(self):
        self.term.text = ['x' * 40] * 20
        self.first.cols = 40
        self.rooms.line('M 0 0 1 0 0 0')
        self.rooms.line('M 0 1 1 0 319 319')
        text, pieces = copied(self.said)
        self.assertEqual(text, '\n'.join(['x' * 40] * 20))
        self.assertEqual(pieces, 1)
        self.said.clear()
        self.rooms.copy(0, 'é' * panes.COPY_MAX)
        text, pieces = copied(self.said)
        # Cut at a character, within the cap.
        self.assertEqual(text, 'é' * (panes.COPY_MAX // 2))
        self.assertEqual(pieces, -(-len(panes.base64.b64encode(text.encode())) // panes.COPY_CHUNK))
        self.assertTrue(all(len(line) <= panes.COPY_CHUNK + 2 for line in self.said))

    def test_a_selection_ends_when_the_screen_changes(self):
        self.term.text = ['abcdef']
        self.rooms.line('M 0 0 1 0 0 0')
        self.rooms.line('M 0 2 1 0 30 0')
        self.term.alternate = True
        self.assertEqual(panes.selected_spans(self.first), {})
        self.assertIsNone(self.first.selection)

    def test_scroll_keys_and_rows(self):
        self.rooms.line('W 0 up')
        self.rooms.line('W 0 down')
        self.rooms.line('W 0 top')
        self.rooms.line('W 0 bottom')
        self.rooms.line('V 0 42')
        self.rooms.line('W 0 sideways')
        self.rooms.line('V 0 -1')
        self.rooms.line('V 9 1')
        self.assertEqual(self.term.calls, [('scroll', -19), ('scroll', 19), ('top',), ('bottom',), ('scroll_to', 42)])
        self.term.alternate = True
        for how in ('up', 'down', 'top', 'bottom'):
            self.rooms.line('W 0 ' + how)
        self.assertEqual(self.first.sent, [b'\x1b[5;2~', b'\x1b[6;2~', b'\x1b[1;2H', b'\x1b[1;2F'])

    def test_a_paste_is_framed_by_the_terminal_and_sent_whole(self):
        self.term.first = 90
        self.rooms.line('P 9 0 6869')
        self.rooms.line('P 9 0 0a21')
        self.assertEqual(self.first.sent, [])
        self.rooms.line('P 9 0 .')
        self.assertEqual(self.term.calls[-2:], [('bottom',), ('paste', b'hi\n!')])
        self.assertEqual(self.first.sent, [b'[hi\n!]'])
        # To a pane of a room that is not shown, by its room.
        self.rooms.line('S 10 40 20')
        self.rooms.line('P 9 1 41')
        self.rooms.line('P 9 1 .')
        self.assertEqual(self.second.sent, [b'[A]'])
        # A bad piece spoils the paste; so does one too long; an end with
        # nothing before it sends nothing.
        self.rooms.line('P 9 1 4')
        self.rooms.line('P 9 1 41')
        self.rooms.line('P 9 1 .')
        self.rooms.line('P 9 1 .')
        self.rooms.line('P 11 0 41')
        self.rooms.line('P 9 7 41')
        self.assertEqual(self.second.sent, [b'[A]'])
        self.rooms.pastes[9, 1] = bytearray(panes.PASTE_MAX)
        self.rooms.line('P 9 1 41')
        self.rooms.line('P 9 1 .')
        self.assertEqual(self.second.sent, [b'[A]'])

    def test_a_long_paste_goes_as_the_pane_reads(self):
        data = bytes(range(256)) * 1024
        self.rooms.line('P 9 0 ' + data.hex())
        self.first.pending = bytearray(panes.PASTE_STEP)
        self.rooms.line('P 9 0 .')
        self.assertEqual(self.first.sent, [])
        self.first.pending = bytearray()
        self.rooms.poll(1.0)
        self.assertEqual(len(self.first.sent), 1)
        self.assertEqual(len(self.first.sent[0]), panes.PASTE_STEP)
        self.first.pending.clear()
        for _ in range(10):
            self.rooms.poll(1.0)
        self.assertEqual(b''.join(self.first.sent), b'[' + data + b']')
        self.assertEqual(self.rooms.backlog, {})


class RealTerminalTests(unittest.TestCase):
    """The same, through the Neuronte display's real terminal (Ghostty), when
    it is there; ZELL_PANES_REAL_DISPLAY=1 makes it required."""

    def setUp(self):
        try:
            import zell_terminal
            self.display = zell_terminal.load_display()
            if not hasattr(self.display.Terminal, 'scrollbar'):
                raise RuntimeError('a display from before 0.2.4')
        except Exception as error:
            if os.environ.get('ZELL_PANES_REAL_DISPLAY') == '1':
                raise
            self.skipTest(f'no 0.2.4 Neuronte display here: {error}')
        Pane.made.clear()
        self.said = []
        self.rooms = panes.Rooms(DISPLAY, ['/bin/sh'], wait=ready, front=front, say=self.said.append,
                                 clock=lambda: 50.0)
        self.rooms.line('S 9 40 8')
        self.pane = self.rooms.rooms[9].current['panes'][0]
        self.terminal = self.display.Terminal(self.pane.cols, self.pane.rows)
        self.addCleanup(self.terminal.close)
        self.pane.terminal = self.terminal

    def test_scrollback_selection_and_copy(self):
        for i in range(30):
            self.terminal.feed(f'line {i}\r\n'.encode())
        self.rooms.poll(0.0)
        self.assertIn('B 9 0 31 23 8 0', self.said)
        self.rooms.line('M 0 0 4 0 0 0')
        self.rooms.line('M 0 0 4 0 0 0')
        self.assertEqual(self.terminal.scrollbar(), (31, 17, 8))
        self.assertEqual(''.join(self.terminal.rows_text()[0]).rstrip(), 'line 17')
        # Row 0 column 5 to row 1 column 6: "17" and "line 18".
        self.rooms.line('M 0 0 1 0 40 0')
        self.rooms.line('M 0 1 1 0 48 16')
        self.assertEqual(copied(self.said)[0], '17\nline 18')
        self.rooms.line('K0d')
        self.assertEqual(self.terminal.scrollbar(), (31, 23, 8))

    def test_the_mouse_and_pastes_as_the_program_asked(self):
        self.terminal.feed(b'\x1b[?1002h\x1b[?1006h\x1b[?2004h')
        self.rooms.line('M 0 0 1 0 9 17')
        self.rooms.line('M 0 2 1 0 25 17')
        self.rooms.line('M 0 0 4 0 0 0')
        self.rooms.line('P 9 0 610a62')
        self.rooms.line('P 9 0 .')
        self.assertEqual(self.pane.sent, [b'\x1b[<0;2;2M', b'\x1b[<32;4;2M', b'\x1b[<64;1;1M',
                                          b'\x1b[200~a\nb\x1b[201~'])
        self.rooms.poll(0.0)
        self.assertIn('B 9 0 8 0 8 1', self.said)


class FailureTests(unittest.TestCase):
    def setUp(self):
        Pane.made.clear()
        self.rooms = panes.Rooms(DISPLAY, ['/bin/sh'], wait=ready, front=front)
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
            panes.publish(shared, np.zeros((1, 3841, 3), dtype=np.uint8), 9)
        with self.assertRaises(ValueError):
            panes.publish(shared, np.zeros((2161, 1, 3), dtype=np.uint8), 9)
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
    def scrollbar(self): return self.pane.rows, 0, self.pane.rows
    def mouse_tracking(self): return False
    def mode(self, number, ansi=False): return False
    def alternate_screen(self): return False
    def scroll_bottom(self): pass
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

    def run_helper(self, lines, broken=False, agents=(), not_runnable=(), old=False):
        folder = Path(self.temp.name)
        # PATH is a folder with a command for each agent given, and files
        # that are not commands for the rest.
        commands = folder / 'bin'
        commands.mkdir(exist_ok=True)
        for name in agents:
            (commands / name).write_text('#!/bin/sh\n')
            (commands / name).chmod(0o755)
        for name in not_runnable:
            (commands / name).write_text('#!/bin/sh\n')
            (commands / name).chmod(0o644)
        display = folder / 'neuronte_display.py'
        # An old display has no scrollback calls.
        display.write_text(FAKE_DISPLAY.replace('def scrollbar(self)', 'def no_scrollbar(self)') if old else FAKE_DISPLAY)
        log = folder / 'log.jsonl'
        fd = os.memfd_create('zell-panes-test', 0)
        os.ftruncate(fd, panes.MAP_BYTES)
        self.addCleanup(os.close, fd)
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(('ZELL_', 'KITTY')) and key != 'WINDOWID'}
        env.update(ZELL_PANES_FD=str(fd), KILIX_ZELL_NEURONTE_DISPLAY=str(display), PANES_TEST_LOG=str(log),
                   KITTY_WINDOW_ID='77', HOME=str(folder), SHELL='/bin/true', PYTHONDONTWRITEBYTECODE='1',
                   WINDOWID='5', KITTY_PID='9', KITTY_LISTEN_ON='unix:@x', KITTY_PUBLIC_KEY='1:x',
                   KITTY_PTY_BROKER_SESSION='s', KITTY_INSTALLATION_DIR='/opt/kilix', PATH=str(commands))
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
        names = list(zell_providers.CLI_FILES)
        first, rest, code, header, rows = self.run_helper(
            [('S 9 40 20', 0.4), ('K6869', 0.2), ('A 0 ' + names[0], 0.2), ('Q', 0.0)], agents=names)
        self.assertEqual((first, code), (b'R\n', 0))
        # The agents found, what each pane runs, and why the agent was not
        # typed: these stand-in panes have no terminal to be at a prompt in.
        self.assertEqual(rest.decode().splitlines(),
                         ['G %s %s' % (name, zell_providers.LABELS[name]) for name in names] +
                         ['B 9 0 20 0 20 0', 'B 9 1 20 0 20 0', 'T 9 0 shell', 'T 9 1 shell',
                          'N 9 0 ' + panes.BUSY])
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

    def test_only_agents_that_can_run_are_offered(self):
        names = list(zell_providers.CLI_FILES)
        first, rest, code, header, rows = self.run_helper([('Q', 0.0)], agents=names[:1], not_runnable=names[1:])
        self.assertEqual((first, code), (b'R\n', 0))
        self.assertEqual(rest.decode().splitlines(), ['G %s %s' % (names[0], zell_providers.LABELS[names[0]])])

    def test_a_display_from_before_scrollback_is_refused(self):
        first, rest, code, header, rows = self.run_helper([('S 9 40 20', 0.2)], old=True)
        self.assertEqual(code, 1)
        self.assertEqual(first, b'E The panes need the Neuronte display from 0.2.4, with scrollback and the mouse.\n')
        self.assertEqual(header[0], b'\0\0\0\0')
        self.assertEqual(rows, [])

    def test_a_broken_display_says_so_without_terminal_text(self):
        first, rest, code, header, rows = self.run_helper([], broken=True)
        self.assertEqual(code, 1)
        self.assertEqual(first, b'E The panes stopped. Check the Neuronte display library and weights.\n')
        self.assertEqual(header[0], b'\0\0\0\0')


if __name__ == '__main__':
    unittest.main()
