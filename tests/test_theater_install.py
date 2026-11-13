"""Fixed-action installer. Package commands, downloads, and confirmation are stubbed."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import zell_theater_install as install


class FakeHost:
    def __init__(self):
        self.commands = []
        self.confirmed = 0
        self.typed = 'install'
        self.present = False
        self.simulate = 0
        self.install_code = 0
        self.downloaded = []
        self.fields = {'package': 'google-chrome-stable', 'architecture': 'amd64'}
        self.fail_download = False
        self.field_calls = 0
        self.cleaned = []
        self.dirs = []

    def which(self, name):
        if self.present:
            return '/usr/bin/' + name
        return None

    def run(self, argv):
        self.commands.append(list(argv))
        if argv and argv[0] == 'sudo':
            if self.install_code == 0:
                self.present = True
            return self.install_code
        return self.simulate

    def confirm(self):
        self.confirmed += 1
        return install.accept_install(self.typed)

    def download(self, url, dest):
        self.downloaded.append((url, dest))
        if self.fail_download:
            raise OSError('download failed')
        Path(dest).write_bytes(b'not a real package')

    def deb_fields(self, path):
        self.field_calls += 1
        if not Path(path).is_file():
            raise ValueError('missing deb')
        return dict(self.fields)

    def private_dir(self):
        path = tempfile.mkdtemp(prefix='zell-theater-chrome-')
        self.dirs.append(path)
        return path

    def cleanup(self, path):
        self.cleaned.append(path)
        install.Host.cleanup(self, path)


class InstallTests(unittest.TestCase):
    def test_usage_rejects_anything_except_the_three_actions(self):
        host = FakeHost()
        for argv in (
            ['zell_theater_install.py'],
            ['zell_theater_install.py', 'install-firefox-esr', 'extra'],
            ['zell_theater_install.py', 'sudo'],
            ['zell_theater_install.py', 'google-chrome-stable'],
            ['zell_theater_install.py', 'apt-get install firefox-esr'],
            ['zell_theater_install.py', 'install-firefox-esr;reboot'],
        ):
            self.assertEqual(install.main(argv, host), 2)
        self.assertEqual(host.commands, [])
        self.assertEqual(host.confirmed, 0)

    def test_confirmation_accepts_only_the_word_install(self):
        self.assertTrue(install.accept_install('install'))
        self.assertTrue(install.accept_install('install\n'))
        for typed in ('', 'no', 'Install', 'install; reboot', 'sudo apt-get install firefox-esr', None, 1):
            self.assertFalse(install.accept_install(typed))

    def test_firefox_cancel_and_preview_failure_do_not_sudo(self):
        cancelled = FakeHost()
        cancelled.typed = 'install; reboot'
        self.assertEqual(install.main(['prog', 'install-firefox-esr'], cancelled), 1)
        self.assertEqual(cancelled.commands, [['apt-get', '--simulate', 'install', 'firefox-esr']])
        self.assertEqual(cancelled.confirmed, 1)
        self.assertFalse(any(cancelled.typed in item for argv in cancelled.commands for item in argv))

        failed = FakeHost()
        failed.simulate = 1
        self.assertEqual(install.main(['prog', 'install-firefox-esr'], failed), 1)
        self.assertEqual(failed.commands, [['apt-get', '--simulate', 'install', 'firefox-esr']])
        self.assertEqual(failed.confirmed, 0)

    def test_firefox_and_chromium_use_fixed_packages(self):
        firefox = FakeHost()
        self.assertEqual(install.main(['prog', 'install-firefox-esr'], firefox), 0)
        self.assertEqual(firefox.commands, [
            ['apt-get', '--simulate', 'install', 'firefox-esr'],
            ['sudo', 'apt-get', 'install', '-y', 'firefox-esr'],
        ])
        self.assertTrue(firefox.present)

        missing = FakeHost()
        missing.install_code = 0
        missing.present = False

        def stay_missing(_name):
            return None

        missing.which = stay_missing
        self.assertEqual(install.main(['prog', 'install-firefox-esr'], missing), 1)
        self.assertEqual(missing.commands[-1][0], 'sudo')

        chromium = FakeHost()
        self.assertEqual(install.main(['prog', 'install-chromium'], chromium), 0)
        self.assertEqual(chromium.commands, [
            ['apt-get', '--simulate', 'install', 'chromium'],
            ['sudo', 'apt-get', 'install', '-y', 'chromium'],
        ])
        joined = ' '.join(' '.join(argv) for argv in chromium.commands)
        self.assertNotIn('firefox', joined)
        self.assertNotIn('chrome', joined)

    def test_sudo_failure_does_not_report_success(self):
        host = FakeHost()
        host.install_code = 7
        self.assertEqual(install.main(['prog', 'install-chromium'], host), 1)
        self.assertEqual(host.commands[-1], ['sudo', 'apt-get', 'install', '-y', 'chromium'])
        self.assertFalse(host.present)

    def test_an_already_present_browser_is_not_reinstalled(self):
        host = FakeHost()
        host.present = True
        self.assertEqual(install.main(['prog', 'install-chromium'], host), 0)
        self.assertEqual(host.commands, [])
        self.assertEqual(host.confirmed, 0)
        self.assertEqual(host.downloaded, [])

    def test_chrome_installs_the_official_local_deb(self):
        host = FakeHost()
        self.assertEqual(install.main(['prog', 'install-chrome-deb'], host), 0)
        self.assertEqual(len(host.downloaded), 1)
        url, dest = host.downloaded[0]
        self.assertEqual(url, install.CHROME_DEB_URL)
        self.assertTrue(dest.endswith('/' + install.CHROME_DEB_NAME))
        self.assertEqual(host.commands, [
            ['apt-get', '--simulate', 'install', dest],
            ['sudo', 'apt-get', 'install', '-y', dest],
        ])
        for argv in host.commands:
            self.assertNotIn('google-chrome-stable', argv)
            self.assertNotIn('sudo', argv[1:])
        self.assertEqual(host.field_calls, 1)
        self.assertEqual(host.cleaned, host.dirs)
        self.assertTrue(host.dirs)
        self.assertFalse(os.path.exists(host.dirs[0]))

    def test_chrome_rejects_bad_metadata_download_failure_and_cancel(self):
        wrong = FakeHost()
        wrong.fields = {'package': 'vim', 'architecture': 'amd64'}
        self.assertEqual(install.main(['prog', 'install-chrome-deb'], wrong), 1)
        self.assertEqual(wrong.commands, [])
        self.assertEqual(wrong.cleaned, wrong.dirs)
        self.assertFalse(os.path.exists(wrong.dirs[0]))

        other_arch = FakeHost()
        other_arch.fields = {'package': 'google-chrome-stable', 'architecture': 'arm64'}
        self.assertEqual(install.main(['prog', 'install-chrome-deb'], other_arch), 1)
        self.assertEqual(other_arch.commands, [])

        failed = FakeHost()
        failed.fail_download = True
        self.assertEqual(install.main(['prog', 'install-chrome-deb'], failed), 1)
        self.assertEqual(failed.commands, [])
        self.assertEqual(failed.field_calls, 0)
        self.assertFalse(os.path.exists(failed.dirs[0]))

        cancelled = FakeHost()
        cancelled.typed = ''
        self.assertEqual(install.main(['prog', 'install-chrome-deb'], cancelled), 1)
        self.assertEqual(len(cancelled.commands), 1)
        self.assertEqual(cancelled.commands[0][0], 'apt-get')
        self.assertFalse(os.path.exists(cancelled.dirs[0]))

    def test_deb_fields_parser_and_download_guard(self):
        self.assertEqual(
            install.parse_deb_fields('google-chrome-stable\namd64\n'),
            {'package': 'google-chrome-stable', 'architecture': 'amd64'},
        )
        self.assertEqual(
            install.parse_deb_fields('Package: google-chrome-stable\nArchitecture: amd64\n'),
            {'package': 'google-chrome-stable', 'architecture': 'amd64'},
        )
        with self.assertRaises(ValueError):
            install.parse_deb_fields('nope')
        with self.assertRaises(ValueError):
            install.Host().download('https://example.com/chrome.deb', os.devnull)
        with self.assertRaises(ValueError):
            install.Host().download(install.CHROME_DEB_URL + '.txt', os.devnull)

    def test_run_rejects_a_shell_and_cleanup_stays_in_the_private_prefix(self):
        source = Path(install.__file__).read_text(encoding='utf-8')
        self.assertNotIn('shell=True', source)
        self.assertNotIn('os.system', source)
        with patch('subprocess.call', return_value=0) as call:
            code = install.Host().run(['apt-get', '--simulate', 'install', 'firefox-esr'])
        self.assertEqual(code, 0)
        self.assertEqual(call.call_args.args[0], ['apt-get', '--simulate', 'install', 'firefox-esr'])
        self.assertNotEqual(call.call_args.kwargs.get('shell'), True)
        with self.assertRaises(ValueError):
            install.Host().run('apt-get install firefox-esr')
        with tempfile.TemporaryDirectory() as parent:
            outsider = Path(parent) / 'other'
            outsider.mkdir()
            install.Host().cleanup(str(outsider))
            self.assertTrue(outsider.is_dir())
            owned = Path(tempfile.mkdtemp(prefix='zell-theater-chrome-', dir=parent))
            install.Host().cleanup(str(owned))
            self.assertFalse(owned.exists())

    def test_commands_never_include_the_typed_text(self):
        host = FakeHost()
        host.typed = 'firefox-esr && sudo reboot'
        self.assertEqual(install.main(['prog', 'install-firefox-esr'], host), 1)
        flat = [item for argv in host.commands for item in argv]
        self.assertNotIn(host.typed, flat)
        self.assertNotIn('sudo', flat)
