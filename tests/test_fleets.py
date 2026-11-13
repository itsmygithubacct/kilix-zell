"""Fleet metadata: vault-reference startup instructions, through the real launch argv."""
import fcntl
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import zell_agents
import zell_fleets as fleets
import zell_rooms as rooms

CANARY = 'CANARY-FLEET-INSTRUCTION-9f3c2a'
POISON_ARGV = 'POISON-ARGV-RECEIPT-19aa'
POISON_ENV = 'POISON-ENV-SECRET-19bb'


def pane(number, agent='codex'):
    return dict(pane_id=number, tab_id=number + 100, broker=f'broker-{number}', agent=agent)


def keys_of(value):
    found = []
    if isinstance(value, dict):
        for key, item in value.items():
            found.append(key)
            found.extend(keys_of(item))
    elif isinstance(value, list):
        for item in value:
            found.extend(keys_of(item))
    return found


class UI:
    def __init__(self, root):
        self.root = Path(root)
        self.answer = True
        self.prompts = []
        self.pages = []

    def state_read(self, name, default):
        path = self.root / name
        if not path.exists():
            return default
        return json.loads(path.read_text())

    def state_write(self, name, value):
        (self.root / name).write_text(json.dumps(value))

    def choose(self, prompt, options):
        self.prompts.append(prompt)
        return self.answer

    def page_text(self, text):
        self.pages.append(text)


class FleetTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.state = Path(self.tmp.name)
        self.env = patch.dict(os.environ, {
            'KILIX_ZELL_STATE': str(self.state),
            'KILIX_ZELL_ARCHIVE_ONLY': '',
        })
        self.env.start()
        self.addCleanup(self.env.stop)
        self.ui = UI(self.state)
        self.vault = fleets.DirectoryVault(self.state / 'vault')
        self.calls = []
        self.created = []
        self.fail_at = 0
        self.snapshots = 0
        self.snapshot_value = dict(caller_pane=1, panes=[pane(1)])
        self.launch_count = 0

        def control(*args):
            self.calls.append(args)
            if not args or args[0] != 'new-tab':
                raise AssertionError(args)
            self.launch_count += 1
            if self.fail_at and self.launch_count == self.fail_at:
                raise RuntimeError('launcher stopped')
            agent = args[args.index('--agent') + 1]
            made = pane(8 + len(self.created), agent)
            self.created.append(made)
            return dict(status='created', pane=made, argv=['do', POISON_ARGV, CANARY],
                        env={'SECRET': POISON_ENV})

        def snap():
            self.snapshots += 1
            return self.snapshot_value

        self.control_patch = patch.object(rooms, '_control', side_effect=control)
        self.snap_patch = patch.object(rooms, '_snapshot', side_effect=snap)
        self.yolo_patch = patch.object(zell_agents, 'yolo_state', return_value=0)
        self.toggle_patch = patch.object(zell_agents, 'toggle_yolo', side_effect=AssertionError('yolo switch'))
        self.vault_patch = patch.object(fleets, 'default_vault', side_effect=AssertionError('kilix-secrets'))
        self.proc_patch = patch('subprocess.run', side_effect=AssertionError('subprocess'))
        for item in (self.control_patch, self.snap_patch, self.yolo_patch, self.toggle_patch,
                     self.vault_patch, self.proc_patch):
            item.start()
            self.addCleanup(item.stop)
        self.create = rooms._create

    def worker(self, slot=0, role='worker', agent='codex', model='gpt-6-astra', effort='medium',
               cwd=None, prompt_ref='none', instructions=None):
        if cwd is None:
            cwd = self.state / 'ledger'
        cwd = Path(cwd)
        if not cwd.exists():
            cwd.mkdir(parents=True)
        spec = dict(slot=slot, role=role, agent=agent, model=model, effort=effort,
                    cwd=str(cwd), prompt_ref=prompt_ref)
        if instructions is not None:
            spec['instructions'] = instructions
        return spec

    def outside(self, needle):
        skip = (self.state / 'vault').resolve()
        hits = []
        for dirpath, dirnames, filenames in os.walk(self.state):
            current = Path(dirpath).resolve()
            if current == skip or skip in current.parents:
                dirnames[:] = []
                continue
            for name in filenames:
                file = Path(dirpath) / name
                try:
                    data = file.read_bytes()
                except OSError:
                    continue
                if needle.encode() in data:
                    hits.append(str(file))
        return hits

    def argv(self, *, slot, role, agent, model, cwd, effort='none', yolo=False, prompt=None):
        title = f'Zell / {role} / slot {slot} / {Path(cwd).name}'
        args = ['new-tab', 1, '--expect-broker', 'broker-1', '--agent', agent, '--model', model,
                '--own-tab', '--cwd', str(cwd), '--title', title]
        if effort != 'none':
            args += ['--reasoning-effort', effort]
        if yolo:
            args.append('--coding-yolo')
        if prompt:
            args += ['--prompt', prompt]
        return args

    def save(self, workers, storage='settings', **kwargs):
        kwargs.setdefault('vault', self.vault)
        return fleets.save_fleet(name=kwargs.pop('name', 'Night'), project=kwargs.pop('project', 'Ledger'),
                                 workers=workers, storage=storage, **kwargs)

    def test_saved_record_is_metadata_only(self):
        project = self.state / 'ledger'
        fleet = self.save([self.worker(cwd=project, prompt_ref='template:role.worker')])
        self.assertEqual(set(fleet['workers'][0]), fleets.WORKER_KEYS)
        self.assertNotIn('prompt', fleet['workers'][0])
        self.assertEqual(fleet['workers'][0]['prompt_ref'], 'template:role.worker')
        self.assertEqual(fleet['storage_used'], 'settings')
        document = fleets.read_store()
        self.assertEqual(document['instruction_storage'], 'vault')
        self.assertEqual(document['version'], 1)
        raw = json.loads((self.state / 'fleets' / 'fleets.json').read_text())
        self.assertNotIn('prompt', keys_of(raw))
        self.assertNotIn('prompt_ref', keys_of(raw))
        self.assertNotIn('workers', keys_of(raw))
        for key in ('model', 'effort', 'cwd', 'role', 'agent', 'instructions'):
            self.assertNotIn(key, keys_of(raw))
        self.assertEqual(raw['fleets'][0]['worker_count'], 1)
        self.assertTrue(raw['fleets'][0]['config_ref'].startswith('vault:'))
        self.assertNotIn('gpt-6-astra', json.dumps(raw))
        info = (self.state / 'fleets').stat()
        file_info = (self.state / 'fleets' / 'fleets.json').stat()
        self.assertTrue(stat.S_ISDIR(info.st_mode))
        self.assertEqual(info.st_mode & 0o777, 0o700)
        self.assertEqual(file_info.st_mode & 0o777, 0o600)
        self.assertEqual(self.vault.added, 1)
        self.assertIs(rooms._create, self.create)

    def test_heterogeneous_launch_argv_without_yolo(self):
        project = (self.state / 'ledger').resolve()
        project.mkdir()
        workers = [
            self.worker(slot=0, role='worker', agent='codex', model='gpt-6-astra', effort='medium',
                        cwd=project, prompt_ref='template:role.worker'),
            self.worker(slot=1, role='reviewer', agent='grok', model='grok-4', effort='none',
                        cwd=project, prompt_ref='none'),
        ]
        fleet = self.save(workers)
        self.ui.answer = False
        cancelled = fleets.resume_fleet(self.ui, fleet['id'], storage='settings', vault=self.vault)
        self.assertEqual(cancelled.status, 'cancelled')
        self.assertEqual(self.calls, [])
        self.ui.answer = True
        report = fleets.resume_fleet(self.ui, fleet['id'], storage='settings', vault=self.vault)
        self.assertEqual(report.status, 'launched')
        self.assertEqual(report.launched, [0, 1])
        cwd = str(project)
        self.assertEqual(list(self.calls[0]), self.argv(
            slot=0, role='worker', agent='codex', model='gpt-6-astra', cwd=cwd, effort='medium',
            prompt=fleets.ROLE_TEMPLATES['role.worker']))
        self.assertEqual(list(self.calls[1]), self.argv(
            slot=1, role='reviewer', agent='grok', model='grok-4', cwd=cwd))
        flat = [item for args in self.calls for item in args]
        self.assertNotIn('--coding-yolo', flat)
        self.assertNotIn('--resume', flat)
        self.assertNotIn(CANARY, flat)
        self.assertIn('No task was dispatched', report.message)
        journal = json.loads((self.state / 'rooms.json').read_text())
        self.assertNotIn(POISON_ARGV, json.dumps(journal))
        self.assertNotIn(POISON_ENV, json.dumps(journal))
        self.assertNotIn('argv', keys_of(journal))
        self.assertNotIn('env', keys_of(journal))
        self.snapshot_value = dict(caller_pane=1, panes=[pane(1), *self.created])
        self.assertTrue(fleets.configuration_matches(fleet['id'], self.snapshot_value, vault=self.vault))
        self.snapshot_value['panes'][1] = dict(self.snapshot_value['panes'][1], cwd='/tmp/other-folder')
        self.assertFalse(fleets.configuration_matches(fleet['id'], self.snapshot_value, vault=self.vault))
        stored = (self.state / 'fleets' / 'fleets.json').read_text()
        journal = (self.state / 'rooms.json').read_text()
        for surface in (stored, journal):
            self.assertNotIn('gpt-6-astra', surface)
            self.assertNotIn('grok-4', surface)
            self.assertNotIn('reviewer', surface)
            self.assertNotIn(cwd, surface)
            for key in ('model', 'effort', 'cwd', 'role', 'agent', 'workers', 'prompt_ref', 'instructions'):
                self.assertNotIn(key, keys_of(json.loads(surface)))

    def test_yolo_on_adds_the_flag_without_changing_the_switch(self):
        stacked = patch.object(zell_agents, 'yolo_state', return_value=1)
        stacked.start()
        self.addCleanup(stacked.stop)
        project = (self.state / 'ledger').resolve()
        fleet = self.save([self.worker(cwd=project, effort='none', prompt_ref='none')])
        report = fleets.resume_fleet(self.ui, fleet['id'], storage='settings', vault=self.vault)
        self.assertEqual(report.status, 'launched')
        self.assertIn('--coding-yolo', self.calls[0])
        self.assertNotIn('--resume', self.calls[0])
        self.assertEqual(self.calls[0][self.calls[0].index('--model') + 1], 'gpt-6-astra')

    def test_qwen_model_is_not_replaced(self):
        project = (self.state / 'ledger').resolve()
        fleet = self.save([self.worker(agent='qwen-omp', model='qwen-custom', effort='none', cwd=project)])
        fleets.resume_fleet(self.ui, fleet['id'], storage='settings', vault=self.vault)
        self.assertIn('qwen-custom', self.calls[0])
        self.assertNotIn('qwen3.8-max', self.calls[0])

    def test_unavailable_settings_are_refused(self):
        project = self.state / 'ledger'
        project.mkdir()
        refused = [
            self.worker(agent='grok', model='grok-4', effort='medium', cwd=project),
            self.worker(agent='codex', model='gpt-6-astra', effort='ultra', cwd=project),
            self.worker(agent='claude', model='claude-opus', effort='high', cwd=project),
            self.worker(agent='gemini', model='gemini-pro', effort='none', cwd=project),
            dict(slot=0, role='worker', agent='codex', effort='medium', cwd=str(project), prompt_ref='none'),
            dict(slot=0, role='worker', agent='codex', model='gpt-6-astra', cwd=str(project), prompt_ref='none'),
        ]
        for spec in refused:
            with self.assertRaises(fleets.FleetError):
                self.save([spec])
        self.assertEqual(self.calls, [])
        self.assertEqual(self.vault.added, 0)
        self.assertFalse((self.state / 'fleets' / 'fleets.json').exists())

    def test_archive_and_symlink_are_rejected(self):
        archive = self.state / 'archive-root'
        target = archive / 'job'
        target.mkdir(parents=True)
        link = self.state / 'via-link'
        link.symlink_to(target, target_is_directory=True)
        os.environ['KILIX_ZELL_ARCHIVE_ONLY'] = str(archive.resolve())
        for cwd in (target, link):
            with self.assertRaises(ValueError):
                self.save([self.worker(cwd=cwd, instructions=CANARY)], storage='vault')
        self.assertEqual(self.vault.added, 0)
        self.assertEqual(self.calls, [])
        self.assertEqual(self.outside(CANARY), [])
        project = self.state / 'ledger'
        project.mkdir()
        fleet = self.save([self.worker(cwd=project, effort='none')])
        os.environ['KILIX_ZELL_ARCHIVE_ONLY'] = str(project.resolve())
        seen = self.snapshots
        with self.assertRaises(ValueError):
            fleets.resume_fleet(self.ui, fleet['id'], storage='settings', vault=self.vault)
        self.assertEqual(self.snapshots, seen)
        self.assertEqual(self.calls, [])

    def test_malformed_and_old_metadata_are_preserved(self):
        root = self.state / 'fleets'
        root.mkdir()
        os.chmod(root, 0o700)
        path = root / 'fleets.json'
        cases = [
            b'{',
            b'{"version":2,"instruction_storage":"unselected","revision":0,"fleets":[]}\n',
            b'{"version":1,"instruction_storage":"settings","revision":0,"fleets":[]}\n',
            json.dumps({
                'version': 1, 'instruction_storage': 'unselected', 'revision': 1,
                'fleets': [{
                    'id': 'old', 'name': 'Old', 'project': 'Old', 'created': '2026-01-01T00:00:00Z',
                    'updated': '2026-01-01T00:00:00Z', 'storage_used': 'settings', 'bindings': [],
                    'workers': [{
                        'slot': 0, 'role': 'worker', 'agent': 'codex', 'model': 'gpt-6-astra',
                        'effort': 'none', 'cwd': '/tmp', 'prompt_ref': 'none',
                        'prompt': 'OLD-PROMPT-BODY-7c1',
                    }],
                }],
            }).encode() + b'\n',
        ]
        for raw in cases:
            path.write_bytes(raw)
            os.chmod(path, 0o600)
            with self.assertRaises(fleets.FleetError) as caught:
                self.save([self.worker()])
            self.assertEqual(path.read_bytes(), raw)
            self.assertNotIn('OLD-PROMPT-BODY-7c1', str(caught.exception))
        self.assertEqual(self.vault.added, 0)
        legacy = json.dumps({
            'version': 1, 'instruction_storage': 'vault', 'revision': 1,
            'fleets': [{
                'id': 'old', 'name': 'Old', 'project': 'Old', 'created': '2026-01-01T00:00:00Z',
                'updated': '2026-01-01T00:00:00Z', 'storage_used': 'vault', 'bindings': [],
                'workers': [{
                    'slot': 0, 'role': 'reviewer', 'agent': 'codex', 'model': 'model-canary-q8n4',
                    'effort': 'xhigh', 'cwd': '/tmp/cwd-canary-p3', 'prompt_ref': 'template:reenter',
                }],
            }],
        }).encode() + b'\n'
        path.write_bytes(legacy)
        os.chmod(path, 0o600)
        with self.assertRaises(fleets.FleetError):
            fleets.read_store()
        self.assertEqual(path.read_bytes(), legacy)
        with self.assertRaises(fleets.FleetError):
            self.save([self.worker()])
        self.assertEqual(path.read_bytes(), legacy)
        self.assertEqual(self.vault.added, 0)

    def test_duplicate_slot_is_not_published(self):
        spec = self.worker(slot=0, instructions=CANARY)
        with self.assertRaises(fleets.FleetError):
            self.save([spec, dict(spec, instructions=CANARY + '-b')], storage='vault')
        self.assertFalse((self.state / 'fleets' / 'fleets.json').exists())
        self.assertEqual(self.vault.added, 0)
        self.assertEqual(self.outside(CANARY), [])

    def test_lock_blocks_the_second_save(self):
        root = self.state / 'fleets'
        root.mkdir()
        os.chmod(root, 0o700)
        path = root / 'fleets.json'
        path.write_text('UNTOUCHED\n')
        os.chmod(path, 0o600)
        fd = os.open(root / '.lock', os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaises(fleets.LockBusy):
                self.save([self.worker()])
        finally:
            fcntl.flock(fd, fcntl.LOCK_UN)
            os.close(fd)
        self.assertEqual(path.read_text(), 'UNTOUCHED\n')

    def test_vault_canary_stays_in_the_vault(self):
        project = (self.state / 'ledger').resolve()
        fleet = self.save([self.worker(cwd=project, instructions=CANARY)], storage='vault')
        self.assertTrue(fleet['workers'][0]['prompt_ref'].startswith('vault:'))
        self.assertNotIn(CANARY, fleet['workers'][0]['prompt_ref'])
        self.assertEqual(self.outside(CANARY), [])
        record = fleet['workers'][0]['prompt_ref'].split(':')[1]
        original = (self.state / 'vault' / record).read_bytes()
        self.assertIn(CANARY.encode(), original)
        self.assertEqual((self.state / 'vault' / record).stat().st_mode & 0o777, 0o600)
        self.vault.locked = True
        waiting = fleets.resume_fleet(self.ui, fleet['id'], storage='vault', vault=self.vault)
        self.assertEqual(waiting.status, 'waiting')
        self.assertEqual(self.calls, [])
        self.assertEqual(self.ui.prompts, [])
        self.assertNotIn(CANARY, waiting.message)
        self.vault.locked = False
        os.unlink(self.state / 'vault' / record)
        missing = fleets.resume_fleet(self.ui, fleet['id'], storage='vault', vault=self.vault)
        self.assertEqual(missing.status, 'waiting')
        self.assertEqual(self.calls, [])
        (self.state / 'vault' / record).write_bytes(CANARY.encode())
        os.chmod(self.state / 'vault' / record, 0o600)
        poisoned = fleets.resume_fleet(self.ui, fleet['id'], storage='vault', vault=self.vault)
        self.assertEqual(poisoned.status, 'waiting')
        self.assertEqual(self.calls, [])
        self.assertNotIn(CANARY, poisoned.message)
        (self.state / 'vault' / record).write_bytes(original)
        os.chmod(self.state / 'vault' / record, 0o600)
        report = fleets.resume_fleet(self.ui, fleet['id'], storage='vault', vault=self.vault)
        self.assertEqual(report.status, 'launched')
        self.assertNotIn('--prompt', self.calls[0])
        self.assertNotIn(CANARY, self.calls[0])
        self.assertNotIn(CANARY, '\n'.join(self.ui.prompts))
        self.assertEqual(self.outside(CANARY), [])
        self.assertEqual(self.outside(POISON_ARGV), [])
        self.assertEqual(self.outside(POISON_ENV), [])

    def test_settings_canary_is_reentered(self):
        project = (self.state / 'ledger').resolve()
        fleet = self.save([self.worker(cwd=project, instructions=CANARY)], storage='settings')
        self.assertEqual(fleet['workers'][0]['prompt_ref'], 'template:reenter')
        self.assertEqual(self.vault.added, 1)
        sealed = (self.state / 'vault' / fleet['config_ref'].split(':')[1]).read_bytes()
        self.assertNotIn(CANARY.encode(), sealed)
        self.assertEqual(self.outside(CANARY), [])
        held = fleets.resume_fleet(self.ui, fleet['id'], storage='settings', vault=self.vault)
        self.assertEqual(held.status, 'needs_reentry')
        self.assertEqual(self.calls, [])
        self.assertEqual(self.ui.prompts, [])
        report = fleets.resume_fleet(self.ui, fleet['id'], storage='settings', vault=self.vault, reentry=True)
        self.assertEqual(report.status, 'launched')
        prompt = self.calls[0][self.calls[0].index('--prompt') + 1]
        self.assertEqual(prompt, fleets.REENTER_TEXT)
        self.assertNotIn(CANARY, self.calls[0])
        self.assertEqual(self.outside(CANARY), [])

    def test_failed_publish_keeps_the_old_fleet_and_the_new_record(self):
        first = self.save([self.worker(effort='none')], name='Old')
        before = (self.state / 'fleets' / 'fleets.json').read_bytes()
        added = self.vault.added
        with patch.object(fleets, 'commit_document', side_effect=fleets.FleetError('publish failed')):
            with self.assertRaises(fleets.FleetError) as caught:
                self.save([self.worker(slot=1, instructions=CANARY)], storage='vault', name='New')
        self.assertNotIn(CANARY, str(caught.exception))
        self.assertEqual((self.state / 'fleets' / 'fleets.json').read_bytes(), before)
        self.assertEqual(fleets.read_store()['fleets'][0]['id'], first['id'])
        self.assertEqual(self.vault.added, added + 1)
        self.assertEqual(self.outside(CANARY), [])

    def test_shared_vault_record_survives_until_the_last_fleet(self):
        project = self.state / 'ledger'
        project.mkdir()
        first = self.save([self.worker(cwd=project, instructions=CANARY)], storage='vault', name='Alpha')
        ref = first['workers'][0]['prompt_ref']
        record = ref.split(':')[1]
        second = self.save([self.worker(cwd=project, prompt_ref=ref, effort='none')], storage='vault', name='Beta')
        self.assertEqual(self.vault.added, 2)
        self.assertEqual(second['workers'][0]['prompt_ref'], ref)
        with self.assertRaises(fleets.FleetError):
            fleets.delete_fleet(first['id'], 'nope', vault=self.vault)
        self.assertTrue((self.state / 'vault' / record).exists())
        fleets.delete_fleet(first['id'], 'Alpha', vault=self.vault)
        self.assertTrue((self.state / 'vault' / record).exists())
        self.assertEqual([item['id'] for item in fleets.read_store()['fleets']], [second['id']])
        fleets.delete_fleet(second['id'], 'Beta', vault=self.vault)
        self.assertFalse((self.state / 'vault' / record).exists())
        self.assertEqual(fleets.read_store()['fleets'], [])

    def test_rename_does_not_copy_the_record(self):
        project = self.state / 'ledger'
        fleet = self.save([self.worker(cwd=project, instructions=CANARY)], storage='vault', name='Alpha')
        added = self.vault.added
        renamed = fleets.rename_fleet(fleet['id'], 'Renamed')
        self.assertEqual(renamed['name'], 'Renamed')
        self.assertEqual(renamed['config_ref'], fleet['config_ref'])
        self.assertNotIn('workers', renamed)
        self.assertEqual(self.vault.added, added)
        self.assertEqual(self.outside(CANARY), [])

    def test_partial_launch_retries_without_duplicating(self):
        project = (self.state / 'ledger').resolve()
        workers = [self.worker(slot=slot, effort='none', cwd=project) for slot in range(3)]
        fleet = self.save(workers)
        self.fail_at = 2
        report = fleets.resume_fleet(self.ui, fleet['id'], storage='settings', vault=self.vault)
        self.assertEqual(report.status, 'partial')
        self.assertEqual(report.launched, [0])
        self.assertEqual(report.unknown, [1])
        self.assertIn('Completed slots: [0]', report.message)
        self.assertIn('Unknown slots: [1]', report.message)
        self.assertIn('No task was dispatched', report.message)
        self.assertEqual(len(self.calls), 2)
        self.fail_at = 0
        self.calls.clear()
        self.snapshot_value = dict(caller_pane=1, panes=[pane(1), *self.created])
        again = fleets.resume_fleet(self.ui, fleet['id'], storage='settings', vault=self.vault)
        titles = [args[args.index('--title') + 1] for args in self.calls]
        self.assertTrue(titles)
        self.assertTrue(all('slot 0' not in title for title in titles))
        self.assertIn('slot 1', titles[0])
        self.assertEqual(again.launched, [1, 2])

    def test_stale_identity_is_not_adopted(self):
        project = (self.state / 'ledger').resolve()
        fleet = self.save([self.worker(cwd=project, effort='none')])
        fleets.resume_fleet(self.ui, fleet['id'], storage='settings', vault=self.vault)
        original = fleets.read_store()['fleets'][0]['bindings'][0]
        recycled = dict(self.created[0], broker='recycled')
        self.snapshot_value = dict(caller_pane=1, panes=[pane(1), recycled])
        self.calls.clear()
        self.ui.prompts.clear()
        stale = fleets.resume_fleet(self.ui, fleet['id'], storage='settings', vault=self.vault)
        self.assertEqual(stale.status, 'stale')
        self.assertEqual(self.calls, [])
        self.assertEqual(self.ui.prompts, [])
        self.assertEqual(fleets.read_store()['fleets'][0]['bindings'][0], original)
        rebound = fleets.resume_fleet(self.ui, fleet['id'], storage='settings', vault=self.vault, rebind=True)
        self.assertEqual(rebound.status, 'launched')
        binding = fleets.read_store()['fleets'][0]['bindings'][0]
        self.assertNotEqual(binding['pane_id'], recycled['pane_id'])
        self.assertNotEqual(binding['broker'], 'recycled')
        self.assertEqual(binding['broker'], self.created[-1]['broker'])

    def test_occupied_screen_is_not_taken(self):
        project = (self.state / 'ledger').resolve()
        fleet = self.save([self.worker(cwd=project, effort='none')])
        fleets.resume_fleet(self.ui, fleet['id'], storage='settings', vault=self.vault)
        original = fleets.read_store()['fleets'][0]['bindings'][0]
        self.ui.state_write('rooms.json', {'screens': {'0': pane(5, 'codex')}})
        self.snapshot_value = dict(caller_pane=1, panes=[pane(1), pane(5, 'codex')])
        self.calls.clear()
        report = fleets.resume_fleet(self.ui, fleet['id'], storage='settings', vault=self.vault)
        self.assertEqual(report.status, 'stale')
        self.assertIn(0, report.occupied)
        self.assertEqual(self.calls, [])
        self.assertEqual(fleets.read_store()['fleets'][0]['bindings'][0], original)

    def test_unbound_live_screen_is_adopted(self):
        project = (self.state / 'ledger').resolve()
        fleet = self.save([self.worker(cwd=project, effort='none')])
        self.ui.state_write('rooms.json', {'screens': {'0': pane(4, 'codex')}})
        self.snapshot_value = dict(caller_pane=1, panes=[pane(1), pane(4, 'codex')])
        self.ui.answer = False
        cancelled = fleets.resume_fleet(self.ui, fleet['id'], storage='settings', vault=self.vault)
        self.assertEqual(cancelled.status, 'cancelled')
        self.assertEqual(fleets.read_store()['fleets'][0]['bindings'], [])
        self.ui.answer = True
        report = fleets.resume_fleet(self.ui, fleet['id'], storage='settings', vault=self.vault)
        self.assertEqual(report.status, 'restored')
        self.assertEqual(self.calls, [])
        binding = fleets.read_store()['fleets'][0]['bindings'][0]
        self.assertEqual(binding['pane_id'], 4)
        self.assertEqual(binding['broker'], 'broker-4')
        self.assertNotIn('agent', binding)
        self.assertTrue(fleets.configuration_matches(fleet['id'], self.snapshot_value, vault=self.vault))

    def test_open_project_does_not_edit_or_launch(self):
        project = self.state / 'ledger'
        project.mkdir()
        (project / 'keep.txt').write_text('keep')
        before = sorted(path.name for path in project.iterdir())
        fleet = self.save([self.worker(cwd=project, effort='none')])
        opened = fleets.open_project(fleet['id'], vault=self.vault)
        self.assertEqual(opened, [str(project.resolve())])
        self.assertEqual(sorted(path.name for path in project.iterdir()), before)
        self.assertEqual((project / 'keep.txt').read_text(), 'keep')
        self.assertEqual(self.calls, [])

    def test_monitor_frame_is_sanitized_and_paged(self):
        self.assertEqual(fleets.monitor_frame(), '1 0 0 0 0')
        project = self.state / 'ledger'
        ids = []
        for number in range(1, 10):
            fleet = self.save([self.worker(cwd=project, effort='none')], name=f'Fleet {number}', project='Ledger')
            ids.append(fleet['id'])
        frame = fleets.monitor_frame(self.snapshot_value)
        lines = frame.splitlines()
        self.assertEqual(lines[0], '1 9 9 0 8')
        self.assertEqual(len(lines), 9)
        self.assertIn('Ledger', lines[1])
        self.assertNotIn(str(project.resolve()), frame)
        self.assertNotIn(CANARY, frame)
        last = fleets.monitor_frame(offset=8)
        self.assertEqual(last.splitlines()[0], '1 9 9 8 1')
        self.assertIn('Fleet 9', last)
        self.assertIn(ids[-1], last)
        self.assertIsNone(fleets.monitor_frame(offset=9))
        path = self.state / 'fleets' / 'fleets.json'
        path.write_text('{')
        with self.assertRaises(fleets.StoreCorrupt):
            fleets.monitor_frame()

    def test_new_save_reloads_the_vault_instruction(self):
        project = (self.state / 'ledger').resolve()
        fleet = fleets.save_fleet(
            name='Night', project='Ledger',
            workers=[self.worker(cwd=project, instructions=CANARY)],
            storage=None, vault=self.vault)
        document = fleets.read_store()
        raw = (self.state / 'fleets' / 'fleets.json').read_text()
        self.assertEqual(document['instruction_storage'], 'vault')
        self.assertNotEqual(document['instruction_storage'], 'unselected')
        self.assertNotEqual(document['instruction_storage'], 'settings')
        self.assertEqual(fleet['storage_used'], 'vault')
        self.assertTrue(fleet['workers'][0]['prompt_ref'].startswith('vault:'))
        self.assertNotIn(CANARY, raw)
        self.assertNotIn(CANARY, fleet['workers'][0]['prompt_ref'])
        self.assertEqual(fleets.reload_instruction(fleet['id'], 0, self.vault), CANARY)
        self.assertEqual(self.ui.prompts, [])

    def test_storage_must_be_chosen_per_call(self):
        with self.assertRaises(fleets.InstructionStorageUnselected):
            fleets.save_fleet(name='A', project='B', workers=[self.worker()], storage='unselected', vault=self.vault)
        self.assertFalse((self.state / 'fleets' / 'fleets.json').exists())
        settings = self.save([self.worker(effort='none')], storage='settings', name='Settings')
        vaulted = self.save([self.worker(slot=1, effort='none')], storage='vault', name='Vaulted')
        document = fleets.read_store()
        self.assertEqual(document['instruction_storage'], 'vault')
        self.assertEqual({item['storage_used'] for item in document['fleets']}, {'settings', 'vault'})
        before = (self.state / 'fleets' / 'fleets.json').read_bytes()
        with self.assertRaises(fleets.FleetError):
            fleets.resume_fleet(self.ui, settings['id'], storage='vault', vault=self.vault)
        with self.assertRaises(fleets.FleetError):
            fleets.resume_fleet(self.ui, vaulted['id'], storage='settings', vault=self.vault)
        self.assertEqual((self.state / 'fleets' / 'fleets.json').read_bytes(), before)
        self.assertEqual(self.calls, [])

    def test_kimi_does_not_receive_launch_text(self):
        project = self.state / 'ledger'
        project.mkdir()
        with self.assertRaises(fleets.FleetError):
            self.save([self.worker(agent='kimi', model='kimi-k2', effort='none', cwd=project,
                                   prompt_ref='template:role.worker')])
        self.assertFalse((self.state / 'fleets' / 'fleets.json').exists())
        fleet = self.save([self.worker(agent='kimi', model='kimi-k2', effort='none', cwd=project,
                                       instructions='type this again')], storage='settings')
        self.assertEqual(fleet['workers'][0]['prompt_ref'], 'template:reenter')
        with self.assertRaises(fleets.FleetError):
            fleets.resume_fleet(self.ui, fleet['id'], storage='settings', vault=self.vault, reentry=True)
        self.assertEqual(self.calls, [])

    def test_prompt_check_is_length_only(self):
        self.assertEqual(rooms._check_prompt('a' * 1024), 'a' * 1024)
        self.assertEqual(rooms._check_prompt(CANARY), CANARY)
        with self.assertRaises(ValueError):
            rooms._check_prompt('a' * 1025)
        self.assertIsNone(fleets.CONFIDENTIAL_PROMPT_TRANSPORT)

    def test_yolo_unreadable_refuses_launch(self):
        stacked = patch.object(zell_agents, 'yolo_state', return_value=-1)
        stacked.start()
        self.addCleanup(stacked.stop)
        fleet = self.save([self.worker(effort='none')])
        with self.assertRaises(fleets.FleetError) as caught:
            fleets.resume_fleet(self.ui, fleet['id'], storage='settings', vault=self.vault)
        self.assertIn('not changed', str(caught.exception))
        self.assertEqual(self.calls, [])
        self.assertEqual(self.ui.prompts, [])

    def test_pilot_menu_cancel_and_launch_path(self):
        self.ui.answer = None
        self.assertIsNone(fleets.pilot_menu(self.ui))
        self.assertFalse((self.state / 'fleets').exists())
        with patch.object(rooms, 'pilot', return_value='rooms-pilot') as pilot:
            self.ui.answer = 'launch'
            self.assertEqual(fleets.pilot_menu(self.ui), 'rooms-pilot')
            pilot.assert_called_once_with(self.ui)


if __name__ == '__main__':
    unittest.main()
