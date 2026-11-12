#!/usr/bin/env python3
"""Operating-system tasks. Every interactive mutation runs in a private PTY.

The room passes only a catalog key, never a command string or a secret. Task
functions use argv, preview targets, and ask before changing user/system data.
"""
from __future__ import annotations
import argparse
import getpass
import json
import os
from pathlib import Path
import re
import resource
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

TASKS = {}
GROUPS = {
    'gatekeeper': 'Gatekeeper', 'signalman': 'Signalman', 'porter': 'Porter',
    'laundress': 'Laundress', 'merchant': 'Merchant', 'herald': 'Herald',
    'timekeeper': 'Timekeeper', 'sentinel': 'Sentinel', 'vault': 'Vault Keeper',
    'tinkerer': 'Tinkerer', 'admin': 'Administrator', 'housekeeper': 'Housekeeper',
    'notices': 'Noticeboard', 'wizard': 'Wizard',
}
STATIONS = dict(zip(range(32, 42), list(GROUPS)[:10]))
PACKAGES = {
    'nmcli':'network-manager', 'bluetoothctl':'bluez', 'ss':'iproute2', 'ip':'iproute2',
    'ping':'iputils-ping', 'getent':'libc-bin', 'curl':'curl', 'ssh':'openssh-client',
    'ssh-keygen':'openssh-client', 'loginctl':'systemd', 'systemctl':'systemd',
    'systemd-inhibit':'systemd', 'systemd-analyze':'systemd', 'timedatectl':'systemd',
    'upower':'upower', 'xdg-screensaver':'xdg-utils', 'xdg-mime':'xdg-utils',
    'gio':'libglib2.0-bin', 'rsync':'rsync', 'smartctl':'smartmontools',
    'udisksctl':'udisks2', 'lsblk':'util-linux', 'findmnt':'util-linux', 'mkfs.ext4':'e2fsprogs',
    'mkfs.vfat':'dosfstools', 'parted':'parted', 'nft':'nftables',
    'scrot':'scrot', 'xclip':'xclip', 'notify-send':'libnotify-bin',
    'wpctl':'wireplumber', 'pactl':'pulseaudio-utils', 'paplay':'pulseaudio-utils',
    'parecord':'pulseaudio-utils', 'lpstat':'cups-client', 'lp':'cups-client',
    'cancel':'cups-client', 'setxkbmap':'x11-xkb-utils', 'xrandr':'x11-xserver-utils',
    'sensors':'lm-sensors', 'ffplay':'ffmpeg', 'ffmpeg':'ffmpeg',
    'brightnessctl':'brightnessctl', 'powerprofilesctl':'power-profiles-daemon',
    'passwd':'passwd', 'last':'wtmpdb', 'sudo':'sudo', 'modinfo':'kmod',
    'apt-get':'apt', 'apt-cache':'apt', 'apt-mark':'apt', 'dpkg-query':'dpkg',
    'zip':'zip', 'unzip':'unzip', 'tar':'tar', 'du':'coreutils', 'df':'coreutils',
}

class Cancelled(Exception):
    pass

def task(group, key, label):
    def decorate(fn):
        TASKS[group+'.'+key] = (group, label, fn)
        return fn
    return decorate

def load_tasks():
    import zell_system, zell_files, zell_schedule, zell_devices  # noqa: F401

def entries(group):
    load_tasks()
    return [(label, key) for key, (g, label, _) in TASKS.items() if g == group]

def state_root():
    root = Path(os.environ.get('GPU_TERMINAL_HOME', Path.home()/'.local/gpu_terminal'))
    return Path(os.environ.get('KILIX_ZELL_STATE', root/'kilix-zell/state'))

def read_state(name, default=None):
    try:
        path = state_root()/name
        if path.is_symlink() or path.stat().st_size > 2**20:
            return default
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return default

def write_state(name, value):
    root = state_root()
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd, temporary = tempfile.mkstemp(prefix='.os-', dir=root)
    try:
        with os.fdopen(fd, 'w') as stream:
            json.dump(value, stream); stream.write('\n')
            stream.flush(); os.fsync(stream.fileno())
        os.replace(temporary, root/name)
    finally:
        Path(temporary).unlink(missing_ok=True)

def ask(prompt, pattern=None):
    value = input(prompt+': ').strip()
    if not value:
        raise Cancelled
    if any(ord(c)<32 for c in value) or (pattern and not re.fullmatch(pattern, value)):
        raise ValueError('Invalid value.')
    return value

def pick(title, rows):
    rows = list(rows)
    print('\n'+title)
    if not rows:
        print('Nothing found.'); raise Cancelled
    for i, (label, _) in enumerate(rows, 1):
        print(f'{i:3}  {label}')
    index = int(ask('Number (Enter cancels)', r'[0-9]+'))-1
    if index not in range(len(rows)):
        raise ValueError('Choose a listed number.')
    return rows[index][1]

def confirm(description, word='yes'):
    print('\n'+str(description))
    if input(f'Type {word} to continue (Enter cancels): ').strip() != word:
        raise Cancelled

def tool(name):
    found = shutil.which(name)
    if not found and '/' not in name:
        for root in ('/usr/sbin', '/sbin'):
            p = Path(root)/name
            if p.is_file() and os.access(p, os.X_OK):
                found = str(p); break
    if not found:
        raise FileNotFoundError(f'{name} is missing. Install {PACKAGES.get(name, name)} using Merchant.')
    return found

def run(argv, *, capture=False, check=True, timeout=None, **kwargs):
    argv = list(map(str, argv))
    if not argv or any('\0' in arg for arg in argv):
        raise ValueError('Invalid command arguments.')
    argv[0] = tool(argv[0])
    if Path(argv[0]).name == 'sudo' and len(argv)>1:
        argv[1] = tool(argv[1])
    # No shell=True anywhere in the task execution path.
    result = subprocess.run(argv, capture_output=capture, text=True,
                            timeout=timeout if timeout is not None else (30 if capture else None), **kwargs)
    if check and result.returncode:
        # Do not include arbitrary stderr (which may contain a secret) in logs.
        raise RuntimeError(f'{Path(argv[0]).name} exited with status {result.returncode}.')
    return result.stdout if capture else result.returncode

def path_input(prompt, *, exists=True, directory=False):
    path = Path(ask(prompt)).expanduser().absolute()
    if exists and (not path.exists() or (directory and not path.is_dir())):
        raise ValueError('Choose an existing '+('folder.' if directory else 'path.'))
    return path

def positive(prompt, maximum=10**9):
    value = int(ask(prompt, r'[0-9]+'))
    if not 0 < value <= maximum:
        raise ValueError('Value outside the allowed range.')
    return value

def hub(ui, group, action=None):
    rows = entries(group)
    key = rows[action][1] if action is not None and 0 <= action < len(rows) else ui.choose(GROUPS[group], rows)
    if key is None:
        return
    from zell_terminal import terminal_loop
    return terminal_loop(ui, [sys.executable, str(Path(__file__).resolve()), '--task', key],
                         title=GROUPS[group].upper()+' / PRIVATE TASK TERMINAL', sensitive=True)

def main():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    os.umask(0o077)
    parser = argparse.ArgumentParser()
    parser.add_argument('--task')
    parser.add_argument('--scheduled')
    args = parser.parse_args()
    load_tasks()
    if args.scheduled:
        from zell_schedule import execute_schedule
        return execute_schedule(args.scheduled)
    if args.task not in TASKS:
        raise ValueError('Unknown task.')
    group, label, fn = TASKS[args.task]
    from zell_schedule import job_start, job_finish
    job = job_start(args.task, label)
    result = 'completed'
    try:
        print(GROUPS[group]+' / '+label+'\n')
        fn()
    except (Cancelled, EOFError, KeyboardInterrupt):
        result = 'cancelled'; print('\nCancelled.')
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        result = 'failed'; print('\n'+str(error))
    finally:
        job_finish(job, result)
    try:
        input('\nEnter to return to Zell... ')
    except (EOFError, KeyboardInterrupt):
        pass
    return 1 if result == 'failed' else 0

if __name__ == '__main__':
    # Decorators in sibling modules must share this registry, not import a
    # second copy of this executable as zell_os.
    sys.modules['zell_os'] = sys.modules[__name__]
    raise SystemExit(main())
