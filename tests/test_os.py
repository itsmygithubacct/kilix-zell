"""Every station row is exercised with commands fenced off from the real host."""
from contextlib import ExitStack, redirect_stdout
import copy
import io
import json
import os
from pathlib import Path
import socket
import stat
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import wave
import shutil
import unittest
from unittest.mock import patch, Mock
import zipfile

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import zell_os as core
core.load_tasks()
import zell_system as system
import zell_files as files
import zell_schedule as schedule
import zell_devices as devices
import zell_audio as audio
import compile_tasks

class ProbeStopped(Exception):pass

class CatalogTests(unittest.TestCase):
    def test_native_catalog_exactly_matches_private_task_registry(self):
        self.assertEqual(compile_tasks.render(),(Path(__file__).resolve().parents[1]/'src/os_tasks.inc').read_text())
        self.assertEqual(len(core.STATIONS),10)
        self.assertTrue(all(0<len(core.entries(g))<=16 for g in core.STATIONS.values()))

def row_test(key):
    def test(self):
        with tempfile.TemporaryDirectory() as root, ExitStack() as stack:
            stack.enter_context(patch.dict(os.environ,{'KILIX_ZELL_STATE':root,'XDG_CONFIG_HOME':root}))
            stack.enter_context(patch('builtins.input',return_value=''))
            stack.enter_context(patch('getpass.getpass',side_effect=ProbeStopped))
            # Stop at the first tool boundary, input, or confirmation. A test
            # can never suspend, touch devices, authenticate, or start players.
            execute=stack.enter_context(patch.object(core.subprocess,'run',side_effect=ProbeStopped))
            stack.enter_context(patch.object(core.subprocess,'Popen',side_effect=ProbeStopped))
            for module in (system,files,schedule,devices):
                for name in ('run','ask','pick','confirm','path_input','positive'):
                    if hasattr(module,name):stack.enter_context(patch.object(module,name,side_effect=ProbeStopped))
            stack.enter_context(patch.object(files,'walk_files',return_value=iter([])))
            stack.enter_context(patch.object(system,'restart_status',return_value={'needed':False,'text':'Current'}))
            stack.enter_context(patch.object(schedule,'aggregate',return_value={'count':0,'text':''}))
            stack.enter_context(redirect_stdout(io.StringIO()))
            try:core.TASKS[key][2]()
            except (ProbeStopped,core.Cancelled):pass
            # Reads and local metadata are permitted; any action reaching an
            # actual process would have raised ProbeStopped above.
            self.assertFalse(list(Path(root).glob('*.service')))
    return test

for key in core.TASKS:
    setattr(CatalogTests,'test_row_'+key.replace('.','_').replace('-','_'),row_test(key))

class SafetyTests(unittest.TestCase):
    def test_restart_detects_kernel_and_gpu_module_changes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'boot').mkdir();(root/'boot/vmlinuz-7.2.0').touch()
            module=root/'sys/module/nvidia';module.mkdir(parents=True);(module/'version').write_text('old')
            report=system.restart_status(root,lambda argv:'new','7.1.0')
            self.assertTrue(report['needed']);self.assertEqual(len(report['reasons']),2)
            (module/'version').write_text('new')
            self.assertFalse(system.restart_status(root,lambda argv:'new','7.2.0')['needed'])

    def test_suspend_cancel_does_not_call_power_operation(self):
        with patch.object(system,'run') as run,patch('builtins.input',return_value='no'):
            with self.assertRaises(core.Cancelled):system.suspend()
            self.assertEqual(run.call_args_list[0].args[0][0],'systemd-inhibit')
            self.assertEqual(run.call_count,1)

    def test_package_remove_simulates_then_requires_exact_word(self):
        with patch.object(system,'run') as run,patch('builtins.input',return_value='yes'):
            with self.assertRaises(core.Cancelled):system.apt_change('remove',['sample'])
            run.assert_called_once_with(['apt-get','--simulate','remove','sample'])

    def test_user_text_stays_one_argument_and_missing_tool_names_package(self):
        with patch.object(core,'tool',return_value='/usr/bin/example'),patch.object(core.subprocess,'run',return_value=subprocess.CompletedProcess([],0)) as call:
            core.run(['example','$(touch /tmp/should-not-exist); value'])
            self.assertEqual(call.call_args.args[0][1],'$(touch /tmp/should-not-exist); value')
            self.assertNotIn('shell',call.call_args.kwargs)
        with patch.object(core.shutil,'which',return_value=None),patch.object(Path,'is_file',return_value=False):
            with self.assertRaisesRegex(FileNotFoundError,'smartmontools'):core.tool('smartctl')

    def test_wifi_secret_never_enters_argv_or_state(self):
        calls=[];fds=[]
        def run(argv,**kwargs):
            calls.append(argv)
            if kwargs.get('pass_fds'):
                fd=kwargs['pass_fds'][0];fds.append(os.read(fd,4096))
            return 0
        with patch.object(system,'pick',side_effect=['connect','wpa-psk']),patch.object(system,'ask',return_value='Cafe'),patch.object(system.getpass,'getpass',return_value='synthetic-wifi-value'),patch.object(system,'confirm'),patch.object(system,'run',side_effect=run),patch('zell_secrets.save_bytes') as save:
            system.wifi()
        self.assertNotIn('synthetic-wifi-value',str(calls))
        self.assertEqual(fds,[b'802-11-wireless-security.psk:synthetic-wifi-value\n'])
        self.assertEqual(save.call_args.args,('Wi-Fi / Cafe','wifi','password',b'synthetic-wifi-value'))
        self.assertTrue(any('wifi-sec.psk-flags' in command and '2' in command for command in calls))

    def test_archive_rejects_traversal_links_and_never_overwrites(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);archive=root/'bad.zip'
            for name in ('../escape','/absolute'):
                with zipfile.ZipFile(archive,'w') as z:z.writestr(name,b'bad')
                with self.assertRaises(ValueError):files.archive_members(archive)
            with tarfile.open(root/'link.tar','w') as tar:
                item=tarfile.TarInfo('link');item.type=tarfile.SYMTYPE;item.linkname='/tmp';tar.addfile(item)
            with self.assertRaises(ValueError):files.archive_members(root/'link.tar')
            with zipfile.ZipFile(archive,'w') as z:z.writestr('folder/file.txt',b'content')
            dest=root/'new';files.extract_archive(archive,dest)
            self.assertEqual((dest/'folder/file.txt').read_bytes(),b'content')
            with self.assertRaises(FileExistsError):files.extract_archive(archive,dest)

    def test_removable_write_refuses_system_mounted_and_swapped_devices(self):
        disk={'name':'/dev/example','type':'disk','size':1024,'serial':'synthetic-serial','wwn':None,'maj:min':'8:16','rm':True,'ro':False,'mountpoints':[None]}
        identity=files.device_identity(disk)
        with patch.object(files,'drives',return_value=[disk]):self.assertEqual(files.writable_device(identity),disk)
        for change in ({'mountpoints':['/']},{'rm':False,'tran':'nvme'},{'serial':'another'},{'ro':True},{'children':[{'mountpoints':['/home']}] } ):
            changed={**disk,**change}
            with patch.object(files,'drives',return_value=[changed]):
                with self.assertRaises(ValueError):files.writable_device(identity)

    def test_backup_checksum_detects_same_size_same_timestamp_edit(self):
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(tmp)/'source';dest=Path(tmp)/'dest';source.mkdir();dest.mkdir()
            (source/'file').write_text('first');files.backup_copy(source,dest);files.verify_backup(source,dest)
            before=(dest/'file').stat();(dest/'file').write_text('other');os.utime(dest/'file',ns=(before.st_atime_ns,before.st_mtime_ns))
            with self.assertRaises(RuntimeError):files.verify_backup(source,dest)
            with self.assertRaises(ValueError):files.backup_copy(source,source/'recursive')
            with patch.dict(os.environ,{'KILIX_ZELL_ARCHIVE_ONLY':tmp+'/archive','KILIX_ZELL_STATE':tmp}):
                with self.assertRaises(ValueError):files.active_path(tmp+'/archive/live')
                self.assertEqual(files.active_path(tmp+'/elsewhere'),Path(tmp).resolve()/'elsewhere')

    def test_backup_snapshots_keep_prior_versions_and_refuse_missing_drive(self):
        with tempfile.TemporaryDirectory() as tmp,patch.dict(os.environ,{'KILIX_ZELL_STATE':tmp+'/state'}),redirect_stdout(io.StringIO()):
            root=Path(tmp);source=root/'source';source.mkdir();destination=root/'backups'
            (source/'file').write_text('first')
            identity=files.backup_volume(destination)
            first=files.perform_backup(source,destination,destination_volume=identity)
            (source/'file').write_text('second')
            second=files.perform_backup(source,destination,destination_volume=identity)
            self.assertNotEqual(first,second)
            self.assertEqual((first/'file').read_text(),'first')
            self.assertEqual((second/'file').read_text(),'second')
            self.assertEqual(len(core.read_state('backups.json')),2)
            with patch.object(files,'backup_volume',return_value={'uuid':'replacement-volume'}):
                with self.assertRaisesRegex(ValueError,'unavailable or changed'):
                    files.perform_backup(source,destination,destination_volume=identity)
            self.assertEqual(len(list(destination.iterdir())),2)

    def test_saved_open_wifi_does_not_request_a_vault_password(self):
        calls=[]
        def run(argv,**kwargs):
            calls.append(argv)
            return '' if kwargs.get('capture') else 0
        with patch.object(system,'pick',return_value='saved'),patch.object(system,'ask',return_value='a'*36),patch.object(system,'run',side_effect=run),patch.object(system,'confirm'),patch('zell_secrets.records') as records:
            system.wifi()
        records.assert_not_called()
        self.assertEqual(calls[-1],['nmcli','connection','up','uuid','a'*36])

    def test_owner_failure_keeps_a_short_explanation(self):
        completed=subprocess.CompletedProcess(['pleb'],1,'','pleb-lock: a local X11 desktop is required\n')
        with patch.object(core,'tool',return_value='/usr/bin/pleb'),patch.object(core.subprocess,'run',return_value=completed):
            with self.assertRaisesRegex(RuntimeError,'local X11 desktop'):
                core.run(['pleb','lock'],detail=True)

    def test_brightness_still_uses_brightnessctl(self):
        with patch.object(devices,'run') as run,patch.object(devices,'positive',return_value=40),patch.object(devices,'confirm'):
            devices.brightness()
        self.assertEqual([call.args[0] for call in run.call_args_list],[['brightnessctl','info'],['brightnessctl','set','40%']])

    def test_tinkerer_does_not_grow_a_second_locker_or_display_autostart(self):
        text=Path(devices.__file__).read_text()
        for banned in ('xrandr','xss-lock','xsettingsd','loginctl'):
            self.assertNotIn(banned,text)
        labels=[label for label,_key in core.entries('tinkerer')]
        self.assertEqual(labels[0],'Display resolution, arrangement and external monitors')
        self.assertIn('Display brightness',labels)
        self.assertLessEqual(len(labels),16)
        for label in ('Lock the screen now','Automatic screen lock','Display blanking and power saving','Night light','Themes, icons, cursor and Kilix font'):
            self.assertIn(label,labels)

    def test_service_units_are_private_escaped_and_preserve_calendar(self):
        with tempfile.TemporaryDirectory() as tmp,patch.dict(os.environ,{'KILIX_ZELL_STATE':tmp+'/space % and $/state','XDG_CONFIG_HOME':tmp}),patch.object(schedule,'run') as run:
            identity='a'*32;schedule.install_schedule(identity,{'calendar':'daily'})
            path=Path(tmp)/'systemd/user'/('zell-task-'+identity+'.service')
            text=path.read_text();self.assertIn('%%',text);self.assertIn(' and $/state',text)
            self.assertEqual(schedule.unit_quote('a$b%c'),'"a$$b%%c"')
            self.assertEqual(path.stat().st_mode&0o777,0o600)
            self.assertIn('OnCalendar=daily',path.with_suffix('.timer').read_text())
            self.assertEqual(run.call_args.args[0][-1],'zell-task-'+identity+'.timer')
            with self.assertRaises(ValueError):schedule.install_schedule('b'*32,{'calendar':'daily\nUnit=evil.service'})

    def test_job_cancellation_requires_process_identity(self):
        with patch.object(schedule,'process_identity',return_value='new'),patch.object(schedule.os,'pidfd_open') as openfd:
            with self.assertRaises(ValueError):schedule.cancel_job({'pid':999999,'identity':'old','label':'Synthetic'})
            openfd.assert_not_called()

    def test_coding_cancel_revalidates_receipt_before_sending(self):
        import zell_rooms
        pane={'pane_id':7,'tab_id':2,'broker':'synthetic-broker','agent':'codex'}
        job={'label':'Synthetic session','pane':pane}
        with patch.object(schedule,'confirm'),patch.object(zell_rooms,'_snapshot',return_value={'panes':[{**pane,'broker':'replacement'}]}),patch.object(zell_rooms,'_control') as control:
            with self.assertRaises(ValueError):schedule.cancel_job(job)
            control.assert_not_called()
        with patch.object(schedule,'confirm'),patch.object(zell_rooms,'_snapshot',return_value={'panes':[pane]}),patch.object(zell_rooms,'_control') as control:
            schedule.cancel_job(job)
            control.assert_called_once_with('key',7,'ctrl-c','--expect-broker','synthetic-broker')

    def test_notices_bound_deduplicate_and_respect_quiet_hours(self):
        with tempfile.TemporaryDirectory() as tmp,patch.dict(os.environ,{'KILIX_ZELL_STATE':tmp}),patch.object(schedule,'quiet_now',return_value=True),patch.object(schedule.subprocess,'run') as run:
            for i in range(205):schedule.notice('Job completed '+str(i),str(i))
            schedule.notice('Duplicate','204')
            rows=core.read_state('notices.json',[]);self.assertEqual(len(rows),200)
            self.assertEqual(sum(r['key']=='204' for r in rows),1);run.assert_not_called()

class MusicTests(unittest.TestCase):
    def test_inflight_pause_reply_does_not_immediately_unmute(self):
        with tempfile.TemporaryDirectory() as tmp,patch.dict(os.environ,{'KILIX_ZELL_STATE':tmp}),patch.object(audio,'background_pause'),patch.object(audio,'amp',return_value={'state':'playing'}):
            audio.toggle_mute();monitor=audio.MusicMonitor()
            self.assertTrue(monitor.poll()['muted'])
            with patch.object(audio,'amp',return_value={'state':'paused'}):self.assertTrue(monitor.poll()['muted'])
            self.assertFalse(monitor.poll()['muted'])

    @unittest.skipUnless(shutil.which('kilix-amp'),'Kilix Amp is not installed')
    def test_real_isolated_amp_with_null_output(self):
        with tempfile.TemporaryDirectory(prefix='zell-amp-test-') as tmp,patch.dict(os.environ,{'KILIX_ZELL_STATE':tmp+'/state','XDG_RUNTIME_DIR':tmp,'XDG_CONFIG_HOME':tmp+'/config'}):
            path=Path(tmp)/'synthetic.wav'
            with wave.open(str(path),'wb') as out:
                out.setnchannels(1);out.setsampwidth(2);out.setframerate(8000);out.writeframes(bytes(960000))
            process=subprocess.Popen(['kilix-amp','--headless',str(path)],env={**audio.amp_environment(),'KILIX_AMP_AUDIO_OUTPUT':'null'},stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            def wait_state(wanted):
                deadline=time.monotonic()+8
                while time.monotonic()<deadline:
                    try:
                        if audio.amp('state')['state']==wanted:return
                    except (OSError,RuntimeError):pass
                    time.sleep(.05)
                self.fail('Isolated Amp did not reach '+wanted)
            try:
                wait_state('playing');audio.dj_started()
                self.assertTrue(audio.toggle_mute());wait_state('paused')
                self.assertFalse(audio.toggle_mute());wait_state('playing')
                audio.toggle_mute();wait_state('paused');monitor=audio.MusicMonitor();monitor.poll()
                audio.amp('play');wait_state('playing');self.assertFalse(monitor.poll()['muted'])
                audio.amp('stop');wait_state('stopped');self.assertFalse(monitor.poll()['dj_active'])
            finally:
                try:audio.amp('quit')
                except (OSError,RuntimeError):pass
                try:process.wait(timeout=5)
                except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=5)

    def test_mute_pauses_dj_unmute_resumes_and_start_clears_mute(self):
        with tempfile.TemporaryDirectory() as tmp,patch.dict(os.environ,{'KILIX_ZELL_STATE':tmp}),patch.object(audio,'background_pause') as bg:
            core.write_state('room-music.json',{'muted':False,'dj_active':True})
            with patch.object(audio,'amp',side_effect=[{'state':'playing'},{}]) as amp:
                self.assertTrue(audio.toggle_mute());self.assertEqual(amp.call_args.args,('pause',))
            bg.assert_called_with(True)
            with patch.object(audio,'amp',side_effect=[{'state':'paused'},{}]) as amp:
                self.assertFalse(audio.toggle_mute());self.assertEqual(amp.call_args.args,('play',))
            core.write_state('room-music.json',{'muted':True});audio.dj_started()
            self.assertFalse(audio.state()['muted']);self.assertTrue(audio.state()['dj_active']);bg.assert_called_with(True)

    def test_dj_gui_play_unmutes_and_stop_restores_background(self):
        with tempfile.TemporaryDirectory() as tmp,patch.dict(os.environ,{'KILIX_ZELL_STATE':tmp}),patch.object(audio,'background_pause') as bg:
            core.write_state('room-music.json',{'muted':True})
            monitor=audio.MusicMonitor()
            with patch.object(audio,'amp',return_value={'state':'playing'}):result=monitor.poll()
            self.assertFalse(result['muted']);bg.assert_called_with(True)
            with patch.object(audio,'amp',return_value={'state':'stopped'}):monitor.poll()
            bg.assert_called_with(False)

    def test_owned_amp_socket_is_distinct_from_other_music(self):
        with tempfile.TemporaryDirectory() as tmp,patch.dict(os.environ,{'XDG_RUNTIME_DIR':tmp,'KILIX_AMP_SOCKET':'/tmp/other-player.sock'}):
            env=audio.amp_environment()
            self.assertNotEqual(env['KILIX_AMP_SOCKET'],'/tmp/other-player.sock')
            self.assertEqual(Path(env['KILIX_AMP_SOCKET']).parent.stat().st_mode&0o777,0o700)

    def test_rpc_handles_events_and_disconnect_without_hanging(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'test.sock'
            with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as server:
                server.bind(str(path));server.listen(1)
                def reply():
                    conn,_=server.accept()
                    with conn:
                        conn.recv(4096);conn.sendall(b'{"event":"start-file"}\n{"error":"success"}\n')
                worker=threading.Thread(target=reply);worker.start()
                self.assertEqual(audio.rpc(path,{'command':['test']}),{'error':'success'});worker.join()

def display_fixture():
    output={'name':'HDMI-1','identity':'a'*64,'enabled':True,'primary':True,'mode':'1920x1080','rate':'60.00',
            'x':0,'y':0,'rotation':'normal','scale':1.0,'filter':''}
    inventory={'outputs':[{**output,'modes':{'1920x1080':['60.00'],'1280x720':['60.00']},'safe':True}],'maximum':[8192,8192]}
    return inventory,{'version':2,'outputs':[dict(output)]}

class TinkererOwnerTests(unittest.TestCase):
    def test_lock_calls_pleb_and_a_failure_is_not_success(self):
        with patch.object(devices,'confirm'),patch.object(devices,'run',side_effect=RuntimeError('pleb exited with status 1. a local X11 desktop is required')):
            with self.assertRaisesRegex(RuntimeError,'not locked'):
                devices.lock_screen()
            self.assertEqual(devices.run.call_args.args[0],['pleb','lock'])
        with patch.object(devices,'confirm'),patch.object(devices,'run',side_effect=FileNotFoundError('pleb is missing. Install pleb using Merchant.')):
            with self.assertRaisesRegex(FileNotFoundError,'pleb is missing'):
                devices.lock_screen()

    def test_screen_lock_argv_read_back_and_preserved_policy(self):
        saved={'version':1,'policy':'on','idle_seconds':600}
        calls=[]
        def runner(argv):
            calls.append(argv)
            if '--show' in argv:
                return json.dumps({'idle_seconds':1,'policy':'session','version':1})
            return ''
        with self.assertRaisesRegex(RuntimeError,'previous policy'):
            devices.apply_screen_lock('off',0,saved,runner)
        self.assertEqual(calls[0],['pleb','screen-lock','--set','off','--idle-seconds','0'])
        self.assertEqual(calls[1],['pleb','screen-lock','--show'])
        self.assertEqual(calls[2],['pleb','screen-lock','--set','on','--idle-seconds','600'])
        self.assertNotIn('lid',str(calls))
        calls.clear()
        def unreadable(argv):
            calls.append(argv)
            if '--show' in argv:return 'not json'
            return ''
        with self.assertRaisesRegex(RuntimeError,'previous policy'):
            devices.apply_screen_lock('off',15,saved,unreadable)
        self.assertEqual(calls[-1],['pleb','screen-lock','--set','on','--idle-seconds','600'])
        calls.clear()
        def show(argv):
            calls.append(argv)
            if '--set' in argv:
                saved.update(policy=argv[argv.index('--set')+1], idle_seconds=int(argv[argv.index('--idle-seconds')+1]))
                return 'Saved. Changes apply at your next login.\n'
            return json.dumps(saved)
        with redirect_stdout(io.StringIO()) as output:
            devices.apply_screen_lock('session',90,dict(saved),show)
        self.assertIn('next login',output.getvalue())
        self.assertEqual(saved,{'version':1,'policy':'session','idle_seconds':90})
        calls.clear()
        with patch.object(devices,'run',side_effect=lambda argv,**kwargs:'not json') as show_run:
            with self.assertRaises(ValueError):devices.screen_lock()
            self.assertEqual(show_run.call_args_list[0].args[0],['pleb','screen-lock','--show'])
            self.assertEqual(len(show_run.call_args_list),1)

    def test_display_preview_timeout_cancel_and_fake_rollback(self):
        inventory,layout=display_fixture()
        proposed=json.loads(json.dumps(layout))
        proposed['outputs'][0]['mode']='1280x720'
        before=json.loads(json.dumps(layout))
        for event in ('timeout','cancel','ui_dead','hotplug'):
            preview=devices.DisplayPreview(before)
            preview.start(proposed)
            self.assertEqual(preview.saved,before)
            getattr(preview,event)()
            self.assertEqual(preview.saved,before)
            self.assertEqual(preview.live,before)
            with self.assertRaises(RuntimeError):preview.confirm()
        preview=devices.DisplayPreview(before)
        preview.start(proposed)
        preview.live['outputs'][0]['mode']='640x480'
        self.assertEqual(preview.confirm(),'reverted')
        self.assertEqual(preview.saved,before)
        self.assertEqual(preview.live,before)
        preview=devices.DisplayPreview(before)
        preview.start(proposed)
        self.assertEqual(preview.confirm(),'confirmed')
        self.assertEqual(preview.saved,proposed)
        with tempfile.TemporaryDirectory() as tmp:
            seen=[]
            def runner(argv,**kwargs):
                seen.append((list(argv),Path(argv[3]).read_text()))
                return 1
            with patch.object(devices,'run',side_effect=runner),patch.object(devices,'tool',return_value='/usr/bin/pleb'):
                self.assertEqual(devices.commit_preview(layout,inventory,Path(tmp)),1)
            self.assertEqual(seen[0][0],['pleb','displays','preview',str(Path(tmp)/'layout.json'),'--timeout','15'])
            self.assertNotIn('modes',seen[0][1])
            self.assertNotIn('xrandr',seen[0][1])
            self.assertFalse((Path(tmp)/'layout.json').exists())
            def interrupt(argv,**kwargs):
                self.assertTrue(Path(argv[3]).is_file())
                raise KeyboardInterrupt
            with patch.object(devices,'run',side_effect=interrupt),patch.object(devices,'tool',return_value='/usr/bin/pleb'):
                with self.assertRaises(KeyboardInterrupt):devices.commit_preview(layout,inventory,Path(tmp))
            self.assertFalse((Path(tmp)/'layout.json').exists())
            with patch.object(devices,'tool',side_effect=FileNotFoundError('pleb is missing. Install pleb using Merchant.')):
                with self.assertRaisesRegex(FileNotFoundError,'pleb is missing'):
                    devices.commit_preview(layout,inventory,Path(tmp))
            self.assertEqual(list(Path(tmp).iterdir()),[])
        refused=json.loads(json.dumps(layout))
        refused['outputs'][0]['enabled']=False
        refused['outputs'][0]['primary']=False
        with patch.object(devices,'run') as run,patch.object(devices,'tool') as tool:
            with self.assertRaisesRegex(ValueError,'Nothing was changed'):
                devices.commit_preview(refused,inventory)
            run.assert_not_called();tool.assert_not_called()

    def test_declined_display_confirmation_does_not_preview(self):
        inventory,layout=display_fixture()
        calls=[]
        def runner(argv,**kwargs):
            calls.append(argv)
            return json.dumps(inventory if argv[2]=='list' else layout)
        with patch.object(devices,'run',side_effect=runner),patch.object(devices,'pick',side_effect=[layout['outputs'][0],'mode','1280x720','60.00']),patch.object(devices,'confirm',side_effect=core.Cancelled):
            with self.assertRaises(core.Cancelled):devices.displays()
        self.assertEqual([argv[2] for argv in calls],['list','snapshot'])

    def test_blanking_preserves_settings_and_stay_awake_is_only_an_inhibitor(self):
        with self.assertRaisesRegex(ValueError,'owned by the screen-lock policy'):
            devices.blanking_argv('/xfce4-power-manager/lid-action-on-ac','uint','4')
        calls=[]
        def missing(argv):
            calls.append(argv)
            raise RuntimeError('xfconf-query exited with status 1.')
        with self.assertRaisesRegex(RuntimeError,'Nothing was changed'):
            devices.apply_blanking('/xfce4-power-manager/dpms-enabled','bool','false',missing)
        self.assertEqual(calls,[['xfconf-query','-c','xfce4-power-manager','-l']])
        calls.clear()
        def mismatch(argv):
            calls.append(list(argv))
            if argv[-1]=='-l':return '/xfce4-power-manager/dpms-enabled'
            if '--set' in argv:return ''
            return 'true'
        with self.assertRaisesRegex(RuntimeError,'previous value'):
            devices.apply_blanking('/xfce4-power-manager/dpms-enabled','bool','false',mismatch)
        self.assertEqual(calls[-1],['xfconf-query','-c','xfce4-power-manager','-p','/xfce4-power-manager/dpms-enabled','--create','--type','bool','--set','true'])
        with patch.object(devices,'confirm',side_effect=core.Cancelled),patch.object(devices,'run') as run:
            with self.assertRaises(core.Cancelled):devices.stay_awake()
            run.assert_not_called()
        with patch.object(devices,'confirm'),patch.object(devices,'run') as run,redirect_stdout(io.StringIO()):
            devices.stay_awake()
        self.assertEqual(run.call_args.args[0],devices.AWAKE)
        self.assertNotIn('xfconf-query',str(run.call_args.args[0]))

    def test_night_light_missing_tool_and_failure_leave_config_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            home=Path(tmp)
            with patch.object(devices,'tool',side_effect=FileNotFoundError('redshift is missing. Install redshift using Merchant.')):
                with self.assertRaisesRegex(FileNotFoundError,'redshift is missing'):
                    devices.apply_night_light(4500,True,home,lambda argv: None)
            self.assertEqual(list(home.rglob('*')),[])
            foreign=home/'redshift'
            foreign.mkdir();config=foreign/'redshift.conf';config.write_text('temp-day=3400\n')
            with patch.object(devices,'tool',return_value='/usr/bin/redshift'):
                with self.assertRaisesRegex(RuntimeError,'left unchanged'):
                    devices.apply_night_light(4500,True,home,lambda argv: None)
            self.assertEqual(config.read_text(),'temp-day=3400\n')
            config.write_text(devices.NIGHT_MARKER+'old\n')
            def fail(argv):
                raise RuntimeError('redshift exited with status 1.')
            with patch.object(devices,'tool',return_value='/usr/bin/redshift'):
                with self.assertRaisesRegex(RuntimeError,'exited'):
                    devices.apply_night_light(4500,True,home,fail)
            self.assertEqual(config.read_text(),devices.NIGHT_MARKER+'old\n')
            self.assertFalse((home/'autostart'/'kilix-zell-redshift.desktop').exists())
            calls=[]
            with patch.object(devices,'tool',return_value='/usr/bin/redshift'),redirect_stdout(io.StringIO()):
                devices.apply_night_light(4500,True,home,lambda argv: calls.append(argv))
            self.assertEqual(calls,[['redshift','-O','4500']])
            text=config.read_text()
            self.assertIn('temp-day=4500',text);self.assertIn('temp-night=4500',text)
            self.assertIn('Exec=redshift -c ',(home/'autostart'/'kilix-zell-redshift.desktop').read_text())
            self.assertEqual(config.stat().st_mode&0o777,0o600)

    def test_theme_and_font_read_back_restores_the_previous_value(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);theme=root/'Clearlooks';(theme/'gtk-3.0').mkdir(parents=True)
            (root/'linked').symlink_to(theme)
            self.assertEqual(devices.installed_choices('theme',[root]),['Clearlooks'])
        calls=[]
        def mismatch(argv):
            calls.append(list(argv))
            if argv[-1]=='-l':return '/Net/ThemeName'
            if '--set' in argv:return ''
            return 'Adwaita'
        with self.assertRaisesRegex(RuntimeError,'previous value'):
            devices.apply_theme('/Net/ThemeName','Clearlooks',mismatch)
        self.assertEqual(calls[-1][-1],'Adwaita')
        self.assertNotIn('xsettingsd',str(calls))
        calls.clear()
        def font(argv,**kwargs):
            calls.append(list(argv))
            if argv[2]=='show':return 'font_size 11.0\n'
            return 'font_size 14\n'
        with self.assertRaisesRegex(RuntimeError,'previous size'):
            devices.apply_font('14','11',font)
        self.assertEqual(calls,[['kilix','screen-size','set','14'],['kilix','screen-size','show'],['kilix','screen-size','set','11']])
        calls.clear()
        with redirect_stdout(io.StringIO()):
            self.assertEqual(devices.apply_font('14','11',lambda argv,**kwargs: calls.append(argv) or 'font_size 14\n'),'14')
        self.assertEqual(calls[0],['kilix','screen-size','set','14'])

if __name__=='__main__':unittest.main()
