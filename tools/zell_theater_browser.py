"""Selected browser on a Kilix private display. No external tab."""
import os
import shutil

BROWSERS = ('chrome', 'chromium', 'firefox-esr')
CHROME_DEB_NAME = 'google-chrome-stable_current_amd64.deb'


def launch_argv(browser, which=None):
    """Argv for one selected browser. Never substitutes another browser."""
    find = which or shutil.which
    if browser == 'chromium':
        binary = find('chromium') or find('chromium-browser')
        if binary is None:
            raise FileNotFoundError('chromium is not installed')
        return [binary]
    if browser == 'chrome':
        binary = find('google-chrome-stable') or find('google-chrome')
        if binary is None:
            raise FileNotFoundError('chrome is not installed')
        return [binary]
    if browser == 'firefox-esr':
        binary = find('firefox-esr')
        if binary is None:
            raise FileNotFoundError('firefox-esr is not installed')
        return [binary]
    raise ValueError('unknown browser')


def chrome_privilege_argv(deb_path):
    """Install the official local deb. Not the bare apt package name."""
    return ['apt-get', 'install', '-y', os.fspath(deb_path)]


def firefox_privilege_argv():
    return ['apt-get', 'install', '-y', 'firefox-esr']


class PrivateBrowser:
    """One XAppSession for the active url. Popout keeps this object."""

    def __init__(self, session_type=None, privilege=None, which=None):
        self.session_type = session_type
        self.privilege = privilege
        self.which = which
        self.session = None
        self.browser = None
        self.url = None
        self.popped = False
        self.launches = []

    def launch(self, browser, url, width, height):
        if self.session_type is None:
            from kilix_sdk.xapp import XAppSession
            self.session_type = XAppSession
        self.close()
        session = self.session_type(f'zell-theater-{os.getpid()}', int(width), int(height), 12)
        try:
            session.start_xvfb(nocursor=True)
            argv = launch_argv(browser, self.which) + ['--new-window', url]
            self.launches.append(argv)
            session.launch_app(argv, isolate_bus=True, clipboard=False)
            session.make_injector()
            session.start_capture(draw_cursor=False)
        except Exception:
            try:
                session.close()
            except Exception:
                pass
            raise
        self.session = session
        self.browser = browser
        self.url = url
        self.popped = False
        return session

    def popout(self):
        if self.session is None:
            raise RuntimeError('no browser session')
        self.popped = True

    def dock(self):
        if self.session is None:
            raise RuntimeError('no browser session')
        self.popped = False

    def pointer(self, x, y, button, action):
        if self.session is None:
            raise RuntimeError('no browser session')
        injector = self.session.make_injector()
        if action == 'move':
            injector.move_click(int(x), int(y), 0, None)
        elif action == 'down':
            injector.move_click(int(x), int(y), int(button or 1), True)
        elif action == 'up':
            injector.move_click(int(x), int(y), int(button or 1), False)
        else:
            raise ValueError('bad pointer action')

    def close(self):
        session = self.session
        self.session = None
        self.browser = None
        self.url = None
        self.popped = False
        if session is not None:
            session.close()
