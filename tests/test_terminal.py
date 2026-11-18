import contextlib
import io
import json
import mmap
import os
import subprocess
from pathlib import Path
import pty
import select
import signal
import struct
import sys
import tempfile
import time
import traceback
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import zell_debug as debug
import zell_terminal as terminal
import zell_update as update
from zell_dialogue import Dialogue


class UpdateTests(unittest.TestCase):
    def setUp(self):
        quiet=patch('zell_schedule.quiet_now',return_value=True)
        quiet.start();self.addCleanup(quiet.stop)

    def test_success_exact_command_private_atomic_stamp(self):
        # The log on, as it is by default: it is in the state folder too.
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, KILIX_ZELL_STATE=directory), \
                patch.object(update, 'run_to_completion', return_value=0) as run, \
                patch('builtins.input', return_value=''), contextlib.redirect_stdout(io.StringIO()):
            os.environ.pop('KILIX_ZELL_DEBUG_LOG', None)
            self.assertEqual(update.main(), 0)
            run.assert_called_once_with('sudo apt update && sudo apt -y upgrade')
            path = Path(directory)/'os-update.json'
            self.assertLess(abs(json.loads(path.read_text())['completed_at'] - time.time()), 5)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual({p.name for p in Path(directory).iterdir()},
                             {'os-update.json','os-jobs.json','notices.json','.os.lock','zell-log.jsonl'})
            jobs=json.loads((Path(directory)/'os-jobs.json').read_text())
            self.assertEqual(next(iter(jobs.values()))['status'],'completed')

    def test_failure_and_interrupt_do_not_stamp(self):
        # -2 is a shell killed by the terminal's Ctrl-C.
        for status, expected in ((7, 7), (-2, 130)):
            with self.subTest(status=status), tempfile.TemporaryDirectory() as directory, \
                    patch.dict(os.environ, KILIX_ZELL_STATE=directory), \
                    patch.object(update, 'run_to_completion', return_value=status), \
                    patch('builtins.input', return_value=''), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(update.main(), expected)
                self.assertFalse((Path(directory)/'os-update.json').exists())

    def interrupted_run(self, command):
        """Run the real runner in a PTY, press Ctrl-C once it starts, return its screen text."""
        with tempfile.TemporaryDirectory() as directory:
            pid, fd = pty.fork()
            if pid == 0:
                os.environ['KILIX_ZELL_STATE'] = directory
                update.COMMAND = command
                try:
                    os._exit(update.main())
                except BaseException:
                    traceback.print_exc()
                finally:
                    os._exit(99)
            output, sent, answered, deadline = b'', False, False, time.monotonic() + 20
            try:
                while time.monotonic() < deadline:
                    if not select.select([fd], [], [], .1)[0]:
                        continue
                    try:
                        chunk = os.read(fd, 4096)
                    except OSError:
                        break
                    if not chunk:
                        break
                    output += chunk
                    if not sent and b'STARTED' in output:
                        sent = True
                        # Let the shell reach its next foreground command: dash
                        # swallows an interrupt that lands between two commands.
                        time.sleep(.3)
                        os.write(fd, b'\x03')
                    if not answered and b'Press Enter' in output:
                        answered = True
                        os.write(fd, b'\n')
            finally:
                os.close(fd)
                status = os.waitstatus_to_exitcode(os.waitpid(pid, 0)[1])
            daemon = Path(directory, 'daemon')
            if daemon.exists():
                with contextlib.suppress(OSError, ValueError):
                    os.killpg(int(daemon.read_text()), signal.SIGKILL)
            self.assertFalse(Path(directory, 'os-update.json').exists())
            return output.decode(errors='replace'), status

    def test_interrupt_waits_for_processes_that_outlive_the_shell(self):
        # The shell dies of Ctrl-C at once; its child ignores it and works on.
        # The prompt to leave must not appear until that child has finished.
        text, status = self.interrupted_run(
            "(trap '' INT; echo STAR''TED; sleep 1; echo LATE-CHILD) & wait")
        self.assertEqual(status, 130, text)
        self.assertIn('LATE-CHILD', text)
        self.assertRegex(text, r'left \d+ process\(es\) running')
        self.assertLess(text.index('LATE-CHILD'), text.index('Press Enter'), text)
        self.assertIn('did not complete (exit -2)', text)

    def test_orphans_are_reaped_while_the_command_still_runs(self):
        # The inner shell exits at once and orphans a short-lived child, which
        # this process adopts. It must be reaped during the run, not left a
        # zombie until the end, and the command's own status must survive.
        probe = ("sh -c 'sleep .2 & echo $! >\"$KILIX_ZELL_STATE/orphan\"'; sleep .8; "
                 "grep -c '^State:.Z' /proc/$(cat \"$KILIX_ZELL_STATE/orphan\")/status "
                 ">\"$KILIX_ZELL_STATE/zombie\" 2>/dev/null; exit 5")
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, KILIX_ZELL_STATE=directory):
            self.assertEqual(update.run_to_completion(probe), 5)
            self.assertTrue(Path(directory, 'orphan').read_text().strip().isdigit())
            # Reaped: its /proc entry is gone, so grep found no zombie line.
            self.assertEqual(Path(directory, 'zombie').read_text().strip(), '')

    def test_interrupt_does_not_wait_for_a_daemon_that_left_the_session(self):
        text, status = self.interrupted_run(
            "setsid sh -c 'sleep 30' >/dev/null 2>&1 & echo $! >\"$KILIX_ZELL_STATE/daemon\"; "
            "echo STAR''TED; sleep 30")
        self.assertEqual(status, 130, text)
        self.assertNotIn('process(es) running', text)

    def test_the_log_is_on_by_default_and_off_when_asked(self):
        with tempfile.TemporaryDirectory() as directory:
            default = Path(directory, 'state', 'zell-log.jsonl')
            named = Path(directory, 'named.jsonl')
            state = str(Path(directory, 'state'))
            for setting, written in ((None, default), ('off', None), ('OFF', None), (str(named), named)):
                env = dict(KILIX_ZELL_STATE=state)
                with patch.dict(os.environ, env), patch.object(update, 'COMMAND', 'exit 3'), \
                        patch('builtins.input', return_value='hunter2'), contextlib.redirect_stdout(io.StringIO()):
                    os.environ.pop('KILIX_ZELL_DEBUG_LOG', None)
                    if setting is not None:
                        os.environ['KILIX_ZELL_DEBUG_LOG'] = setting
                    self.assertEqual(debug.path(), written and str(written))
                    sizes = {p: p.stat().st_size for p in (default, named) if p.exists()}
                    self.assertEqual(update.main(), 3)
                for p in (default, named):
                    grew = p.exists() and p.stat().st_size > sizes.get(p, 0)
                    self.assertEqual(grew, p == written, (setting, p))
            events = [json.loads(line) for line in default.read_text().splitlines()]
            self.assertEqual([e['event'] for e in events], ['update.started', 'update.shell_exited', 'update.finished'])
            self.assertEqual(events[1]['status'], 3)
            self.assertEqual(default.stat().st_mode & 0o777, 0o600)
            self.assertNotIn('hunter2', default.read_text() + named.read_text())  # typed input is never logged

    def test_the_log_is_kept_to_two_files(self):
        with tempfile.TemporaryDirectory() as directory:
            # In folders not made yet: the log makes them.
            where = Path(directory, 'not', 'yet', 'log.jsonl')
            with patch.dict(os.environ, KILIX_ZELL_DEBUG_LOG=str(where)), patch.object(debug, 'LOG_BYTES', 200):
                for n in range(40):
                    debug.log('tick', n=n)
            old, new = Path(str(where) + '.1'), where
            self.assertLess(new.stat().st_size, 200 + 100)
            self.assertLess(old.stat().st_size, 200 + 100)
            numbers = [json.loads(line)['n'] for path in (old, new) for line in path.read_text().splitlines()]
            self.assertEqual(numbers, list(range(numbers[0], 40)))
            self.assertFalse(Path(str(where) + '.2').exists())
            self.assertEqual(where.parent.stat().st_mode & 0o777, 0o700)

    def test_a_rotation_waits_for_another_and_keeps_its_file(self):
        # Helper B finds the log full and waits for the lock while helper A
        # moves it aside and starts a new one: B then writes to the new one
        # and does not move it over the full one A kept.
        with tempfile.TemporaryDirectory() as directory:
            where = Path(directory, 'log.jsonl')
            where.write_text(''.join(json.dumps({'event': 'old', 'n': n}) + '\n' for n in range(60)))
            real, state = debug.fcntl.flock, {'inside': False}

            def flock(fd, how):
                if not state['inside']:
                    state['inside'] = True
                    debug.log('from A')
                real(fd, how)
            with patch.dict(os.environ, KILIX_ZELL_DEBUG_LOG=str(where)), patch.object(debug, 'LOG_BYTES', 1000), \
                    patch.object(debug.fcntl, 'flock', flock):
                debug.log('from B')
            old = [json.loads(line)['event'] for line in Path(str(where) + '.1').read_text().splitlines()]
            new = [json.loads(line)['event'] for line in where.read_text().splitlines()]
            self.assertEqual((old, new), (['old'] * 60, ['from A', 'from B']))

    def test_a_reopen_that_fails_after_a_rotation_closes_once(self):
        with tempfile.TemporaryDirectory() as directory:
            where = Path(directory, 'log.jsonl')
            where.write_text('x' * 2000)
            opens, closes, real_open, real_close = [], [], debug._open, os.close

            def failing_open(path):
                opens.append(path)
                if len(opens) == 2:
                    raise OSError(28, 'No space left on device')
                return real_open(path)

            def close(fd):
                closes.append(fd)
                real_close(fd)
            with patch.dict(os.environ, KILIX_ZELL_DEBUG_LOG=str(where)), patch.object(debug, 'LOG_BYTES', 1000), \
                    patch.object(debug, '_open', failing_open), patch.object(debug.os, 'close', close):
                debug.log('lost')
            self.assertEqual(len(opens), 2)
            self.assertEqual(len(closes), len(set(closes)), closes)
            self.assertEqual(len(closes), 1)

    def test_the_vitals_monitor_stopped_is_logged_as_done_not_crashed(self):
        helper = Path(__file__).resolve().parent.parent / 'tools' / 'desktop.py'
        with tempfile.TemporaryDirectory() as directory:
            where = Path(directory, 'log.jsonl')
            env = dict(os.environ, KILIX_ZELL_DEBUG_LOG=str(where), KILIX_ZELL_STATE=directory)
            child = subprocess.Popen([sys.executable, str(helper), '-1', '0', '--dialogue'], env=env,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            try:
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline and b'"vitals"' not in (where.read_bytes() if where.exists() else b''):
                    time.sleep(0.1)
                child.send_signal(signal.SIGTERM)
                child.wait(timeout=20)
            finally:
                if child.poll() is None:
                    child.kill()
                    child.wait()
                child.stdin.close()
                child.stdout.close()
            events = [json.loads(line)['event'] for line in where.read_text().splitlines()]
            self.assertIn('vitals', events)
            self.assertEqual(events[-1], 'helper.done', events)
            self.assertNotIn('helper.crashed', events)

    def test_a_floating_terminal_that_fails_says_where_the_log_is(self):
        with tempfile.TemporaryDirectory() as directory:
            where = Path(directory, 'log.jsonl')
            ui = SimpleNamespace(DIALOGUE=SimpleNamespace(), page_text=Mock())
            with patch.dict(os.environ, KILIX_ZELL_DEBUG_LOG=str(where)):
                os.environ.pop('ZELL_TERMINAL_FD', None)
                self.assertIsNone(terminal.terminal_loop(ui, ['true'], title='t'))
                self.assertIn(f'What went wrong is in {where}.', ui.page_text.call_args.args[0])
                self.assertIn("KeyError('ZELL_TERMINAL_FD')", where.read_text())
                self.assertIsNone(terminal.terminal_loop(ui, ['true'], title='t', sensitive=True))
                self.assertNotIn('What went wrong', ui.page_text.call_args.args[0])

    def test_confirm_or_cancel(self):
        ui = SimpleNamespace(choose=Mock(return_value=None))
        with patch.object(terminal, 'terminal_loop', return_value=0) as loop:
            terminal.run_update(ui)
            loop.assert_not_called()
            ui.choose.return_value = 'run'
            self.assertEqual(terminal.run_update(ui), 0)
            self.assertIn(update.COMMAND, ui.choose.call_args.args[0])
            self.assertEqual(loop.call_args.kwargs['command'],
                             [sys.executable, str(Path(update.__file__))])


class TerminalTests(unittest.TestCase):
    def test_fragmented_and_multiple_keys_bypass_workspace_prefix(self):
        pane = SimpleNamespace(send=Mock())
        pending = bytearray()
        terminal.forward_input(pending, b'K01', pane)
        terminal.forward_input(pending, b'71\nK1b\nK6162630d\nKnope\n', pane)
        self.assertEqual([c.args[0] for c in pane.send.call_args_list], [b'\x01q', b'\x1b', b'abc\r'])
        self.assertEqual(pending, b'')

    def test_argb_frame_header_and_dimension_bounds(self):
        import numpy as np
        shared = bytearray(terminal.BUFFER_SIZE)
        struct.pack_into('<II', shared, 0, 999, 1)
        self.assertEqual(terminal.dimensions(shared), (150, 8))
        terminal.publish_frame(shared, np.array([[[0x12, 0x34, 0x56], [1, 2, 3]]], dtype=np.uint8))
        self.assertEqual(struct.unpack_from('<5I', shared), (999, 1, 2, 1, 2))
        self.assertEqual(shared[32:40], b'\x56\x34\x12\xff\x03\x02\x01\xff')

    def test_small_workspace_resize(self):
        display = terminal.load_display()
        workspace = terminal.make_workspace(display, 80, 25, ['/bin/sh', '-c', 'exit 0'])
        try:
            workspace.resize(30, 8)
            self.assertEqual((workspace.cols, workspace.rows), (30, 8))
            self.assertEqual(len(workspace.grid()), 240)
            workspace.resize(40, 20)
            self.assertEqual(len(workspace.grid()), 800)
        finally:
            workspace.close()

    def test_real_harmless_pty_frames_and_input(self):
        # Uses the existing CPU renderer; never invokes the update runner.
        try:
            display = terminal.load_display()
            if not (Path(display.__file__).parent/'build/libneuronte-display.so').exists():
                self.skipTest('Neuronte native parser is not built')
        except (ImportError, RuntimeError):
            self.skipTest('Neuronte renderer is unavailable')
        home = Path(os.environ.get('NEURONTE_HOME', Path.home()/'projects/neuronte'))
        if not (home/'build/libneuronte.so').exists():
            self.skipTest('Neuronte CPU mask library is not built')
        fd = os.memfd_create('zell-test', 0)
        os.ftruncate(fd, terminal.BUFFER_SIZE)
        shared = mmap.mmap(fd, terminal.BUFFER_SIZE)
        struct.pack_into('<II', shared, 0, 30, 8)
        read_fd, write_fd = os.pipe()
        try:
            os.write(write_fd, b'K68656c6c6f0d\n')
            with os.fdopen(read_fd, 'r') as reader, tempfile.TemporaryDirectory() as cache, \
                    patch.dict(os.environ, ZELL_TERMINAL_FD=str(fd), XDG_CACHE_HOME=cache):
                writer = io.StringIO()
                ui = SimpleNamespace(DIALOGUE=Dialogue(reader, writer), page_text=Mock())
                result = terminal.terminal_loop(ui, [sys.executable, '-c',
                    'import sys; print("PTY ready", flush=True); s=input(); sys.exit(0 if s=="hello" else 3)'])
                self.assertEqual(result, 0)
                ui.page_text.assert_not_called()
                self.assertEqual(writer.getvalue(), 'T\n')
                _, _, width, height, sequence = struct.unpack_from('<5I', shared)
                self.assertEqual((width, height), (240, 128))
                self.assertGreater(sequence, 0)
                self.assertEqual(sequence % 2, 0)
                self.assertEqual(shared[35], 255)
        finally:
            os.close(write_fd)
            shared.close()
            os.close(fd)


if __name__ == '__main__':
    unittest.main()
