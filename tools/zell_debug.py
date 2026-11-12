"""Optional debug log: one JSON line per event when KILIX_ZELL_DEBUG_LOG names a file.

Off unless the variable is set. Events are what the helpers did and how they
ended. Keyboard input and terminal contents are never passed here: the
floating terminal carries a sudo password.
"""
import json
import os
import sys
import time
import traceback


def enabled():
    return bool(os.environ.get('KILIX_ZELL_DEBUG_LOG'))


def log(event, **fields):
    path = os.environ.get('KILIX_ZELL_DEBUG_LOG')
    if not path:
        return
    record = dict(t=round(time.time(), 3), pid=os.getpid(),
                  src=os.path.basename(sys.argv[0] or '?'), event=event, **fields)
    try:
        # One O_APPEND write per line keeps concurrent helpers from interleaving.
        fd = os.open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o600)
        try:
            os.write(fd, (json.dumps(record, default=str) + '\n').encode())
        finally:
            os.close(fd)
    except OSError:
        pass  # A debug log must never break the action it describes.


def error(event, **fields):
    """Log the exception being handled, with its traceback."""
    log(event, error=repr(sys.exc_info()[1]), traceback=traceback.format_exc(), **fields)
