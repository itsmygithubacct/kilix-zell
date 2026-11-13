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


def readings(now=None):
    now = time.time() if now is None else now
    entries = cache()
    return [reading(entries.get(provider), now) for provider in PROVIDERS]


def needs(now=None):
    """What the person must do, one line per blocked provider."""
    now = time.time() if now is None else now
    entries, lines = cache(), []
    for provider in PROVIDERS:
        entry = entries.get(provider)
        entry = entry if isinstance(entry, dict) else {}
        status = entry.get('status')
        if status in ATTENTION:
            fix = entry.get('fix') or ('grok login' if provider == 'grok' else f'tmux attach -t {provider}_usage')
            lines.append(f'{provider}: {entry.get("reason") or "needs you"}\n    Do this: {fix}')
        elif reading(entry, now) == -1:
            reason = str(entry.get('reason') or 'no reading yet').splitlines()[0][:160]
            lines.append(f'{provider}: no current reading ({reason}).\n    Try: Read mana now')
    return lines


def claudex(*words, timeout=150):
    script = zell_vitals.claudex_script()
    if script is None:
        raise FileNotFoundError('Claudex is not installed')
    # The home folder: a project folder makes a client stop at its trust question.
    return subprocess.run([sys.executable, str(script), '--config', str(zell_vitals.config_path()), *words],
                          capture_output=True, text=True, timeout=timeout, cwd=Path.home())


def read(max_age):
    """Read every provider whose last good reading is older than max_age seconds."""
    runtime = zell_vitals.config_path().parent / '.loop'
    runtime.mkdir(parents=True, exist_ok=True)
    with (runtime / 'zell-mana.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        return claudex('usage', 'read', *PROVIDERS, '--max-age', str(int(max_age)), '--json')


def automatic(ui):
    return bool(ui.state_read(SETTING, {'enabled': True}).get('enabled', True))


def due(ui, now=None):
    """Whether the automatic refresh should run now."""
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
    """Start one automatic reading when one is due. Never blocks the caller."""
    if not due(ui, now) or not _refresh.acquire(blocking=False):
        return False

    def work():
        try:
            # Recorded first, so a failing reader is retried at RETRY, not every tick.
            ui.state_write('mana-auto-last.json', {'at': time.time()})
            read(MAX_AGE)
        except (OSError, subprocess.SubprocessError):
            pass
        finally:
            _refresh.release()
    threading.Thread(target=work, daemon=True).start()
    return True


def summary(ui):
    lines = [zell_vitals.snapshot(ui)['details'], '',
             'Automatic readings: ' + ('on' if automatic(ui) else 'off') +
             f' (every {MAX_AGE // 60} minutes while Zell runs; no tokens are spent).']
    blocked = needs()
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
        blocked = needs()
        return ui.page_text('\n'.join(blocked) if blocked else 'Nothing needs you. Every provider has a current reading.')
    if action == 6:
        enabled = not automatic(ui)
        ui.state_write(SETTING, {'enabled': enabled})
        return ui.page_text('Automatic mana readings are now ' + ('on.' if enabled else 'off.') + '\n\n' + summary(ui))
    raise ValueError('Unknown mana console action')
