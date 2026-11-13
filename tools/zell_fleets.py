"""Save and resume fleet launch metadata.

The owner has not chosen how custom startup instructions are kept. Both paths
exist and neither is the stored policy: a vault record reference, or settings
only with the text requested again on resume. Account eligibility is not chosen
here. A fleet file never holds a prompt body, a credential, an environment
snapshot, or a raw argv receipt, and saving one does not change the shared
YOLO switch.
"""
from __future__ import annotations

import datetime
import fcntl
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import uuid

import zell_agents
import zell_rooms
from zell_paths import state_root

FILE_NAME = 'fleets.json'
VERSION = 1
INSTRUCTION_STORAGE = 'unselected'
STORAGE_VAULT = 'vault'
STORAGE_SETTINGS = 'settings'
REVISION_LIMIT = 1000000000
SECRET_LIMIT = 65536
# Kilix agent-control puts --prompt on the client argv. There is no descriptor
# transport for confidential startup text in that boundary.
CONFIDENTIAL_PROMPT_TRANSPORT = None
AGENTS = {
    'codex': {'effort': 'reasoning', 'prompt_argv': True},
    'claude': {'effort': None, 'prompt_argv': True},
    'kimi': {'effort': None, 'prompt_argv': False},
    'grok': {'effort': None, 'prompt_argv': True},
    'qwen-omp': {'effort': None, 'prompt_argv': True},
}
EFFORTS = ('low', 'medium', 'high', 'xhigh', 'max')
ROLES = ('controller', 'worker', 'coordinator', 'implementer', 'reviewer', 'coding')
ROLE_TEMPLATES = {
    'role.controller': 'You are the controller for this saved fleet. Await the user task before making changes.',
    'role.worker': 'You are a worker for this saved fleet. Await the user task before making changes.',
    'role.coordinator': 'You are the coordinator for this saved fleet. Await the user task before making changes.',
    'role.implementer': 'You are the implementer for this saved fleet. Await the user task before making changes.',
    'role.reviewer': 'You are the reviewer for this saved fleet. Await the user task before making changes.',
    'role.coding': 'You are a coding session for this saved fleet. Await the user task before making changes.',
}
REENTER_TEXT = ('Custom instructions for this worker were not stored. '
                'Wait for the user to type them. Do not invent a task.')
WORKER_KEYS = {'slot', 'role', 'agent', 'model', 'effort', 'cwd', 'prompt_ref'}
INPUT_KEYS = WORKER_KEYS | {'instructions'}
BINDING_KEYS = {'slot', 'pane_id', 'tab_id', 'broker', 'agent'}
FLEET_KEYS = {'id', 'name', 'project', 'created', 'updated', 'storage_used', 'workers', 'bindings'}
ROOT_KEYS = {'version', 'instruction_storage', 'revision', 'fleets'}
FORBIDDEN = {
    'prompt', 'prompts', 'credential', 'credentials', 'secret', 'secrets', 'password',
    'token', 'api_key', 'apikey', 'env', 'environment', 'environ', 'argv', 'transcript',
    'authorization', 'cookie', 'yolo', 'coding_yolo', 'account_mode', 'instructions',
}
_VAULT_REF = re.compile(r'vault:([0-9a-f]{32}):([0-9a-f]{16})\Z')
_TEMPLATE_REF = re.compile(r'template:(role\.(?:controller|worker|coordinator|implementer|reviewer|coding)|reenter)\Z')
_FLEET_ID = re.compile(r'[A-Za-z0-9._-]{1,63}\Z')
_FIELD = re.compile(r'[A-Za-z_]{1,32}\Z')


class FleetError(ValueError):
    pass


class InstructionStorageUnselected(FleetError):
    pass


class StoreCorrupt(FleetError):
    pass


class LockBusy(FleetError):
    pass


class VaultLocked(FleetError):
    pass


class VaultMissing(FleetError):
    pass


class ResumeReport:
    def __init__(self, status, message, launched=(), completed=(), unknown=(),
                 stale=(), occupied=(), waiting=()):
        self.status = status
        self.message = message
        self.launched = list(launched)
        self.completed = list(completed)
        self.unknown = list(unknown)
        self.stale = list(stale)
        self.occupied = list(occupied)
        self.waiting = list(waiting)


def _now():
    return datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def _empty():
    return {'version': VERSION, 'instruction_storage': INSTRUCTION_STORAGE, 'revision': 0, 'fleets': []}


def _directory():
    return state_root() / 'fleets'


def _ensure_dir():
    root = _directory()
    if root.is_symlink():
        raise FleetError('The fleet directory is a symlink. Nothing was written.')
    root.mkdir(parents=True, exist_ok=True)
    os.chmod(root, 0o700)
    info = root.lstat()
    if not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode) or info.st_mode & 0o077:
        raise FleetError('The fleet directory must be a private directory. Nothing was written.')
    return root


def _lock(root):
    fd = os.open(root / '.lock', os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        os.fchmod(fd, 0o600)
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(fd)
        raise LockBusy('Another fleet save holds the lock. This save did not change the file.')
    except OSError:
        os.close(fd)
        raise
    return fd


def _unlock(fd):
    try:
        fcntl.flock(fd, fcntl.LOCK_UN)
    finally:
        os.close(fd)


def _read_bytes(root):
    path = root / FILE_NAME
    if not path.exists() and not path.is_symlink():
        return None
    if path.is_symlink():
        raise StoreCorrupt('fleets.json is a symlink. The last file was left unchanged.')
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode):
            raise StoreCorrupt('fleets.json is not a regular file. The last file was left unchanged.')
        chunks, total = [], 0
        while total <= 1024 * 1024:
            block = os.read(fd, 65536)
            if not block:
                break
            chunks.append(block)
            total += len(block)
        if total > 1024 * 1024:
            raise StoreCorrupt('fleets.json is too large. The last file was left unchanged.')
        return b''.join(chunks)
    finally:
        os.close(fd)


def _atomic_write(root, text):
    temporary = root / f'.{FILE_NAME}.{os.getpid()}.tmp'
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        os.fchmod(fd, 0o600)
        payload = text.encode()
        view = memoryview(payload)
        while view:
            view = view[os.write(fd, view):]
        os.fsync(fd)
    except OSError:
        os.close(fd)
        os.unlink(temporary)
        raise
    os.close(fd)
    os.chmod(temporary, 0o600)
    os.replace(temporary, root / FILE_NAME)
    os.chmod(root / FILE_NAME, 0o600)
    dirfd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(dirfd)
    finally:
        os.close(dirfd)


def _bad_field(key):
    if isinstance(key, str) and _FIELD.fullmatch(key):
        return FleetError(f'Unsupported field {key}. It was not stored and no default was substituted.')
    return FleetError('Unsupported field. It was not stored and no default was substituted.')


def _forbid(value):
    if isinstance(value, dict):
        for key, item in value.items():
            if key in FORBIDDEN:
                raise _bad_field(key)
            _forbid(item)
    elif isinstance(value, list):
        for item in value:
            _forbid(item)


def _label(value, what):
    if not isinstance(value, str) or not 1 <= len(value) <= 95 or any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise FleetError(f'{what} must be 1..95 characters without controls.')
    return value


def _token(value, what, limit=200):
    if (not isinstance(value, str) or not 1 <= len(value) <= limit or value.startswith('-')
            or any(ord(c) <= 32 or ord(c) == 127 for c in value)):
        raise FleetError(f'{what} must be one plain token. No default was substituted.')
    return value


def _stamp(value):
    if not isinstance(value, str) or not 1 <= len(value) <= 40 or any(ord(c) < 32 for c in value):
        raise StoreCorrupt('A fleet timestamp is invalid. The last file was left unchanged.')
    return value


def parse_prompt_ref(value):
    if value == 'none':
        return ('none', None)
    match = _TEMPLATE_REF.fullmatch(value) if isinstance(value, str) else None
    if match:
        return ('template', match.group(1))
    match = _VAULT_REF.fullmatch(value) if isinstance(value, str) else None
    if match:
        return ('vault', (match.group(1), match.group(2)))
    raise FleetError('prompt_ref must be none, a fixed template id, or a vault record id.')


def _validate_worker(worker, *, storage):
    if not isinstance(worker, dict):
        raise FleetError('A worker record must be an object.')
    extra = set(worker) - WORKER_KEYS
    if extra:
        raise _bad_field(next(iter(extra)))
    if set(worker) != WORKER_KEYS:
        raise FleetError('A worker record needs slot, role, agent, model, effort, cwd, and prompt_ref.')
    if type(worker['slot']) is not int or worker['slot'] not in range(5):
        raise FleetError('A worker slot must be an integer from 0 through 4.')
    if worker['role'] not in ROLES:
        raise FleetError('Role must be a defined role id. No default role was substituted.')
    if worker['agent'] not in AGENTS:
        raise FleetError(f'{worker["agent"] if isinstance(worker["agent"], str) else "That client"} is not a Kilix '
                         'launch client. No default client was substituted.')
    _token(worker['model'], 'Model')
    if not isinstance(worker['effort'], str):
        raise FleetError('Effort must be explicit. No default effort was substituted.')
    if worker['effort'] != 'none':
        if AGENTS[worker['agent']]['effort'] != 'reasoning' or worker['effort'] not in EFFORTS:
            raise FleetError('Kilix agent-control accepts --reasoning-effort only for codex, and only its known '
                             'levels. That effort was refused and no default was substituted.')
    if not isinstance(worker['cwd'], str) or not worker['cwd']:
        raise FleetError('Each worker needs a project directory.')
    kind, detail = parse_prompt_ref(worker['prompt_ref'])
    if kind == 'vault' and storage != STORAGE_VAULT:
        raise FleetError('A vault instruction reference is only valid on the vault path.')
    if kind == 'template' and detail != 'reenter' and not AGENTS[worker['agent']]['prompt_argv']:
        raise FleetError(f'{worker["agent"]} has no launch-prompt transport in Kilix. '
                         'No prompt was dropped in its place.')
    return kind


def _validate_binding(binding, slots):
    if not isinstance(binding, dict) or set(binding) != BINDING_KEYS:
        raise StoreCorrupt('A fleet binding is invalid. The last file was left unchanged.')
    if type(binding['slot']) is not int or binding['slot'] not in slots:
        raise StoreCorrupt('A fleet binding does not match a worker. The last file was left unchanged.')
    if type(binding['pane_id']) is not int or type(binding['tab_id']) is not int or binding['pane_id'] <= 0 or binding['tab_id'] <= 0:
        raise StoreCorrupt('A fleet binding identity is invalid. The last file was left unchanged.')
    broker = binding['broker']
    if not isinstance(broker, str) or not 1 <= len(broker) <= 200 or any(ord(c) < 32 for c in broker):
        raise StoreCorrupt('A fleet binding identity is invalid. The last file was left unchanged.')
    if binding['agent'] not in AGENTS:
        raise StoreCorrupt('A fleet binding client is invalid. The last file was left unchanged.')


def _validate_document(document):
    if not isinstance(document, dict) or set(document) != ROOT_KEYS:
        raise StoreCorrupt('fleets.json has an unexpected shape. The last file was left unchanged.')
    _forbid(document)
    if document['version'] != VERSION or document['instruction_storage'] != INSTRUCTION_STORAGE:
        raise StoreCorrupt('fleets.json version or instruction storage is not the unselected store. '
                           'The last file was left unchanged.')
    if type(document['revision']) is not int or not 0 <= document['revision'] <= REVISION_LIMIT:
        raise StoreCorrupt('fleets.json revision is invalid. The last file was left unchanged.')
    if not isinstance(document['fleets'], list):
        raise StoreCorrupt('fleets.json has an unexpected shape. The last file was left unchanged.')
    seen = set()
    for fleet in document['fleets']:
        if not isinstance(fleet, dict) or set(fleet) != FLEET_KEYS:
            raise StoreCorrupt('A fleet record is invalid. The last file was left unchanged.')
        if not isinstance(fleet['id'], str) or not _FLEET_ID.fullmatch(fleet['id']) or fleet['id'] in seen:
            raise StoreCorrupt('A fleet id is invalid. The last file was left unchanged.')
        seen.add(fleet['id'])
        _label(fleet['name'], 'Fleet name')
        _label(fleet['project'], 'Project label')
        _stamp(fleet['created'])
        _stamp(fleet['updated'])
        if fleet['storage_used'] not in {STORAGE_VAULT, STORAGE_SETTINGS}:
            raise StoreCorrupt('A fleet instruction path is invalid. The last file was left unchanged.')
        if not isinstance(fleet['workers'], list) or not fleet['workers'] or not isinstance(fleet['bindings'], list):
            raise StoreCorrupt('A fleet record is invalid. The last file was left unchanged.')
        slots = []
        for worker in fleet['workers']:
            _validate_worker(worker, storage=fleet['storage_used'])
            slots.append(worker['slot'])
        if len(slots) != len(set(slots)):
            raise StoreCorrupt('A fleet repeats a worker slot. The last file was left unchanged.')
        bound = []
        for binding in fleet['bindings']:
            _validate_binding(binding, set(slots))
            bound.append(binding['slot'])
        if len(bound) != len(set(bound)):
            raise StoreCorrupt('A fleet repeats a bound slot. The last file was left unchanged.')
    return document


def _parse(raw):
    try:
        document = json.loads(raw.decode())
    except (UnicodeError, json.JSONDecodeError, AttributeError):
        raise StoreCorrupt('fleets.json is malformed. The last file was left unchanged.')
    return _validate_document(document)


def read_store():
    root = _directory()
    if not root.exists():
        return _empty()
    if root.is_symlink() or not stat.S_ISDIR(root.lstat().st_mode):
        raise StoreCorrupt('The fleet directory is invalid. The last file was left unchanged.')
    raw = _read_bytes(root)
    return _empty() if raw is None else _parse(raw)


def commit_document(mutator):
    """Read, modify, and publish fleets.json under an exclusive lock."""
    root = _ensure_dir()
    fd = _lock(root)
    try:
        raw = _read_bytes(root)
        current = _empty() if raw is None else _parse(raw)
        updated = mutator(json.loads(json.dumps(current)))
        if updated is None:
            return current
        updated['version'] = VERSION
        updated['instruction_storage'] = INSTRUCTION_STORAGE
        revision = current['revision'] + 1
        if revision > REVISION_LIMIT:
            raise FleetError('The fleet revision is full. The file was not changed.')
        updated['revision'] = revision
        _validate_document(updated)
        _atomic_write(root, json.dumps(updated, indent=2, sort_keys=True) + '\n')
        return updated
    finally:
        _unlock(fd)


def _find(document, fleet_id):
    matches = [fleet for fleet in document['fleets'] if fleet['id'] == fleet_id]
    if len(matches) != 1:
        raise FleetError('That fleet is not in the saved file.')
    return matches[0]


def _binding(fleet, slot):
    matches = [item for item in fleet['bindings'] if item['slot'] == slot]
    return matches[0] if len(matches) == 1 else None


def _vault_ids(document):
    found = []
    for fleet in document['fleets']:
        for worker in fleet['workers']:
            kind, detail = parse_prompt_ref(worker['prompt_ref'])
            if kind == 'vault':
                found.append(detail[0])
    return found


def _instruction_bytes(value):
    if value is None or value == b'' or value == '':
        return None
    body = bytearray(value.encode() if isinstance(value, str) else value)
    if not body or len(body) > SECRET_LIMIT:
        body[:] = b'\0' * len(body)
        raise FleetError('Instructions are empty or over the vault limit. Nothing was stored.')
    return body


def _seal(body):
    fd = os.memfd_create('zell-fleet-instructions', os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
    try:
        view = memoryview(body)
        while view:
            view = view[os.write(fd, view):]
        os.lseek(fd, 0, os.SEEK_SET)
        fcntl.fcntl(fd, fcntl.F_ADD_SEALS,
                    fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL)
        return fd
    except OSError:
        os.close(fd)
        raise


class DirectoryVault:
    """In-process vault boundary. Records live only as mode-0600 files read from a descriptor."""

    def __init__(self, root):
        self.root = Path(root)
        self.locked = False
        self.added = 0

    def _root(self):
        if self.root.is_symlink():
            raise FleetError('The vault directory is a symlink.')
        self.root.mkdir(parents=True, exist_ok=True)
        os.chmod(self.root, 0o700)
        return self.root

    def add_fd(self, fd):
        os.lseek(fd, 0, os.SEEK_SET)
        body = os.read(fd, SECRET_LIMIT + 1)
        if not body or len(body) > SECRET_LIMIT:
            raise FleetError('Instructions are empty or over the vault limit. Nothing was stored.')
        record_id = uuid.uuid4().hex
        path = self._root() / record_id
        out = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        try:
            os.write(out, body)
            os.fchmod(out, 0o600)
        finally:
            os.close(out)
        self.added += 1
        return record_id

    def read_fd(self, record_id):
        if self.locked:
            raise VaultLocked('The instruction record is locked. That worker is waiting. '
                              'No default instructions were substituted.')
        path = self.root / record_id
        if path.is_symlink() or not path.is_file():
            raise VaultMissing('The instruction record is missing. That worker is waiting. '
                               'No default instructions were substituted.')
        return os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)

    def delete(self, record_id):
        path = self.root / record_id
        if path.is_symlink() or not path.is_file():
            return
        os.unlink(path)


class SecretsVault:
    """kilix-secrets descriptor boundary. The secret is never a command argument."""

    def add_fd(self, fd):
        import zell_secrets
        result = subprocess.run(zell_secrets.command(
            'add', '--type', 'fleet-instructions', '--label', 'Zell fleet instructions',
            '--field', 'body', '--secret-fd', str(fd)), pass_fds=(fd,), capture_output=True, timeout=20)
        if result.returncode:
            raise VaultLocked('The vault did not store the instructions. The fleet was not published.')
        text = result.stdout.decode('ascii', 'replace').strip()
        if not re.fullmatch(r'[0-9a-f]{32}', text):
            raise FleetError('The vault did not return a record id. The fleet was not published.')
        return text

    def read_fd(self, record_id):
        import zell_secrets
        zell_secrets.record_id(record_id)
        reader = [sys.executable, '-c',
                  'import os\nfd=int(os.environ["KILIX_SECRET_FD"])\n'
                  'data=os.read(fd,65537)\nos.write(1,data)\n']
        result = subprocess.run(zell_secrets.command('run', record_id, 'body', '--', *reader),
                                capture_output=True, timeout=20)
        if result.returncode or not result.stdout or len(result.stdout) > SECRET_LIMIT:
            raise VaultLocked('The instruction record is locked or missing. That worker is waiting. '
                              'No default instructions were substituted.')
        fd = os.memfd_create('zell-fleet-read', os.MFD_CLOEXEC)
        try:
            os.write(fd, result.stdout)
            os.lseek(fd, 0, os.SEEK_SET)
        except OSError:
            os.close(fd)
            raise
        result.stdout = b''
        return fd

    def delete(self, record_id):
        import zell_secrets
        zell_secrets.record_id(record_id)
        subprocess.run(zell_secrets.command('delete', record_id, '--confirm', record_id),
                       capture_output=True, timeout=20)


def default_vault():
    return SecretsVault()


def _prepare_worker(spec, storage, vault, new_records):
    if not isinstance(spec, dict):
        raise FleetError('A worker record must be an object.')
    extra = set(spec) - INPUT_KEYS
    if extra:
        raise _bad_field(next(iter(extra)))
    if 'effort' not in spec:
        raise FleetError('Effort must be explicit. No default effort was substituted.')
    body = _instruction_bytes(spec.get('instructions'))
    try:
        cwd = str(zell_rooms._project_path(spec.get('cwd', '')))
        if body is not None and storage == STORAGE_SETTINGS:
            prompt_ref = 'template:reenter'
        elif body is not None and storage == STORAGE_VAULT:
            prompt_ref = 'none'
        elif body is not None:
            raise InstructionStorageUnselected(
                'Instruction storage is unselected. Choose the vault path or the settings-only path for this save.')
        elif 'prompt_ref' not in spec:
            raise FleetError('prompt_ref must be none, a fixed template id, or a vault record id.')
        else:
            prompt_ref = spec['prompt_ref']
        worker = {
            'slot': spec.get('slot'), 'role': spec.get('role'), 'agent': spec.get('agent'),
            'model': spec.get('model'), 'effort': spec['effort'], 'cwd': cwd, 'prompt_ref': prompt_ref,
        }
        _validate_worker(worker, storage=storage)
        if body is not None and storage == STORAGE_VAULT:
            if vault is None:
                raise FleetError('The vault path was requested without a vault boundary.')
            descriptor = _seal(body)
            try:
                record_id = vault.add_fd(descriptor)
            finally:
                os.close(descriptor)
            if not re.fullmatch(r'[0-9a-f]{32}', record_id):
                raise FleetError('The vault did not return a record id. The fleet was not published.')
            new_records.append(record_id)
            worker['prompt_ref'] = f'vault:{record_id}:{uuid.uuid4().hex[:16]}'
            _validate_worker(worker, storage=storage)
        return worker
    finally:
        if body is not None:
            body[:] = b'\0' * len(body)


def _require_storage(storage):
    if storage not in {STORAGE_VAULT, STORAGE_SETTINGS}:
        raise InstructionStorageUnselected(
            'Instruction storage is unselected. Choose the vault path or the settings-only path for this save.')
    return storage


def save_fleet(*, name, project, workers, storage, vault=None, fleet_id=None):
    storage = _require_storage(storage)
    if vault is None and storage == STORAGE_VAULT:
        vault = default_vault()
    name, project = _label(name, 'Fleet name'), _label(project, 'Project label')
    if not isinstance(workers, list) or not workers:
        raise FleetError('A fleet needs at least one worker.')
    new_records = []
    try:
        prepared = [_prepare_worker(spec, storage, vault, new_records) for spec in workers]
    except Exception:
        # A failed validation after a vault write must not delete that record here:
        # the publish step is the only place a reference becomes visible, and a
        # shared record is never removed because this save failed.
        raise
    slots = [worker['slot'] for worker in prepared]
    if len(slots) != len(set(slots)):
        raise FleetError('A fleet repeats a worker slot. Nothing was published.')
    fleet_id = fleet_id or str(uuid.uuid4())
    if not _FLEET_ID.fullmatch(fleet_id):
        raise FleetError('The fleet id is not valid. Nothing was published.')
    previous_ids = []

    def mutate(current):
        nonlocal previous_ids
        now = _now()
        existing = next((fleet for fleet in current['fleets'] if fleet['id'] == fleet_id), None)
        if existing:
            previous_ids = [item for item in _vault_ids({'fleets': [existing]})]
            created = existing['created']
            current['fleets'] = [fleet for fleet in current['fleets'] if fleet['id'] != fleet_id]
        else:
            created = now
        current['fleets'].append({
            'id': fleet_id, 'name': name, 'project': project, 'created': created, 'updated': now,
            'storage_used': storage, 'workers': prepared,
            'bindings': existing['bindings'] if existing else [],
        })
        kept = {worker['slot'] for worker in prepared}
        current['fleets'][-1]['bindings'] = [item for item in current['fleets'][-1]['bindings'] if item['slot'] in kept]
        return current

    try:
        document = commit_document(mutate)
    except Exception:
        # New vault records stay. A failed metadata publish does not delete them,
        # and it does not delete a record another fleet already references.
        raise
    if vault is not None:
        published = set(_vault_ids(document))
        for record_id in previous_ids:
            if record_id not in published:
                try:
                    vault.delete(record_id)
                except OSError:
                    pass
    return _find(document, fleet_id)


def rename_fleet(fleet_id, name):
    renamed = _label(name, 'Fleet name')

    def mutate(current):
        fleet = _find(current, fleet_id)
        fleet['name'] = renamed
        fleet['updated'] = _now()
        return current

    return _find(commit_document(mutate), fleet_id)


def delete_fleet(fleet_id, confirmation, vault=None):
    document = read_store()
    fleet = _find(document, fleet_id)
    if confirmation != fleet['name']:
        raise FleetError('Deletion was not confirmed. The fleet and its instruction records were kept.')
    owned = _vault_ids({'fleets': [fleet]})

    def mutate(current):
        _find(current, fleet_id)
        current['fleets'] = [item for item in current['fleets'] if item['id'] != fleet_id]
        return current

    updated = commit_document(mutate)
    if vault is not None:
        still = set(_vault_ids(updated))
        for record_id in owned:
            if record_id not in still:
                try:
                    vault.delete(record_id)
                except OSError:
                    pass
    return updated


def open_project(fleet_id):
    """Validate the saved directories. This does not launch a client or edit the project."""
    fleet = _find(read_store(), fleet_id)
    return [str(zell_rooms._project_path(worker['cwd'])) for worker in fleet['workers']]


def configuration_matches(fleet_id, snapshot):
    """True only when every saved slot is the verified live pane, not merely the same folder label."""
    fleet = _find(read_store(), fleet_id)
    if not isinstance(snapshot, dict) or not fleet['workers']:
        return False
    for worker in fleet['workers']:
        binding = _binding(fleet, worker['slot'])
        live = zell_rooms._live(binding, snapshot) if binding else None
        if not live or live.get('agent') != worker['agent']:
            return False
        live_cwd = live.get('cwd')
        if isinstance(live_cwd, str) and live_cwd:
            try:
                if Path(live_cwd).expanduser().resolve() != Path(worker['cwd']).resolve():
                    return False
            except OSError:
                return False
    return True


def _readable(vault, record_id):
    fd = vault.read_fd(record_id)
    blob = bytearray()
    try:
        while len(blob) <= SECRET_LIMIT:
            chunk = os.read(fd, 4096)
            if not chunk:
                break
            blob.extend(chunk)
        if not blob or len(blob) > SECRET_LIMIT:
            raise VaultMissing('The instruction record is missing. That worker is waiting. '
                               'No default instructions were substituted.')
    finally:
        blob[:] = b'\0' * len(blob)
        os.close(fd)


def _startup_text(worker):
    kind, detail = parse_prompt_ref(worker['prompt_ref'])
    if kind == 'none':
        return None
    if kind == 'template' and detail == 'reenter':
        return REENTER_TEXT
    if kind == 'template':
        return ROLE_TEMPLATES[detail]
    return None


def _classify(worker, fleet, state, snapshot):
    binding = _binding(fleet, worker['slot'])
    screen = state.get('screens', {}).get(str(worker['slot']))
    live_binding = zell_rooms._live(binding, snapshot) if binding else None
    live_screen = zell_rooms._live(screen, snapshot) if isinstance(screen, dict) else None
    if live_binding and live_binding.get('agent') == worker['agent']:
        return 'live', live_binding
    if binding is None and live_screen and live_screen.get('agent') == worker['agent']:
        return 'adopt', live_screen
    if binding and not live_binding:
        return ('occupied', live_screen) if live_screen else ('stale', None)
    if live_screen:
        return 'occupied', live_screen
    if live_binding:
        return 'unverified', live_binding
    return 'missing', None


def resume_fleet(ui, fleet_id, *, storage, rebind=False, reentry=False, vault=None):
    storage = _require_storage(storage)
    fleet = _find(read_store(), fleet_id)
    if fleet['storage_used'] != storage:
        raise FleetError('This fleet uses a different instruction path. The unselected policy was not changed.')
    for worker in fleet['workers']:
        zell_rooms._project_path(worker['cwd'])
    if vault is None and storage == STORAGE_VAULT:
        vault = default_vault()
    snapshot = zell_rooms._snapshot()
    state = zell_rooms._state(ui)
    plan = []
    stale, occupied, waiting = [], [], []
    for worker in fleet['workers']:
        kind, live = _classify(worker, fleet, state, snapshot)
        if kind in {'stale', 'occupied', 'unverified'} and not rebind:
            (occupied if kind == 'occupied' else stale).append(worker['slot'])
        elif kind in {'stale', 'occupied', 'unverified'} and rebind:
            plan.append((worker, 'missing', None))
        else:
            plan.append((worker, 'missing' if kind == 'missing' else 'live', live))
    if stale or occupied:
        return ResumeReport('stale', 'Saved panes do not match the live session. No worker was launched and no '
                             'unrelated pane was adopted. Pass an explicit rebind to create the missing slots.',
                             stale=stale, occupied=occupied)
    for worker, action, _live in plan:
        kind, detail = parse_prompt_ref(worker['prompt_ref'])
        if kind == 'template' and detail == 'reenter' and not reentry:
            return ResumeReport('needs_reentry', 'Custom instructions were not stored. Re-enter them for this '
                                 'resume. No worker was launched.')
        if kind == 'vault':
            try:
                _readable(vault, detail[0])
            except (VaultLocked, VaultMissing):
                waiting.append(worker['slot'])
        text = _startup_text(worker) if action == 'missing' else None
        if text and not AGENTS[worker['agent']]['prompt_argv']:
            raise FleetError(f'{worker["agent"]} has no launch-prompt transport in Kilix. No worker was launched.')
        if text:
            zell_rooms._check_prompt(text)
    if waiting:
        return ResumeReport('waiting', 'A locked or missing instruction record left that worker waiting. '
                             'No worker was launched and no default text was substituted.', waiting=waiting)
    yolo = zell_agents.yolo_state()
    if yolo < 0:
        raise FleetError('The shared YOLO setting could not be read. No worker was launched and the switch was not changed.')
    missing = [worker for worker, action, _live in plan if action == 'missing']
    preview = [f'Fleet {fleet["name"]} / {fleet["project"]}',
               f'Shared YOLO is {"on" if yolo else "off"}. This resume will not change that switch.',
               'No task will be dispatched.']
    for worker in fleet['workers']:
        kind, _detail = parse_prompt_ref(worker['prompt_ref'])
        delivery = 'manual instructions in the client' if kind == 'vault' else kind
        preview.append(f"slot {worker['slot']}: {worker['agent']} {worker['model']} effort {worker['effort']} "
                       f"cwd {worker['cwd']} instructions {delivery}")
    if ui.choose('\n'.join(preview), [('Resume fleet', True), ('Cancel', False)]) is not True:
        return ResumeReport('cancelled', 'Resume cancelled. No worker was launched.')
    launched, unknown = [], []
    for worker, action, live in plan:
        if unknown:
            break
        if action != 'missing':
            if _binding(fleet, worker['slot']) is None and live:
                _publish_binding(fleet_id, worker, live)
            continue
        launch = {
            'agent': worker['agent'], 'model': worker['model'], 'effort': worker['effort'],
            'yolo': bool(yolo), 'slot': worker['slot'], 'prompt': _startup_text(worker),
        }
        try:
            pane = zell_rooms._create(ui, state, zell_rooms._snapshot(), Path(worker['cwd']),
                                     worker['slot'], worker['role'], launch=launch)
        except (OSError, RuntimeError, ValueError, TypeError, subprocess.TimeoutExpired):
            unknown.append(worker['slot'])
            break
        try:
            _publish_binding(fleet_id, worker, pane)
        except (FleetError, OSError):
            unknown.append(worker['slot'])
            break
        launched.append(worker['slot'])
    completed = [worker['slot'] for worker, action, _live in plan if action != 'missing'] + launched
    if unknown:
        return ResumeReport('partial', f'Stopped after a partial launch. Completed slots: {completed}. '
                             f'Unknown slots: {unknown}. Remaining slots were not started. No task was dispatched.',
                             launched=launched, completed=completed, unknown=unknown)
    status = 'launched' if launched else 'restored'
    return ResumeReport(status, 'Fleet configuration restored. No task was dispatched.',
                        launched=launched, completed=completed)


def _publish_binding(fleet_id, worker, pane):
    binding = {'slot': worker['slot'], 'pane_id': pane['pane_id'], 'tab_id': pane['tab_id'],
               'broker': pane['broker'], 'agent': worker['agent']}

    def mutate(current):
        fleet = _find(current, fleet_id)
        fleet['bindings'] = [item for item in fleet['bindings'] if item['slot'] != worker['slot']]
        fleet['bindings'].append(binding)
        fleet['bindings'].sort(key=lambda item: item['slot'])
        fleet['updated'] = _now()
        return current

    commit_document(mutate)


def _live_count(fleet, snapshot):
    if not isinstance(snapshot, dict):
        return 0
    count = 0
    for worker in fleet['workers']:
        binding = _binding(fleet, worker['slot'])
        live = zell_rooms._live(binding, snapshot) if binding else None
        if live and live.get('agent') == worker['agent']:
            count += 1
    return count


def monitor_frame(snapshot=None, offset=0):
    """One sanitized F page. Paths, prompts, and credentials are not included."""
    document = read_store()
    fleets = document['fleets']
    total = len(fleets)
    if type(offset) is not int or offset < 0 or (offset > 0 and offset >= total) or offset > total:
        return None
    page = fleets[offset:offset + 8]
    if total == 0 and offset != 0:
        return None
    lines = [f'1 {document["revision"]} {total} {offset} {len(page)}']
    for fleet in page:
        name, project = fleet['name'], fleet['project']
        if any(ord(c) < 32 or ord(c) == 127 for c in name + project):
            return None
        lines.append(f"{fleet['id']}\t{name}\t{project}\t{len(fleet['workers'])}\t{_live_count(fleet, snapshot)}")
    text = '\n'.join(lines)
    if len(text.encode()) > 3500:
        return None
    return text


def pilot_menu(ui):
    """Pilot entry. Launch arrangement stays the existing rooms path; fleet actions share this backend."""
    action = ui.choose('Pilot / fleets', [
        ('Launch an arrangement', 'launch'),
        ('Save a fleet', 'save'),
        ('Resume a fleet', 'resume'),
        ('Rename a fleet', 'rename'),
        ('Delete a fleet', 'delete'),
        ('Open a project', 'open')])
    if action is None:
        return None
    try:
        if action == 'launch':
            return zell_rooms.pilot(ui)
        if action == 'save':
            return _interactive_save(ui)
        if action == 'resume':
            return _interactive_resume(ui)
        if action == 'rename':
            return _interactive_rename(ui)
        if action == 'delete':
            return _interactive_delete(ui)
        if action == 'open':
            return _interactive_open(ui)
    except (FleetError, OSError, RuntimeError, ValueError, TypeError, subprocess.TimeoutExpired) as error:
        ui.page_text(f'Fleet: {error}\nNo task was dispatched.')
        return None


def _choose_storage(ui):
    return ui.choose('Instruction storage is not selected. Choose a path for this action only.', [
        ('Encrypted vault reference', STORAGE_VAULT),
        ('Settings only; re-enter instructions on resume', STORAGE_SETTINGS)])


def _interactive_save(ui):
    storage = _choose_storage(ui)
    if storage not in {STORAGE_VAULT, STORAGE_SETTINGS}:
        return None
    from zell_projects import projects
    listed = projects(ui)
    project_path = ui.choose('Project directory', [(item['name'], item['path']) for item in listed]) if listed else None
    if project_path is None:
        typed = input('Project directory: ').strip()
        if not typed:
            return None
        project_path = typed
    name = input('Fleet name: ').strip()
    label = input('Project label (not a secret): ').strip()
    workers, slot = [], 0
    while slot < 5:
        if workers and ui.choose('Add another worker?', [('Add worker', True), ('Save fleet', False)]) is not True:
            break
        role = ui.choose('Role', [(item, item) for item in ROLES])
        agent = ui.choose('Client', [(item, item) for item in AGENTS])
        model = input('Model (no default will be substituted): ').strip()
        effort = input('Effort (none, or a Codex level): ').strip()
        if not effort:
            ui.page_text('Effort must be explicit. No default effort was substituted.')
            return None
        prompt_mode = ui.choose('Startup text for this worker', [
            ('None', 'none'), ('Fixed role template', 'template'), ('Custom instructions for this action', 'custom')])
        spec = {'slot': slot, 'role': role, 'agent': agent, 'model': model, 'effort': effort,
                'cwd': project_path, 'prompt_ref': 'none'}
        if prompt_mode == 'template':
            spec['prompt_ref'] = 'template:role.' + role
        elif prompt_mode == 'custom':
            spec['instructions'] = input('Custom instructions (not written to the fleet file): ')
        elif prompt_mode != 'none':
            return None
        workers.append(spec)
        slot += 1
    fleet = save_fleet(name=name, project=label, workers=workers, storage=storage)
    ui.page_text(f'Saved fleet {fleet["name"]}. Instruction storage for the product remains unselected. '
                 'No task was dispatched.')
    return fleet


def _selected_fleet(ui):
    document = read_store()
    return ui.choose('Saved fleets', [(f'{fleet["name"]} — {fleet["project"]}', fleet['id']) for fleet in document['fleets']])


def _interactive_resume(ui):
    fleet_id = _selected_fleet(ui)
    if fleet_id is None:
        return None
    storage = _choose_storage(ui)
    reentry = ui.choose('Custom instructions, if this fleet did not store them, must be typed again.', [
        ('I will re-enter them in the client', True), ('Cancel', False)]) is True
    rebind = ui.choose('If a saved pane identity is stale or occupied, create a new worker instead of adopting it?', [
        ('Rebind those slots', True), ('Keep them and stop', False)]) is True
    report = resume_fleet(ui, fleet_id, storage=storage, reentry=reentry, rebind=rebind)
    ui.page_text(report.message)
    return report


def _interactive_rename(ui):
    fleet_id = _selected_fleet(ui)
    if fleet_id is None:
        return None
    fleet = rename_fleet(fleet_id, input('New fleet name: ').strip())
    ui.page_text(f'Renamed to {fleet["name"]}. Instruction records were not copied.')
    return fleet


def _interactive_delete(ui):
    fleet_id = _selected_fleet(ui)
    if fleet_id is None:
        return None
    confirmation = input('Type the fleet name to delete it: ')
    delete_fleet(fleet_id, confirmation, vault=default_vault())
    ui.page_text('Fleet deleted. Only unshared instruction records owned by that fleet were removed.')


def _interactive_open(ui):
    fleet_id = _selected_fleet(ui)
    if fleet_id is None:
        return None
    paths = open_project(fleet_id)
    ui.page_text('Project directories:\n' + '\n'.join(paths) + '\nNo files were changed and no agent was launched.')
    return paths
