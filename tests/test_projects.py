import copy
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import zell_projects as projects


class ProjectTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = str(Path(self.temp.name).resolve())
        self.state = {}
        self.ui = Mock(BASE=Path(self.path))
        self.ui.state_read.side_effect = lambda name, default: copy.deepcopy(self.state.get(name, default))
        self.ui.state_write.side_effect = lambda name, value: self.state.update({name: copy.deepcopy(value)})
        projects._cache.clear()

    def registered(self, watch=False):
        self.state['projects.json'] = [{'name': 'Example', 'path': self.path, 'watch': watch}]

    def test_registration_normalizes_repo_root_and_watch_is_opt_in(self):
        child = Path(self.path) / 'subfolder'
        child.mkdir()
        with patch.object(projects, 'repo_root', return_value=self.path):
            entry = projects.register(self.ui, str(child))
            self.assertEqual(entry['path'], self.path)
            self.assertFalse(entry['watch'])
            projects.register(self.ui, self.path)
            self.assertEqual(len(projects.projects(self.ui)), 1)
            projects.set_watch(self.ui, self.path, True)
        self.assertTrue(projects.projects(self.ui)[0]['watch'])

    def test_nonrepo_can_register_but_cannot_watch(self):
        with patch.object(projects, 'repo_root', return_value=None):
            projects.register(self.ui, self.path)
            with self.assertRaises(ValueError):
                projects.set_watch(self.ui, self.path, True)
        self.assertFalse(projects.projects(self.ui)[0]['watch'])

    def test_metadata_is_validated_without_git_or_implicit_scans(self):
        self.state['projects.json'] = [None, {'path': 'relative'}, {'path': self.path, 'watch': 'yes'}, {'path': self.path}]
        with patch.object(projects, 'git') as git, patch.object(projects, 'status') as status:
            self.assertEqual(projects.projects(self.ui), [{'path': self.path, 'name': Path(self.path).name, 'watch': False}])
            self.assertEqual(projects.dirty_summary(self.ui)['count'], 0)
            git.assert_not_called()
            status.assert_not_called()

    def test_porcelain_counts_files_once_including_renames_and_untracked(self):
        raw = b'MM staged-and-unstaged\0R  renamed\0old-name\0?? folder/a\0?? folder/b\0 M line\nbreak\0'
        self.assertEqual(len(projects.dirty_files(raw)), 5)
        self.assertEqual(projects.dirty_files(b' M same\0 M same\0'), [b'same'])
        with self.assertRaises(ValueError):
            projects.dirty_files(b'R  missing-source\0')

    def test_watch_cache_ttl_refresh_and_disable(self):
        self.registered(True)
        with patch.object(projects, 'status', return_value=[b'a', b'b']) as status, patch.object(projects.time, 'monotonic', return_value=100) as clock:
            self.assertEqual(projects.dirty_summary(self.ui)['count'], 2)
            projects.dirty_summary(self.ui)
            self.assertEqual(status.call_count, 1)
            clock.return_value = 160
            projects.dirty_summary(self.ui)
            self.assertEqual(status.call_count, 2)
            projects.dirty_summary(self.ui, refresh=True)
            self.assertEqual(status.call_count, 3)
            projects.set_watch(self.ui, self.path, False)
            self.assertEqual(projects.dirty_summary(self.ui)['count'], 0)
            self.assertEqual(status.call_count, 3)

    def test_missing_watched_repository_is_unknown_not_clean(self):
        self.registered(True)
        with patch.object(projects, 'status', side_effect=OSError('offline')):
            summary = projects.dirty_summary(self.ui)
        self.assertFalse(summary['known'])
        self.assertEqual(len(summary['errors']), 1)
        self.assertIn('offline', summary['details'])

    def test_git_is_readonly_and_ambient_repository_override_removed(self):
        with patch.object(projects.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stdout=b'', stderr=b'')) as run, patch.dict(projects.os.environ, {'GIT_DIR': '/wrong', 'GIT_OPTIONAL_LOCKS': '1'}):
            projects.git(self.path, 'status', '--porcelain=v1', '-z', '--untracked-files=all')
        args, kwargs = run.call_args
        self.assertEqual(kwargs['env']['GIT_OPTIONAL_LOCKS'], '0')
        self.assertNotIn('GIT_DIR', kwargs['env'])
        self.assertIn('core.fsmonitor=false', args[0])
        self.assertIn('--untracked-files=all', args[0])
        self.assertNotIn('shell', kwargs)

    def test_detached_status_has_no_branch_dependency(self):
        with patch.object(projects, 'repo_root', return_value=self.path), patch.object(projects, 'git', return_value=SimpleNamespace(returncode=0, stdout=b'MM x\0', stderr=b'')) as git:
            self.assertEqual(projects.status(self.path), [b'x'])
        self.assertEqual(git.call_args.args[1:], ('status', '--porcelain=v1', '-z', '--untracked-files=all'))

    def test_launch_only_explicit_menu_and_delegates_destination(self):
        self.registered()
        rooms = SimpleNamespace(project_session=Mock())
        self.ui.choose.side_effect = [self.path, None, self.path, 'screen', self.path, 'tab', None]
        with patch.dict(sys.modules, {'zell_rooms': rooms}):
            projects.hub(self.ui)
        self.assertEqual(rooms.project_session.call_count, 2)
        self.assertEqual(rooms.project_session.call_args_list[0].kwargs, {'screen': 'command-center'})
        self.assertEqual(rooms.project_session.call_args_list[1].kwargs, {'screen': None})

    def test_remove_registration_does_not_touch_project(self):
        self.registered(True)
        marker = Path(self.path) / 'keep'
        marker.write_text('keep')
        self.ui.choose.side_effect = [self.path, 'remove', None]
        with patch.object(projects, 'git') as git:
            projects.hub(self.ui)
            git.assert_not_called()
        self.assertEqual(projects.projects(self.ui), [])
        self.assertEqual(marker.read_text(), 'keep')


if __name__ == '__main__':
    unittest.main()
