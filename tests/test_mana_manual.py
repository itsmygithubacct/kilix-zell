#!/usr/bin/env python3
"""Mana set by hand: the scripts, the stored values, and where they show."""
import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import zell_mana  # noqa: E402
import zell_mana_manual as manual  # noqa: E402
import zell_vitals  # noqa: E402

NOW = 2_000_000.0
# The three providers, in the Mana console's order.
P1, P2, P3 = manual.PROVIDERS
SCRIPTS = ROOT / 'tools/mana'


def ok(left, checked):
    return {'status': 'ok', 'checked_at': checked,
            'windows': [{'remaining_percent': left, 'resets_at': NOW + 9000}]}


class StoreTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.state = Path(self.folder.name) / 'state'

    def tearDown(self):
        self.folder.cleanup()

    def test_nothing_is_set_at_first(self):
        self.assertEqual(manual.load(self.state), {})
        self.assertEqual(manual.load(None), {})

    def test_values_are_kept_with_the_time_they_were_set(self):
        manual.set_values(self.state, {P2: 62, P1: 40}, NOW)
        manual.set_values(self.state, {P2: 7}, NOW + 5)
        self.assertEqual(manual.load(self.state), {P2: (7, NOW + 5), P1: (40, NOW)})
        self.assertEqual(stat.S_IMODE(self.state.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE((self.state / manual.NAME).stat().st_mode), 0o600)
        self.assertEqual([path.name for path in self.state.iterdir()], [manual.NAME])

    def test_the_ends_of_the_range_are_values(self):
        manual.set_values(self.state, {P1: 0, P3: 100}, NOW)
        self.assertEqual(manual.load(self.state), {P1: (0, NOW), P3: (100, NOW)})

    def test_a_bad_value_changes_nothing(self):
        manual.set_values(self.state, {P2: 62}, NOW)
        before = (self.state / manual.NAME).read_bytes()
        for wanted in ({P2: 101}, {P2: -1}, {P2: 50.0}, {P2: True},
                       {P2: '50'}, {'qwen': 50}, {P1: 10, P3: 200}, {}):
            with self.assertRaises(ValueError, msg=wanted):
                manual.set_values(self.state, wanted, NOW + 9)
            self.assertEqual((self.state / manual.NAME).read_bytes(), before, wanted)

    def test_clearing_one_or_all(self):
        manual.set_values(self.state, {P2: 62, P1: 40, P3: 9}, NOW)
        self.assertEqual(manual.clear(self.state, [P2]), [P2])
        self.assertEqual(sorted(manual.load(self.state)), [P1, P3])
        self.assertEqual(manual.clear(self.state, [P2]), [])
        with self.assertRaises(ValueError):
            manual.clear(self.state, ['qwen'])
        self.assertEqual(sorted(manual.load(self.state)), [P1, P3])
        self.assertEqual(manual.clear(self.state), [P1, P3])
        self.assertEqual(manual.load(self.state), {})

    def test_clearing_nothing_makes_no_file(self):
        self.assertEqual(manual.clear(self.state), [])
        self.assertFalse(self.state.exists())

    def test_a_damaged_file_reads_as_nothing_set(self):
        self.state.mkdir()
        good = {'percent': 40, 'at': NOW}
        for text in ('', 'not json', '[]', '{"providers": []}',
                     json.dumps({'providers': {P2: {'percent': 140, 'at': NOW}}}),
                     json.dumps({'providers': {P2: {'percent': 40.5, 'at': NOW}}}),
                     json.dumps({'providers': {P2: {'percent': True, 'at': NOW}}}),
                     json.dumps({'providers': {P2: {'percent': 40, 'at': 'noon'}}}),
                     json.dumps({'providers': {P2: {'percent': 40, 'at': -1}}}),
                     json.dumps({'providers': {P2: {'percent': 40}}}),
                     json.dumps({'providers': {'qwen': good}})):
            (self.state / manual.NAME).write_text(text)
            self.assertEqual(manual.load(self.state), {}, text)
        # One bad entry does not hide a good one.
        (self.state / manual.NAME).write_text(json.dumps({'providers': {P2: {'percent': 140, 'at': NOW}, P1: good}}))
        self.assertEqual(manual.load(self.state), {P1: (40, NOW)})

    def test_what_a_person_may_type(self):
        self.assertEqual([manual.percent(text) for text in ('0', '100', '62', ' 62% ', '62 %', '61.6', '0.4')],
                         [0, 100, 62, 62, 62, 62, 0])
        for text in ('', '%', '-1', '100.5', '101', 'half', 'nan', 'inf', '6 2', '62 left'):
            with self.assertRaises(ValueError, msg=text):
                manual.percent(text)
        self.assertEqual(manual.pairs([P2.capitalize(), '62', P1, '40%']), {P2: 62, P1: 40})
        self.assertEqual(manual.pairs(['all', '50']), {P1: 50, P2: 50, P3: 50})
        self.assertEqual(manual.pairs(['all', '50', P3, '9']), {P1: 50, P2: 50, P3: 9})
        for words in ([], [P2], [P2, '62', P1], ['qwen', '5'], ['62', P2], [P2, 'lots']):
            with self.assertRaises(ValueError, msg=words):
                manual.pairs(words)

    def test_which_value_stands(self):
        hand = (62, NOW)
        self.assertFalse(manual.stands(None, 19, NOW))
        # No automatic reading, or one that is not a number: the hand value stands.
        for automatic in (None, -1, -2):
            self.assertTrue(manual.stands(hand, automatic, NOW + 50), automatic)
            self.assertTrue(manual.stands(hand, automatic, None), automatic)
        # A good automatic reading replaces it only when it was taken later.
        self.assertTrue(manual.stands(hand, 19, NOW - 1))
        self.assertTrue(manual.stands(hand, 19, NOW))
        self.assertTrue(manual.stands(hand, 19, None))
        self.assertFalse(manual.stands(hand, 19, NOW + 1))
        self.assertFalse(manual.stands(hand, 0, NOW + 1))

    def test_the_state_folder_is_the_room_interface_s(self):
        import desktop
        home = Path(self.folder.name)
        for env in ({'KILIX_ZELL_STATE': str(home / 'a')}, {'GPU_TERMINAL_HOME': str(home / 'b')},
                    {'KILIX_ZELL_STATE': str(home / 'a'), 'GPU_TERMINAL_HOME': str(home / 'b')}, {}):
            clean = {k: v for k, v in os.environ.items() if k not in ('KILIX_ZELL_STATE', 'GPU_TERMINAL_HOME')}
            with patch.dict(os.environ, {**clean, **env}, clear=True):
                expected = Path(os.environ.get('KILIX_ZELL_STATE', Path(os.environ.get(
                    'GPU_TERMINAL_HOME', Path.home() / '.local/gpu_terminal')) / 'kilix-zell/state'))
                self.assertEqual(manual.state_dir(), expected, env)
        self.assertIs(manual.PROVIDERS, zell_vitals.PROVIDERS)
        self.assertEqual(len(set(manual.PROVIDERS)), 3)
        self.assertEqual(desktop.MANA_SCRIPTS, SCRIPTS)


class ShownTests(unittest.TestCase):
    """The Mana console's three numbers and the MANA bar."""

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        root = Path(self.folder.name)
        (root / '.loop').mkdir()
        self.cache = root / '.loop/usage-pages.json'
        self.state = root / 'state'
        self.environment = patch.dict(os.environ, {'CLAUDEX_CONFIG': str(root / 'loop.json')})
        self.environment.start()
        self.ui = SimpleNamespace(BASE=self.state, state_read=lambda name, default: default)

    def tearDown(self):
        self.environment.stop()
        self.folder.cleanup()

    def write(self, **providers):
        self.cache.write_text(json.dumps({'providers': providers}))

    def test_without_a_state_folder_nothing_changes(self):
        self.write(**{P1: ok(19, NOW - 60)})
        manual.set_values(self.state, {P1: 77, P2: 62}, NOW)
        self.assertEqual(zell_mana.readings(NOW), [19, -1, -1])

    def test_a_hand_value_fills_a_missing_reading(self):
        self.write(**{P1: ok(19, NOW - 60),
                      P2: {'status': 'input_required', 'checked_at': NOW - 30, 'windows': [], 'reason': 'needs website percentages'},
                      P3: {'status': 'login_required', 'checked_at': NOW - 30, 'windows': []}})
        self.assertEqual(zell_mana.readings(NOW, self.state), [19, -1, -2])
        self.assertEqual(len(zell_mana.needs(NOW, self.state)), 2)
        self.assertIn(f'set-mana {P2}', zell_mana.needs(NOW, self.state)[0])
        manual.set_values(self.state, {P2: 62, P3: 5}, NOW - 10)
        self.assertEqual(zell_mana.readings(NOW, self.state), [19, 62, 5])
        self.assertEqual(zell_mana.needs(NOW, self.state), [])
        # Still asked for when nobody passes the folder.
        self.assertEqual(len(zell_mana.needs(NOW)), 2)

    def test_a_newer_automatic_reading_replaces_it_and_an_older_one_does_not(self):
        manual.set_values(self.state, {P1: 77}, NOW - 100)
        self.write(**{P1: ok(19, NOW - 200)})
        self.assertEqual(zell_mana.readings(NOW, self.state)[0], 77)
        self.write(**{P1: ok(19, NOW - 50)})
        self.assertEqual(zell_mana.readings(NOW, self.state)[0], 19)
        # Set again after that reading: the hand value is back.
        manual.set_values(self.state, {P1: 33}, NOW - 5)
        self.assertEqual(zell_mana.readings(NOW, self.state)[0], 33)
        manual.clear(self.state, [P1])
        self.assertEqual(zell_mana.readings(NOW, self.state)[0], 19)

    def test_a_failed_later_reading_does_not_replace_it(self):
        manual.set_values(self.state, {P2: 62}, NOW - 100)
        self.write(**{P2: {'status': 'input_required', 'checked_at': NOW - 5, 'windows': []}})
        self.assertEqual(zell_mana.readings(NOW, self.state)[1], 62)

    def test_the_mana_bar_uses_it(self):
        self.write(**{P1: ok(20, NOW - 60)})
        value, age, lines = zell_vitals.mana(self.ui, NOW)
        self.assertEqual((value, age), (20, 60))
        manual.set_values(self.state, {P2: 60, P3: 100}, NOW - 300)
        value, age, lines = zell_vitals.mana(self.ui, NOW)
        self.assertEqual(value, 60)        # mean of 20, 60 and 100
        self.assertEqual(age, 300)         # the oldest number shown
        self.assertIn(f'{P2}: 60% remaining (set by hand)', lines[1])
        self.assertIn(f'{P3}: 100% remaining (set by hand)', lines[2])
        self.assertIn('Missing/unknown providers: none', lines)
        # A newer automatic reading wins here too.
        manual.set_values(self.state, {P1: 90}, NOW - 600)
        self.assertEqual(zell_vitals.mana(self.ui, NOW)[0], 60)
        # And an interface with no state folder shows only automatic readings.
        self.assertEqual(zell_vitals.mana(SimpleNamespace(), NOW)[0], 20)


class ScriptTests(unittest.TestCase):
    """The three scripts in the Command computer's folder, run as a person would."""

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.state = Path(self.folder.name) / 'state'
        self.env = {**os.environ, 'KILIX_ZELL_STATE': str(self.state), 'PYTHONDONTWRITEBYTECODE': '1'}

    def tearDown(self):
        self.folder.cleanup()

    def run_script(self, name, *words, stdin=''):
        return subprocess.run([str(SCRIPTS / name), *words], input=stdin, capture_output=True, text=True,
                              env=self.env, cwd=SCRIPTS, timeout=60)

    def test_the_folder_holds_the_scripts_and_their_notes(self):
        self.assertEqual(sorted(path.name for path in SCRIPTS.iterdir() if path.name != '__pycache__'),
                         ['README', 'clear-mana', 'set-mana', 'show-mana'])
        for name in ('set-mana', 'show-mana', 'clear-mana'):
            self.assertTrue(os.access(SCRIPTS / name, os.X_OK), name)
            self.assertIn(name, (SCRIPTS / 'README').read_text())
            done = self.run_script(name, '--help')
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertIn(name, done.stdout)
        self.assertFalse(self.state.exists())

    def test_set_show_clear(self):
        done = self.run_script('set-mana', P2, '62', P1, '40%')
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn(f'{P2}: 62% left', done.stdout)
        self.assertEqual({k: v[0] for k, v in manual.load(self.state).items()}, {P2: 62, P1: 40})
        shown = self.run_script('show-mana')
        self.assertEqual(shown.returncode, 0, shown.stderr)
        self.assertIn(f'{P1}: 40% left, set by hand', shown.stdout)
        self.assertIn(f'{P2}: 62% left, set by hand', shown.stdout)
        self.assertIn(f'{P3}: not set by hand', shown.stdout)
        done = self.run_script('set-mana', 'all', '5')
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual({k: v[0] for k, v in manual.load(self.state).items()}, {P2: 5, P1: 5, P3: 5})
        done = self.run_script('clear-mana', P2)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(sorted(manual.load(self.state)), [P1, P3])
        done = self.run_script('clear-mana')
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(manual.load(self.state), {})
        self.assertIn('not set by hand', self.run_script('show-mana').stdout)

    def test_a_refused_request_says_so_and_changes_nothing(self):
        self.run_script('set-mana', P2, '62')
        before = (self.state / manual.NAME).read_bytes()
        for script, words in (('set-mana', (P2, '150')), ('set-mana', ('qwen', '5')), ('set-mana', (P2,)),
                              ('set-mana', (P1, '10', P3, 'lots')), ('clear-mana', ('qwen',)),
                              ('show-mana', (P2,))):
            done = self.run_script(script, *words)
            self.assertEqual(done.returncode, 2, (script, words, done.stdout, done.stderr))
            self.assertEqual((self.state / manual.NAME).read_bytes(), before, (script, words))
        # With no words and no terminal there is nobody to ask.
        done = self.run_script('set-mana')
        self.assertEqual(done.returncode, 2, done.stdout + done.stderr)
        self.assertIn('Nothing was changed', done.stderr)
        self.assertEqual((self.state / manual.NAME).read_bytes(), before)

    def test_being_asked_on_a_terminal(self):
        import pty
        master, slave = pty.openpty()
        try:
            process = subprocess.Popen([str(SCRIPTS / 'set-mana')], stdin=slave, stdout=slave, stderr=slave,
                                       env=self.env, cwd=SCRIPTS, close_fds=True)
            os.close(slave)
            slave = -1
            # The first is left as it is; the second gets a wrong answer, then 48; the third 3.
            os.write(master, b'\n200\n48\n3%\n')
            self.assertEqual(process.wait(timeout=60), 0)
        finally:
            os.close(master)
            if slave >= 0:
                os.close(slave)
        self.assertEqual({k: v[0] for k, v in manual.load(self.state).items()}, {P2: 48, P3: 3})

    def test_the_computer_s_shell_starts_among_the_scripts(self):
        import desktop
        command = desktop.command_computer('/bin/sh')
        self.assertEqual(command[:2], ['/bin/sh', '-c'])
        self.assertEqual(command[-3:], [str(SCRIPTS), str(desktop.BASE), '/bin/sh'])
        # A shell that is not there falls back to /bin/sh.
        self.assertEqual(desktop.command_computer('/no/such/shell')[-1], '/bin/sh')
        probe = [*command[:-2], str(self.state), '/bin/sh']
        # A home of its own: a shell that lost its state folder writes there,
        # not into the real one.
        apart = {k: v for k, v in os.environ.items() if k not in ('KILIX_ZELL_STATE', 'GPU_TERMINAL_HOME')}
        apart.update(HOME=self.folder.name, PYTHONDONTWRITEBYTECODE='1')
        done = subprocess.run(probe, input='pwd\necho "state=$KILIX_ZELL_STATE"\ncommand -v set-mana\n'
                              f'set-mana {P3} 12\nshow-mana\n', capture_output=True, text=True, env=apart, timeout=60)
        self.assertEqual(done.returncode, 0, done.stderr)
        lines = done.stdout.splitlines()
        self.assertIn('set-mana, show-mana, clear-mana', lines[0])
        self.assertIn(str(SCRIPTS), lines)
        self.assertIn(f'state={self.state}', lines)
        self.assertIn(str(SCRIPTS / 'set-mana'), lines)
        self.assertIn(f'{P3}: 12% left, set by hand.', lines)
        self.assertEqual({k: v[0] for k, v in manual.load(self.state).items()}, {P3: 12})
        # A missing folder stops it before any shell is started.
        gone = [*command[:4], str(self.state / 'none'), str(self.state), '/bin/sh']
        done = subprocess.run(gone, input='echo started\n', capture_output=True, text=True, env=apart, timeout=60)
        self.assertEqual(done.returncode, 1)
        self.assertNotIn('started', done.stdout)

    def test_the_station_opens_that_shell_in_the_floating_terminal(self):
        import desktop
        import zell_terminal
        opened = []
        with patch.object(zell_terminal, 'terminal_loop',
                          lambda ui, command, title=None, sensitive=False: opened.append((command, title, sensitive)) or 0):
            self.assertEqual(desktop.desktop(56, 0), 0)
        self.assertEqual(opened, [(desktop.command_computer(), 'Command computer', False)])
        self.assertEqual(opened[0][0][4], str(SCRIPTS))

    def test_the_monitor_sends_hand_values_to_the_console(self):
        # The room's monitor passes its own state folder to the readings.
        source = (ROOT / 'tools/desktop.py').read_text()
        self.assertIn('zell_mana.readings(directory=BASE)', source)
        self.assertNotIn('zell_mana.readings()', source)


if __name__ == '__main__':
    unittest.main()
