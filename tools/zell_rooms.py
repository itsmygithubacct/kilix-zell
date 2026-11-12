"""Real terminal sessions and read-only mounted-disk browsing for Zell's rooms."""
from __future__ import annotations

import json
from pathlib import Path
import re
import shutil
import stat
import subprocess
import uuid

STATE = 'rooms.json'
MODEL = 'gpt-6-astra'
PROMPT_LIMIT = 1024  # kilix agent-control: 1-1024 UTF-8 bytes, one line


def _command(args, timeout=45):
    result = subprocess.run(['kilix', *args], capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError((result.stderr or result.stdout or 'Kilix request failed').strip()[:4000])
    return result.stdout


def _control(*args):
    value = json.loads(_command(['agent-control', *map(str, args)]))
    if not isinstance(value, dict):
        raise ValueError('Kilix returned an invalid receipt.')
    return value


def _snapshot():
    return _control('list')


def previews(ui):
    """Bounded, read-only previews of receipt-bound coding sessions.

    Never persist screen text or infer that a quiet agent has completed work.
    Revalidate identity after capture before returning anything to the room.
    """
    state = _state(ui)
    rows = [dict(status='No assigned session', text='Click to start a session') for _ in range(5)]
    if not state['screens']:
        return rows
    try:
        snapshot = json.loads(_command(['agent-control', 'list'], timeout=2))
        if not isinstance(snapshot, dict):
            raise ValueError('Invalid session snapshot')
        captured = []
        for i in range(5):
            pane = _live(state['screens'].get(str(i)), snapshot)
            if not pane:
                rows[i] = dict(status='Session closed', text='Click to start a new session')
                continue
            if pane.get('agent') != 'codex':
                rows[i] = dict(status='Client not verified', text='Focus the tab to inspect it')
                continue
            try:
                output = _command(['agent-control', 'dump', str(pane['pane_id']), '--lines', '8'], timeout=2)
                text = '\n'.join(''.join(c for c in line if c.isprintable())[:100]
                                 for line in output.splitlines()[-6:])[:1100]
                captured.append((i, pane, text))
            except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired):
                rows[i] = dict(status='Preview unavailable', text='Click to inspect the session')
        fresh = json.loads(_command(['agent-control', 'list'], timeout=2))
        if not isinstance(fresh, dict):
            raise ValueError('Invalid session snapshot')
        for i, pane, text in captured:
            current = _live(pane, fresh)
            if current and current.get('agent') == 'codex':
                rows[i] = dict(status=f"Tab {pane['tab_id']} / live screen", text=text)
            else:
                rows[i] = dict(status='Session changed', text='Refresh before opening this session')
    except (OSError, RuntimeError, ValueError, TypeError, subprocess.TimeoutExpired):
        rows = [dict(status='Preview unavailable', text='Click to inspect the session') for _ in range(5)]
    return rows


def _identity(pane):
    return (isinstance(pane, dict) and type(pane.get('pane_id')) is int
            and pane['pane_id'] > 0 and type(pane.get('tab_id')) is int
            and isinstance(pane.get('broker'), str) and bool(pane['broker']))


def _live(saved, snapshot):
    if not _identity(saved):
        return None
    matches = [p for p in snapshot.get('panes', []) if _identity(p)
               and all(p[k] == saved[k] for k in ('pane_id', 'tab_id', 'broker'))]
    return matches[0] if len(matches) == 1 else None


def _state(ui):
    state = ui.state_read(STATE, {})
    if not isinstance(state, dict) or not isinstance(state.get('screens', {}), dict):
        state = {}
    state.setdefault('screens', {})
    state['schema'] = 'kilix.zell.rooms/v1'
    return state


def _project_path(path):
    cwd = Path(path).expanduser().resolve()
    if cwd == Path('/mnt/22tb') or Path('/mnt/22tb') in cwd.parents:
        raise ValueError('/mnt/22tb is archive storage; choose another disk for coding.')
    if not cwd.is_dir():
        raise ValueError('Choose an existing project directory.')
    return cwd


def _anchor(snapshot):
    anchors = [p for p in snapshot.get('panes', [])
               if p.get('pane_id') == snapshot.get('caller_pane') and _identity(p)]
    if len(anchors) != 1:
        raise ValueError('No verified caller pane. Open Zell from a Kilix terminal; caller identity is never guessed.')
    return anchors[0]


def _create(ui, state, snapshot, cwd, index=None, role='Coding session', prompt=None):
    anchor = _anchor(snapshot)
    args = ['new-tab', anchor['pane_id'], '--expect-broker', anchor['broker'],
            '--agent', 'codex', '--model', MODEL, '--own-tab', '--cwd', cwd,
            '--title', f'Zell / {role} / {cwd.name}']
    if prompt:
        _check_prompt(prompt)
        args += ['--prompt', prompt]
    receipt = _control(*args)
    pane = receipt.get('pane')
    if receipt.get('status') != 'created' or not _identity(pane):
        raise ValueError('No valid creation receipt. Inspect Kilix tabs before retrying; nothing was assigned.')
    # Journal every successful receipt, including sessions displaced from a screen.
    if not isinstance(state.get('sessions'), list):
        state['sessions'] = []
    state['sessions'].append(dict(receipt=receipt, project=str(cwd), role=role))
    if index is not None:
        previous = state['screens'].get(str(index))
        if _identity(previous):
            if not isinstance(state.get('retired_screens'), list):
                state['retired_screens'] = []
            state['retired_screens'].append(dict(screen=index, pane=previous))
        state['screens'][str(index)] = dict(pane)
    ui.state_write(STATE, state)
    return pane


def _check_prompt(prompt):
    if len(prompt.encode()) > PROMPT_LIMIT:
        raise ValueError(f'Launch prompt is {len(prompt.encode())} bytes; Kilix accepts {PROMPT_LIMIT}. '
                         'Use a shorter project path.')
    return prompt


def _role_prompt(role, cwd, workers=None):
    """Launch prompt for a role. workers: created worker panes, or None before creation."""
    prompt = (f'You are the {role} for project {cwd}. Await the user task before making changes. '
              'Other sessions share this checkout; coordinate changes and do not overwrite their work.')
    if role != 'Coordinator':
        return prompt
    rules = (' Inspect fresh Kilix agent-control identities before any explicit task dispatch. '
             'Use supported verified delivery only; never guess a pane identity or type unverified commands.')
    listed = (f' Your workers are the tabs titled "Zell / Worker N / {cwd.name}" '
              'in `kilix agent-control list`.' + rules)
    if workers:
        # Identity only: a full creation receipt per worker overruns the limit.
        exact = (' Your workers: ' + '; '.join(
            f"pane {p['pane_id']} tab {p['tab_id']} broker {p['broker']}" for p in workers) + '.' + rules)
        if len((prompt + exact).encode()) <= PROMPT_LIMIT:
            return prompt + exact
    return prompt + listed


def _created_text(panes):
    return ('Created ' + ', '.join(f"tab {p['tab_id']} / pane {p['pane_id']}" for p in panes)
            + f'.\ncodex --no-daemon --model {MODEL}\n'
            'Client startup is not yet verified. Focus the tabs to finish any startup prompts.')


def _replace(ui, state, indexes, snapshot):
    occupied = [(i, p) for i in indexes
                if (p := _live(state['screens'].get(str(i)), snapshot))]
    if not occupied:
        return True
    detail = ', '.join(f"screen {i + 1} (tab {p['tab_id']})" for i, p in occupied)
    return ui.choose('Reassign screens?\n' + detail + '\nExisting tabs stay open and their receipts are retained.',
                     [('Reassign screens', True), ('Cancel', False)]) is True


def _start(ui, index, state, snapshot):
    _anchor(snapshot)
    cwd = _project_path(input('Project directory (Enter = home): ').strip() or Path.home())
    if not _replace(ui, state, [index], snapshot):
        return
    pane = _create(ui, state, snapshot, cwd, index, 'Controller' if index == 0 else f'Worker {index}')
    ui.page_text(_created_text([pane]))


def project_session(ui, projectpath, screen=None):
    """Launch from an explicit project action; optional screen binding is 0..4."""
    try:
        cwd = _project_path(projectpath)
        if screen == 'command-center':
            screen = ui.choose('Assign project session to a screen',
                               [('Controller', 0)] + [(f'Worker {i}', i) for i in range(1, 5)])
            if screen is None:
                return
        if screen is not None and (type(screen) is not int or screen not in range(5)):
            raise ValueError('Choose a screen from 0 through 4.')
        state, snapshot = _state(ui), _snapshot()
        if screen is not None and not _replace(ui, state, [screen], snapshot):
            return
        pane = _create(ui, state, snapshot, cwd, screen,
                       'Coding session' if screen is None else 'Controller' if screen == 0 else f'Worker {screen}')
        ui.page_text(_created_text([pane]))
        return pane
    except (OSError, RuntimeError, ValueError, TypeError, subprocess.TimeoutExpired) as error:
        ui.page_text(f'Project session: {error}\nNo automatic retry was attempted. Inspect existing tabs before another launch.')


def pilot(ui):
    """Launch an explicitly previewed project arrangement, persisting each receipt."""
    created = []
    try:
        from zell_projects import projects
        project = ui.choose('Pilot / registered project',
                            [(p['name'] + ' — ' + str(p['path']), p) for p in projects(ui)])
        if project is None:
            return
        cwd = _project_path(project['path'])
        plan = ui.choose('Pilot / session arrangement', [
            ('Coordinator + two workers (3 tabs)', [(1, 'Worker 1'), (2, 'Worker 2'), (0, 'Coordinator')]),
            ('Implementation and review pair (2 tabs)', [(0, 'Implementer'), (1, 'Reviewer')]),
            ('Single coding session (1 tab)', [(0, 'Coding session')])])
        if not plan:
            return
        # Every prompt is checked before the first tab exists: a late refusal
        # would leave workers running without their coordinator.
        for _, role in plan:
            _check_prompt(_role_prompt(role, cwd))
        preview = (f"Launch this arrangement?\nProject: {cwd}\nModel: {MODEL} (--no-daemon)\n"
                   f"New tabs: {len(plan)}\n" + '\n'.join(f'Screen {i + 1}: {role}' for i, role in plan)
                   + '\nSessions receive their role and await your task. No project edits are requested at launch.')
        if ui.choose(preview, [('Launch arrangement', True), ('Cancel', False)]) is not True:
            return
        state, snapshot = _state(ui), _snapshot()
        _anchor(snapshot)
        if not _replace(ui, state, [i for i, _ in plan], snapshot):
            return
        for index, role in plan:
            created.append(_create(ui, state, _snapshot(), cwd, index, role, _role_prompt(role, cwd, created)))
        ui.page_text(_created_text(created) + '\nUse the controller screen task menu to dispatch work.')
    except (OSError, RuntimeError, ValueError, TypeError, KeyError, ImportError, subprocess.TimeoutExpired) as error:
        ui.page_text(f'Pilot stopped: {error}\n{len(created)} successful creation receipts saved. '
                     'Existing tabs are retained. Inspect tabs before retrying; an unconfirmed creation may still exist.')


def _dispatch(ui, state):
    snapshot = _snapshot()
    workers = [(f"Worker {i} — tab {p['tab_id']}", p)
               for i in range(1, 5)
               if (p := _live(state['screens'].get(str(i)), snapshot)) and p.get('agent') == 'codex']
    target = ui.choose('Dispatch a task to a verified Codex worker', workers)
    if target is None:
        return
    text = input('Task to send (one line): ').strip()
    if not text:
        return
    if text.startswith(('/', '!', '#')) or any(ord(c) < 32 for c in text) or len(text.encode()) > 700:
        raise ValueError('Use a plain one-line task of at most 700 UTF-8 bytes, without a client command prefix.')
    if ui.choose(f"Send to pane {target['pane_id']}?\n{text}", [('Send task', True), ('Cancel', False)]) is not True:
        return
    current = _live(target, _snapshot())
    if not current or current.get('agent') != 'codex':
        raise ValueError('Worker identity changed; no task sent.')
    # deliver re-verifies broker, caller, client and composer state, and reports
    # application delivery. Never fall back to unverified send-text or keys.
    receipt = _control('deliver', target['pane_id'], '--expect-broker', target['broker'],
                       '--text', text, '--message-id', 'zell-' + uuid.uuid4().hex,
                       '--timeout', '30', '--mode', 'steer')
    ui.page_text(json.dumps(receipt, indent=2))


def screen(ui, index):
    """Screen zero controls four workers; each screen owns a receipt-bound tab."""
    if index not in range(5):
        return ui.page_text('Unknown command screen.')
    try:
        state = _state(ui)
        snapshot = _snapshot()
        pane = _live(state['screens'].get(str(index)), snapshot)
        title = 'Controller' if index == 0 else f'Worker {index}'
        status = (f"tab {pane['tab_id']}, pane {pane['pane_id']}; client: {pane.get('agent') or 'not verified'}"
                  if pane else 'No live assigned session')
        options = [('Focus session', 'focus')] if pane else [('Start Codex session', 'start')]
        options += [('Session status', 'status')]
        if index == 0:
            options += [('List four workers', 'workers'), ('Send a task to a worker', 'dispatch'), ('Task board', 'tasks')]
        action = ui.choose(f'{title} — {status}', options)
        if action == 'start':
            _start(ui, index, state, snapshot)
        elif action == 'focus':
            current = _live(pane, _snapshot())
            if not current:
                raise ValueError('Session identity changed; refresh this screen.')
            _command(['pane', 'focus', f"pane:{current['pane_id']}"])
        elif action == 'status':
            ui.page_text(json.dumps(pane, indent=2) if pane else status)
        elif action == 'workers':
            ui.page_text('\n'.join(f"Worker {i}: " +
                (f"tab {p['tab_id']}, pane {p['pane_id']}, client {p.get('agent') or 'not verified'}"
                 if (p := _live(state['screens'].get(str(i)), snapshot)) else 'not running') for i in range(1, 5)))
        elif action == 'dispatch':
            _dispatch(ui, state)
        elif action == 'tasks':
            from zell_journal import tasks
            tasks(ui)
    except (OSError, RuntimeError, ValueError, TypeError, subprocess.TimeoutExpired) as error:
        ui.page_text(f'Command Center: {error}\nNo automatic retry was attempted.')


def _unescape(value):
    return re.sub(r'\\([0-7]{3})', lambda m: chr(int(m[1], 8)), value)


def disks():
    """One bin per mounted device/filesystem, excluding virtual and bind copies."""
    try:
        lines = Path('/proc/self/mountinfo').read_text().splitlines()
    except OSError:
        return []
    mounts = {}
    for line in lines:
        try:
            left, right = line.split(' - ', 1)
            fields, fs = left.split(), right.split()
            device, root, mount = fields[2], _unescape(fields[3]), _unescape(fields[4])
            source = _unescape(fs[1])
            if not source.startswith('/dev/') or re.match(r'/dev/(loop|ram|zram|fd)', source):
                continue
            # Prefer filesystem roots to bind mounts and shorter canonical mounts.
            rank = (root != '/', len(mount), mount)
            if device not in mounts or rank < mounts[device][0]:
                mounts[device] = (rank, mount, source, fs[0])
        except (ValueError, IndexError):
            continue
    result = []
    for _, mount, source, fstype in mounts.values():
        try:
            total, used, free = shutil.disk_usage(mount)
        except OSError:
            total = used = free = 0
        result.append(dict(path=mount, label=f'{Path(source).name} · {mount}',
                           source=source, filesystem=fstype, total=total, free=free,
                           used_percent=round(100 * used / total, 1) if total else None))
    return sorted(result, key=lambda d: (d['path'] != '/', d['path']))


def laundry(ui, index):
    rows = disks()
    # Eight visible bins; the eighth offers overflow when necessary.
    if index == 7 and len(rows) > 8:
        disk = ui.choose('More mounted disks', [(d['label'], d) for d in rows[7:]])
    else:
        disk = rows[index] if 0 <= index < len(rows) else None
    if not disk:
        return ui.page_text('No mounted disk assigned to this bin.')
    capacity = (f"{disk['used_percent']}% full · {disk['free']/2**30:.1f} GiB free / {disk['total']/2**30:.1f} GiB"
                if disk['used_percent'] is not None else 'Capacity unavailable (permission or device error)')
    if ui.choose(f"{disk['label']}\n{capacity}", [('Browse files (read only)', 'browse')]) != 'browse':
        return
    try:
        picked = ui.file_pick(Path(disk['path']))
        if picked is None:
            return
        picked = Path(picked)
        if not stat.S_ISREG(picked.stat().st_mode):
            return ui.page_text('Choose a regular file.')
        # Do not launch an editor or arbitrary file handler from a read-only bin.
        if picked.suffix.lower() in {'.txt', '.md', '.json', '.jsonl', '.log', '.csv', '.py', '.c', '.h'}:
            ui.open_file(picked)
        else:
            ui.page_text(f'{picked}\n{picked.stat().st_size:,} bytes\nRead-only browsing: this file type has no built-in preview.')
    except OSError as error:
        ui.page_text(f'Cannot browse this disk: {error}')


def room_help(ui, room):
    ui.page_text('Command Center: five screens open real Codex tabs. Screen one is the controller; '
                 'use its task menu to send a deliberate task to a verified worker. Walk back to return upstairs.'
                 if room in {'command', 'command_center', 1} else
                 'Laundry: each labeled bin represents a mounted filesystem. Fill level shows disk usage. '
                 'Select a bin to inspect capacity and browse files without deleting anything.')
