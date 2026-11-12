#!/usr/bin/env python3
"""Zell's interactive desktop services. Commands are argv lists, never shell text."""
from __future__ import annotations
import calendar
import configparser
import datetime
import json
import os
from pathlib import Path
import re
import signal
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from urllib.parse import parse_qs, urlparse
from types import SimpleNamespace
sys.path.insert(0, str(Path(__file__).resolve().parent))
import zell_media
import zell_library
import zell_vitals
import zell_debug
import fcntl
import struct
import termios
import zell_rooms
import zell_terminal
import zell_projects
import zell_weather
import zell_os
import zell_audio
import zell_agents
from zell_dialogue import Dialogue
DIALOGUE = None

BASE = Path(os.environ.get('KILIX_ZELL_STATE', Path(os.environ.get('GPU_TERMINAL_HOME', Path.home()/'.local/gpu_terminal'))/'kilix-zell/state'))

def state_write(name, value):
    BASE.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd, temporary = tempfile.mkstemp(prefix='.'+name, dir=BASE)
    try:
        with os.fdopen(fd,'w') as f:
            json.dump(value, f, indent=2); f.write('\n')
        os.replace(temporary, BASE/name)
    finally:
        if os.path.exists(temporary): os.unlink(temporary)

def state_read(name, default):
    try: return json.loads((BASE/name).read_text())
    except (OSError, ValueError): return default

def open_tab(argv, title=None):
    command=['kilix','new-tab','--title',title or 'Zell / '+Path(argv[0]).name,'--',
             sys.executable,str(Path(__file__).resolve()),'--terminal',json.dumps(argv)]
    result=subprocess.run(command,capture_output=True,text=True,timeout=20)
    print('Opened in a Kilix tab. Return to the cave tab when ready.' if result.returncode==0
          else 'Could not open a Kilix tab: '+(result.stderr or result.stdout))
    return result.returncode


def fit_neuronte(argv, size=None):
    """Size a Neuronte display command to the tab it is about to fill.

    neuronte_display.py draws a fixed grid (80x25 unless told otherwise), which
    is a small picture in the corner of a full tab. Called inside the new tab,
    this asks the terminal for its pixel size and passes the grid that fits.
    """
    if len(argv)<2 or Path(argv[1]).name!='neuronte_display.py':return argv
    options=argv[2:argv.index('--')] if '--' in argv else argv[2:]
    if '--cols' in options or '--rows' in options:return argv
    if size is None:
        try:
            _,_,width,height=struct.unpack('HHHH',fcntl.ioctl(sys.stdout.fileno(),termios.TIOCGWINSZ,bytes(8)))
        except (OSError,ValueError):return argv
    else:width,height=size
    if width<=0 or height<=0:return argv  # the terminal did not report pixels
    # 8x16-pixel cells; the display accepts 20..240 columns and 18..120 rows.
    cols=max(20,min(240,width//8));rows=max(18,min(120,height//16))
    return [*argv[:2],'--cols',str(cols),'--rows',str(rows),*argv[2:]]


def run(argv, *, pause=True):
    if not shutil.which(argv[0]):
        print(f'{argv[0]} is not installed. Use Administrator to install the tool.'); return 127
    if DIALOGUE:
        interactive={'kilix-tui','top','htop','btop','alsamixer','nano','vim','vi','nvim','less','kilix-launcher'}
        external=(Path(argv[0]).name in interactive or argv[0]=='sudo' or
                  (argv[0]=='kilix' and len(argv)>1 and argv[1] in {'shell','settings','app'}))
        if external:return open_tab(argv)
        if Path(argv[0]).name in {'journalctl','systemctl'}:argv=[*argv,'--no-pager']
        print('Working...')
        process=subprocess.Popen(argv,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        # Drain all output, keeping only a bounded preview in memory.
        preview='';truncated=False
        for line in iter(lambda:process.stdout.readline(4096),''):
            if len(preview)<5200:preview+=line[:5200-len(preview)]
            else:truncated=True
        process.stdout.close()
        result=process.wait()
        DIALOGUE.page(preview+('\n[Output truncated.]' if truncated else '')+f'\nExit status: {result}')
        return result
    process=subprocess.Popen(argv)
    try: result=process.wait()
    except KeyboardInterrupt:
        process.terminate()
        try: process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait()
        result=130
    if result: print(f'Command exited with status {result}.')
    if pause: input('\nEnter to return to Zell... ')
    return result

def choose(title, entries):
    if DIALOGUE:return DIALOGUE.choose(title,entries)
    if not entries: print('Nothing found.'); return None
    query=''; page=0
    while True:
        rows=[e for e in entries if query.casefold() in str(e[0]).casefold()]
        print('\n'+title+'\n'+'─'*50)
        for i,(label,_) in enumerate(rows[page:page+18],page+1): print(f'{i:3}  {label}')
        print(f'{len(rows)} results. /text filter, n next, p previous, Enter back')
        answer=input('Choose: ').strip()
        if not answer:return None
        if answer.startswith('/'):query=answer[1:];page=0
        elif answer=='n':page=min(max(0,len(rows)-1)//18*18,page+18)
        elif answer=='p':page=max(0,page-18)
        elif answer.isdigit() and 0<int(answer)<=len(rows): return rows[int(answer)-1][1]

def page_text(text):
    if DIALOGUE:return DIALOGUE.page(text)
    if shutil.which('less'):
        subprocess.run(['less','-R'],input=text.encode(),check=False)
    else: print(text);input('\nEnter to continue... ')

def file_pick(root, suffixes=None):
    root=Path(root).expanduser()
    while True:
        try: children=sorted(root.iterdir(),key=lambda p:(not p.is_dir(),p.name.casefold()))
        except OSError as e: print(e);return None
        entries=[('../',root.parent)]
        entries += [(p.name+('/' if p.is_dir() else ''),p) for p in children if not p.name.startswith('.') and (p.is_dir() or suffixes is None or p.suffix.lower() in suffixes)]
        picked=choose(str(root),entries)
        if picked is None:return None
        if picked.is_dir(): root=picked
        else:return picked

def open_file(path):
    if path is None:return
    if path.suffix.lower() in {'.txt','.md','.json','.jsonl','.log','.csv','.py','.c','.h'}:
        if DIALOGUE:
            with path.open(errors='replace') as stream:preview=stream.read(5501)
            page_text(preview)
            if choose(path.name,[('Open full file in a Kilix tab','open')])=='open':open_tab(['less','-R','--',str(path)])
        else:run(['less','-R','--',str(path)])
    else:run(['xdg-open',str(path)])

def log_roots():
    home=Path.home()
    return [home/'.grok/sessions',home/'.codex/sessions',home/'.local/gpu_terminal/kilix/state/transcripts',home/'.local/state/kilix']

def log_files(roots):
    rows=[]
    for root in roots:
        if not root.exists():continue
        for directory, dirs, files in os.walk(root,followlinks=False):
            dirs[:]=[d for d in dirs if d not in {'node_modules','.git','compaction_checkpoints'} and not (Path(directory)/d).is_symlink()]
            for name in files:
                p=Path(directory)/name
                if p.is_symlink() or p.suffix not in {'.log','.jsonl','.txt'}:continue
                try:rows.append((p.stat().st_mtime,p))
                except OSError:pass
            if len(rows)>20000:break
    return [(str(p.relative_to(Path.home())),p) for _,p in sorted(rows,reverse=True)]

def logs(action):
    if action==6:return open_file(file_pick(Path('/var/log')))
    if action<3:
        options=[['--user','-n','200'],['--user','-u','*','-n','200'],['-b','-p','warning','-n','200']][action]
        if action==0:options=['-n','200']
        return run(['journalctl',*options])
    roots=log_roots()
    selected=choose('Recent logs (newest first)',log_files(roots[:2] if action==3 else roots[2:] if action==4 else roots))
    if not selected:return
    if action==5:
        needle=input('Literal search text: ').strip()
        if not needle:return
        found=[]
        with selected.open(errors='replace') as f:
            for number,line in enumerate(f,1):
                if needle.casefold() in line.casefold():found.append(f'{number}: {line[:2000]}')
                if len(found)>=500:found.append('Stopped at 500 matches.');break
        page_text(''.join(found) or 'No matches.')
    else:open_file(selected)

def valid_package(name):return bool(re.fullmatch(r'[a-z0-9][a-z0-9+.-]*(?::[a-z0-9]+)?',name))
def admin(action):
    if action==6:return zell_terminal.run_update(sys.modules[__name__])
    if action==7:return page_text(vitals_snapshot()['details'])
    if action==0:return run(['kilix','settings'])
    if action==1:
        packages=input('Debian package names (space separated): ').split()
        if not packages or not all(valid_package(p) for p in packages):print('Invalid package name.');return
        if run(['apt-get','--simulate','install',*packages],pause=False):return
        if input('Install the packages shown above? Type install: ')=='install':run(['sudo','apt-get','install',*packages])
    elif action==2:run(['dpkg-query','-W'])
    elif action==3:
        result=subprocess.run(['systemctl','--user','list-units','--type=service','--all','--no-legend','--plain'],capture_output=True,text=True)
        if result.returncode:print(result.stderr);return
        units=[(line.strip(),line.split()[0]) for line in result.stdout.splitlines() if line.split()]
        unit=choose('User services',units)
        if unit:
            operation=choose(unit,[(name,name) for name in ['status','start','restart','stop']])
            if operation and (operation=='status' or input(f'{operation} {unit}? Type yes: ')=='yes'):
                run(['systemctl','--user',operation,unit])
    elif action==4:run(['ip','-brief','address'])
    elif action==5:
        print('Current Zell settings:',state_read('model.json',{'model':'auto','device':'auto'}))
        model=choose('Model',[('Automatic (Ask first, 10 second larger-model limit)','auto'),('Qwen 3.5 0.8B no-loop','qwen-0.8b-no-loop-02'),('Qwen 3.5 4B','qwen3.5-4b')])
        if model is None:return
        run(['kilix-llm','devices'],pause=False)
        device=input('Device (auto or VulkanN; Enter=auto): ').strip() or 'auto'
        if not re.fullmatch(r'auto|Vulkan[0-9]+',device):print('Invalid device.');return
        state_write('model.json',{'schema':'kilix.zell.model/v1','model':model,'device':device});print('Saved. The next question uses this selection.')

def health():
    lines=[datetime.datetime.now().strftime('%Y-%m-%d %H:%M'),f'Load averages: {os.getloadavg()}']
    for root in ('/',str(Path.home())):
        total,used,free=shutil.disk_usage(root);lines.append(f'{root}: {free/2**30:.1f} GiB free / {total/2**30:.1f} GiB total')
    try:
        lines.extend(line.strip() for line in Path('/proc/meminfo').read_text().splitlines() if line.startswith(('MemTotal:','MemAvailable:','SwapFree:')))
        lines.append(f'Uptime: {float(Path("/proc/uptime").read_text().split()[0])/3600:.1f} hours')
    except OSError:pass
    if shutil.which('nvidia-smi'):
        p=subprocess.run(['nvidia-smi','--query-gpu=name,memory.used,power.draw,temperature.gpu','--format=csv'],capture_output=True,text=True);lines.append(p.stdout or p.stderr)
    page_text('\n'.join(lines))

def cleanup_candidates(root, now=None):
    now=time.time() if now is None else now
    if root.is_symlink() or not root.exists():return []
    results=[]
    for directory,dirs,files in os.walk(root,followlinks=False):
        dirs[:]=[d for d in dirs if not (Path(directory)/d).is_symlink()]
        for name in files:
            p=Path(directory)/name
            try:
                st=p.lstat()
                if p.is_file() and not p.is_symlink() and now-st.st_mtime>30*86400:results.append((p,st.st_size,st.st_mtime_ns))
            except OSError:pass
    return results

def housekeeping(action):
    if action==0:return health()
    if action==1:return run(['df','-h'])
    if action==2:return run(['du','-x','-h','--max-depth=1',str(Path.home())])
    if action==4:return run(['top'])
    if action==5:
        run(['du','-sh','/var/cache/apt/archives'],pause=False)
        print('apt-get clean removes downloaded package archives, not installed software.')
        if input('Clean the downloaded package cache? Type clean: ')=='clean':run(['sudo','apt-get','clean'])
        return
    root=Path.home()/'.cache/thumbnails'
    rows=cleanup_candidates(root)
    print(f'{len(rows)} thumbnails older than 30 days: {sum(r[1] for r in rows)/2**20:.2f} MiB')
    for p,size,_ in rows[:100]:print(f'{size:9} {p}')
    if len(rows)>100:print(f'...and {len(rows)-100} more files under {root}')
    if not rows or input('Permanently remove these cached thumbnails? Type clean: ')!='clean':return
    removed=0
    for p,size,stamp in rows:
        try:
            if root.is_symlink() or any(q.is_symlink() for q in p.parents if q!=Path('/')):continue
            st=p.lstat()
            if not p.is_symlink() and st.st_mtime_ns==stamp and st.st_size==size:p.unlink();removed+=1
        except OSError:pass
    print(f'Removed {removed} old cached thumbnails. Personal files were not selected.')

def valid_url(url):
    parsed=urlparse(url)
    return parsed.scheme in {'https','http'} and bool(parsed.hostname) and not parsed.username and not parsed.password

def valid_playlist_url(url):
    parsed=urlparse(url)
    return valid_url(url) and parsed.hostname in {'youtube.com','www.youtube.com','music.youtube.com','youtu.be'} and bool(parse_qs(parsed.query).get('list'))

def media_ui():
    return SimpleNamespace(BASE=BASE,state_read=state_read,state_write=state_write,
                           choose=choose,page_text=page_text,valid_url=valid_url)

def play(target):
    return zell_media.play(media_ui(),str(target))

def music(action):
    if action==8:return zell_audio.configure(sys.modules[__name__])
    if action==7:return zell_library.hub(sys.modules[__name__],'music')
    ui=media_ui()
    if action==0:return zell_media.amp(ui)
    if action==2:return zell_media.hub(ui)
    if action==3:return zell_media.hub(ui,add=True)
    if action==4:return run(['alsamixer'])
    if action==6:return zell_media.jobs(ui)
    if action==5:
        last=state_read('last-media.json',{})
        source=last.get('source','') if isinstance(last,dict) else ''
        target=input(f'Media path or URL (Enter uses {source or "last played"}): ').strip() or source
        if target:zell_media.start_transcript(ui,target)
        return
    target=input('Local media path or HTTP(S) URL: ').strip()
    if not valid_url(target):
        path=Path(target).expanduser()
        if not target or not path.is_file():print('Enter an existing file or an HTTP(S) URL.');return
        target=str(path.resolve())
    action=choose('Open media',[('Watch video','watch'),('Listen in Kilix Amp','listen'),
        ('Transcribe audio','transcribe'),('Watch and transcribe','watch_transcribe'),
        ('Listen and transcribe','listen_transcribe')])
    if not action:return
    if 'transcribe' in action:zell_media.start_transcript(ui,target)
    if action.startswith('watch') or action.startswith('listen'):
        zell_media.play(ui,target,listen=action.startswith('listen'))

def find_file():
    query=input('Filename contains: ').strip().casefold()
    if not query:return
    results=[]
    for directory,dirs,files in os.walk(Path.home(),followlinks=False):
        dirs[:]=[d for d in dirs if not d.startswith('.') and d not in {'node_modules','build','target'} and not (Path(directory)/d).is_symlink()]
        for name in files:
            if query in name.casefold():results.append((str(Path(directory)/name),Path(directory)/name))
        if len(results)>=1000:break
    open_file(choose('Matching files (up to 1000)',results[:1000]))

def library(action):
    if action==0:return zell_library.hub(sys.modules[__name__],'documents')
    if action==7:return zell_library.guide(sys.modules[__name__],'documents')
    if action==1:return find_file()
    if action==5:return run(['kilix','app','run','kilix-pdf-conversion'])
    p=file_pick(Path.home())
    if not p:return
    if action==2:return run(['kilix','app','run','kilix-pdf-conversion','--','ocr',str(p)])
    if action==3:
        dest=p.with_name(p.stem+'-ocr.pdf')
        if dest.exists():print(f'{dest} already exists; choose a different input or rename the output.');return
        if input(f'Create searchable PDF {dest}? Type create: ')=='create':run(['ocrmypdf','--skip-text',str(p),str(dest)])
    if action in {4,6}:
        if p.suffix.lower() not in {'.txt','.md'}:print('Choose a text or Markdown file. Use OCR for scanned documents first.');return
        argv=['kilix-tts','--speak','-']
        if action==6:
            output=p.with_name(p.stem+'-audiobook.wav')
            if output.exists():print(f'{output} exists; refusing to overwrite it.');return
            if input(f'Create {output}? Type create: ')!='create':return
            argv+=['--output',str(output)]
        if not shutil.which('kilix-tts'):print('kilix-tts is not installed.');return
        with p.open('rb') as source:
            result=subprocess.run(argv,stdin=source,check=False,capture_output=bool(DIALOGUE))
        if result.returncode:print(f'Speech failed (status {result.returncode}).')

def notes(action):
    if action==3:
        from zell_journal import tasks
        return tasks(sys.modules[__name__])
    if action==4:
        from zell_journal import conversations
        return conversations(sys.modules[__name__])
    BASE.mkdir(parents=True,exist_ok=True,mode=0o700);path=BASE/'notebook.txt'
    if action==0:return page_text(path.read_text() if path.exists() else 'Your notebook is empty.')
    if action==1:
        text=input('Note: ').strip()
        if text:
            fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_APPEND,0o600)
            with os.fdopen(fd,'a') as f:f.write(datetime.datetime.now().strftime('%Y-%m-%d %H:%M')+'  '+text+'\n')
    else:
        argv=[*shlex.split(os.environ.get('EDITOR','nano')),str(path)]
        open_tab(argv) if DIALOGUE else run(argv)

def applications():
    if shutil.which('kilix-launcher'):return run(['kilix-launcher'])
    roots=[Path(os.environ.get('XDG_DATA_HOME',Path.home()/'.local/share'))]
    roots += [Path(p) for p in os.environ.get('XDG_DATA_DIRS','/usr/local/share:/usr/share').split(':')]
    entries=[];seen=set()
    for root in roots:
        for path in sorted((root/'applications').glob('*.desktop')):
            if path.name in seen:continue
            seen.add(path.name)
            parser=configparser.ConfigParser(interpolation=None,strict=False)
            try:
                parser.read(path);entry=parser['Desktop Entry']
                if entry.get('Type')!='Application' or entry.get('Hidden','false')=='true' or entry.get('NoDisplay','false')=='true':continue
                if entry.get('TryExec') and not shutil.which(entry['TryExec']):continue
                if entry.get('Exec'):entries.append((entry.get('Name',path.stem),path))
            except (OSError,configparser.Error,KeyError):continue
    choice=choose('Applications',sorted(entries,key=lambda e:e[0].casefold()))
    if choice:run(['gio','launch',str(choice)])

def neuronte_shell():
    try:
        display=zell_terminal.load_display()
        if not (Path(display.__file__).parent/'build/libneuronte-display.so').is_file():
            raise RuntimeError('Neuronte presenter is missing.')
    except (RuntimeError, ImportError, OSError):
        return page_text('Neuronte terminal is unavailable. Install the complete Zell runtime payload.')
    shell=os.environ.get('SHELL') or '/bin/bash'
    if not Path(shell).is_file() or not os.access(shell,os.X_OK):shell='/bin/sh'
    print('Neuronte shell: Ctrl-A then q exits; Ctrl-A then n opens a terminal tab. '
          'Return to the cave tab to continue walking.')
    return open_tab([sys.executable,str(Path(__file__).with_name('zell_shell.py')),'--',shell,'-i'],title='Zell / Neuronte shell')


LAST_VITALS=None
def vitals_snapshot():
    ui=sys.modules[__name__]
    status=zell_vitals.snapshot(ui)
    from zell_system import restart_status
    restart=restart_status()
    status['restart_needed']=int(restart['needed'])
    status['details']+='\n\n'+restart['text']
    if restart['needed']:status['health']=min(status['health'] if status['health']>=0 else 100,85)
    watched=zell_projects.dirty_summary(ui)
    base=status['health'];count=watched['count']
    status['os_health']=base
    if count:status['health']=max(1,(base if base>=0 else 100)-count)
    status['dirty_files']=count if watched['known'] else -1
    global LAST_VITALS
    summary={k:status[k] for k in ('health','os_health','dirty_files','fatigue','mana','age_days')}
    # Fatigue moves every sample; log only when something else changed.
    if zell_debug.enabled() and {k:v for k,v in summary.items() if k!='fatigue'}!=LAST_VITALS:
        LAST_VITALS={k:v for k,v in summary.items() if k!='fatigue'};zell_debug.log('vitals',**summary)
    status['details']+=f"\n\nCombined health: {status['health']}%; Git penalty: {count} percentage points.\n"+watched['details']
    if base<0 and count:status['details']+='\nOS age unknown: only the known Git penalty is applied to 100%.'
    return status


def desktop(station,action):
    ui=sys.modules.get(__name__) or SimpleNamespace(**globals())
    if station==46:return zell_agents.toggle_yolo(ui)
    if station==47:return zell_agents.updates(ui)
    if station in zell_os.STATIONS:return zell_os.hub(ui,zell_os.STATIONS[station],action)
    if station==45:
        muted=zell_audio.toggle_mute()
        if DIALOGUE:DIALOGUE.send('A',str(int(muted))+' '+str(int(zell_audio.state().get('dj_active',False))))
        return
    if station==0:return zell_os.hub(ui,'wizard',0)
    if station==2:
        if action==1:return zell_os.hub(ui,'merchant')
        if action==2:return zell_os.hub(ui,'admin',1)
        if action==3:return zell_os.hub(ui,'admin',0)
        if action==4:return zell_os.hub(ui,'signalman')
    if station==3 and action>=6:return zell_os.hub(ui,'housekeeper',action-6)
    if station==9 and action==5:return zell_os.hub(ui,'notices',0)
    if station==8 and action==5:
        return zell_os.hub(ui,'tinkerer',8)
    if station==31:
        from zell_secrets import hub
        return hub(sys.modules[__name__])
    if station==-1:
        ui=sys.modules[__name__]
        audio_monitor=zell_audio.MusicMonitor()
        import atexit,threading
        atexit.register(audio_monitor.close)
        audio_stop=threading.Event()
        def audio_updates():
            previous=None
            previous_yolo=None
            while not audio_stop.is_set():
                try:
                    audio=audio_monitor.poll()
                    value=str(int(audio.get('muted',False)))+' '+str(int(audio.get('dj_active',False)))
                    if value!=previous:DIALOGUE.send('A',value);previous=value
                except (OSError,ValueError,RuntimeError,subprocess.SubprocessError):pass
                # A room-entry request forces a fresh reading, even when the
                # file has not changed since the previous room visit.
                import select
                refresh=False
                if select.select([DIALOGUE.reader],[],[],.25)[0]:
                    request=os.read(DIALOGUE.reader.fileno(),128)
                    if not request:return
                    refresh=b'Y' in request
                yolo=zell_agents.yolo_state()
                if refresh or yolo!=previous_yolo:
                    DIALOGUE.send('Y',str(yolo));previous_yolo=yolo
        audio_thread=threading.Thread(target=audio_updates,daemon=True);audio_thread.start()
        def stop_monitor(_signum,_frame):
            audio_stop.set();audio_thread.join(timeout=1);audio_monitor.close();raise SystemExit(0)
        signal.signal(signal.SIGTERM,stop_monitor)
        signal.signal(signal.SIGHUP,stop_monitor)
        while True:
            status=vitals_snapshot()
            DIALOGUE.send('V',' '.join(str(status[k]) for k in ('health','fatigue','mana','age_days','mana_age','os_health','dirty_files')))
            DIALOGUE.send('J',str(status['restart_needed']))
            from zell_schedule import aggregate
            notices=aggregate();DIALOGUE.send('N',str(notices['count'])+' '+notices['text'])
            weather=zell_weather.snapshot()
            DIALOGUE.send('W',f"{weather['state']} {weather['icon']} {weather['temperature']}\n{weather['place']}\n{weather['condition']}")
            try:drives=zell_rooms.disks()
            except OSError:drives=[]
            DIALOGUE.send('D')
            for index,disk in enumerate(drives[:8]):
                used=disk.get('used_percent');percent=round(used) if isinstance(used,(int,float)) else -1
                label=disk['label'] if index<7 or len(drives)<=8 else 'More drives (Enter)'
                DIALOGUE.send('B',f'{percent} {label}')
            for index,screen in enumerate(zell_rooms.previews(ui)):
                DIALOGUE.send('S',f"{index} {screen['status']}\n{screen['text']}")
            time.sleep(5)
    if station==29:return zell_projects.hub(sys.modules[__name__])
    if station==30:return zell_rooms.pilot(sys.modules[__name__])
    if 15<=station<=19:return zell_rooms.screen(sys.modules[__name__],station-15)
    if 20<=station<=27:return zell_rooms.laundry(sys.modules[__name__],station-20)
    if station==12:return neuronte_shell()
    if station==1:return logs(action)
    if station==2:return admin(action)
    if station==3:return housekeeping(action)
    if station==4:return music(action)
    if station==5:return library(action)
    if station==6:return run(['kilix-tui'],pause=False)
    if station==7:return run(['kilix','shell'],pause=False)
    if station==8:
        if action==4:return zell_weather.report(sys.modules[__name__])
        if action==0:return applications()
        if action==1:return open_file(file_pick(Path.home()))
        if action==2:return open_file(file_pick(Path.home()/'Downloads'))
        if action==3:
            url=input('HTTP(S) URL: ').strip()
            if valid_url(url):return run(['xdg-open',url])
            print('Invalid URL.');return
    if station==9:return notes(action)
    if station==11:
        if action==2:return zell_vitals.request_mana(sys.modules[__name__])
        if action==1:return health()
        now=datetime.datetime.now()
        return page_text(now.strftime('%A %d %B %Y, %H:%M:%S')+'\n\n'+calendar.month(now.year,now.month))
    raise ValueError('Unknown station')

if __name__=='__main__':
    if len(sys.argv)>2 and sys.argv[1]=='--terminal':
        argv=fit_neuronte(json.loads(sys.argv[2]))
        zell_debug.log('tab.command',argv=argv)
        try:
            status=subprocess.call(argv)
            input(f'\nCommand exited {status}. Enter to close this tab... ')
        except (EOFError,KeyboardInterrupt):pass
        raise SystemExit(0)
    if '--dialogue' in sys.argv:
        DIALOGUE=Dialogue();DIALOGUE.install()
    try:
        station,action=map(int,sys.argv[1:3])
        if DIALOGUE:
            from zell_journal import recorder
            DIALOGUE.record=recorder(sys.modules[__name__],station)
        zell_debug.log('helper.start',station=station,action=action,dialogue=bool(DIALOGUE))
        if not DIALOGUE:print('\033[2J\033[H'+'ZELL NEURONTE 0.1.1 / DESKTOP SERVICES\n')
        desktop(station,action)
        zell_debug.log('helper.done')
    except (EOFError,KeyboardInterrupt):zell_debug.log('helper.cancelled')
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:
        zell_debug.error('helper.failed')
        print(f'Could not complete the action: {error}')
    except BaseException:
        zell_debug.error('helper.crashed')
        raise
    if not DIALOGUE and len(sys.argv)>1 and sys.argv[1] not in {'6','7'}:
        try:input('\nEnter to return to the cave... ')
        except (EOFError,KeyboardInterrupt):pass
