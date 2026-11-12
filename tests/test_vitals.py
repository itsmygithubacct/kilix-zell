import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import zell_vitals as vitals


class VitalsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.runtime = self.base / '.loop'
        self.runtime.mkdir()
        self.ui = Mock()
        self.ui.state_read.return_value = {}
        self.now = 2000000000
        for context in (patch.object(vitals, 'config_path', return_value=self.base / 'loop.json'),
                        patch.object(vitals, 'APT_HISTORY', self.base / 'apt'),
                        patch.object(vitals, 'UPGRADE_STAMPS', ()),
                        patch.object(vitals.time, 'time', return_value=self.now)):
            context.start()
            self.addCleanup(context.stop)
        vitals._previous_cpu = vitals._smoothed = None

    def cache(self, providers):
        (self.runtime / 'usage-pages.json').write_text(json.dumps({'providers': providers}))

    def entry(self, remaining=60, checked=None, reset=None):
        return {'status': 'ok', 'checked_at': checked or self.now - 100,
                'windows': [{'remaining_percent': remaining, 'resets_at': reset or self.now + 5000}]}

    def test_health_age_boundaries_and_floor(self):
        with patch.object(vitals, 'fatigue', return_value=80):
            self.assertEqual(vitals.snapshot(self.ui)['health'], -1)
            for age, expected in ((0, 100), (86399, 100), (86400, 90), (2 * 86400, 80), (40 * 86400, 10)):
                self.ui.state_read.return_value = {'completed_at': self.now - age}
                value = vitals.snapshot(self.ui)
                self.assertEqual(value['health'], expected)
                self.assertEqual(value['age_days'], age // 86400)

    def test_history_requires_completed_upgrade_not_update(self):
        folder = self.base / 'apt'
        folder.mkdir()
        (folder / 'history.log').write_text('Commandline: apt upgrade\nEnd-Date: 2026-01-01  01:00:00\n\n'
            'Commandline: apt update\nEnd-Date: 2026-02-01  01:00:00\n\n'
            'Commandline: apt upgrade\nError: failure\nEnd-Date: 2026-03-01  01:00:00\n\n'
            'Commandline: apt full-upgrade\nStart-Date: 2026-04-01  01:00:00\n')
        self.assertEqual(vitals.last_upgrade(self.ui), vitals.datetime(2026, 1, 1, 1).timestamp())

    def test_only_an_upgrade_operation_counts(self):
        for line in ('apt upgrade', 'apt -y upgrade', 'apt-get dist-upgrade -y', '/usr/bin/unattended-upgrade',
                     'apt-get -o DPkg::Lock::Timeout=600 -y full-upgrade', 'apt -t trixie-backports upgrade',
                     'aptitude safe-upgrade'):
            self.assertTrue(vitals.is_upgrade(line), line)
        for line in ('apt-get install distro-upgrade-tool', 'apt install -y upgrade-system', 'apt update',
                     'apt-get -o upgrade install vim', 'apt remove unattended-upgrades', 'dpkg -i upgrade.deb', ''):
            self.assertFalse(vitals.is_upgrade(line), line)
        folder = Path(self.temp.name) / 'apt-install'
        folder.mkdir()
        vitals.APT_HISTORY = folder
        (folder / 'history.log').write_text('Commandline: apt-get install distro-upgrade-tool\n'
                                            'End-Date: 2026-05-01  01:00:00\n')
        self.assertIsNone(vitals.last_upgrade(self.ui))

    def test_missing_claudex_reports_without_throttling(self):
        self.cache({})
        with patch.object(vitals, 'claudex_script', return_value=None), patch.object(vitals, 'fatigue', return_value=50), \
                patch.object(vitals.subprocess, 'run') as run:
            vitals.request_mana(self.ui)
        run.assert_not_called()
        self.assertIn('Claudex usage reader was not found', self.ui.page_text.call_args.args[0])
        self.assertFalse((self.runtime / 'zell-mana-attempts.json').exists())

    def test_claudex_found_behind_the_installed_command(self):
        # An installed Zell has no sibling checkout; the command on PATH leads to the reader.
        folder = Path(self.temp.name) / 'claudex-install'
        folder.mkdir()
        (folder / 'pane_loop.py').write_text('')
        (folder / 'claudex').write_text('#!/bin/sh\n')
        (folder / 'claudex').chmod(0o755)
        link = Path(self.temp.name) / 'bin'
        link.mkdir()
        (link / 'claudex').symlink_to(folder / 'claudex')
        with patch.object(vitals, '__file__', str(Path(self.temp.name) / 'a/b/tools/zell_vitals.py')), \
                patch.dict(os.environ, {'PATH': str(link)}):
            self.assertEqual(vitals.claudex_script(), folder / 'pane_loop.py')
            with patch.dict(os.environ, {'PATH': str(folder / 'nowhere')}):
                self.assertIsNone(vitals.claudex_script())

    def test_headroom_and_smoothing(self):
        with patch.object(vitals, 'cpu_memory', side_effect=[((100, 90), 90), ((200, 180), 80), ((300, 180), 90)]):
            self.assertEqual(vitals.fatigue(), -1)
            self.assertEqual(vitals.fatigue(), 80)
            self.assertEqual(vitals.fatigue(), 60)

    def test_missing_reset_and_failed_are_unknown(self):
        failed = self.entry(100)
        failed['status'] = 'error'
        self.cache({'codex': self.entry(60), 'claude': self.entry(100, reset=self.now - 1), 'grok': failed})
        value, age, lines = vitals.mana(self.ui, self.now)
        self.assertEqual((value, age), (60, 100))
        self.assertIn('Missing/unknown providers: claude, grok', lines)

    def test_aggregate_uses_restrictive_window_then_known_mean(self):
        entry = self.entry(90)
        entry['windows'].append({'remaining_percent': 30, 'resets_at': self.now + 10})
        self.cache({'codex': entry, 'grok': self.entry(70)})
        self.assertEqual(vitals.mana(self.ui, self.now)[0], 50)

    def test_refresh_respects_other_tools_cache_and_preserves_providers(self):
        self.cache({'codex': self.entry(), 'claude': self.entry(40, self.now - 3600), 'qwen': self.entry(25)})
        def check(provider):
            cached = vitals.read_json(self.runtime / 'usage-pages.json')['providers']
            cached[provider] = self.entry(75, self.now)
            self.cache(cached)
        with patch.object(vitals, 'check_provider', side_effect=check) as call, patch.object(vitals, 'fatigue', return_value=50):
            vitals.request_mana(self.ui)
            self.assertEqual([c.args[0] for c in call.call_args_list], ['claude', 'grok'])
            vitals.request_mana(self.ui)
            self.assertEqual(call.call_count, 2)
        cached = vitals.read_json(self.runtime / 'usage-pages.json')['providers']
        self.assertEqual(cached['codex']['windows'][0]['remaining_percent'], 60)
        self.assertEqual(cached['qwen']['windows'][0]['remaining_percent'], 25)

    def test_failed_requests_throttled_and_old_values_not_shown(self):
        self.cache({'codex': self.entry(90, self.now - 4000)})
        with patch.object(vitals, 'check_provider', side_effect=OSError('offline')) as call, patch.object(vitals, 'fatigue', return_value=50):
            vitals.request_mana(self.ui)
            vitals.request_mana(self.ui)
            self.assertEqual(call.call_count, 3)
            self.assertEqual(vitals.snapshot(self.ui)['mana'], -1)
        self.assertEqual(vitals.read_json(self.runtime / 'usage-pages.json')['providers']['codex']['windows'][0]['remaining_percent'], 90)

    def test_concurrent_requests_serialize_and_recheck(self):
        entered, release = threading.Event(), threading.Event()
        calls = []
        def check(provider):
            calls.append(provider)
            if len(calls) == 1:
                entered.set()
                self.assertTrue(release.wait(5))
            cached = vitals.read_json(self.runtime / 'usage-pages.json').get('providers', {})
            cached[provider] = self.entry(75, self.now)
            self.cache(cached)
        with patch.object(vitals, 'check_provider', side_effect=check), patch.object(vitals, 'fatigue', return_value=50):
            first = threading.Thread(target=vitals.request_mana, args=(self.ui,))
            second = threading.Thread(target=vitals.request_mana, args=(self.ui,))
            first.start()
            self.assertTrue(entered.wait(5))
            second.start()
            release.set()
            first.join(5)
            second.join(5)
            self.assertFalse(first.is_alive() or second.is_alive())
        self.assertEqual(calls, list(vitals.PROVIDERS))

    def test_snapshot_and_report_never_check_providers(self):
        self.cache({'codex': self.entry(50), 'qwen': self.entry(100)})
        with patch.object(vitals, 'check_provider') as call, patch.object(vitals, 'fatigue', return_value=50):
            self.assertEqual(vitals.snapshot(self.ui)['mana'], 50)
            vitals.report(self.ui)
            call.assert_not_called()
        self.assertNotIn('qwen', self.ui.page_text.call_args.args[0])

    def test_monitor_never_checks_providers_and_emits_wire_order(self):
        with patch.object(vitals, 'check_provider') as call, patch.object(vitals, 'fatigue', return_value=50), patch.object(vitals.time, 'sleep', side_effect=InterruptedError):
            with self.assertRaises(InterruptedError):
                vitals.monitor(self.ui)
            call.assert_not_called()
        self.ui.DIALOGUE.send.assert_called_once_with('V', '-1 50 -1 -1 -1')


if __name__ == '__main__':
    unittest.main()
