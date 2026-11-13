import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import zell_theater as theater
import zell_theater_browser as browser
import zell_theater_fit as fit


class FakeInjector:
    def __init__(self):
        self.calls = []

    def move_click(self, x, y, button=0, press=None):
        self.calls.append((x, y, button, press))


class FakeSession:
    def __init__(self, *args, **kwargs):
        self.injector = FakeInjector()
        self.kwargs = None
        self.argv = None
        self.closed = False

    def start_xvfb(self, **kwargs):
        pass

    def launch_app(self, argv, **kwargs):
        self.argv = list(argv)
        self.kwargs = kwargs

    def make_injector(self):
        return self.injector

    def start_capture(self, **kwargs):
        pass

    def set_geometry(self, width, height):
        self.size = (width, height)

    def close(self):
        self.closed = True


def quiet_hold(*_args):
    return False


class TheaterTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def session(self, **kwargs):
        runtime = kwargs.pop('runtime', None) or {
            'role': 'installed', 'commit': '', 'selected': str(self.root), 'missing': '',
        }
        kwargs.setdefault('hold_audio', quiet_hold)
        kwargs.setdefault('play_frames', False)
        return theater.TheaterSession(runtime, **kwargs)

    def file(self, name):
        path = self.root / name
        path.write_bytes(b'')
        return path

    def test_contain_16_by_9_and_bounded_fullscreen(self):
        self.assertEqual(fit.contain(1920, 1080, 1408, 585), (1040, 585))
        self.assertEqual(fit.cell_pad(1040, 585), (1040, 588))
        self.assertEqual(fit.bound_frame(2560, 1440), (2048, 1152))
        session = self.session()
        self.assertEqual(session.published_size(1920, 1080)[:2], (1040, 585))
        session.dest = (2560, 1440)
        self.assertEqual(session.published_size(1920, 1080)[:2], (2048, 1152))
        session.handle({'cmd': 'resize', 'width': 2560, 'height': 1440})
        self.assertEqual(session.dest, (2560, 1440))
        before = session.dest
        with self.assertRaises(theater.ProtocolError):
            session.handle({'cmd': 'resize', 'width': 0, 'height': 100})
        self.assertEqual(session.dest, before)
        self.assertIsNone(fit.map_pointer(0, 10, 1408, 585, 1040, 585, 1040, 585))
        self.assertEqual(fit.map_pointer(184, 10, 1408, 585, 1040, 585, 1040, 585)[0], 0)

    def test_malformed_line_changes_nothing(self):
        session = self.session()
        path = self.file('clip.mp4')
        session.handle({'cmd': 'open', 'id': 'a', 'source': {'kind': 'file', 'path': str(path)}, 'browser': None})
        before = dict(session.items)
        generation = session.generation
        events = session.handle_line(b'{')
        self.assertEqual(events[-1]['error'], 'malformed')
        self.assertEqual(set(session.items), set(before))
        self.assertEqual(session.generation, generation)
        events = session.handle_line(b'{"cmd":"nope"}')
        self.assertEqual(events[-1]['detail'], 'unknown cmd')
        self.assertEqual(session.generation, generation)
        events = session.handle_line(b'{' * (theater.LINE_MAX + 1))
        self.assertEqual(events[-1]['detail'], 'line too long')

    def test_missing_weights_is_fatal_and_writes_no_frame(self):
        runtime = theater.select_runtime({'NEURONTE_HOME': str(self.root), 'GPU_TERMINAL_SOURCE_HOME': ''})
        self.assertEqual(runtime['role'], 'missing')
        self.assertTrue(runtime['missing'].endswith('neuronte-video-masks-v0.nrt'))
        session = self.session(runtime=runtime)
        fd = os.memfd_create('theater')
        stdout = io.BytesIO()
        code = theater.serve(io.BytesIO(b'{"cmd":"stop"}\n'), stdout, session, fd)
        self.assertEqual(code, 0)
        lines = [json.loads(line) for line in stdout.getvalue().splitlines()]
        self.assertEqual(lines[0]['event'], 'fatal')
        self.assertEqual(lines[0]['runtime']['role'], 'missing')
        self.assertNotIn('canonical_player', lines[0]['runtime'])
        os.lseek(fd, 0, os.SEEK_SET)
        self.assertEqual(os.read(fd, 4), b'\x00\x00\x00\x00')

    def test_stop_reaps_a_stand_in_child(self):
        session = self.session()
        child = theater.spawn([sys.executable, '-c', 'import time; time.sleep(30)'])
        session.audio_child = child
        session.handle({'cmd': 'stop'})
        self.assertIsNotNone(child.poll())

    def test_use_restores_playheads_and_keeps_one_audio_child(self):
        alive = []

        def launch(_item):
            proc = theater.spawn([sys.executable, '-c', 'import time; time.sleep(30)'])
            alive.append(proc)
            return proc

        session = self.session(audio_launcher=launch)
        first = self.file('a.mp4')
        second = self.file('b.mp4')
        session.handle({'cmd': 'open', 'id': 'a', 'source': {'kind': 'file', 'path': str(first)}, 'browser': None})
        session.handle({'cmd': 'open', 'id': 'b', 'source': {'kind': 'file', 'path': str(second)}, 'browser': None})
        session.handle({'cmd': 'use', 'id': 'a'})
        session.handle({'cmd': 'seek_ms', 'position_ms': 1250})
        session.handle({'cmd': 'use', 'id': 'b'})
        session.handle({'cmd': 'seek_ms', 'position_ms': 400})
        status = session.handle({'cmd': 'use', 'id': 'a'})
        self.assertEqual(status['position_ms'], 1250)
        self.assertEqual(session.items['b']['position_ms'], 400)
        status = session.handle({'cmd': 'use', 'id': 'b'})
        self.assertEqual(status['position_ms'], 400)
        self.assertEqual(session.items['a']['position_ms'], 1250)
        self.assertEqual(len([proc for proc in alive if proc.poll() is None]), 1)
        session.shutdown()
        self.assertEqual([proc for proc in alive if proc.poll() is None], [])

    def test_folder_listing_and_sidecar_captions(self):
        self.file('movie.mp4')
        (self.root / 'notes.txt').write_text('no')
        (self.root / 'movie.srt').write_text('1\n00:00:01,000 --> 00:00:02,000\nHello there\n')
        session = self.session()
        listing = session.handle({'cmd': 'open', 'id': 'dir', 'source': {'kind': 'folder', 'path': str(self.root)}, 'browser': None})
        self.assertEqual([row['name'] for row in listing['entries']], ['movie.mp4'])
        session.handle({'cmd': 'open', 'id': 'mov', 'source': {'kind': 'file', 'path': str(self.root / 'movie.mp4')}, 'browser': None})
        session.drain()
        session.handle({'cmd': 'use', 'id': 'mov'})
        text = [event for event in session.handle_line(b'{"cmd":"seek_ms","position_ms":1500}') if event['event'] == 'caption']
        self.assertEqual(text[-1]['text'], 'Hello there')
        hidden = [event for event in session.handle_line(b'{"cmd":"cc","on":false}') if event['event'] == 'caption']
        self.assertEqual(hidden[-1]['text'], '')

    def test_bad_caption_does_not_stop_playback(self):
        path = self.file('clip.mp4')
        path.with_suffix('.vtt').write_text('this is not a caption file')
        session = self.session()
        status = session.handle({'cmd': 'open', 'id': 'c', 'source': {'kind': 'file', 'path': str(path)}, 'browser': None})
        self.assertEqual(status['state'], 'paused')
        self.assertTrue(status['error'])
        status = session.handle({'cmd': 'use', 'id': 'c'})
        self.assertEqual(status['state'], 'playing')
        self.assertEqual(session.items['c']['caption_error'] != '', True)

    def test_firefox_install_does_not_launch_chromium(self):
        seen = []

        def privilege(argv):
            seen.append(list(argv))
            return 7

        def which(name):
            return '/usr/bin/chromium' if name == 'chromium' else None

        page = browser.PrivateBrowser(session_type=FakeSession, privilege=privilege, which=which)
        session = self.session(browser=page)
        session.handle({'cmd': 'open', 'id': 'web', 'source': {'kind': 'url', 'url': 'https://example.com/a'}, 'browser': 'firefox-esr'})
        self.assertEqual(seen[0][-1], 'firefox-esr')
        self.assertNotIn('chromium', seen[0])
        self.assertNotIn('google-chrome-stable', seen[0])
        self.assertEqual(page.launches, [])
        self.assertNotIn('web', session.items)

    def test_chrome_install_uses_the_local_deb(self):
        seen = []

        def privilege(argv):
            seen.append(list(argv))
            return 7

        page = browser.PrivateBrowser(session_type=FakeSession, privilege=privilege, which=lambda _name: None)
        session = self.session(browser=page)
        session.handle({'cmd': 'open', 'id': 'web', 'source': {'kind': 'url', 'url': 'https://example.com/a'}, 'browser': 'chrome'})
        self.assertEqual(seen[0][-1], browser.CHROME_DEB_NAME)
        self.assertNotIn('google-chrome-stable', seen[0])
        self.assertNotIn('sudo', seen[0])
        self.assertEqual(page.launches, [])

    def test_browser_stays_on_the_private_display(self):
        def which(name):
            return '/usr/bin/chromium' if name in ('chromium', 'chromium-browser') else None

        page = browser.PrivateBrowser(session_type=FakeSession, which=which)
        session = self.session(browser=page)
        session.handle({'cmd': 'open', 'id': 'web', 'source': {'kind': 'url', 'url': 'https://example.com/watch'}, 'browser': 'chromium'})
        self.assertEqual(page.launches[0][0], '/usr/bin/chromium')
        self.assertNotIn('new-tab', page.launches[0])
        self.assertTrue(page.session.kwargs['isolate_bus'])
        self.assertFalse(page.session.kwargs['clipboard'])
        session.handle({'cmd': 'use', 'id': 'web'})
        same = page.session
        session.handle({'cmd': 'popout'})
        session.handle({'cmd': 'dock'})
        self.assertIs(page.session, same)
        self.assertEqual(len(page.launches), 1)
        session.items['web']['src_w'] = 1920
        session.items['web']['src_h'] = 1080
        session.handle({'cmd': 'pointer', 'x': 0, 'y': 10, 'button': 1, 'action': 'move'})
        self.assertEqual(page.session.injector.calls, [])
        session.handle({'cmd': 'pointer', 'x': 200, 'y': 10, 'button': 1, 'action': 'down'})
        self.assertTrue(page.session.injector.calls)

    def test_generation_drops_a_stale_frame(self):
        session = self.session()
        session.items['a'] = theater._blank('file', str(self.file('a.mp4')))
        session.items['b'] = theater._blank('file', str(self.file('b.mp4')))
        session.active = 'a'
        session.generation = 1
        fd = os.memfd_create('theater')
        session.attach(fd)
        pixels = bytes([9, 8, 7, 255]) * (2 * 4)
        self.assertEqual(session.write_frame(pixels, 2, 4, 1), 2)
        session.use_item('b')
        self.assertIsNone(session.write_frame(pixels, 2, 4, 1))
        os.lseek(fd, 0, os.SEEK_SET)
        header = os.read(fd, theater.HEADER)
        magic, _version, width, _height, seq, _flags, _pos, _dur, _stride, _fmt, generation = struct.unpack_from('<4s10I', header)
        self.assertEqual(magic, b'ZTHR')
        self.assertEqual((width, seq), (0, 0))
        self.assertNotEqual(generation, 1)
        current = session.generation
        self.assertEqual(session.write_frame(pixels, 2, 4, current), 2)
        os.lseek(fd, 0, os.SEEK_SET)
        header = os.read(fd, theater.HEADER)
        _magic, _version, width, height, seq, _flags, _pos, _dur, _stride, _fmt, generation = struct.unpack_from('<4s10I', header)
        self.assertEqual((width, height, seq, generation), (2, 4, 2, current))
        os.lseek(fd, theater.HEADER, os.SEEK_SET)
        self.assertEqual(os.read(fd, 4), pixels[:4])

    def test_runtime_metadata_is_discovered(self):
        tree = self.root / 'runtime'
        (tree / 'weights').mkdir(parents=True)
        (tree / 'build').mkdir()
        (tree / 'weights' / 'neuronte-video-masks-v0.nrt').write_bytes(b'nrt')
        (tree / 'build' / 'libneuronte.so').write_bytes(b'so')
        commit = 'ab' * 20
        (tree / '.plebian-os-neuronte.json').write_text(json.dumps({'commit': commit}))
        found = theater.select_runtime({'NEURONTE_HOME': str(tree)})
        self.assertEqual(found['role'], 'fixture')
        self.assertEqual(found['commit'], commit)
        self.assertEqual(found['selected'], str(tree))
        installed = theater.select_runtime({'GPU_TERMINAL_SOURCE_HOME': str(self.root), 'NEURONTE_HOME': ''})
        # The tree is not at source/neuronte, so the installed path is absent.
        self.assertEqual(installed['role'], 'missing')
        source = self.root / 'src'
        neuronte = source / 'neuronte'
        (neuronte / 'weights').mkdir(parents=True)
        (neuronte / 'build').mkdir()
        (neuronte / 'weights' / 'neuronte-video-masks-v0.nrt').write_bytes(b'nrt')
        (neuronte / 'build' / 'libneuronte.so').write_bytes(b'so')
        installed = theater.select_runtime({'GPU_TERMINAL_SOURCE_HOME': str(source), 'NEURONTE_HOME': ''})
        self.assertEqual(installed['role'], 'installed')
        self.assertEqual(installed['commit'], '')
        text = '\n'.join(path.read_text() for path in (
            Path(theater.__file__), Path(browser.__file__), Path(fit.__file__)))
        self.assertNotIn('canonical_player', text)
        self.assertNotIn('eb835f', text)

    def test_subprocess_protocol(self):
        env = {'PATH': os.environ.get('PATH', ''), 'GPU_TERMINAL_SOURCE_HOME': str(self.root), 'PYTHONDONTWRITEBYTECODE': '1'}
        proc = subprocess.run(
            [sys.executable, '-B', str(Path(theater.__file__))],
            input=b'{}\n{"cmd":"stop"}\n', capture_output=True, env=env, timeout=15, check=False)
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        lines = [json.loads(line) for line in proc.stdout.splitlines()]
        self.assertEqual(lines[0]['event'], 'fatal')
        self.assertEqual(lines[1]['event'], 'error')
        self.assertEqual(lines[-1]['event'], 'status')


class NeuronteFixtureTests(unittest.TestCase):
    def test_masks_roundtrip_lavfi_and_pattern(self):
        home = os.environ.get('NEURONTE_HOME') or ''
        if not home:
            self.fail('NEURONTE_HOME is unset. Point it at the RC9 runtime fixture.')
        runtime = theater.select_runtime({'NEURONTE_HOME': home})
        self.assertEqual(runtime['role'], 'fixture')
        self.assertEqual(runtime['missing'], '')
        record = json.loads((Path(home) / '.plebian-os-neuronte.json').read_text())
        self.assertEqual(runtime['commit'], record['commit'])
        import numpy as np
        session = theater.TheaterSession(runtime, hold_audio=quiet_hold, play_frames=False)
        session.generation = 1
        session.dest = (2, 4)
        pattern = np.arange(2 * 4 * 3, dtype=np.uint8).reshape(4, 2, 3)
        self.assertIsNotNone(session.render_rgb(pattern, 1))
        pixels, width, height = theater.fit_bgr(session.ink, pattern)
        bgra = np.frombuffer(pixels, dtype=np.uint8).reshape(height, width, 4)
        back = np.stack([bgra[..., 2], bgra[..., 1], bgra[..., 0]], axis=-1)
        self.assertTrue(np.array_equal(back, pattern))
        raw = subprocess.check_output([
            'ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'color=c=red:s=8x4:d=0.1',
            '-frames:v', '1', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-',
        ], timeout=15)
        red = np.frombuffer(raw, dtype=np.uint8).reshape(4, 8, 3).copy()
        session.dest = (8, 4)
        session.generation = 1
        self.assertIsNotNone(session.render_rgb(red, 1))
        self.assertEqual(session.shown[:2], (8, 4))
        fitted, width, height = theater.fit_bgr(session.ink, red)
        bgra = np.frombuffer(fitted, dtype=np.uint8).reshape(height, width, 4)
        back = np.stack([bgra[..., 2], bgra[..., 1], bgra[..., 0]], axis=-1)
        self.assertTrue(np.array_equal(back, red))
