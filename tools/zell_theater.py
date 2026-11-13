"""Theater helper. Speaks the frozen theater media contract on stdin and stdout.

The process is separate from the station terminal. Pictures are Neuronte
MultiInk frames. A selected browser stays on a Kilix private display.
"""
from __future__ import annotations

import ctypes
import json
import mmap
import os
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from pathlib import Path

sys.dont_write_bytecode = True

from zell_theater_browser import BROWSERS, CHROME_DEB_NAME, PrivateBrowser
from zell_theater_fit import MAP_LIMIT, bound_frame, cell_pad, contain, map_pointer

ID_RE = re.compile(r'^[A-Za-z0-9_-]{1,32}$')
LINE_MAX = 65536
LISTING_MAX = 200
PATH_MAX = 4096
HEADER = 64
MAP_BYTES = HEADER + MAP_LIMIT * MAP_LIMIT * 4
MAGIC = b'ZTHR'
COMMIT_RE = re.compile(r'^[0-9a-f]{40}$')
VIDEO_SUFFIXES = {'.mp4', '.mkv', '.webm', '.mov', '.m4v', '.ogv'}
TIME_RE = re.compile(r'(?:(\d+):)?(\d+):(\d+(?:[.,]\d+)?)')
LOOPBACK_HOSTS = frozenset(('127.0.0.1', 'localhost', '::1'))


def select_runtime(environ=None):
    """Choose NEURONTE_HOME, else the source-home install. Do not scan other trees."""
    env = os.environ if environ is None else environ
    home = env.get('NEURONTE_HOME') or ''
    if home:
        path, role = Path(home), 'fixture'
    else:
        source = env.get('GPU_TERMINAL_SOURCE_HOME') or ''
        path, role = (Path(source) / 'neuronte' if source else None), 'installed'
    selected = '' if path is None else str(path)
    weights = None if path is None else path / 'weights' / 'neuronte-video-masks-v0.nrt'
    library = None if path is None else path / 'build' / 'libneuronte.so'
    if path is None or not weights.is_file() or not library.is_file():
        missing = selected or 'neuronte'
        if path is not None and not weights.is_file():
            missing = str(weights)
        elif path is not None and not library.is_file():
            missing = str(library)
        return {'role': 'missing', 'commit': '', 'selected': selected, 'missing': missing}
    commit = ''
    record = path / '.plebian-os-neuronte.json'
    try:
        value = json.loads(record.read_text(encoding='utf-8')).get('commit', '')
        if isinstance(value, str) and COMMIT_RE.fullmatch(value):
            commit = value
    except (OSError, ValueError, AttributeError):
        commit = ''
    return {'role': role, 'commit': commit, 'selected': selected, 'missing': ''}


def pack_header(width, height, seq, flags, position_ms, duration_ms, generation):
    if not 0 <= width <= MAP_LIMIT or not 0 <= height <= MAP_LIMIT:
        raise ValueError('frame does not fit the theater map')
    if width and height and width * height * 4 > MAP_BYTES - HEADER:
        raise ValueError('frame does not fit the theater map')
    body = struct.pack(
        '<4sIIIIIIIIII', MAGIC, 1, width, height, seq & 0xffffffff, flags,
        min(int(position_ms), 0xffffffff), min(int(duration_ms), 0xffffffff),
        width * 4, 1, generation & 0xffffffff)
    if len(body) > HEADER:
        raise ValueError('header')
    return body + bytes(HEADER - len(body))


def spawn(argv, **kwargs):
    """Child dies with this process and lives in its own process group."""
    parent = os.getpid()

    def preexec():
        os.setsid()
        try:
            ctypes.CDLL(None).prctl(1, signal.SIGTERM)
        except Exception:
            pass
        if os.getppid() != parent:
            os._exit(0)

    kwargs.setdefault('stdin', subprocess.DEVNULL)
    kwargs.setdefault('stdout', subprocess.DEVNULL)
    kwargs.setdefault('stderr', subprocess.DEVNULL)
    return subprocess.Popen(argv, preexec_fn=preexec, **kwargs)


def reap(child):
    if child is None:
        return
    if child.poll() is None:
        try:
            os.killpg(child.pid, signal.SIGTERM)
        except (ProcessLookupError, PermissionError, OSError):
            try:
                child.terminate()
            except OSError:
                pass
        try:
            child.wait(timeout=0.5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError, OSError):
                try:
                    child.kill()
                except OSError:
                    pass
            try:
                child.wait(timeout=0.5)
            except subprocess.TimeoutExpired:
                pass
    else:
        try:
            child.wait(timeout=0.1)
        except subprocess.TimeoutExpired:
            pass
    for stream in (child.stdout, child.stderr, child.stdin):
        if stream is not None:
            try:
                stream.close()
            except Exception:
                pass


def parse_cues(text, kind):
    body = text.lstrip('\ufeff')
    if kind == 'vtt' and not body.lstrip().startswith('WEBVTT'):
        raise ValueError('caption file is not WebVTT')
    cues = []
    for block in re.split(r'\n\s*\n', body.strip()):
        start = end = None
        lines = []
        for line in block.splitlines():
            if '-->' in line and start is None:
                left, right = line.split('-->', 1)
                start, end = _stamp(left), _stamp(right)
            elif start is not None and line.strip() and not line.startswith('NOTE'):
                lines.append(line.strip())
        if start is not None and end is not None and end >= start:
            cues.append((start, end, ' '.join(lines)[:400]))
    return cues


def _stamp(value):
    match = TIME_RE.search(value.strip())
    if not match:
        raise ValueError('caption time')
    hours = int(match.group(1) or 0)
    minutes = int(match.group(2))
    seconds = float(match.group(3).replace(',', '.'))
    return int(round((hours * 3600 + minutes * 60 + seconds) * 1000))


def cue_at(cues, position_ms):
    for start, end, text in cues:
        if start <= position_ms < end:
            return text, start, end
    return '', 0, 0


def load_sidecar(path):
    for suffix, kind in (('.srt', 'srt'), ('.vtt', 'vtt')):
        side = Path(path).with_suffix(suffix)
        if not side.is_file():
            continue
        try:
            return parse_cues(side.read_text(encoding='utf-8', errors='replace'), kind), ''
        except (OSError, ValueError) as error:
            return [], str(error)
    return [], ''


def list_folder(path):
    root = Path(path)
    if not root.is_dir():
        raise ProtocolError('folder is missing')
    entries = []
    for child in sorted(root.iterdir()):
        if child.name.startswith('.') or child.suffix.lower() not in VIDEO_SUFFIXES or not child.is_file():
            continue
        name, full = child.name, str(child)
        if len(name) > PATH_MAX or len(full) > PATH_MAX:
            continue
        entries.append({'name': name, 'path': full})
        if len(entries) >= LISTING_MAX:
            break
    while entries:
        encoded = json.dumps({'event': 'listing', 'id': 'x', 'entries': entries})
        if len(encoded.encode()) <= LINE_MAX:
            break
        entries.pop()
    return entries


def probe_file(path):
    info = {'width': 0, 'height': 0, 'duration_ms': 0, 'audio': False, 'rate': 0.0}
    try:
        out = subprocess.run(
            ['ffprobe', '-v', 'error', '-show_entries',
             'stream=codec_type,width,height,r_frame_rate:format=duration', '-of', 'json', path],
            capture_output=True, text=True, timeout=3, check=False)
        doc = json.loads(out.stdout or '{}')
    except (OSError, subprocess.TimeoutExpired, json.JSONDecodeError):
        return info
    for stream in doc.get('streams') or []:
        if stream.get('codec_type') == 'audio':
            info['audio'] = True
        if stream.get('codec_type') == 'video' and not info['width']:
            info['width'] = int(stream.get('width') or 0)
            info['height'] = int(stream.get('height') or 0)
            rate = str(stream.get('r_frame_rate') or '')
            if '/' in rate:
                num, den = rate.split('/', 1)
                if den not in ('0', '') and num.isdigit() and den.isdigit():
                    info['rate'] = int(num) / int(den)
    try:
        info['duration_ms'] = int(float((doc.get('format') or {}).get('duration') or 0) * 1000)
    except (TypeError, ValueError):
        info['duration_ms'] = 0
    return info


def _playable(info):
    """A file can be queued only when probe found a real picture."""
    if not isinstance(info, dict):
        return False
    try:
        width = int(info.get('width') or 0)
        height = int(info.get('height') or 0)
    except (TypeError, ValueError):
        return False
    return width > 0 and height > 0 and not isinstance(info.get('width'), bool)


def load_ink(selected):
    import importlib.util
    root = Path(selected)
    script = root / 'tools' / 'neuronte_video.py'
    if not script.is_file():
        raise FileNotFoundError(script)
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location('zell_runtime_neuronte_video', script)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.MultiInk(
        weights=root / 'weights' / 'neuronte-video-masks-v0.nrt',
        library=root / 'build' / 'libneuronte.so',
        cell_w=2, cell_h=4)


def fit_bgr(ink, rgb):
    """Pad, encode, decode, and crop one RGB frame. Returns BGRA bytes and size."""
    import numpy as np
    frame = np.asarray(rgb)
    if frame.dtype != np.uint8 or frame.ndim != 3 or frame.shape[2] != 3:
        raise ValueError('frame must be HxWx3 uint8 RGB')
    height, width = frame.shape[:2]
    pad_w, pad_h = cell_pad(width, height)
    if pad_h != height or pad_w != width:
        frame = np.pad(frame, ((0, pad_h - height), (0, pad_w - width), (0, 0)), mode='edge')
    fitted = ink.decode(ink.encode(frame))[:height, :width]
    bgra = np.empty((height, width, 4), np.uint8)
    bgra[..., 0] = fitted[..., 2]
    bgra[..., 1] = fitted[..., 1]
    bgra[..., 2] = fitted[..., 0]
    bgra[..., 3] = 255
    return bgra.tobytes(), width, height


def scale_rgb(rgb, width, height):
    import numpy as np
    frame = np.asarray(rgb)
    src_h, src_w = frame.shape[:2]
    if (src_w, src_h) == (width, height):
        return frame
    ys = np.clip(np.arange(height) * src_h // height, 0, src_h - 1)
    xs = np.clip(np.arange(width) * src_w // width, 0, src_w - 1)
    return np.ascontiguousarray(frame[ys][:, xs])


class ProtocolError(Exception):
    def __init__(self, detail):
        super().__init__(detail)
        self.detail = detail


def _acceptable_url(url):
    """https anywhere without credentials, or http only on a loopback host."""
    if not isinstance(url, str) or not url or len(url.encode()) > PATH_MAX:
        return False
    parts = urllib.parse.urlsplit(url)
    if parts.username or parts.password:
        return False
    host = (parts.hostname or '').lower()
    if parts.scheme == 'https':
        return bool(host)
    if parts.scheme == 'http':
        return host in LOOPBACK_HOSTS
    return False


def mpv_time_pos(path, timeout=0.05):
    """One time-pos read from an mpv IPC socket. None when the player is not ready."""
    if not path:
        return None
    sock = socket.socket(socket.AF_UNIX)
    sock.settimeout(timeout)
    try:
        sock.connect(os.fspath(path))
        sock.sendall(b'{"command":["get_property","time-pos"],"request_id":1}\n')
        buf = b''
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if b'\n' not in buf:
                try:
                    part = sock.recv(4096)
                except socket.timeout:
                    return None
                if not part:
                    return None
                buf += part
                continue
            line, buf = buf.split(b'\n', 1)
            try:
                reply = json.loads(line)
            except json.JSONDecodeError:
                continue
            if reply.get('request_id') != 1:
                continue
            data = reply.get('data')
            if reply.get('error') == 'success' and isinstance(data, (int, float)):
                return float(data)
            return None
    except (OSError, ValueError):
        return None
    finally:
        try:
            sock.close()
        except OSError:
            pass
    return None


class PlaybackClock:
    """mpv position when a socket exists, otherwise monotonic time from the seek origin."""

    def __init__(self, origin_seconds, socket_path):
        self.origin = float(origin_seconds)
        self.socket_path = socket_path
        self.started = time.monotonic()
        self.synced = None

    def now(self):
        mono = time.monotonic()
        if self.socket_path and (self.synced is None or mono - self.synced[0] >= 0.1):
            pos = mpv_time_pos(self.socket_path, 0.05)
            if pos is not None:
                self.synced = (time.monotonic(), pos)
        if self.synced is not None:
            return max(0.0, self.synced[1] + (time.monotonic() - self.synced[0]))
        return self.origin + (mono - self.started)

    def close(self):
        self.socket_path = None


class FrameMap:
    def __init__(self, fd):
        self.fd = fd
        self.mm = None
        self.seq = 0
        if fd is None or fd < 0:
            return
        os.ftruncate(fd, MAP_BYTES)
        self.mm = mmap.mmap(fd, MAP_BYTES)

    def clear(self, generation):
        self.seq = 0
        if self.mm is None:
            return
        self.mm[:HEADER] = pack_header(0, 0, 0, 0, 0, 0, generation)

    def publish(self, width, height, flags, position_ms, duration_ms, generation, pixels):
        expected = width * height * 4
        if len(pixels) != expected:
            raise ValueError('pixel size')
        odd = self.seq + 1
        if odd % 2 == 0:
            odd += 1
        even = odd + 1
        header = pack_header(width, height, even, flags, position_ms, duration_ms, generation)
        if self.mm is not None:
            self.mm[:HEADER] = pack_header(width, height, odd, flags, position_ms, duration_ms, generation)
            self.mm[HEADER:HEADER + expected] = pixels
            self.mm[:HEADER] = header
        self.seq = even
        return even

    def touch(self, width, height, flags, position_ms, duration_ms, generation):
        """Publish new control fields for the picture already in the map."""
        odd = self.seq + 1
        if odd % 2 == 0:
            odd += 1
        even = odd + 1
        if self.mm is not None:
            self.mm[:HEADER] = pack_header(width, height, odd, flags, position_ms, duration_ms, generation)
            self.mm[:HEADER] = pack_header(width, height, even, flags, position_ms, duration_ms, generation)
        self.seq = even
        return even


def _install_fields(browser):
    """Fixed install operation for the private-terminal bridge. Not a shell command."""
    if browser == 'chrome':
        return {'action': 'install-chrome-deb', 'deb': CHROME_DEB_NAME}
    if browser == 'firefox-esr':
        return {'action': 'install-firefox-esr'}
    if browser == 'chromium':
        return {'action': 'install-chromium'}
    raise ProtocolError('url open requires a browser')


class TheaterSession:
    def __init__(self, runtime, browser=None, hold_audio=None, audio_launcher=None, play_frames=False, browser_factory=None):
        self.runtime = {key: runtime[key] for key in ('role', 'commit', 'selected')}
        self.missing = runtime.get('missing') or ''
        # One PrivateBrowser per URL. An injected instance is the first page, for tests.
        self._shared_browser = browser
        self._browser_factory = browser_factory
        self.pages = {}
        self.hold_audio = hold_audio if hold_audio is not None else _hold_audio
        self.audio_launcher = audio_launcher
        self.play_frames = play_frames
        self.items = {}
        self.active = None
        self.generation = 0
        self.dest = (1408, 585)
        self.captions_on = True
        self.frames = FrameMap(None)
        self.shown = (0, 0, 0)
        self.ink = None
        self.audio_child = None
        self.decoder = None
        self.thread = None
        self.audio_held = False
        self.stop_playback = threading.Event()
        self.socket_dirs = []
        self.audio_socket = None
        self.pending_install = None
        self.stopped = False
        self.lock = threading.Lock()
        self._fresh = []

    def emit(self, event):
        with self.lock:
            self._fresh.append(event)
        return event

    def drain(self):
        with self.lock:
            events = self._fresh
            self._fresh = []
        return events

    def start(self):
        if self.runtime['role'] == 'missing':
            self.emit({
                'event': 'fatal',
                'error': f'{self.missing} is missing',
                'runtime': dict(self.runtime),
            })
        else:
            self.emit({'event': 'ready', 'protocol': 1, 'runtime': dict(self.runtime)})
        return self.drain()

    def attach(self, fd):
        self.frames = FrameMap(fd)

    def handle_line(self, line):
        if isinstance(line, str):
            raw = line.encode()
        else:
            raw = line
        if raw.endswith(b'\n'):
            raw = raw[:-1]
        if len(raw) > LINE_MAX:
            self.emit({'event': 'error', 'error': 'malformed', 'detail': 'line too long'})
            return self.drain()
        try:
            command = json.loads(raw.decode())
        except (UnicodeError, json.JSONDecodeError):
            self.emit({'event': 'error', 'error': 'malformed', 'detail': 'bad json'})
            return self.drain()
        if not isinstance(command, dict) or 'cmd' not in command:
            self.emit({'event': 'error', 'error': 'malformed', 'detail': 'missing cmd'})
            return self.drain()
        try:
            self.handle(command)
        except ProtocolError as error:
            self.emit({'event': 'error', 'error': 'malformed', 'detail': error.detail})
        return self.drain()

    def handle(self, command):
        name = command.get('cmd')
        if name == 'open':
            return self.open_item(command)
        if name == 'use':
            return self.use_item(command.get('id'))
        if name == 'pause':
            return self.set_paused(True)
        if name == 'resume':
            return self.set_paused(False)
        if name == 'seek_ms':
            return self.seek(command.get('position_ms'))
        if name == 'close':
            return self.close_item(command.get('id'))
        if name == 'resize':
            return self.resize(command.get('width'), command.get('height'))
        if name == 'cc':
            self.captions_on = bool(command.get('on'))
            return self._emit_caption(self.active)
        if name == 'popout':
            return self.move_browser(True)
        if name == 'dock':
            return self.move_browser(False)
        if name == 'pointer':
            return self.pointer(command)
        if name == 'key':
            return self.send_key(command)
        if name in ('install_result', 'install_cancel'):
            return self.complete_install(command)
        if name == 'stop':
            self.shutdown()
            self.stopped = True
            return self.status('', 'ended')
        raise ProtocolError('unknown cmd')

    def open_item(self, command):
        identity = command.get('id')
        source = command.get('source')
        browser = command.get('browser', None)
        if not isinstance(identity, str) or not ID_RE.fullmatch(identity):
            raise ProtocolError('bad id')
        if identity in self.items:
            raise ProtocolError('duplicate id')
        if not isinstance(source, dict):
            raise ProtocolError('bad source')
        kind = source.get('kind')
        if kind in ('file', 'folder'):
            if browser is not None:
                raise ProtocolError('file open rejects a browser')
            path = source.get('path', '')
            if not isinstance(path, str) or not path or len(path.encode()) > PATH_MAX:
                raise ProtocolError('bad path')
            if kind == 'folder':
                entries = list_folder(path)
                # A listing is a query. It is not a queued item and it does
                # not retire the picture, so the current movie keeps playing.
                return self.emit({
                    'event': 'listing', 'id': identity, 'entries': entries,
                    'generation': self.generation,
                })
            if not Path(path).is_file():
                raise ProtocolError('file is missing')
            info = probe_file(path)
            if not _playable(info):
                # No queue entry and no audio hold. The current movie stays up.
                return self.emit({
                    'event': 'error', 'error': 'decode', 'id': identity,
                    'detail': 'file has no video',
                })
            cues, caption_error = load_sidecar(path)
            self._bump(clear=True)
            self.items[identity] = _blank(kind, path, info, cues, caption_error)
            if self.active and not self.items[self.active].get('paused'):
                self.begin_playback()
            return self.status(identity, 'paused')
        if kind == 'url':
            return self.open_url(identity, source, browser)
        raise ProtocolError('bad source')

    def open_url(self, identity, source, browser):
        if browser not in BROWSERS:
            raise ProtocolError('url open requires a browser')
        url = source.get('url', '')
        if not _acceptable_url(url):
            raise ProtocolError('bad url')
        if self.pending_install is not None:
            raise ProtocolError('install in progress')
        if _browser_missing(browser, self._which()):
            return self.request_install(identity, browser, url)
        return self._open_ready_url(identity, browser, url)

    def _which(self):
        if self._shared_browser is not None:
            return getattr(self._shared_browser, 'which', None)
        for page in self.pages.values():
            found = getattr(page, 'which', None)
            if found is not None:
                return found
        return None

    def _make_page(self, identity):
        page = self.pages.get(identity)
        if page is not None:
            return page
        if self._shared_browser is not None and not self.pages:
            page = self._shared_browser
        elif self._browser_factory is not None:
            page = self._browser_factory()
        else:
            page = PrivateBrowser()
        self.pages[identity] = page
        return page

    def _active_page(self):
        if self.active is None:
            return None
        return self.pages.get(self.active)

    def request_install(self, identity, browser, url):
        """Ask the parent for one fixed install action. Do not run it here."""
        fields = _install_fields(browser)
        if 'sudo' in fields.values() or fields.get('action') == 'google-chrome-stable':
            raise ProtocolError('refusing a bare chrome package install')
        self.pending_install = {'id': identity, 'browser': browser, 'url': url}
        self.emit({
            'event': 'install', 'id': identity, 'browser': browser,
            'phase': 'request', **fields,
        })
        return None

    def complete_install(self, command):
        pending = self.pending_install
        if pending is None:
            raise ProtocolError('no install')
        if command.get('id') != pending['id']:
            raise ProtocolError('install id')
        self.pending_install = None
        identity, browser, url = pending['id'], pending['browser'], pending['url']
        fields = _install_fields(browser)
        cancelled = command.get('cmd') == 'install_cancel' or command.get('ok') is not True
        if cancelled:
            detail = 'cancelled' if command.get('cmd') == 'install_cancel' else 'install failed'
            self.emit({'event': 'install', 'id': identity, 'browser': browser, 'phase': 'error', 'detail': detail, **fields})
            self.emit({'event': 'error', 'error': 'install', 'detail': detail})
            return None
        if _browser_missing(browser, self._which()):
            self.emit({'event': 'install', 'id': identity, 'browser': browser, 'phase': 'error', 'detail': 'still missing', **fields})
            self.emit({'event': 'error', 'error': 'install', 'detail': 'still missing'})
            return None
        self.emit({'event': 'install', 'id': identity, 'browser': browser, 'phase': 'done', 'detail': 'installed', **fields})
        return self._open_ready_url(identity, browser, url)

    def _open_ready_url(self, identity, browser, url):
        width, height = _browser_size(*self.dest)
        page = self._make_page(identity)
        try:
            page.launch(browser, url, width, height)
        except Exception as error:
            self.pages.pop(identity, None)
            self.emit({'event': 'error', 'error': 'capture', 'detail': _clean(error) or 'private display unavailable'})
            return None
        self._remember_url(identity, url, browser)
        return self.status(identity, 'paused')

    def _remember_url(self, identity, url, browser):
        self._bump(clear=True)
        self.items[identity] = _blank('url', url)
        self.items[identity]['browser'] = browser
        self.items[identity]['url'] = url
        if self.active and not self.items[self.active].get('paused'):
            self.begin_playback()

    def use_item(self, identity):
        if not isinstance(identity, str) or not ID_RE.fullmatch(identity):
            raise ProtocolError('bad id')
        if identity not in self.items:
            raise ProtocolError('unknown id')
        # The leaving page must actually pause before this id becomes audible.
        detail = self._silence_other_pages(identity)
        if detail:
            self.emit({'event': 'error', 'error': 'capture', 'detail': detail})
            if self.active is None:
                return None
            return self.status(self.active, self._activity_state(self.active))
        self.halt_playback()
        with self.lock:
            self.generation = (self.generation + 1) & 0xffffffff
            self._clear_shown()
            self.active = identity
            for other, row in self.items.items():
                row['paused'] = other != identity
                if other == identity:
                    row.pop('ended', None)
        self._emit_caption(identity)
        self.begin_playback()
        self._sync_shown_header()
        return self.status(identity, self._activity_state(identity))

    def set_paused(self, paused):
        if self.active is None:
            raise ProtocolError('nothing playing')
        if paused:
            if self.items[self.active].get('kind') == 'url':
                try:
                    silenced = self._pause_page(self.active)
                except Exception as error:
                    self.emit({'event': 'error', 'error': 'capture', 'detail': _clean(error)})
                    return self.status(self.active, 'playing')
                if not silenced:
                    self.emit({'event': 'error', 'error': 'capture', 'detail': 'browser stayed audible'})
                    return self.status(self.active, 'playing')
            self.halt_playback()
            with self.lock:
                self.items[self.active]['paused'] = True
            self._set_hold(False)
            self._sync_shown_header()
        else:
            with self.lock:
                item = self.items[self.active]
                item['paused'] = False
                item.pop('ended', None)
            self.begin_playback()
            self._sync_shown_header()
        if paused:
            state = 'paused'
        else:
            state = self._activity_state(self.active)
        return self.status(self.active, state)

    def seek(self, position):
        if self.active is None or not isinstance(position, int) or position < 0:
            raise ProtocolError('bad seek')
        item = self.items[self.active]
        playing = not item['paused']
        if playing:
            self.halt_playback()
        item.pop('ended', None)
        item['position_ms'] = position
        self._emit_caption(self.active)
        if playing:
            self.begin_playback()
        self._sync_shown_header()
        return self.status(self.active, self._activity_state(self.active))

    def close_item(self, identity):
        if not isinstance(identity, str) or not ID_RE.fullmatch(identity):
            raise ProtocolError('bad id')
        if identity not in self.items:
            raise ProtocolError('unknown id')
        kind = self.items[identity].get('kind')
        closing_active = self.active == identity
        self.halt_playback()
        with self.lock:
            self.generation = (self.generation + 1) & 0xffffffff
            self.items.pop(identity)
            if closing_active:
                self.active = None
                self._set_hold(False)
            self._clear_shown()
        if kind == 'url':
            page = self.pages.pop(identity, None)
            if page is not None:
                try:
                    page.close()
                except Exception:
                    pass
        self._emit_caption(self.active)
        if self.active and not self.items[self.active]['paused']:
            self.begin_playback()
        return self.status(self.active or '', 'paused' if self.active is None or self.items[self.active]['paused'] else 'playing')

    def resize(self, width, height):
        if not isinstance(width, int) or not isinstance(height, int):
            raise ProtocolError('bad resize')
        if not 1 <= width <= 16384 or not 1 <= height <= 16384:
            raise ProtocolError('resize out of range')
        playing = self.active is not None and not self.items[self.active]['paused']
        if playing:
            self.halt_playback()
        self.dest = (width, height)
        # The capture viewport stays at launch size. set_geometry only
        # rewrites injector bounds, so resize and popout do not call it.
        # The next frame is contained into this destination.
        if playing:
            self.begin_playback()
        state = 'paused'
        if self.active is not None and not self.items[self.active]['paused']:
            state = 'playing'
        return self.status(self.active or '', state)

    def move_browser(self, popped):
        if self.active is None or self.items[self.active].get('kind') != 'url':
            raise ProtocolError('no browser item')
        page = self._active_page()
        if page is None:
            raise ProtocolError('no browser item')
        if popped:
            page.popout()
        else:
            page.dock()
        return self.status(self.active, 'playing' if not self.items[self.active]['paused'] else 'paused')

    def pointer(self, command):
        if self.active is None:
            raise ProtocolError('no picture')
        x, y = command.get('x'), command.get('y')
        action = command.get('action')
        button = command.get('button', 0)
        if button is None:
            button = 0
        # bool is an int subclass. A JSON true coordinate must not become 1.
        if (isinstance(x, bool) or isinstance(y, bool) or isinstance(button, bool)
                or not isinstance(x, int) or not isinstance(y, int) or not isinstance(button, int)
                or action not in ('down', 'up', 'move')):
            raise ProtocolError('bad pointer')
        item = self.items[self.active]
        try:
            src_w, src_h = self._pointer_source(item)
            content_w, content_h = contain(src_w, src_h, *self.dest)
            point = map_pointer(x, y, self.dest[0], self.dest[1], content_w, content_h, src_w, src_h)
        except ValueError as error:
            raise ProtocolError('bad pointer') from error
        page = self._active_page()
        if point is not None and item.get('kind') == 'url' and page is not None and getattr(page, 'session', None) is not None:
            page.pointer(point[0], point[1], button, action)
        return self.status(self.active, 'playing' if not item['paused'] else 'paused')

    def send_key(self, command):
        """Tap one page key, or paste text, through the private display.

        key() presses and releases once. modifiers is the Kilix bit mask.
        A down/up boolean is not that mask. An up event is not sent again.
        """
        if self.active is None or self.items[self.active].get('kind') != 'url':
            raise ProtocolError('no browser item')
        action = command.get('action', 'down')
        if isinstance(action, bool) or action not in ('down', 'up'):
            raise ProtocolError('bad key')
        text = command.get('text', '')
        key_name = command.get('key', '')
        modifiers = command.get('modifiers', 0)
        if modifiers is None:
            modifiers = 0
        if not isinstance(text, str) or isinstance(modifiers, bool) or not isinstance(modifiers, int):
            raise ProtocolError('bad key')
        if text == '' and (not isinstance(key_name, str) or not key_name or len(key_name) > 32):
            raise ProtocolError('bad key')
        item = self.items[self.active]
        if action == 'up':
            return self.status(self.active, 'playing' if not item['paused'] else 'paused')
        page = self._active_page()
        sender = getattr(page, 'key', None)
        if page is None or sender is None:
            raise ProtocolError('no key input')
        try:
            sender(key_name, modifiers, text)
        except ValueError as error:
            raise ProtocolError(_clean(error) or 'bad key') from error
        except Exception as error:
            self.emit({'event': 'error', 'error': 'capture', 'detail': _clean(error) or 'key was not injected'})
        return self.status(self.active, 'playing' if not item['paused'] else 'paused')

    def _pointer_source(self, item):
        """Fixed capture size. Contain this into the destination, then map."""
        if item.get('src_w') and item.get('src_h'):
            return int(item['src_w']), int(item['src_h'])
        page = self._active_page()
        viewport = getattr(page, 'viewport', None) if page is not None else None
        if isinstance(viewport, tuple) and len(viewport) == 2:
            return int(viewport[0]), int(viewport[1])
        return self.dest

    def _apply_media(self, identity, state):
        if not isinstance(state, dict):
            return
        with self.lock:
            item = self.items.get(identity)
            if item is None:
                return
            position = state.get('position_ms')
            if isinstance(position, (int, float)) and not isinstance(position, bool) and position >= 0:
                item['position_ms'] = int(position)
            duration = state.get('duration_ms')
            if isinstance(duration, (int, float)) and not isinstance(duration, bool) and duration >= 0:
                item['duration_ms'] = int(duration)

    def _call_page(self, identity, name):
        page = self.pages.get(identity)
        method = getattr(page, name, None) if page is not None else None
        if method is None:
            return None
        return method()

    def _set_hold(self, active):
        self.hold_audio(bool(active), os.getpid())
        self.audio_held = bool(active)

    def _confirmed_paused(self, state):
        """True when pause returned without saying the page is still playing.

        A stand-in may return nothing. A real page that returns paused false,
        or a present video with no boolean, is still audible.
        """
        if not isinstance(state, dict):
            return True
        if state.get('paused') is False:
            return False
        if state.get('present') is True and state.get('paused') is not True:
            return False
        return True

    def _pause_page(self, identity):
        """Pause one page. False means it is still audible. An exception propagates."""
        page = self.pages.get(identity)
        if page is None or not hasattr(page, 'pause'):
            return True
        state = page.pause()
        if not self._confirmed_paused(state):
            return False
        self._apply_media(identity, state)
        item = self.items.get(identity)
        if item is not None:
            item['paused'] = True
        return True

    def _silence_other_pages(self, identity):
        """Pause every other page. Return an error detail when one stays audible."""
        for key in list(self.pages):
            if key == identity:
                continue
            try:
                if not self._pause_page(key):
                    return 'browser stayed audible'
            except Exception as error:
                return _clean(error) or 'browser stayed audible'
        return ''

    def render_rgb(self, rgb, generation=None):
        """Fit one RGB array through MultiInk and publish it, or drop a stale generation."""
        if generation is None:
            generation = self.generation
        if generation != self.generation:
            return None
        if self.runtime['role'] == 'missing':
            self.emit({'event': 'fatal', 'error': f'{self.missing} is missing', 'runtime': dict(self.runtime)})
            return None
        if self.ink is None:
            try:
                self.ink = load_ink(self.runtime['selected'])
            except Exception as error:
                self.missing = str(getattr(error, 'filename', '') or error)
                self.runtime = {'role': 'missing', 'commit': '', 'selected': self.runtime['selected']}
                self.emit({'event': 'fatal', 'error': f'{self.missing} is missing', 'runtime': dict(self.runtime)})
                return None
        import numpy as np
        frame = np.asarray(rgb)
        src_h, src_w = frame.shape[:2]
        fitted_w, fitted_h = contain(src_w, src_h, *self.dest)
        frame_w, frame_h = bound_frame(fitted_w, fitted_h)
        scaled = scale_rgb(frame, frame_w, frame_h)
        pixels, width, height = fit_bgr(self.ink, scaled)
        return self.write_frame(pixels, width, height, generation)

    def write_frame(self, pixels, width, height, generation):
        if generation != self.generation or self.runtime['role'] == 'missing':
            return None
        item = self.items.get(self.active) or {'position_ms': 0, 'duration_ms': 0, 'paused': True}
        flags = self._header_flags(item)
        seq = self.frames.publish(width, height, flags, item.get('position_ms', 0), item.get('duration_ms', 0), generation, pixels)
        self.shown = (width, height, seq)
        return seq

    def published_size(self, src_w, src_h):
        fitted_w, fitted_h = contain(src_w, src_h, *self.dest)
        frame_w, frame_h = bound_frame(fitted_w, fitted_h)
        return frame_w, frame_h, *cell_pad(frame_w, frame_h)

    def begin_playback(self):
        if self.active is None:
            return
        if self.thread is not None and self.thread.is_alive():
            return
        item = self.items[self.active]
        if item.get('paused'):
            return
        item.pop('decode_error', None)
        item.pop('ended', None)
        identity = self.active
        if item.get('kind') == 'url':
            self._begin_url(identity)
            return
        if item.get('kind') != 'file':
            return
        self._begin_file(identity, item)

    def _begin_file(self, identity, item):
        if not item.get('src_w') or not item.get('src_h'):
            self.emit({
                'event': 'error', 'error': 'decode', 'id': identity,
                'detail': 'file has no video',
            })
            self._fail_start(identity, 'file has no video')
            return
        detail = self._silence_other_pages(None)
        if detail:
            self.emit({'event': 'error', 'error': 'capture', 'detail': detail})
            self._fail_start(identity, detail)
            return
        if self.play_frames and item.get('src_w') and item.get('src_h'):
            self._ensure_ink()
            if self.ink is None:
                self._fail_start(identity, 'model', release_hold=self.audio_held)
                return
        self._set_hold(True)
        self.audio_socket = None
        if self.audio_launcher is not None:
            child = self.audio_launcher(item)
            if child is not None:
                self.audio_child = child
        if not self.play_frames or not item.get('src_w') or not item.get('src_h'):
            return
        frame_w, frame_h = self.published_size(item['src_w'], item['src_h'])[:2]
        rate = item.get('rate') or 12.0
        argv = [
            'ffmpeg', '-v', 'error', '-ss', f"{item['position_ms'] / 1000:.3f}",
            '-i', item['path'], '-an', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
            '-s', f'{frame_w}x{frame_h}', '-',
        ]
        try:
            proc = spawn(argv, stdout=subprocess.PIPE)
        except OSError as error:
            detail = _clean(error)
            self.emit({'event': 'error', 'error': 'decode', 'detail': detail})
            self._fail_start(identity, detail or 'decode', release_hold=True)
            return
        self.decoder = proc
        self.stop_playback.clear()
        generation = self.generation
        identity = self.active
        socket_path = self.audio_socket
        self.thread = threading.Thread(
            target=self._decode,
            args=(proc, identity, generation, frame_w, frame_h, rate, item['position_ms'], socket_path),
            daemon=True)
        self.thread.start()

    def _begin_url(self, identity):
        detail = self._silence_other_pages(identity)
        if detail:
            self.emit({'event': 'error', 'error': 'capture', 'detail': detail})
            self._fail_start(identity, detail)
            return
        if self.play_frames:
            page = self.pages.get(identity)
            if page is None or not hasattr(page, 'frame'):
                self.emit({'event': 'error', 'error': 'capture', 'detail': 'browser frames are not available'})
                self._fail_start(identity, 'browser frames are not available', release_hold=self.audio_held)
                return
            self._ensure_ink()
            if self.ink is None:
                self._fail_start(identity, 'model', release_hold=self.audio_held)
                return
        self._set_hold(True)
        try:
            state = self._call_page(identity, 'resume')
        except Exception as error:
            detail = _clean(error)
            self.emit({'event': 'error', 'error': 'capture', 'detail': detail or 'private display unavailable'})
            self._fail_start(identity, detail or 'capture', release_hold=True)
            return
        self._apply_media(identity, state)
        if not self.play_frames:
            return
        self.stop_playback.clear()
        generation = self.generation
        self.thread = threading.Thread(
            target=self._capture, args=(identity, generation), daemon=True)
        self.thread.start()

    def _fail_start(self, identity, detail, release_hold=False):
        """A start that did not become playing owns no child and is not playing."""
        item = self.items.get(identity)
        if item is not None:
            item['paused'] = True
            item['decode_error'] = detail or 'playback did not start'
        self._drop_children()
        if release_hold:
            try:
                self._set_hold(False)
            except Exception:
                self.audio_held = False
        self._sync_shown_header()

    def _drop_children(self):
        audio, decoder = self.audio_child, self.decoder
        self.audio_child = None
        self.decoder = None
        self.audio_socket = None
        reap(decoder)
        reap(audio)

    def _activity_state(self, identity):
        item = self.items.get(identity) or {}
        if item.get('decode_error'):
            return 'error'
        if item.get('ended'):
            return 'ended'
        if item.get('paused'):
            return 'paused'
        return 'playing'

    def _header_flags(self, item):
        flags = 1 if item.get('paused') else 0
        if item.get('ended'):
            flags |= 2
        if item.get('caption_error') or item.get('decode_error'):
            flags |= 4
        return flags

    def _sync_shown_header(self):
        """Rewrite paused, ended, error, and position on the current picture."""
        width, height, seq = self.shown
        if not seq or width <= 0 or height <= 0:
            return
        item = self.items.get(self.active) or {}
        new_seq = self.frames.touch(
            width, height, self._header_flags(item),
            item.get('position_ms', 0), item.get('duration_ms', 0), self.generation)
        self.shown = (width, height, new_seq)

    def _ensure_ink(self):
        if self.ink is not None or self.runtime['role'] == 'missing':
            return
        try:
            self.ink = load_ink(self.runtime['selected'])
        except Exception as error:
            self.emit({'event': 'fatal', 'error': _clean(error), 'runtime': dict(self.runtime)})

    def _note_position(self, identity, generation, position_ms):
        with self.lock:
            if self.stop_playback.is_set() or generation != self.generation or self.active != identity:
                return False
            item = self.items.get(identity)
            if item is None:
                return False
            item['position_ms'] = max(0, int(position_ms))
            return True

    def _current_caption(self, identity):
        if not identity or not self.captions_on:
            return ''
        item = self.items.get(identity) or {}
        if item.get('caption_error'):
            return ''
        text, _start, _end = cue_at(item.get('cues') or [], item.get('position_ms', 0))
        return text

    def _sync_caption(self, identity, previous):
        text = self._current_caption(identity)
        if text != previous:
            self._emit_caption(identity)
        return text

    def _finish_media(self, identity, generation, ended, detail, error_name='decode'):
        if self.stop_playback.is_set() or generation != self.generation or self.active != identity:
            return
        item = self.items.get(identity)
        if item is None:
            return
        if ended:
            if item.get('duration_ms'):
                item['position_ms'] = item['duration_ms']
            item['ended'] = True
            item['paused'] = True
            self._emit_caption(identity)
            self.status(identity, 'ended')
        else:
            item['paused'] = True
            item['decode_error'] = detail or error_name
            self.emit({'event': 'error', 'error': error_name, 'detail': detail or error_name})
            self.status(identity, 'error')
        self._sync_shown_header()
        try:
            self._set_hold(False)
        except Exception:
            self.audio_held = False
        self._drop_children()

    def _decode(self, proc, identity, generation, width, height, rate, origin_ms, socket_path):
        import numpy as np
        needed = width * height * 3
        clock = PlaybackClock(origin_ms / 1000.0, socket_path)
        index = 0
        origin = origin_ms / 1000.0
        rate = rate or 12.0
        last_status = -10 ** 9
        last_text = self._current_caption(identity)
        try:
            while not self.stop_playback.is_set():
                chunk = _read_exact(proc.stdout, needed, self.stop_playback)
                if chunk is None:
                    break
                due = origin + index / rate
                index += 1
                while not self.stop_playback.is_set():
                    now = clock.now()
                    if now + 0.001 >= due:
                        break
                    time.sleep(min(0.02, max(0.0, due - now)))
                if self.stop_playback.is_set():
                    break
                now = clock.now()
                if not self._note_position(identity, generation, now * 1000):
                    break
                if now - due <= 1.5 / rate:
                    rgb = np.frombuffer(chunk, dtype=np.uint8).reshape(height, width, 3).copy()
                    try:
                        pixels, out_w, out_h = fit_bgr(self.ink, rgb)
                    except Exception as error:
                        self._finish_media(identity, generation, False, _clean(error))
                        reap(proc)
                        return
                    if self.stop_playback.is_set() or generation != self.generation or self.active != identity:
                        break
                    self.write_frame(pixels, out_w, out_h, generation)
                item = self.items.get(identity) or {}
                pos = item.get('position_ms', 0)
                if pos - last_status >= 100:
                    last_status = pos
                    self.status(identity, 'playing')
                last_text = self._sync_caption(identity, last_text)
        finally:
            clock.close()
        if self.stop_playback.is_set() or generation != self.generation or self.active != identity:
            reap(proc)
            return
        try:
            code = proc.wait(timeout=1)
        except subprocess.TimeoutExpired:
            reap(proc)
            if not self.stop_playback.is_set():
                self._finish_media(identity, generation, False, 'decode stalled')
            return
        if self.stop_playback.is_set() or generation != self.generation or self.active != identity:
            return
        if code == 0:
            self._finish_media(identity, generation, True, '')
        else:
            self._finish_media(identity, generation, False, f'decode exit {code}')

    def _read_media(self, page):
        state_fn = getattr(page, 'media_state', None)
        if state_fn is None:
            return None
        try:
            state = state_fn()
        except Exception:
            return None
        if not isinstance(state, dict):
            return None
        return state

    def _at_end(self, state):
        if not isinstance(state, dict) or state.get('present') is False:
            return False
        duration = state.get('duration_ms')
        position = state.get('position_ms')
        if isinstance(duration, bool) or not isinstance(duration, (int, float)) or duration <= 0:
            return False
        if isinstance(position, bool) or not isinstance(position, (int, float)):
            return False
        return position >= duration

    def _browser_position(self, page, identity, origin, started):
        state = self._read_media(page)
        if state is not None:
            self._apply_media(identity, state)
            pos = state.get('position_ms')
            if isinstance(pos, (int, float)) and not isinstance(pos, bool) and pos >= 0:
                return float(pos), state
        return (origin + (time.monotonic() - started)) * 1000, state

    def _complete_browser(self, identity, generation, state):
        """The page reached its duration. Finish only if this pause confirms quiet.

        state is the media_state that reached the duration. A paused flag in
        that snapshot must not release the hold when pause() is still playing
        or raises.
        """
        del state
        try:
            silenced = self._pause_page(identity)
        except Exception as error:
            self.emit({
                'event': 'error', 'error': 'capture',
                'detail': _clean(error) or 'browser stayed audible',
            })
            return
        if not silenced:
            self.emit({'event': 'error', 'error': 'capture', 'detail': 'browser stayed audible'})
            return
        self._finish_media(identity, generation, True, '')

    def _fail_capture(self, identity, generation, detail):
        """A dead capture must not drop the hold while the page is still playing."""
        try:
            silenced = self._pause_page(identity)
        except Exception as error:
            silenced = False
            detail = _clean(error) or detail
        if not silenced:
            self.emit({'event': 'error', 'error': 'capture', 'detail': detail or 'capture'})
            return
        self._finish_media(identity, generation, False, detail or 'capture', 'capture')

    def _capture(self, identity, generation):
        page = self.pages.get(identity)
        if page is None or not hasattr(page, 'frame'):
            self.emit({'event': 'error', 'error': 'capture', 'detail': 'browser frames are not available'})
            return
        item = self.items.get(identity) or {}
        origin = item.get('position_ms', 0) / 1000.0
        started = time.monotonic()
        last_status = -10 ** 9
        last_text = self._current_caption(identity)
        while not self.stop_playback.is_set():
            position_ms, state = self._browser_position(page, identity, origin, started)
            if self._at_end(state):
                self._complete_browser(identity, generation, state)
                return
            if not self._note_position(identity, generation, position_ms):
                break
            try:
                frame = page.frame(0.1)
            except Exception as error:
                self._fail_capture(identity, generation, _clean(error))
                return
            if frame is not None:
                try:
                    width, height, raw = frame
                    import numpy as np
                    rgb = np.frombuffer(raw, dtype=np.uint8).reshape(int(height), int(width), 3).copy()
                    with self.lock:
                        shown = self.items.get(identity)
                        if shown is not None and generation == self.generation:
                            shown['src_w'] = int(width)
                            shown['src_h'] = int(height)
                    self.render_rgb(rgb, generation)
                except Exception as error:
                    self._fail_capture(identity, generation, _clean(error))
                    return
            pos = (self.items.get(identity) or {}).get('position_ms', 0)
            if pos - last_status >= 100:
                last_status = pos
                self.status(identity, 'playing')
            last_text = self._sync_caption(identity, last_text)
            time.sleep(0.02)

    def halt_playback(self):
        self.stop_playback.set()
        audio, decoder, thread = self.audio_child, self.decoder, self.thread
        self.audio_child = None
        self.decoder = None
        self.audio_socket = None
        reap(decoder)
        reap(audio)
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout=3)
        if thread is not None and thread.is_alive():
            self.thread = thread
            return
        self.thread = None
        self.stop_playback.clear()

    def shutdown(self):
        self.halt_playback()
        try:
            self._set_hold(False)
        except Exception:
            self.audio_held = False
        for page in list(self.pages.values()):
            try:
                page.close()
            except Exception:
                pass
        self.pages.clear()
        for directory in self.socket_dirs:
            try:
                for child in Path(directory).iterdir():
                    child.unlink()
                Path(directory).rmdir()
            except OSError:
                pass
        self.socket_dirs.clear()

    def status(self, identity, state):
        item = self.items.get(identity, {})
        width = height = seq = 0
        if self.shown[2] and state != 'ended':
            width, height, seq = self.shown
        if state == 'ended' or identity == '' and self.active is None:
            width = height = seq = 0
        return self.emit({
            'event': 'status', 'id': identity, 'state': state,
            'position_ms': item.get('position_ms', 0), 'duration_ms': item.get('duration_ms', 0),
            'width': width, 'height': height, 'seq': seq, 'generation': self.generation,
            'error': item.get('caption_error') or None, 'browser': item.get('browser'),
            'runtime': dict(self.runtime),
        })

    def _emit_caption(self, identity):
        text, start, end = '', 0, 0
        if identity and self.captions_on:
            item = self.items.get(identity) or {}
            if not item.get('caption_error'):
                text, start, end = cue_at(item.get('cues') or [], item.get('position_ms', 0))
        return self.emit({
            'event': 'caption', 'id': identity or '', 'text': text,
            'start_ms': start, 'end_ms': end, 'generation': self.generation,
        })

    def _bump(self, clear):
        self.halt_playback()
        with self.lock:
            self.generation = (self.generation + 1) & 0xffffffff
            if clear:
                self._clear_shown()

    def _clear_shown(self):
        self.frames.clear(self.generation)
        self.shown = (0, 0, 0)


def _blank(kind, path, info=None, cues=None, caption_error=''):
    info = info or {}
    return {
        'kind': kind, 'path': path, 'position_ms': 0,
        'duration_ms': info.get('duration_ms', 0), 'paused': True,
        'src_w': info.get('width') or 0, 'src_h': info.get('height') or 0,
        'audio': bool(info.get('audio')), 'rate': info.get('rate') or 0,
        'cues': cues or [], 'caption_error': caption_error, 'browser': None,
    }


def _browser_missing(browser, which):
    import shutil
    find = which or shutil.which
    if browser == 'chromium':
        return find('chromium') is None and find('chromium-browser') is None
    if browser == 'chrome':
        return find('google-chrome') is None and find('google-chrome-stable') is None
    return find(browser) is None


def _browser_size(dest_w, dest_h):
    return bound_frame(*contain(dest_w, dest_h, dest_w, dest_h))


def _hold_audio(active, owner):
    try:
        import zell_audio
    except ImportError:
        return False
    hook = getattr(zell_audio, 'theater_audio', None)
    if hook is None:
        return False
    return hook(bool(active), int(owner))


def _clean(error):
    text = ''.join(ch for ch in str(error) if ch.isprintable())
    return text[:400]


def _read_exact(stream, count, stop):
    if stream is None:
        return None
    data = bytearray()
    while len(data) < count:
        if stop.is_set():
            return None
        try:
            piece = stream.read(count - len(data))
        except (OSError, ValueError):
            return None
        if not piece:
            return None
        data.extend(piece)
    return bytes(data)


def product_audio(session, item, ao=None):
    session.audio_socket = None
    if not item.get('audio'):
        return None
    import shutil
    player = shutil.which('kilix-mpv') or shutil.which('mpv')
    if player is None:
        return None
    directory = Path(tempfile.mkdtemp(prefix='zell-theater-'))
    session.socket_dirs.append(directory)
    socket_path = directory / 'mpv.sock'
    session.audio_socket = socket_path
    argv = [
        player, '--no-config', '--no-video', '--no-terminal', '--really-quiet',
        f'--input-ipc-server={socket_path}', f"--start={item.get('position_ms', 0) / 1000:.3f}",
        '--keep-open=no', '--audio-display=no',
    ]
    if ao:
        argv.append(f'--ao={ao}')
    argv.extend(['--', item['path']])
    return spawn(argv)


def _read_available(stdin, timeout):
    """Read one chunk, or None at EOF. A real pipe waits at most timeout seconds."""
    fd = None
    try:
        fd = stdin.fileno()
    except (AttributeError, OSError, ValueError):
        fd = None
    if fd is not None:
        try:
            ready, _, _ = select.select([fd], [], [], timeout)
        except (ValueError, OSError):
            return b''
        if not ready:
            return b''
        reader = getattr(stdin, 'read1', None)
        try:
            data = reader(65536) if reader is not None else os.read(fd, 65536)
        except BlockingIOError:
            return b''
        if isinstance(data, str):
            data = data.encode()
        if not data:
            return None
        return data
    data = stdin.read(65536)
    if isinstance(data, str):
        data = data.encode()
    if not data:
        return None
    return data


def _feed(stdout, session, pending, discarding, chunk):
    """Assemble lines without holding more than LINE_MAX of an unfinished line."""
    view = chunk
    while view and not session.stopped:
        if discarding:
            nl = view.find(b'\n')
            if nl < 0:
                return pending, True
            view = view[nl + 1:]
            discarding = False
            continue
        nl = view.find(b'\n')
        take = view if nl < 0 else view[:nl]
        if len(pending) + len(take) > LINE_MAX:
            pending = bytearray()
            _write(stdout, {'event': 'error', 'error': 'malformed', 'detail': 'line too long'})
            if nl < 0:
                return pending, True
            view = view[nl + 1:]
            continue
        if nl < 0:
            pending.extend(view)
            return pending, False
        line = bytes(pending) + view[:nl]
        pending = bytearray()
        view = view[nl + 1:]
        if not line.strip():
            continue
        for event in session.handle_line(line):
            _write(stdout, event)
    return pending, discarding


def serve(stdin, stdout, session, frame_fd=None):
    session.attach(frame_fd)
    for event in session.start():
        _write(stdout, event)
    pending = bytearray()
    discarding = False
    while not session.stopped:
        for event in session.drain():
            _write(stdout, event)
        if session.stopped:
            break
        chunk = _read_available(stdin, 0.05)
        if chunk is None:
            break
        if chunk:
            pending, discarding = _feed(stdout, session, pending, discarding, chunk)
    for event in session.drain():
        _write(stdout, event)
    session.shutdown()
    return 0


def _write(stdout, event):
    payload = json.dumps(event, separators=(',', ':')).encode() + b'\n'
    if len(payload) > LINE_MAX:
        payload = b'{"event":"error","error":"malformed","detail":"reply too long"}\n'
    stdout.write(payload)
    stdout.flush()


def main():
    runtime = select_runtime()
    session = TheaterSession(runtime, play_frames=True)
    session.audio_launcher = lambda item: product_audio(session, item)
    raw = os.environ.get('ZELL_THEATER_FD')
    frame_fd = int(raw) if raw else None
    try:
        return serve(sys.stdin.buffer, sys.stdout.buffer, session, frame_fd)
    finally:
        session.shutdown()


if __name__ == '__main__':
    sys.exit(main())
