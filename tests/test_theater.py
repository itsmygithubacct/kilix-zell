import io
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
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
        def which(name):
            return '/usr/bin/chromium' if name == 'chromium' else None

        page = browser.PrivateBrowser(session_type=FakeSession, which=which)
        session = self.session(browser=page)
        events = session.handle_line(json.dumps({
            'cmd': 'open', 'id': 'web', 'browser': 'firefox-esr',
            'source': {'kind': 'url', 'url': 'https://example.com/a'},
        }).encode())
        install = [event for event in events if event['event'] == 'install'][-1]
        self.assertEqual(install['action'], 'install-firefox-esr')
        self.assertNotIn('chromium', json.dumps(install))
        self.assertNotIn('apt-get', json.dumps(install))
        self.assertNotIn('sudo', json.dumps(install))
        self.assertNotIn('command', install)
        self.assertEqual(page.launches, [])
        self.assertNotIn('web', session.items)
        cancelled = session.handle_line(json.dumps({'cmd': 'install_cancel', 'id': 'web'}).encode())
        self.assertTrue(any(event['event'] == 'error' and event['error'] == 'install' for event in cancelled))
        self.assertEqual(page.launches, [])
        self.assertNotIn('web', session.items)

    def test_chrome_install_uses_the_local_deb(self):
        ready = {'on': False}

        def which(name):
            if ready['on'] and name in ('google-chrome', 'google-chrome-stable'):
                return '/usr/bin/google-chrome-stable'
            return None

        page = browser.PrivateBrowser(session_type=FakeSession, which=which)
        session = self.session(browser=page)
        original = subprocess.Popen

        def boom(*args, **kwargs):
            raise AssertionError('install must not spawn a process')

        subprocess.Popen = boom
        try:
            events = session.handle_line(json.dumps({
                'cmd': 'open', 'id': 'web', 'browser': 'chrome',
                'source': {'kind': 'url', 'url': 'https://example.com/a'},
            }).encode())
            failed = session.handle_line(json.dumps({'cmd': 'install_result', 'id': 'web', 'ok': False}).encode())
        finally:
            subprocess.Popen = original
        install = [event for event in events if event['event'] == 'install'][-1]
        self.assertEqual(install['action'], 'install-chrome-deb')
        self.assertEqual(install['deb'], browser.CHROME_DEB_NAME)
        self.assertNotEqual(install['action'], 'google-chrome-stable')
        self.assertNotIn('apt-get', json.dumps(install))
        self.assertNotIn('sudo', json.dumps(install))
        self.assertTrue(any(event['error'] == 'install' for event in failed if event['event'] == 'error'))
        self.assertEqual(page.launches, [])
        self.assertNotIn('web', session.items)
        again = session.handle_line(json.dumps({
            'cmd': 'open', 'id': 'web', 'browser': 'chrome',
            'source': {'kind': 'url', 'url': 'https://example.com/a'},
        }).encode())
        self.assertEqual(again[-1]['phase'], 'request')
        ready['on'] = True
        done = session.handle_line(json.dumps({'cmd': 'install_result', 'id': 'web', 'ok': True}).encode())
        self.assertEqual(page.launches[0][0], '/usr/bin/google-chrome-stable')
        self.assertIn('--new-window', page.launches[0])
        self.assertIn('web', session.items)
        self.assertEqual(session.items['web']['paused'], True)
        self.assertTrue(any(event.get('phase') == 'done' for event in done if event['event'] == 'install'))

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
        self.assertEqual(page.session.injector.calls[-1][:2], (29, 18))

    def test_pause_updates_header_flags_without_a_new_picture(self):
        session = self.session()
        session.items['a'] = theater._blank('file', str(self.file('a.mp4')))
        session.items['a']['paused'] = False
        session.items['a']['position_ms'] = 1500
        session.items['a']['duration_ms'] = 20000
        session.active = 'a'
        fd = os.memfd_create('theater')
        session.attach(fd)
        pixels = bytes([1, 2, 3, 255]) * (2 * 4)
        session.write_frame(pixels, 2, 4, session.generation)
        session.handle({'cmd': 'pause'})
        os.lseek(fd, 0, os.SEEK_SET)
        header = os.read(fd, theater.HEADER)
        _magic, _version, width, height, seq, flags, pos, dur, _stride, _fmt, generation = struct.unpack_from('<4s10I', header)
        self.assertEqual((width, height, flags, pos, dur), (2, 4, 1, 1500, 20000))
        self.assertGreater(seq, 2)
        self.assertEqual(seq % 2, 0)
        self.assertEqual(generation, session.generation)
        os.lseek(fd, theater.HEADER, os.SEEK_SET)
        self.assertEqual(os.read(fd, len(pixels)), pixels)
        session.handle({'cmd': 'seek_ms', 'position_ms': 1800})
        os.lseek(fd, 0, os.SEEK_SET)
        header = os.read(fd, theater.HEADER)
        _magic, _version, width, height, seq, flags, pos, dur, _stride, _fmt, _generation = struct.unpack_from('<4s10I', header)
        self.assertEqual((width, height, flags, pos), (2, 4, 1, 1800))
        os.lseek(fd, theater.HEADER, os.SEEK_SET)
        self.assertEqual(os.read(fd, len(pixels)), pixels)
        session.items['a']['paused'] = False
        session._finish_media('a', session.generation, True, '')
        os.lseek(fd, 0, os.SEEK_SET)
        header = os.read(fd, theater.HEADER)
        _magic, _version, _width, _height, _seq, flags, pos, _dur, _stride, _fmt, _generation = struct.unpack_from('<4s10I', header)
        self.assertEqual(flags, 1 | 2)
        self.assertEqual(pos, 20000)
        os.close(fd)

    def test_model_failure_does_not_report_playing_or_keep_audio(self):
        holds = []
        children = []

        def hold(active, _owner):
            holds.append(bool(active))
            return False

        def launch(_item):
            proc = theater.spawn([sys.executable, '-c', 'import time; time.sleep(30)'])
            children.append(proc)
            return proc

        session = self.session(play_frames=True, audio_launcher=launch, hold_audio=hold)
        clip = os.environ.get('PLAYBACK_FIXTURE')
        clip = str(Path(clip) / 'sample-a.mp4') if clip else ''
        if clip and Path(clip).is_file():
            session.handle_line(json.dumps({
                'cmd': 'open', 'id': 'a', 'browser': None,
                'source': {'kind': 'file', 'path': clip},
            }).encode())
        else:
            session.items['a'] = theater._blank('file', str(self.file('a.mp4')), {
                'width': 160, 'height': 96, 'duration_ms': 20000, 'audio': True, 'rate': 12,
            })
        original = theater.load_ink

        def boom(_selected):
            raise RuntimeError('synthetic model load failure')

        theater.load_ink = boom
        try:
            events = session.handle_line(json.dumps({'cmd': 'use', 'id': 'a'}).encode())
            started = list(holds)
        finally:
            theater.load_ink = original
            session.shutdown()
        fatal_at = next(index for index, event in enumerate(events) if event.get('event') == 'fatal')
        status_at = next(index for index, event in enumerate(events) if event.get('event') == 'status')
        self.assertLess(fatal_at, status_at)
        self.assertEqual(events[fatal_at]['error'], 'synthetic model load failure')
        self.assertNotEqual(events[status_at]['state'], 'playing')
        self.assertFalse(any(event.get('state') == 'playing' for event in events))
        self.assertEqual(session.items['a']['paused'], True)
        self.assertIsNone(session.audio_child)
        self.assertEqual(children, [])
        self.assertEqual(started, [])

    def test_decoder_start_failure_reaps_the_audio_child(self):
        holds = []
        children = []

        def hold(active, _owner):
            holds.append(bool(active))
            return False

        def launch(_item):
            proc = theater.spawn([sys.executable, '-c', 'import time; time.sleep(30)'])
            children.append(proc)
            return proc

        session = self.session(play_frames=True, audio_launcher=launch, hold_audio=hold)
        session.ink = object()
        session.items['a'] = theater._blank('file', str(self.file('a.mp4')), {
            'width': 16, 'height': 8, 'duration_ms': 20000, 'audio': True, 'rate': 12,
        })
        original = theater.spawn

        def spawning(argv, **kwargs):
            if argv and argv[0] == 'ffmpeg':
                raise OSError('ffmpeg unavailable')
            return original(argv, **kwargs)

        theater.spawn = spawning
        try:
            events = session.handle_line(json.dumps({'cmd': 'use', 'id': 'a'}).encode())
            started = list(holds)
        finally:
            theater.spawn = original
            session.shutdown()
        self.assertTrue(any(event.get('event') == 'error' and event.get('error') == 'decode' for event in events))
        self.assertFalse(any(event.get('state') == 'playing' for event in events))
        self.assertEqual(session.items['a']['paused'], True)
        self.assertEqual(len(children), 1)
        self.assertIsNotNone(children[0].poll())
        self.assertEqual(started, [True, False])

    def test_browser_resume_failure_is_not_playing(self):
        class Bad(FramePage):
            def resume(self):
                raise RuntimeError('control down')

        holds = []
        page = Bad()
        session = self.session(browser=page, hold_audio=lambda active, _owner: holds.append(bool(active)) or False)
        session.handle_line(json.dumps({
            'cmd': 'open', 'id': 'web', 'browser': 'chromium',
            'source': {'kind': 'url', 'url': 'https://example.com/a'},
        }).encode())
        events = session.handle_line(json.dumps({'cmd': 'use', 'id': 'web'}).encode())
        self.assertTrue(any(event.get('event') == 'error' and event.get('error') == 'capture' for event in events))
        self.assertFalse(any(event.get('state') == 'playing' for event in events))
        self.assertEqual(session.items['web']['paused'], True)
        self.assertEqual(holds, [True, False])

    def test_key_taps_once_and_text_is_pasted(self):
        page = FramePage()
        session = self.session(browser=page)
        session.handle({
            'cmd': 'open', 'id': 'web', 'browser': 'chromium',
            'source': {'kind': 'url', 'url': 'https://example.com/a'},
        })
        session.handle({'cmd': 'use', 'id': 'web'})
        session.handle({'cmd': 'key', 'key': 'a', 'action': 'down'})
        session.handle({'cmd': 'key', 'key': 'a', 'action': 'up'})
        session.handle({'cmd': 'key', 'text': 'hi', 'modifiers': 4})
        self.assertEqual(page.keys, [('a', 0, ''), ('', 4, 'hi')])
        rejected = session.handle_line(json.dumps({'cmd': 'key', 'key': 'b', 'modifiers': True}).encode())
        self.assertEqual(rejected[-1]['error'], 'malformed')
        self.assertEqual(page.keys, [('a', 0, ''), ('', 4, 'hi')])
        self.assertTrue(all(isinstance(modifiers, int) and not isinstance(modifiers, bool) for _key, modifiers, _text in page.keys))

    def test_resize_and_popout_keep_the_capture_viewport(self):
        def which(name):
            return '/usr/bin/chromium' if name in ('chromium', 'chromium-browser') else None

        page = browser.PrivateBrowser(session_type=FakeSession, which=which)
        session = self.session(browser=page)
        session.handle({
            'cmd': 'open', 'id': 'web', 'browser': 'chromium',
            'source': {'kind': 'url', 'url': 'https://example.com/a'},
        })
        page.viewport = (320, 180)
        session.handle({'cmd': 'use', 'id': 'web'})
        session.handle({'cmd': 'resize', 'width': 800, 'height': 600})
        session.handle({'cmd': 'popout'})
        session.handle({'cmd': 'dock'})
        self.assertEqual(page.viewport, (320, 180))
        self.assertFalse(hasattr(page.session, 'size'))
        self.assertEqual(session.dest, (800, 600))
        self.assertEqual(fit.contain(320, 180, 800, 600), (800, 450))
        session.items['web']['src_w'] = 320
        session.items['web']['src_h'] = 180
        session.handle({'cmd': 'pointer', 'x': 0, 'y': 10, 'button': 1, 'action': 'move'})
        self.assertEqual(page.session.injector.calls, [])

    def test_switch_stores_the_paused_page_playhead(self):
        class MediaPage(FramePage):
            def pause(self):
                self.paused = True
                return {'paused': True, 'position_ms': 3200, 'duration_ms': 20000, 'present': True}

            def resume(self):
                self.paused = False
                self.resumes += 1
                return {'paused': False, 'position_ms': 400, 'duration_ms': 20000, 'present': True}

        made = []

        def factory():
            page = MediaPage()
            made.append(page)
            return page

        session = self.session(browser_factory=factory)
        for identity in ('one', 'two'):
            session.handle({
                'cmd': 'open', 'id': identity, 'browser': 'chromium',
                'source': {'kind': 'url', 'url': 'https://example.com/' + identity},
            })
        session.handle({'cmd': 'use', 'id': 'one'})
        session.handle({'cmd': 'use', 'id': 'two'})
        self.assertEqual(session.items['one']['position_ms'], 3200)
        self.assertEqual(session.items['one']['duration_ms'], 20000)
        self.assertEqual(session.items['one']['paused'], True)
        self.assertGreaterEqual(made[1].resumes, 1)
        self.assertEqual(session.items['two']['paused'], False)

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

    def test_serve_caps_a_line_while_reading(self):
        session = self.session()
        path = self.file('clip.mp4')
        opened = json.dumps({
            'cmd': 'open', 'id': 'a', 'browser': None,
            'source': {'kind': 'file', 'path': str(path)},
        }).encode() + b'\n'
        payload = opened + (b'y' * 70000) + b'\n{"cmd":"stop"}\n'
        stdout = io.BytesIO()
        code = theater.serve(io.BytesIO(payload), stdout, session, None)
        self.assertEqual(code, 0)
        lines = [json.loads(line) for line in stdout.getvalue().splitlines()]
        self.assertTrue(any(event.get('detail') == 'line too long' for event in lines))
        self.assertFalse(any(event.get('detail') == 'bad json' for event in lines))
        self.assertEqual(lines[-1]['event'], 'status')

    def test_loopback_http_is_a_url_and_public_http_is_not(self):
        def which(name):
            return '/usr/bin/chromium' if name in ('chromium', 'chromium-browser') else None

        page = browser.PrivateBrowser(session_type=FakeSession, which=which)
        session = self.session(browser=page)
        with self.assertRaises(theater.ProtocolError):
            session.handle({'cmd': 'open', 'id': 'bad', 'browser': 'chromium', 'source': {'kind': 'url', 'url': 'http://example.com/a'}})
        with self.assertRaises(theater.ProtocolError):
            session.handle({'cmd': 'open', 'id': 'auth', 'browser': 'chromium', 'source': {'kind': 'url', 'url': 'https://user:pass@example.com/a'}})
        self.assertEqual(page.launches, [])
        session.handle({'cmd': 'open', 'id': 'local', 'browser': 'chromium', 'source': {'kind': 'url', 'url': 'http://127.0.0.1:9/clip'}})
        self.assertEqual(page.launches[0][-1], 'http://127.0.0.1:9/clip')

    def test_each_url_has_its_own_browser(self):
        made = []

        def which(name):
            return '/usr/bin/chromium' if name in ('chromium', 'chromium-browser') else None

        def factory():
            page = browser.PrivateBrowser(session_type=FakeSession, which=which)
            made.append(page)
            return page

        session = self.session(browser_factory=factory)
        session.handle({'cmd': 'open', 'id': 'one', 'browser': 'chromium', 'source': {'kind': 'url', 'url': 'https://example.com/one'}})
        session.handle({'cmd': 'open', 'id': 'two', 'browser': 'chromium', 'source': {'kind': 'url', 'url': 'https://example.com/two'}})
        self.assertEqual(len(made), 2)
        first = made[0].session
        second = made[1].session
        self.assertFalse(first.closed)
        self.assertFalse(second.closed)
        self.assertEqual(made[0].launches[0][-1], 'https://example.com/one')
        self.assertEqual(made[1].launches[0][-1], 'https://example.com/two')
        session.handle({'cmd': 'close', 'id': 'one'})
        self.assertTrue(first.closed)
        self.assertFalse(second.closed)

    def test_mpv_clock_reads_time_pos(self):
        path = self.root / 'mpv.sock'
        ready = threading.Event()

        def serve_socket():
            server = socket.socket(socket.AF_UNIX)
            server.bind(str(path))
            server.listen(1)
            ready.set()
            conn, _addr = server.accept()
            data = b''
            while b'\n' not in data:
                data += conn.recv(1024)
            conn.sendall(b'{"event":"property-change"}\n{"error":"success","data":1.5,"request_id":1}\n')
            conn.close()
            server.close()

        thread = threading.Thread(target=serve_socket)
        thread.start()
        self.assertTrue(ready.wait(1))
        clock = theater.PlaybackClock(0, path)
        value = 0
        for _ in range(20):
            value = clock.now()
            if value > 1:
                break
            time.sleep(0.02)
        clock.close()
        thread.join(1)
        self.assertGreater(value, 1.2)
        self.assertLess(value, 2.5)


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

    def test_fixture_playback_paces_and_emits_without_commands(self):
        home = os.environ.get('NEURONTE_HOME') or ''
        root = os.environ.get('PLAYBACK_FIXTURE') or ''
        if not home:
            self.fail('NEURONTE_HOME is unset. Point it at the RC9 runtime fixture.')
        if not root:
            self.fail('PLAYBACK_FIXTURE is unset. Point it at the synthetic A/B clips.')
        clip_a = Path(root) / 'sample-a.mp4'
        clip_b = Path(root) / 'sample-b.mp4'
        self.assertTrue(clip_a.is_file() and clip_b.is_file())
        runtime = theater.select_runtime({'NEURONTE_HOME': home})
        session = theater.TheaterSession(runtime, hold_audio=quiet_hold, play_frames=True)
        session.audio_launcher = lambda item: theater.product_audio(session, item, ao='null')
        fd = os.memfd_create('theater')
        pump = Pump(session, fd)
        try:
            pump.send({'cmd': 'resize', 'width': 160, 'height': 96})
            pump.send({'cmd': 'open', 'id': 'a', 'browser': None, 'source': {'kind': 'file', 'path': str(clip_a)}})
            self.assertTrue(pump.wait_for(lambda event: event.get('event') == 'status' and event.get('id') == 'a' and event.get('state') == 'paused', 8))
            pump.send({'cmd': 'use', 'id': 'a'})
            self.assertTrue(pump.wait_for(lambda event: event.get('state') == 'playing' and event.get('id') == 'a', 8))
            mark = len(pump.events)
            started = time.monotonic()
            self.assertTrue(pump.wait_for(
                lambda event: event.get('event') == 'status' and event.get('id') == 'a' and event.get('position_ms', 0) >= 1200,
                4, mark))
            elapsed = time.monotonic() - started
            latest = [event for event in pump.events if event.get('event') == 'status' and event.get('id') == 'a'][-1]
            position = latest['position_ms']
            self.assertGreater(elapsed, 0.6)
            self.assertLess(position, int(elapsed * 1000) + 1500)
            self.assertLess(position, 8000)
            captions = [event.get('text') for event in pump.events if event.get('event') == 'caption']
            self.assertIn('First synthetic caption.', captions)
            self.assertNotIn('Second video caption.', captions)
            magic, _version, width, height, seq, _flags, header_pos, _dur, _stride, _fmt, _gen = stable_header(fd)
            frames = seq // 2
            self.assertEqual(magic, b'ZTHR')
            self.assertEqual((width, height), (160, 96))
            self.assertGreater(header_pos, 800)
            self.assertGreater(frames, 4)
            self.assertLess(frames, 70)
            decoder = session.decoder
            audio = session.audio_child
            held = position
            mark = len(pump.events)
            pump.send({'cmd': 'pause'})
            self.assertTrue(pump.wait_for(lambda event: event.get('id') == 'a' and event.get('state') == 'paused', 3, mark))
            _magic, _version, width, height, seq, flags, header_pos, _dur, _stride, _fmt, _gen = stable_header(fd)
            self.assertEqual(flags & 1, 1)
            self.assertEqual((width, height), (160, 96))
            self.assertGreater(seq, 0)
            self.assertEqual(seq % 2, 0)
            self.assertGreater(header_pos, 800)
            if decoder is not None:
                self.assertIsNotNone(decoder.poll())
            if audio is not None:
                self.assertIsNotNone(audio.poll())
            time.sleep(0.35)
            pump.collect(0.05)
            paused_at = [event for event in pump.events if event.get('event') == 'status' and event.get('id') == 'a' and event.get('state') == 'paused'][-1]['position_ms']
            self.assertLess(abs(paused_at - held), 500)
            pump.send({'cmd': 'open', 'id': 'b', 'browser': None, 'source': {'kind': 'file', 'path': str(clip_b)}})
            pump.send({'cmd': 'use', 'id': 'b'})
            self.assertTrue(pump.wait_for(lambda event: event.get('id') == 'b' and event.get('state') == 'playing', 8))
            self.assertTrue(pump.wait_for(
                lambda event: event.get('event') == 'caption' and event.get('text') == 'Second video caption.',
                3))
            self.assertLess(abs(session.items['a']['position_ms'] - paused_at), 500)
            mark = len(pump.events)
            pump.send({'cmd': 'use', 'id': 'a'})
            resumed = pump.wait_for(lambda event: event.get('id') == 'a' and event.get('state') == 'playing', 8, mark)
            self.assertTrue(resumed)
            again = [event for event in pump.events[mark:] if event.get('id') == 'a' and event.get('state') == 'playing'][-1]
            self.assertGreater(again['position_ms'], 700)
            self.assertLess(abs(again['position_ms'] - paused_at), 1500)
        finally:
            pump.send({'cmd': 'stop'})
            pump.finish()
            os.close(fd)

    def test_capture_frames_reach_the_memfd(self):
        home = os.environ.get('NEURONTE_HOME') or ''
        if not home:
            self.fail('NEURONTE_HOME is unset. Point it at the RC9 runtime fixture.')
        runtime = theater.select_runtime({'NEURONTE_HOME': home})
        page = FramePage()
        session = theater.TheaterSession(
            runtime, browser=page, hold_audio=quiet_hold, play_frames=True)
        session.dest = (16, 8)
        fd = os.memfd_create('theater')
        pump = Pump(session, fd)
        try:
            pump.send({'cmd': 'open', 'id': 'web', 'browser': 'chromium', 'source': {'kind': 'url', 'url': 'http://127.0.0.1:9/page'}})
            self.assertTrue(pump.wait_for(lambda event: event.get('id') == 'web' and event.get('state') == 'paused', 3))
            pump.send({'cmd': 'use', 'id': 'web'})
            self.assertTrue(pump.wait_for(lambda event: event.get('id') == 'web' and event.get('position_ms', 0) >= 150, 3))
            magic, _version, width, height, seq, _flags, header_pos, _dur, _stride, _fmt, _gen = stable_header(fd)
            self.assertEqual(magic, b'ZTHR')
            self.assertEqual((width, height), (16, 8))
            self.assertGreater(seq, 0)
            self.assertEqual(seq % 2, 0)
            self.assertGreater(header_pos, 0)
            os.lseek(fd, theater.HEADER, os.SEEK_SET)
            pixels = os.read(fd, width * height * 4)
            self.assertEqual(len(pixels), width * height * 4)
            self.assertNotEqual(pixels, b'\x00' * len(pixels))
            self.assertGreater(page.resumes, 0)
            pump.send({'cmd': 'pause'})
            self.assertTrue(pump.wait_for(lambda event: event.get('id') == 'web' and event.get('state') == 'paused', 3))
            self.assertTrue(page.paused)
            self.assertFalse(page.closed)
        finally:
            pump.send({'cmd': 'stop'})
            pump.finish()
            os.close(fd)


class FramePage:
    """Stand-in page with the browser adapter's frame and media methods."""

    def __init__(self):
        self.session = self
        self.launches = []
        self.paused = True
        self.closed = False
        self.resumes = 0
        self.keys = []

    def launch(self, browser, url, width, height):
        self.launches.append((browser, url, width, height))

    def frame(self, timeout=0.1):
        if self.paused:
            return None
        return 16, 8, bytes([255, 0, 0]) * (16 * 8)

    def pause(self):
        self.paused = True

    def resume(self):
        self.paused = False
        self.resumes += 1

    def media_state(self):
        return {'paused': self.paused, 'position_ms': None, 'duration_ms': None}

    def close(self):
        self.closed = True

    def popout(self):
        self.popped = True

    def dock(self):
        self.popped = False

    def pointer(self, x, y, button, action):
        self.point = (x, y, button, action)

    def key(self, key, modifiers=0, text=''):
        self.keys.append((key, modifiers, text))

    @staticmethod
    def which(name):
        if name in ('chromium', 'chromium-browser'):
            return '/usr/bin/chromium'
        return None


class Pump:
    def __init__(self, session, frame_fd):
        self.session = session
        self.events = []
        self.buf = b''
        self.errors = []
        self.read_fd, self.write_fd = os.pipe()
        self.out_r, self.out_w = os.pipe()
        os.set_blocking(self.out_r, False)
        self.thread = threading.Thread(target=self._serve, args=(frame_fd,), daemon=True)
        self.thread.start()

    def _serve(self, frame_fd):
        stdin = os.fdopen(self.read_fd, 'rb', buffering=0)
        stdout = os.fdopen(self.out_w, 'wb', buffering=0)
        try:
            theater.serve(stdin, stdout, self.session, frame_fd)
        except Exception as error:
            self.errors.append(error)
        finally:
            for handle in (stdin, stdout):
                try:
                    handle.close()
                except OSError:
                    pass

    def send(self, command):
        try:
            os.write(self.write_fd, json.dumps(command).encode() + b'\n')
        except OSError:
            pass

    def collect(self, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                chunk = os.read(self.out_r, 65536)
            except BlockingIOError:
                time.sleep(0.02)
                continue
            if not chunk:
                break
            self.buf += chunk
            while b'\n' in self.buf:
                line, self.buf = self.buf.split(b'\n', 1)
                if line.strip():
                    self.events.append(json.loads(line))
        return self.events

    def wait_for(self, predicate, timeout, start=0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.collect(0.05)
            if any(predicate(event) for event in self.events[start:]):
                return True
            if self.errors:
                return False
        return False

    def finish(self):
        try:
            os.close(self.write_fd)
        except OSError:
            pass
        self.thread.join(5)
        self.collect(0.2)


def stable_header(fd):
    last = None
    for _ in range(8):
        os.lseek(fd, 0, os.SEEK_SET)
        raw = os.read(fd, theater.HEADER)
        unpacked = struct.unpack_from('<4s10I', raw)
        seq = unpacked[4]
        if seq % 2 == 0 and unpacked == last:
            return unpacked
        last = unpacked
        time.sleep(0.01)
    return last
