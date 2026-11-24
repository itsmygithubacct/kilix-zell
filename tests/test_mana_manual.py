#!/usr/bin/env python3
"""Mana set by hand: the scripts, the stored values, and where they show."""
import fcntl
import json
import os
import stat
import subprocess
import sys
import tempfile
import time
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


def good(left, checked):
    """A good reading taken at any time, with a window that resets after it."""
    return {'status': 'ok', 'checked_at': checked, 'windows': [{'remaining_percent': left, 'resets_at': checked + 9000}]}


AHEAD = 10 * 86400


class StoreTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.state = Path(self.folder.name) / 'state'
        # Setting a value looks at Claudex's readings. Here there are none,
        # whatever the machine running the test has.
        self.environment = patch.dict(os.environ, {'CLAUDEX_CONFIG': str(Path(self.folder.name) / 'loop.json')})
        self.environment.start()

    def tearDown(self):
        self.environment.stop()
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
        # The values and the lock that keeps two writers apart, and no leftovers.
        self.assertEqual(sorted(path.name for path in self.state.iterdir()), [manual.NAME, manual.LOCK])
        self.assertEqual(stat.S_IMODE((self.state / manual.LOCK).stat().st_mode), 0o600)

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
                     # Times no clock gave: a number too large for a float, one
                     # past the year 3000, and the two that are not numbers.
                     '{"providers": {"%s": {"percent": 40, "at": %s}}}' % (P2, '9' * 400),
                     json.dumps({'providers': {P2: {'percent': 40, 'at': 1e18}}}),
                     json.dumps({'providers': {P2: {'percent': 40, 'at': manual.LATEST + 1}}}),
                     '{"providers": {"%s": {"percent": 40, "at": NaN}}}' % P2,
                     '{"providers": {"%s": {"percent": 40, "at": Infinity}}}' % P2,
                     json.dumps({'providers': {P2: {'percent': 40, 'at': True}}}),
                     # Nested deeper than the reader will follow.
                     '[' * 100000 + ']' * 100000,
                     '{"providers": {"%s": %s}}' % (P2, '{"a":' * 100000 + '1' + '}' * 100000),
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

    def test_when_a_good_automatic_reading_was_taken(self):
        self.assertEqual(manual.taken(ok(19, NOW + 1)), NOW + 1)
        self.assertEqual(manual.taken(ok(0, NOW + 1)), NOW + 1)
        # Good is judged when it was taken: its window has reset since.
        old = {'status': 'ok', 'checked_at': NOW - 500, 'windows': [{'remaining_percent': 80, 'resets_at': NOW - 100}]}
        self.assertEqual(manual.taken(old), NOW - 500)
        # A reset time that is no number is no reset time; the reading is read.
        self.assertEqual(manual.taken({'status': 'ok', 'checked_at': NOW, 'windows': [
            {'remaining_percent': 50, 'resets_at': 10 ** 400}]}), NOW)
        for entry in (None, 'ok', {}, {'status': 'ok', 'windows': [{'remaining_percent': 5}]},
                      {'status': 'ok', 'checked_at': NOW, 'windows': [{'remaining_percent': 10 ** 400}]},
                      {'status': 'input_required', 'checked_at': NOW, 'windows': []},
                      {'status': 'ok', 'checked_at': NOW, 'windows': []},
                      {'status': 'ok', 'checked_at': NOW, 'windows': [{'remaining_percent': 150}]},
                      {'status': 'ok', 'checked_at': NOW, 'windows': [{'remaining_percent': 50, 'resets_at': NOW}]},
                      {'status': 'ok', 'checked_at': 'noon', 'windows': [{'remaining_percent': 50}]},
                      {'status': 'ok', 'checked_at': True, 'windows': [{'remaining_percent': 50}]},
                      {'status': 'ok', 'checked_at': -5, 'windows': [{'remaining_percent': 50}]},
                      {'status': 'ok', 'checked_at': 10 ** 400, 'windows': [{'remaining_percent': 50}]},
                      {'status': 'ok', 'checked_at': float('nan'), 'windows': [{'remaining_percent': 50}]}):
            self.assertIsNone(manual.taken(entry), entry)

    def test_which_values_stand(self):
        manual.set_values(self.state, {P1: 62, P2: 40, P3: 9}, NOW)
        before = (self.state / manual.NAME).read_bytes()
        hand = {P1: (62, NOW), P2: (40, NOW), P3: (9, NOW)}
        self.assertEqual(manual.standing(None, {}, NOW), {})
        # No automatic reading, a failed one, or a good one that is not newer.
        for entries in ({}, None, {P1: 'junk'}, {P1: {'status': 'input_required', 'checked_at': NOW + 50, 'windows': []}},
                        {P1: ok(19, NOW - 1)}, {P1: ok(19, NOW)}, {'qwen': ok(19, NOW + 5)}):
            with patch.object(manual, 'automatic_entries', return_value={}):
                self.assertEqual(manual.standing(self.state, entries, NOW + 60), hand, entries)
            self.assertEqual((self.state / manual.NAME).read_bytes(), before, entries)
        # A good reading taken later retires that value, and only that one.
        self.assertEqual(manual.standing(self.state, {P1: ok(19, NOW + 1), P2: ok(3, NOW - 1)}, NOW + 60),
                         {P2: (40, NOW), P3: (9, NOW)})
        self.assertEqual(manual.load(self.state), {P2: (40, NOW), P3: (9, NOW)})
        self.assertEqual(manual.standing(self.state, {P3: ok(0, NOW + 1)}, NOW + 60), {P2: (40, NOW)})

    def test_a_retired_value_does_not_come_back(self):
        manual.set_values(self.state, {P1: 5}, NOW)
        good = {'status': 'ok', 'checked_at': NOW + 3600, 'windows': [{'remaining_percent': 80, 'resets_at': NOW + 7200}]}
        self.assertEqual(manual.standing(self.state, {P1: good}, NOW + 3700), {})
        # The window resets, a later reading fails, a month passes.
        for entries, now in (({P1: good}, NOW + 7300),
                             ({P1: {'status': 'error', 'checked_at': NOW + 9000, 'windows': []}}, NOW + 9100),
                             ({}, NOW + 30 * 86400)):
            self.assertEqual(manual.standing(self.state, entries, now), {}, now)
        self.assertEqual(manual.load(self.state), {})

    def test_a_reading_that_was_good_when_taken_retires_it_even_seen_late(self):
        # Zell was not open while the reading was fresh. Its window has reset
        # by now, but it was a real answer, and it was taken after the value.
        manual.set_values(self.state, {P1: 5}, NOW)
        late = {'status': 'ok', 'checked_at': NOW + 60, 'windows': [{'remaining_percent': 80, 'resets_at': NOW + 600}]}
        self.assertEqual(manual.standing(self.state, {P1: late}, NOW + 5000), {})

    def test_a_value_dated_ahead_is_taken_as_set_now(self):
        # Set while the clock ran ten days fast. It must not outlast good readings.
        manual.set_values(self.state, {P1: 5, P2: 6}, NOW + 10 * 86400)
        self.assertEqual(manual.standing(self.state, {}, NOW), {P1: (5, NOW), P2: (6, NOW)})
        self.assertEqual(manual.load(self.state), {P1: (5, NOW), P2: (6, NOW)})
        self.assertEqual(manual.standing(self.state, {P1: ok(19, NOW + 30)}, NOW + 60), {P2: (6, NOW)})

    def test_a_value_remembers_the_reading_it_was_set_over(self):
        failed = {'status': 'error', 'checked_at': NOW - 7, 'windows': []}
        manual.set_values(self.state, {P1: 12, P2: 13, P3: 14}, NOW, {P1: good(90, NOW - 50), P2: failed})
        stored = json.loads((self.state / manual.NAME).read_text())['providers']
        self.assertEqual({k: v.get('over', 'absent') for k, v in stored.items()}, {P1: NOW - 50, P2: NOW - 7, P3: None})
        # Setting one again keeps what the others remember.
        manual.set_values(self.state, {P3: 15}, NOW + 1, {P3: good(1, NOW - 3)})
        stored = json.loads((self.state / manual.NAME).read_text())['providers']
        self.assertEqual({k: v.get('over', 'absent') for k, v in stored.items()}, {P1: NOW - 50, P2: NOW - 7, P3: NOW - 3})
        self.assertEqual(manual.load(self.state), {P1: (12, NOW), P2: (13, NOW), P3: (15, NOW + 1)})
        # A good reading in place of the failed one it was set over retires it.
        self.assertEqual(sorted(manual.standing(self.state, {P2: good(50, NOW + 9)}, NOW + 60)), [P1, P3])

    def test_a_clock_that_ran_fast_cannot_delete_a_newer_value(self):
        # The clock ran ten days fast for a reading and then for the value
        # set after it. Corrected, the value is dated ahead and is taken as
        # set now, which is before the reading's stamp. The reading is still
        # the one the value was set over, and does not retire it.
        fast = {P1: good(90, NOW + AHEAD - 3600)}
        manual.set_values(self.state, {P1: 12}, NOW + AHEAD, fast)
        for now in (NOW, NOW + 5, NOW + AHEAD + 5):
            self.assertEqual(manual.standing(self.state, fast, now), {P1: (12, NOW)}, now)
        self.assertEqual(manual.load(self.state), {P1: (12, NOW)})
        # A reading taken after the correction is another reading, and does.
        self.assertEqual(manual.standing(self.state, {P1: good(70, NOW + 3600)}, NOW + 4000), {})

    def test_a_reading_stamped_ahead_cannot_delete_a_value_set_now(self):
        # Only the reading was stamped by the fast clock: set-mana, then show-mana.
        ahead = {P1: good(90, NOW + AHEAD)}
        manual.set_values(self.state, {P1: 12}, NOW, ahead)
        self.assertEqual(manual.standing(self.state, ahead, NOW + 5), {P1: (12, NOW)})
        self.assertEqual(manual.load(self.state), {P1: (12, NOW)})

    def test_a_reading_written_after_a_value_dated_ahead_retires_it_at_once(self):
        # Set while the clock ran fast, over a reading taken before. After the
        # correction a new reading is taken, before Zell next looks. Its stamp
        # is older than the value's, but it is not the reading the value was
        # set over, so it was written after the value: it retires it, and does
        # not wait for the next one.
        before = {P1: good(90, NOW - 600)}
        manual.set_values(self.state, {P1: 12, P2: 13}, NOW + 3600, before)
        after = {P1: good(40, NOW + 10)}
        self.assertEqual(manual.standing(self.state, after, NOW + 20), {P2: (13, NOW + 20)})
        self.assertEqual(manual.load(self.state), {P2: (13, NOW + 20)})
        # Over no reading at all: any good reading there now came later.
        manual.set_values(self.state, {P3: 14}, NOW + 3600, {})
        self.assertEqual(manual.standing(self.state, {P3: good(40, NOW + 30)}, NOW + 40), {P2: (13, NOW + 20)})
        # A failed reading in its place retires nothing.
        manual.set_values(self.state, {P1: 15}, NOW + 3600, before)
        failed = {'status': 'error', 'checked_at': NOW + 50, 'windows': []}
        self.assertEqual(manual.standing(self.state, {P1: failed}, NOW + 60), {P1: (15, NOW + 60), P2: (13, NOW + 20)})

    def test_what_a_value_was_set_over_is_unknown_when_damaged(self):
        # A damaged "over" is as good as none said: the value dated ahead is
        # left as it is, and an older reading does not retire it.
        self.state.mkdir()
        for over in ('noon', True, -5, [], {}, float('nan')):
            text = json.dumps({'providers': {P1: {'percent': 12, 'at': NOW + AHEAD, 'over': over}}})
            (self.state / manual.NAME).write_text(text)
            self.assertEqual(manual.standing(self.state, {P1: good(90, NOW - 5)}, NOW), {P1: (12, NOW + AHEAD)}, over)
            self.assertEqual((self.state / manual.NAME).read_text(), text, over)

    def test_a_file_that_does_not_say_what_a_value_was_set_over(self):
        # Written by hand or by the first version: only the two times are
        # compared, and a value dated ahead is left as it is.
        self.state.mkdir()
        text = json.dumps({'providers': {P1: {'percent': 12, 'at': NOW + AHEAD}, P2: {'percent': 13, 'at': NOW, 'over': 'noon'}}})
        (self.state / manual.NAME).write_text(text)
        older = {P1: good(90, NOW + AHEAD - 3600), P2: good(90, NOW - 5)}
        self.assertEqual(manual.standing(self.state, older, NOW), {P1: (12, NOW + AHEAD), P2: (13, NOW)})
        self.assertEqual((self.state / manual.NAME).read_text(), text)
        self.assertEqual(manual.standing(self.state, {P1: good(90, NOW + AHEAD + 1), P2: good(90, NOW + 1)}, NOW + 2), {})

    def test_a_value_set_during_the_look_is_kept(self):
        # The look decides outside the lock, then reads again inside it: a
        # value set in between is newer than the reading and must survive.
        manual.set_values(self.state, {P1: 5}, NOW)
        real = manual._locked

        class SetJustBefore:
            def __enter__(inner):
                manual._save(self.state, {P1: (44, NOW + 100, None)})
                inner.held = real(self.state)
                return inner.held.__enter__()

            def __exit__(inner, *error):
                return inner.held.__exit__(*error)
        with patch.object(manual, '_locked', lambda directory: SetJustBefore()):
            self.assertEqual(manual.standing(self.state, {P1: ok(19, NOW + 50)}, NOW + 200), {P1: (44, NOW + 100)})
        self.assertEqual(manual.load(self.state), {P1: (44, NOW + 100)})

    @unittest.skipIf(os.geteuid() == 0, 'root writes anywhere')
    def test_a_folder_that_cannot_be_written_still_gives_the_right_answer(self):
        manual.set_values(self.state, {P1: 5, P2: 6}, NOW)
        before = (self.state / manual.NAME).read_bytes()
        # With the lock file there, the save is what fails; without it (a file
        # an older Zell or a person wrote), taking the lock already does.
        for lock_file in (True, False):
            if not lock_file:
                (self.state / manual.LOCK).unlink()
            self.state.chmod(0o500)
            try:
                self.assertEqual(manual.standing(self.state, {P1: ok(19, NOW + 50)}, NOW + 60), {P2: (6, NOW)}, lock_file)
            finally:
                self.state.chmod(0o700)
            self.assertEqual((self.state / manual.NAME).read_bytes(), before)
            self.assertEqual(sorted(path.name for path in self.state.iterdir()),
                             [manual.NAME, manual.LOCK] if lock_file else [manual.NAME])

    def test_one_writer_at_a_time(self):
        manual.set_values(self.state, {P1: 5}, NOW)
        env = {**os.environ, 'KILIX_ZELL_STATE': str(self.state), 'PYTHONDONTWRITEBYTECODE': '1',
               'CLAUDEX_CONFIG': str(Path(self.folder.name) / 'loop.json')}
        for script, words in (('set-mana', (P2, '62')), ('clear-mana', (P1,))):
            held = os.open(self.state / manual.LOCK, os.O_RDWR)
            try:
                fcntl.flock(held, fcntl.LOCK_EX)
                process = subprocess.Popen([str(SCRIPTS / script), *words], env=env, cwd=SCRIPTS,
                                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                with self.assertRaises(subprocess.TimeoutExpired, msg=script):
                    process.wait(timeout=1.5)
                self.assertEqual({k: v[0] for k, v in manual.load(self.state).items()}, {P1: 5}, script)
            finally:
                os.close(held)
            self.assertEqual(process.wait(timeout=60), 0, script)
            manual.set_values(self.state, {P1: 5}, NOW)
            manual.clear(self.state, [P2])

    def test_the_state_folder_is_the_room_interface_s(self):
        import desktop
        home = Path(self.folder.name)
        # What the room interface itself works out, in a process of its own.
        ask = ('import sys; sys.path.insert(0, sys.argv[1]); import desktop, zell_mana_manual; '
               'print(repr(str(desktop.BASE))); print(repr(str(zell_mana_manual.state_dir())))')
        for env in ({'KILIX_ZELL_STATE': str(home / 'a')}, {'GPU_TERMINAL_HOME': str(home / 'b')},
                    {'KILIX_ZELL_STATE': str(home / 'a'), 'GPU_TERMINAL_HOME': str(home / 'b')}, {},
                    {'KILIX_ZELL_STATE': 'relative/state'}, {'KILIX_ZELL_STATE': ''},
                    {'KILIX_ZELL_STATE': '', 'GPU_TERMINAL_HOME': str(home / 'b')}):
            clean = {k: v for k, v in os.environ.items() if k not in ('KILIX_ZELL_STATE', 'GPU_TERMINAL_HOME')}
            with patch.dict(os.environ, {**clean, **env}, clear=True):
                expected = Path(os.environ.get('KILIX_ZELL_STATE', Path(os.environ.get(
                    'GPU_TERMINAL_HOME', Path.home() / '.local/gpu_terminal')) / 'kilix-zell/state'))
                self.assertEqual(manual.state_dir(), expected, env)
            done = subprocess.run([sys.executable, '-c', ask, str(ROOT / 'tools')], capture_output=True, text=True,
                                  env={**clean, **env, 'PYTHONDONTWRITEBYTECODE': '1'}, cwd=home, timeout=60)
            room, scripts = done.stdout.splitlines()
            self.assertEqual(room, scripts, env)
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
        # A newer automatic reading wins here too: this value was set
        # before the reading of a minute ago existed.
        manual.set_values(self.state, {P1: 90}, NOW - 600, {})
        self.assertEqual(zell_vitals.mana(self.ui, NOW)[0], 60)
        # Set over that reading, it stands.
        manual.set_values(self.state, {P1: 90}, NOW - 600)
        self.assertEqual(zell_vitals.mana(self.ui, NOW)[0], 83)
        manual.clear(self.state, [P1])
        # And an interface with no state folder shows only automatic readings.
        # The provider state it reads is a folder of this test's own.
        apart = Path(self.folder.name) / 'no-hand-values'
        self.assertEqual(zell_vitals.mana(SimpleNamespace(), NOW, apart)[0], 20)

    def test_the_console_and_the_bar_agree(self):
        # A value an automatic reading replaced is gone from both, whatever
        # happens to that reading afterwards.
        manual.set_values(self.state, {P1: 5}, NOW - 7200)
        self.write(**{P1: ok(80, NOW - 3600)})
        self.assertEqual(zell_mana.readings(NOW, self.state)[0], 80)
        self.assertEqual(zell_vitals.mana(self.ui, NOW)[0], 80)
        stale = {'status': 'ok', 'checked_at': NOW - 3600, 'windows': [{'remaining_percent': 80, 'resets_at': NOW - 10}]}
        for entry in (stale, {'status': 'error', 'checked_at': NOW - 5, 'windows': []}, None):
            self.write(**({P1: entry} if entry else {}))
            self.assertEqual(zell_mana.readings(NOW, self.state)[0], -1, entry)
            value, _, lines = zell_vitals.mana(self.ui, NOW)
            self.assertEqual(value, -1, entry)
            self.assertNotIn('set by hand', lines[0])
        # The bar is asked first this time, and retires the value itself.
        manual.set_values(self.state, {P2: 9}, NOW - 7200)
        self.write(**{P2: ok(70, NOW - 3600)})
        self.assertEqual(zell_vitals.mana(self.ui, NOW)[0], 70)
        self.assertEqual(manual.load(self.state), {})

    def test_they_agree_when_a_provider_row_is_published(self):
        # One verified source publishes 80% for the first provider. What the
        # person set by hand comes before it on the console and in the bar.
        import zell_providers
        source = zell_providers._source(P1, 'vault', 'vault:' + 'a' * 32, 'none')
        source.update(state='verified', percent=80, verified_at=int(NOW) - 60, reason='none')
        state = zell_providers.blank_state()
        state['sources'] = [source]
        zell_providers.write_state(self.state, state)
        zell_providers.forget(self.state)
        self.assertEqual(zell_mana.console_frames(self.state, NOW)[-1], ('G', 'mana 80 -1 -1'))
        self.assertEqual(zell_vitals.mana(self.ui, NOW)[0], 80)
        manual.set_values(self.state, {P1: 10, P2: 50}, NOW - 30)
        self.assertEqual(zell_mana.console_frames(self.state, NOW)[-1], ('G', 'mana 10 50 -1'))
        value, _, lines = zell_vitals.mana(self.ui, NOW)
        self.assertEqual(value, 30)        # mean of the two the person set
        self.assertIn(f'{P1}: 10% remaining (set by hand)', lines[0])
        manual.clear(self.state, [P1])
        self.assertEqual(zell_mana.console_frames(self.state, NOW)[-1], ('G', 'mana 80 50 -1'))
        self.assertEqual(zell_vitals.mana(self.ui, NOW)[0], 65)
        manual.clear(self.state)
        self.assertEqual(zell_vitals.mana(self.ui, NOW)[0], 80)

    def test_the_console_frames_go_out_in_one_write(self):
        from zell_dialogue import Dialogue
        written = []
        writer = SimpleNamespace(write=written.append, flush=lambda: written.append('flush'))
        manual.set_values(self.state, {P2: 62}, NOW - 10)
        zell_mana.send_console(Dialogue(reader=None, writer=writer), self.state, NOW)
        self.assertEqual(len(written), 2)
        self.assertEqual(written[1], 'flush')
        lines = written[0].splitlines()
        self.assertEqual([(line[0], bytes.fromhex(line[1:]).decode()) for line in lines],
                         zell_mana.console_frames(self.state, NOW))
        self.assertEqual(lines[-1], Dialogue.frame('G', 'mana -1 62 -1').rstrip('\n'))
        # One frame alone is sent as it always was.
        del written[:]
        Dialogue(reader=None, writer=writer).send('V', 'a\x1bb')
        self.assertEqual(written, ['V' + b'ab'.hex() + '\n', 'flush'])

    def test_what_the_monitor_sends_for_the_console(self):
        import zell_providers
        self.write(**{P1: ok(19, NOW - 60)})
        manual.set_values(self.state, {P2: 62, P3: 5}, NOW - 10)
        # A state folder with no providers file: the snapshot has no rows and
        # is three bare integers. The mana line comes after it, marked.
        frames = zell_mana.console_frames(self.state, NOW)
        self.assertEqual(frames, [('G', '1 1 0'), ('G', 'mana 19 62 5')])
        self.assertEqual(frames[:-1], zell_providers.monitor_frames(self.state, NOW))
        # Rows a snapshot publishes replace Claudex's readings, and come after
        # what the person said. A row with no percentage is not published.
        snapshot = [('G', f'1 7 3\n{P1} 44 verified api unknown\n{P2} 45 verified api unknown\n{P3} -1 stale api unknown'),
                    ('H', '1 2 0')]
        with patch.object(zell_providers, 'monitor_frames', return_value=snapshot):
            self.assertEqual(zell_mana.console_frames(self.state, NOW), snapshot + [('G', 'mana 44 62 5')])
            manual.clear(self.state)
            self.write(**{P1: ok(19, NOW - 60), P3: ok(33, NOW - 60)})
            self.assertEqual(zell_mana.console_frames(self.state, NOW)[-1], ('G', 'mana 44 45 -1'))
        self.assertEqual(zell_mana.published(snapshot), {P1: 44, P2: 45, P3: -1})
        self.assertEqual(zell_mana.published([('G', '1 1 0'), ('G', 'junk\n\nword\nname x'), ('F', f'1 1 1\n{P1} 9')]), {})
        self.assertEqual(zell_mana.readings(NOW, None, {P1: 101, P2: True, P3: '50'}), [19, -1, 33])
        self.assertEqual(zell_mana.readings(NOW, None, {P1: -1, P2: 70}), [-1, 70, -1])
        # A snapshot whose rows all lack a percentage publishes nothing.
        self.assertEqual(zell_mana.readings(NOW, None, {P3: -1}), [19, -1, 33])

    def published(self, rows):
        import zell_providers
        state = zell_providers.blank_state()
        for n, (provider, percent) in enumerate(rows.items()):
            source = zell_providers._source(provider, 'vault', 'vault:' + chr(97 + n) * 32, 'none')
            source.update(state='verified', percent=percent, verified_at=int(NOW) - 60, reason='none')
            state['sources'].append(source)
        zell_providers.write_state(self.state, state)
        zell_providers.forget(self.state)

    def console(self):
        return [int(word) for word in zell_mana.console_frames(self.state, NOW)[-1][1].split()[1:]]

    def test_the_bar_is_the_mean_of_what_the_console_shows(self):
        # Claudex has readings for all three; snapshots publish some.
        self.write(**{P1: ok(10, NOW - 60), P2: ok(20, NOW - 60), P3: ok(40, NOW - 60)})
        for rows, hand, shown in (({}, {}, [10, 20, 40]),
                                  ({P1: 80}, {}, [80, -1, -1]),
                                  ({P1: 80}, {P3: 5}, [80, -1, 5]),
                                  ({P1: -1, P2: 70}, {}, [-1, 70, -1]),
                                  ({P1: -1}, {}, [10, 20, 40]),
                                  ({P2: 70}, {}, [-1, 70, -1]),
                                  ({P1: 30, P2: 70, P3: 90}, {P2: 0}, [30, 0, 90]),
                                  ({}, {P1: 100}, [100, 20, 40])):
            manual.clear(self.state)
            self.published(rows)
            if hand:
                manual.set_values(self.state, hand, NOW - 30)
            self.assertEqual(self.console(), shown, (rows, hand))
            known = [value for value in shown if value >= 0]
            self.assertEqual(zell_vitals.mana(self.ui, NOW)[0], round(sum(known) / len(known)), (rows, hand))

    def test_the_bar_also_counts_published_providers_the_console_has_no_place_for(self):
        # The console has places for three. The bar is the mean of every
        # published percentage, as it was before there was a console.
        import zell_providers
        other = next(name for name in zell_providers.IDS if name not in manual.PROVIDERS)
        self.write(**{P2: ok(20, NOW - 60)})
        self.published({P1: 80, other: 10})
        self.assertEqual(self.console(), [80, -1, -1])
        value, _, lines = zell_vitals.mana(self.ui, NOW)
        self.assertEqual(value, 45)
        self.assertEqual(lines[:3], [f'{P1}: 80% remaining (published by a provider source)',
                                     f'{P2}: not published by a provider source',
                                     f'{P3}: not published by a provider source'])

    def test_a_reading_followed_by_a_failed_check_is_shown_by_neither(self):
        self.write(**{P1: ok(10, NOW - 60), P2: ok(20, NOW - 60)})
        attempts = self.cache.with_name('zell-mana-attempts.json')
        attempts.write_text(json.dumps({P1: {'at': NOW - 30, 'failed': True}, P2: {'at': NOW - 30, 'failed': False}}))
        self.assertEqual(self.console(), [-1, 20, -1])
        self.assertEqual(zell_vitals.mana(self.ui, NOW)[0], 20)
        # A reading with no time it was taken, or one too large to be a time.
        self.write(**{P1: {'status': 'ok', 'windows': [{'remaining_percent': 50}]},
                      P2: {'status': 'ok', 'checked_at': 10 ** 400, 'windows': [{'remaining_percent': 50}]},
                      P3: ok(30, NOW - 60)})
        attempts.unlink()
        self.assertEqual(self.console(), [-1, -1, 30])
        self.assertEqual(zell_vitals.mana(self.ui, NOW)[0], 30)

    def test_damaged_readings_are_no_error_for_the_monitor(self):
        # The room's monitor asks whether a reading is due; nothing it reads may raise there.
        ui = SimpleNamespace(state_read=lambda name, default: {'enabled': True} if name == zell_mana.SETTING else default)
        with patch.object(zell_vitals, 'claudex_script', return_value=Path('/x/pane_loop.py')):
            self.assertTrue(zell_mana.automatic(ui))
            for entry in ({'status': 'ok', 'checked_at': 10 ** 400, 'windows': [{'remaining_percent': 50}]},
                          {'status': 'ok', 'checked_at': -10 ** 400, 'windows': []},
                          {'status': 'ok', 'checked_at': float('inf'), 'windows': []}):
                self.write(**{P1: entry})
                self.assertTrue(zell_mana.due(ui, NOW), entry)
        nested = '[' * 100000 + ']' * 100000
        self.cache.write_text('{"providers": {"%s": %s}}' % (P1, nested))
        self.assertEqual(zell_vitals.mana(self.ui, NOW)[0], -1)
        self.assertEqual(self.console(), [-1, -1, -1])


class ScriptTests(unittest.TestCase):
    """The three scripts in the Command computer's folder, run as a person would."""

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.state = Path(self.folder.name) / 'state'
        # Claudex's readings come from this folder too: none, so no reading
        # on the machine running the test can retire a value set here.
        self.env = {**os.environ, 'KILIX_ZELL_STATE': str(self.state), 'PYTHONDONTWRITEBYTECODE': '1',
                    'CLAUDEX_CONFIG': str(Path(self.folder.name) / 'loop.json')}

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
        self.assertEqual(command[-3:], [str(SCRIPTS), str(Path(desktop.BASE).resolve()), '/bin/sh'])
        # The shell changes folder, so a state folder named from where Zell
        # was started is given in full.
        with patch.object(desktop, 'BASE', Path('zstate')):
            self.assertEqual(desktop.command_computer('/bin/sh')[-2], str(Path.cwd() / 'zstate'))
        with patch.object(desktop, 'BASE', Path('')):
            self.assertEqual(desktop.command_computer('/bin/sh')[-2], str(Path.cwd()))
        # A shell that is not there falls back to /bin/sh.
        self.assertEqual(desktop.command_computer('/no/such/shell')[-1], '/bin/sh')
        probe = [*command[:-2], str(self.state), '/bin/sh']
        # A home of its own: a shell that lost its state folder writes there,
        # not into the real one.
        apart = {k: v for k, v in os.environ.items() if k not in ('KILIX_ZELL_STATE', 'GPU_TERMINAL_HOME')}
        apart.update(HOME=self.folder.name, PYTHONDONTWRITEBYTECODE='1',
                     CLAUDEX_CONFIG=str(Path(self.folder.name) / 'loop.json'))
        done = subprocess.run(probe, input='pwd\necho "state=$KILIX_ZELL_STATE"\ncommand -v set-mana\n'
                              f'set-mana {P3} 12\nshow-mana\n', capture_output=True, text=True, env=apart, timeout=60)
        self.assertEqual(done.returncode, 0, done.stderr)
        lines = done.stdout.splitlines()
        self.assertIn('set-mana, show-mana, clear-mana', lines[0])
        # For a shell whose own start-up file drops the folder from PATH.
        self.assertIn('./set-mana', lines[2])
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
        # The room's monitor sends the console's frames for its own state folder.
        source = (ROOT / 'tools/desktop.py').read_text()
        self.assertIn('zell_mana.send_console(DIALOGUE,BASE)', source)
        self.assertNotIn('zell_mana.console_frames(', source)
        self.assertNotIn('zell_mana.readings(', source)
        self.assertNotIn('zell_providers.monitor_frames(', source)

    def test_show_mana_tells_what_stands(self):
        # A good reading taken after the value retires it here as well, and a
        # damaged file is nothing set, not a traceback.
        manual.set_values(self.state, {P1: 40, P2: 62}, 1000.0)
        cache = Path(self.folder.name) / '.loop/usage-pages.json'
        cache.parent.mkdir()
        cache.write_text(json.dumps({'providers': {P1: {'status': 'ok', 'checked_at': time.time() - 60, 'windows': [
            {'remaining_percent': 19, 'resets_at': time.time() + 9000}]}}}))
        shown = self.run_script('show-mana')
        self.assertEqual(shown.returncode, 0, shown.stderr)
        self.assertIn(f'{P1}: not set by hand', shown.stdout)
        self.assertIn(f'{P2}: 62% left, set by hand', shown.stdout)
        self.assertEqual(sorted(manual.load(self.state)), [P2])
        for at in ('9' * 400, '1e18', 'NaN', '[' * 100000 + ']' * 100000):
            (self.state / manual.NAME).write_text('{"providers": {"%s": {"percent": 40, "at": %s}}}' % (P2, at))
            shown = self.run_script('show-mana')
            self.assertEqual((shown.returncode, shown.stderr), (0, ''), at[:8])
            self.assertIn(f'{P2}: not set by hand', shown.stdout)
            # set-mana repairs it.
            self.assertEqual(self.run_script('set-mana', P2, '41').returncode, 0, at[:8])
            self.assertEqual({k: v[0] for k, v in manual.load(self.state).items()}, {P2: 41})
        # A damaged reading in Claudex's cache is no reading, not a traceback.
        for window, left in (('{"remaining_percent": %s}' % ('9' * 400), True),
                             ('{"remaining_percent": 19, "resets_at": %s}' % ('9' * 400), False)):
            cache.write_text('{"providers": {"%s": {"status": "ok", "checked_at": %r, "windows": [%s]}}}' % (P2, time.time(), window))
            shown = self.run_script('show-mana')
            self.assertEqual((shown.returncode, shown.stderr), (0, ''), window[:30])
            # No percentage is no reading. A reset time that is no number is
            # no reset time, and that reading is good and newer.
            self.assertEqual(f'{P2}: 41% left, set by hand' in shown.stdout, left, window[:30])


if __name__ == '__main__':
    unittest.main()
