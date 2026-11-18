"""Zell's log: one JSON line per event, on by default.

It is `zell-log.jsonl` in Zell's state directory; KILIX_ZELL_DEBUG_LOG names
another file, or is `off` to keep none. At LOG_BYTES it is moved to the same
name with `.1` added, the one before that dropped, so it never grows past
twice that. Events are what the helpers did and how they ended. Keyboard
input and terminal contents are never passed here: the floating terminal
carries a sudo password.
"""
import json
import os
import sys
import time
import traceback

from zell_paths import state_root

LOG_BYTES = 4 << 20
OFF = ('off', '0', 'no', 'false')


def path():
    """The file logged to, or None when the log is off."""
    named = os.environ.get('KILIX_ZELL_DEBUG_LOG')
    if named is not None and named.strip().lower() in OFF:
        return None
    return named or str(state_root()/'zell-log.jsonl')


def enabled():
    return path() is not None


def log(event, **fields):
    where = path()
    if not where:
        return
    record = dict(t=round(time.time(), 3), pid=os.getpid(),
                  src=os.path.basename(sys.argv[0] or '?'), event=event, **fields)
    try:
        os.makedirs(os.path.dirname(where) or '.', mode=0o700, exist_ok=True)
        try:
            if os.stat(where).st_size >= LOG_BYTES:
                os.replace(where, where + '.1')
        except FileNotFoundError:
            pass
        # One O_APPEND write per line keeps concurrent helpers from interleaving.
        fd = os.open(where, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o600)
        try:
            os.write(fd, (json.dumps(record, default=str) + '\n').encode())
        finally:
            os.close(fd)
    except OSError:
        pass  # A log must never break the action it describes.


def error(event, **fields):
    """Log the exception being handled, with its traceback."""
    log(event, error=repr(sys.exc_info()[1]), traceback=traceback.format_exc(), **fields)
