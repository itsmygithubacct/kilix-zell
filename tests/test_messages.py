from contextlib import closing
import json
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import zell_maintenance as maintenance
import zell_messages as messages
import zell_os


class Messages(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        env = patch.dict(os.environ, {'KILIX_ZELL_STATE':str(self.root/'state'),'XDG_CACHE_HOME':str(self.root/'cache')})
        env.start()
        self.addCleanup(env.stop)

    def process(self, pid, name, ticks, rss, birth=15):
        entry = self.root/'proc'/str(pid)
        entry.mkdir(exist_ok=True, parents=True)
        fields = ['0']*22
        fields[0], fields[11], fields[19], fields[21] = 'S',str(ticks),str(birth),str(rss)
        (entry/'stat').write_text(f'{pid} ({name}) '+ ' '.join(fields))

    def test_fatigue_uses_interval_cpu_or_resident_memory_and_persists_speech(self):
        self.process(10,'editor',40,6)
        self.process(11,'renderer',5,8)
        self.process(12,'browser',500,92)
        meter=messages.FatigueSpeech(self.root/'proc')
        with patch.object(messages,'cpu_memory',side_effect=[((100,40),90),((200,42),90),((300,44),90),((400,140),20)]), \
             patch.object(messages.time,'monotonic',side_effect=[200,205,210,335]):
            self.assertIsNone(meter.sample(20))
            self.process(11,'renderer',65,8)
            self.process(12,'reused-pid',700,92,birth=35)
            stamp,cpu=meter.sample(18)
            self.assertIn('renderer uses the most CPU',cpu)
            self.assertNotIn('reused',cpu)
            self.assertIsNone(meter.sample(15))
            self.assertIn('reused-pid and renderer use the most RAM',meter.sample(20)[1])
        self.assertEqual(zell_os.read_state('speech-history.json',[])[0],{'time':stamp,'text':cpu})

    def test_history_merges_desktop_system_and_game_with_timestamps(self):
        db=self.root/'cache/xfce4/notifyd/log.sqlite'
        db.parent.mkdir(parents=True)
        with closing(sqlite3.connect(db)) as con:
            con.execute('CREATE TABLE notifications(timestamp,app_name,summary,body)')
            con.execute('INSERT INTO notifications VALUES(3000000,"Calendar","Meeting","Room 4")')
            con.commit()
        zell_os.write_state('notices.json',[{'time':2,'text':'Job finished'}])
        system=json.dumps({'__REALTIME_TIMESTAMP':'4000000','SYSLOG_IDENTIFIER':'systemd','MESSAGE':'Service ready'})
        with patch.object(messages,'program_json',return_value=system),patch.object(messages.shutil,'which',return_value='journalctl'):
            self.assertEqual(messages.history(),[(4,'systemd','Service ready'),(3,'Calendar','Meeting Room 4'),(2,'Zell','Job finished')])

    def test_maintenance_uses_selected_action_and_cache_browser_stays_in_root(self):
        with patch('zell_update.main',return_value=7) as upgrade, patch.object(maintenance.subprocess,'call',return_value=0) as call:
            self.assertEqual(maintenance.main('upgrade'),7)
            upgrade.assert_called_once_with(pause=False)
            for key,argv in [('clean',['sudo','apt','clean']),('autoclean',['sudo','apt','autoclean']),
                             ('rotate',['sudo','logrotate','/etc/logrotate.conf'])]:
                self.assertEqual(maintenance.main(key),0)
                call.assert_called_with(argv)
        root=self.root/'cache';(root/'subfolder').mkdir(parents=True)
        (root/'external').symlink_to(self.root)
        with patch('builtins.input',side_effect=['1','..','..','q']),patch('builtins.print') as out:
            self.assertEqual(maintenance.cache_browser(),0)
            lines=[str(call.args[0]) for call in out.call_args_list]
            self.assertIn('\n'+str(root/'subfolder'),lines)
            self.assertNotIn('\n'+str(self.root),lines)


if __name__=='__main__':
    unittest.main()
