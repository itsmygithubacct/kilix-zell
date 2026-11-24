"""Run the maintenance action selected in Zell's Updates panel inside its own PTY."""
import os
from pathlib import Path
import subprocess
import sys

COMMANDS = {
    'clean': ['sudo', 'apt', 'clean'],
    'autoclean': ['sudo', 'apt', 'autoclean'],
    'rotate': ['sudo', 'logrotate', '/etc/logrotate.conf'],
}


def cache_browser():
    root = Path(os.environ.get('XDG_CACHE_HOME') or Path.home()/'.cache').resolve()
    current = root
    while True:
        print(f'\n{current}', flush=True)
        folders = []
        try:
            entries = sorted(current.iterdir(), key=lambda p: p.name.casefold())
            for item in entries:
                if item.is_dir() and not item.is_symlink():
                    folders.append(item)
                    print(f'{len(folders):3}  {item.name}/')
                else:
                    print(f'     {item.name}  ({item.lstat().st_size:,} bytes)')
        except OSError as error:
            print(error)
        choice = input('Folder number, .. to go back, or q to return: ').strip()
        if choice.lower()=='q':
            return 0
        if choice=='..':
            current = root if current==root else current.parent
        elif choice.isdecimal() and 1<=int(choice)<=len(folders):
            target=folders[int(choice)-1].resolve()
            if target.is_relative_to(root):
                current=target


def main(action):
    if action=='upgrade':
        from zell_update import main as upgrade
        return upgrade(pause=False)
    if action=='cache':
        return cache_browser()
    command=COMMANDS[action]
    print('$ '+' '.join(command),flush=True)
    return subprocess.call(command)


if __name__=='__main__':
    try:
        result=main(sys.argv[1])
        print(f'\nFinished (exit {result}).',flush=True)
    except (OSError, EOFError, KeyboardInterrupt) as error:
        print(f'\nStopped. {error}',flush=True)
    shell=os.environ.get('SHELL') or '/bin/sh'
    if not os.path.isabs(shell) or not os.access(shell,os.X_OK):
        shell='/bin/sh'
    os.execl(shell,Path(shell).name,'-i')
