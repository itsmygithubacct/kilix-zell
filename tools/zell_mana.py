"""The Command Center's Mana console.

It shows what each coding provider has left, opens the providers' own usage
pages in a browser drawn inside a Kilix tab, and keeps the readings current
without being asked. Readings come from Claudex's usage reader: Codex and
Claude from their standing tmux sessions, Grok from its CLI login. Reading a
balance sends no prompt to a model and spends no tokens.
"""
from __future__ import annotations

import fcntl
import subprocess
import sys
import threading
import time
from pathlib import Path

import zell_mana_manual
import zell_providers
import zell_vitals

PROVIDERS = zell_vitals.PROVIDERS
PAGES = ('dashboard', 'codex', 'claude', 'grok')
# A reading older than this is read again by the automatic refresh.
MAX_AGE = 1800
# After any automatic attempt, wait this long before the next one.
RETRY = 600
ATTENTION = ('attention', 'login_required')
SETTING = 'mana-auto.json'
_refresh = threading.Lock()


def cache():
    entries = zell_vitals.read_json(zell_vitals.config_path().parent / '.loop/usage-pages.json').get('providers', {})
    return entries if isinstance(entries, dict) else {}


def reading(entry, now):
    """Percent left, -1 when unknown, -2 when only the person can unblock it."""
    if isinstance(entry, dict) and entry.get('status') in ATTENTION:
        return -2
    value = zell_vitals.provider_value(entry, now)
    return -1 if value is None else max(0, min(100, round(value)))


def by_hand(entries, now, directory):
    """The values set by hand that stand now: {provider: percent left}."""
    return {provider: percent for provider, (percent, _) in
            zell_mana_manual.standing(directory, entries, now).items()}


def readings(now=None, directory=None, rows=None):
    """One number per provider. directory is Zell's state folder, where the
    values set by hand at the Command computer are kept. rows are the
    percentages a provider snapshot publishes, {provider: percent}.

    What the person said comes first, then a published percentage, then
    Claudex's own reading."""
    now = time.time() if now is None else now
    entries = cache()
    standing = by_hand(entries, now, directory)
    rows = rows or {}
    return [standing[provider] if provider in standing
            else rows[provider] if type(rows.get(provider)) is int and 0 <= rows[provider] <= 100
            else reading(entries.get(provider), now)
            for provider in PROVIDERS]


def published(frames):
    """The percentages in provider snapshot frames: {provider: percent}."""
    rows = {}
    for kind, payload in frames:
        if kind != 'G':
            continue
        for line in str(payload).splitlines()[1:]:
            words = line.split()
            try:
                rows[words[0]] = int(words[1])
            except (IndexError, ValueError):
                continue
    return rows


def console_frames(directory, now=None):
    """What the room's monitor sends for the Mana console, in order.

    The provider snapshot goes first. The interface sets the console's three
    numbers from a snapshot's rows alone, so a snapshot with no row for a
    provider blanks that number; the mana line goes last and has the final
    word. It carries the word "mana": a snapshot with no rows is three bare
    integers too.
    """
    import zell_providers
    frames = list(zell_providers.monitor_frames(directory, now))
    numbers = readings(now, directory, published(frames))
    return frames + [('G', 'mana ' + ' '.join(str(value) for value in numbers))]


def send_console(dialogue, directory, now=None):
    """Send the console's frames in one write. Between the snapshot and the
    mana line the console's numbers are the snapshot's alone, and a read
    that fell between two writes could draw them."""
    dialogue.send_together(console_frames(directory, now))


def needs(now=None, directory=None):
    """What the person must do, one line per blocked provider."""
    now = time.time() if now is None else now
    entries, lines = cache(), []
    standing = by_hand(entries, now, directory)
    for provider in PROVIDERS:
        if provider in standing:
            continue
        entry = entries.get(provider)
        entry = entry if isinstance(entry, dict) else {}
        status = entry.get('status')
        if status in ATTENTION:
            fix = entry.get('fix') or ('grok login' if provider == 'grok' else f'tmux attach -t {provider}_usage')
            lines.append(f'{provider}: {entry.get("reason") or "needs you"}\n    Do this: {fix}')
        elif reading(entry, now) == -1:
            reason = str(entry.get('reason') or 'no reading yet').splitlines()[0][:160]
            lines.append(f'{provider}: no current reading ({reason}).\n    Try: Read mana now'
                         f'\n    Or set it by hand at the Command computer: set-mana {provider} 50')
    return lines


def claudex(*words, timeout=150, install=True):
    """Run Claudex. When no copy is found, Kilix is asked to install it
    first: Claudex is installed on first use. Only a task the person asked
    for installs anything; the automatic reading passes install=False."""
    script, reason = zell_vitals.claudex_ready(install=install)
    if script is None:
        raise FileNotFoundError(f'Claudex is not installed and Kilix could not install it ({reason})' if install
                                else f'Claudex was not run: {reason}')
    # The home folder: a project folder makes a client stop at its trust question.
    return subprocess.run([sys.executable, str(script), '--config', str(zell_vitals.config_path()), *words],
                          capture_output=True, text=True, timeout=timeout, cwd=Path.home())


def read(max_age, install=True):
    """Read every provider whose last good reading is older than max_age seconds."""
    runtime = zell_vitals.config_path().parent / '.loop'
    runtime.mkdir(parents=True, exist_ok=True)
    with (runtime / 'zell-mana.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        return claudex('usage', 'read', *PROVIDERS, '--max-age', str(int(max_age)), '--json', install=install)


def automatic(ui):
    return bool(ui.state_read(SETTING, {'enabled': True}).get('enabled', True))


def due(ui, now=None):
    """Whether the automatic refresh should run now. Never without Claudex:
    looking for it installs nothing, so the automatic reading cannot be what
    installs it."""
    now = time.time() if now is None else now
    if not automatic(ui) or zell_vitals.claudex_script() is None:
        return False
    last = ui.state_read('mana-auto-last.json', {}).get('at')
    if isinstance(last, (int, float)) and 0 <= now - last < RETRY:
        return False
    entries = cache()
    for provider in PROVIDERS:
        entry = entries.get(provider)
        checked = entry.get('checked_at') if isinstance(entry, dict) else None
        if reading(entry, now) < 0 or not isinstance(checked, (int, float)) or now - checked > MAX_AGE:
            return True
    return False


def refresh_in_background(ui, now=None):
    """Start one automatic reading when one is due. The reading itself never
    blocks the caller. Looking for Claudex can: at most one short question to
    Kilix every few minutes while no copy exists (zell_vitals.LOOK_TIMEOUT)."""
    if not due(ui, now) or not _refresh.acquire(blocking=False):
        return False

    def work():
        try:
            # Recorded first, so a failing reader is retried at RETRY, not every tick.
            ui.state_write('mana-auto-last.json', {'at': time.time()})
            # The copy seen a moment ago may be gone by the time the lock is
            # had. The automatic reading still installs nothing.
            read(MAX_AGE, install=False)
        except (OSError, subprocess.SubprocessError):
            pass
        finally:
            _refresh.release()
    threading.Thread(target=work, daemon=True).start()
    return True


def summary(ui):
    directory = getattr(ui, "BASE", None)
    lines = [zell_providers.mode_line(directory), zell_vitals.snapshot(ui, directory)['details'], '',
             'Automatic readings: ' + ('on' if automatic(ui) else 'off') +
             f' (every {MAX_AGE // 60} minutes while Zell runs; no tokens are spent).']
    blocked = needs(directory=directory)
    lines += ['', 'Needs you:' if blocked else 'Nothing needs you.', *blocked]
    return '\n'.join(lines)


def open_page(ui, which):
    words = ['dashboard', 'open', '--pane'] if which == 'dashboard' else ['dashboard', 'provider', which, '--pane']
    try:
        done = claudex(*words, timeout=60)
    except (OSError, subprocess.SubprocessError) as error:
        return ui.page_text(f'The usage browser could not be opened: {error}')
    if done.returncode:
        detail = (done.stdout or done.stderr or '').strip()[:600]
        return ui.page_text('The usage browser could not be opened.\n\n' + detail)
    target = 'The usage dashboard' if which == 'dashboard' else f'The {which} usage page'
    ui.page_text(f'{target} is open in a new browser tab.\n\n'
                 'Sign in there once if the page asks; the browser keeps its own profile.\n'
                 'Return to this tab when you are done.')


def hub(ui, action=0):
    if 0 <= action < len(PAGES):
        return open_page(ui, PAGES[action])
    if action == 4:
        try:
            read(0)
        except (OSError, subprocess.SubprocessError) as error:
            return ui.page_text(f'Mana could not be read: {error}\n\n' + summary(ui))
        return ui.page_text(summary(ui))
    if action == 5:
        blocked = needs(directory=getattr(ui, 'BASE', None))
        return ui.page_text('\n'.join(blocked) if blocked else 'Nothing needs you. Every provider has a current reading.')
    if action == 6:
        enabled = not automatic(ui)
        ui.state_write(SETTING, {'enabled': enabled})
        return ui.page_text('Automatic mana readings are now ' + ('on.' if enabled else 'off.') + '\n\n' + summary(ui))
    raise ValueError('Unknown mana console action')
