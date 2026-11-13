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
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.dont_write_bytecode = True

from zell_theater_browser import (
    BROWSERS, CHROME_DEB_NAME, PrivateBrowser, chrome_privilege_argv, firefox_privilege_argv,
)
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


class TheaterSession:
    def __init__(self, runtime, browser=None, hold_audio=None, audio_launcher=None, play_frames=False):
        self.runtime = {key: runtime[key] for key in ('role', 'commit', 'selected')}
        self.missing = runtime.get('missing') or ''
        self.browser = browser if browser is not None else PrivateBrowser()
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
        self.stop_playback = threading.Event()
        self.socket_dirs = []
        self.stopped = False
        self.lock = threading.Lock()
        self._fresh = []

    def emit(self, event):
        self._fresh.append(event)
        return event

    def drain(self):
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
                self._bump(clear=True)
                self.items[identity] = _blank(kind, path)
                return self.emit({'event': 'listing', 'id': identity, 'entries': entries, 'generation': self.generation})
            if not Path(path).is_file():
                raise ProtocolError('file is missing')
            info = probe_file(path)
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
        if not isinstance(url, str) or not url.startswith('https://') or len(url.encode()) > PATH_MAX:
            raise ProtocolError('bad url')
        if _browser_missing(browser, self.browser.which):
            return self.install_browser(identity, browser, url)
        width, height = _browser_size(*self.dest)
        try:
            self.browser.launch(browser, url, width, height)
        except Exception as error:
            self.emit({'event': 'error', 'error': 'capture', 'detail': _clean(error) or 'private display unavailable'})
            return None
        self._remember_url(identity, url, browser)
        return self.status(identity, 'paused')

    def install_browser(self, identity, browser, url):
        if browser == 'chrome':
            argv = chrome_privilege_argv(CHROME_DEB_NAME)
        elif browser == 'firefox-esr':
            argv = firefox_privilege_argv()
        else:
            argv = ['apt-get', 'install', '-y', 'chromium']
        if 'google-chrome-stable' in argv or 'sudo' in argv:
            raise ProtocolError('refusing a bare chrome package install')
        self.emit({
            'event': 'install', 'browser': browser, 'phase': 'starting',
            'detail': 'private terminal', 'command': argv,
        })
        if self.browser.privilege is None:
            self.emit({'event': 'install', 'browser': browser, 'phase': 'error', 'detail': 'private terminal required', 'command': argv})
            self.emit({'event': 'error', 'error': 'install', 'detail': 'private terminal required'})
            return None
        result = self.browser.privilege(argv)
        if result != 0:
            self.emit({'event': 'install', 'browser': browser, 'phase': 'error', 'detail': str(result), 'command': argv})
            self.emit({'event': 'error', 'error': 'install', 'detail': 'install failed'})
            return None
        self.emit({'event': 'install', 'browser': browser, 'phase': 'done', 'detail': 'installed', 'command': argv})
        width, height = _browser_size(*self.dest)
        try:
            self.browser.launch(browser, url, width, height)
        except Exception as error:
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
        if identity not in self.items:
            raise ProtocolError('unknown id')
        self.halt_playback()
        with self.lock:
            self.generation = (self.generation + 1) & 0xffffffff
            self._clear_shown()
            self.active = identity
            for other, row in self.items.items():
                row['paused'] = other != identity
        self._emit_caption(identity)
        self.begin_playback()
        state = 'playing' if not self.items[identity]['paused'] else 'paused'
        return self.status(identity, state)

    def set_paused(self, paused):
        if self.active is None:
            raise ProtocolError('nothing playing')
        if paused:
            self.halt_playback()
            with self.lock:
                self.items[self.active]['paused'] = True
            self.hold_audio(False, os.getpid())
        else:
            with self.lock:
                self.items[self.active]['paused'] = False
            self.begin_playback()
        return self.status(self.active, 'paused' if paused else 'playing')

    def seek(self, position):
        if self.active is None or not isinstance(position, int) or position < 0:
            raise ProtocolError('bad seek')
        item = self.items[self.active]
        playing = not item['paused']
        if playing:
            self.halt_playback()
        item['position_ms'] = position
        self._emit_caption(self.active)
        if playing:
            self.begin_playback()
        return self.status(self.active, 'paused' if item['paused'] else 'playing')

    def close_item(self, identity):
        if identity not in self.items:
            raise ProtocolError('unknown id')
        closing_active = self.active == identity
        if closing_active:
            self.halt_playback()
        with self.lock:
            self.generation = (self.generation + 1) & 0xffffffff
            self.items.pop(identity)
            if closing_active:
                self.active = None
                self._clear_shown()
                self.hold_audio(False, os.getpid())
                self.browser.close()
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
        if self.browser.session is not None and hasattr(self.browser.session, 'set_geometry'):
            try:
                self.browser.session.set_geometry(*_browser_size(width, height))
            except Exception:
                pass
        if playing:
            self.begin_playback()
        state = 'paused'
        if self.active is not None and not self.items[self.active]['paused']:
            state = 'playing'
        return self.status(self.active or '', state)

    def move_browser(self, popped):
        if self.active is None or self.items[self.active].get('kind') != 'url':
            raise ProtocolError('no browser item')
        if popped:
            self.browser.popout()
        else:
            self.browser.dock()
        return self.status(self.active, 'playing' if not self.items[self.active]['paused'] else 'paused')

    def pointer(self, command):
        if self.active is None:
            raise ProtocolError('no picture')
        x, y = command.get('x'), command.get('y')
        action = command.get('action')
        if not isinstance(x, int) or not isinstance(y, int) or action not in ('down', 'up', 'move'):
            raise ProtocolError('bad pointer')
        item = self.items[self.active]
        src_w = item.get('src_w') or self.dest[0]
        src_h = item.get('src_h') or self.dest[1]
        content_w, content_h = contain(src_w, src_h, *self.dest)
        display_w, display_h = bound_frame(content_w, content_h)
        point = map_pointer(x, y, self.dest[0], self.dest[1], content_w, content_h, display_w, display_h)
        if point is not None and item.get('kind') == 'url' and self.browser.session is not None:
            self.browser.pointer(point[0], point[1], int(command.get('button') or 0), action)
        return self.status(self.active, 'playing' if not item['paused'] else 'paused')

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
        flags = 1 if item.get('paused') else 0
        if item.get('caption_error'):
            flags |= 4
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
        item = self.items[self.active]
        if item.get('paused') or item.get('kind') != 'file':
            if item.get('kind') == 'url' and not item.get('paused'):
                self.hold_audio(True, os.getpid())
            return
        self.hold_audio(True, os.getpid())
        if self.audio_launcher is not None:
            child = self.audio_launcher(item)
            if child is not None:
                self.audio_child = child
        if not self.play_frames or not item.get('src_w') or not item.get('src_h'):
            return
        if self.ink is None and self.runtime['role'] != 'missing':
            try:
                self.ink = load_ink(self.runtime['selected'])
            except Exception as error:
                self.emit({'event': 'fatal', 'error': _clean(error), 'runtime': dict(self.runtime)})
                return
        if self.ink is None:
            return
        frame_w, frame_h = self.published_size(item['src_w'], item['src_h'])[:2]
        rate = item.get('rate') or 12
        argv = [
            'ffmpeg', '-v', 'error', '-ss', f"{item['position_ms'] / 1000:.3f}",
            '-i', item['path'], '-an', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
            '-s', f'{frame_w}x{frame_h}', '-',
        ]
        try:
            proc = spawn(argv, stdout=subprocess.PIPE)
        except OSError as error:
            self.emit({'event': 'error', 'error': 'decode', 'detail': _clean(error)})
            return
        self.decoder = proc
        generation = self.generation
        identity = self.active
        self.stop_playback = threading.Event()
        self.thread = threading.Thread(
            target=self._decode, args=(proc, identity, generation, frame_w, frame_h, rate, item['position_ms']),
            daemon=True)
        self.thread.start()

    def _decode(self, proc, identity, generation, width, height, rate, origin_ms):
        import numpy as np
        needed = width * height * 3
        started = time.monotonic()
        index = 0
        origin = origin_ms / 1000
        while not self.stop_playback.is_set():
            chunk = _read_exact(proc.stdout, needed, self.stop_playback)
            if chunk is None:
                break
            due = origin + index / rate
            index += 1
            clock = origin + (time.monotonic() - started)
            if clock - due > 1.5 / rate:
                continue
            if generation != self.generation or self.active != identity:
                break
            rgb = np.frombuffer(chunk, dtype=np.uint8).reshape(height, width, 3)
            try:
                pixels, out_w, out_h = fit_bgr(self.ink, rgb)
            except Exception:
                break
            with self.lock:
                if generation != self.generation or self.active != identity or self.stop_playback.is_set():
                    break
                self.write_frame(pixels, out_w, out_h, generation)
        reap(proc)

    def halt_playback(self):
        self.stop_playback.set()
        audio, decoder, thread = self.audio_child, self.decoder, self.thread
        self.audio_child = None
        self.decoder = None
        self.thread = None
        reap(decoder)
        reap(audio)
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout=1)

    def shutdown(self):
        self.halt_playback()
        try:
            self.hold_audio(False, os.getpid())
        except Exception:
            pass
        try:
            self.browser.close()
        except Exception:
            pass
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
        piece = stream.read(count - len(data))
        if not piece:
            return None
        data.extend(piece)
    return bytes(data)


def product_audio(session, item):
    if not item.get('audio'):
        return None
    import shutil
    player = shutil.which('kilix-mpv') or shutil.which('mpv')
    if player is None:
        return None
    directory = Path(tempfile.mkdtemp(prefix='zell-theater-'))
    session.socket_dirs.append(directory)
    socket_path = directory / 'mpv.sock'
    argv = [
        player, '--no-config', '--no-video', '--no-terminal', '--really-quiet',
        f'--input-ipc-server={socket_path}', f"--start={item.get('position_ms', 0) / 1000:.3f}",
        '--keep-open=no', '--audio-display=no', '--', item['path'],
    ]
    return spawn(argv)


def serve(stdin, stdout, session, frame_fd=None):
    session.attach(frame_fd)
    for event in session.start():
        _write(stdout, event)
    while not session.stopped:
        line = stdin.readline()
        if not line:
            break
        if not line.strip():
            continue
        for event in session.handle_line(line):
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
    browser = PrivateBrowser()
    session = TheaterSession(runtime, browser=browser, play_frames=True)
    session.audio_launcher = lambda item: product_audio(session, item)
    raw = os.environ.get('ZELL_THEATER_FD')
    frame_fd = int(raw) if raw else None
    try:
        return serve(sys.stdin.buffer, sys.stdout.buffer, session, frame_fd)
    finally:
        session.shutdown()


if __name__ == '__main__':
    sys.exit(main())
