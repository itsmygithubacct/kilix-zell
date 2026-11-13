"""Selected browser on a Kilix private display. No external tab.

One PrivateBrowser is one URL item. The capture viewport stays at the size
passed to launch. Popout and dock keep that same session and page. The
orchestrator scales the fixed RGB frame. Launch installs a document-start
hold before the URL loads. Pause and resume talk to the page's media
elements through the browser's own control API, and the hold keeps later
elements paused until resume.
"""
import atexit
import base64
import json
import os
import select
import shutil
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time

BROWSERS = ('chrome', 'chromium', 'firefox-esr')
CHROME_DEB_NAME = 'google-chrome-stable_current_amd64.deb'
_MOD_MASK = 1 | 2 | 4 | 8 | 64 | 128
_KEY_NAMES = {
    'Enter', 'Escape', 'Backspace', 'Tab',
    'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight',
    'Home', 'End', 'PageUp', 'PageDown', 'Insert', 'Delete',
    *(f'F{number}' for number in range(1, 13)),
}
_PROFILES = set()
_SESSIONS = 0
_HOLD_JS = r"""
(() => {
  if (window.__zellTheaterHoldInstalled) return;
  window.__zellTheaterHoldInstalled = true;
  const key = 'zell-theater-hold';
  function held() {
    const root = document.documentElement;
    if (root) {
      const mark = root.getAttribute('data-zell-hold-state');
      if (mark === '0') return false;
      if (mark === '1') return true;
    }
    try {
      const stored = sessionStorage.getItem(key);
      if (stored === '0') return false;
      if (stored === '1') return true;
    } catch (error) {}
    return true;
  }
  function pauseNode(node) {
    if (!node || node.paused || typeof node.pause !== 'function') return;
    const tag = node.tagName;
    if (tag !== 'VIDEO' && tag !== 'AUDIO') return;
    try { node.pause(); } catch (error) {}
  }
  function sweep() {
    if (!held()) return;
    const nodes = document.querySelectorAll('video, audio');
    for (let index = 0; index < nodes.length; index += 1) pauseNode(nodes[index]);
  }
  document.addEventListener('play', (event) => {
    if (held()) pauseNode(event.target);
  }, true);
  let armed = false;
  function arm() {
    const root = document.documentElement;
    if (root && root.getAttribute('data-zell-hold') !== '1')
      root.setAttribute('data-zell-hold', '1');
    if (!root || armed) return;
    armed = true;
    new MutationObserver(sweep).observe(root, {childList: true, subtree: true});
  }
  arm();
  try {
    new MutationObserver(arm).observe(document, {childList: true, subtree: true});
  } catch (error) {}
  setInterval(() => { arm(); sweep(); }, 200);
})();
"""
_HOLD_MANIFEST = """{
  "manifest_version": 3,
  "name": "zell-theater-hold",
  "version": "1.0",
  "host_permissions": ["<all_urls>"],
  "content_scripts": [{
    "matches": ["<all_urls>"],
    "match_about_blank": true,
    "all_frames": true,
    "run_at": "document_start",
    "js": ["hold.js"]
  }]
}
"""
_MEDIA_JS = r"""
(() => {
  const action = %s;
  const key = 'zell-theater-hold';
  if (action === 'pause' || action === 'play') {
    const hold = action === 'pause';
    window.__zellTheaterHold = hold;
    try { sessionStorage.setItem(key, hold ? '1' : '0'); } catch (error) {}
    const root = document.documentElement;
    if (root) root.setAttribute('data-zell-hold-state', hold ? '1' : '0');
  }
  const nodes = Array.from(document.querySelectorAll('video, audio'));
  for (const node of nodes) {
    if (action === 'pause') node.pause();
    if (action === 'play') {
      const pending = node.play();
      if (pending && pending.catch) pending.catch(() => {});
    }
  }
  const videos = nodes.filter((node) => node.tagName === 'VIDEO');
  const primary = videos.length ? videos[0] : null;
  const millis = (value) => Number.isFinite(value) ? Math.round(value * 1000) : null;
  return {
    present: !!primary,
    paused: primary ? videos.every((node) => node.paused) : null,
    position_ms: primary ? millis(primary.currentTime) : null,
    duration_ms: primary ? millis(primary.duration) : null
  };
})()
"""


def launch_argv(browser, which=None):
    """Argv for one selected browser. Never substitutes another browser."""
    find = which or shutil.which
    if browser == 'chromium':
        binary = find('chromium') or find('chromium-browser')
        if binary is None:
            raise FileNotFoundError('chromium is not installed')
        return [binary]
    if browser == 'chrome':
        binary = find('google-chrome-stable') or find('google-chrome')
        if binary is None:
            raise FileNotFoundError('chrome is not installed')
        return [binary]
    if browser == 'firefox-esr':
        binary = find('firefox-esr')
        if binary is None:
            raise FileNotFoundError('firefox-esr is not installed')
        return [binary]
    raise ValueError('unknown browser')


def chrome_privilege_argv(deb_path):
    """Install the official local deb. Not the bare apt package name."""
    return ['apt-get', 'install', '-y', os.fspath(deb_path)]


def firefox_privilege_argv():
    return ['apt-get', 'install', '-y', 'firefox-esr']


def _kilix_config():
    """Installed Kilix config directory that contains kilix_sdk."""
    candidates = []
    home = os.environ.get('KILIX_HOME')
    if home:
        candidates.append(os.path.join(os.path.abspath(os.path.expanduser(home)), 'config'))
    source = os.environ.get('GPU_TERMINAL_SOURCE_HOME')
    if source:
        candidates.append(os.path.join(
            os.path.abspath(os.path.expanduser(source)), 'kilix', 'config'))
    program = shutil.which('kilix')
    if program:
        candidates.append(os.path.join(os.path.dirname(os.path.realpath(program)), 'config'))
    seen = []
    for path in candidates:
        if path in seen:
            continue
        seen.append(path)
        if os.path.isfile(os.path.join(path, 'kilix_sdk', 'xapp.py')):
            return path
    raise RuntimeError('kilix_sdk is not installed')


def _load_session_type():
    try:
        from kilix_sdk.xapp import XAppSession
        return XAppSession
    except ImportError:
        config = _kilix_config()
        if config not in sys.path:
            sys.path.insert(0, config)
        from kilix_sdk.xapp import XAppSession
        return XAppSession


def _proc_start(pid):
    try:
        with open(f'/proc/{int(pid)}/stat', encoding='ascii') as handle:
            fields = handle.read().rsplit(') ', 1)[1].split()
        return fields[19]
    except (IndexError, OSError, ValueError):
        return None


def _same_proc(pid, start):
    return start is not None and _proc_start(pid) == start


def _children(pid):
    found = []
    try:
        names = os.listdir('/proc')
    except OSError:
        return found
    for name in names:
        if not name.isdigit():
            continue
        child = int(name)
        try:
            with open(f'/proc/{child}/stat', encoding='ascii') as handle:
                text = handle.read()
            ppid = int(text.rsplit(') ', 1)[1].split()[1])
        except (IndexError, OSError, ValueError):
            continue
        if ppid == pid:
            found.append(child)
    return found


def _descendants(pid):
    ordered = []
    for child in _children(pid):
        ordered.extend(_descendants(child))
        ordered.append(child)
    return ordered


def _kill_recorded(record):
    """Stop processes whose pid and start time still match the record."""
    if not record:
        return
    try:
        with open(record, encoding='ascii') as handle:
            lines = handle.read().splitlines()
    except OSError:
        return
    found = []
    seen = set()
    for line in lines:
        if not line.strip():
            continue
        pid_text, start = line.split()
        pid = int(pid_text)
        if not _same_proc(pid, start):
            continue
        for candidate in [*_descendants(pid), pid]:
            if candidate in seen or candidate <= 1 or candidate == os.getpid():
                continue
            candidate_start = _proc_start(candidate)
            if candidate_start is None:
                continue
            seen.add(candidate)
            found.append((candidate, candidate_start))
    for pid, start in found:
        if _same_proc(pid, start):
            try:
                os.kill(pid, signal.SIGTERM)
            except OSError:
                pass
    deadline = time.monotonic() + 1.5
    for pid, start in found:
        while time.monotonic() < deadline and _same_proc(pid, start):
            time.sleep(0.05)
        if _same_proc(pid, start):
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass


def _remove_profile(path):
    if not path or not os.path.basename(path).startswith('zell-theater-'):
        return
    try:
        info = os.lstat(path)
    except OSError:
        return
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
        return
    if info.st_uid != os.geteuid():
        return
    shutil.rmtree(path, ignore_errors=True)
    _PROFILES.discard(path)


def _cleanup_profiles():
    for path in list(_PROFILES):
        _remove_profile(path)


atexit.register(_cleanup_profiles)


def _reap_main(argv):
    if len(argv) != 4:
        raise SystemExit(2)
    parent = int(argv[0])
    parent_start = argv[1]
    record = argv[2]
    profile = argv[3]

    def _stop(_signum, _frame):
        os._exit(0)

    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)
    while _same_proc(parent, parent_start):
        time.sleep(0.2)
    _kill_recorded(record)
    _remove_profile(profile)


def _free_port():
    sock = socket.socket()
    try:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]
    finally:
        sock.close()


def _read_http_json(port, path, timeout):
    sock = socket.create_connection(('127.0.0.1', port), timeout)
    try:
        sock.settimeout(timeout)
        request = (
            f'GET {path} HTTP/1.1\r\n'
            f'Host: 127.0.0.1:{port}\r\n'
            'Connection: close\r\n\r\n'
        )
        sock.sendall(request.encode())
        data = b''
        while b'\r\n\r\n' not in data:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data += chunk
            if len(data) > 1024 * 1024:
                raise RuntimeError('browser control header is too large')
        head, body = data.split(b'\r\n\r\n', 1)
        status = head.split(b'\r\n', 1)[0]
        if b' 200 ' not in status:
            raise RuntimeError(f'browser control HTTP failed: {status.decode("latin1", "replace")}')
        length = None
        for line in head.decode('latin1').split('\r\n')[1:]:
            if line.lower().startswith('content-length:'):
                length = int(line.split(':', 1)[1].strip())
        if length is None or length < 0 or length > 8 * 1024 * 1024:
            raise RuntimeError('browser control response has no usable length')
        while len(body) < length:
            chunk = sock.recv(length - len(body))
            if not chunk:
                break
            body += chunk
        if len(body) < length:
            raise RuntimeError('browser control response was truncated')
        return json.loads(body[:length])
    finally:
        sock.close()


def _ws_send(sock, text):
    payload = text.encode()
    mask = os.urandom(4)
    header = bytearray([0x81])
    length = len(payload)
    if length < 126:
        header.append(0x80 | length)
    elif length < 65536:
        header.append(0x80 | 126)
        header.extend(struct.pack('!H', length))
    else:
        header.append(0x80 | 127)
        header.extend(struct.pack('!Q', length))
    header.extend(mask)
    masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
    sock.sendall(bytes(header) + masked)


def _ws_recv(sock):
    def exact(count):
        buf = b''
        while len(buf) < count:
            chunk = sock.recv(count - len(buf))
            if not chunk:
                raise RuntimeError('browser control closed')
            buf += chunk
        return buf

    parts = []
    kind = None
    while True:
        first, second = exact(2)
        opcode = first & 0x0F
        fin = bool(first & 0x80)
        length = second & 0x7F
        if length == 126:
            length = struct.unpack('!H', exact(2))[0]
        elif length == 127:
            length = struct.unpack('!Q', exact(8))[0]
        if length > 8 * 1024 * 1024:
            raise RuntimeError('browser control frame is too large')
        if second & 0x80:
            raise RuntimeError('browser control sent a masked frame')
        payload = exact(length)
        if opcode == 8:
            raise RuntimeError('browser control closed')
        if opcode == 9:
            _ws_send_pong(sock, payload)
            continue
        if opcode in (0, 1):
            if opcode == 1:
                kind = 1
                parts = [payload]
            else:
                parts.append(payload)
            if fin and kind == 1:
                return b''.join(parts).decode()
            continue
        if fin and opcode == 10:
            continue


def _ws_send_pong(sock, payload):
    header = bytearray([0x8A])
    mask = os.urandom(4)
    length = len(payload)
    if length < 126:
        header.append(0x80 | length)
    else:
        raise RuntimeError('browser control ping is too large')
    header.extend(mask)
    masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
    sock.sendall(bytes(header) + masked)


def _ws_connect(port, path, timeout):
    sock = socket.create_connection(('127.0.0.1', port), timeout)
    sock.settimeout(timeout)
    key = base64.b64encode(os.urandom(16)).decode()
    request = (
        f'GET {path} HTTP/1.1\r\n'
        f'Host: 127.0.0.1:{port}\r\n'
        'Upgrade: websocket\r\n'
        'Connection: Upgrade\r\n'
        f'Sec-WebSocket-Key: {key}\r\n'
        'Sec-WebSocket-Version: 13\r\n\r\n'
    )
    sock.sendall(request.encode())
    data = b''
    while b'\r\n\r\n' not in data:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
    status = data.split(b'\r\n', 1)[0]
    if b' 101 ' not in status:
        sock.close()
        raise RuntimeError('browser control websocket was refused')
    return sock


def _normalize_media(value):
    if isinstance(value, dict) and 'present' not in value and set(value) == {'value'}:
        value = value['value']
    if not isinstance(value, dict) or 'present' not in value:
        raise RuntimeError('browser media state is unreadable')
    present = bool(value.get('present'))
    if not present:
        return {
            'paused': None, 'position_ms': None, 'duration_ms': None, 'present': False,
        }
    paused = value.get('paused')
    if not isinstance(paused, bool):
        paused = None

    def millis(name):
        number = value.get(name)
        if isinstance(number, bool) or not isinstance(number, int) or number < 0:
            return None
        return number

    return {
        'paused': paused,
        'position_ms': millis('position_ms'),
        'duration_ms': millis('duration_ms'),
        'present': True,
    }


def _public_media(state):
    return {key: state[key] for key in ('paused', 'position_ms', 'duration_ms', 'present')}


def _read_raw_frame(fd, width, height, timeout, pending):
    need = width * height * 3
    deadline = time.monotonic() + timeout
    while len(pending) < need:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return None
        ready, _, _ = select.select([fd], [], [], remaining)
        if not ready:
            return None
        try:
            chunk = os.read(fd, need - len(pending))
        except BlockingIOError:
            continue
        if not chunk:
            return None
        pending.extend(chunk)
    frame = bytes(pending[:need])
    del pending[:need]
    return width, height, frame


def _write_firefox_prefs(profile, port):
    path = os.path.join(profile, 'user.js')
    text = (
        f'user_pref("marionette.port", {int(port)});\n'
        'user_pref("browser.shell.checkDefaultBrowser", false);\n'
        'user_pref("datareporting.policy.dataSubmissionEnabled", false);\n'
        'user_pref("browser.aboutwelcome.enabled", false);\n'
        'user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);\n'
        'user_pref("browser.sessionstore.resume_from_crash", false);\n'
        # Marionette play() is not a user gesture, so audible resume needs
        # autoplay allowed. The document-start hold keeps an inactive page paused.
        'user_pref("media.autoplay.default", 0);\n'
        'user_pref("media.autoplay.blocking_policy", 0);\n'
        'user_pref("layout.css.devPixelsPerPx", "1.0");\n'
    )
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w', encoding='ascii') as handle:
        handle.write(text)


def _write_private(path, text):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w', encoding='utf-8') as handle:
        handle.write(text)


def _write_hold_extension(profile):
    root = os.path.join(profile, 'zell-hold')
    os.makedirs(root, mode=0o700, exist_ok=True)
    os.chmod(root, 0o700)
    _write_private(os.path.join(root, 'manifest.json'), _HOLD_MANIFEST)
    _write_private(os.path.join(root, 'hold.js'), _HOLD_JS)
    return root


def _log_tail(path):
    try:
        with open(path, 'rb') as handle:
            handle.seek(0, os.SEEK_END)
            size = handle.tell()
            handle.seek(max(0, size - 2000))
            return handle.read().decode('utf-8', 'replace')
    except OSError:
        return ''


class PrivateBrowser:
    """One XAppSession for one url. Popout keeps this object."""

    def __init__(self, session_type=None, privilege=None, which=None):
        self.session_type = session_type
        self.privilege = privilege
        self.which = which
        self.session = None
        self.browser = None
        self.url = None
        self.popped = False
        self.launches = []
        self.viewport = None
        self._profile = None
        self._record = None
        self._reaper = None
        self._control = None
        self._control_kind = None
        self._message = 0
        self._initial_frame = None
        self._raw_pending = bytearray()
        self._log_path = None
        self._debug_port = None
        self._hold_installed = False
        self._lock = threading.RLock()

    def launch(self, browser, url, width, height):
        if browser not in BROWSERS:
            raise ValueError('unknown browser')
        binary = launch_argv(browser, self.which)
        width, height = int(width), int(height)
        if width <= 0 or height <= 0:
            raise ValueError('browser size must be positive')
        if not isinstance(url, str) or not url:
            raise ValueError('bad url')
        self.close()
        profile = tempfile.mkdtemp(prefix=f'zell-theater-{browser}-')
        os.chmod(profile, 0o700)
        _PROFILES.add(profile)
        port = _free_port()
        # The command line stays on about:blank. The requested URL is loaded
        # only after the document-start hold is installed.
        command = _launch_command(browser, binary[0], profile, 'about:blank', width, height, port)
        global _SESSIONS
        _SESSIONS += 1
        if self.session_type is None:
            self.session_type = _load_session_type()
        session = self.session_type(
            f'zell-theater-{os.getpid()}-{_SESSIONS}', width, height, 12)
        log_path = os.path.join(profile, 'browser.log')
        self._profile = profile
        self._record = os.path.join(profile, 'pids')
        self._log_path = log_path
        self._debug_port = port
        try:
            session.start_xvfb(nocursor=True)
            if getattr(session, 'display', None):
                self._arm_reaper()
                self._record_pid(getattr(session.server, 'pid', None))
            self.launches.append(command)
            log = open(log_path, 'ab')
            try:
                session.launch_app(command, isolate_bus=True, clipboard=False, stderr=log)
            finally:
                log.close()
            if getattr(session, 'display', None):
                self._record_pid(getattr(session.app, 'pid', None))
                bus = getattr(session, '_app_bus', None)
                if bus is not None:
                    self._record_pid(getattr(bus, 'daemon_pid', None))
            session.make_injector()
            started = session.start_capture(draw_cursor=False)
            if getattr(session, 'display', None):
                if getattr(session, 'capture', None) is None and getattr(session, 'capture_process', None) is None:
                    raise RuntimeError('private display capture did not start')
                capture = getattr(session, 'capture_process', None)
                if capture is not None:
                    self._record_pid(getattr(capture, 'pid', None))
                initial = getattr(started, 'initial_frame', None)
                if isinstance(initial, (bytes, bytearray)) and len(initial) == width * height * 3:
                    self._initial_frame = bytes(initial)
                self.url = url
                self._connect_control(browser, port, width, height)
                self._navigate(url)
                self._wait_for_hold(url)
                self._apply_media('pause')
        except Exception as error:
            try:
                session.close()
            except Exception:
                pass
            self._disarm_reaper()
            _kill_recorded(self._record)
            detail = _log_tail(log_path)
            self._profile = None
            self.url = None
            self._hold_installed = False
            _remove_profile(profile)
            self._control_close()
            if detail:
                raise RuntimeError(f'{error}\n{detail}') from error
            raise
        self.session = session
        self.browser = browser
        self.url = url
        self.viewport = (width, height)
        self.popped = False
        return session

    def frame(self, timeout=0.1):
        with self._lock:
            if self.session is None:
                raise RuntimeError('no browser session')
            timeout = float(timeout)
            if timeout < 0:
                raise ValueError('bad timeout')
            capture = getattr(self.session, 'capture', None)
            process = getattr(self.session, 'capture_process', None)
            if capture is None and process is None:
                raise RuntimeError('private display capture is not running')
            if capture is not None:
                return self._frame_damage(capture, timeout)
            if process.poll() is not None:
                raise RuntimeError('capture process exited')
            stdout = process.stdout
            if stdout is None:
                raise RuntimeError('capture process has no frames')
            width = int(getattr(self.session, 'width', 0) or 0)
            height = int(getattr(self.session, 'height', 0) or 0)
            if width <= 0 or height <= 0:
                raise RuntimeError('capture size is unknown')
            return _read_raw_frame(stdout.fileno(), width, height, timeout, self._raw_pending)

    def key(self, key, modifiers=0, text=''):
        """Tap one key, or paste text, on the private display only."""
        with self._lock:
            if self.session is None:
                raise RuntimeError('no browser session')
            if isinstance(modifiers, bool) or not isinstance(modifiers, int) or modifiers < 0 or modifiers & ~_MOD_MASK:
                raise ValueError('bad modifiers')
            if not isinstance(text, str):
                raise ValueError('bad text')
            injector = self.session.make_injector()
            if text:
                if any(0xD800 <= ord(char) <= 0xDFFF or ord(char) > 0x10FFFF for char in text):
                    raise ValueError('bad text')
                if not hasattr(injector, 'paste') or not injector.paste(text):
                    reason = getattr(injector, 'last_input_refusal', None) or 'text was not injected'
                    raise RuntimeError(reason)
                return True
            if not isinstance(key, str) or not (len(key) == 1 or key in _KEY_NAMES):
                raise ValueError('bad key')
            if not hasattr(injector, 'chord'):
                raise RuntimeError('browser injector has no keyboard')
            if not injector.chord(key, modifiers, 1):
                raise RuntimeError('key was not injected')
            injector.chord(key, 0, 3)
            return True

    def pointer(self, x, y, button, action):
        with self._lock:
            if self.session is None:
                raise RuntimeError('no browser session')
            if isinstance(x, bool) or isinstance(y, bool) or not isinstance(x, int) or not isinstance(y, int):
                raise ValueError('bad pointer')
            if action not in ('move', 'down', 'up'):
                raise ValueError('bad pointer action')
            injector = self.session.make_injector()
            if action == 'move':
                injector.move_click(int(x), int(y), 0, None)
            elif action == 'down':
                injector.move_click(int(x), int(y), int(button or 1), True)
            else:
                injector.move_click(int(x), int(y), int(button or 1), False)

    def pause(self):
        with self._lock:
            return _public_media(self._read_page('pause'))

    def resume(self):
        with self._lock:
            return _public_media(self._read_page('play'))

    def media_state(self):
        with self._lock:
            return _public_media(self._read_page('state'))

    def popout(self):
        with self._lock:
            if self.session is None:
                raise RuntimeError('no browser session')
            self.popped = True

    def dock(self):
        with self._lock:
            if self.session is None:
                raise RuntimeError('no browser session')
            self.popped = False

    def close(self):
        with self._lock:
            session = self.session
            self.session = None
            self.browser = None
            self.url = None
            self.popped = False
            self.viewport = None
            self._initial_frame = None
            self._raw_pending.clear()
            failure = None
            if session is not None:
                try:
                    session.close()
                except Exception as error:
                    failure = error
            _kill_recorded(self._record)
            self._disarm_reaper()
            self._control_close()
            self._hold_installed = False
            profile = self._profile
            self._profile = None
            self._record = None
            _remove_profile(profile)
            if failure is not None:
                raise failure

    def _frame_damage(self, capture, timeout):
        deadline = time.monotonic() + timeout
        while True:
            pumped = capture.pump()
            if pumped:
                frame = pumped[0]
                width = int(capture.width)
                height = int(capture.height)
                if len(frame) != width * height * 3:
                    raise RuntimeError('capture frame has the wrong size')
                self._initial_frame = None
                return width, height, frame
            if self._initial_frame is not None and time.monotonic() >= deadline:
                frame = self._initial_frame
                self._initial_frame = None
                return int(capture.width), int(capture.height), frame
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            events = getattr(capture, 'events', None)
            if events is None:
                return None
            select.select([events], [], [], remaining)

    def _read_page(self, action):
        if self.session is None:
            raise RuntimeError('no browser session')
        if getattr(self.session, 'display', None) is None:
            raise RuntimeError('browser control is not connected')
        self._ensure_control()
        return self._apply_media(action)

    def _page_value(self, expression):
        """Evaluate one page expression. Tests use this for their own DOM."""
        with self._lock:
            if self.session is None:
                raise RuntimeError('no browser session')
            if getattr(self.session, 'display', None) is None:
                raise RuntimeError('browser control is not connected')
            self._ensure_control()
            return self._eval(expression)

    def _apply_media(self, action):
        # DevTools treats only the resume evaluation as a user gesture.
        value = self._eval(_MEDIA_JS % json.dumps(action), user_gesture=(action == 'play'))
        return _normalize_media(value)

    def _eval(self, expression, user_gesture=False):
        if self._control_kind == 'marionette':
            value = self._marionette(
                'WebDriver:ExecuteScript',
                {'script': 'return (' + expression + ')', 'args': []})
        else:
            value = self._devtools(expression, user_gesture=user_gesture)
        if isinstance(value, dict) and 'present' not in value and set(value) == {'value'}:
            return value['value']
        return value

    def _navigate(self, url):
        if self._control_kind == 'marionette':
            self._marionette('WebDriver:Navigate', {'url': url})
            return
        result = self._devtools_command('Page.navigate', {'url': url})
        if result.get('errorText'):
            raise RuntimeError(f'browser navigation failed: {result["errorText"]}')

    def _wait_for_hold(self, url):
        # The finished document may be a redirect, an https upgrade, or a
        # login hop. The hold has to be on that document, not on the URL
        # that was requested.
        probe = (
            '(() => {'
            ' const href = String(location.href || "");'
            ' const root = document.documentElement;'
            ' const hold = root ? root.getAttribute("data-zell-hold") : null;'
            ' const ready = document.readyState;'
            ' const scheme = /^(https?:|file:)/.test(href);'
            ' return {held: hold === "1" && scheme && ready !== "loading",'
            ' href: href, ready: ready, hold: hold};'
            '})()'
        )
        deadline = time.monotonic() + 20
        last = None
        stable = None
        stable_since = None
        while time.monotonic() < deadline:
            try:
                last = self._eval(probe)
            except Exception as error:
                last = str(error)
                stable = None
            else:
                href = last.get('href') if isinstance(last, dict) else None
                if isinstance(last, dict) and last.get('held') and href:
                    if href != stable:
                        stable = href
                        stable_since = time.monotonic()
                    elif time.monotonic() - stable_since >= 0.4:
                        return
                else:
                    stable = None
            time.sleep(0.15)
        raise RuntimeError(f'browser page did not hold before use: requested {url} landed {last}')

    def _install_hold(self):
        if self._hold_installed:
            return
        if self._control_kind == 'marionette':
            root = _write_hold_extension(self._profile)
            result = self._marionette('Addon:Install', {'path': root, 'temporary': True})
            if not result:
                raise RuntimeError('browser hold extension did not install')
        else:
            # Page.enable has to precede this registration. Without it the
            # script is accepted and then never runs on the next document.
            self._devtools_command('Page.addScriptToEvaluateOnNewDocument', {'source': _HOLD_JS})
        self._hold_installed = True

    def _connect_control(self, browser, port, width, height):
        deadline = time.monotonic() + 25
        last = None
        while time.monotonic() < deadline:
            try:
                if browser == 'firefox-esr':
                    self._connect_marionette(port, width, height)
                else:
                    self._connect_devtools(port)
                    self._devtools_command('Page.enable', {})
                self._install_hold()
                return
            except Exception as error:
                last = error
                self._hold_installed = False
                self._control_close()
                time.sleep(0.25)
        detail = _log_tail(self._log_path)
        message = f'browser control did not become ready: {last}'
        if detail:
            message = f'{message}\n{detail}'
        raise RuntimeError(message)

    def _ensure_control(self):
        if self._control is not None:
            return
        if self.browser is None or self._debug_port is None or self.viewport is None:
            raise RuntimeError('browser control is not connected')
        self._connect_control(self.browser, self._debug_port, *self.viewport)

    def _connect_devtools(self, fallback_port):
        port_file = os.path.join(self._profile, 'DevToolsActivePort')
        if not os.path.isfile(port_file):
            raise RuntimeError('browser debug port is not ready')
        with open(port_file, encoding='ascii') as handle:
            lines = handle.read().splitlines()
        port = int(lines[0]) if lines else fallback_port
        if not 1 <= port <= 65535:
            raise RuntimeError('browser debug port is invalid')
        targets = _read_http_json(port, '/json/list', 5)
        pages = [row for row in targets if isinstance(row, dict) and row.get('type') == 'page']
        chosen = None
        for row in pages:
            if row.get('url') == self.url:
                chosen = row
                break
        if chosen is None:
            for row in pages:
                page_url = str(row.get('url') or '')
                if page_url.startswith(('http://', 'https://', 'file:')):
                    chosen = row
                    break
        if chosen is None and pages:
            chosen = pages[0]
        socket_url = None if chosen is None else chosen.get('webSocketDebuggerUrl')
        if not socket_url:
            raise RuntimeError('browser page target is not ready')
        path = socket_url.split('://', 1)[1]
        path = path[path.find('/'):]
        self._control = _ws_connect(port, path, 5)
        self._control_kind = 'devtools'
        self._debug_port = port

    def _connect_marionette(self, port, width, height):
        sock = socket.create_connection(('127.0.0.1', port), 3)
        sock.settimeout(10)
        self._control = sock
        self._control_kind = 'marionette'
        self._message = 0
        hello = self._marionette_recv()
        if not isinstance(hello, dict) or hello.get('applicationType') != 'gecko':
            raise RuntimeError('marionette handshake failed')
        self._marionette('WebDriver:NewSession', {'capabilities': {'alwaysMatch': {}}})
        self._marionette('Marionette:SetContext', {'value': 'content'})
        try:
            # Kiosk already places the window. A refused resize must not
            # abandon a session whose page is on the private display.
            self._marionette('WebDriver:SetWindowRect', {
                'x': 0, 'y': 0, 'width': int(width), 'height': int(height),
            })
        except RuntimeError:
            pass

    def _devtools_command(self, method, params):
        self._message += 1
        ident = self._message
        _ws_send(self._control, json.dumps({
            'id': ident,
            'method': method,
            'params': params,
        }))
        while True:
            reply = json.loads(_ws_recv(self._control))
            if reply.get('id') != ident:
                continue
            if reply.get('error'):
                raise RuntimeError(f'browser control failed: {reply["error"]}')
            return reply.get('result') or {}

    def _devtools(self, script, user_gesture=False):
        result = self._devtools_command('Runtime.evaluate', {
            'expression': script,
            'returnByValue': True,
            'userGesture': bool(user_gesture),
        })
        details = result.get('exceptionDetails')
        value = result.get('result', {})
        if details or value.get('subtype') == 'error':
            raise RuntimeError(f'browser page script failed: {details or value}')
        return value.get('value')

    def _marionette(self, command, params):
        self._message += 1
        payload = [0, self._message, command, params]
        data = json.dumps(payload, separators=(',', ':')).encode()
        self._control.sendall(f'{len(data)}:'.encode() + data)
        reply = self._marionette_recv()
        if not isinstance(reply, list) or len(reply) < 4 or reply[1] != self._message:
            raise RuntimeError('marionette reply does not match the command')
        if reply[2]:
            raise RuntimeError(f'marionette {command} failed: {reply[2]}')
        return reply[3]

    def _marionette_recv(self):
        buf = b''
        while b':' not in buf:
            chunk = self._control.recv(1)
            if not chunk:
                raise RuntimeError('marionette closed')
            buf += chunk
            if len(buf) > 12:
                raise RuntimeError('marionette length is invalid')
        length = int(buf[:-1])
        if length < 0 or length > 8 * 1024 * 1024:
            raise RuntimeError('marionette message is too large')
        body = b''
        while len(body) < length:
            chunk = self._control.recv(length - len(body))
            if not chunk:
                raise RuntimeError('marionette closed')
            body += chunk
        return json.loads(body)

    def _control_close(self):
        control = self._control
        self._control = None
        self._control_kind = None
        if control is not None:
            try:
                control.close()
            except Exception:
                pass

    def _arm_reaper(self):
        if self._reaper is not None or not self._record or not self._profile:
            return
        fd = os.open(self._record, os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_NOFOLLOW, 0o600)
        os.close(fd)
        parent_start = _proc_start(os.getpid())
        if parent_start is None:
            raise RuntimeError('cannot record the browser owner')
        self._reaper = subprocess.Popen(
            [sys.executable, __file__, '--reap', str(os.getpid()), parent_start,
             self._record, self._profile],
            start_new_session=True,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            close_fds=True,
        )

    def _record_pid(self, pid):
        if not pid or not self._record:
            return
        start = _proc_start(pid)
        if start is None:
            return
        fd = os.open(self._record, os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_NOFOLLOW, 0o600)
        try:
            os.write(fd, f'{int(pid)} {start}\n'.encode())
            os.fsync(fd)
        finally:
            os.close(fd)

    def _disarm_reaper(self):
        reaper = self._reaper
        self._reaper = None
        if reaper is None:
            return
        try:
            if reaper.poll() is None:
                reaper.terminate()
                reaper.wait(timeout=2)
        except Exception:
            try:
                reaper.kill()
            except Exception:
                pass


def _launch_command(browser, binary, profile, url, width, height, port):
    extension = _write_hold_extension(profile)
    if browser == 'firefox-esr':
        _write_firefox_prefs(profile, port)
        return [
            binary, '--profile', profile, '--no-remote', '--marionette', '--kiosk', url,
        ]
    return [
        binary,
        f'--user-data-dir={profile}',
        '--no-first-run',
        '--no-default-browser-check',
        '--disable-sync',
        '--disable-default-apps',
        '--disable-popup-blocking',
        '--disable-gpu',
        '--force-device-scale-factor=1',
        '--remote-debugging-port=0',
        '--remote-debugging-address=127.0.0.1',
        '--remote-allow-origins=*',
        f'--disable-extensions-except={extension}',
        f'--load-extension={extension}',
        f'--window-size={width},{height}',
        '--window-position=0,0',
        '--ozone-platform=x11',
        '--kiosk',
        url,
    ]


if __name__ == '__main__':
    if sys.argv[1:2] == ['--reap']:
        _reap_main(sys.argv[2:])
