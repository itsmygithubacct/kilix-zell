"""The canvas objects follow the neuronte-sdk checkout a build names.

usage: test_nsdk_ident.py NSDK_CHECKOUT MODULES

Each case builds the two canvas objects in a scratch build directory.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NSDK = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else None
MODULES = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else None
SOURCE = 'src/neuronte_sdk_canvas.c'
HEADER = 'include/neuronte_sdk_canvas.h'
HAS_CANVAS = bool(NSDK and MODULES and (NSDK / SOURCE).is_file() and (NSDK / HEADER).is_file())


def make(build, sdk, *flags):
    # A parent make's flags would hand this one its job server and its variables.
    env = {k: v for k, v in os.environ.items() if k not in ('MAKEFLAGS', 'MFLAGS', 'MAKELEVEL')}
    argv = ['make', '-s', *flags, '-C', str(ROOT), f'BUILD={build}', f'MODULES={MODULES}', f'NSDK_DIR={sdk}',
            str(build / 'canvas.o'), str(build / 'nsdk_canvas.o')]
    return subprocess.run(argv, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def copy_of(tmp, name):
    sdk = Path(tmp) / name
    for rel in (SOURCE, HEADER):
        (sdk / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(NSDK / rel, sdk / rel)
    return sdk


def age(path):
    # Older than anything built here, so only the stamp can ask for a rebuild.
    os.utime(path, (1_000_000_000, 1_000_000_000))


@unittest.skipUnless(HAS_CANVAS, 'no neuronte-sdk checkout with the canvas was named')
class SdkIdentTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='zell-nsdk-ident-')
        self.addCleanup(self.tmp.cleanup)
        self.build = Path(self.tmp.name) / 'build'
        self.objects = [self.build / 'canvas.o', self.build / 'nsdk_canvas.o']

    def built(self, sdk, *flags):
        result = make(self.build, sdk, *flags)
        self.assertEqual(result.returncode, 0, result.stdout)
        return [path.stat().st_mtime_ns for path in self.objects]

    def test_the_same_checkout_is_not_compiled_twice(self):
        first = self.built(NSDK)
        stamp = self.build / 'nsdk.ident'
        self.assertEqual(stamp.read_text().splitlines()[0], str(NSDK))
        self.assertEqual(len(stamp.read_text().splitlines()), 3)
        recorded = stamp.stat().st_mtime_ns
        self.assertEqual(self.built(NSDK), first)
        self.assertEqual(stamp.stat().st_mtime_ns, recorded)

    def test_another_checkout_compiles_both_objects_again(self):
        other = copy_of(self.tmp.name, 'other')
        with (other / SOURCE).open('a') as stream:
            stream.write('\nint zell_test_marker_from_the_other_checkout;\n')
        age(other / SOURCE)
        age(other / HEADER)
        first = self.built(NSDK)
        second = self.built(other)
        self.assertTrue(all(b != a for a, b in zip(first, second)), (first, second))
        symbols = subprocess.run(['nm', str(self.build / 'nsdk_canvas.o')], check=True,
                                 stdout=subprocess.PIPE, text=True).stdout
        self.assertIn('zell_test_marker_from_the_other_checkout', symbols)
        self.assertEqual(self.built(other), second)
        third = self.built(NSDK)
        self.assertTrue(all(b != a for a, b in zip(second, third)), (second, third))
        symbols = subprocess.run(['nm', str(self.build / 'nsdk_canvas.o')], check=True,
                                 stdout=subprocess.PIPE, text=True).stdout
        self.assertNotIn('zell_test_marker_from_the_other_checkout', symbols)

    def test_an_edit_in_place_compiles_again_whatever_its_date(self):
        sdk = copy_of(self.tmp.name, 'edited')
        first = self.built(sdk)
        with (sdk / HEADER).open('a') as stream:
            stream.write('\n/* edited */\n')
        age(sdk / HEADER)
        second = self.built(sdk)
        self.assertTrue(all(b != a for a, b in zip(first, second)), (first, second))

    def test_a_checkout_that_was_removed_does_not_stop_the_next_build(self):
        gone = copy_of(self.tmp.name, 'gone')
        first = self.built(gone)
        shutil.rmtree(gone)
        # A build made before the stamp left a dependency file that names
        # its checkout's source. It must not be read.
        (self.build / 'nsdk_canvas.d').write_text(
            f'{self.build}/nsdk_canvas.o: {gone}/{SOURCE} {gone}/{HEADER}\n{gone}/{HEADER}:\n')
        second = self.built(NSDK)
        self.assertTrue(all(b != a for a, b in zip(first, second)), (first, second))

    def test_the_sdk_object_leaves_no_dependency_file(self):
        self.built(NSDK)
        self.assertTrue((self.build / 'canvas.d').is_file())
        self.assertFalse((self.build / 'nsdk_canvas.d').exists())

    def test_always_make_builds_a_good_checkout(self):
        self.built(NSDK)
        self.built(NSDK, '-B')
        self.built(NSDK, '-B', '-j4')

    def test_a_checkout_with_no_canvas_is_turned_away_once(self):
        bare = Path(self.tmp.name) / 'bare'
        (bare / 'include').mkdir(parents=True)
        for sdk in (bare, Path(self.tmp.name) / 'missing'):
            for flags in ((), ('-j4',)):
                with self.subTest(sdk=sdk.name, flags=flags):
                    result = make(self.build, sdk, *flags)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(result.stdout.count('has no canvas'), 1, result.stdout)
                    self.assertIn(str(sdk), result.stdout)
                    self.assertFalse((self.build / 'nsdk_canvas.o').exists())

    def test_half_a_canvas_is_turned_away(self):
        half = copy_of(self.tmp.name, 'half')
        (half / SOURCE).unlink()
        result = make(self.build, half)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout.count('has no canvas'), 1, result.stdout)


if __name__ == '__main__':
    unittest.main()
