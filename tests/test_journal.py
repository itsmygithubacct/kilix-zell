import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import desktop
import zell_journal as journal


class JournalTests(unittest.TestCase):
    def test_opt_in_and_vault_exclusion(self):
        ui = SimpleNamespace(state_read=Mock(return_value={}), state_write=Mock())
        self.assertIsNone(journal.recorder(ui, 1))
        ui.state_read.return_value = {'enabled': True}
        for station in (-1, 9, 31):
            self.assertIsNone(journal.recorder(ui, station))
        ui.state_write.assert_not_called()

    def test_history_is_private_bounded_and_can_be_cleared(self):
        with tempfile.TemporaryDirectory() as root, patch.object(desktop, 'BASE', Path(root)):
            desktop.state_write(journal.SETTINGS, {'enabled': True})
            record = journal.recorder(desktop, 1)
            for i in range(110):
                record('report', str(i))
            rows = desktop.state_read(journal.HISTORY, [])
            self.assertEqual(len(rows), 100)
            self.assertEqual(rows[0]['text'], '10')
            self.assertEqual((Path(root)/journal.HISTORY).stat().st_mode & 0o777, 0o600)
            wizard = Path(root)/'wizard-conversations.jsonl'; wizard.write_text('{}\n')
            with patch.object(desktop, 'choose', side_effect=['clear', True, None]):
                journal.conversations(desktop)
            self.assertEqual(desktop.state_read(journal.HISTORY, []), [])
            self.assertFalse(wizard.exists())

    def test_tasks_persist_add_complete_and_remove(self):
        with tempfile.TemporaryDirectory() as root, patch.object(desktop, 'BASE', Path(root)), \
             patch.object(desktop, 'choose', side_effect=['add', None]), patch('builtins.input', return_value='Finish artwork'):
            journal.tasks(desktop)
            row = desktop.state_read(journal.TASKS, [])[0]
            self.assertFalse(row['done'])
            with patch.object(desktop, 'choose', side_effect=[row['id'], 'toggle', None]):
                journal.tasks(desktop)
            self.assertTrue(desktop.state_read(journal.TASKS, [])[0]['done'])
            with patch.object(desktop, 'choose', side_effect=[row['id'], 'remove', None]):
                journal.tasks(desktop)
            self.assertEqual(desktop.state_read(journal.TASKS, []), [])


if __name__ == '__main__':
    unittest.main()
