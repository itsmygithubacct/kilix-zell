"""Mana set by hand.

A person can tell Zell what a coding provider has left when no automatic
reading can. The numbers live in Zell's own state folder. A value set by hand
stands until it is cleared, set again, or a good automatic reading is taken
after it. Such a reading retires the value for good: it does not come back
when that reading later goes stale or a later reading fails.
"""
from __future__ import annotations

import contextlib
import fcntl
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
LOCK = 'mana-manual.lock'
# No time a value was set lies beyond this (the year 3000). A damaged file
# with a larger number is read as nothing set, not as an error.
LATEST = 32503680000


def state_dir():
    """Zell's state folder: the rule the room interface uses. An empty
    KILIX_ZELL_STATE counts as not set."""
    home = os.environ.get('GPU_TERMINAL_HOME') or Path.home() / '.local/gpu_terminal'
    return Path(os.environ.get('KILIX_ZELL_STATE') or Path(home) / 'kilix-zell/state')


def _entry(value):
    """One stored entry as (percent, time set), or None when it is not valid."""
    if not isinstance(value, dict):
        return None
    percent, at = value.get('percent'), value.get('at')
    if type(percent) is not int or not 0 <= percent <= 100:
        return None
    # The range is checked first: it is exact for an integer of any size,
    # and math.isfinite raises on one too large for a float.
    if type(at) not in (int, float) or not 0 <= at <= LATEST or not math.isfinite(at):
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


@contextlib.contextmanager
def _locked(directory):
    """One writer at a time: a change reads the file, alters it and saves it."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd = os.open(directory / LOCK, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        yield
    finally:
        os.close(fd)


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
    with _locked(directory):
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
    if not (Path(directory) / NAME).exists():
        # Nothing was ever set here, and forgetting nothing makes no folder.
        return []
    with _locked(directory):
        values = load(directory)
        removed = [provider for provider in names if values.pop(provider, None) is not None]
        _save(directory, values)
    return removed


def automatic_entries():
    """The automatic readings Claudex keeps, one entry per provider."""
    cache = zell_vitals.read_json(zell_vitals.config_path().parent / '.loop/usage-pages.json')
    entries = cache.get('providers', {}) if isinstance(cache, dict) else {}
    return entries if isinstance(entries, dict) else {}


def taken(entry):
    """When an automatic reading was taken, if it was a good one then; else None.

    Good is judged at the time of the reading. A reading whose window has
    since reset was still a real answer when it was taken.
    """
    if not isinstance(entry, dict):
        return None
    checked = entry.get('checked_at')
    if type(checked) not in (int, float) or not 0 <= checked <= LATEST or not math.isfinite(checked):
        return None
    return float(checked) if zell_vitals.provider_value(entry, checked) is not None else None


def _settle(values, entries, now):
    """Apply the rules to loaded values. True when something changed."""
    changed = False
    for provider in list(values):
        percent, at = values[provider]
        if at > now:
            # Set while the clock ran ahead. It was set no later than now.
            values[provider] = (percent, at) = (percent, float(now))
            changed = True
        reading = taken(entries.get(provider)) if isinstance(entries, dict) else None
        if reading is not None and reading > at:
            del values[provider]
            changed = True
    return changed


def standing(directory, entries=None, now=None):
    """The values set by hand that stand now: {provider: (percent left, time set)}.

    entries are the automatic readings (Claudex's own when not given). A good
    automatic reading taken after a value was set retires that value, and the
    retirement is written down, so the value cannot come back later. Where the
    folder cannot be written the answer is still right for this moment.
    """
    if directory is None:
        return {}
    now = time.time() if now is None else float(now)
    entries = automatic_entries() if entries is None else entries
    values = load(directory)
    if not values or not _settle(dict(values), entries, now):
        return values
    try:
        with _locked(directory):
            # Read again inside the lock: a value may just have been set.
            values = load(directory)
            if _settle(values, entries, now):
                _save(directory, values)
    except OSError:
        _settle(values, entries, now)
    return values
