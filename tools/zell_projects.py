"""Explicit project registration and opt-in, local, read-only Git health watch."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import threading
import time

_cache = {}
_cache_lock = threading.Lock()


def projects(ui):
    """Return validated metadata without scanning repositories or dropping offline paths."""
    saved = ui.state_read('projects.json', [])
    if not isinstance(saved, list):
        return []
    result, seen = [], set()
    for item in saved:
        if not isinstance(item, dict) or not isinstance(item.get('path'), str):
            continue
        path = item['path']
        if not path or '\x00' in path or not Path(path).is_absolute():
            continue
        path = str(Path(path).resolve())
        if path in seen:
            continue
        seen.add(path)
        name = item.get('name')
        name = name.strip() if isinstance(name, str) else ''
        result.append({'path': path, 'name': name or Path(path).name or path,
                       'watch': item.get('watch') is True})
    return result


def git(path, *args):
    # Ambient Git variables must not redirect the worktree, index or object store.
    env = {key: value for key, value in os.environ.items() if not key.startswith('GIT_')}
    env['GIT_OPTIONAL_LOCKS'] = '0'
    return subprocess.run(['git', '-c', 'core.fsmonitor=false', '-c', 'core.untrackedCache=false',
                           '-C', str(path), *args], capture_output=True, timeout=15, env=env)


def repo_root(path):
    result = git(path, 'rev-parse', '--show-toplevel')
    if result.returncode:
        return None
    root = os.fsdecode(result.stdout).rstrip('\n')
    return str(Path(root).resolve()) if root else None


def register(ui, path, name=''):
    folder = Path(path).expanduser().resolve()
    if not folder.is_dir():
        raise ValueError('Choose an existing project folder.')
    root = repo_root(folder)
    canonical = root or str(folder)
    entries = projects(ui)
    existing = next((entry for entry in entries if entry['path'] == canonical), None)
    if existing:
        return existing
    entry = {'path': canonical, 'name': name.strip() or Path(canonical).name or canonical, 'watch': False}
    entries.append(entry)
    ui.state_write('projects.json', entries)
    return entry


def set_watch(ui, path, enabled):
    entries = projects(ui)
    entry = next((item for item in entries if item['path'] == path), None)
    if entry is None:
        raise ValueError('Project is not registered.')
    if enabled and repo_root(path) != path:
        raise ValueError('Health watch requires a registered Git repository root.')
    entry['watch'] = bool(enabled)
    ui.state_write('projects.json', entries)


def dirty_files(raw):
    """Porcelain v1 -z: XY path NUL; rename/copy adds an old-path NUL."""
    tokens, index, paths = raw.split(b'\x00'), 0, set()
    while index < len(tokens):
        token = tokens[index]
        index += 1
        if not token:
            continue
        if len(token) < 4 or token[2:3] != b' ':
            raise ValueError('Invalid Git status response.')
        status, path = token[:2], token[3:]
        paths.add(path)
        if b'R' in status or b'C' in status:
            if index >= len(tokens) or not tokens[index]:
                raise ValueError('Incomplete Git rename status.')
            index += 1
    return sorted(paths)


def status(path):
    if repo_root(path) != path:
        raise ValueError('Repository is unavailable or no longer at its registered root.')
    result = git(path, 'status', '--porcelain=v1', '-z', '--untracked-files=all')
    if result.returncode:
        raise ValueError(os.fsdecode(result.stderr).strip() or 'Git status failed.')
    return dirty_files(result.stdout)


def dirty_summary(ui, refresh=False):
    """Count dirty files in opted-in roots, cached 60s. Errors never imply clean."""
    entries = [entry for entry in projects(ui) if entry['watch']]
    key = (str(ui.BASE), tuple((entry['path'], entry['name']) for entry in entries))
    with _cache_lock:
        now = time.monotonic()
        cached = _cache.get(key)
        if not refresh and cached and 0 <= now - cached[0] < 60:
            return {**cached[1], 'errors': list(cached[1]['errors'])}
        count, details, errors = 0, [], []
        for entry in entries:
            try:
                files = status(entry['path'])
                count += len(files)
                details.append(f"{entry['name']}: {len(files)} dirty files ({entry['path']})")
                # repr keeps filenames containing newlines/control characters unambiguous.
                details.extend('  ' + repr(os.fsdecode(path)) for path in files)
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                message = f"{entry['name']}: {error}"
                errors.append(message)
                details.append(message)
        if not entries:
            details.append('No projects opted into Git health watch.')
        result = {'count': count, 'details': '\n'.join(details), 'errors': errors, 'known': not errors}
        _cache.clear()
        _cache[key] = (now, result)
        return {**result, 'errors': list(errors)}


def hub(ui):
    while True:
        entries = projects(ui)
        action = ui.choose('Git Wizard — projects', [('Register project folder', 'register'),
            ('Health watch summary', 'summary')] +
            [(f"{entry['name']} | {'watching' if entry['watch'] else 'not watched'} | {entry['path']}", entry['path']) for entry in entries])
        if action is None:
            return
        try:
            if action == 'register':
                path = input('Project folder: ').strip()
                if path:
                    entry = register(ui, path)
                    ui.page_text(f"Registered {entry['name']}: {entry['path']}\nHealth watch is opt-in. Open this project to enable it.")
            elif action == 'summary':
                ui.page_text(dirty_summary(ui, refresh=True)['details'])
            else:
                entry = next((item for item in entries if item['path'] == action), None)
                if not entry:
                    continue
                operation = ui.choose(entry['name'], [('Show Git status', 'status'),
                    ('Disable health watch' if entry['watch'] else 'Enable health watch (-1 health per dirty file)', 'watch'),
                    ('Launch coding in a Kilix tab', 'tab'), ('Launch coding on a command center screen', 'screen'),
                    ('Remove project registration (keep files)', 'remove')])
                if operation == 'remove':
                    ui.state_write('projects.json', [item for item in entries if item['path'] != action])
                elif operation == 'watch':
                    set_watch(ui, action, not entry['watch'])
                elif operation == 'status':
                    files = status(action)
                    ui.page_text(f'{len(files)} dirty files\n' + '\n'.join(repr(os.fsdecode(path)) for path in files))
                elif operation in {'tab', 'screen'}:
                    import zell_rooms
                    zell_rooms.project_session(ui, action, screen='command-center' if operation == 'screen' else None)
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            ui.page_text(str(error))
