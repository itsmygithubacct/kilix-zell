import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import zell_media as media
import desktop

class MediaTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.base=Path(self.temp.name)
        self.base_patch=patch.object(desktop,'BASE',self.base);self.base_patch.start()
        self.ui=desktop.media_ui()
    def tearDown(self):self.base_patch.stop();self.temp.cleanup()
    def test_creator_and_playlist_normalization(self):
        self.assertEqual(media.source_url('@Example'),('creator','https://www.youtube.com/@Example'))
        self.assertEqual(media.source_url('https://youtube.com/@Example/videos?view=0')[1],'https://www.youtube.com/@Example')
        self.assertEqual(media.source_url('https://youtube.com/watch?v=abcdefghijk&list=PL_example')[0],'playlist')
        for bad in ['https://youtube.com.evil/@Example','file:///tmp/x','https://name:pass@youtube.com/@Example','https://youtube.com/watch?v=abcdefghijk','https://youtube.com/']:
            with self.assertRaises(ValueError):media.source_url(bad)
    def test_legacy_playlist_migration_and_unfollow(self):
        desktop.state_write('playlists.json',[{'name':'Old','url':'https://youtube.com/watch?v=abcdefghijk&list=PL_old'}])
        lib=media.Library(self.ui);self.assertEqual(len(lib.data['sources']),1)
        lib.unfollow(lib.data['sources'][0])
        self.assertEqual(media.Library(self.ui).data['sources'],[])
    def test_refresh_tracks_new_content_and_preserves_cache_on_error(self):
        lib=media.Library(self.ui);source=lib.follow('@Example','Creator')
        payload={'entries':[{'id':'abcdefghijk','title':'First','timestamp':100}]}
        with patch.object(media,'extract',return_value=payload):self.assertEqual(lib.refresh(),[])
        self.assertEqual(len(lib.feed(unseen=True)),1)
        lib.mark_seen(lib.feed()[0]);self.assertEqual(lib.feed(unseen=True),[])
        payload['entries'].insert(0,{'id':'lmnopqrstuv','title':'Second','timestamp':200})
        with patch.object(media,'extract',return_value=payload):lib.refresh()
        self.assertEqual([r['title'] for r in lib.feed(unseen=True)],['Second'])
        with patch.object(media,'extract',side_effect=RuntimeError('offline')):self.assertEqual(len(lib.refresh()),3)
        reloaded=media.Library(self.ui)
        self.assertEqual([r['title'] for r in reloaded.feed()],['Second','First'])
    def test_creator_refresh_visits_all_three_tabs(self):
        lib=media.Library(self.ui);lib.follow('@Example','Creator')
        with patch.object(media,'extract',return_value={'entries':[]}) as extract:
            lib.refresh()
        self.assertEqual([c.args[0].rsplit('/',1)[1] for c in extract.call_args_list],['videos','shorts','streams'])
    def test_untrusted_entries_cannot_supply_commands_or_other_urls(self):
        source={'url':'https://www.youtube.com/@Example','name':'Creator'}
        payload={'entries':[None,{'id':'--exec bad'},{'id':'abcdefghijk','title':'bad\x1b[2J','url':'file:///etc/passwd'}]}
        result=media.entries(payload,source,1)
        self.assertEqual(len(result),1)
        self.assertEqual(result[0]['url'],'https://www.youtube.com/watch?v=abcdefghijk')
        self.assertNotIn('\x1b',result[0]['title'])
    def test_amp_opens_native_player(self):
        with patch.object(media,'popup',return_value=True) as popup:
            desktop.music(0)
            self.assertEqual(popup.call_args.args[1],['kilix-amp'])
            media.play(self.ui,'/tmp/test.mp3',listen=True)
            self.assertEqual(popup.call_args.args[1],['kilix-amp','/tmp/test.mp3'])
    def test_url_audio_is_cached_for_amp(self):
        def download(argv,timeout):
            Path(argv[argv.index('-o')+1].replace('%(ext)s','mp3')).write_bytes(b'audio')
            return ''
        with patch.object(media.shutil,'which',return_value='/bin/tool'),patch.object(media,'capture',side_effect=download) as capture,patch.object(media,'popup',return_value=True) as popup:
            url='https://www.youtube.com/watch?v=abcdefghijk'
            self.assertTrue(media.play(self.ui,url,listen=True))
            path=Path(popup.call_args.args[1][1])
            self.assertEqual(path.read_bytes(),b'audio')
            self.assertTrue(media.play(self.ui,url,listen=True))
            self.assertEqual(capture.call_count,1)
            self.assertFalse(list(path.parent.glob('download-*')))
    def test_audio_download_failure_does_not_launch(self):
        with patch.object(media.shutil,'which',return_value='/bin/tool'),patch.object(media,'capture',side_effect=RuntimeError('offline')),patch.object(media,'popup') as popup:
            self.assertFalse(media.play(self.ui,'https://example.org/audio',listen=True))
            popup.assert_not_called()

    def test_failed_launch_does_not_mark_seen(self):
        lib=media.Library(self.ui);source=lib.follow('@Example','Creator')
        item=media.entries({'entries':[{'id':'abcdefghijk'}]},source,1)[0]
        with patch.object(self.ui,'choose',side_effect=['watch',None]),patch.object(media,'play',return_value=False):
            media.item_menu(self.ui,lib,item)
        self.assertEqual(lib.data['seen'],[])
    def test_job_handles_multiple_chunks_and_cleans_work_files(self):
        job=self.base/'job';job.mkdir();source=self.base/'input.wav';source.write_bytes(b'fixture')
        media.atomic_json(job/'job.json',{'source':str(source),'status':'queued'})
        calls=[]
        def capture(argv,timeout):
            calls.append(argv)
            if argv[0]=='ffmpeg':
                template=argv[-1]
                for i in range(2):Path(template % i).write_bytes(b'pcm fixture')
                return ''
            return 'hello world'
        with patch.object(media,'voice_runtime',return_value=(Path('/provider'),Path('/model'))),patch.object(media,'capture',side_effect=capture):
            self.assertEqual(media.transcribe_worker(job),0)
        result=json.loads((job/'job.json').read_text())
        self.assertEqual(result['status'],'complete');self.assertEqual(result['completed_chunks'],2)
        self.assertIn('[00:01:00] hello world',(job/'transcript.txt').read_text())
        self.assertFalse(list(job.glob('audio-*')))
        self.assertEqual(sum(a[0]=='/provider' for a in calls),2)
    def test_failed_transcription_never_claims_completion(self):
        job=self.base/'job';job.mkdir();media.atomic_json(job/'job.json',{'source':'/missing','status':'queued'})
        with patch.object(media,'voice_runtime',side_effect=RuntimeError('no model')):
            self.assertEqual(media.transcribe_worker(job),1)
        self.assertEqual(json.loads((job/'job.json').read_text())['status'],'failed')
        self.assertFalse((job/'transcript.txt').exists())
    def test_missing_model_does_not_start_job(self):
        with patch.object(media,'voice_runtime',side_effect=RuntimeError('no model')),patch.object(media.subprocess,'Popen') as launch:
            self.assertIsNone(media.start_transcript(self.ui,'https://example.org/a.mp3'))
            launch.assert_not_called()

if __name__=='__main__':unittest.main()
