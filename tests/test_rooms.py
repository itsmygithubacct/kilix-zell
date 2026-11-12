import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import zell_rooms as rooms


def pane(number, agent='codex'):
    return dict(pane_id=number, tab_id=number + 100, broker=f'broker-{number}', agent=agent)


class RoomsTests(unittest.TestCase):
    def setUp(self):
        self.ui = SimpleNamespace(state_read=Mock(return_value={}), state_write=Mock(),
                                  choose=Mock(), page_text=Mock(), file_pick=Mock(), open_file=Mock())
        # No test may accidentally contact a live terminal or dispatch a real task.
        self.run_patch = patch.object(rooms.subprocess, 'run', side_effect=AssertionError('Unmocked terminal call'))
        self.run_patch.start()
        self.addCleanup(self.run_patch.stop)

    def test_live_preview_is_bounded_and_receipt_revalidated(self):
        self.ui.state_read.return_value = {'screens': {'0': pane(2)}}
        snapshot = json.dumps({'panes': [pane(2)]})
        with patch.object(rooms, '_command', side_effect=[snapshot, 'x'*10000, snapshot]) as command:
            rows = rooms.previews(self.ui)
        self.assertIn('live screen', rows[0]['status'])
        self.assertLessEqual(len(rows[0]['text']), 1100)
        self.assertTrue(all(c.kwargs['timeout'] == 2 for c in command.call_args_list))
        self.ui.state_write.assert_not_called()
        changed = json.dumps({'panes': [dict(pane(2), broker='recycled') ]})
        with patch.object(rooms, '_command', side_effect=[snapshot, 'screen contents', changed]):
            rows = rooms.previews(self.ui)
        self.assertEqual(rows[0]['status'], 'Session changed')
        self.assertNotIn('screen contents', str(rows))

    def test_unverified_clients_are_never_captured(self):
        self.ui.state_read.return_value = {'screens': {'0': pane(2)}}
        snapshot = json.dumps({'panes': [pane(2, 'shell')]})
        with patch.object(rooms, '_command', return_value=snapshot) as command:
            rows = rooms.previews(self.ui)
        self.assertEqual(rows[0]['status'], 'Client not verified')
        self.assertFalse(any('dump' in c.args[0] for c in command.call_args_list))

    def test_launch_saves_exact_receipt_and_uses_own_tab(self):
        self.ui.choose.return_value = 'start'
        responses = [dict(caller_pane=1, panes=[pane(1)]), dict(status='created', pane=pane(8))]
        with patch.object(rooms, '_control', side_effect=responses) as control, patch('builtins.input', return_value=str(Path.home())):
            rooms.screen(self.ui, 2)
        self.assertEqual(self.ui.state_write.call_args.args[1]['screens']['2'], pane(8))
        argv = control.call_args.args
        self.assertIn('--coding-yolo', argv)
        self.assertIn('--own-tab', argv)
        self.assertIn('gpt-6-astra', argv)
        self.assertIn('--expect-broker', argv)

    def test_unverified_caller_cannot_launch(self):
        self.ui.choose.return_value = 'start'
        with patch.object(rooms, '_control', return_value=dict(caller_pane=None, panes=[pane(1)])) as control:
            rooms.screen(self.ui, 1)
        self.assertEqual(control.call_count, 1)
        self.ui.state_write.assert_not_called()

    def test_invalid_receipt_not_saved(self):
        self.ui.choose.return_value = 'start'
        with patch.object(rooms, '_control', side_effect=[dict(caller_pane=1, panes=[pane(1)]), dict(status='created', pane={})]), patch('builtins.input', return_value=str(Path.home())):
            rooms.screen(self.ui, 1)
        self.ui.state_write.assert_not_called()

    def test_archive_disk_cannot_be_working_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive=Path(tmp).resolve()/'archive';(archive/'projects').mkdir(parents=True)
            with patch.dict(os.environ, {'KILIX_ZELL_ARCHIVE_ONLY': str(archive), 'KILIX_ZELL_STATE': tmp}), \
                    patch('builtins.input', return_value=str(archive/'projects')), patch.object(rooms, '_control') as control:
                with self.assertRaisesRegex(ValueError, 'archive storage'):
                    rooms._start(self.ui, 0, {'screens': {}}, dict(caller_pane=1, panes=[pane(1)]))
            control.assert_not_called()

    def test_no_folder_is_reserved_unless_this_machine_lists_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            project=Path(tmp).resolve()/'archive/projects';project.mkdir(parents=True)
            with patch.dict(os.environ, {'KILIX_ZELL_STATE': tmp}):
                os.environ.pop('KILIX_ZELL_ARCHIVE_ONLY', None)
                self.assertEqual(rooms._project_path(project), project)
                (Path(tmp)/'archive-only-paths').write_text('# archives\n'+str(project.parent)+'\n')
                with self.assertRaisesRegex(ValueError, 'archive storage'):
                    rooms._project_path(project)

    def test_recycled_pane_id_cannot_be_focused(self):
        self.ui.state_read.return_value = {'screens': {'1': pane(2)}}
        self.ui.choose.return_value = 'focus'
        changed = dict(pane(2), broker='replacement')
        with patch.object(rooms, '_snapshot', side_effect=[dict(panes=[pane(2)]), dict(panes=[changed])]):
            rooms.screen(self.ui, 1)
        self.assertIn('identity changed', self.ui.page_text.call_args.args[0])

    def test_task_dispatch_requires_confirmation_and_rechecks_identity(self):
        self.ui.choose.side_effect = [pane(2), True]
        state = {'screens': {'1': pane(2)}}
        with patch.object(rooms, '_snapshot', return_value=dict(panes=[pane(2)])), patch('builtins.input', return_value='Run the unit tests'), patch.object(rooms, '_control', return_value={'delivery_verified': True}) as control:
            rooms._dispatch(self.ui, state)
        argv = control.call_args.args
        self.assertEqual(argv[0], 'deliver')
        self.assertIn('broker-2', argv)
        self.assertIn('Run the unit tests', argv)

    def test_cancel_never_sends(self):
        self.ui.choose.side_effect = [pane(2), False]
        with patch.object(rooms, '_snapshot', return_value=dict(panes=[pane(2)])), patch('builtins.input', return_value='Do a task'), patch.object(rooms, '_control') as control:
            rooms._dispatch(self.ui, {'screens': {'1': pane(2)}})
        control.assert_not_called()

    def test_changed_worker_never_receives_task(self):
        self.ui.choose.side_effect = [pane(2), True]
        with patch.object(rooms, '_snapshot', side_effect=[dict(panes=[pane(2)]), dict(panes=[])]), patch('builtins.input', return_value='Do a task'), patch.object(rooms, '_control') as control:
            with self.assertRaisesRegex(ValueError, 'identity changed'):
                rooms._dispatch(self.ui, {'screens': {'1': pane(2)}})
        control.assert_not_called()

    def test_binary_browse_does_not_launch_external_handler(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'example.bin'
            path.write_bytes(b'example')
            self.ui.choose.return_value = 'browse'
            self.ui.file_pick.return_value = path
            with patch.object(rooms, 'disks', return_value=[dict(path=directory, label='Test disk', used_percent=1, total=100, free=99)]):
                rooms.laundry(self.ui, 0)
            self.ui.open_file.assert_not_called()
            self.assertEqual(path.read_bytes(), b'example')

    def test_project_regular_tab_journals_without_screen_binding(self):
        with patch.object(rooms, '_snapshot', return_value=dict(caller_pane=1, panes=[pane(1)])), patch.object(rooms, '_control', return_value=dict(status='created', pane=pane(8))):
            result = rooms.project_session(self.ui, Path.home())
        state = self.ui.state_write.call_args.args[1]
        self.assertEqual(result, pane(8))
        self.assertEqual(state['screens'], {})
        self.assertEqual(state['sessions'][0]['receipt']['pane'], pane(8))

    def test_project_replacement_requires_explicit_confirmation(self):
        self.ui.state_read.return_value = {'screens': {'0': pane(2)}}
        self.ui.choose.return_value = False
        with patch.object(rooms, '_snapshot', return_value=dict(caller_pane=1, panes=[pane(1), pane(2)])), patch.object(rooms, '_control') as control:
            rooms.project_session(self.ui, Path.home(), screen=0)
        control.assert_not_called()
        self.ui.state_write.assert_not_called()

    def test_confirmed_replacement_retains_old_pane(self):
        self.ui.state_read.return_value = {'screens': {'0': pane(2)}}
        self.ui.choose.return_value = True
        with patch.object(rooms, '_snapshot', return_value=dict(caller_pane=1, panes=[pane(1), pane(2)])), patch.object(rooms, '_control', return_value=dict(status='created', pane=pane(8))):
            rooms.project_session(self.ui, Path.home(), screen=0)
        state = self.ui.state_write.call_args.args[1]
        self.assertEqual(state['screens']['0'], pane(8))
        self.assertEqual(state['retired_screens'][0]['pane'], pane(2))

    def test_pilot_cancel_preview_launches_nothing(self):
        project = dict(path=str(Path.home()), name='Example')
        module = SimpleNamespace(projects=lambda ui: [project])
        self.ui.choose.side_effect = [project, [(0, 'Coding session')], False]
        with patch.dict(sys.modules, zell_projects=module), patch.object(rooms, '_control') as control:
            rooms.pilot(self.ui)
        control.assert_not_called()

    def test_pilot_partial_failure_preserves_first_receipt_without_retry(self):
        project = dict(path=str(Path.home()), name='Example')
        module = SimpleNamespace(projects=lambda ui: [project])
        self.ui.choose.side_effect = [project, [(1, 'Worker 1'), (2, 'Worker 2'), (0, 'Coordinator')], True]
        with patch.dict(sys.modules, zell_projects=module), patch.object(rooms, '_snapshot', return_value=dict(caller_pane=1, panes=[pane(1)])), patch.object(rooms, '_control', side_effect=[dict(status='created', pane=pane(8)), RuntimeError('creation unconfirmed')]) as control:
            rooms.pilot(self.ui)
        self.assertEqual(control.call_count, 2)
        self.ui.state_write.assert_called_once()
        self.assertEqual(self.ui.state_write.call_args.args[1]['screens']['1'], pane(8))
        self.assertIn('1 successful creation receipts saved', self.ui.page_text.call_args.args[0])

    def test_pilot_coordinator_receives_exact_worker_receipts(self):
        project = dict(path=str(Path.home()), name='Example')
        module = SimpleNamespace(projects=lambda ui: [project])
        self.ui.choose.side_effect = [project, [(1, 'Worker 1'), (2, 'Worker 2'), (0, 'Coordinator')], True]
        with patch.dict(sys.modules, zell_projects=module), patch.object(rooms, '_snapshot', return_value=dict(caller_pane=1, panes=[pane(1)])), patch.object(rooms, '_control', side_effect=[dict(status='created', pane=pane(i)) for i in [8, 9, 10]]) as control:
            rooms.pilot(self.ui)
        self.assertEqual(self.ui.state_write.call_count, 3)
        self.assertIn('broker-8', control.call_args.args[-1])
        self.assertIn('broker-9', control.call_args.args[-1])
        self.assertEqual(self.ui.state_write.call_args.args[1]['screens']['0'], pane(10))

    def test_pilot_coordinator_prompt_fits_with_realistic_receipts(self):
        # Real receipts carry far more than an identity; embedding them overran 1024 bytes.
        def real(number):
            return dict(pane(number), broker='b' * 64, cwd='/srv/projects/example', title='Zell / Worker',
                        env=dict(KITTY_PTY_BROKER_SESSION='0' * 16), foreground=['codex', '--no-daemon'] * 20)
        self.assertGreater(len(json.dumps([real(8), real(9)]).encode()), 1024)
        project = dict(path=str(Path.home()), name='Example')
        module = SimpleNamespace(projects=lambda ui: [project])
        self.ui.choose.side_effect = [project, [(1, 'Worker 1'), (2, 'Worker 2'), (0, 'Coordinator')], True]
        with patch.dict(sys.modules, zell_projects=module), patch.object(rooms, '_snapshot', return_value=dict(caller_pane=1, panes=[pane(1)])), patch.object(rooms, '_control', side_effect=[dict(status='created', pane=real(i)) for i in [8, 9, 10]]) as control:
            rooms.pilot(self.ui)
        self.assertEqual(control.call_count, 3)
        prompt = control.call_args.args[-1]
        self.assertLessEqual(len(prompt.encode()), 1024)
        self.assertIn('pane 8 tab 108 broker ' + 'b' * 64, prompt)
        self.assertIn('pane 9 tab 109', prompt)
        self.assertEqual(self.ui.state_write.call_args.args[1]['screens']['0']['pane_id'], 10)

    def test_pilot_coordinator_falls_back_to_tab_titles_when_identities_overrun(self):
        workers = [dict(pane(n), broker='b' * 500) for n in (8, 9)]
        prompt = rooms._role_prompt('Coordinator', Path('/srv/example'), workers)
        self.assertLessEqual(len(prompt.encode()), 1024)
        self.assertIn('Zell / Worker N / example', prompt)
        self.assertNotIn('b' * 500, prompt)

    def test_pilot_oversized_prompt_launches_nothing(self):
        with tempfile.TemporaryDirectory() as root:
            deep = Path(root, *['d' * 200] * 5)
            deep.mkdir(parents=True)
            project = dict(path=str(deep), name='Deep')
            module = SimpleNamespace(projects=lambda ui: [project])
            self.ui.choose.side_effect = [project, [(1, 'Worker 1'), (2, 'Worker 2'), (0, 'Coordinator')], True]
            with patch.dict(sys.modules, zell_projects=module), patch.object(rooms, '_snapshot', return_value=dict(caller_pane=1, panes=[pane(1)])), patch.object(rooms, '_control') as control:
                rooms.pilot(self.ui)
        control.assert_not_called()
        self.ui.state_write.assert_not_called()
        self.assertEqual(self.ui.choose.call_count, 2)  # never reached the launch confirmation
        self.assertIn('Kilix accepts 1024', self.ui.page_text.call_args.args[0])

    def test_mounts_deduplicate_bind_and_skip_pseudo(self):
        mounts = ('1 0 8:1 / / rw - ext4 /dev/sda1 rw\n'
                  '2 0 8:1 /home /mnt/copy rw - ext4 /dev/sda1 rw\n'
                  '3 0 0:2 / /proc rw - proc proc rw\n'
                  '4 0 7:0 / /snap/x rw - squashfs /dev/loop0 ro\n'
                  '5 0 8:2 / /mnt/My\\040Disk rw - ext4 /dev/sdb1 rw\n')
        with patch.object(Path, 'read_text', return_value=mounts), patch.object(rooms.shutil, 'disk_usage', side_effect=[(100, 70, 30), PermissionError()]):
            found = rooms.disks()
        self.assertEqual([d['path'] for d in found], ['/', '/mnt/My Disk'])
        self.assertEqual(found[0]['used_percent'], 70)
        self.assertIsNone(found[1]['used_percent'])

    def test_mountinfo_unavailable(self):
        with patch.object(Path, 'read_text', side_effect=PermissionError()):
            self.assertEqual(rooms.disks(), [])

    def test_overflow_bin_lists_all_remaining_disks(self):
        rows = [dict(path=f'/disk{i}', label=str(i), used_percent=1, total=100, free=99) for i in range(10)]
        self.ui.choose.side_effect = [rows[9], None]
        with patch.object(rooms, 'disks', return_value=rows):
            rooms.laundry(self.ui, 7)
        self.assertEqual(len(self.ui.choose.call_args_list[0].args[1]), 3)
        self.ui.file_pick.assert_not_called()


if __name__ == '__main__':
    unittest.main()
