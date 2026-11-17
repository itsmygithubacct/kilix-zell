#!/usr/bin/env python3
"""Claudex is installed on first use, by Kilix, and never by merely looking.

Every test runs against a stand-in `kilix` command in a temporary folder. No
test can reach the real one: PATH holds the stand-in and the system tools only.
"""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import zell_mana  # noqa: E402
import zell_vitals as vitals  # noqa: E402

KILIX = r'''#!/bin/sh
# A stand-in for `kilix app install kilix-claudex`.
ctl=$(dirname "$0")/../ctl
printf '%s|auto=%s|cwd=%s\n' "$*" "${KILIX_APP_AUTO_INSTALL-unset}" "$PWD" >> "$ctl/calls"
[ "$*" = "app install kilix-claudex" ] || { echo "kilix: unexpected arguments" >&2; exit 64; }
if [ -e "$ctl/slow" ]; then sleep 5; fi
if [ -e "$ctl/says" ]; then cat "$ctl/says"; if [ -e "$ctl/broken" ]; then exit 3; fi; exit 0; fi
if [ -x "$ctl/app/claudex" ]; then echo "$ctl/app/claudex"; exit 0; fi
if [ "${KILIX_APP_AUTO_INSTALL-1}" = 0 ]; then
    echo "kilix app kilix-claudex: not installed; set KILIX_APP_AUTO_INSTALL=1 to build it" >&2; exit 1
fi
if [ -e "$ctl/offline" ]; then
    echo "kilix app kilix-claudex: fetching" >&2
    echo "kilix app kilix-claudex: the network is down" >&2; exit 1
fi
echo "kilix app kilix-claudex: needs Python 3.10 or newer" >&2
mkdir -p "$ctl/app"
cat > "$ctl/app/pane_loop.py" <<'PY'
import json, sys
from pathlib import Path
Path(__file__).with_name('ran').write_text(json.dumps(sys.argv[1:]))
PY
printf '#!/bin/sh\n' > "$ctl/app/claudex"; chmod 755 "$ctl/app/claudex"
echo "$ctl/app/claudex"
'''


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name).resolve()
        self.ctl = self.base / 'ctl'
        self.ctl.mkdir()
        (self.base / 'bin').mkdir()
        (self.base / '.loop').mkdir()
        self.kilix = self.base / 'bin/kilix'
        self.kilix.write_text(KILIX)
        self.kilix.chmod(0o755)
        self.clock = [2_000_000.0]
        self.said = []
        env = {k: v for k, v in os.environ.items() if k not in ('KILIX_ZELL_CLAUDEX', 'KILIX_APP_AUTO_INSTALL')}
        env.update(PATH=f'{self.base}/bin:/usr/bin:/bin', CLAUDEX_CONFIG=str(self.base / 'loop.json'),
                   HOME=str(self.base), PYTHONDONTWRITEBYTECODE='1')
        for context in (patch.dict(os.environ, env, clear=True),
                        # No Claudex checkout beside this Zell.
                        patch.object(vitals, '__file__', str(self.base / 'a/b/tools/zell_vitals.py')),
                        patch.object(vitals.time, 'time', side_effect=lambda: self.clock[0]),
                        patch.dict(vitals._catalog, {'script': None, 'asked': None})):
            context.start()
            self.addCleanup(context.stop)
        self.reader = self.ctl / 'app/pane_loop.py'

    def calls(self):
        log = self.ctl / 'calls'
        return log.read_text().splitlines() if log.exists() else []

    def install_calls(self):
        return [line for line in self.calls() if '|auto=0|' not in line]

    def test_looking_asks_kilix_without_installing_and_not_on_every_look(self):
        self.assertIsNone(vitals.claudex_script())
        self.assertEqual(self.calls(), [f'app install kilix-claudex|auto=0|cwd={self.base}'])
        self.assertFalse(self.reader.exists())
        # The room's monitor looks every few seconds: the answer is kept.
        for _ in range(20):
            self.clock[0] += 5
            self.assertIsNone(vitals.claudex_script())
        self.assertEqual(len(self.calls()), 1)
        self.clock[0] = 2_000_000.0 + vitals.CATALOG_RECHECK - 1
        vitals.claudex_script()
        self.assertEqual(len(self.calls()), 1)
        self.clock[0] = 2_000_000.0 + vitals.CATALOG_RECHECK
        self.assertIsNone(vitals.claudex_script())
        self.assertEqual(len(self.calls()), 2)
        self.assertEqual(self.install_calls(), [])
        self.assertFalse(self.reader.exists())

    def test_first_use_installs_once_and_says_so(self):
        self.assertEqual(vitals.claudex_ready(self.said.append), (self.reader, ''))
        self.assertTrue(self.reader.is_file())
        self.assertEqual(self.install_calls(), [f'app install kilix-claudex|auto=unset|cwd={self.base}'])
        self.assertEqual(len(self.said), 2)
        self.assertIn('not installed yet', self.said[0])
        self.assertIn('installed', self.said[1])
        # From here on it is simply found: no more words, no more installs,
        # and no more questions to Kilix.
        asked = len(self.calls())
        for _ in range(5):
            self.clock[0] += 1000
            self.assertEqual(vitals.claudex_ready(self.said.append), (self.reader, ''))
            self.assertEqual(vitals.claudex_script(), self.reader)
        self.assertEqual((len(self.said), len(self.calls())), (2, asked))

    def test_a_copy_kilix_installed_earlier_is_found_by_looking(self):
        vitals.claudex_catalog(install=True)
        (self.ctl / 'calls').unlink()
        vitals._catalog.update(script=None, asked=None)
        self.assertEqual(vitals.claudex_script(), self.reader)
        self.assertEqual(self.install_calls(), [])
        self.assertEqual(vitals.claudex_ready(self.said.append), (self.reader, ''))
        self.assertEqual((self.said, self.install_calls()), ([], []))

    def test_a_copy_that_was_removed_is_not_believed(self):
        vitals.claudex_ready(self.said.append)
        (self.ctl / 'app/claudex').unlink()
        self.reader.unlink()
        self.clock[0] += vitals.CATALOG_RECHECK
        self.assertIsNone(vitals.claudex_script())

    def test_a_copy_beside_zell_or_on_path_needs_no_kilix(self):
        local = self.base / 'elsewhere'
        local.mkdir()
        (local / 'pane_loop.py').write_text('')
        with patch.dict(os.environ, {'KILIX_ZELL_CLAUDEX': str(local / 'pane_loop.py')}):
            self.assertEqual(vitals.claudex_script(), local / 'pane_loop.py')
            self.assertEqual(vitals.claudex_ready(self.said.append), (local / 'pane_loop.py', ''))
        self.assertEqual((self.calls(), self.said), ([], []))

    def test_the_person_s_refusal_of_installs_is_kept(self):
        with patch.dict(os.environ, {'KILIX_APP_AUTO_INSTALL': '0'}):
            script, reason = vitals.claudex_ready(self.said.append)
        self.assertIsNone(script)
        self.assertIn('KILIX_APP_AUTO_INSTALL=1', reason)
        self.assertEqual(self.install_calls(), [])
        self.assertFalse(self.reader.exists())

    def test_a_failed_install_gives_kilix_s_last_words(self):
        (self.ctl / 'offline').write_text('')
        script, reason = vitals.claudex_ready(self.said.append)
        self.assertIsNone(script)
        self.assertEqual(reason, 'kilix app kilix-claudex: the network is down')
        self.assertEqual(len(self.said), 1)
        # Not installed is not final: the next task tries again.
        (self.ctl / 'offline').unlink()
        self.assertEqual(vitals.claudex_ready(self.said.append), (self.reader, ''))

    def test_without_kilix_nothing_is_tried_or_promised(self):
        self.kilix.unlink()
        self.assertEqual(vitals.claudex_ready(self.said.append), (None, 'the kilix command was not found'))
        self.assertIsNone(vitals.claudex_script())
        self.assertEqual(self.said, [])

    def test_what_kilix_prints_must_lead_to_a_reader(self):
        stray = self.base / 'stray'
        stray.mkdir()
        (stray / 'claudex').write_text('')
        for text in ('', '\n', 'relative/claudex\n', f'{stray}/claudex\n', f'{self.base}/none/claudex\n'):
            (self.ctl / 'says').write_text(text)
            script, reason = vitals.claudex_catalog(install=True)
            self.assertIsNone(script, text)
            self.assertTrue(reason, text)
        # The command may be a link into the installed folder.
        (self.ctl / 'says').unlink()
        vitals.claudex_catalog(install=True)
        # A relative path is refused even where it would lead to a reader:
        # it means nothing once Zell is started from another folder.
        (self.ctl / 'says').write_text('app/claudex\n')
        here = os.getcwd()
        self.addCleanup(os.chdir, here)
        os.chdir(self.ctl)
        self.assertTrue((Path('app/claudex').resolve().with_name('pane_loop.py')).is_file())
        self.assertIsNone(vitals.claudex_catalog()[0])
        os.chdir(here)
        link = self.base / 'bin/claudex-link'
        link.symlink_to(self.ctl / 'app/claudex')
        (self.ctl / 'says').write_text(f'kilix app: note\n{link}\n')
        self.assertEqual(vitals.claudex_catalog(), (self.reader, ''))

    def test_a_path_from_a_kilix_that_failed_is_not_believed(self):
        vitals.claudex_catalog(install=True)
        (self.ctl / 'says').write_text(f'{self.ctl}/app/claudex\n')
        self.assertEqual(vitals.claudex_catalog(), (self.reader, ''))
        (self.ctl / 'broken').write_text('')
        self.assertEqual(vitals.claudex_catalog(), (None, 'kilix app install kilix-claudex ended with status 3'))

    def test_an_install_that_hangs_is_given_up(self):
        (self.ctl / 'slow').write_text('')
        with patch.object(vitals, 'INSTALL_TIMEOUT', 0.3):
            script, reason = vitals.claudex_ready(self.said.append)
        self.assertIsNone(script)
        self.assertIn('timed out', reason)

    def test_read_mana_now_installs_then_reads(self):
        with patch('builtins.print') as told:
            done = zell_mana.claudex('usage', 'read', '--json')
        # The station's page shows the helper's two lines about the install.
        self.assertEqual([call.args[0][:21] for call in told.call_args_list], ['Claudex is not instal', 'Claudex is installed.'])
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(json.loads((self.ctl / 'app/ran').read_text()),
                         ['--config', str(self.base / 'loop.json'), 'usage', 'read', '--json'])
        self.assertEqual(len(self.install_calls()), 1)
        zell_mana.claudex('usage', 'show')
        self.assertEqual(len(self.install_calls()), 1)

    def test_the_console_says_why_when_it_cannot_install(self):
        (self.ctl / 'offline').write_text('')
        pages = []
        ui = SimpleNamespace(page_text=pages.append, state_read=lambda name, default: default,
                             state_write=lambda name, value: None)
        with patch.object(vitals, 'snapshot', return_value={'details': 'DETAILS'}), patch('builtins.print'):
            zell_mana.hub(ui, 4)
            zell_mana.hub(ui, 1)
        self.assertIn('Mana could not be read', pages[0])
        self.assertIn('Kilix could not install it (kilix app kilix-claudex: the network is down)', pages[0])
        self.assertIn('could not be opened', pages[1])
        self.assertIn('the network is down', pages[1])

    def test_the_automatic_reading_never_installs(self):
        state = {}
        ui = SimpleNamespace(state_read=lambda name, default: state.get(name, default),
                             state_write=lambda name, value: state.__setitem__(name, value))
        for _ in range(3):
            self.assertFalse(zell_mana.due(ui, self.clock[0]))
            self.assertFalse(zell_mana.refresh_in_background(ui, self.clock[0]))
            self.clock[0] += vitals.CATALOG_RECHECK
        self.assertEqual(self.install_calls(), [])
        self.assertFalse(self.reader.exists())
        self.assertEqual(state, {})
        # Once a task has installed it, the automatic reading is due.
        vitals.claudex_ready(self.said.append)
        self.assertTrue(zell_mana.due(ui, self.clock[0]))

    def test_the_wall_clock_s_mana_check_installs_then_checks(self):
        ui = SimpleNamespace(page_text=self.said.append, state_read=lambda name, default: default)
        checked = []
        with patch.object(vitals, 'check_provider', side_effect=checked.append), \
                patch.object(vitals, 'snapshot', return_value={'details': 'DETAILS'}), patch('builtins.print'):
            vitals.request_mana(ui)
        self.assertEqual(len(self.install_calls()), 1)
        self.assertEqual(checked, list(vitals.PROVIDERS))

    def test_the_wall_clock_says_why_when_it_cannot_install(self):
        (self.ctl / 'offline').write_text('')
        ui = SimpleNamespace(page_text=self.said.append, state_read=lambda name, default: default)
        with patch.object(vitals, 'check_provider') as check, \
                patch.object(vitals, 'snapshot', return_value={'details': 'DETAILS'}), patch('builtins.print'):
            vitals.request_mana(ui)
        check.assert_not_called()
        self.assertIn('Claudex usage reader was not found', self.said[-1])
        self.assertIn('the network is down', self.said[-1])
        self.assertIn('kilix app install kilix-claudex', self.said[-1])
        # Nothing was asked of any provider, so nothing is recorded or throttled.
        self.assertFalse((self.base / '.loop/zell-mana-attempts.json').exists())


if __name__ == '__main__':
    unittest.main()
