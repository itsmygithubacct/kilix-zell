import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import zell_agents as agents


class AgentControlsTests(unittest.TestCase):
    def test_shared_file_wins_and_each_read_observes_external_changes(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {agents.KEY: 'on'}):
            path = Path(tmp)/'settings.conf'
            with patch.object(agents, 'settings_path', return_value=path):
                self.assertEqual(agents.yolo_state(), 1)
                path.write_text('OTHER=keep\n')
                self.assertEqual(agents.yolo_state(), 0)
                path.write_text(agents.KEY+'=on\n')
                self.assertEqual(agents.yolo_state(), 1)
                path.write_text(agents.KEY+'=on\n'+agents.KEY+'=off\n')
                self.assertEqual(agents.yolo_state(), 0)
                path.unlink(); os.mkfifo(path)
                self.assertEqual(agents.yolo_state(), -1)

    def test_switch_writes_shared_setting_and_reports_verified_value(self):
        ui = SimpleNamespace(DIALOGUE=Mock())
        for current, target in [(0, 'on'), (1, 'off')]:
            with patch.object(agents, 'yolo_state', side_effect=[current, 1-current]), \
                    patch.object(agents.subprocess, 'run', return_value=Mock(returncode=0)) as run:
                agents.toggle_yolo(ui)
            self.assertEqual(run.call_args.args[0], ['kilix', 'settings', '--set', 'coding_yolo='+target])
            ui.DIALOGUE.send.assert_called_with('Y', str(1-current))

    def test_failed_or_unreadable_write_never_claims_a_changed_switch(self):
        ui = SimpleNamespace(DIALOGUE=Mock())
        with patch.object(agents, 'yolo_state', return_value=-1), patch.object(agents.subprocess, 'run') as run:
            with self.assertRaises(ValueError): agents.toggle_yolo(ui)
            run.assert_not_called()
        with patch.object(agents, 'yolo_state', return_value=0), \
                patch.object(agents.subprocess, 'run', return_value=Mock(returncode=1, stderr='write failed')):
            with self.assertRaises(RuntimeError): agents.toggle_yolo(ui)
        with patch.object(agents, 'yolo_state', return_value=0), \
                patch.object(agents.subprocess, 'run', return_value=Mock(returncode=0)):
            with self.assertRaises(RuntimeError): agents.toggle_yolo(ui)
        ui.DIALOGUE.send.assert_not_called()

    def test_update_selection_is_saved_and_only_selected_clients_run(self):
        rows = [dict(id=k, label=k.title(), kind='agent', installed=True) for k in ('codex','grok')]
        ui = SimpleNamespace(DIALOGUE=Mock(), state_read=Mock(return_value={}), state_write=Mock(),
                             choose=Mock(side_effect=['none','agent:grok','run']), page_text=Mock())
        with patch.object(agents, 'catalog', return_value=rows), patch('zell_terminal.terminal_loop') as terminal:
            agents.updates(ui)
        terminal.assert_called_once()
        self.assertEqual(terminal.call_args.args[1][2:], ['--run-updates','grok'])
        self.assertTrue(terminal.call_args.kwargs['sensitive'])
        ui.state_write.assert_called_with(agents.STATE, {'update_agents':['grok']})
        ui.choose.side_effect=['all','run']
        with patch.object(agents, 'catalog', return_value=rows), patch('zell_terminal.terminal_loop') as terminal:
            agents.updates(ui)
        self.assertEqual(terminal.call_args.args[1][2:], ['--run-updates','codex','grok'])

    def test_cancel_and_empty_selection_never_run_an_updater(self):
        ui = SimpleNamespace(DIALOGUE=Mock(), state_read=Mock(return_value={'update_agents':[]}),
                             state_write=Mock(), choose=Mock(side_effect=['run',None]), page_text=Mock())
        with patch.object(agents, 'catalog', return_value=[]), patch('zell_terminal.terminal_loop') as terminal:
            agents.updates(ui)
        terminal.assert_not_called()
        ui.page_text.assert_called_once()

    def test_removed_client_is_not_reintroduced_from_saved_selection(self):
        ui = SimpleNamespace(DIALOGUE=Mock(), state_read=Mock(return_value={'update_agents':['grok','codex']}),
                             state_write=Mock(), choose=Mock(return_value='run'), page_text=Mock())
        with patch.object(agents, 'catalog', return_value=[dict(id='codex',label='Codex',installed=True)]), \
                patch('zell_terminal.terminal_loop') as terminal:
            agents.updates(ui)
        self.assertEqual(terminal.call_args.args[1][2:], ['--run-updates','codex'])

    def test_final_update_report_waits_for_acknowledgement(self):
        with patch.object(agents.subprocess, 'run', return_value=Mock(returncode=7)) as run, \
                patch('builtins.input', return_value='') as pause:
            self.assertEqual(agents.run_updates(['codex','grok']),7)
        run.assert_called_once_with(['kilix','install','--update','codex','grok'],check=False)
        self.assertIn('exit 7',pause.call_args.args[0])

    def test_switch_errors_remain_visible_until_acknowledged(self):
        ui=SimpleNamespace(page_text=Mock())
        with patch.object(agents, 'toggle_yolo', side_effect=ValueError('cannot read settings')):
            agents.switch(ui)
        ui.page_text.assert_called_once_with('YOLO setting: cannot read settings')


if __name__ == '__main__': unittest.main()
