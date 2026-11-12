"""Neuronte shell that follows the host pane's pixel geometry."""
import fcntl
import os
from pathlib import Path
import signal
import struct
import sys
import termios
import tty

from zell_terminal import load_display, renderer_paths


def geometry(fd=1):
    rows, cols, width, height = struct.unpack('HHHH', fcntl.ioctl(fd, termios.TIOCGWINSZ, bytes(8)))
    return (max(20, min(240, width//8 if width else cols or 80)),
            max(18, min(120, height//16 if height else rows or 25)))


def resize(workspace, presenter, size):
    if size == (workspace.cols, workspace.rows):
        return presenter, False
    workspace.lib.nd_presenter_close(presenter)
    workspace.resize(*size)
    presenter = workspace.lib.nd_presenter_open(workspace.cols*8, workspace.rows*16)
    if not presenter:
        raise RuntimeError('Cannot resize Neuronte presentation.')
    return presenter, True


def drop_exited(workspace):
    """Close panes whose program has ended; quit once none remain.

    The display keeps a finished pane on screen. For a shell that leaves
    `exit` with nothing to do, so finish the way a terminal tab does.
    """
    changed = False
    for tab in list(workspace.tabs):
        for pane in [p for p in tab['panes'] if p.exit_status is not None]:
            tab['panes'].remove(pane)
            pane.close()
            changed = True
        if not tab['panes']:
            workspace.tabs.remove(tab)
    if not workspace.tabs:
        workspace.quit = True
    elif changed:
        workspace.tab = min(workspace.tab, len(workspace.tabs)-1)
        workspace.focus = min(workspace.focus, len(workspace.current['panes'])-1)
        for tab in workspace.tabs:
            workspace.layout(tab)
    return changed


def main(argv=None):
    import numpy as np
    argv = list(sys.argv[1:] if argv is None else argv)
    command = argv[1:] if argv[:1] == ['--'] else argv
    if not os.isatty(0) or not os.isatty(1):
        raise RuntimeError('The Neuronte shell requires a terminal.')
    display = load_display()
    weights, library = renderer_paths()
    cache = display.MaskCache(weights, library,
        Path(os.environ.get('XDG_CACHE_HOME', Path.home()/'.cache'))/'neuronte-display', 8, 16)
    workspace = presenter = saved = None
    old_signals = {}
    resized = True
    try:
        workspace = display.Workspace(*geometry(), 8, 16, command or None)
        renderer = display.Renderer(cache)
        saved = termios.tcgetattr(0)
        tty.setraw(0)
        presenter = workspace.lib.nd_presenter_open(workspace.cols*8, workspace.rows*16)
        if not presenter:
            raise RuntimeError('Cannot start Neuronte presentation.')
        def stop(_signum, _frame):
            workspace.quit = True
        def winch(_signum, _frame):
            nonlocal resized
            resized = True
        for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
            old_signals[sig] = signal.signal(sig, stop)
        old_signals[signal.SIGWINCH] = signal.signal(signal.SIGWINCH, winch)
        dirty = True
        while not workspace.quit:
            if resized:
                resized = False
                size = geometry()
                previous, presenter = presenter, None
                presenter, changed = resize(workspace, previous, size)
                dirty = dirty or changed
            if dirty:
                frame = renderer.render(workspace.grid(), workspace.cols, workspace.rows)
                rgba = np.concatenate((frame, np.full((*frame.shape[:2], 1), 255, np.uint8)), axis=2)
                if workspace.lib.nd_present(presenter, rgba.ctypes.data, frame.shape[1], frame.shape[0]):
                    raise RuntimeError('Neuronte presentation failed.')
            dirty = workspace.poll(.04, keyboard=True)
            dirty = drop_exited(workspace) or dirty
    finally:
        if presenter:
            workspace.lib.nd_presenter_close(presenter)
        if saved is not None:
            termios.tcsetattr(0, termios.TCSADRAIN, saved)
        if workspace:
            workspace.close()
        cache.close()
        for sig, handler in old_signals.items():
            signal.signal(sig, handler)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
