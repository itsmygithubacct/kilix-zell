"""Private browser adapter. Live browser checks use a local silent fixture."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import zell_theater_browser as browser


class FakeInjector:
    def __init__(self):
        self.calls = []

    def move_click(self, x, y, button=0, press=None):
        self.calls.append(('pointer', x, y, button, press))

    def chord(self, key, mods, etype, shifted=None):
        self.calls.append(('chord', key, mods, etype))
        return True

    def paste(self, text):
        self.calls.append(('paste', text))
        return True


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

    def close(self):
        self.closed = True


PAGE = """<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>theater</title>
<style>
html,body{margin:0;height:100%;background:#081018;color:#fff;font-family:sans-serif}
#field{position:absolute;left:0;top:0;width:100%;height:38%;font-size:32px;box-sizing:border-box}
#btn{position:absolute;left:0;top:42%;width:100%;height:24%;font-size:32px}
#counter{position:absolute;left:8px;top:68%;font-size:28px}
video{position:absolute;left:0;top:68%;width:100%;height:32%;background:#000;object-fit:cover}
</style></head>
<body>
<input id="field" type="text">
<button id="btn" type="button">add</button>
<div id="counter">0</div>
<video id="v" src="silent-neuronte.mp4" muted playsinline preload="auto"></video>
<script>
let n = 0;
document.getElementById('btn').addEventListener('click', () => {
  n += 1;
  document.getElementById('counter').textContent = String(n);
});
</script>
</body></html>
"""

PLAYER_PAGE = """<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>player</title></head>
<body style="margin:0;background:#000">
<video id="player" src="silent-neuronte.mp4" autoplay muted playsinline preload="auto"></video>
</body></html>
"""

DELAY_PARENT = """<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>delay</title></head>
<body style="margin:0;background:#102030">
<script>
window.addPlayer = (src) => {
  const frame = document.createElement('iframe');
  frame.id = 'stage';
  frame.width = '640';
  frame.height = '480';
  frame.src = src;
  document.body.appendChild(frame);
};
window.swapPlayer = (src) => {
  const frame = document.getElementById('stage');
  frame.src = src;
};
</script>
</body></html>
"""

EMBED_PAGE = """<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>embed</title></head>
<body style="margin:0;background:#102030">
<iframe id="stage" src="%s" width="640" height="480"></iframe>
</body></html>
"""

AUTOPLAY_PAGE = """<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>autoplay</title></head>
<body>
<video id="early" src="silent-neuronte.mp4" autoplay playsinline preload="auto"></video>
<script>
setTimeout(() => {
  const video = document.createElement('video');
  video.id = 'late';
  video.autoplay = true;
  video.playsInline = true;
  video.preload = 'auto';
  video.src = 'silent-neuronte.mp4';
  document.body.appendChild(video);
}, 800);
</script>
</body></html>
"""


def fixture_video():
    raw = os.environ.get('ZELL_BROWSER_FIXTURE', '')
    if not raw:
        return None
    path = Path(raw)
    if path.is_dir():
        path = path / 'silent-neuronte.mp4'
    if path.is_file():
        return path
    return None


def installed(name):
    try:
        browser.launch_argv(name)
    except (FileNotFoundError, ValueError):
        return False
    return True


def live_reason(name):
    if shutil.which('Xvfb') is None:
        return 'Xvfb is not installed'
    if fixture_video() is None:
        return 'ZELL_BROWSER_FIXTURE is not set to silent-neuronte.mp4'
    if not installed(name):
        return f'{name} is not installed'
    return None


class QuietHandler(SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):
        return


class RedirectHandler(QuietHandler):
    def do_GET(self):
        path = self.path.split('?', 1)[0]
        if path == '/redirect':
            self.send_response(302)
            self.send_header('Location', '/video')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if path == '/video':
            self.path = '/index.html'
        super().do_GET()


class LocalPage:
    def __init__(self, root, handler=None):
        handler = handler or QuietHandler
        self.httpd = ThreadingHTTPServer(
            ('127.0.0.1', 0), lambda *args, **kwargs: handler(*args, directory=str(root), **kwargs))
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()
        self.origin = f'http://127.0.0.1:{self.httpd.server_address[1]}'
        self.url = f'{self.origin}/index.html'

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def differing_frames(page):
    first = None
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        frame = page.frame(0.5)
        if frame is None:
            continue
        width, height, rgb = frame
        if len(rgb) != width * height * 3:
            raise AssertionError('short frame')
        if first is None:
            first = frame
            continue
        if rgb != first[2]:
            changed = sum(left != right for left, right in zip(rgb, first[2]))
            if changed > 1000:
                return first, frame
    raise AssertionError('frames did not change')


def wait_playing(page, minimum=300):
    page.resume()
    deadline = time.monotonic() + 8
    last = None
    while time.monotonic() < deadline:
        last = page.media_state()
        if last['present'] and last['paused'] is False and (last['position_ms'] or 0) >= minimum:
            return last
        time.sleep(0.15)
    raise AssertionError(last)


def cmdline_contains(token):
    found = []
    for name in os.listdir('/proc'):
        if not name.isdigit():
            continue
        try:
            command = Path(f'/proc/{name}/cmdline').read_bytes().replace(b'\0', b' ')
        except OSError:
            continue
        if token.encode() in command:
            found.append(int(name))
    return found


class BrowserAdapterTests(unittest.TestCase):
    def test_launch_argv_never_substitutes(self):
        self.assertEqual(browser.launch_argv('chromium', lambda name: '/bin/chromium' if name == 'chromium' else None),
                         ['/bin/chromium'])
        with self.assertRaises(FileNotFoundError):
            browser.launch_argv('firefox-esr', lambda _name: '/bin/chromium' if _name == 'chromium' else None)
        self.assertEqual(browser.chrome_privilege_argv('google-chrome-stable_current_amd64.deb')[-1],
                         'google-chrome-stable_current_amd64.deb')
        self.assertNotIn('google-chrome-stable', browser.chrome_privilege_argv('local.deb')[:-1])
        self.assertEqual(browser.firefox_privilege_argv()[-1], 'firefox-esr')
        self.assertNotIn('field', browser._MEDIA_JS)
        self.assertNotIn('counter', browser._MEDIA_JS)
        self.assertNotIn('getElementById', browser._MEDIA_JS)
        self.assertIn('contentDocument', browser._MEDIA_JS)

    def test_frame_media_merges_the_embedded_player(self):
        parent = {'present': False, 'paused': None, 'position_ms': None, 'duration_ms': None}
        child = {'present': True, 'paused': True, 'position_ms': 2, 'duration_ms': 20000}
        self.assertEqual(browser._merge_media([parent, child]), {
            'paused': True, 'position_ms': 2, 'duration_ms': 20000, 'present': True,
        })
        playing = {'present': True, 'paused': False, 'position_ms': 500, 'duration_ms': 20000}
        merged = browser._merge_media([child, playing])
        self.assertFalse(merged['paused'])
        self.assertEqual(merged['position_ms'], 2)
        self.assertTrue(browser._same_url('http://127.0.0.1:9/player/', 'http://127.0.0.1:9/player'))
        self.assertFalse(browser._same_url('http://127.0.0.1:9/player', 'http://127.0.0.1:8/player'))

    def test_missing_browser_does_not_launch(self):
        page = browser.PrivateBrowser(session_type=FakeSession, which=lambda _name: None)
        with self.assertRaises(FileNotFoundError):
            page.launch('chrome', 'http://127.0.0.1/', 320, 200)
        self.assertEqual(page.launches, [])
        self.assertIsNone(page.session)

    def test_private_profile_flags_and_input_translation(self):
        def which(name):
            return f'/usr/bin/{name}'

        page = browser.PrivateBrowser(session_type=FakeSession, which=which)
        page.launch('chromium', 'http://127.0.0.1/a', 320, 200)
        argv = page.launches[0]
        self.assertEqual(argv[0], '/usr/bin/chromium')
        self.assertTrue(any(arg.startswith('--user-data-dir=') for arg in argv))
        self.assertNotIn('--profile', argv)
        self.assertTrue(page.session.kwargs['isolate_bus'])
        self.assertFalse(page.session.kwargs['clipboard'])
        self.assertNotIn('new-tab', argv)
        self.assertNotIn('--autoplay-policy=no-user-gesture-required', argv)
        self.assertEqual(argv[-1], 'about:blank')
        self.assertNotIn('http://127.0.0.1/a', argv)
        self.assertTrue(any(arg.startswith('--load-extension=') and arg.endswith('/zell-hold') for arg in argv))
        self.assertTrue(any(
            arg.startswith('--disable-extensions-except=') and arg.endswith('/zell-hold') for arg in argv))
        profile = next(arg.split('=', 1)[1] for arg in argv if arg.startswith('--user-data-dir='))
        self.assertTrue(profile.startswith(tempfile.gettempdir()))
        page.launch('firefox-esr', 'http://127.0.0.1/b', 320, 200)
        firefox = page.launches[-1]
        self.assertEqual(firefox[0], '/usr/bin/firefox-esr')
        self.assertIn('--profile', firefox)
        self.assertIn('--no-remote', firefox)
        self.assertEqual(firefox[-1], 'about:blank')
        self.assertNotIn('http://127.0.0.1/b', firefox)
        self.assertNotIn('--user-data-dir', ' '.join(firefox))
        page.launch('chrome', 'http://127.0.0.1/a', 320, 200)

        def comparable(command):
            prefixes = ('--user-data-dir=', '--load-extension=', '--disable-extensions-except=')
            return [arg for arg in command[1:] if not arg.startswith(prefixes)]

        self.assertEqual(comparable(page.launches[-1]), comparable(argv))
        self.assertTrue(page.launches[-1][0].endswith('google-chrome-stable'))
        stub = FakeInjector()

        class Session(FakeSession):
            def make_injector(self):
                return stub

        page.session = Session()
        self.assertTrue(page.key('a', modifiers=5))
        self.assertEqual(stub.calls[:2], [('chord', 'a', 5, 1), ('chord', 'a', 0, 3)])
        self.assertTrue(page.key('Enter', text='hi'))
        self.assertEqual(stub.calls[-1], ('paste', 'hi'))
        with self.assertRaises(ValueError):
            page.key('Nope')
        with self.assertRaises(ValueError):
            page.key('a', modifiers=-1)
        with self.assertRaises(ValueError):
            page.key('a', modifiers=16)
        page.pointer(3, 4, 1, 'down')
        self.assertEqual(stub.calls[-1], ('pointer', 3, 4, 1, True))
        same = page.session
        page.popout()
        page.dock()
        self.assertIs(page.session, same)
        self.assertFalse(page.popped)
        page.close()
        self.assertIsNone(page.session)
        self.assertFalse(Path(profile).exists())

    def test_raw_capture_assembles_one_frame_and_reaper_checks_start_time(self):
        read_fd, write_fd = os.pipe()
        os.set_blocking(read_fd, False)
        payload = bytes(range(12))
        pending = bytearray()

        def produce():
            os.write(write_fd, payload[:5])
            time.sleep(0.15)
            os.write(write_fd, payload[5:])

        thread = threading.Thread(target=produce)
        thread.start()
        frame = browser._read_raw_frame(read_fd, 2, 2, 2, pending)
        thread.join()
        self.assertEqual(frame, (2, 2, payload))
        self.assertIsNone(browser._read_raw_frame(read_fd, 2, 2, 0.05, pending))
        os.close(read_fd)
        os.close(write_fd)

        proc = subprocess.Popen(['sleep', '60'], start_new_session=True)
        record_dir = None
        try:
            start = browser._proc_start(proc.pid)
            record_dir = Path(tempfile.mkdtemp(prefix='zell-theater-record-'))
            record = record_dir / 'pids'
            record.write_text(f'{proc.pid} 1\n', encoding='ascii')
            browser._kill_recorded(str(record))
            self.assertIsNone(proc.poll())
            record.write_text(f'{proc.pid} {start}\n', encoding='ascii')
            browser._kill_recorded(str(record))
            self.assertIsNotNone(proc.wait(timeout=3))
        finally:
            if proc.poll() is None:
                proc.kill()
            if record_dir is not None:
                shutil.rmtree(record_dir, ignore_errors=True)

    def test_installed_browser_frames_input_and_pause(self):
        for name in ('chromium', 'firefox-esr'):
            reason = live_reason(name)
            with self.subTest(browser=name):
                if reason:
                    self.skipTest(reason)
                self._exercise(name)

    def test_pause_switch_resume_keeps_each_playhead(self):
        missing = [name for name in ('chromium', 'firefox-esr') if live_reason(name)]
        if missing:
            self.skipTest(', '.join(missing))
        with tempfile.TemporaryDirectory(prefix='zell-browser-page-') as root:
            root_path = Path(root)
            (root_path / 'index.html').write_text(PAGE, encoding='utf-8')
            (root_path / 'silent-neuronte.mp4').write_bytes(fixture_video().read_bytes())
            site = LocalPage(root_path)
            first = browser.PrivateBrowser()
            second = browser.PrivateBrowser()
            try:
                first.launch('chromium', site.url, 640, 480)
                second.launch('firefox-esr', site.url, 640, 480)
                self._reach_page(first)
                self._reach_page(second)
                played = wait_playing(first)
                held = first.pause()
                self.assertTrue(held['paused'])
                self.assertEqual(held['position_ms'], first.media_state()['position_ms'])
                self.assertGreaterEqual(held['position_ms'], played['position_ms'])
                other = wait_playing(second)
                time.sleep(1.0)
                stayed = first.media_state()
                self.assertTrue(stayed['paused'])
                self.assertEqual(stayed['position_ms'], held['position_ms'])
                second_held = second.pause()
                self.assertTrue(second_held['paused'])
                resumed = wait_playing(first, held['position_ms'] + 250)
                time.sleep(0.8)
                quiet = second.media_state()
                self.assertTrue(quiet['paused'])
                self.assertEqual(quiet['position_ms'], second_held['position_ms'])
                self.assertGreaterEqual(other['position_ms'], 300)
            finally:
                first.close()
                second.close()
                site.close()

    def test_inactive_open_holds_autoplay_and_delayed_media(self):
        for name in ('chromium', 'firefox-esr'):
            reason = live_reason(name)
            with self.subTest(browser=name):
                if reason:
                    self.skipTest(reason)
                self._hold_inactive(name)

    def test_redirect_lands_held_on_the_destination(self):
        for name in ('chromium', 'firefox-esr'):
            reason = live_reason(name)
            with self.subTest(browser=name):
                if reason:
                    self.skipTest(reason)
                self._hold_redirect(name)

    def test_delayed_cross_origin_player_inherits_active_policy(self):
        missing = [name for name in ('chromium', 'firefox-esr') if live_reason(name)]
        if missing:
            self.skipTest(', '.join(missing))
        for name in ('chromium', 'firefox-esr'):
            with self.subTest(browser=name):
                self._delayed_cross_origin(name)

    def test_embedded_player_follows_the_hold(self):
        missing = [name for name in ('chromium', 'firefox-esr') if live_reason(name)]
        if missing:
            self.skipTest(', '.join(missing))
        for name in ('chromium', 'firefox-esr'):
            for cross_origin in (False, True):
                with self.subTest(browser=name, cross_origin=cross_origin):
                    self._embedded_player(name, cross_origin)

    def test_parent_death_reaps_the_private_display(self):
        reason = live_reason('chromium')
        if reason:
            self.skipTest(reason)
        tools = str(Path(browser.__file__).resolve().parent)
        with tempfile.TemporaryDirectory(prefix='zell-browser-death-') as root:
            root_path = Path(root)
            (root_path / 'index.html').write_text(PAGE, encoding='utf-8')
            (root_path / 'silent-neuronte.mp4').write_bytes(fixture_video().read_bytes())
            site = LocalPage(root_path)
            manifest = root_path / 'pids'
            ready = root_path / 'ready'
            child = r'''
import os, sys
sys.path.insert(0, os.environ["ZELL_TOOLS"])
import zell_theater_browser as adapter
page = adapter.PrivateBrowser()
page.launch(os.environ["ZELL_BROWSER"], os.environ["ZELL_URL"], 640, 480)
reaper = page._reaper.pid
lines = [f"{reaper} {adapter._proc_start(reaper)}", f"profile {page._profile}"]
lines.extend(open(page._record, encoding="ascii").read().splitlines())
handle = open(os.environ["ZELL_PIDS"], "w", encoding="ascii")
handle.write("\n".join(lines) + "\n")
handle.flush()
os.fsync(handle.fileno())
handle.close()
open(os.environ["ZELL_READY"], "w", encoding="ascii").close()
os.kill(os.getpid(), 9)
'''
            env = dict(os.environ)
            env.update(ZELL_TOOLS=tools, ZELL_BROWSER='chromium', ZELL_URL=site.url,
                       ZELL_PIDS=str(manifest), ZELL_READY=str(ready))
            proc = subprocess.Popen([sys.executable, '-c', child], env=env, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic() + 60
                while time.monotonic() < deadline and not ready.exists():
                    if proc.poll() is not None:
                        raise AssertionError(proc.stderr.read().decode('utf-8', 'replace'))
                    time.sleep(0.2)
                self.assertTrue(ready.exists(), 'browser launch did not become ready')
                proc.wait(timeout=10)
                text = manifest.read_text(encoding='ascii')
                rows = []
                profile = None
                for line in text.splitlines():
                    kind, value = line.split(' ', 1)
                    if kind == 'profile':
                        profile = value
                    else:
                        rows.append((int(kind), value))
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    if all(not browser._same_proc(pid, start) for pid, start in rows):
                        break
                    time.sleep(0.2)
                alive = [(pid, start) for pid, start in rows if browser._same_proc(pid, start)]
                self.assertEqual(alive, [])
                self.assertIsNotNone(profile)
                self.assertFalse(Path(profile).exists())
                self.assertEqual(cmdline_contains(profile), [])
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=5)
                if proc.stderr is not None:
                    proc.stderr.close()
                site.close()

    def _exercise(self, name):
        with tempfile.TemporaryDirectory(prefix='zell-browser-page-') as root:
            root_path = Path(root)
            (root_path / 'index.html').write_text(PAGE, encoding='utf-8')
            (root_path / 'silent-neuronte.mp4').write_bytes(fixture_video().read_bytes())
            site = LocalPage(root_path)
            page = browser.PrivateBrowser()
            try:
                session = page.launch(name, site.url, 640, 480)
                self._reach_page(page)
                played = wait_playing(page)
                differing_frames(page)
                self.assertEqual(played['duration_ms'], 20000)
                held = page.pause()
                self.assertTrue(held['paused'])
                self.assertGreaterEqual(held['position_ms'], 300)
                time.sleep(1.0)
                stayed = page.media_state()
                self.assertTrue(stayed['paused'])
                self.assertEqual(stayed['position_ms'], held['position_ms'])
                same = page.session
                page.popout()
                page.dock()
                self.assertIs(page.session, same)
                self.assertEqual(page.viewport, (640, 480))
                self.assertFalse(page.popped)
                after = page.media_state()
                self.assertEqual(after['position_ms'], held['position_ms'])
                recorded = self._recorded(page)
                profile = page._profile
                reaper = page._reaper.pid
                reaper_start = browser._proc_start(reaper)
                page.close()
                self.assertIsNone(page.session)
                self.assertTrue(getattr(session, '_closed', False))
                self.assertFalse(Path(profile).exists())
                self.assertFalse(browser._same_proc(reaper, reaper_start))
                for pid, start in recorded:
                    self.assertFalse(browser._same_proc(pid, start))
                self.assertEqual(cmdline_contains(profile), [])
            finally:
                page.close()
                site.close()

    def _hold_inactive(self, name):
        with tempfile.TemporaryDirectory(prefix='zell-browser-autoplay-') as root:
            root_path = Path(root)
            (root_path / 'index.html').write_text(AUTOPLAY_PAGE, encoding='utf-8')
            (root_path / 'silent-neuronte.mp4').write_bytes(fixture_video().read_bytes())
            site = LocalPage(root_path)
            page = browser.PrivateBrowser()
            try:
                page.launch(name, site.url, 640, 480)
                early = self._wait_video(page, 'early')
                self.assertLessEqual(early['position_ms'], 300)
                late = self._wait_video(page, 'late')
                self.assertLessEqual(late['position_ms'], 300)
                state = page.media_state()
                self.assertTrue(state['present'])
                self.assertTrue(state['paused'])
                self.assertLessEqual(state['position_ms'], 300)
                time.sleep(1.0)
                again_early = self._video(page, 'early')
                again_late = self._video(page, 'late')
                self.assertTrue(again_early['paused'])
                self.assertTrue(again_late['paused'])
                self.assertEqual(again_early['position_ms'], early['position_ms'])
                self.assertEqual(again_late['position_ms'], late['position_ms'])
                stayed = page.media_state()
                self.assertTrue(stayed['paused'])
                self.assertEqual(stayed['position_ms'], state['position_ms'])
                public = set(stayed)
                self.assertEqual(public, {'paused', 'position_ms', 'duration_ms', 'present'})
            finally:
                page.close()
                site.close()

    def _hold_redirect(self, name):
        with tempfile.TemporaryDirectory(prefix='zell-browser-redirect-') as root:
            root_path = Path(root)
            (root_path / 'index.html').write_text(AUTOPLAY_PAGE, encoding='utf-8')
            (root_path / 'silent-neuronte.mp4').write_bytes(fixture_video().read_bytes())
            site = LocalPage(root_path, RedirectHandler)
            page = browser.PrivateBrowser()
            try:
                page.launch(name, f'{site.origin}/redirect', 640, 480)
                self.assertEqual(page.url, f'{site.origin}/redirect')
                landed = page._page_value('location.href')
                self.assertTrue(str(landed).endswith('/video'), landed)
                early = self._wait_video(page, 'early')
                self.assertLessEqual(early['position_ms'], 300)
                late = self._wait_video(page, 'late')
                self.assertLessEqual(late['position_ms'], 300)
                state = page.media_state()
                self.assertTrue(state['paused'])
                self.assertLessEqual(state['position_ms'] or 0, 300)
                time.sleep(1.0)
                stayed = page.media_state()
                self.assertTrue(stayed['paused'])
                self.assertEqual(stayed['position_ms'], state['position_ms'])
                self.assertEqual(self._video(page, 'late')['position_ms'], late['position_ms'])
            finally:
                page.close()
                site.close()

    def _delayed_cross_origin(self, name):
        with tempfile.TemporaryDirectory(prefix='zell-browser-delay-') as root:
            root_path = Path(root)
            player = root_path / 'player'
            parent = root_path / 'parent'
            player.mkdir()
            parent.mkdir()
            (player / 'silent-neuronte.mp4').write_bytes(fixture_video().read_bytes())
            (player / 'active.html').write_text(PLAYER_PAGE, encoding='utf-8')
            (player / 'later.html').write_text(PLAYER_PAGE, encoding='utf-8')
            (parent / 'index.html').write_text(DELAY_PARENT, encoding='utf-8')
            player_site = LocalPage(player)
            site = LocalPage(parent)
            page = browser.PrivateBrowser()
            try:
                page.launch(name, site.url, 640, 480)
                self.assertFalse(page.media_state()['present'])
                opened = page.resume()
                self.assertFalse(opened['present'])
                active = f'{player_site.origin}/active.html'
                page._page_value('addPlayer(%s)' % json.dumps(active))
                played = self._wait_advanced(page)
                self.assertGreaterEqual(played['position_ms'], 300)
                self.assertEqual(played['duration_ms'], 20000)
                self.assertEqual(
                    page._page_value('document.querySelectorAll("video, audio").length'), 0)
                paused = page.pause()
                self.assertTrue(paused['paused'])
                self.assertGreaterEqual(paused['position_ms'], 300)
                time.sleep(1.0)
                stayed = page.media_state()
                self.assertTrue(stayed['paused'])
                self.assertEqual(stayed['position_ms'], paused['position_ms'])
                later = f'{player_site.origin}/later.html'
                page._page_value('swapPlayer(%s)' % json.dumps(later))
                held = self._wait_reloaded_hold(page, later)
                self.assertLessEqual(held['position_ms'], 300)
                self.assertEqual(held['duration_ms'], 20000)
                time.sleep(1.0)
                quiet = page.media_state()
                self.assertTrue(quiet['paused'])
                self.assertEqual(quiet['position_ms'], held['position_ms'])
                self.assertEqual(set(quiet), {'paused', 'position_ms', 'duration_ms', 'present'})
            finally:
                page.close()
                site.close()
                player_site.close()

    def _wait_advanced(self, page, minimum=300):
        deadline = time.monotonic() + 12
        last = None
        while time.monotonic() < deadline:
            last = page.media_state()
            if last['present'] and last['paused'] is False and (last['position_ms'] or 0) >= minimum:
                return last
            time.sleep(0.15)
        raise AssertionError(last)

    def _wait_reloaded_hold(self, page, url):
        deadline = time.monotonic() + 12
        last = None
        while time.monotonic() < deadline:
            last = page.media_state()
            src = page._page_value(
                '(document.getElementById("stage") || {}).src || ""')
            if (str(src) == url and last['present'] and last['paused'] is True
                    and last['duration_ms'] == 20000 and last['position_ms'] is not None
                    and last['position_ms'] <= 300):
                return last
            time.sleep(0.15)
        raise AssertionError((last, url))

    def _embedded_player(self, name, cross_origin):
        with tempfile.TemporaryDirectory(prefix='zell-browser-embed-') as root:
            root_path = Path(root)
            player = root_path / 'player'
            player.mkdir()
            (player / 'silent-neuronte.mp4').write_bytes(fixture_video().read_bytes())
            player_site = None
            if cross_origin:
                (player / 'index.html').write_text(PLAYER_PAGE, encoding='utf-8')
                player_site = LocalPage(player)
                parent = root_path / 'parent'
                parent.mkdir()
                (parent / 'index.html').write_text(
                    EMBED_PAGE % f'{player_site.origin}/index.html', encoding='utf-8')
                site = LocalPage(parent)
            else:
                (player / 'player.html').write_text(PLAYER_PAGE, encoding='utf-8')
                (player / 'index.html').write_text(EMBED_PAGE % 'player.html', encoding='utf-8')
                site = LocalPage(player)
            page = browser.PrivateBrowser()
            try:
                page.launch(name, site.url, 640, 480)
                self.assertEqual(page._page_value('document.querySelectorAll("video, audio").length'), 0)
                self.assertEqual(page._page_value('document.querySelectorAll("iframe, frame").length'), 1)
                held = self._wait_held_player(page)
                time.sleep(1.0)
                stayed = page.media_state()
                self.assertTrue(stayed['paused'])
                self.assertEqual(stayed['position_ms'], held['position_ms'])
                self.assertLessEqual(stayed['position_ms'], 300)
                if not cross_origin:
                    child = self._frame_video(page)
                    self.assertTrue(child['paused'])
                    self.assertEqual(child['position_ms'], stayed['position_ms'])
                played = wait_playing(page, max(300, (held['position_ms'] or 0) + 250))
                self.assertFalse(played['paused'])
                self.assertGreaterEqual(played['position_ms'], 300)
                if not cross_origin:
                    child = self._frame_video(page)
                    self.assertFalse(child['paused'])
                    self.assertGreaterEqual(child['position_ms'], 300)
                paused = page.pause()
                self.assertTrue(paused['paused'])
                time.sleep(1.0)
                quiet = page.media_state()
                self.assertTrue(quiet['paused'])
                self.assertEqual(quiet['position_ms'], paused['position_ms'])
                self.assertEqual(set(quiet), {'paused', 'position_ms', 'duration_ms', 'present'})
                if not cross_origin:
                    child = self._frame_video(page)
                    self.assertTrue(child['paused'])
                    self.assertEqual(child['position_ms'], quiet['position_ms'])
            finally:
                page.close()
                site.close()
                if player_site is not None:
                    player_site.close()

    def _wait_held_player(self, page):
        deadline = time.monotonic() + 12
        last = None
        while time.monotonic() < deadline:
            last = page.media_state()
            if (last['present'] and last['paused'] is True and last['duration_ms'] == 20000
                    and last['position_ms'] is not None and last['position_ms'] <= 300):
                return last
            time.sleep(0.15)
        raise AssertionError(last)

    def _frame_video(self, page):
        return page._page_value(
            '(() => {'
            ' const frame = document.getElementById("stage");'
            ' const doc = frame && frame.contentDocument;'
            ' const node = doc && doc.getElementById("player");'
            ' if (!node) return null;'
            ' const position = Number.isFinite(node.currentTime) ? Math.round(node.currentTime * 1000) : null;'
            ' return {paused: !!node.paused, position_ms: position};'
            '})()'
        )

    def _video(self, page, ident):
        return page._page_value(
            '(() => { const node = document.getElementById(%s);'
            ' if (!node) return null;'
            ' const position = Number.isFinite(node.currentTime) ? Math.round(node.currentTime * 1000) : null;'
            ' return {paused: !!node.paused, position_ms: position}; })()' % json.dumps(ident)
        )

    def _wait_video(self, page, ident):
        deadline = time.monotonic() + 8
        last = None
        while time.monotonic() < deadline:
            last = self._video(page, ident)
            if last and last['paused'] and last['position_ms'] is not None:
                return last
            time.sleep(0.15)
        raise AssertionError(last)

    def _reach_page(self, page):
        deadline = time.monotonic() + 15
        last = None
        while time.monotonic() < deadline:
            last = page.media_state()
            if last['present'] and last['duration_ms'] == 20000:
                break
            time.sleep(0.2)
        else:
            raise AssertionError(last)
        page.pointer(120, 80, 1, 'down')
        page.pointer(120, 80, 1, 'up')
        time.sleep(0.2)
        page.key('a')
        page.key('b')
        time.sleep(0.2)
        page.pointer(120, 250, 1, 'down')
        page.pointer(120, 250, 1, 'up')
        time.sleep(0.4)
        sample = page._page_value(
            "({field: (document.getElementById('field') || {}).value || '',"
            " counter: (document.getElementById('counter') || {}).textContent || ''})"
        )
        self.assertEqual(sample['field'], 'ab', sample)
        self.assertEqual(sample['counter'], '1', sample)

    def _recorded(self, page):
        rows = []
        text = Path(page._record).read_text(encoding='ascii')
        for line in text.splitlines():
            pid_text, start = line.split()
            rows.append((int(pid_text), start))
        return rows


if __name__ == '__main__':
    unittest.main()
