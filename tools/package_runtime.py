#!/usr/bin/env python3
"""Build Zell's relocatable runtime; never copy user state or vault records."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def copy(source, target, executable=False):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    target.chmod(0o755 if executable else 0o644)


def build(dest, modules, neuronte, ghostty):
    display = modules/'neuronte-display'
    secrets = modules/'kilix-secrets'
    copy(ROOT/'zell-neuronte', dest/'zell-neuronte', True)
    copy(ROOT/'bin/zell', dest/'bin/zell', True)
    copy(ROOT/'VERSION', dest/'VERSION')
    for script in sorted((ROOT/'tools').glob('*.py')):
        if script.name not in {'compile_desktop.py', 'package_runtime.py'}:
            copy(script, dest/'tools'/script.name)
    copy(neuronte/'weights/neuronte-split-v4.nrt', dest/'weights/neuronte-split-v4.nrt')
    copy(neuronte/'build/libneuronte.so', dest/'lib/libneuronte.so', True)
    copy(ghostty/'lib/libghostty-vt.so.0', dest/'lib/libghostty-vt.so.0', True)
    copy(display/'neuronte_display.py', dest/'libexec/neuronte-display/neuronte_display.py')
    native = dest/'libexec/neuronte-display/build/libneuronte-display.so'
    native.parent.mkdir(parents=True, exist_ok=True)
    # The presenter links the same sources with an install-relative RUNPATH.
    subprocess.run([os.environ.get('CC', 'cc'), '-O2', '-std=c11', '-shared', '-fPIC', '-pthread',
        '-D_POSIX_C_SOURCE=200809L', '-D_DEFAULT_SOURCE', str(display/'native.c'),
        str(modules/'kitty-framebuffer/src/kitty_framebuffer.c'),
        '-I'+str(modules/'kitty-framebuffer/include'), '-I'+str(ghostty/'include'),
        '-L'+str(ghostty/'lib'), '-Wl,-rpath,$ORIGIN/../../../lib', '-lghostty-vt', '-lz', '-o', str(native)], check=True)
    native.chmod(0o755)
    for name in ('kilix-secrets', 'kilix-secretsd'):
        copy(secrets/'build'/name, dest/'libexec'/name, True)
    for name in ('kilix-secrets.service', 'kilix-secrets.socket'):
        copy(secrets/'service'/name, dest/'service'/name)
    for name, source in [('zell', ROOT), ('neuronte', neuronte), ('ghostty', ghostty),
        ('neuronte-display', display), ('kilix-secrets', secrets),
        ('kitty-framebuffer', modules/'kitty-framebuffer'), ('soft-raster', modules/'soft-raster'),
        ('kitty-keyboard', modules/'kitty-input/third_party/kitty_keyboard')]:
        copy(source/'LICENSE', dest/'licenses'/name)
    copy(secrets/'THIRD_PARTY_NOTICES.md', dest/'licenses/kilix-secrets-notices')
    for path in [dest/'zell-neuronte', *sorted((dest/'lib').glob('*')), native,
                 dest/'libexec/kilix-secrets', dest/'libexec/kilix-secretsd']:
        subprocess.run(['strip', '--strip-unneeded', str(path)], check=True)
    files = {}
    for path in sorted(dest.rglob('*')):
        if path.is_file():
            files[path.relative_to(dest).as_posix()] = dict(
                sha256=hashlib.sha256(path.read_bytes()).hexdigest(), bytes=path.stat().st_size,
                mode=path.stat().st_mode & 0o777)
    manifest = dict(schema='kilix.zell.runtime/v1', version=(ROOT/'VERSION').read_text().strip(),
                    architecture=platform.machine(), files=files)
    (dest/'runtime.json').write_text(json.dumps(manifest, indent=2, sort_keys=True)+'\n')
    (dest/'runtime.json').chmod(0o644)


def install_service(app, unit_dir):
    unit_dir.mkdir(parents=True, exist_ok=True)
    service = (app/'service/kilix-secrets.service').read_text()
    daemon = str(app/'libexec/kilix-secretsd').replace('%', '%%').replace('\\', '\\\\').replace('"', '\\"')
    service = service.replace('/usr/local/bin/kilix-secretsd', '"'+daemon+'"')
    (unit_dir/'kilix-secrets.service').write_text(service)
    (unit_dir/'kilix-secrets.service').chmod(0o644)
    copy(app/'service/kilix-secrets.socket', unit_dir/'kilix-secrets.socket')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--modules', type=Path, default=ROOT.parents[1]/'kilix-modules')
    parser.add_argument('--neuronte', type=Path, default=Path.home()/'projects/neuronte')
    parser.add_argument('--ghostty', type=Path, default=Path.home()/'projects/batty/build/ghostty')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--install', type=Path)
    parser.add_argument('--bin-dir', type=Path)
    parser.add_argument('--user-unit-dir', type=Path)
    args = parser.parse_args()
    if not args.output and not args.install:
        parser.error('--output or --install is required')
    ROOT.joinpath('build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='runtime-', dir=ROOT/'build') as temporary:
        dest = Path(temporary)/'zell'
        build(dest, args.modules.resolve(), args.neuronte.resolve(), args.ghostty.resolve())
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            with tarfile.open(args.output, 'w:xz') as archive:
                for path in sorted(dest.rglob('*')):
                    if not path.is_file():
                        continue
                    info = archive.gettarinfo(path, arcname=path.relative_to(dest).as_posix())
                    info.uid = info.gid = info.mtime = 0
                    info.uname = info.gname = ''
                    with path.open('rb') as stream:
                        archive.addfile(info, stream)
            print('Runtime:', args.output)
            print('SHA-256:', hashlib.sha256(args.output.read_bytes()).hexdigest())
        if args.install:
            app = args.install.resolve()
            app.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix='.zell-install-', dir=app.parent) as stage:
                staged = Path(stage)/'app'
                shutil.copytree(dest, staged)
                if app.is_symlink():
                    raise ValueError('Refusing a symlink install destination')
                if app.exists():
                    app.rename(Path(stage)/'previous')
                staged.rename(app)
            if args.bin_dir:
                copy(app/'bin/zell', args.bin_dir/'zell', True)
            if args.user_unit_dir:
                install_service(app, args.user_unit_dir)
            print('Installed:', app)


if __name__ == '__main__':
    main()
