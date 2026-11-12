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

    def _run_until(self, workspace, done, seconds=5):
        deadline = time.monotonic()+seconds
        while time.monotonic() < deadline:
            workspace.poll(.01, keyboard=False)
            shell.drop_exited(workspace)
            if done():
                return True
        return False

    def test_exit_in_the_only_shell_ends_the_session(self):
        display = terminal.load_display()
        workspace = display.Workspace(80, 25, 8, 16, ['/bin/sh', '-c', 'exit 0'])
        try:
            self.assertFalse(workspace.quit)
            self.assertTrue(self._run_until(workspace, lambda: workspace.quit),
                            'the session must end when its only shell exits')
            self.assertEqual(workspace.tabs, [])
        finally:
            workspace.close()

    def test_exit_in_one_split_keeps_the_other_shell(self):
        display = terminal.load_display()
        marker = Path(os.environ.get('TMPDIR', '/tmp'))/f'zell-shell-test-{os.getpid()}'
        marker.unlink(missing_ok=True)
        # The first pane lives; later panes exit once the marker exists.
        workspace = display.Workspace(80, 25, 8, 16,
            ['/bin/sh', '-c', f'test -e {marker} && exit 0; printf READY; sleep 30'])
        try:
            keeper = workspace.selected
            self.assertTrue(self._run_until(workspace,
                lambda: 'READY' in ''.join(chr(c[0]) for c in workspace.grid())), 'first shell did not start')
            marker.touch()
            workspace.split(True)
            self.assertEqual(len(workspace.current['panes']), 2)
            self.assertTrue(self._run_until(workspace, lambda: len(workspace.panes()) == 1),
                            'the finished split must close')
            self.assertFalse(workspace.quit)
            self.assertIs(workspace.selected, keeper)
            self.assertIsNone(keeper.exit_status)
            self.assertEqual(keeper.cols, 80)
        finally:
            marker.unlink(missing_ok=True)
            workspace.close()


if __name__ == '__main__':
    unittest.main()
