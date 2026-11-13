import importlib.util
import json
import os
from pathlib import Path
import sys
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
    def test_theater_install_uses_the_fixed_action(self):
        deb='google-chrome-stable_current_amd64.deb'
        self.assertTrue(d.theater_install_action_ok('firefox-esr','install-firefox-esr',None))
        self.assertTrue(d.theater_install_action_ok('chromium','install-chromium',''))
        self.assertTrue(d.theater_install_action_ok('chrome','install-chrome-deb',deb))
        self.assertFalse(d.theater_install_action_ok('chrome','install-chrome-deb','google-chrome-stable'))
        self.assertFalse(d.theater_install_action_ok('firefox-esr','install-firefox-esr',deb))
        self.assertFalse(d.theater_install_action_ok('chrome','install-firefox-esr',deb))
        firefox=d.theater_install_command({'browser':'firefox-esr','action':'install-firefox-esr','title':'Theater install'})
        self.assertEqual(firefox[0],sys.executable)
        self.assertTrue(firefox[1].endswith('zell_theater_install.py'))
        self.assertEqual(firefox[2],'install-firefox-esr')
        self.assertEqual(len(firefox),3)
        chrome=d.theater_install_command({'browser':'chrome','action':'install-chrome-deb','deb':deb})
        self.assertEqual(chrome[2],'install-chrome-deb')
        self.assertNotIn('sudo',chrome)
        self.assertNotIn('google-chrome-stable',chrome)
        self.assertIsNone(d.theater_install_command({'browser':'firefox-esr','action':'install-firefox-esr','argv':['sudo','apt-get']}))
        self.assertIsNone(d.theater_install_command({'argv':['apt-get','install','-y',deb]}))

    def test_local_helper_install_listing_and_keys(self):
        tools=Path(__file__).resolve().parents[1]/'tools'
        for name in ('zell_theater.py','zell_theater_browser.py','zell_theater_install.py'):
            self.assertTrue((tools/name).is_file(),name)
        if str(tools) not in sys.path:sys.path.insert(0,str(tools))
        import zell_theater as theater
        import zell_theater_browser as browser_mod
        import zell_theater_install as install
        runtime={'role':'installed','commit':'','selected':str(tools),'missing':''}

        class Missing:
            def which(self,name):
                return None

        def opened(browser):
            session=theater.TheaterSession(runtime,browser=Missing(),hold_audio=lambda *_args:False,play_frames=False)
            self.assertIsNone(session.handle({'cmd':'open','id':'u1','browser':browser,'source':{'kind':'url','url':'https://example.com/a'}}))
            events=[row for row in session.drain() if row.get('event')=='install']
            self.assertEqual(len(events),1)
            event=events[0]
            self.assertEqual(event['phase'],'request')
            self.assertNotIn('argv',event)
            self.assertTrue(d.theater_install_action_ok(event['browser'],event['action'],event.get('deb')))
            command=d.theater_install_command(event)
            self.assertEqual(command[2],event['action'])
            self.assertTrue(command[1].endswith('zell_theater_install.py'))
            encoded=json.dumps(event)
            self.assertLessEqual(len(encoded.encode()),65536)
            self.assertNotIn('argv',encoded)
            return event

        firefox=opened('firefox-esr')
        self.assertEqual(firefox['action'],'install-firefox-esr')
        self.assertNotIn('deb',firefox)
        chrome=opened('chrome')
        self.assertEqual(chrome['action'],'install-chrome-deb')
        self.assertEqual(chrome['deb'],browser_mod.CHROME_DEB_NAME)
        self.assertEqual(chrome['deb'],'google-chrome-stable_current_amd64.deb')
        chromium=opened('chromium')
        self.assertEqual(chromium['action'],'install-chromium')
        self.assertNotIn('deb',chromium)

        class Present:
            def which(self,name):
                return '/usr/bin/'+name
            def run(self,argv):
                raise AssertionError(argv)
            def confirm(self):
                raise AssertionError('confirm')
            def cleanup(self,path):
                return None
        self.assertEqual(install.main(['zell_theater_install.py','install-firefox-esr'],Present()),0)
        self.assertEqual(install.main(['zell_theater_install.py','sudo'],Present()),2)
        self.assertEqual(install.main(['zell_theater_install.py','install-firefox-esr','extra'],Present()),2)

        with tempfile.TemporaryDirectory() as root:
            root=Path(root)
            leaf=root
            while len(str(leaf))<560:
                leaf=leaf/('n'*40)
            leaf.mkdir(parents=True)
            video=leaf/'café.mp4'
            video.write_bytes(b'')
            self.assertGreater(len(str(video)),512)
            session=theater.TheaterSession(runtime,browser=Missing(),hold_audio=lambda *_args:False,play_frames=False)
            listed=session.handle({'cmd':'open','id':'fold1','browser':None,'source':{'kind':'folder','path':str(leaf)}})
            self.assertEqual(listed['event'],'listing')
            self.assertEqual(listed['entries'][0]['name'],'café.mp4')
            self.assertGreater(len(listed['entries'][0]['path']),512)
            self.assertIn('café',listed['entries'][0]['path'])
            self.assertLessEqual(len(json.dumps(listed).encode()),65536)
            self.assertEqual(session.items,{})
            first=root/'a.mp4'
            second=root/'b.mp4'
            first.write_bytes(b'')
            second.write_bytes(b'')
            # Empty stand-ins are not videos. Give them metadata so this
            # protocol check can open one. Non-empty invalid bytes stay rejected.
            original_probe=theater.probe_file
            def probe(path):
                info=original_probe(path)
                try:
                    empty=Path(path).is_file() and Path(path).stat().st_size==0
                except OSError:
                    empty=False
                if empty and not theater._playable(info):
                    return {'width':32,'height':32,'duration_ms':1000,'audio':False,'rate':12.0}
                return info
            theater.probe_file=probe
            try:
                opened_file=session.handle({'cmd':'open','id':'a','browser':None,'source':{'kind':'file','path':str(first)}})
                self.assertEqual(opened_file['id'],'a')
                self.assertEqual(opened_file['state'],'paused')
                session.active='a'
                session.items['a']['paused']=False
                session.items['a']['src_w']=32
                session.items['a']['src_h']=32
                other=session.handle({'cmd':'open','id':'b','browser':None,'source':{'kind':'file','path':str(second)}})
                self.assertEqual(other['id'],'b')
                self.assertEqual(other['state'],'paused')
                self.assertEqual(session.active,'a')
            finally:
                theater.probe_file=original_probe

        class Page:
            def __init__(self):
                self.keys=[]
            def which(self,name):
                return '/bin/true'
            def launch(self,browser,url,width,height):
                self.browser=browser
                self.url=url
            def key(self,key,modifiers=0,text=''):
                self.keys.append((key,modifiers,text))
                return True
            def close(self):
                return None
        page=Page()
        keyed=theater.TheaterSession(runtime,browser=page,hold_audio=lambda *_args:False,play_frames=False)
        keyed.handle({'cmd':'open','id':'u1','browser':'firefox-esr','source':{'kind':'url','url':'https://example.com/a'}})
        keyed.handle({'cmd':'use','id':'u1'})
        self.assertEqual(keyed.active,'u1')
        before=len(page.keys)
        keyed.handle({'cmd':'key','action':'up','key':'Enter','modifiers':0,'text':''})
        self.assertEqual(len(page.keys),before)
        keyed.handle({'cmd':'key','action':'down','key':'Enter','modifiers':0,'text':''})
        self.assertEqual(page.keys[-1][0],'Enter')

        class Inj:
            def __init__(self):
                self.chords=[]
            def chord(self,key,modifiers,action):
                self.chords.append((key,modifiers,action))
                return True
        class Sess:
            def __init__(self):
                self.inj=Inj()
            def make_injector(self):
                return self.inj
        real=browser_mod.PrivateBrowser(which=lambda name:None)
        real.session=Sess()
        self.assertTrue(real.key('Enter',0,''))
        self.assertEqual([row[2] for row in real.session.inj.chords],[1,3])

if __name__=='__main__':unittest.main()
