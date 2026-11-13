"""Zell-owned music players. DJ playback wins; mute pauses, never changes system volume."""
from __future__ import annotations
import json
import os
from pathlib import Path
import shutil
import socket
import stat
import subprocess
import time
from zell_os import read_state, write_state

def runtime():
    root=Path(os.environ.get('XDG_RUNTIME_DIR','/tmp'))/('zell-audio-'+str(os.getuid()))
    root.mkdir(mode=0o700,exist_ok=True)
    info=root.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid!=os.getuid() or info.st_mode&0o077:
        raise RuntimeError('Zell audio runtime directory is not private.')
    return root

def amp_environment():
    return {**os.environ,'KILIX_AMP_SOCKET':str(runtime()/'dj.sock')}

def rpc(path, request, timeout=.3):
    info=path.lstat()
    if not stat.S_ISSOCK(info.st_mode) or info.st_uid!=os.getuid():raise RuntimeError('Untrusted audio socket.')
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as channel:
        channel.settimeout(timeout);channel.connect(str(path))
        channel.sendall(json.dumps(request).encode()+b'\n')
        data=b''
        while len(data)<65536:
            if b'\n' not in data:
                chunk=channel.recv(4096)
                if not chunk:raise RuntimeError('Audio service disconnected.')
                data+=chunk
            if b'\n' in data:
                reply=json.loads(data.split(b'\n',1)[0])
                # mpv may emit property/events ahead of the command reply.
                if 'event' in reply:
                    data=data.split(b'\n',1)[1];continue
                return reply
    raise RuntimeError('Audio reply is too large.')

def amp(command):
    reply=rpc(runtime()/'dj.sock',{'cmd':command,'protocol':1})
    if reply.get('protocol')!=1 or not reply.get('ok'):raise RuntimeError('DJ service refused the operation.')
    return reply

def background_pause(paused):
    path=runtime()/'background.sock'
    if path.exists():rpc(path,{'command':['set_property','pause',bool(paused)]})

def state():
    value=read_state('room-music.json',{})
    return value if isinstance(value,dict) else {}

def _pid_alive(pid):
    if type(pid) is not int or pid<=0:return False
    try:os.kill(pid,0)
    except ProcessLookupError:return False
    except PermissionError:return True
    return True

def _theater_live(value):
    return _pid_alive(value.get('theater_owner'))

def _clear_theater_hold(value):
    """Release one playback's hold. Resume the DJ only if this hold paused it, it is still paused, and neither mute nor dj_held owns that pause."""
    if value.get('theater_paused_dj') and not value.get('muted') and not value.get('dj_held'):
        try:
            if amp('state').get('state')=='paused':amp('play')
        except (OSError,ValueError,RuntimeError):pass
    value.pop('theater_owner',None)
    value.pop('theater_paused_dj',None)

def theater_audio(active, owner):
    """Pause room music and a playing DJ for one theater owner.

    dj_active is left to the real Amp session. A different pid cannot release
    the hold, and a dead owner is cleared by MusicMonitor.poll as well as by
    the parent reap calling this with active False.
    """
    from zell_schedule import locked
    if type(owner) is not int or owner<=0:raise ValueError('theater audio owner must be a process id')
    with locked():
        value=state()
        dead=None
        if value.get('theater_owner') is not None and not _pid_alive(value.get('theater_owner')):
            dead=value.get('theater_owner')
            _clear_theater_hold(value)
        if not active:
            if value.get('theater_owner')!=owner:
                if dead is not None:
                    try:background_pause(bool(value.get('muted')) or bool(value.get('dj_active')))
                    except (OSError,ValueError,RuntimeError):pass
                    write_state('room-music.json',value)
                return dead==owner
            _clear_theater_hold(value)
        elif _theater_live(value) and value.get('theater_owner')!=owner:
            return False
        else:
            paused_dj=bool(value.get('theater_paused_dj')) and value.get('theater_owner')==owner
            try:
                current=amp('state').get('state')
                if current in {'playing','loading'}:
                    amp('pause');paused_dj=True
            except (OSError,ValueError,RuntimeError):pass
            value['theater_owner']=owner
            value['theater_paused_dj']=paused_dj
        try:background_pause(bool(value.get('muted')) or bool(value.get('dj_active')) or _theater_live(value) or bool(active and value.get('theater_owner')==owner))
        except (OSError,ValueError,RuntimeError):pass
        write_state('room-music.json',value)
        return True

def dj_started():
    from zell_schedule import locked
    with locked():
        value=state();value.update(muted=False,dj_active=True,dj_held=False)
        try:background_pause(True)
        except (OSError,ValueError,RuntimeError):pass
        write_state('room-music.json',value)

def toggle_mute():
    from zell_schedule import locked
    with locked():
        value=state();muted=not value.get('muted',False)
        try:
            current=amp('state').get('state')
            if muted and current in {'playing','loading'}:
                amp('pause');value.update(dj_held=True,dj_pause_seen=False,mute_requested=time.time())
            elif muted and value.get('theater_paused_dj') and current=='paused':
                # The theater already paused this DJ. Mute takes the resume.
                value.update(dj_held=True,dj_pause_seen=False,mute_requested=time.time())
            elif not muted and value.get('dj_held') and current=='paused':
                if _theater_live(value):
                    # Mute owned this pause. The live theater takes the one resume.
                    value['dj_held']=False
                    value['theater_paused_dj']=True
                else:
                    amp('play');value['dj_held']=False
        except (OSError,ValueError,RuntimeError):pass
        value['muted']=muted
        try:background_pause(muted or value.get('dj_active',False) or _theater_live(value))
        except (OSError,ValueError,RuntimeError):pass
        write_state('room-music.json',value)
    return muted

class MusicMonitor:
    def __init__(self):
        self.process=None;self.source=None;self.last_pause=None

    def close(self):
        if self.process is not None:
            self.process.terminate()
            try:self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:self.process.kill();self.process.wait()
            self.process=None
        self.source=None

    def poll(self):
        from zell_schedule import locked
        with locked():
            value=state()
            if value.get('theater_owner') is not None and not _pid_alive(value.get('theater_owner')):
                _clear_theater_hold(value)
                write_state('room-music.json',value)
            previous=dict(value)
            try:
                current=amp('state').get('state')
                active=current in {'playing','paused','loading'}
                if current=='paused' and value.get('dj_held'):value['dj_pause_seen']=True
                # Play in the DJ's own GUI also clears the switch.
                if current in {'playing','loading'} and value.get('muted') and (
                        not value.get('dj_held') or value.get('dj_pause_seen') or time.time()-value.get('mute_requested',0)>1):
                    value['muted']=False;value['dj_held']=False
            except (OSError,ValueError,RuntimeError):active=False
            if active!=value.get('dj_active'):
                value['dj_active']=active
            if value!=previous:write_state('room-music.json',value)
        source=value.get('source')
        paused=value.get('muted',False) or active or _theater_live(value)
        if self.process is not None and self.process.poll() is not None:
            self.process=None;self.source=None
        if source!=self.source:
            self.close()
            if source and Path(source).is_file() and shutil.which('mpv'):
                path=runtime()/'background.sock'
                if path.exists():
                    # Never remove another live player's endpoint.
                    try:rpc(path,{'command':['get_property','pause']});return value
                    except (OSError,ValueError,RuntimeError):
                        if path.is_socket() and path.lstat().st_uid==os.getuid():path.unlink()
                self.process=subprocess.Popen(['mpv','--no-config','--no-video','--idle=no','--loop-file=inf',
                    '--pause='+('yes' if paused else 'no'),'--volume='+str(max(0,min(100,int(value.get('volume',30))))),
                    '--input-ipc-server='+str(path),'--',source],stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                self.source=source;self.last_pause=None
        if paused!=self.last_pause:
            try:background_pause(paused);self.last_pause=paused
            except (OSError,ValueError,RuntimeError):pass
        return value

def configure(ui):
    choice=ui.choose('Zell background music',[('Choose a local music track','choose'),('Stop background music','stop'),('Background volume','volume')])
    if choice is None:return
    updates={}
    if choice=='choose':
        path=ui.file_pick(Path.home(),{'.mp3','.ogg','.flac','.wav','.opus','.m4a'})
        if path is None:return
        updates['source']=str(path.resolve())
        ui.page_text('This track loops quietly in Zell. DJ playback pauses it. The music switch pauses the active music.')
    elif choice=='stop':updates['source']=None
    else:
        volume=input('Background volume (0-100): ').strip()
        if not volume.isdigit() or not 0<=int(volume)<=100:raise ValueError('Choose a volume from 0 to 100.')
        updates['volume']=int(volume)
        try:rpc(runtime()/'background.sock',{'command':['set_property','volume',int(volume)]})
        except (OSError,ValueError,RuntimeError):pass
    from zell_schedule import locked
    with locked():
        value=state();value.update(updates);write_state('room-music.json',value)
