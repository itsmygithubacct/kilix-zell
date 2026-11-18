#!/usr/bin/env python3
"""Tests for tools/zell_attach.py: a Kilix pane shown and used from another
terminal. Stand-ins replace kilix_sdk, the broker and the terminal parser;
with ZELL_PANES_REAL_DISPLAY=1 the Neuronte display's parser is used too.
"""
import os
from pathlib import Path
import pty
import re
import select
import subprocess
import sys
import tempfile
import textwrap
import time
import types
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import zell_attach as attach  # noqa: E402

FG, BG = 0xDCE2EB, 0x13171E


class FakeTerminal:
    """The parts of neuronte_display.Terminal the viewer uses. `screen` is
    the rows shown (strings); `history` rows above them when scrolled."""

    def __init__(self, cols, rows):
        self.cols, self.rows = cols, rows
        self.screen = [''] * rows
        self.cursor = (0, 0)
        self.modes, self.fed, self.back, self.kept_above = {25}, [], 0, 0
        self.alternate = False
        self.mice = []
        self.named = ''

    def title(self):
        return self.named

    def feed(self, data):
        self.fed.append(data)
        return b'\x1b[?62c'  # a reply the viewer must not send

    def resize(self, cols, rows, cw=8, ch=16):
        self.cols, self.rows = cols, rows

    def snapshot(self):
        cells = []
        for row in range(self.rows):
            text = (self.screen[row] if row < len(self.screen) else '').ljust(self.cols)[:self.cols]
            cells += [(ord(ch), FG, BG, 0) for ch in text]
        return cells, self.cursor

    def rows_text(self):
        return [list((self.screen[row] if row < len(self.screen) else '').ljust(self.cols)[:self.cols])
                for row in range(self.rows)]

    def scrollbar(self):
        kept = self.rows + self.kept_above
        return kept, kept - self.rows - self.back, self.rows

    def scroll(self, rows):
        self.back = min(max(0, self.back - rows), self.kept_above)

    def scroll_bottom(self):
        self.back = 0

    def mode(self, number, ansi=False):
        if number > 9000:
            raise ValueError('unknown')
        return number in self.modes

    def alternate_screen(self):
        return self.alternate

    def mouse_tracking(self):
        return bool(self.modes & {1000, 1002, 1003})

    def mouse(self, action, button, mods, x, y):
        self.mice.append((action, button, mods, x, y))
        return f'<{action};{button};{mods};{x};{y}>'.encode()

    def close(self):
        pass


FAKE_DISPLAY = types.SimpleNamespace(Terminal=FakeTerminal)


def kilix_pane(**fields):
    base = dict(id=7, tab_id=2, os_window_id=1, title='codex', cwd=str(Path.home() / 'proj'), pid=1,
                cmdline=(), process='codex', is_focused=False, is_self=False, env={},
                broker_session='abc123', columns=20, lines=6)
    base.update(fields)
    return types.SimpleNamespace(**base)


class FakeKilix:
    def __init__(self, output=b'', panes=()):
        self.output, self.sent, self.sizes, self.entries = output, [], [], list(panes)

    def observe(self, session):
        # What the broker would stream, then the pane stays open.
        script = 'cat "$1"; exec sleep 30'
        path = tempfile.NamedTemporaryFile(delete=False)
        path.write(self.output)
        path.close()
        self.path = path.name
        return subprocess.Popen(['/bin/sh', '-c', script, 'observe', path.name],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

    def size(self, session):
        return self.sizes.pop(0) if self.sizes else None

    def send(self, pane_id, data):
        self.sent.append((pane_id, data))

    def listing(self):
        return self.entries


def wait_for(predicate, seconds=5):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.01)
    return False


class PureTests(unittest.TestCase):
    def test_follow_keeps_the_cursor_in_view(self):
        self.assertEqual(attach.follow(0, 3, 10, 8), 0)
        self.assertEqual(attach.follow(0, 15, 10, 30), 6)
        self.assertEqual(attach.follow(6, 10, 10, 30), 6)
        self.assertEqual(attach.follow(6, 2, 10, 30), 2)
        self.assertEqual(attach.follow(50, 29, 10, 30), 20)
        self.assertEqual(attach.follow(-4, 0, 10, 30), 0)
        # One row past the view moves it by one.
        self.assertEqual(attach.follow(0, 10, 10, 30), 1)
        self.assertEqual(attach.follow(5, 15, 10, 30), 6)

    def test_escaped_bytes_are_read_back_as_themselves(self):
        self.assertEqual(attach.escaped(b'ls -l\r'), 'ls -l\\x0d')
        self.assertEqual(attach.escaped(b'\x1b[A\\n\x7f\x00'), '\\x1b[A\\\\n\\x7f\\x00')
        self.assertEqual(attach.escaped('é→'.encode()), 'é→')
        # Not an option of the kitten; not UTF-8 is left out.
        self.assertEqual(attach.escaped(b'-x'), '\\x2dx')
        self.assertEqual(attach.escaped(b'a\xffb'), 'ab')

    def test_split_input_keeps_order_and_holds_a_cut_report(self):
        events, held = attach.split_input(b'ab\x1b[<0;3;4Mc\x1b[<64;1;1M\x1b[<0;3;4m\x1b[A', b'')
        self.assertEqual(events, [('keys', b'ab'), ('pointer', (0, 3, 4, False)), ('keys', b'c'),
                                  ('pointer', (64, 1, 1, False)), ('pointer', (0, 3, 4, True)),
                                  ('keys', b'\x1b[A')])
        self.assertEqual(held, b'')
        events, held = attach.split_input(b'x\x1b[<35;1', b'')
        self.assertEqual((events, held), ([('keys', b'x')], b'\x1b[<35;1'))
        events, held = attach.split_input(b'0;2M', held)
        self.assertEqual((events, held), ([('pointer', (35, 10, 2, False))], b''))
        # A lone Esc at the end waits for what follows it.
        self.assertEqual(attach.split_input(b'q\x1b', b''), ([('keys', b'q')], b'\x1b'))

    def test_sgr(self):
        self.assertEqual(attach.sgr(0x010203, 0xa0b0c0, 0), '\x1b[0;38;2;1;2;3;48;2;160;176;192m')
        self.assertEqual(attach.sgr(0, 0, 1 | 4 | 16), '\x1b[0;1;4;7;38;2;0;0;0;48;2;0;0;0m')

    def test_wide_characters_and_controls_in_a_row(self):
        cells = [(0, FG, BG, 0)] * 4
        self.assertEqual(attach.row_text(cells, ['a', '日', '', 'b'], 0, 4), attach.sgr(FG, BG, 0) + 'a日b')
        # Half a wide character at the right edge is a blank.
        self.assertEqual(attach.row_text(cells[:2], ['a', '日', ''], 0, 2), attach.sgr(FG, BG, 0) + 'a ')
        self.assertEqual(attach.row_text(cells[:2], ['\x07', 'z'], 0, 2), attach.sgr(FG, BG, 0) + ' z')
        # Scrolled sideways into a wide character's second half: a blank
        # keeps the columns.
        self.assertEqual(attach.row_text(cells[:3], ['日', '', 'a', 'b'], 1, 3), attach.sgr(FG, BG, 0) + ' ab')

    def test_fit_counts_cells_not_characters(self):
        self.assertEqual(attach.fit('a日b', 3), 'a日')
        self.assertEqual(attach.fit('日日', 3), '日 ')
        self.assertEqual(attach.fit('e\u0301x', 3), 'e\u0301x ')
        self.assertEqual(attach.fit('a\x07', 3), 'a  ')


class KilixTests(unittest.TestCase):
    def kilix(self, panes_module, own='60'):
        return attach.Kilix(panes_module, '/bin/true', '/run/user/1/kilix-pty-broker', own)

    def test_the_list_leaves_out_this_window_and_panes_without_a_broker(self):
        tab = types.SimpleNamespace(title='Zell', panes=(kilix_pane(id=60), kilix_pane(id=7), kilix_pane(id=3, is_self=True),
                                                          kilix_pane(id=8, broker_session=None),
                                                          kilix_pane(id=9, broker_session='../x')))
        other = types.SimpleNamespace(title='Work', panes=(kilix_pane(id=11, broker_session='def'),))
        workspace = types.SimpleNamespace(tabs=lambda: iter((tab, other)))
        module = types.SimpleNamespace(snapshot=lambda: workspace)
        self.assertEqual([(title, pane.id) for title, pane in self.kilix(module).listing()], [('Zell', 7), ('Work', 11)])

    def test_bytes_go_by_send_bytes_where_kilix_has_it(self):
        sent = []
        module = types.SimpleNamespace(send_bytes=lambda target, data: sent.append(('bytes', target, data)),
                                       send=lambda target, text: sent.append(('text', target, text)))
        self.kilix(module).send(7, b'\x1b[A')
        del module.send_bytes
        self.kilix(module).send(7, b'\x1b[A')
        self.kilix(module).send(7, b'\xff')
        self.assertEqual(sent, [('bytes', 'pane:7', b'\x1b[A'), ('text', 'pane:7', '\\x1b[A')])

    def test_a_bad_session_is_not_observed(self):
        with self.assertRaises(attach.AttachError):
            self.kilix(None).observe('../../etc')

    def test_load_says_what_is_missing(self):
        with self.assertRaisesRegex(attach.AttachError, 'KITTY_LISTEN_ON'):
            attach.Kilix.load({})
        with self.assertRaisesRegex(attach.AttachError, 'broker was not found'):
            attach.Kilix.load({'KITTY_LISTEN_ON': 'unix:@x', 'KITTY_PTY_BROKER_EXECUTABLE': '/nonexistent'})
        with self.assertRaisesRegex(attach.AttachError, 'runtime'):
            attach.Kilix.load({'KITTY_LISTEN_ON': 'unix:@x', 'KITTY_PTY_BROKER_EXECUTABLE': '/bin/true',
                               'KITTY_PTY_BROKER_RUNTIME': 'relative'})


class SenderTests(unittest.TestCase):
    def test_in_order_joined_and_failures_said(self):
        got = []

        class Slow:
            def send(self, pane_id, data):
                got.append(data)
                if data == b'bad':
                    raise RuntimeError('send to pane 7: no matching window\nmore')
                time.sleep(0.05)
        sender = attach.Sender(Slow(), 7)
        for piece in (b'a', b'b', b'c', b'd'):
            sender.put(piece)
        self.assertTrue(wait_for(lambda: b''.join(got) == b'abcd'))
        self.assertLess(len(got), 4)
        sender.put(b'bad')
        self.assertTrue(wait_for(lambda: sender.error == 'send to pane 7: no matching window'))
        sender.put(b'e')
        self.assertTrue(wait_for(lambda: sender.error is None))
        sender.stop()
        sender.join(2)
        self.assertFalse(sender.is_alive())


class ViewTests(unittest.TestCase):
    def setUp(self):
        self.now = 100.0
        self.kilix = FakeKilix(output=b'hello')
        self.view = attach.View(FAKE_DISPLAY, self.kilix, kilix_pane(), clock=lambda: self.now)
        self.addCleanup(self.view.close)
        self.addCleanup(lambda: os.unlink(self.kilix.path))
        self.term = self.view.terminal

    def test_the_stream_is_fed_and_replies_are_not_sent(self):
        self.assertTrue(wait_for(lambda: select.select([self.view], [], [], 0)[0]))
        self.assertTrue(self.view.read())
        self.assertEqual(self.term.fed, [b'hello'])
        self.assertTrue(wait_for(lambda: not self.view.sender.waiting.qsize()))
        self.assertEqual(self.kilix.sent, [])
        self.assertEqual((self.term.cols, self.term.rows), (20, 6))

    def test_the_pane_ending_ends_the_view(self):
        self.view.process.kill()
        self.view.process.wait()
        while not self.view.ended:
            select.select([self.view], [], [], 1)
            self.view.read()
        self.assertTrue(self.view.ended)

    def test_its_size_follows_the_pane(self):
        self.kilix.sizes = [(30, 8), (30, 8)]
        self.assertFalse(self.view.check_size())
        self.now += attach.SIZE_SECONDS
        self.assertTrue(self.view.check_size())
        self.assertEqual((self.term.cols, self.term.rows), (30, 8))
        self.now += attach.SIZE_SECONDS
        self.assertFalse(self.view.check_size())

    def test_it_is_drawn_to_fit_following_the_cursor(self):
        self.term.cols = 60
        self.term.screen = ['row0 abcdefghijklmno', 'row1', 'row2', 'row3', 'row4', 'row5 end']
        self.term.cursor = (8, 5)
        text, drawn = self.view.paint(50, 4, [])
        # Three rows and the status line; the cursor's row shown.
        self.assertEqual(len(drawn), 4)
        self.assertEqual(self.view.oy, 3)
        self.assertIn('row3', drawn[0])
        self.assertIn('row5 end', drawn[2])
        self.assertTrue(drawn[3].startswith('\x1b[0;7m Kilix pane 7 / Ctrl-] leaves / codex / 60x6'))
        self.assertTrue(text.endswith('\x1b[3;9H\x1b[?25h'))
        # The window is called after the pane, once.
        self.assertTrue(text.startswith('\x1b]2;Kilix 7: codex\x07'))
        # Only the rows that changed are drawn again.
        self.term.screen[4] = 'ROW4'
        text, drawn = self.view.paint(50, 4, drawn)
        self.assertNotIn('\x1b]2;', text)
        self.assertNotIn('\x1b[1;1H', text)
        self.assertIn('\x1b[2;1H', text)
        self.assertNotIn('\x1b[3;1H', text)
        # A cursor past the width wraps its row; the cursor is on the second
        # piece, which is followed.
        self.term.cursor = (55, 5)
        text, drawn = self.view.paint(50, 4, drawn)
        self.assertEqual(self.view.oy, 4)
        self.assertEqual(self.view.lines, [(4, 0, 50), (5, 0, 50), (5, 50, 10)])
        self.assertIn('row5 end', drawn[1])
        self.assertNotIn('from column', drawn[3])
        self.assertTrue(text.endswith('\x1b[3;6H\x1b[?25h'))
        # The title its program sets names it.
        self.term.named = 'vim notes.txt'
        text, drawn = self.view.paint(50, 4, drawn)
        self.assertTrue(text.startswith('\x1b]2;Kilix 7: vim notes.txt\x07'))
        # A hidden cursor is hidden here.
        self.term.modes.discard(25)
        text, drawn = self.view.paint(50, 4, drawn)
        self.assertTrue(text.endswith('\x1b[?25l'))

    def test_long_rows_wrap_at_spaces_and_whole_characters(self):
        text = list('hello world foo'.ljust(20))
        self.assertEqual(attach.wrap_row(text, 8, 20), [(0, 6), (6, 6), (12, 8)])
        self.assertEqual(attach.wrap_row(list('abcdefghij'), 4, 10), [(0, 4), (4, 4), (8, 2)])
        # A wide character is never cut: its halves go together.
        self.assertEqual(attach.wrap_row(['a', '\u65e5', '', 'b'], 2, 4), [(0, 1), (1, 2), (3, 1)])
        # The blank end is not wrapped, but up to the cursor it is.
        self.assertEqual(attach.wrap_row(list('ab'.ljust(10)), 6, 10), [(0, 6)])
        self.assertEqual(attach.wrap_row([' '] * 10, 4, 10), [(0, 4)])
        self.assertEqual(attach.wrap_row([' '] * 10, 4, 10, 9), [(0, 4), (4, 4), (8, 2)])
        # Narrower than here: one piece, the whole row.
        self.assertEqual(attach.wrap_row(list('a b c'), 30, 5), [(0, 5)])
        # Wrapped, every row is drawn with every character, none cut.
        self.term.cols = 30
        self.term.screen = ['the quick brown fox jumps over', 'x']
        self.term.cursor = (1, 1)
        _text, drawn = self.view.paint(12, 8, [])
        self.assertEqual([re.sub('\x1b\\[[0-9;]*[A-Za-z]', '', line).rstrip() for line in drawn[:5]],
                         ['the quick', 'brown fox', 'jumps over', 'x', ''])

    def test_a_full_screen_program_is_not_wrapped(self):
        # On the alternate screen rows keep the program's width: a window of
        # it from the cursor, as before wrapping, every row its own line.
        self.term.alternate, self.term.cols = True, 60
        self.term.screen = ['row0 ' + 'w' * 50, 'row1', 'row2', 'row3', 'row4', '-- INSERT -- notes.txt']
        self.term.cursor = (55, 0)
        _text, drawn = self.view.paint(50, 8, [])
        self.assertEqual(self.view.ox, 6)
        self.assertEqual(self.view.lines, [(row, 6, 50) for row in range(6)])
        self.assertIn('from column 7', drawn[-1])
        self.assertIn('notes.txt', drawn[5])
        self.view.pointer(0, 3, 6, False, 50, 8)
        self.term.modes.add(1000)
        self.view.pointer(0, 3, 6, False, 50, 8)
        self.assertEqual(self.term.mice, [(0, 1, 0, 8 * 8, 5 * 16)])
        # Back on the main screen, rows wrap again from column 1.
        self.term.alternate = False
        _text, drawn = self.view.paint(50, 8, [])
        self.assertEqual((self.view.ox, self.view.lines[:2]), (0, [(0, 0, 5), (0, 5, 50)]))

    def test_the_wheel_moves_a_line_at_a_time_through_wrapped_lines(self):
        # Six rows that wrap to two lines each, shown six lines at a time,
        # nothing kept above: the wheel goes up through the wrapped lines,
        # to the first, and back down to the cursor's, then follows it.
        self.term.cols = 60
        self.term.screen = [f'{row}' + 'x' * 50 for row in range(6)]
        self.term.cursor = (0, 5)
        self.view.paint(30, 7, [])
        self.assertEqual((self.view.oy, self.view.top), (5, None))
        tops = []
        for low in (64, 64, 64, 65, 65, 65):
            self.view.pointer(low, 3, 3, False, 30, 7)
            self.view.paint(30, 7, [])
            tops.append((self.view.oy, self.view.top))
        self.assertEqual(tops, [(2, (1, 0)), (0, (0, 0)), (0, (0, 0)), (3, (1, 30)), (6, None), (6, None)])
        self.assertEqual(self.term.back, 0)
        # Typing goes back to the cursor's lines.
        self.view.pointer(64, 3, 3, False, 30, 7)
        self.view.send(b'x')
        self.view.paint(30, 7, [])
        self.assertEqual((self.view.oy, self.view.top), (6, None))
        # With rows kept above, the parser is moved only near its first row.
        self.term.kept_above = 40
        self.view.pointer(64, 3, 3, False, 30, 7)
        self.assertEqual(self.term.back, 0)
        self.view.paint(30, 7, [])
        self.view.pointer(64, 3, 3, False, 30, 7)
        self.assertEqual(self.term.back, 3)

    def test_a_held_view_says_so_and_a_full_screen_program_drops_it(self):
        self.term.cols = 60
        self.term.screen = [f'{row}' + 'x' * 50 for row in range(6)]
        self.term.cursor = (0, 5)
        _text, drawn = self.view.paint(30, 7, [])
        self.assertNotIn('held', drawn[-1])
        for _ in range(3):
            self.view.pointer(64, 3, 3, False, 30, 7)
            _text, drawn = self.view.paint(30, 7, [])
        self.assertEqual((self.view.top, self.term.back), ((0, 0), 0))
        self.assertIn('held', drawn[-1])
        # The program goes to its alternate screen, its cursor far right:
        # the view follows it, and says nothing is held.
        self.term.alternate = True
        self.term.cursor = (55, 4)
        _text, drawn = self.view.paint(30, 7, [])
        self.assertEqual((self.view.top, self.view.ox), (None, 26))
        self.assertNotIn('held', drawn[-1])
        self.term.cursor = (0, 0)
        self.view.paint(30, 7, [])
        self.assertEqual((self.view.oy, self.view.ox), (0, 0))
        # Back on the main screen, the cursor's lines.
        self.term.alternate, self.term.cursor = False, (0, 5)
        self.view.paint(30, 7, [])
        self.assertEqual((self.view.oy, self.view.top), (5, None))

    def test_a_top_line_scrolled_above_the_rows_counts_a_line_a_row(self):
        # The parser moved down past the kept top line, two rows: those two
        # rows are at least a line each, so a step of three down lands on
        # the third line here, not three lines past the first.
        self.assertEqual(self.term.rows, 6)
        self.term.kept_above, self.term.back = 40, 10
        _kept, first, _shown = self.term.scrollbar()
        self.view.top, self.view.move = (first - 2, 0), 3
        self.view.paint(30, 5, [])
        self.assertEqual((self.view.oy, self.view.top), (1, (first + 1, 0)))

    def test_a_screen_that_fits_is_never_held(self):
        self.term.screen = ['one', 'two', '$ ']
        self.term.cursor = (2, 2)
        self.view.paint(30, 11, [])
        self.view.pointer(64, 3, 3, False, 30, 11)
        _text, drawn = self.view.paint(30, 11, [])
        self.assertIsNone(self.view.top)
        self.assertNotIn('held', drawn[-1])

    def test_scrolled_back_it_shows_from_the_top_and_says_so(self):
        self.term.kept_above, self.term.back = 40, 12
        _text, drawn = self.view.paint(30, 8, [])
        self.assertEqual(self.view.oy, 0)  # six rows fit in seven
        self.assertIn('12 lines back', drawn[-1])
        # Typing goes back to the live rows.
        self.view.send(b'x')
        self.assertEqual(self.term.back, 0)
        self.assertTrue(wait_for(lambda: self.kilix.sent == [(7, b'x')]))

    def test_the_mouse_goes_to_a_program_that_asked_else_the_wheel_scrolls(self):
        # Row 0 is wider than here: it is drawn as two rows.
        self.term.kept_above, self.term.cols = 40, 60
        self.term.screen[0] = 'x' * 45
        self.view.paint(30, 8, [])
        self.assertEqual(self.view.lines[:3], [(0, 0, 30), (0, 30, 30), (1, 0, 30)])
        # No tracking: the wheel scrolls; a click does nothing.
        self.view.pointer(64, 3, 3, False, 30, 8)
        self.assertEqual(self.term.back, 3)
        self.view.pointer(65, 3, 3, False, 30, 8)
        self.assertEqual(self.term.back, 0)
        self.view.pointer(0, 3, 3, False, 30, 8)
        self.assertEqual(self.term.mice, [])
        # Tracking: encoded as it asked, at the pane's cell through the
        # wrapping.
        self.term.modes.add(1000)
        self.view.pointer(0 | 16 | 8, 3, 3, False, 30, 8)
        self.view.pointer(0, 3, 3, True, 30, 8)
        # Shift keeps a click here too.
        self.view.pointer(0 | 4, 3, 3, False, 30, 8)
        self.view.pointer(32 + 2, 4, 3, False, 30, 8)
        self.view.pointer(65, 1, 1, False, 30, 8)
        self.view.pointer(0, 3, 2, False, 30, 8)
        self.assertEqual(self.term.mice, [(0, 1, 6, 16, 16), (1, 1, 0, 16, 16), (2, 2, 0, 24, 16), (0, 5, 0, 0, 0),
                                          (0, 1, 0, 256, 0)])
        # Shift keeps the wheel here.
        self.view.pointer(64 | 4, 3, 3, False, 30, 8)
        self.assertEqual(self.term.back, 3)
        self.term.back = 0
        # The status line is not the pane's, even over a pane taller than
        # here; past a piece's end is its last cell, so a release made there
        # still reaches the program.
        self.term.rows = 40
        self.view.pointer(0, 3, 8, False, 30, 8)
        self.assertEqual(len(self.term.mice), 5)
        self.term.screen[1] = 'aaaa bbbb'
        self.term.cols = 12
        self.term.screen[0] = 'aaaa bbbb cc'
        self.view.paint(8, 8, [])
        self.assertEqual(self.view.lines[:2], [(0, 0, 5), (0, 5, 7)])
        self.view.pointer(0, 2, 1, False, 8, 8)
        self.view.pointer(0, 7, 1, True, 8, 8)
        self.assertEqual(self.term.mice[-2:], [(0, 1, 0, 8, 0), (1, 1, 0, 32, 0)])
        self.assertTrue(wait_for(lambda: len(b''.join(data for _id, data in self.kilix.sent)) > 0))

    def test_the_wheel_on_an_alternate_screen_is_arrows_when_asked(self):
        self.term.alternate, self.term.kept_above = True, 40
        self.view.pointer(64, 3, 3, False, 30, 8)
        self.assertEqual(self.term.back, 0)
        self.term.modes.add(1007)
        self.view.pointer(64, 3, 3, False, 30, 8)
        self.term.modes.add(1)
        self.view.pointer(65, 3, 3, False, 30, 8)
        self.assertTrue(wait_for(lambda: b''.join(d for _i, d in self.kilix.sent) == b'\x1b[A' * 3 + b'\x1bOB' * 3))

    def test_a_narrow_status_line_keeps_what_matters(self):
        self.term.kept_above, self.term.back = 40, 12
        self.view.sender.error = 'send to pane 7: no matching window'
        _text, drawn = self.view.paint(72, 8, [])
        self.assertTrue(drawn[-1].startswith('\x1b[0;7m Kilix pane 7 / not sent: send to pane 7: no matching window / 12 lin'))

    def test_this_terminal_asks_for_what_the_program_asked_for(self):
        self.assertEqual(self.view.modes(), {1: False, 2004: False, 1003: False, 1002: True})
        self.term.modes |= {1, 2004, 1003}
        self.assertEqual(self.view.modes(), {1: True, 2004: True, 1003: True, 1002: False})


class ShowTests(unittest.TestCase):
    """show() itself, its input from a pipe."""

    def test_a_lone_esc_goes_to_the_pane_and_the_view_goes_on(self):
        kilix = FakeKilix(output=b'hello')
        read, write = os.pipe()
        out = os.open(os.devnull, os.O_WRONLY)
        self.addCleanup(os.close, out)
        self.addCleanup(os.close, read)
        self.addCleanup(os.close, write)
        screen = attach.Screen(read, out)
        os.write(write, b'\x1b')

        def later():
            time.sleep(0.4)
            os.write(write, b'q\x1d')
        import threading
        threading.Thread(target=later, daemon=True).start()
        note = attach.show(FAKE_DISPLAY, kilix, ('Work', kilix_pane()), screen)
        self.addCleanup(lambda: os.unlink(kilix.path))
        self.assertEqual(note, '')
        self.assertTrue(wait_for(lambda: b''.join(d for _i, d in kilix.sent) == b'\x1bq'))

    def test_a_wide_title_never_wraps_the_status_line(self):
        import unicodedata
        kilix = FakeKilix()
        view = attach.View(FAKE_DISPLAY, kilix, kilix_pane(title='日本' * 30), clock=time.monotonic)
        self.addCleanup(view.close)
        self.addCleanup(lambda: os.unlink(kilix.path))
        _text, drawn = view.paint(41, 6, [])
        status = drawn[-1][len('\x1b[0;7m'):-len('\x1b[0m')]
        cells = sum(2 if unicodedata.east_asian_width(ch) in 'WF' else 1 for ch in status)
        self.assertEqual(cells, 41)


class RealDisplayTests(unittest.TestCase):
    """The viewer over the Neuronte display's real parser."""

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

    def test_a_pane_wider_than_here_is_drawn_from_its_cursor(self):
        kilix = FakeKilix(output=b'\x1b[31mred\x1b[0m ' + b'x' * 50 + b'\r\nsecond line\x1b[?1000h\x1b[?2004h')
        view = attach.View(self.display, kilix, kilix_pane(columns=60, lines=10), clock=time.monotonic)
        self.addCleanup(os.unlink, kilix.path)
        self.addCleanup(view.close)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not view.terminal.mode(2004):
            if select.select([view], [], [], 0.2)[0]:
                view.read()
        text, drawn = view.paint(30, 6, [])
        self.assertIn('second line', text)
        # Wider than here, the first row wraps, after its space: none of it
        # is lost.
        self.assertEqual(view.lines[:4], [(0, 0, 4), (0, 4, 30), (0, 34, 26), (1, 0, 30)])
        self.assertIn('38;2;', text)
        self.assertIn('Kilix pane 7 / Ctrl-] leaves', drawn[-1])
        self.assertIn('Kilix pane 7 / Ctrl-] leaves / codex / 60x10', view.paint(70, 6, [])[1][-1])
        self.assertEqual(view.modes(), {1: False, 2004: True, 1003: False, 1002: True})
        view.terminal.feed(b'\r\n' * 20)
        view.paint(30, 6, [])
        view.pointer(64, 2, 2, False, 30, 6)
        self.assertTrue(wait_for(lambda: kilix.sent))
        # 1000 without 1006: the reports it asked for are X10's.
        self.assertEqual(kilix.sent[0][1], b'\x1b[M' + bytes([32 + 64, 32 + 2, 32 + 7]))


class RealScrollTests(RealDisplayTests):
    def view_of(self, columns, lines):
        kilix = FakeKilix(output=b'')
        view = attach.View(self.display, kilix, kilix_pane(columns=columns, lines=lines), clock=time.monotonic)
        self.addCleanup(os.unlink, kilix.path)
        self.addCleanup(view.close)
        return view

    def shown(self, view, cols, rows):
        text = re.sub('\x1b\\[[0-9;?]*[A-Za-z]|\x1b\\][^\x07]*\x07', '', view.paint(cols, rows, [])[0])
        return set(int(n) for n in re.findall(r'L(\d{3})', text))

    def wheel_through(self, view):
        """The rows shown going up to the oldest line kept and back down to
        the live view, a wheel step at a time."""
        seen = self.shown(view, 30, 11)
        for _ in range(3000):
            view.pointer(64, 3, 3, False, 30, 11)
            seen |= self.shown(view, 30, 11)
            if view.terminal.scrollbar()[1] == 0 and view.oy == 0:
                break
        else:
            self.fail('the wheel never reached the oldest line')
        for _ in range(3000):
            view.pointer(65, 3, 3, False, 30, 11)
            seen |= self.shown(view, 30, 11)
            if view.top is None:
                return seen
        self.fail('the wheel never came back to the live view')

    def test_the_wheel_reaches_every_wrapped_row(self):
        view = self.view_of(60, 10)
        view.terminal.feed(b''.join(b'L%03d ' % n + b'x' * 51 + b'\r\n' for n in range(200)))
        seen = self.wheel_through(view)
        # Every row still kept is reached.
        kept = view.terminal.scrollbar()[0]
        self.assertGreater(kept, 100)
        self.assertEqual(set(range(max(0, 201 - kept), 200)) - seen, set())

    def test_the_wheel_reaches_every_row_wrapped_past_the_view(self):
        # Rows four lines long, two and a half to a view; and rows longer
        # than the view itself.
        for columns, width in ((120, 111), (400, 395)):
            view = self.view_of(columns, 10)
            view.terminal.feed(b''.join(b'L%03d ' % n + b'x' * min(width, columns - 5) + b'\r\n' for n in range(120)))
            seen = self.wheel_through(view)
            kept = view.terminal.scrollbar()[0]
            self.assertEqual(set(range(max(0, 121 - kept), 120)) - seen, set(), columns)
            self.assertIsNone(view.top)

    def test_a_full_screen_program_keeps_every_row(self):
        view = self.view_of(60, 10)
        screen = b'\x1b[?1049h\x1b[H' + b''.join(b'%d word word word word word word word word word word\r\n' % n for n in range(9))
        view.terminal.feed(screen + b'\x1b[7m-- INSERT -- notes.txt\x1b[0m\x1b[1;1H')
        text = view.paint(30, 11, [])[0]
        self.assertIn('INSERT', text)
        self.assertEqual(len(view.lines), 10)


class EndToEndTests(unittest.TestCase):
    """zell_attach.py itself, in a PTY: the list, a pane shown, typing,
    Ctrl-] back to the list, q out. kilix_sdk and the broker are stand-ins
    that write what they were asked to a log."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        root = Path(self.dir.name)
        self.log = root / 'log'
        sdk = root / 'kilix' / 'config' / 'kilix_sdk'
        sdk.mkdir(parents=True)
        (sdk / '__init__.py').write_text('')
        (sdk / 'panes.py').write_text(textwrap.dedent(f'''
            import types
            LOG = {str(self.log)!r}
            def snapshot():
                pane = types.SimpleNamespace(id=7, title='agent', cwd='/w', process='codex', broker_session='s7',
                                             columns=40, lines=6)
                zell = types.SimpleNamespace(id=60, title='Zell', cwd='/', process='zell', broker_session='s60',
                                             columns=80, lines=24)
                tab = types.SimpleNamespace(title='Work', panes=(zell, pane))
                return types.SimpleNamespace(tabs=lambda: iter((tab,)))
            def send_bytes(target, data):
                with open(LOG, 'ab') as log:
                    log.write(b'send ' + target.encode() + b' ' + data.hex().encode() + b'\\n')
        '''))
        broker = root / 'broker'
        broker.write_text(textwrap.dedent(f'''\
            #!/bin/sh
            echo "broker $*" >> {self.log}
            case "$3" in
            observe) printf 'pane says hi'; exec sleep 60 ;;
            esac
            case "$5" in
            status) echo '{{"columns": 40, "rows": 6}}' ;;
            esac
        '''))
        broker.chmod(0o755)
        self.env = dict(os.environ, KITTY_LISTEN_ON='unix:@test', KITTY_WINDOW_ID='60',
                        KITTY_PTY_BROKER_EXECUTABLE=str(broker), KITTY_PTY_BROKER_RUNTIME=str(root / 'run'),
                        KILIX_HOME=str(root / 'kilix'), PYTHONDONTWRITEBYTECODE='1')
        if 'KILIX_ZELL_NEURONTE_DISPLAY' not in self.env and os.environ.get('ZELL_PANES_REAL_DISPLAY') != '1':
            try:
                import zell_terminal
                zell_terminal.load_display()
            except Exception as error:
                self.skipTest(f'no Neuronte display here: {error}')

    def read_until(self, fd, needle, seconds=10):
        seen = b''
        deadline = time.monotonic() + seconds
        while needle not in seen and time.monotonic() < deadline:
            if select.select([fd], [], [], 0.2)[0]:
                try:
                    seen += os.read(fd, 65536)
                except OSError:
                    break
        return seen

    def test_choose_show_type_and_leave(self):
        pid, fd = pty.fork()
        if pid == 0:
            os.execve(sys.executable, [sys.executable, str(ROOT / 'tools' / 'zell_attach.py')], self.env)
        try:
            seen = self.read_until(fd, b'codex')
            self.assertIn(b'Kilix panes', seen)
            self.assertNotIn(b'zell', seen)
            # This terminal's title is kept, then the window is called the
            # list, then the pane shown.
            self.assertLess(seen.index(b'\x1b[22;0t'), seen.index(b'\x1b]2;Kilix panes\x07'))
            os.write(fd, b'\r')
            seen = self.read_until(fd, b'pane says hi')
            self.assertIn(b'pane says hi', seen)
            self.assertIn(b'Kilix pane 7', seen)
            self.assertIn(b'\x1b]2;Kilix 7: agent\x07', seen)
            os.write(fd, b'ls\r')
            self.assertTrue(wait_for(lambda: self.log.exists() and b'send pane:7 6c730d' in self.log.read_bytes()))
            os.write(fd, b'\x1d')
            seen = self.read_until(fd, b'Up and Down choose')
            self.assertIn(b'Up and Down choose', seen)
            self.assertIn(b'\x1b]2;Kilix panes\x07', seen)
            os.write(fd, b'\x1b')
            # Leaving puts this terminal's title back.
            self.assertIn(b'\x1b[23;0t', self.read_until(fd, b'\x1b[23;0t'))
            _, status = os.waitpid(pid, 0)
            pid = None
            self.assertEqual(os.waitstatus_to_exitcode(status), 0)
            log = self.log.read_text()
            self.assertIn('broker --runtime-dir', log)
            self.assertIn('observe s7', log)
        finally:
            if pid:
                os.kill(pid, 9)
                os.waitpid(pid, 0)
            os.close(fd)


if __name__ == '__main__':
    unittest.main()
