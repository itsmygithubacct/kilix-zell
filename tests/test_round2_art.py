"""Delivered round-two sheets keep their measured bytes and cell anchors.

The compiler may clear sub-visible alpha. It must not crop these sheets to
independent bounds or rewrite the source PNGs.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'assets/v0.1.1'
MANIFEST = ASSETS / 'round2-art-manifest.json'
COMPILER = ROOT / 'tools/compile_desktop.py'


def sha256(path):
    digest = hashlib.sha256()
    digest.update(path.read_bytes())
    return digest.hexdigest()


def load_compiler():
    spec = importlib.util.spec_from_file_location('compile_desktop', COMPILER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Round2ArtTests(unittest.TestCase):
    def test_integrated_pngs_match_the_delivered_manifest(self):
        manifest = json.loads(MANIFEST.read_text())
        self.assertEqual(len(manifest['assets']), 4)
        for asset in manifest['assets']:
            path = ASSETS / asset['file']
            self.assertTrue(path.is_file(), asset['file'])
            self.assertEqual(sha256(path), asset['sha256'])
            for prompt in asset['prompt_files']:
                self.assertTrue((ASSETS / prompt).is_file(), prompt)

    def test_compiler_keeps_shared_anchors_and_clears_monitor_glass(self):
        manifest = json.loads(MANIFEST.read_text())
        compiler = load_compiler()
        images = compiler.load_images()
        offset = 0
        by_name = {}
        for name, sheet in compiler.SHEETS:
            by_name[name] = images[offset:offset + len(sheet)]
            offset += len(sheet)
        self.assertEqual(offset, len(images))
        expected = {
            'floor_panels': (384, 512),
            'monitor_frames': (768, 1024),
            'mana_button': (512, 512),
            'workshop_stool': (1536, 1024),
        }
        for name, size in expected.items():
            cells = by_name[name]
            self.assertGreater(len(cells), 0)
            for cell in cells:
                self.assertEqual(cell.size, size)
        # The stool stays on the room canvas. A bbox crop would be far smaller.
        self.assertGreater(by_name['workshop_stool'][0].getchannel('A').getextrema()[1], 32)
        monitors = {asset['file']: asset for asset in manifest['assets']}
        sheet = monitors['command-monitor-frames.png']
        columns, rows = sheet['columns'], sheet['rows']
        width, height = sheet['dimensions']
        cell_w, cell_h = width // columns, height // rows
        compiled = by_name['monitor_frames']
        self.assertEqual(len(compiled), columns * rows)
        for rect in sheet['inner_glass_rectangles']:
            x0, y0, x1, y1 = rect
            col, row = x0 // cell_w, y0 // cell_h
            self.assertEqual(col, (x1 - 1) // cell_w)
            self.assertEqual(row, (y1 - 1) // cell_h)
            local = compiled[row * columns + col].crop((
                x0 - col * cell_w, y0 - row * cell_h,
                x1 - col * cell_w, y1 - row * cell_h))
            self.assertEqual(local.getchannel('A').getextrema(), (0, 0))


if __name__ == '__main__':
    unittest.main()
