"""Pane rooms whose panes are embedded Kilix windows (kilix-embed-pane).

The protocol tests use a stand-in engine and map; RealEngineTests runs a
private Kilix engine with embedded windows (ZELL_TEST_KITTY=<built kitty
checkout>) on its own X display and drives the rooms through it.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import zell_panes as panes  # noqa: E402

KEP = panes.load_embed_module()
FG, BG = 0xD0D0D0, 0x101418
DISPLAY = SimpleNamespace(FG=FG, BG=BG)
needs_module = unittest.skipIf(KEP is None, 'kilix-embed-pane not found (KILIX_ZELL_EMBED_PANE)')


def make_frame(cols, rows, text=(), cursor=(0, 0), history=0, offset=0, modes=0, tracking=0, pid=4242,
               title='', frames=1):
    cells = []
    for y in range(rows):
        line = text[y] if y < len(text) else ''
        for x in range(cols):
            ch = ord(line[x]) if x < len(line) else 0
            cells.append(KEP.Cell(ch, 0, FG, BG, KEP.COLOR_SAME_AS_FG, 0, 0, 0, 0))
    return KEP.Frame(cols, rows, tuple(cells), bytes([1] * rows), (0,), (*cursor, 0, True, True, True, 0),
                     modes, tracking, 0, FG, BG, history, offset, 0, frames, title, pid)


class Reader:
    """MapReader over frames a test hands it."""
    waiting = {}

    def __init__(self, path):
        self.path = path

    def read(self):
        return Reader.waiting.pop(self.path, None)

    def close(self):
        pass


class Engine:
    def __init__(self, root):
        self.root, self.next, self.calls, self.made = root, 1, [], {}
        self.listen_on, self.fail_launch, self.fail_resize = 'unix:@zell-test', False, False

    def launch(self, argv, cols, rows, cell_size=None, env=(), cwd=None, title=None):
        if self.fail_launch:
            raise KEP.EmbedError('no engine')
        window = KEP.EmbeddedWindow(self.next, f'{self.next:032x}', os.path.join(self.root, f'{self.next}.kepm'),
                                    cols, rows, 8, 16)
        Path(window.map).touch()
        self.next += 1
        self.made[window.id] = SimpleNamespace(argv=list(argv), env=list(env), cwd=cwd, cell_size=cell_size)
        self.calls.append(('launch', window.id, cols, rows))
        return window

    def resize(self, window, cols, rows, cell_size=None):
        if self.fail_resize:
            raise KEP.EmbedError('busy')
        self.calls.append(('resize', window.id, cols, rows))
        window.cols, window.rows = cols, rows

    def close(self, window):
        self.calls.append(('close', window.id))
        if os.path.exists(window.map):
            os.unlink(window.map)


class Connection:
    def __init__(self):
        self.sent, self.selection_text = [], ''

    def send(self, window, event, mods=0):
        self.sent.append((window.id, list(event), mods))

    def send_bytes(self, window, data):
        self.sent.append((window.id, 'bytes', bytes(data)))

    def selection(self, window):
        self.sent.append((window.id, 'selection?'))
        return self.selection_text

    def close(self):
        pass


@needs_module
class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.engine, self.conn = Engine(self.dir.name), Connection()
        kep = SimpleNamespace(**{k: getattr(KEP, k) for k in dir(KEP) if not k.startswith('_')})
        kep.MapReader, kep.InputConnection = Reader, lambda listen_on: self.conn
        self.backend = panes.EmbedBackend(kep, self.engine)
        self.said, self.hung, self.now = [], [], 100.0
        self.fronts = {}
        self.rooms = panes.EmbedRooms(
            DISPLAY, ['/bin/sh'], self.backend, viewer_env=['KILIX_HOME=/k', 'ZELL_KILIX_WINDOW_ID=3'],
            agents=[('codex', 'Codex', 'codex'), ('kilix', 'Kilix', None)], say=self.said.append,
            clock=lambda: self.now, hang_up=lambda group, sig: self.hung.append(group), viewer=True,
            front=lambda pane: self.fronts.get(pane.window.id, (pane.pid or None, 'sh')))
        Reader.waiting.clear()

    def tearDown(self):
        self.rooms.close()
        self.dir.cleanup()

    def show(self, room=9, cols=40, rows=20):
        self.rooms.line(f'S {room} {cols} {rows}')
        return self.rooms.rooms[room]

    def feed(self, pane, **kw):
        Reader.waiting[pane.window.map] = make_frame(pane.cols, pane.rows, **kw)
        self.rooms.poll(self.now)

    def events(self, window_id=None):
        return [e for e in self.conn.sent if window_id is None or e[0] == window_id]

    def test_a_room_is_kilix_windows_sized_to_its_columns(self):
        room = self.show(9, 40, 20)
        self.assertEqual([c for c in self.engine.calls if c[0] == 'launch'], [('launch', 1, 20, 20), ('launch', 2, 19, 20)])
        made = self.engine.made[1]
        self.assertEqual((made.argv, made.cell_size, made.cwd), (['/bin/sh'], (8, 16), str(Path.home())))
        self.assertEqual(len(self.show(10, 40, 20).current['panes']), 4)
        self.engine.calls.clear()
        self.show(9, 40, 20)
        self.assertEqual(self.engine.calls, [])
        self.show(9, 61, 22)
        self.assertEqual(sorted(self.engine.calls), [('resize', 1, 30, 22), ('resize', 2, 30, 22)])
        self.rooms.line('Z 1')
        self.assertIn(('resize', 2, 61, 22), self.engine.calls)
        self.assertIs(self.rooms.rooms[9], room)

    def test_a_resize_the_engine_refuses_keeps_the_pane(self):
        room = self.show()
        self.engine.fail_resize = True
        self.show(9, 61, 22)
        self.assertEqual([p.cols for p in room.panes()], [20, 19])
        self.assertTrue(all(p.fd is not None for p in room.panes()))

    def test_keys_go_to_the_focused_window(self):
        self.show()
        self.rooms.line('K6c730d')
        self.rooms.line('F 1')
        self.rooms.line('K71')
        self.assertEqual(self.events(), [(1, 'bytes', b'ls\r'), (2, 'bytes', b'q')])

    def test_the_grid_is_kilix_cells_with_the_focused_cursor(self):
        room = self.show(9, 40, 8)
        left, right = room.current['panes']
        self.feed(left, text=['$ hi'], cursor=(2, 0))
        self.feed(right, text=['right'], cursor=(0, 0))
        records = panes.embed_grid(DISPLAY, room)
        self.assertEqual(''.join(chr(r[0]) for r in records[:4]), '$ hi')
        self.assertEqual(records[2][3] & 16, 16)               # the focused pane's cursor
        self.assertEqual(records[20], (0x2502, panes.SEPARATOR, BG, 0))
        self.assertEqual(''.join(chr(r[0]) for r in records[21:26]), 'right')
        self.assertEqual(records[21][3] & 16, 0)               # not the other pane's
        # Scrolled back, the cursor is not drawn.
        self.feed(left, text=['$ hi'], cursor=(2, 0), history=50, offset=5)
        self.assertEqual(panes.embed_grid(DISPLAY, room)[2][3] & 16, 0)

    def test_pointer_events_are_the_engines_numbers(self):
        room = self.show()
        self.feed(room.current['panes'][0])
        # Left press with Shift at pixel (20, 40): cell (2, 2), right half.
        self.rooms.line('M 0 0 1 1 20 40')
        # Right release with Ctrl, motion with none held and Alt.
        self.rooms.line('M 0 1 2 2 9 0')
        self.rooms.line('M 0 2 0 4 3 0')
        # Middle, back; the wheel up and down; a wheel release is nothing.
        self.rooms.line('M 0 0 3 0 0 0')
        self.rooms.line('M 0 0 8 0 0 0')
        self.rooms.line('M 0 0 4 0 8 16')
        self.rooms.line('M 0 0 5 2 8 16')
        self.rooms.line('M 0 1 4 0 8 16')
        self.assertEqual([(e[1], e[2]) for e in self.events(1)], [
            (['mouse', '0', '0', '2', '2', '20', '40', '0'], 1),
            (['mouse', '1', '1', '1', '0', '9', '0', '1'], 4),
            (['mouse', '2', '-1', '0', '0', '3', '0', '1'], 2),
            (['mouse', '0', '2', '0', '0', '0', '0', '1'], 0),
            (['mouse', '0', '3', '0', '0', '0', '0', '1'], 0),
            (['scroll', '3', '1', '1', '8', '16'], 0),
            (['scroll', '-3', '1', '1', '8', '16'], 4),
        ])

    def test_a_left_release_copies_kilixs_selection(self):
        room = self.show()
        pane = room.current['panes'][0]
        self.feed(pane)
        self.conn.selection_text = 'two\nlines\n'
        self.rooms.line('M 0 1 1 0 0 0')
        # Asked after the release, on the same connection.
        self.assertEqual(self.events(1)[-2:], [(1, ['mouse', '1', '0', '0', '0', '0', '0', '1'], 0), (1, 'selection?')])
        self.assertEqual(self.said[-1], 'N 9 0 Copied 2 lines.')
        self.assertEqual(self.said[-2], 'Y .')
        # A program that has the mouse gets the release; nothing is copied...
        self.conn.sent.clear()
        self.feed(pane, tracking=1)
        self.rooms.line('M 0 1 1 0 0 0')
        self.assertNotIn((1, 'selection?'), self.conn.sent)
        # ...unless Shift is held. An empty selection says nothing.
        self.conn.selection_text = ''
        said = len(self.said)
        self.rooms.line('M 0 1 1 1 0 0')
        self.assertIn((1, 'selection?'), self.conn.sent)
        self.assertEqual(len(self.said), said)

    def test_page_keys_and_the_scrollbar_scroll_kilixs_history(self):
        room = self.show(9, 40, 20)
        pane = room.current['panes'][0]
        self.feed(pane, history=100, offset=10)
        for line in ('W 0 up', 'W 0 down', 'W 0 top', 'W 0 bottom', 'V 0 30', 'V 0 9999', 'V 0 0'):
            self.rooms.line(line)
        self.assertEqual([e[1] for e in self.events(1)], [
            ['scroll-to', '29'], ['scroll-to', '0'], ['scroll-to', '100'], ['scroll-to', '0'],
            ['scroll-to', '70'], ['scroll-to', '0'], ['scroll-to', '100']])
        # On the alternate screen the program gets Shift and the page keys.
        self.conn.sent.clear()
        self.feed(pane, history=100, modes=KEP.MODE_ALTERNATE_SCREEN)
        self.rooms.line('W 0 up')
        self.rooms.line('V 0 3')
        self.assertEqual(self.events(1), [(1, 'bytes', panes.SHIFTED_KEYS['up'])])

    def test_scrollback_and_mouse_wants_are_reported(self):
        room = self.show(9, 40, 20)
        self.feed(room.current['panes'][0], history=100, offset=10)
        self.assertIn('B 9 0 120 90 20 0', self.said)
        self.feed(room.current['panes'][0], history=100, tracking=3)
        self.assertIn('B 9 0 120 100 20 3', self.said)
        self.feed(room.current['panes'][0], history=100, modes=KEP.MODE_ALTERNATE_SCREEN, tracking=1)
        self.assertIn('B 9 0 20 0 20 1', self.said)

    def test_a_paste_is_one_kilix_paste(self):
        self.show()
        self.rooms.line('P 9 1 6869')
        self.rooms.line('P 9 1 0a7468c3a9')
        self.rooms.line('P 9 1 .')
        self.rooms.line('P 9 0 zz')
        self.rooms.line('P 9 0 .')
        self.assertEqual(self.events(), [(2, ['paste', 'hi\nthé'], 0)])

    def test_an_ended_window_is_opened_again(self):
        room = self.show()
        old = room.current['panes'][0]
        os.unlink(old.window.map)
        self.rooms.poll(time.monotonic())
        self.assertIsNone(old.fd)
        self.assertIs(room.current['panes'][0], old)       # not before RESPAWN_SECONDS
        self.rooms.poll(time.monotonic() + panes.RESPAWN_SECONDS)
        fresh = room.current['panes'][0]
        self.assertIsNot(fresh, old)
        self.assertEqual((fresh.cols, fresh.rows), (old.cols, old.rows))
        # When it cannot be opened, the ended one stays and is tried later.
        os.unlink(fresh.window.map)
        self.engine.fail_launch = True
        self.rooms.poll(time.monotonic() + 10)
        self.assertIs(room.current['panes'][0], fresh)

    def test_restart_hangs_up_what_runs_in_front(self):
        room = self.show()
        pane = room.current['panes'][0]
        self.feed(pane, pid=500)
        self.fronts[pane.window.id] = (777, 'vim')
        self.rooms.line('X 0')
        self.assertEqual(self.hung, [777])
        self.assertIn(('close', 1), self.engine.calls)
        self.assertIsNot(room.current['panes'][0], pane)

    def test_an_agent_is_typed_only_at_the_shells_prompt(self):
        room = self.show()
        pane = room.current['panes'][0]
        self.feed(pane, pid=500)
        self.fronts[pane.window.id] = (500, 'sh')
        self.rooms.line('A 0 codex')
        self.assertEqual(self.events(1), [(1, 'bytes', panes.CLEAR_LINE + b'codex\r')])
        self.fronts[pane.window.id] = (600, 'vim')
        self.rooms.line('A 0 codex')
        self.assertEqual(self.said[-1], f'N 9 0 {panes.BUSY}')

    def test_kilix_panes_are_a_window_of_their_own_over_the_shell(self):
        room = self.show()
        shell = room.current['panes'][0]
        self.rooms.line('A 0 kilix')
        view = room.current['panes'][0]
        self.assertIs(view.shell, shell)
        made = self.engine.made[view.window.id]
        self.assertEqual(made.argv, [sys.executable, str(panes.ATTACH)])
        self.assertEqual(made.env, ['KILIX_HOME=/k', 'ZELL_KILIX_WINDOW_ID=3'])
        # Leaving at once: it could not show them. The shell is back.
        os.unlink(view.window.map)
        self.rooms.poll(self.now + 0.5)
        self.assertIs(room.current['panes'][0], shell)
        self.assertIn(f'N 9 0 {panes.NO_VIEW}', self.said)
        # Leaving later says nothing; X over the viewer shows the shell.
        self.rooms.line('A 0 kilix')
        view = room.current['panes'][0]
        os.unlink(view.window.map)
        said = len(self.said)
        self.rooms.poll(self.now + panes.VIEW_FAILED_SECONDS + 1)
        self.assertIs(room.current['panes'][0], shell)
        self.assertEqual(len([s for s in self.said[said:] if s.startswith('N ')]), 0)
        self.rooms.line('A 0 kilix')
        self.rooms.line('X 0')
        self.assertIs(room.current['panes'][0], shell)

    def test_close_closes_every_window(self):
        self.show(9)
        self.show(10)
        self.rooms.line('A 0 kilix')
        self.rooms.close()
        closed = {c[1] for c in self.engine.calls if c[0] == 'close'}
        self.assertEqual(closed, set(range(1, self.engine.next)))


class LoadingTests(unittest.TestCase):
    def test_no_module_no_backend(self):
        with tempfile.TemporaryDirectory() as empty:
            self.assertIsNone(panes.load_embed_module({'KILIX_ZELL_EMBED_PANE': empty}))
            self.assertIsNone(panes.EmbedBackend.open({'KILIX_ZELL_EMBED_PANE': empty, 'KITTY_LISTEN_ON': 'unix:@x'}))

    @needs_module
    def test_an_engine_without_embedded_windows_is_no_backend(self):
        with tempfile.TemporaryDirectory() as d:
            kitten = os.path.join(d, 'kitten')
            with open(kitten, 'w') as f:
                f.write('#!/bin/sh\nexit 1\n')
            os.chmod(kitten, 0o700)
            self.assertIsNone(panes.EmbedBackend.open({'KITTY_LISTEN_ON': 'unix:@x', 'KILIX_KITTEN': kitten}))
            self.assertIsNone(panes.EmbedBackend.open({'KILIX_KITTEN': kitten}))

    def test_foreground_needs_a_live_pid(self):
        self.assertEqual(panes.embed_foreground(SimpleNamespace(pid=0)), (None, None))
        self.assertEqual(panes.embed_foreground(SimpleNamespace(pid=2 ** 22 + 1)), (None, None))


def free_display():
    for n in range(90, 140):
        if not os.path.exists(f'/tmp/.X11-unix/X{n}') and not os.path.exists(f'/tmp/.X{n}-lock'):
            return f':{n}'
    raise unittest.SkipTest('no free X display')


@needs_module
@unittest.skipUnless(os.environ.get('ZELL_TEST_KITTY') and shutil.which('Xvfb'),
                     'set ZELL_TEST_KITTY to a built kitty checkout with embedded windows (and have Xvfb)')
class RealEngineTests(unittest.TestCase):
    """The rooms through a private Kilix engine: real windows, real shells."""

    @classmethod
    def setUpClass(cls):
        kitty = os.environ['ZELL_TEST_KITTY']
        cls.dir = tempfile.TemporaryDirectory()
        root = cls.dir.name
        for name in ('config', 'runtime', 'cache'):
            os.mkdir(os.path.join(root, name), 0o700)
        display = free_display()
        cls.xvfb = subprocess.Popen(['Xvfb', display, '-screen', '0', '1600x1000x24', '-nolisten', 'tcp'],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.sock = f'unix:@zell-embed-test-{os.getpid()}'
        env = {'HOME': os.environ['HOME'], 'PATH': '/usr/bin:/bin', 'DISPLAY': display, 'TERM': 'xterm-256color',
               'KITTY_CONFIG_DIRECTORY': f'{root}/config', 'KITTY_RUNTIME_DIRECTORY': f'{root}/runtime',
               'KITTY_CACHE_DIRECTORY': f'{root}/cache', 'LIBGL_ALWAYS_SOFTWARE': '1'}
        for _ in range(100):
            if os.path.exists(f'/tmp/.X11-unix/X{display[1:]}'):
                break
            time.sleep(0.1)
        cls.log = open(os.path.join(root, 'engine.log'), 'wb')
        cls.engine_process = subprocess.Popen(
            [f'{kitty}/kitty/launcher/kitty', '--config', 'NONE', '-o', 'allow_remote_control=socket',
             '-o', 'enable_audio_bell=no', '-o', 'shell_integration=disabled', '--listen-on', cls.sock,
             'sh', '-c', 'sleep 100000'], env=env, stdout=cls.log, stderr=cls.log)
        cls.engine = KEP.Engine(cls.sock, f'{kitty}/kitty/launcher/kitten', env={'PATH': '/usr/bin:/bin',
                                                                               'HOME': os.environ['HOME']})
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            try:
                cls.engine.run('ls')
                break
            except KEP.EmbedError:
                time.sleep(0.2)
        else:
            cls.tearDownClass()
            raise RuntimeError('the private engine did not start')

    @classmethod
    def tearDownClass(cls):
        for process in (cls.engine_process, cls.xvfb):
            process.terminate()
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        cls.log.close()
        cls.dir.cleanup()

    def setUp(self):
        self.said = []
        backend = panes.EmbedBackend(KEP, self.engine)
        self.rooms = panes.EmbedRooms(DISPLAY, ['sh'], backend, say=self.said.append)

    def tearDown(self):
        self.rooms.close()

    def text(self, pane):
        f = pane.frame
        if f is None:
            return []
        return [''.join(KEP.cell_text(f, y * f.cols + x) for x in range(f.cols)).rstrip() for y in range(f.rows)]

    def wait(self, test, seconds=10):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.rooms.poll(time.monotonic())
            if test():
                return
            time.sleep(0.05)
        self.fail(f'timed out; panes: {[self.text(p) for p in self.rooms.rooms[9].panes()]}')

    def test_typing_resizing_scrolling_pasting_and_restarting(self):
        self.rooms.line('S 9 61 16')
        room = self.rooms.rooms[9]
        left, right = room.current['panes']
        self.wait(lambda: left.frame is not None and right.frame is not None)
        self.rooms.line('K' + b'stty size\r'.hex())
        self.wait(lambda: '16 30' in self.text(left))
        records = panes.embed_grid(DISPLAY, room)
        rows = [''.join(chr(r[0]) for r in records[y * 61:y * 61 + 30]).rstrip() for y in range(16)]
        self.assertIn('16 30', rows)
        # A new size reaches the program.
        self.rooms.line('S 9 81 20')
        self.rooms.line('K' + b'clear; stty size\r'.hex())
        self.wait(lambda: '20 40' in self.text(left))
        # The wheel scrolls Kilix's history, reported to Zell.
        self.rooms.line('K' + b'seq 1 300\r'.hex())
        self.wait(lambda: '300' in self.text(left))
        self.rooms.line('M 0 0 4 0 8 8')
        self.wait(lambda: left.frame.view_offset == panes.WHEEL_ROWS)
        self.wait(lambda: any(s.startswith('B 9 0 ') and s.split()[4] == str(left.frame.history_rows - 3)
                              for s in self.said))
        self.rooms.line('W 0 bottom')
        self.wait(lambda: left.frame.view_offset == 0)
        self.rooms.line('V 0 0')
        self.wait(lambda: left.frame.view_offset == left.frame.history_rows)
        # Typing shows the live rows again.
        self.rooms.line('K' + b'\x15'.hex())
        self.wait(lambda: left.frame.view_offset == 0)
        # A paste reaches the other pane.
        self.rooms.line('F 1')
        self.rooms.line('P 9 1 ' + b'echo pasted-ok\r'.hex())
        self.rooms.line('P 9 1 .')
        self.wait(lambda: 'pasted-ok' in self.text(right))
        # Restart: a new window in the same place.
        old_id = left.window.id
        self.rooms.line('X 0')
        fresh = room.current['panes'][0]
        self.assertNotEqual(fresh.window.id, old_id)
        self.wait(lambda: fresh.frame is not None)
        self.assertFalse(os.path.exists(left.window.map))

    def test_a_shell_that_exits_comes_back(self):
        self.rooms.line('S 9 61 16')
        left = self.rooms.rooms[9].current['panes'][0]
        self.wait(lambda: left.frame is not None)
        self.rooms.line('K' + b'exit\r'.hex())
        self.wait(lambda: left.fd is None)
        time.sleep(panes.RESPAWN_SECONDS)
        self.wait(lambda: self.rooms.rooms[9].current['panes'][0] is not left)
        fresh = self.rooms.rooms[9].current['panes'][0]
        self.wait(lambda: fresh.frame is not None)

    def test_a_drag_selects_and_the_release_copies(self):
        self.rooms.line('S 9 61 16')
        left = self.rooms.rooms[9].current['panes'][0]
        self.wait(lambda: left.frame is not None)
        self.rooms.line('K' + b'clear; echo pick-this-word\r'.hex())
        self.wait(lambda: 'pick-this-word' in self.text(left))
        y = self.text(left).index('pick-this-word') * 16 + 8
        for line in (f'M 0 0 1 0 1 {y}', f'M 0 2 1 0 50 {y}', f'M 0 1 1 0 110 {y}'):
            self.rooms.line(line)
        self.assertEqual(self.said[-1], 'N 9 0 Copied 1 line.')
        import base64
        pieces = [s[2:] for s in self.said if s.startswith('Y ') and s != 'Y .']
        self.assertEqual(base64.b64decode(''.join(pieces)).decode(), 'pick-this-word')

    def test_the_windows_are_kilix_panes(self):
        self.rooms.line('S 9 61 16')
        left = self.rooms.rooms[9].current['panes'][0]
        self.wait(lambda: left.frame is not None)
        self.rooms.line('K' + b'echo listed-here\r'.hex())
        self.wait(lambda: 'listed-here' in self.text(left))
        self.assertIn('listed-here', self.engine.run('get-text', '--match', f'id:{left.window.id}'))
        self.assertIn(f'"id": {left.window.id}', self.engine.run('ls'))


if __name__ == '__main__':
    unittest.main()
