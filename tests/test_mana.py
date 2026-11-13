#!/usr/bin/env python3
"""The Mana console: readings, what needs the person, and the automatic refresh."""
import json
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import zell_mana  # noqa: E402
import zell_vitals  # noqa: E402

NOW = 2_000_000.0


def ok(left, checked=NOW - 60):
    return {'status': 'ok', 'checked_at': checked,
            'windows': [{'remaining_percent': left, 'resets_at': NOW + 9000},
                        {'remaining_percent': 90, 'resets_at': NOW + 9000}]}


class ManaTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        root = Path(self.folder.name)
        (root / '.loop').mkdir()
        self.cache = root / '.loop/usage-pages.json'
        self.environment = patch.dict(os.environ, {'CLAUDEX_CONFIG': str(root / 'loop.json')})
        self.environment.start()
        self.state = {}
        self.pages = []
        self.ui = SimpleNamespace(
            state_read=lambda name, default: self.state.get(name, default),
            state_write=lambda name, value: self.state.__setitem__(name, value),
            page_text=self.pages.append)

    def tearDown(self):
        self.environment.stop()
        self.folder.cleanup()

    def write(self, **providers):
        self.cache.write_text(json.dumps({'providers': providers}))

    def test_readings_are_the_tightest_window_unknown_or_waiting(self):
        self.write(codex=ok(19.4), claude={'status': 'attention', 'checked_at': NOW, 'windows': [],
                                           'reason': 'trust question', 'fix': 'tmux attach -t claude_usage'},
                   grok={'status': 'error', 'reason': 'boom', 'checked_at': NOW})
        self.assertEqual(zell_mana.readings(NOW), [19, -2, -1])
        lines = zell_mana.needs(NOW)
        self.assertEqual(len(lines), 2)
        self.assertIn('tmux attach -t claude_usage', lines[0])
        self.assertIn('Read mana now', lines[1])
        self.write(codex=ok(19), claude=ok(52), grok={'status': 'login_required', 'checked_at': NOW})
        self.assertEqual(zell_mana.readings(NOW), [19, 52, -2])
        self.assertIn('grok login', zell_mana.needs(NOW)[0])
        # A window that has already reset is no longer a reading.
        self.write(codex={'status': 'ok', 'checked_at': NOW - 60,
                          'windows': [{'remaining_percent': 5, 'resets_at': NOW - 1}]})
        self.assertEqual(zell_mana.readings(NOW)[0], -1)

    def test_automatic_refresh_is_due_only_for_stale_or_missing_readings(self):
        with patch.object(zell_vitals, 'claudex_script', return_value=Path('/x/pane_loop.py')):
            self.write(codex=ok(19), claude=ok(52), grok=ok(1))
            self.assertFalse(zell_mana.due(self.ui, NOW))
            self.write(codex=ok(19, NOW - zell_mana.MAX_AGE - 1), claude=ok(52), grok=ok(1))
            self.assertTrue(zell_mana.due(self.ui, NOW))
            self.write(codex=ok(19), claude=ok(52))
            self.assertTrue(zell_mana.due(self.ui, NOW))
            # A recent attempt, even a failed one, holds the next back.
            self.state['mana-auto-last.json'] = {'at': NOW - zell_mana.RETRY + 5}
            self.assertFalse(zell_mana.due(self.ui, NOW))
            self.state['mana-auto-last.json'] = {'at': NOW - zell_mana.RETRY - 5}
            self.assertTrue(zell_mana.due(self.ui, NOW))
            self.state[zell_mana.SETTING] = {'enabled': False}
            self.assertFalse(zell_mana.due(self.ui, NOW))
        with patch.object(zell_vitals, 'claudex_script', return_value=None):
            self.state.clear()
            self.assertFalse(zell_mana.due(self.ui, NOW))

    def test_background_refresh_reads_once_with_the_freshness_limit(self):
        calls = []

        def fake(*words, timeout=150):
            calls.append(words)
            return SimpleNamespace(returncode=0, stdout='{}', stderr='')
        self.write(codex=ok(19))
        with patch.object(zell_vitals, 'claudex_script', return_value=Path('/x/pane_loop.py')), \
                patch.object(zell_mana, 'claudex', side_effect=fake):
            self.assertTrue(zell_mana.refresh_in_background(self.ui, NOW))
            deadline = time.time() + 5
            while not calls and time.time() < deadline:
                time.sleep(.01)
            while zell_mana._refresh.locked() and time.time() < deadline:
                time.sleep(.01)
            self.assertEqual(calls, [('usage', 'read', 'codex', 'claude', 'grok', '--max-age',
                                      str(zell_mana.MAX_AGE), '--json')])
            self.assertIn('mana-auto-last.json', self.state)
            self.assertFalse(zell_mana.refresh_in_background(self.ui, time.time()))

    def test_pages_open_through_claudex_in_a_pane_browser(self):
        calls = []

        def fake(*words, timeout=150):
            calls.append(words)
            return SimpleNamespace(returncode=0, stdout='{}', stderr='')
        with patch.object(zell_mana, 'claudex', side_effect=fake):
            for action in range(4):
                zell_mana.hub(self.ui, action)
        self.assertEqual(calls, [('dashboard', 'open', '--pane'), ('dashboard', 'provider', 'codex', '--pane'),
                                 ('dashboard', 'provider', 'claude', '--pane'),
                                 ('dashboard', 'provider', 'grok', '--pane')])
        self.assertIn('codex usage page', self.pages[1])
        with patch.object(zell_mana, 'claudex', return_value=SimpleNamespace(returncode=1, stdout='no kilix', stderr='')):
            zell_mana.hub(self.ui, 2)
        self.assertIn('could not be opened', self.pages[-1])
        self.assertIn('no kilix', self.pages[-1])

    def test_the_switch_and_the_forced_reading(self):
        calls = []
        with patch.object(zell_vitals, 'snapshot', return_value={'details': 'DETAILS'}), \
                patch.object(zell_mana, 'claudex',
                             side_effect=lambda *words, timeout=150: calls.append(words)):
            zell_mana.hub(self.ui, 6)
            self.assertEqual(self.state[zell_mana.SETTING], {'enabled': False})
            self.assertIn('now off', self.pages[-1])
            zell_mana.hub(self.ui, 6)
            self.assertTrue(zell_mana.automatic(self.ui))
            zell_mana.hub(self.ui, 4)
        self.assertEqual(calls[-1][:2] + calls[-1][-3:], ('usage', 'read', '--max-age', '0', '--json'))
        self.assertIn('DETAILS', self.pages[-1])
        with self.assertRaises(ValueError):
            zell_mana.hub(self.ui, 7)


if __name__ == '__main__':
    unittest.main()
