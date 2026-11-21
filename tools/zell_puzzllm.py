"""Resolve and build the PuzzLLM played through the Workshop hatch."""
import fcntl
import os
from pathlib import Path
import re
import subprocess
import tempfile


REPOSITORY = 'https://github.com/itsmygithubacct/kilix-games.git'
BRANCH = 'work/puzzllm'


def command(*args, cwd=None, timeout=None):
    # The station's stdout carries dialogue frames. Keep build output off it.
    result = subprocess.run(args, cwd=cwd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, errors='replace', timeout=timeout,
                            env={**os.environ, 'GIT_TERMINAL_PROMPT': '0'})
    if result.returncode:
        raise RuntimeError((result.stderr or result.stdout).strip()[-1600:] or f'{args[0]} exited {result.returncode}')
    return result.stdout


def development_game():
    sources = [Path.home()/'gpu_terminal']
    if os.environ.get('GPU_TERMINAL_SOURCE_HOME'):
        sources.insert(0, Path(os.environ['GPU_TERMINAL_SOURCE_HOME']).expanduser())
    for base in dict.fromkeys(sources):
        for repo in (base/'games/kilix-games', base/'kilix-games'):
            if not (repo/'.git').exists():
                continue
            try:
                records = command('git', 'worktree', 'list', '--porcelain', '-z', cwd=repo)
            except (OSError, RuntimeError):
                continue
            for record in records.split('\0\0'):
                fields = record.split('\0')
                if 'branch refs/heads/'+BRANCH in fields:
                    game = Path(fields[0].removeprefix('worktree '))/'puzzllm'
                    if (game/'bin/puzzllm').is_file():
                        return game
    return None


def published_revision():
    ref = 'refs/heads/'+BRANCH
    fields = command('git', 'ls-remote', '--exit-code', '--heads', REPOSITORY, ref, timeout=10).split()
    if len(fields) != 2 or fields[1] != ref or not re.fullmatch('[0-9a-f]{40}', fields[0]):
        raise RuntimeError(f'Could not resolve {ref}')
    return fields[0]


def download(cache, revision):
    with tempfile.TemporaryDirectory(dir=cache) as temporary:
        tree = Path(temporary)/'source'
        command('git', 'init', '--quiet', str(tree))
        command('git', 'remote', 'add', 'origin', REPOSITORY, cwd=tree)
        command('git', 'fetch', '--quiet', '--depth=1', '--filter=blob:none', '--no-tags', 'origin', revision, cwd=tree)
        command('git', 'sparse-checkout', 'set', 'puzzllm', 'third_party', cwd=tree)
        command('git', 'checkout', '--quiet', '--detach', 'FETCH_HEAD', cwd=tree)
        command('git', 'submodule', 'update', '--init', '--recursive', '--depth=1', cwd=tree)
        tree.rename(cache/revision)


def build(game):
    launcher = game/'bin/puzzllm'
    if not os.access(launcher, os.X_OK) or not (game/'Makefile').is_file():
        raise RuntimeError(f'Expected a PuzzLLM source checkout in {game}')
    command('make', '--no-print-directory', 'all', cwd=game)
    return launcher


def launcher():
    cache_home = Path(os.environ.get('XDG_CACHE_HOME') or Path.home()/'.cache').expanduser()
    cache = cache_home.resolve()/'kilix-zell/puzzllm'
    cache.mkdir(parents=True, exist_ok=True, mode=0o700)
    with (cache/'build.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if os.environ.get('PUZZLLM_ROOT'):
            return build(Path(os.environ['PUZZLLM_ROOT']).expanduser().resolve())
        local = development_game()
        current = cache/'current'
        try:
            revision = published_revision()
        except (OSError, RuntimeError, subprocess.TimeoutExpired):
            if not local and not current.is_dir():
                raise
            print('Could not check for a PuzzLLM update. Opening an available build.', flush=True)
            revision = current.resolve().name if current.is_dir() else None
        if local and (revision is None or subprocess.run(
                ['git', 'merge-base', '--is-ancestor', revision, 'HEAD'], cwd=local,
                stderr=subprocess.DEVNULL).returncode == 0):
            return build(local)
        tree = cache/revision
        if not tree.is_dir():
            print('Downloading PuzzLLM and its game libraries…', flush=True)
            download(cache, revision)
        executable = build(tree/'puzzllm')
        # Keep the last working revision available offline. A running game
        # continues to read its own scripts and art when another tab updates.
        link = cache/'current.next'
        link.unlink(missing_ok=True)
        link.symlink_to(revision)
        link.replace(current)
        return executable
