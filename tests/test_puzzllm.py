import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import desktop
import zell_puzzllm as puzzle


class PuzzLLMHatch(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.repo = self.base/'upstream'
        self.sources = self.base/'sources'
        env = patch.dict(os.environ, {'HOME':str(self.base), 'XDG_CACHE_HOME':str(self.base/'cache'),
                                      'GPU_TERMINAL_SOURCE_HOME':str(self.sources), 'PUZZLLM_ROOT':'',
                                      'PUZZLLM_STATE':''})
        env.start()
        self.addCleanup(env.stop)
        remote = patch.object(puzzle, 'REPOSITORY', self.repo.as_uri())
        remote.start()
        self.addCleanup(remote.stop)
        self.git('init','-q','-b',puzzle.BRANCH,str(self.repo),cwd=self.base)
        self.git('config','user.name','Test')
        self.git('config','user.email','test@example.invalid')
        game = self.repo/'puzzllm'
        (game/'bin').mkdir(parents=True)
        launcher = game/'bin/puzzllm'
        launcher.write_text('#!/bin/sh\ncat "$(dirname "$0")/../built"\n')
        launcher.chmod(0o755)
        (game/'Makefile').write_text('all: edition\n\tcp edition built\n')
        self.publish('one')

    def git(self, *args, cwd=None):
        return subprocess.check_output(['git',*args],cwd=cwd or self.repo,stderr=subprocess.PIPE,text=True).strip()

    def publish(self, edition):
        (self.repo/'puzzllm/edition').write_text(edition)
        self.git('add','puzzllm')
        self.git('commit','-qm',edition)

    def test_published_updates_and_offline_launch_keep_each_games_files(self):
        stale = self.sources/'games/kilix-games'
        stale.mkdir(parents=True)
        (stale/'.git').write_text('gitdir: missing\n')
        first = puzzle.launcher()
        self.publish('two')
        second = puzzle.launcher()
        self.assertNotEqual(first, second)
        self.assertEqual(subprocess.check_output([first],text=True), 'one')
        self.assertEqual(subprocess.check_output([second],text=True), 'two')
        with patch.object(puzzle,'published_revision',side_effect=RuntimeError('offline')):
            self.assertEqual(puzzle.launcher(), second)

    def test_active_worktree_and_explicit_source(self):
        repo = self.sources/'games/kilix-games'
        repo.parent.mkdir(parents=True)
        self.git('clone','-q',str(self.repo),str(repo))
        self.git('switch','--detach',cwd=repo)
        tree = self.base/'current puzzles'
        self.git('worktree','add','-q',str(tree),puzzle.BRANCH,cwd=repo)
        game = tree/'puzzllm'
        (game/'edition').write_text('in progress')
        self.assertEqual(puzzle.launcher(),game/'bin/puzzllm')
        self.assertEqual((game/'built').read_text(),'in progress')
        self.publish('updated upstream')
        self.assertNotEqual(puzzle.launcher(),game/'bin/puzzllm')
        self.assertEqual((game/'edition').read_text(),'in progress')
        with patch.dict(os.environ,{'PUZZLLM_ROOT':str(game)}), patch.object(puzzle,'published_revision') as lookup:
            self.assertEqual(puzzle.launcher(),game/'bin/puzzllm')
            lookup.assert_not_called()

    def test_station_opens_one_tab_and_reports_launch_failure(self):
        launcher = self.base/'with spaces/bin/puzzllm'
        with patch.object(puzzle,'launcher',return_value=launcher),patch.object(desktop,'open_tab',return_value=0) as tab:
            self.assertEqual(desktop.desktop(57,0),0)
            tab.assert_called_once_with([str(launcher)],title='PuzzLLM')
            state = self.base/'game progress'
            with patch.dict(os.environ,{'PUZZLLM_STATE':str(state)}):
                desktop.desktop(57,0)
            tab.assert_called_with(['env','PUZZLLM_STATE='+str(state),str(launcher)],title='PuzzLLM')
            tab.return_value=1
            with self.assertRaises(SystemExit) as failure:
                desktop.desktop(57,0)
            self.assertEqual(failure.exception.code,1)


if __name__=='__main__':
    unittest.main()
