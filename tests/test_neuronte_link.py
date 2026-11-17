"""The Neuronte programs hold no soft-raster code.

usage: test_neuronte_link.py PROGRAM...

Every function soft-raster exports starts with sr_ and its font tables with
font, and a linked object brings all of its symbols, so one such name in a
program means the library is back in its link. Each program must also show
Zell's own canvas, which proves that its symbols can be read at all.

The canvas is neuronte-sdk's, reached through src/canvas.c. Each program
must hold that canvas and nothing else of the SDK: its cell surface opens
libneuronte for itself, which Zell does not use.

This reads names. The SDK's canvas is adapted from soft-raster under other
names, and tests/test_canvas.c is what holds it to the same results. The
last test reads the adapter's text for the plain ways of doing pixel work
there; it cannot see every way.
"""
from pathlib import Path
import re
import subprocess
import sys
import unittest

PROGRAMS = [Path(arg) for arg in sys.argv[1:]]
del sys.argv[1:]

CANVAS = {'zell_canvas_init', 'zell_canvas_free'}
SDK_CANVAS = {
    'nsdk_canvas_init', 'nsdk_canvas_wrap', 'nsdk_canvas_free', 'nsdk_canvas_set_clip',
    'nsdk_canvas_reset_clip', 'nsdk_canvas_clear', 'nsdk_canvas_fill_rect',
    'nsdk_canvas_pack_rgba', 'nsdk_canvas_write_ppm'}


def symbols(program):
    listing = subprocess.run(['nm', str(program)], check=True, stdout=subprocess.PIPE, text=True).stdout
    return {line.split()[-1] for line in listing.splitlines() if line.split()}


class NeuronteLinkTests(unittest.TestCase):
    def test_programs_were_named(self):
        self.assertGreaterEqual(len(PROGRAMS), 6, PROGRAMS)
        self.assertIn('zell-neuronte', {program.name for program in PROGRAMS})
        self.assertIn('snap-neuronte', {program.name for program in PROGRAMS})

    def test_no_program_holds_soft_raster(self):
        for program in PROGRAMS:
            with self.subTest(program=program.name):
                names = symbols(program)
                self.assertLessEqual(CANVAS, names, 'the canvas symbols cannot be read')
                found = sorted(name for name in names if re.match(r'sr_[a-z]', name) or name.startswith('font'))
                self.assertEqual(found, [])

    def test_the_canvas_draws_only_what_the_header_lists(self):
        header = (Path(__file__).resolve().parents[1] / 'src' / 'canvas.h').read_text()
        declared = set(re.findall(r'\b(zell_canvas_[a-z_]+)\(', header))
        self.assertEqual(declared, {
            'zell_canvas_init', 'zell_canvas_wrap', 'zell_canvas_free', 'zell_canvas_set_clip',
            'zell_canvas_reset_clip', 'zell_canvas_clear', 'zell_canvas_fill_rect',
            'zell_canvas_pack_rgba', 'zell_canvas_write_ppm'})
        for program in PROGRAMS:
            with self.subTest(program=program.name):
                self.assertLessEqual({name for name in symbols(program) if name.startswith('zell_canvas_')}, declared)

    def test_the_canvas_is_the_sdk_one_and_no_more_of_the_sdk(self):
        for program in PROGRAMS:
            with self.subTest(program=program.name):
                self.assertEqual({name for name in symbols(program) if name.startswith('nsdk_')}, SDK_CANVAS)

    def test_the_adapter_does_no_pixel_work(self):
        source = (Path(__file__).resolve().parents[1] / 'src' / 'canvas.c').read_text()
        code = re.sub(r'/\*.*?\*/', '', source, flags=re.S)
        # It passes the buffer on. It never indexes or walks it, loops,
        # allocates or opens a file.
        for word in ('[', '++', '--', 'for (', 'while (', 'memset', 'memcpy', 'malloc', 'calloc', 'free(',
                     'fopen', 'fwrite', '>>', '<<', 'math.h', 'soft_raster.c'):
            self.assertNotIn(word, code.replace('nsdk_canvas_free(', '').replace('zell_canvas_free(', ''), word)
        for name in SDK_CANVAS:
            self.assertEqual(code.count(name + '('), 1, name)


if __name__ == '__main__':
    unittest.main()
