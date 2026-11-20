"""Building against another Neuronte checkout compiles the Neuronte objects again.

usage: test_neuronte_ident.py NEURONTE_CHECKOUT
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NEURONTE = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else None


def make(build, target, **variables):
    # A parent make's flags would hand this one its job server and its variables.
    env = {k: v for k, v in os.environ.items() if k not in ('MAKEFLAGS', 'MFLAGS', 'MAKELEVEL')}
    argv = ['make', '-s', '-C', str(ROOT), f'BUILD={build}']
    argv += [f'{name}={value}' for name, value in variables.items()]
    result = subprocess.run(argv + [str(target)], env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        raise AssertionError(result.stdout)


class NeuronteIdentTests(unittest.TestCase):
    def test_every_neuronte_object_follows_the_recorded_checkout(self):
        text = (ROOT / 'Makefile').read_text()
        compiled = set(re.findall(r'^(\$\(BUILD\)/\S+\.o):[^\n]*\n\t[^\n]*\$\(N_CPPFLAGS\)', text, re.M))
        block = re.search(r'^N_OBJS := \\\n((?:\t\S+(?: \\)?\n)+)', text, re.M)
        self.assertIsNotNone(block)
        listed = set(re.findall(r'\$\(BUILD\)/\S+\.o', block.group(1)))
        self.assertTrue(compiled)
        self.assertEqual(compiled, listed)
        self.assertRegex(text, r'(?m)^\$\(N_OBJS\): \$\(N_STAMP\)$')

    @unittest.skipUnless(NEURONTE and (NEURONTE / 'include/neuronte.h').is_file(),
                         'no Neuronte checkout was named')
    def test_another_checkout_or_weights_file_compiles_again(self):
        with tempfile.TemporaryDirectory(prefix='zell-ident-') as tmp:
            build = Path(tmp) / 'build'
            alias = Path(tmp) / 'neuronte-alias'
            alias.symlink_to(NEURONTE, target_is_directory=True)
            obj = build / 'draw_neuronte.o'
            ident = build / 'neuronte.ident'
            display = Path(tmp) / 'custom display'

            def built(**variables):
                variables.setdefault('DISPLAY_DIR', display)
                make(build, obj, **variables)
                return obj.stat().st_mtime_ns

            # The default weights path follows the checkout, so each step
            # names both and changes one.
            weights = Path(tmp) / 'first.nrt'
            first = built(NEURONTE=NEURONTE, NEURONTE_WEIGHTS=weights)
            self.assertEqual(ident.read_text(), f'{NEURONTE} {weights} {display}/neuronte_display.py\n')
            recorded = ident.stat().st_mtime_ns
            self.assertEqual(built(NEURONTE=NEURONTE, NEURONTE_WEIGHTS=weights), first)
            self.assertEqual(ident.stat().st_mtime_ns, recorded)

            second = built(NEURONTE=alias, NEURONTE_WEIGHTS=weights)
            self.assertNotEqual(second, first)
            self.assertEqual(ident.read_text(), f'{alias} {weights} {display}/neuronte_display.py\n')
            self.assertEqual(built(NEURONTE=alias, NEURONTE_WEIGHTS=weights), second)

            other = Path('relative weights') / 'other.nrt'
            third = built(NEURONTE=alias, NEURONTE_WEIGHTS=other)
            self.assertNotEqual(third, second)
            self.assertEqual(ident.read_text(), f'{alias} {ROOT / other} {display}/neuronte_display.py\n')
            self.assertEqual(built(NEURONTE=alias, NEURONTE_WEIGHTS=other), third)
            self.assertNotEqual(built(NEURONTE=alias, NEURONTE_WEIGHTS=other, DISPLAY_DIR=display/'new'), third)


if __name__ == '__main__':
    unittest.main()
