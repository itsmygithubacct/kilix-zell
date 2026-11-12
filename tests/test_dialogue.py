import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import desktop
from zell_dialogue import Dialogue

class DialogueTests(unittest.TestCase):
    def test_paged_choices_and_filter_preserve_values(self):
        # Index 32 is Next; index 2 on page two selects original item 34.
        writer=io.StringIO()
        d=Dialogue(io.StringIO('32\n2\n'),writer)
        self.assertEqual(d.choose('Files',[(f'File {i}',i) for i in range(40)]),34)
        self.assertTrue(all(len(line)<8192 for line in writer.getvalue().splitlines()))
        d=Dialogue(io.StringIO('32\nFile 39\n0\n'),io.StringIO())
        # 40 entries: Filter follows Next at index 33, not 32.
        d.reader=io.StringIO('33\nFile 39\n0\n')
        self.assertEqual(d.choose('Files',[(f'File {i}',i) for i in range(40)]),39)

    def test_notices_remain_visible_before_next_menu(self):
        writer=io.StringIO();d=Dialogue(io.StringIO('\n0\n'),writer)
        d.write('The refresh failed; cached uploads are still available.')
        self.assertEqual(d.choose('Uploads',[('Cached item',7)]),7)
        frames=writer.getvalue().splitlines()
        self.assertLess(frames.index('R'),next(i for i,f in enumerate(frames) if f.startswith('M')))

    def test_escape_and_eof(self):
        self.assertIsNone(Dialogue(io.StringIO('\x1b\n'),io.StringIO()).choose('Menu',[('one',1)]))
        with self.assertRaises(KeyboardInterrupt):Dialogue(io.StringIO('\x1b\n'),io.StringIO()).input('Name')
        with self.assertRaises(EOFError):Dialogue(io.StringIO(''),io.StringIO()).page('Report')

    def test_long_question_is_paged_in_full_before_its_menu(self):
        question='Launch this arrangement?\nProject: /srv/project\nScreen 2: Worker 1\nScreen 1: Coordinator'
        writer=io.StringIO();d=Dialogue(io.StringIO('\n0\n'),writer)
        self.assertIs(d.choose(question,[('Launch',True),('Cancel',False)]),True)
        frames=writer.getvalue().splitlines()
        paged=''.join(bytes.fromhex(f[1:]).decode() for f in frames[:frames.index('R')] if f.startswith('O'))
        self.assertEqual(paged,question)
        title=bytes.fromhex(next(f for f in frames if f.startswith('M'))[1:]).decode()
        self.assertEqual(title,'Launch this arrangement? / 2 results')
        # Esc on the page cancels: the menu is never offered.
        writer=io.StringIO()
        self.assertIsNone(Dialogue(io.StringIO('\x1b\n'),writer).choose(question,[('Launch',True)]))
        self.assertFalse(any(f.startswith('M') for f in writer.getvalue().splitlines()))
        # One long line is paged too, and its menu title is shortened.
        writer=io.StringIO();d=Dialogue(io.StringIO('\n0\n'),writer)
        self.assertEqual(d.choose('x'*90,[('one',1)]),1)
        frames=writer.getvalue().splitlines()
        self.assertIn('R',frames)
        self.assertEqual(bytes.fromhex(next(f for f in frames if f.startswith('M'))[1:]).decode(),'x'*37+'... / 1 results')

    def test_real_helper_keeps_report_and_input_on_protocol(self):
        with tempfile.TemporaryDirectory() as root:
            env=dict(os.environ,KILIX_ZELL_STATE=root)
            command=[sys.executable,str(Path(desktop.__file__))]
            result=subprocess.run(command+['11','0','--dialogue'],input='\n',text=True,capture_output=True,env=env,timeout=5)
            self.assertEqual(result.returncode,0,result.stderr)
            frames=result.stdout.splitlines()
            self.assertIn('R',frames)
            text=''.join(bytes.fromhex(f[1:]).decode() for f in frames if f.startswith('O'))
            self.assertIn('202',text)
            result=subprocess.run(command+['9','1','--dialogue'],input='Remember the books\n',text=True,capture_output=True,env=env,timeout=5)
            self.assertEqual(result.returncode,0)
            self.assertTrue(result.stdout.startswith('P'))
            self.assertIn('Remember the books',Path(root,'notebook.txt').read_text())
            result=subprocess.run(command+['4','2','--dialogue'],input='\x1b\n',text=True,capture_output=True,env=env,timeout=5)
            self.assertEqual(result.returncode,0)
            self.assertIn('Q',result.stdout.splitlines())

    def test_tabs_use_argument_lists_and_reports_stay_inside(self):
        dialogue=Dialogue(io.StringIO('\n'),io.StringIO())
        with patch.object(desktop,'DIALOGUE',dialogue),patch.object(desktop,'open_tab',return_value=0) as tab:
            self.assertEqual(desktop.run(['top']),0)
            tab.assert_called_once_with(['top'])
            tab.reset_mock()
            self.assertEqual(desktop.run([sys.executable,'-c','print("system report")']),0)
            tab.assert_not_called()
        output=dialogue.writer.getvalue()
        self.assertIn('73797374656d207265706f7274',output)

    def test_neuronte_switch_launches_real_renderer_in_tab(self):
        with tempfile.TemporaryDirectory() as root:
            script=Path(root)/'neuronte_display.py';script.write_text('')
            (Path(root)/'build').mkdir();(Path(root)/'build/libneuronte-display.so').touch()
            with patch.dict(os.environ,{'KILIX_ZELL_NEURONTE_DISPLAY':str(script),'SHELL':'/bin/sh'}),patch.object(desktop,'open_tab',return_value=0) as tab:
                desktop.desktop(12,0)
                self.assertEqual(tab.call_args.args[0],[sys.executable,str(script),'--','/bin/sh','-i'])
                self.assertEqual(tab.call_args.kwargs['title'],'Zell / Neuronte shell')
            with patch.dict(os.environ,{'KILIX_ZELL_NEURONTE_DISPLAY':str(Path(root)/'missing')}),patch.object(desktop,'open_tab') as tab:
                desktop.desktop(12,0);tab.assert_not_called()

    def test_combined_git_and_os_health_floors(self):
        base={'health':90,'fatigue':100,'mana':-1,'age_days':1,'mana_age':-1,'details':'OS'}
        with patch.object(desktop.zell_vitals,'snapshot',side_effect=lambda ui:dict(base)),patch.object(desktop.zell_projects,'dirty_summary',return_value={'count':12,'details':'project changes','known':True}) as dirty:
            self.assertEqual(desktop.vitals_snapshot()['health'],78)
            dirty.return_value={'count':200,'details':'changes','known':True}
            self.assertEqual(desktop.vitals_snapshot()['health'],1)
            dirty.return_value={'count':0,'details':'not watched','known':True}
            self.assertEqual(desktop.vitals_snapshot()['health'],90)
            base['health']=-1
            self.assertEqual(desktop.vitals_snapshot()['health'],-1)

    def test_failed_tab_launch_reports_failure(self):
        result=subprocess.CompletedProcess([],1,stdout='',stderr='caller unavailable')
        with patch.object(desktop.subprocess,'run',return_value=result) as run,patch('builtins.print') as output:
            self.assertEqual(desktop.open_tab(['less','--','a; echo surprise']),1)
            argv=run.call_args.args[0]
            self.assertEqual(argv[:2],['kilix','new-tab'])
            self.assertEqual(__import__('json').loads(argv[-1]),['less','--','a; echo surprise'])
            self.assertIn('caller unavailable',output.call_args.args[0])

if __name__=='__main__':unittest.main()
