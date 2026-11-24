"""Exercise the installer and launcher using a small packaged game."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import package_runtime


class InstallTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.payload = self.root/'payload'
        (self.payload/'bin').mkdir(parents=True)
        shutil.copy2(ROOT/'bin/zell', self.payload/'bin/zell')
        weights = self.payload/'weights/neuronte-split-v9.nrt'
        weights.parent.mkdir()
        weights.touch()
        (self.payload/'runtime.json').write_text(json.dumps({'schema': 'kilix.zell.runtime/v1'}))
        binary = self.payload/'zell-neuronte'
        binary.write_text('#!/bin/sh\nprintf "%s\\n" "$ZELL_NEURONTE_WEIGHTS" "$@"\n')
        binary.chmod(0o755)
        self.modules = self.root/'modules'
        for name, content in [('neuronte-sdk/src/neuronte_sdk_canvas.c', ''),
                              ('neuronte-display/native.c', 'nd_scroll'),
                              ('kitty-framebuffer/include/kitty_framebuffer.h', 'kittyfb_write_control')]:
            path = self.modules/name
            path.parent.mkdir(parents=True)
            path.write_text(content)

    def install(self, app, bin_dir):
        args = ['package_runtime.py', '--modules', str(self.modules), '--install', str(app),
                '--bin-dir', str(bin_dir)]
        with patch.object(sys, 'argv', args), patch.object(package_runtime, 'ROOT', self.root), \
                patch.object(package_runtime, 'build', side_effect=lambda dest, *_: shutil.copytree(self.payload, dest)):
            package_runtime.main()

    def test_custom_install_launches_after_staging_tree_moves(self):
        stage = self.root/'stage'
        app = stage/'share/game files'
        bin_dir = stage/'commands'
        self.install(app, bin_dir)
        (self.payload/'release').write_text('updated')
        self.install(app, bin_dir)
        moved = self.root/'moved'
        stage.rename(moved)
        game = moved/'share/game files'
        self.assertEqual((game/'release').read_text(), 'updated')
        env = {'HOME': str(self.root), 'PATH': f'{moved}/commands:/usr/bin:/bin'}
        for command in ('zell', str(game/'bin/zell')):
            result = subprocess.run([command, 'two words'], cwd=self.root, env=env,
                                    capture_output=True, text=True, check=True)
            self.assertEqual(result.stdout.splitlines(),
                             [str(game/'weights/neuronte-split-v9.nrt'), 'two words'])

    def test_unrelated_directory_and_symbolic_links_are_left_in_place(self):
        app = self.root/'linked'
        target = self.root/'other'
        target.mkdir()
        note = target/'notes.txt'
        note.write_text('kept')
        app.symlink_to(target)
        for dest in (app, target):
            with self.subTest(dest=dest), self.assertRaises(ValueError):
                self.install(dest, self.root/'bin')
            self.assertTrue(app.is_symlink())
            self.assertEqual(note.read_text(), 'kept')
        dangling = self.root/'dangling'
        dangling.symlink_to(self.root/'absent')
        with self.assertRaises(ValueError):
            self.install(dangling, self.root/'bin')
        self.assertFalse((self.root/'absent').exists())

    def test_failed_activation_restores_previous_game_and_launcher(self):
        app = self.root/'installed'
        bin_dir = self.root/'bin'
        self.install(app, bin_dir)
        (self.payload/'release').write_text('new')
        rename = Path.rename

        def fail_activation(path, dest):
            if path.name == 'app' and dest == app:
                raise OSError('could not activate staged game')
            return rename(path, dest)

        with patch.object(Path, 'rename', fail_activation), self.assertRaises(OSError):
            self.install(app, bin_dir)
        self.assertTrue((app/'runtime.json').is_file())
        self.assertFalse((app/'release').exists())
        self.assertTrue(os.access(bin_dir/'zell', os.X_OK))


if __name__ == '__main__':
    unittest.main()
