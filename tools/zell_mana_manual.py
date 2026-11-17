"""Mana set by hand.

A person can tell Zell what a coding provider has left when no automatic
reading can. The numbers live in Zell's own state folder. A value set by hand
stands until it is cleared, set again, or a good automatic reading is taken
after it.
"""
from __future__ import annotations

import json
import math
import os
import tempfile
import time
from pathlib import Path

import zell_vitals

# The providers the Mana console shows, in its order.
PROVIDERS = zell_vitals.PROVIDERS
NAME = 'mana-manual.json'


def state_dir():
    """Zell's state folder: the same rule the room interface uses."""
    home = os.environ.get('GPU_TERMINAL_HOME') or Path.home() / '.local/gpu_terminal'
    return Path(os.environ.get('KILIX_ZELL_STATE') or Path(home) / 'kilix-zell/state')


def _entry(value):
    """One stored entry as (percent, time set), or None when it is not valid."""
    if not isinstance(value, dict):
        return None
    percent, at = value.get('percent'), value.get('at')
    if type(percent) is not int or not 0 <= percent <= 100:
        return None
    if type(at) not in (int, float) or not math.isfinite(at) or at < 0:
        return None
    return percent, float(at)


def load(directory):
    """Every valid value set by hand: {provider: (percent left, time set)}."""
    if directory is None:
        return {}
    try:
        stored = json.loads((Path(directory) / NAME).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}
    providers = stored.get('providers') if isinstance(stored, dict) else None
    if not isinstance(providers, dict):
        return {}
    found = {}
    for provider in PROVIDERS:
        entry = _entry(providers.get(provider))
        if entry is not None:
            found[provider] = entry
    return found


def _save(directory, values):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    text = json.dumps({'providers': {provider: {'percent': percent, 'at': at}
                                     for provider, (percent, at) in sorted(values.items())}}, indent=1) + '\n'
    fd, temporary = tempfile.mkstemp(prefix='.' + NAME, dir=directory)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as handle:
            handle.write(text)
        os.replace(temporary, directory / NAME)
    except BaseException:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def percent(text):
    """A percentage left, 0 to 100, from what a person typed. ValueError otherwise."""
    word = str(text).strip()
    if word.endswith('%'):
        word = word[:-1].strip()
    try:
        value = float(word)
    except ValueError:
        raise ValueError(f'{text!r} is not a percentage from 0 to 100') from None
    if not math.isfinite(value) or not 0 <= value <= 100:
        raise ValueError(f'{text!r} is not a percentage from 0 to 100')
    return int(round(value))


def pairs(words):
    """Turn 'codex 62 grok 40' or 'all 50' into {provider: percent}."""
    if not words or len(words) % 2:
        raise ValueError('give a provider and a percentage left, for example: codex 62')
    wanted = {}
    for name, value in zip(words[::2], words[1::2]):
        name = name.strip().lower()
        if name != 'all' and name not in PROVIDERS:
            raise ValueError(f'{name!r} is not a provider; use ' + ', '.join(PROVIDERS) + ' or all')
        for provider in (PROVIDERS if name == 'all' else (name,)):
            wanted[provider] = percent(value)
    return wanted


def set_values(directory, wanted, now=None):
    """Record percentages left. Nothing is written unless every one is valid."""
    now = time.time() if now is None else float(now)
    checked = {}
    for provider, value in wanted.items():
        if provider not in PROVIDERS:
            raise ValueError(f'{provider!r} is not a provider')
        if type(value) is not int or not 0 <= value <= 100:
            raise ValueError(f'{value!r} is not a percentage from 0 to 100')
        checked[provider] = (value, now)
    if not checked:
        raise ValueError('nothing to set')
    values = load(directory)
    values.update(checked)
    _save(directory, values)
    return values


def clear(directory, providers=None):
    """Forget values set by hand: the named providers, or all of them."""
    names = PROVIDERS if not providers else tuple(providers)
    for provider in names:
        if provider not in PROVIDERS:
            raise ValueError(f'{provider!r} is not a provider; use ' + ', '.join(PROVIDERS))
    values = load(directory)
    removed = [provider for provider in names if values.pop(provider, None) is not None]
    if removed or (Path(directory) / NAME).exists():
        _save(directory, values)
    return removed


def stands(manual, automatic, checked):
    """Whether a value set by hand is the one to show.

    automatic is the automatic reading (below zero when there is none) and
    checked is when it was taken. A good automatic reading taken after the
    value was set replaces it.
    """
    if manual is None:
        return False
    newer = type(checked) in (int, float) and checked > manual[1]
    return not (automatic is not None and automatic >= 0 and newer)
