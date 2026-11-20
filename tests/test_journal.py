from concurrent.futures import ThreadPoolExecutor
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
        journal.recorder(ui, 1)('report', 'off by default')
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

    def test_open_stations_share_history_and_honor_opt_out(self):
        with tempfile.TemporaryDirectory() as root, patch.object(desktop, 'BASE', Path(root)):
            first, second = (journal.recorder(desktop, station) for station in (1, 2))
            first('report', 'before opting in')
            self.assertFalse((desktop.BASE/journal.HISTORY).exists())
            with patch.object(desktop, 'choose', side_effect=['toggle', True, None]):
                journal.conversations(desktop)

            def report(i):
                (first if i % 2 else second)('report', str(i))
            with ThreadPoolExecutor(max_workers=4) as pool:
                list(pool.map(report, range(24)))
            rows = desktop.state_read(journal.HISTORY, [])
            self.assertCountEqual([row['text'] for row in rows], map(str, range(24)))
            with patch.object(desktop, 'choose', side_effect=['toggle', None]):
                journal.conversations(desktop)
            first('report', 'after opting out')
            self.assertEqual(desktop.state_read(journal.HISTORY, []), rows)
            with patch.object(desktop, 'choose', side_effect=['clear', True, None]):
                journal.conversations(desktop)
            second('reply', 'after clearing')
            self.assertEqual(desktop.state_read(journal.HISTORY, []), [])

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
