"""Private local tasks and opt-in station conversations; never vault content."""
import datetime
import json
import uuid
from zell_paths import state_lock

TASKS = 'tasks.json'
HISTORY = 'conversations.json'
SETTINGS = 'conversation-settings.json'


def _enabled(ui):
    settings = ui.state_read(SETTINGS, {})
    return isinstance(settings, dict) and settings.get('enabled') is True


def _rows(ui, name):
    rows = ui.state_read(name, [])
    return [r for r in rows if isinstance(r, dict)] if isinstance(rows, list) else []


def tasks(ui):
    while True:
        rows = _rows(ui, TASKS)
        choices = [('Add a task', 'add')]
        choices += [(('[done] ' if r.get('done') else '[open] ') + str(r.get('title', '')),
                     r.get('id')) for r in rows]
        picked = ui.choose('Task board / saved on this machine', choices)
        if picked is None:
            return
        if picked == 'add':
            title = input('Task: ').strip()[:500]
            if not title:
                continue
            with state_lock(ui.BASE):
                rows = _rows(ui, TASKS)
                if len(rows) < 200:
                    rows.append(dict(id=uuid.uuid4().hex, title=title, done=False))
                    ui.state_write(TASKS, rows)
                    continue
            ui.page_text('The task board holds 200 tasks. Remove a finished task first.')
            continue
        row = next((r for r in rows if r.get('id') == picked), None)
        if row is None:
            continue
        done = not row.get('done')
        action = ui.choose(str(row.get('title', '')), [('Mark done' if done else 'Reopen', 'toggle'),
                                                       ('Remove task', 'remove')])
        if action not in ('toggle', 'remove'):
            continue
        with state_lock(ui.BASE):
            rows = _rows(ui, TASKS)
            row = next((r for r in rows if r.get('id') == picked), None)
            if row is None:
                continue
            if action == 'remove':
                rows.remove(row)
            else:
                row['done'] = done
            ui.state_write(TASKS, rows)


def recorder(ui, station):
    # Key Master is excluded regardless of the history setting. Monitor and
    # notebook views are excluded to avoid recording vault metadata or history.
    if station in {-1, 9, 31} or station>=32:
        return None
    def record(role, text):
        if not _enabled(ui):
            return
        with state_lock(ui.BASE):
            # The setting may have changed while another tab held the lock.
            if _enabled(ui):
                rows = _rows(ui, HISTORY)
                rows.append(dict(station=station, role=role, text=str(text)[:5500],
                                 time=datetime.datetime.now().isoformat(timespec='seconds')))
                ui.state_write(HISTORY, rows[-100:])
    return record


def conversations(ui):
    while True:
        enabled = _enabled(ui)
        action = ui.choose('Station conversations', [('Read saved conversations', 'read'),
            ('Stop saving conversations' if enabled else 'Enable local conversation history', 'toggle'),
            ('Clear saved conversations', 'clear')])
        if action is None:
            return
        if action == 'read':
            rows = _rows(ui, HISTORY)
            try:
                for line in (ui.BASE/'wizard-conversations.jsonl').read_text().splitlines():
                    row = json.loads(line)
                    if isinstance(row, dict):
                        row['time'] = datetime.datetime.fromtimestamp(row['time']).isoformat(timespec='seconds')
                        row['text'] = str(row.get('question', '')) + '\n\n' + str(row.get('text', ''))
                        rows.append(row)
            except (OSError, ValueError, KeyError, TypeError):
                pass
            rows.sort(key=lambda row: str(row.get('time', '')))
            selected = ui.choose('Saved station conversations', [(str(r.get('time', '')) + ' / ' + str(r.get('text', ''))[:70], r) for r in reversed(rows)])
            if selected:
                ui.page_text(selected.get('text', ''))
        elif action == 'toggle':
            if not enabled and ui.choose('Save future station reports and replies on this machine? '
                'Key Master and terminal contents are always excluded. Reports may contain personal data.',
                [('Enable history', True), ('Cancel', False)]) is not True:
                continue
            with state_lock(ui.BASE):
                ui.state_write(SETTINGS, {'enabled': not enabled})
        elif action == 'clear':
            if ui.choose('Clear saved conversations?', [('Cancel', False), ('Clear', True)]) is True:
                with state_lock(ui.BASE):
                    ui.state_write(HISTORY, [])
                    (ui.BASE/'wizard-conversations.jsonl').unlink(missing_ok=True)
