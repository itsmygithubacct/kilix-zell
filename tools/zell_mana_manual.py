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
# A stored value that does not say which reading it was set over.
UNKNOWN = 'unknown'


def state_dir():
    """Zell's state folder, by the very rule the room interface uses
    (desktop.BASE), so the scripts write where Zell reads."""
    return Path(os.environ.get('KILIX_ZELL_STATE', Path(os.environ.get(
        'GPU_TERMINAL_HOME', Path.home() / '.local/gpu_terminal')) / 'kilix-zell/state'))


def _time(value):
    """A time a clock could have given, as a float; None for anything else."""
    # The range is checked first: it is exact for an integer of any size,
    # and math.isfinite raises on one too large for a float.
    if type(value) not in (int, float) or not 0 <= value <= LATEST or not math.isfinite(value):
        return None
    return float(value)


def _entry(value):
    """One stored entry as (percent, time set, over), or None when it is not valid.

    over names the automatic reading that was there when the value was set:
    its time, None when there was none, UNKNOWN for a file that does not say.
    """
    if not isinstance(value, dict):
        return None
    percent, at = value.get('percent'), _time(value.get('at'))
    if type(percent) is not int or not 0 <= percent <= 100 or at is None:
        return None
    over = UNKNOWN
    if 'over' in value:
        over = None if value['over'] is None else _time(value['over'])
        if value['over'] is not None and over is None:
            over = UNKNOWN
    return percent, at, over


def _load(directory):
    if directory is None:
        return {}
    try:
        stored = json.loads((Path(directory) / NAME).read_text(encoding='utf-8'))
    except (OSError, ValueError, RecursionError):
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


def _plain(values):
    return {provider: entry[:2] for provider, entry in values.items()}


def load(directory):
    """Every valid value set by hand: {provider: (percent left, time set)}."""
    return _plain(_load(directory))


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
    stored = {}
    for provider, (percent, at, over) in sorted(values.items()):
        stored[provider] = {'percent': percent, 'at': at}
        if over != UNKNOWN:
            stored[provider]['over'] = over
    text = json.dumps({'providers': stored}, indent=1) + '\n'
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


def set_values(directory, wanted, now=None, entries=None):
    """Record percentages left. Nothing is written unless every one is valid.

    entries are the automatic readings there now (Claudex's own when not
    given). Each value remembers the reading it was set over, so that
    reading can never retire it, whatever the clocks said.
    """
    now = time.time() if now is None else float(now)
    for provider, value in wanted.items():
        if provider not in PROVIDERS:
            raise ValueError(f'{provider!r} is not a provider')
        if type(value) is not int or not 0 <= value <= 100:
            raise ValueError(f'{value!r} is not a percentage from 0 to 100')
    if not wanted:
        raise ValueError('nothing to set')
    entries = automatic_entries() if entries is None else entries
    checked = {}
    for provider, value in wanted.items():
        entry = entries.get(provider) if isinstance(entries, dict) else None
        checked[provider] = (value, now, _time(entry.get('checked_at')) if isinstance(entry, dict) else None)
    with _locked(directory):
        values = _load(directory)
        values.update(checked)
        _save(directory, values)
    return _plain(values)


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
        values = _load(directory)
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
    checked = _time(entry.get('checked_at'))
    if checked is None:
        return None
    return checked if zell_vitals.provider_value(entry, checked) is not None else None


def _settle(values, entries, now):
    """Apply the rules to loaded values. True when something changed."""
    changed = False
    for provider in list(values):
        percent, at, over = values[provider]
        if over != UNKNOWN and at > now:
            # Set while the clock ran ahead. It was set no later than now, and
            # the reading it was set over is known, so nothing older can
            # retire it for this.
            at = float(now)
            values[provider] = (percent, at, over)
            changed = True
        reading = taken(entries.get(provider)) if isinstance(entries, dict) else None
        if reading is None or (over != UNKNOWN and reading == over):
            # No good reading, or the very one the value was set over.
            continue
        if reading > at:
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
    values = _load(directory)
    if not values or not _settle(dict(values), entries, now):
        return _plain(values)
    try:
        with _locked(directory):
            # Read again inside the lock: a value may just have been set.
            values = _load(directory)
            if _settle(values, entries, now):
                _save(directory, values)
    except OSError:
        _settle(values, entries, now)
    return _plain(values)
