"""Command Center controls backed by Kilix's shared settings and updater."""
import json
import os
import stat
import subprocess

from zell_weather import settings_path

STATE = 'agents.json'
KEY = 'KILIX_CODING_YOLO'


def yolo_state():
    """Read the same file/default as Kilix; -1 means the read failed."""
    try:
        fd = os.open(settings_path(), os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
        with os.fdopen(fd, 'rb') as stream:
            if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
                return -1
            data = stream.read(1024 * 1024 + 1)
        if len(data) > 1024 * 1024:
            return -1
        text = data.decode('utf-8', errors='replace')
        value = 'off'
        for line in text.splitlines():
            name, separator, setting = line.strip().partition('=')
            if separator and name.strip() == KEY:
                value = setting.strip()
    except FileNotFoundError:
        value = 'on' if os.environ.get(KEY, 'off').strip().lower() == 'on' else 'off'
    except (OSError, UnicodeError):
        return -1
    return int(value.lower() in ('on', '1', 'true', 'yes'))


def toggle_yolo(ui):
    current = yolo_state()
    if current < 0:
        raise ValueError('Cannot read the shared YOLO setting: '+str(settings_path()))
    result = subprocess.run(['kilix', 'settings', '--set', 'coding_yolo='+('off' if current else 'on')],
                            capture_output=True, text=True, timeout=15)
    if result.returncode:
        raise RuntimeError((result.stderr or result.stdout or 'Could not save the shared setting')[:2000])
    actual = yolo_state()
    if actual != 1-current:
        raise RuntimeError('The shared YOLO setting could not be verified after saving.')
    if ui.DIALOGUE:
        ui.DIALOGUE.send('Y', str(actual))


def catalog():
    result = subprocess.run(['kilix', 'install', '--json'], capture_output=True, text=True, timeout=20)
    if result.returncode:
        raise RuntimeError((result.stderr or 'Cannot read the coding-agent catalog')[:2000])
    rows = json.loads(result.stdout)
    if not isinstance(rows, list):
        raise ValueError('Invalid coding-agent catalog')
    return [row for row in rows if isinstance(row, dict) and row.get('kind') == 'agent'
            and isinstance(row.get('id'), str) and isinstance(row.get('label'), str)]


def updates(ui):
    """Remember a selection, then run it in the interactive PTY for sudo/input."""
    from zell_terminal import terminal_loop
    try:
        rows = catalog()
        available = {row['id'] for row in rows if row.get('installed')}
        state = ui.state_read(STATE, {})
        saved = state.get('update_agents') if isinstance(state, dict) else None
        selected = available.intersection(saved) if isinstance(saved, list) and all(isinstance(x, str) for x in saved) else set(available)
        while True:
            labels = ', '.join(row['label'] for row in rows if row['id'] in selected) or 'None selected'
            choices = [('Update selected: '+labels, 'run'), ('Select all installed agents', 'all'),
                       ('Clear selection', 'none')]
            choices += [(('[x] ' if row['id'] in selected else '[ ] ')+row['label']+
                         ('' if row['id'] in available else ' (not installed)'), 'agent:'+row['id']) for row in rows]
            choice = ui.choose('Coding-agent updates / '+labels, choices)
            if choice is None:
                return
            if choice == 'run':
                if not selected:
                    ui.page_text('Select at least one installed coding agent.')
                    continue
                ui.state_write(STATE, {'update_agents': sorted(selected)})
                argv = ['kilix', 'install', '--update', *sorted(selected)]
                if ui.DIALOGUE:
                    return terminal_loop(ui, argv, title='Coding-agent updates / Ctrl-C interrupts', sensitive=True)
                return ui.run(argv)
            if choice == 'all':
                selected = set(available)
            elif choice == 'none':
                selected.clear()
            elif choice.startswith('agent:'):
                key = choice.removeprefix('agent:')
                if key not in available:
                    ui.page_text('This client is not installed. Install it before selecting it for updates.')
                    continue
                selected.symmetric_difference_update({key})
            ui.state_write(STATE, {'update_agents': sorted(selected)})
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        ui.page_text('Agent updates: '+str(error))
