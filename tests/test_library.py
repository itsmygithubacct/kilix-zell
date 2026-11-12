import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import desktop
import zell_library as local

class LibraryTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name)
        self.state=patch.object(desktop,'BASE',self.root/'state');self.state.start()
    def tearDown(self):self.state.stop();self.tmp.cleanup()
    def test_scan_filter_dedup_refresh_and_removal_preserve_originals(self):
        books=self.root/'Books';books.mkdir();(books/'one.epub').write_text('book')
        (books/'song.mp3').write_text('song');(books/'.hidden.pdf').touch()
        (books/'alias.pdf').symlink_to(books/'one.epub')
        lib=local.Library(desktop,'documents')
        self.assertEqual(lib.add(str(books),True),(1,[]))
        self.assertEqual(lib.add(str(books/'one.epub')),(0,[]))
        (books/'two.pdf').write_text('pdf');self.assertEqual(lib.refresh(),(1,[]))
        lib=local.Library(desktop,'documents');self.assertEqual(len(lib.data['files']),2)
        lib.remove_file(str(books/'one.epub'))
        self.assertTrue((books/'one.epub').is_file())
        (books/'two.pdf').unlink();self.assertIn(str(books/'two.pdf'),lib.data['files'])
    def test_limit_and_missing_folder_are_reported(self):
        folder=self.root/'Music';folder.mkdir()
        for i in range(3):(folder/f'{i}.flac').touch()
        lib=local.Library(desktop,'music')
        with patch.object(local,'LIMIT',2):
            count,errors=lib.add(str(folder),True);self.assertEqual(count,2);self.assertTrue(errors)
        with self.assertRaises(ValueError):lib.add(str(self.root/'missing'),True)
    def test_launcher_selects_neuronte_and_preserves_arguments(self):
        app=self.root/'installed';(app/'weights').mkdir(parents=True)
        (app/'weights/neuronte-split-v4.nrt').touch()
        binary=app/'zell-neuronte';binary.write_text('#!/bin/sh\nprintf "%s\\n" "$ZELL_NEURONTE_WEIGHTS" "$@"\n');binary.chmod(0o755)
        launcher=Path(__file__).resolve().parents[1]/'bin/zell'
        env=dict(os.environ,ZELL_NEURONTE_HOME=str(app));env.pop('ZELL_NEURONTE_WEIGHTS',None)
        result=subprocess.run([str(launcher),'argument with spaces','--version'],capture_output=True,text=True,env=env,check=True)
        self.assertEqual(result.stdout.splitlines(),[str(app/'weights/neuronte-split-v4.nrt'),'argument with spaces','--version'])
    def test_launcher_does_not_fall_back_to_legacy_zell(self):
        app=self.root/'kilix-apps/kilix-zell';app.mkdir(parents=True)
        (app/'zell').write_text('#!/bin/sh\nexit 0\n');(app/'zell').chmod(0o755)
        env=dict(os.environ,ZELL_NEURONTE_HOME='',GPU_TERMINAL_HOME=str(self.root/'runtime'),GPU_TERMINAL_SOURCE_HOME=str(self.root))
        result=subprocess.run([str(Path(__file__).resolve().parents[1]/'bin/zell')],capture_output=True,text=True,env=env)
        self.assertEqual(result.returncode,127)
        self.assertIn('make install',result.stderr)

if __name__=='__main__':unittest.main()
