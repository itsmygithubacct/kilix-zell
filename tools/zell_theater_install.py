"""Fixed-action browser install for the private terminal.

Native launches this program with one enumerated action. It does not
accept a shell command. sudo reads the password from this process's
terminal. This program does not read or log that password.
"""
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request


ACTIONS = ('install-firefox-esr', 'install-chromium', 'install-chrome-deb')
PACKAGES = {
    'install-firefox-esr': 'firefox-esr',
    'install-chromium': 'chromium',
}
BINARIES = {
    'install-firefox-esr': ('firefox-esr',),
    'install-chromium': ('chromium', 'chromium-browser'),
    'install-chrome-deb': ('google-chrome-stable', 'google-chrome'),
}
CHROME_DEB_NAME = 'google-chrome-stable_current_amd64.deb'
CHROME_DEB_URL = 'https://dl.google.com/linux/direct/google-chrome-stable_current_amd64.deb'
CHROME_PACKAGE = 'google-chrome-stable'
CHROME_ARCH = 'amd64'
_DOWNLOAD_LIMIT = 256 * 1024 * 1024


def accept_install(typed):
    """The only accepted confirmation. It is never copied into a command."""
    return isinstance(typed, str) and typed.strip() == 'install'


def parse_deb_fields(text):
    """Read Package and Architecture from dpkg-deb -f output."""
    lines = [line.strip() for line in str(text).splitlines() if line.strip()]
    if len(lines) == 2 and ':' not in lines[0] and ':' not in lines[1]:
        return {'package': lines[0], 'architecture': lines[1]}
    fields = {}
    for line in lines:
        if ':' not in line:
            continue
        key, value = line.split(':', 1)
        fields[key.strip().lower()] = value.strip()
    package = fields.get('package')
    architecture = fields.get('architecture')
    if not package or not architecture:
        raise ValueError('package metadata is unreadable')
    return {'package': package, 'architecture': architecture}


def _present(action, which):
    return any(which(name) for name in BINARIES[action])


def _run(host, argv):
    try:
        code = host.run(list(argv))
    except OSError:
        return 1
    if isinstance(code, bool) or not isinstance(code, int):
        return 1
    return code


class _ChromeRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not str(newurl).startswith('https://dl.google.com/'):
            raise ValueError('refusing a chrome download redirect')
        return super().redirect_request(req, fp, code, msg, headers, newurl)


class Host:
    """Real apt, download, and confirmation. Tests replace these methods."""

    def which(self, name):
        return shutil.which(name)

    def run(self, argv):
        if not isinstance(argv, list) or not argv:
            raise ValueError('bad command')
        if any(not isinstance(item, str) or not item or '\n' in item or '\x00' in item for item in argv):
            raise ValueError('bad command')
        return subprocess.call(argv)

    def confirm(self):
        print('Type install to continue (Enter cancels): ', end='', flush=True)
        try:
            typed = sys.stdin.readline()
        except EOFError:
            return False
        return accept_install(typed)

    def download(self, url, dest):
        if url != CHROME_DEB_URL:
            raise ValueError('refusing an unofficial chrome package')
        opener = urllib.request.build_opener(_ChromeRedirect)
        remaining = _DOWNLOAD_LIMIT
        with opener.open(url, timeout=120) as response, open(dest, 'wb') as handle:
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                remaining -= len(chunk)
                if remaining < 0:
                    raise ValueError('chrome package is too large')
                handle.write(chunk)
        os.chmod(dest, 0o600)

    def deb_fields(self, path):
        result = subprocess.run(
            ['dpkg-deb', '-f', path, 'Package', 'Architecture'],
            capture_output=True, text=True, check=False,
        )
        if result.returncode != 0:
            raise ValueError('package metadata is unreadable')
        return parse_deb_fields(result.stdout)

    def private_dir(self):
        path = tempfile.mkdtemp(prefix='zell-theater-chrome-')
        os.chmod(path, 0o700)
        return path

    def cleanup(self, path):
        if not path or not os.path.basename(path).startswith('zell-theater-chrome-'):
            return
        try:
            info = os.lstat(path)
        except OSError:
            return
        if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
            return
        if info.st_uid != os.geteuid():
            return
        shutil.rmtree(path, ignore_errors=True)


def _preview_and_install(host, preview, command):
    print('Previewing the package change.', flush=True)
    if _run(host, preview) != 0:
        print('Package preview failed.', flush=True)
        return 1
    if not host.confirm():
        print('Install cancelled.', flush=True)
        return 1
    print('Installing. Type the sudo password in this terminal if asked.', flush=True)
    if _run(host, command) != 0:
        print('Install failed.', flush=True)
        return 1
    return 0


def install_package(action, host):
    package = PACKAGES[action]
    code = _preview_and_install(
        host,
        ['apt-get', '--simulate', 'install', package],
        ['sudo', 'apt-get', 'install', '-y', package],
    )
    if code != 0:
        return code
    if not _present(action, host.which):
        print('The selected browser is still missing.', flush=True)
        return 1
    print('Installed.', flush=True)
    return 0


def install_chrome(host):
    directory = host.private_dir()
    try:
        dest = os.path.join(directory, CHROME_DEB_NAME)
        if os.path.basename(dest) != CHROME_DEB_NAME:
            print('Chrome package path is not the official deb.', flush=True)
            return 1
        try:
            host.download(CHROME_DEB_URL, dest)
        except (OSError, ValueError, urllib.error.URLError):
            print('Chrome download failed.', flush=True)
            return 1
        if os.path.islink(dest) or not os.path.isfile(dest):
            print('Chrome package was not downloaded.', flush=True)
            return 1
        try:
            fields = host.deb_fields(dest)
        except (OSError, ValueError, subprocess.SubprocessError):
            print('Chrome package metadata is unreadable.', flush=True)
            return 1
        if not isinstance(fields, dict):
            print('Chrome package metadata is unreadable.', flush=True)
            return 1
        if fields.get('package') != CHROME_PACKAGE or fields.get('architecture') != CHROME_ARCH:
            print('The downloaded package is not official Chrome for amd64.', flush=True)
            return 1
        code = _preview_and_install(
            host,
            ['apt-get', '--simulate', 'install', dest],
            ['sudo', 'apt-get', 'install', '-y', dest],
        )
        if code != 0:
            return code
        if not _present('install-chrome-deb', host.which):
            print('The selected browser is still missing.', flush=True)
            return 1
        print('Installed.', flush=True)
        return 0
    finally:
        host.cleanup(directory)


def perform(action, host):
    if action not in ACTIONS:
        return 2
    if _present(action, host.which):
        print('The selected browser is already installed.', flush=True)
        return 0
    if action == 'install-chrome-deb':
        return install_chrome(host)
    return install_package(action, host)


def main(argv=None, host=None):
    argv = list(sys.argv if argv is None else argv)
    if len(argv) != 2 or argv[1] not in ACTIONS:
        print(
            'usage: zell_theater_install.py install-firefox-esr|install-chromium|install-chrome-deb',
            file=sys.stderr, flush=True,
        )
        return 2
    host = host or Host()
    try:
        return perform(argv[1], host)
    except KeyboardInterrupt:
        return 1
    except (OSError, ValueError):
        print('Install failed.', flush=True)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
