"""Local vitals. Only request_mana may invoke a provider.

Fatigue is remaining headroom: min(CPU idle%, MemAvailable/MemTotal%).
CPU uses successive /proc/stat deltas (including iowait as idle, excluding
already-counted guest ticks). A 0.25 EMA smooths samples; the first CPU sample
is unknown. Mana is the mean of known providers' most restrictive windows.
"""
from __future__ import annotations

from datetime import datetime
import fcntl
import gzip
import json
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import threading
import time

PROVIDERS = ('codex', 'claude', 'grok')
APT_HISTORY = Path('/var/log/apt')
UPGRADE_STAMPS = (Path('/var/lib/apt/periodic/upgrade-stamp'),)
_sample_lock = threading.Lock()
_previous_cpu = None
_smoothed = None


def number(value):
    return float(value) if isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value) else None


def read_json(path):
    try:
        value = json.loads(path.read_text())
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def config_path():
    if os.environ.get('CLAUDEX_CONFIG'):
        return Path(os.environ['CLAUDEX_CONFIG']).expanduser()
    return Path(os.environ.get('GPU_TERMINAL_HOME') or Path.home() / '.local/gpu_terminal').expanduser() / 'kilix-apps/kilix-claudex/loop.json'


UPGRADE_OPERATIONS = ('upgrade', 'full-upgrade', 'dist-upgrade', 'safe-upgrade')
APT_VALUE_OPTIONS = ('-o', '--option', '-c', '--config-file', '-t', '--target-release', '--default-release')


def is_upgrade(commandline):
    """True when an apt history Commandline ran an upgrade operation.

    The operation is the first argument that is not an option, so a package
    whose name merely contains "upgrade" does not count.
    """
    try:
        words = shlex.split(commandline)
    except ValueError:
        words = commandline.split()
    if not words:
        return False
    tool = os.path.basename(words[0])
    if tool in ('unattended-upgrade', 'unattended-upgrades'):
        return True
    if tool not in ('apt', 'apt-get', 'aptitude'):
        return False
    skip = False
    for word in words[1:]:
        if skip:
            skip = False
        elif word in APT_VALUE_OPTIONS:
            skip = True
        elif not word.startswith('-'):
            return word in UPGRADE_OPERATIONS
    return False


def last_upgrade(ui):
    state = ui.state_read('os-update.json', {})
    dates = [number(state.get('completed_at'))] if isinstance(state, dict) else []
    for path in UPGRADE_STAMPS:
        try:
            dates.append(path.stat().st_mtime)
        except OSError:
            pass
    for path in APT_HISTORY.glob('history.log*'):
        try:
            opener = gzip.open if path.suffix == '.gz' else open
            with opener(path, 'rt', errors='replace') as stream:
                content = stream.read()
            for block in re.split(r'\n\s*\n', content):
                command = re.search(r'^Commandline:\s*(.*)$', block, re.M)
                if re.search(r'^Error:', block, re.M) or not command or not is_upgrade(command[1]):
                    continue
                end = re.search(r'^End-Date:\s*(\d{4}-\d\d-\d\d\s+\d\d:\d\d:\d\d)', block, re.M)
                if end:
                    dates.append(datetime.strptime(' '.join(end[1].split()), '%Y-%m-%d %H:%M:%S').timestamp())
        except (OSError, ValueError, EOFError):
            pass
    return max((v for v in dates if v is not None and v > 0), default=None)


def cpu_memory():
    fields = Path('/proc/stat').read_text().splitlines()[0].split()
    ticks = list(map(int, fields[1:9]))
    if fields[0] != 'cpu' or len(ticks) < 5:
        raise ValueError('missing CPU statistics')
    memory = {}
    for line in Path('/proc/meminfo').read_text().splitlines():
        key, value = line.split(':', 1)
        memory[key] = int(value.split()[0])
    return (sum(ticks), ticks[3] + ticks[4]), 100 * memory['MemAvailable'] / memory['MemTotal']


def fatigue():
    global _previous_cpu, _smoothed
    with _sample_lock:
        try:
            cpu, memory = cpu_memory()
        except (OSError, ValueError, KeyError, ZeroDivisionError, IndexError):
            return -1
        previous, _previous_cpu = _previous_cpu, cpu
        if previous is None or cpu[0] <= previous[0] or cpu[1] < previous[1]:
            return -1 if _smoothed is None else round(_smoothed)
        headroom = max(0, min(100, memory, 100 * (cpu[1] - previous[1]) / (cpu[0] - previous[0])))
        _smoothed = headroom if _smoothed is None else .25 * headroom + .75 * _smoothed
        return round(_smoothed)


def provider_value(entry, now):
    if not isinstance(entry, dict) or entry.get('status') != 'ok':
        return None
    windows = entry.get('windows')
    if not isinstance(windows, list) or not windows:
        return None
    values = []
    for window in windows:
        if not isinstance(window, dict):
            return None
        reset = number(window.get('resets_at'))
        remaining = number(window.get('remaining_percent'))
        if remaining is None or not 0 <= remaining <= 100 or (reset is not None and reset <= now):
            return None
        values.append(remaining)
    return min(values)


def mana(ui, now):
    cache = read_json(config_path().parent / '.loop/usage-pages.json')
    entries = cache.get('providers', {})
    if not isinstance(entries, dict):
        entries = {}
    attempts = read_json(config_path().parent / '.loop/zell-mana-attempts.json')
    values, ages, lines, missing = [], [], [], []
    for provider in PROVIDERS:
        entry = entries.get(provider, {})
        entry = entry if isinstance(entry, dict) else {}
        checked = number(entry.get('checked_at'))
        attempt = attempts.get(provider, {})
        attempt = attempt if isinstance(attempt, dict) else {}
        failed = attempt.get('failed') and (number(attempt.get('at')) or 0) > (checked or 0)
        value = None if failed or not checked else provider_value(entry, now)
        stamp = max(checked or 0, number(attempt.get('at')) or 0)
        if value is None:
            missing.append(provider)
        else:
            values.append(value)
            if checked:
                ages.append(max(0, int(now - checked)))
        lines.append(f'{provider}: ' + ('unknown' if value is None else f'{value:g}% remaining') +
                     (f'; checked {datetime.fromtimestamp(stamp).isoformat(timespec="seconds")}' if stamp else '; never checked'))
    lines.append('Mana: mean of known provider minima; unknown providers excluded.')
    lines.append('Missing/unknown providers: ' + (', '.join(missing) or 'none'))
    return round(sum(values) / len(values)) if values else -1, max(ages) if ages else -1, lines


def snapshot(ui):
    now = time.time()
    upgraded = last_upgrade(ui)
    age = max(0, int((now - upgraded) // 86400)) if upgraded else -1
    health = max(10, 100 - 10 * age) if age >= 0 else -1
    remaining = fatigue()
    magic, magic_age, lines = mana(ui, now)
    details = '\n'.join([f'Health: {health}; days since successful upgrade: {age}',
        'Health stays 100 for 24 hours, then loses 10 per elapsed day (floor 10). Unknown = -1.',
        f'Fatigue headroom: {remaining}%; min(CPU idle, available memory), smoothed with 25% new sample.',
        f'Mana: {magic}%; oldest known reading age: {magic_age} seconds', *lines])
    return dict(health=health, fatigue=remaining, mana=magic, age_days=age, mana_age=magic_age, details=details)


def monitor(ui):
    while True:
        value = snapshot(ui)
        ui.DIALOGUE.send('V', ' '.join(str(value[k]) for k in ('health', 'fatigue', 'mana', 'age_days', 'mana_age')))
        time.sleep(5)


def claudex_script():
    """Claudex's usage reader: beside a source checkout, or behind the command."""
    candidates = []
    if os.environ.get('KILIX_ZELL_CLAUDEX'):
        candidates.append(Path(os.environ['KILIX_ZELL_CLAUDEX']).expanduser())
    candidates.append(Path(__file__).resolve().parents[2] / 'kilix-claudex/pane_loop.py')
    launcher = shutil.which('claudex')
    if launcher:
        candidates.append(Path(launcher).resolve().with_name('pane_loop.py'))
    return next((path for path in candidates if path.is_file()), None)


def check_provider(provider):
    script = claudex_script()
    if script is None:
        raise FileNotFoundError('Claudex is not installed')
    # The reader starts the provider's client in the directory it is given.
    # From a project folder the client stops at its folder-trust question and
    # no usage is read, so always read from the home folder.
    subprocess.run([sys.executable, str(script), '--config', str(config_path()), 'usage', 'read', provider, '--full'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True, timeout=120,
                   cwd=Path.home())


def write_json(path, value):
    temporary = path.with_name(path.name + f'.{os.getpid()}.tmp')
    try:
        temporary.write_text(json.dumps(value))
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def request_mana(ui):
    runtime = config_path().parent / '.loop'
    if claudex_script() is None:
        # Nothing was asked of any provider, so nothing is recorded or throttled.
        ui.page_text('Mana refresh unavailable: the Claudex usage reader was not found. Install Claudex '
                     '(the `claudex` command) or set KILIX_ZELL_CLAUDEX to its pane_loop.py.\n\n'
                     + snapshot(ui)['details'])
        return
    try:
        runtime.mkdir(parents=True, exist_ok=True)
        with (runtime / 'zell-mana.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            for provider in PROVIDERS:
                # Re-read inside the lock, including reads performed by Claudex.
                cached = read_json(runtime / 'usage-pages.json').get('providers', {})
                entry = cached.get(provider, {}) if isinstance(cached, dict) else {}
                entry = entry if isinstance(entry, dict) else {}
                attempts = read_json(runtime / 'zell-mana-attempts.json')
                attempt = attempts.get(provider, {})
                attempt = attempt if isinstance(attempt, dict) else {}
                last = max(number(entry.get('checked_at')) or 0, number(attempt.get('at')) or 0)
                now = time.time()
                if last and now - last < 3600:
                    continue
                # Persist before invocation so crashes and failed checks are throttled too.
                attempts[provider] = {'at': now, 'failed': True}
                write_json(runtime / 'zell-mana-attempts.json', attempts)
                try:
                    check_provider(provider)
                except (OSError, subprocess.SubprocessError):
                    continue
                attempts[provider]['failed'] = False
                write_json(runtime / 'zell-mana-attempts.json', attempts)
    except OSError as error:
        ui.page_text(f'Mana refresh unavailable: {error}\n\n' + snapshot(ui)['details'])
        return
    report(ui)


def report(ui):
    ui.page_text(snapshot(ui)['details'])
