#!/usr/bin/env python3
"""Run the explicitly confirmed OS update inside the floating terminal's PTY."""
import ctypes
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

import zell_debug

COMMAND = 'sudo apt update && sudo apt -y upgrade'


def record_success():
    root = Path(os.environ.get('GPU_TERMINAL_HOME', Path.home()/'.local/gpu_terminal'))
    directory = Path(os.environ.get('KILIX_ZELL_STATE', root/'kilix-zell/state'))
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd, temporary = tempfile.mkstemp(prefix='.os-update-', dir=directory)
    try:
        with os.fdopen(fd, 'w') as stream:
            json.dump({'completed_at': time.time()}, stream)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, directory/'os-update.json')
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def adopt_orphans():
    """Become the parent of orphaned descendants, so they stay findable."""
    try:
        ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0)  # PR_SET_CHILD_SUBREAPER
    except (OSError, AttributeError):
        pass


def stragglers():
    """Live descendants still attached to this terminal session.

    A daemon that left the session is not waited for; a process that merely
    outlived the shell (for example after Ctrl-C) is.
    """
    session, parents, live = os.getsid(0), {}, set()
    for entry in os.scandir('/proc'):
        if not entry.name.isdigit():
            continue
        try:
            fields = Path(entry.path, 'stat').read_text().rsplit(')', 1)[1].split()
        except (OSError, IndexError):
            continue
        parents[int(entry.name)] = int(fields[1])
        if fields[0] != 'Z' and int(fields[3]) == session:
            live.add(int(entry.name))
    found = []
    for pid in live:
        parent = parents.get(pid)
        while parent and parent != os.getpid():
            parent = parents.get(parent)
        if parent:
            found.append(pid)
    return found


def run_to_completion(command):
    """Run command; return only when it and its terminal descendants are gone.

    Ctrl-C reaches the whole foreground group from the terminal, so here it is
    only noted, never turned into a kill or an early return: the prompt must
    not appear while work continues.
    """
    previous = signal.signal(signal.SIGINT, lambda number, frame: zell_debug.log('update.interrupt'))
    try:
        adopt_orphans()
        process = subprocess.Popen(['/bin/sh', '-c', command])
        zell_debug.log('update.started', command=command, shell_pid=process.pid)
        # Adopted orphans exit before the shell does; reap them as they go so a
        # long update does not collect zombies. WNOWAIT leaves the shell's own
        # status for Popen.
        while True:
            try:
                ended = os.waitid(os.P_ALL, 0, os.WEXITED | os.WNOWAIT)
            except ChildProcessError:
                break
            if ended is None or ended.si_pid == process.pid:
                break
            os.waitpid(ended.si_pid, 0)
        result = process.wait()
        zell_debug.log('update.shell_exited', status=result)
        announced = False
        while True:
            try:
                while os.waitpid(-1, os.WNOHANG)[0]:
                    pass
            except ChildProcessError:
                pass
            left = stragglers()
            if not left:
                return result
            if not announced:
                announced = True
                zell_debug.log('update.waiting_for_stragglers', pids=left)
                print(f'\nThe command left {len(left)} process(es) running in this terminal. '
                      'Waiting for them to finish; Ctrl-C interrupts them.', flush=True)
            time.sleep(.2)
    finally:
        signal.signal(signal.SIGINT, previous)


def main():
    print('$ ' + COMMAND, flush=True)
    result = run_to_completion(COMMAND)
    if result == 0:
        try:
            record_success()
            zell_debug.log('update.recorded_success')
            print('\nOS update completed successfully.', flush=True)
        except OSError:
            zell_debug.error('update.record_failed')
            print('\nOS update completed, but its completion time could not be saved.', flush=True)
    else:
        print(f'\nOS update did not complete (exit {result}).', flush=True)
    try:
        input('Press Enter to return to the room. ')
    except (EOFError, KeyboardInterrupt):
        pass
    zell_debug.log('update.finished', status=result)
    return result if result >= 0 else 128 - result


if __name__ == '__main__':
    raise SystemExit(main())
