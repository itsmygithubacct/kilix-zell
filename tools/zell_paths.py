"""Folders this machine keeps for archives only.

Nothing is reserved by default. A machine lists its archive-only folders, one
absolute path per line, in `archive-only-paths` in Zell's state directory, or
in KILIX_ZELL_ARCHIVE_ONLY separated by colons. Zell then refuses to start
coding sessions or active file work there.
"""
from __future__ import annotations
import os
from pathlib import Path


def state_root():
    root = Path(os.environ.get('GPU_TERMINAL_HOME', Path.home()/'.local/gpu_terminal'))
    return Path(os.environ.get('KILIX_ZELL_STATE', root/'kilix-zell/state'))


def archive_only_roots():
    names = [p for p in os.environ.get('KILIX_ZELL_ARCHIVE_ONLY', '').split(':') if p]
    try:
        listed = state_root()/'archive-only-paths'
        if not listed.is_symlink() and listed.stat().st_size <= 65536:
            names += [line.strip() for line in listed.read_text().splitlines()
                      if line.strip() and not line.lstrip().startswith('#')]
    except (OSError, ValueError):
        pass
    return [Path(name) for name in names if name.startswith('/') and name != '/']


def archive_only_root(path):
    """Return the reserved folder that contains path, or None."""
    path = Path(path)
    for root in archive_only_roots():
        if path == root or root in path.parents:
            return root
    return None
