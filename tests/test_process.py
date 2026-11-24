"""Run the question pipeline with a controlling tty, like a Kilix pane."""
import os
import pathlib
import pty
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time

with tempfile.TemporaryDirectory(prefix="zell-process-") as directory:
    app = pathlib.Path(directory)/"install"
    app.mkdir()
    binary = app/"zell"
    shutil.copy2(sys.argv[1], binary)
    with subprocess.Popen([binary, "--test-runtime-upgrade"], stdin=subprocess.PIPE, stdout=subprocess.PIPE) as probe:
        assert select.select([probe.stdout], [], [], 5)[0] and probe.stdout.readline() == b"ready\n"
        previous = app.with_name("previous")
        app.rename(previous)
        (app/"tools").mkdir(parents=True)
        (app/"tools/desktop.py").touch()
        shutil.rmtree(previous)
        probe.communicate(b"\n", timeout=5)
        assert probe.returncode == 0, "the running game lost its station helpers after an upgrade"
    fake = pathlib.Path(directory) / "kilix-llm"
    fake.write_text("""#!/usr/bin/env python3
import os, sys, time
try:
    fd = os.open('/dev/tty', os.O_WRONLY)
except OSError:
    pass
else:
    os.write(fd, b'TERMINAL LEAK\\n')
    sys.exit(42)
if sys.argv[-1].endswith('wait'):
    print('ready', flush=True)
    time.sleep(30)
else:
    print('Loading model...\\n> Hello\\n\\nHello from the model.\\n\\n[ Prompt: 1 token ]\\nExiting...')
""")
    fake.chmod(0o700)
    # Only this stub can receive a privilege request in the maintenance test.
    sudo = pathlib.Path(directory)/'sudo'
    sudo.write_text("""#!/bin/sh
echo "simulated-$* command"
""")
    sudo.chmod(0o700)
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["PATH"] = directory + os.pathsep + os.environ["PATH"]
        os.environ["ZELL_TEST_MAINTENANCE"] = directory
        os.execv(sys.argv[1], [sys.argv[1]])
    output = bytearray()
    deadline = time.monotonic() + 15
    try:
        while True:
            if time.monotonic() > deadline:
                os.kill(pid, signal.SIGKILL)
                raise AssertionError("question pipeline hung")
            if select.select([fd], [], [], 0.1)[0]:
                try:
                    chunk = os.read(fd, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                output.extend(chunk)
    finally:
        os.close(fd)
        _, status = os.waitpid(pid, 0)
    assert os.waitstatus_to_exitcode(status) == 0, output.decode(errors="replace")
    assert b"TERMINAL LEAK" not in output, output
    assert b"process test ok" in output, output
    print("controlling-terminal capture and cancellation: ok")
