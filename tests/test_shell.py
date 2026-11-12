import fcntl
import os
from pathlib import Path
import pty
import struct
import sys
import termios
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import zell_shell as shell
import zell_terminal as terminal


class ShellTests(unittest.TestCase):
    def test_actual_pty_geometry_follows_pixel_resize(self):
        master, slave = pty.openpty()
        try:
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 80, 1904, 1035))
            self.assertEqual(shell.geometry(slave), (238, 64))
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 20, 60, 800, 480))
            self.assertEqual(shell.geometry(slave), (100, 30))
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 100, 0, 0))
            self.assertEqual(shell.geometry(slave), (100, 30))
        finally:
            os.close(master); os.close(slave)

    def test_resize_keeps_shell_and_updates_pty(self):
        display = terminal.load_display()
        workspace = display.Workspace(80, 25, 8, 16, ['/bin/sh', '-c', 'printf READY; sleep 30'])
        try:
            pane = workspace.selected
            deadline = time.monotonic()+3
            while time.monotonic()<deadline:
                workspace.poll(.01, keyboard=False)
                if 'READY' in ''.join(chr(c[0]) for c in workspace.grid()):
                    break
            else:
                self.fail('Shell did not start')
            old_fd, old_pid = pane.fd, pane.pid
            # The framebuffer is replaced, but the real PTY remains alive.
            native = workspace.lib
            workspace.lib = SimpleNamespace(nd_presenter_close=Mock(), nd_presenter_open=Mock(return_value=456))
            presenter, changed = shell.resize(workspace, 123, (140, 40))
            self.assertEqual((presenter, changed), (456, True))
            self.assertEqual((pane.fd, pane.pid), (old_fd, old_pid))
            rows, cols, _, _ = struct.unpack('HHHH', fcntl.ioctl(pane.fd, termios.TIOCGWINSZ, bytes(8)))
            self.assertGreater(cols, 80)
            self.assertGreater(rows, 25)
            workspace.lib.nd_presenter_open.assert_called_once_with(1120, 640)
            workspace.lib = native
        finally:
            workspace.close()


if __name__ == '__main__':
    unittest.main()
