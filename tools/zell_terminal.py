"""Neuronte PTY frames shared with Zell's native floating terminal."""
import importlib.util
import mmap
import os
from pathlib import Path
import select
import signal
import struct
import sys
import time

from zell_update import COMMAND
import zell_debug

HEADER_SIZE = 32
MAX_WIDTH, MAX_HEIGHT = 1200, 640
BUFFER_SIZE = HEADER_SIZE + MAX_WIDTH * MAX_HEIGHT * 4


def load_display():
    root = Path(os.environ.get('GPU_TERMINAL_HOME', Path.home()/'.local/gpu_terminal'))
    override = os.environ.get('KILIX_ZELL_NEURONTE_DISPLAY')
    paths = [Path(override).expanduser()] if override else [
        Path(__file__).resolve().parents[1]/'libexec/neuronte-display/neuronte_display.py',
        Path(os.environ.get('GPU_TERMINAL_SOURCE_HOME', Path.home()/'gpu_terminal')) /
        'kilix-modules/neuronte-display/neuronte_display.py',
        Path.home()/'gpu_terminal/kilix-modules/neuronte-display/neuronte_display.py',
        root/'kilix-modules/neuronte-display/neuronte_display.py',
        Path(__file__).resolve().parents[3]/'kilix-modules/neuronte-display/neuronte_display.py',
    ]
    for path in paths:
        if path.is_dir():
            path = path/'neuronte_display.py'
        if path.is_file():
            spec = importlib.util.spec_from_file_location('zell_neuronte_display', path)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            return module
    raise RuntimeError('Neuronte display is unavailable. Set KILIX_ZELL_NEURONTE_DISPLAY to neuronte_display.py.')


def renderer_paths():
    app = Path(__file__).resolve().parents[1]
    home = Path(os.environ.get('NEURONTE_HOME', Path.home()/'projects/neuronte'))
    weights = app/'weights/neuronte-split-v4.nrt'
    library = app/'lib/libneuronte.so'
    return (Path(os.environ.get('ZELL_NEURONTE_WEIGHTS', weights if weights.is_file() else home/'weights/neuronte-split-v4.nrt')),
            Path(os.environ.get('ZELL_NEURONTE_LIBRARY', library if library.is_file() else home/'build/libneuronte.so')))


def dimensions(shared):
    cols, rows = struct.unpack_from('<II', shared)
    return max(20, min(150, cols)), max(8, min(40, rows))


def publish_frame(shared, frame):
    # Native little-endian ARGB is stored as B, G, R, A bytes.
    import numpy as np
    height, width, channels = frame.shape
    if channels != 3 or width > MAX_WIDTH or height > MAX_HEIGHT:
        raise ValueError('Terminal frame exceeds the shared surface')
    pixels = np.empty((height, width, 4), dtype=np.uint8)
    pixels[:, :, :3] = frame[:, :, ::-1]
    pixels[:, :, 3] = 255
    payload = pixels.tobytes()
    sequence = (struct.unpack_from('<I', shared, 16)[0] + 1) | 1
    struct.pack_into('<I', shared, 16, sequence & 0xffffffff)
    shared[HEADER_SIZE:HEADER_SIZE + len(payload)] = payload
    struct.pack_into('<II', shared, 8, width, height)
    struct.pack_into('<I', shared, 16, (sequence + 1) & 0xffffffff)


def forward_input(pending, data, pane):
    """Consume every complete K frame; retain only an incomplete wire frame."""
    pending.extend(data)
    while b'\n' in pending:
        line, _, rest = pending.partition(b'\n')
        pending[:] = rest
        if line[:1] != b'K':
            continue
        try:
            raw = bytes.fromhex(line[1:].decode('ascii'))
        except (ValueError, UnicodeDecodeError):
            continue
        pane.send(raw)
    if len(pending) > 131072:
        raise RuntimeError('Terminal input frame is too large')


def make_workspace(display, cols, rows, command, title='OS update / keyboard goes to terminal / Ctrl-C interrupts'):
    class FloatingWorkspace(display.Workspace):
        def resize(self, cols, rows):
            # The upstream presenter enforces 18 rows, but its layout supports
            # a smaller single pane in this embedded surface.
            if rows >= 18:
                return super().resize(cols, rows)
            self.cols, self.rows = cols, rows
            for tab in self.tabs:
                self.layout(tab)

        def grid(self):
            records = super().grid()
            label = title
            records[-self.cols:] = [(ord(c), display.FG, 0x263448, 0)
                                    for c in label[:self.cols].ljust(self.cols)]
            return records
    return FloatingWorkspace(cols, rows, 8, 16, command)


def terminal_loop(ui, command, title=None, sensitive=False):
    workspace = cache = shared = None
    old_signals = {}

    def disconnected(_signum, _frame):
        raise EOFError

    try:
        if not ui.DIALOGUE:
            raise RuntimeError('The floating terminal requires the native room interface.')
        for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
            old_signals[sig] = signal.signal(sig, disconnected)
        fd = int(os.environ['ZELL_TERMINAL_FD'])
        os.set_inheritable(fd, False)
        shared = mmap.mmap(fd, BUFFER_SIZE)
        display = load_display()
        cache_dir = Path(os.environ.get('XDG_CACHE_HOME', Path.home()/'.cache'))/'neuronte-display'
        weights, library = renderer_paths()
        cache = display.MaskCache(weights, library, cache_dir, 8, 16)
        renderer = display.Renderer(cache)
        cols, rows = dimensions(shared)
        workspace = (make_workspace(display, cols, rows, command, title=title) if title
                     else make_workspace(display, cols, rows, command))
        if not sensitive:
            zell_debug.log('terminal.opened', cols=cols, rows=rows, command=command, display=display.__file__)
        input_fd = ui.DIALOGUE.reader.fileno()
        pending = bytearray()
        ui.DIALOGUE.send('T', '')
        dirty, next_frame = True, 0.0
        while True:
            requested = dimensions(shared)
            if requested != (workspace.cols, workspace.rows):
                workspace.resize(*requested)
                dirty = True
            # Poll the PTY independently of the 10 Hz rendering limit.
            dirty = workspace.poll(0.01, keyboard=False) or dirty
            if select.select([input_fd], [], [], 0)[0]:
                data = os.read(input_fd, 65536)
                if not data:
                    raise EOFError
                forward_input(pending, data, workspace.selected)
            now = time.monotonic()
            if dirty and now >= next_frame:
                publish_frame(shared, renderer.render(workspace.grid(), workspace.cols, workspace.rows))
                dirty = False
                next_frame = time.monotonic() + 0.1
            pane = workspace.selected
            if pane.exit_status is not None and pane.fd is None:
                # Wait for the next frame slot so even the final render is capped.
                if dirty:
                    continue
                zell_debug.log('terminal.command_exited', status=pane.exit_status)
                return pane.exit_status
    except EOFError:
        zell_debug.log('terminal.closed_by_room')
        return None
    except (OSError, ValueError, RuntimeError, ImportError, KeyError):
        if not sensitive:
            zell_debug.error('terminal.failed')
        # Stop the PTY before waiting for acknowledgement of a failed surface.
        if workspace is not None:
            workspace.close()
            workspace = None
        # Never include arbitrary terminal data or input in the dialogue protocol.
        ui.page_text('Could not open or continue the floating Neuronte terminal. '
                     'Check the Neuronte display library, weights, and shared surface configuration.')
        return None
    finally:
        if workspace is not None:
            workspace.close()
        if cache is not None:
            cache.close()
        if shared is not None:
            if sensitive:
                # The renderer's output may include a recovery code. Remove
                # every shared frame before the surface is handed back.
                shared[8:] = b'\0' * (BUFFER_SIZE - 8)
            shared.close()
        for sig, handler in old_signals.items():
            signal.signal(sig, handler)


def run_update(ui):
    choice = ui.choose('Update the OS?\n' + COMMAND +
                       '\nType your sudo password only in the floating terminal.',
                       [('Run OS update', 'run'), ('Cancel', None)])
    zell_debug.log('update.confirmation', choice=choice)
    if choice != 'run':
        return None
    return terminal_loop(ui, command=[sys.executable, str(Path(__file__).with_name('zell_update.py'))])
