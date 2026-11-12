import importlib.util
import os
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

spec=importlib.util.spec_from_file_location('desktop',Path(__file__).resolve().parents[1]/'tools/desktop.py')
d=importlib.util.module_from_spec(spec);spec.loader.exec_module(d)

class DesktopTests(unittest.TestCase):
    def test_package_names_cannot_inject_options_or_shell(self):
        for value in ['vim','libssl3:amd64','g++']:self.assertTrue(d.valid_package(value))
        for value in ['-y','vim; reboot','$(id)','a b','/tmp/package.deb','']:self.assertFalse(d.valid_package(value))
    def test_urls(self):
        self.assertTrue(d.valid_url('https://www.youtube.com/playlist?list=abc'))
        for url in ['file:///etc/passwd','--script=x','javascript:alert(1)','https://a:b@example.org']:
            self.assertFalse(d.valid_url(url))
    def test_follow_requires_a_playlist_identifier(self):
        self.assertTrue(d.valid_playlist_url('https://www.youtube.com/playlist?list=PL123'))
        self.assertFalse(d.valid_playlist_url('https://www.youtube.com/watch?v=abc'))
        self.assertFalse(d.valid_playlist_url('https://example.org/playlist?list=PL123'))
    def test_cleanup_ignores_new_files_and_symlinks(self):
        with tempfile.TemporaryDirectory() as root:
            root=Path(root);cache=root/'cache';cache.mkdir()
            old=cache/'old.png';old.write_text('old');os.utime(old,(1,1))
            fresh=cache/'new.png';fresh.write_text('new')
            external=root/'personal';external.write_text('important');os.utime(external,(1,1))
            (cache/'link').symlink_to(external)
            (cache/'directory').symlink_to(root,target_is_directory=True)
            self.assertEqual([r[0] for r in d.cleanup_candidates(cache)],[old])
            self.assertEqual(external.read_text(),'important')
    def test_cleanup_requires_explicit_confirmation(self):
        with tempfile.TemporaryDirectory() as root:
            root=Path(root);cache=root/'.cache/thumbnails';cache.mkdir(parents=True)
            file=cache/'old';file.write_text('keep');os.utime(file,(1,1))
            with patch.object(d.Path,'home',return_value=root),patch('builtins.input',return_value='no'):
                d.housekeeping(3)
            self.assertTrue(file.exists())
    def test_state_is_atomic_private_and_model_schema_is_compatible(self):
        with tempfile.TemporaryDirectory() as root,patch.object(d,'BASE',Path(root)):
            with patch.object(d,'choose',return_value='qwen3.5-4b'),patch.object(d,'run'),patch('builtins.input',return_value='Vulkan1'):
                d.admin(5)
            saved=d.state_read('model.json',{})
            self.assertEqual(saved,{'schema':'kilix.zell.model/v1','model':'qwen3.5-4b','device':'Vulkan1'})
            self.assertEqual((Path(root)/'model.json').stat().st_mode&0o777,0o600)
    def test_shell_and_tui_are_separate_commands(self):
        with patch.object(d,'run') as run:
            d.desktop(6,0);run.assert_called_with(['kilix-tui'],pause=False)
            d.desktop(7,0);run.assert_called_with(['kilix','shell'],pause=False)
    def test_missing_command_does_not_spawn(self):
        with patch.object(d.shutil,'which',return_value=None),patch.object(d.subprocess,'run') as run:
            self.assertEqual(d.run(['missing-tool']),127);run.assert_not_called()

if __name__=='__main__':unittest.main()
