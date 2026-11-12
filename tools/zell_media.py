"""Local creator subscriptions, recent uploads, and finite transcription jobs."""
from __future__ import annotations
import datetime as dt
import json
import hashlib
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from urllib.parse import parse_qs, urlparse

HOSTS = {'youtube.com', 'www.youtube.com', 'm.youtube.com', 'music.youtube.com', 'youtu.be'}
VIDEO_ID = re.compile(r'[A-Za-z0-9_-]{11}')


def clean(text):
    return ''.join(c for c in str(text) if c.isprintable())[:300]


def source_url(value):
    if value.startswith('@'): value='https://www.youtube.com/'+value
    p=urlparse(value)
    if p.scheme not in {'http','https'} or p.hostname not in HOSTS or p.username or p.password:
        raise ValueError('Enter a YouTube creator URL, @handle, or playlist URL.')
    playlist=parse_qs(p.query).get('list',[''])[0]
    if re.fullmatch(r'[A-Za-z0-9_-]+',playlist):
        return 'playlist','https://www.youtube.com/playlist?list='+playlist
    path=p.path.rstrip('/')
    path=re.sub(r'/(videos|shorts|streams|featured)$','',path)
    if re.fullmatch(r'/@[^/\s?#]+|/(channel|c|user)/[^/\s?#]+',path):
        return 'creator','https://www.youtube.com'+path
    raise ValueError('This is a video link. Follow its creator or a playlist instead.')


def capture(argv, timeout=60):
    """Keep tool errors and cancellation out of the interactive terminal."""
    process=subprocess.Popen(argv,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        output,error=process.communicate(timeout=timeout)
    except BaseException:
        process.terminate()
        try: process.communicate(timeout=2)
        except subprocess.TimeoutExpired: process.kill();process.communicate()
        raise
    if process.returncode: raise RuntimeError(clean(error[-1500:]) or f'{argv[0]} exited {process.returncode}')
    return output


def extract(url):
    if not shutil.which('yt-dlp'): raise RuntimeError('yt-dlp is not installed.')
    return json.loads(capture(['yt-dlp','--ignore-config','--flat-playlist','--dump-single-json',
        '--skip-download','--no-warnings','--socket-timeout','15','--retries','1',
        '--playlist-end','30','--',url],timeout=60))


def entries(info, source, discovered):
    result=[]
    for rank, item in enumerate(info.get('entries') or []):
        if not isinstance(item,dict): continue
        ident=item.get('id','')
        if not isinstance(ident,str) or not VIDEO_ID.fullmatch(ident):continue
        if item.get('availability') in {'private','premium_only','subscriber_only'}:continue
        title=clean(item.get('title') or ident)
        if title in {'[Private video]','[Deleted video]'}:continue
        published=item.get('timestamp') or item.get('release_timestamp') or 0
        if not isinstance(published,(int,float)):published=0
        if not published and re.fullmatch(r'\d{8}',str(item.get('upload_date',''))):
            try:published=dt.datetime.strptime(item['upload_date'],'%Y%m%d').replace(tzinfo=dt.timezone.utc).timestamp()
            except ValueError:pass
        result.append({'id':ident,'url':'https://www.youtube.com/watch?v='+ident,
            'title':title,'source':source['url'],'creator':clean(item.get('channel') or source['name']),
            'published':published,'discovered':discovered,'rank':rank})
    return result


class Library:
    def __init__(self, ui):
        self.ui=ui
        record=ui.state_read('media.json',{})
        self.data=record if isinstance(record,dict) else {}
        for key,default in [('sources',[]),('items',[]),('seen',[]),('refreshed',{})]:
            if not isinstance(self.data.get(key),type(default)):self.data[key]=default
        sources=[]
        legacy=ui.state_read('playlists.json',[])
        for row in self.data['sources']+(legacy if isinstance(legacy,list) else []):
            if not isinstance(row,dict):continue
            try:kind,url=source_url(row.get('url',''))
            except (ValueError,TypeError,AttributeError):continue
            if any(s['url']==url for s in sources):continue
            sources.append({'kind':kind,'url':url,'name':clean(row.get('name') or url)})
        self.data['sources']=sources
        self.data['items']=[r for r in self.data['items'] if isinstance(r,dict) and VIDEO_ID.fullmatch(str(r.get('id',''))) and any(s['url']==r.get('source') for s in sources)]
        self.data['schema']='kilix.zell.media/v1'

    def save(self):self.ui.state_write('media.json',self.data)

    def follow(self, value, name):
        kind,url=source_url(value)
        record={'kind':kind,'url':url,'name':clean(name or url)}
        self.data['sources']=[s for s in self.data['sources'] if s['url']!=url]+[record]
        self.save();return record

    def refresh(self, sources=None):
        errors=[]
        for source in sources if sources is not None else self.data['sources']:
            print('Checking '+source['name']+'...',flush=True)
            urls=[source['url']+'/'+tab for tab in ('videos','shorts','streams')] if source['kind']=='creator' else [source['url']]
            found=[];success=False
            for url in urls:
                try:
                    found.extend(entries(extract(url),source,time.time()));success=True
                except (RuntimeError,ValueError,subprocess.TimeoutExpired,OSError) as error:
                    errors.append(source['name']+': '+str(error))
            if not success:continue
            old={r['id']:r for r in self.data['items'] if r['source']==source['url']}
            for row in found:
                if row['id'] in old:row['discovered']=old[row['id']]['discovered']
                old[row['id']]=row
            retained=sorted(old.values(),key=lambda r:(r.get('published') or r['discovered'],-r.get('rank',0)),reverse=True)[:150]
            self.data['items']=[r for r in self.data['items'] if r['source']!=source['url']]+retained
            self.data['refreshed'][source['url']]=time.time()
            self.save()
        return errors

    def feed(self, source=None, unseen=False):
        seen=set(self.data['seen']);unique={}
        for row in self.data['items']:
            if source and row['source']!=source['url']:continue
            if unseen and row['id'] in seen:continue
            unique[row['id']]=row
        return sorted(unique.values(),key=lambda r:(r.get('published') or r['discovered'],-r.get('rank',0)),reverse=True)

    def mark_seen(self, item):
        self.data['seen']=list(dict.fromkeys(self.data['seen']+[item['id']]))[-10000:];self.save()

    def unfollow(self, source):
        self.data['sources']=[s for s in self.data['sources'] if s['url']!=source['url']]
        self.data['items']=[r for r in self.data['items'] if r['source']!=source['url']]
        # Remove the migrated legacy entry too, so it cannot reappear next time.
        old=self.ui.state_read('playlists.json',[])
        if isinstance(old,list):
            keep=[]
            for row in old:
                try:matches=isinstance(row,dict) and source_url(row.get('url',''))[1]==source['url']
                except (ValueError,TypeError,AttributeError):matches=False
                if not matches:keep.append(row)
            self.ui.state_write('playlists.json',keep)
        self.save()


def popup(ui, argv, name, env=None):
    if not shutil.which(argv[0]):print(f'{argv[0]} is not installed.');return False
    ui.BASE.mkdir(parents=True,exist_ok=True,mode=0o700)
    with (ui.BASE/'media-player.log').open('ab') as log:
        process=subprocess.Popen(argv,stdin=subprocess.DEVNULL,stdout=log,stderr=log,start_new_session=True,**({'env':env} if env else {}))
    try:status=process.wait(timeout=.25)
    except subprocess.TimeoutExpired:
        print(name+' opened.');return True
    if status:print(f'{name} could not open; see {ui.BASE / "media-player.log"}.');return False
    return True


def amp_audio(ui, target):
    """Amp requires files; download URL audio once into Zell's media cache."""
    if not ui.valid_url(str(target)):return str(Path(target).expanduser().resolve())
    if not shutil.which('yt-dlp') or not shutil.which('ffmpeg'):
        raise RuntimeError('URL audio needs yt-dlp and ffmpeg.')
    cache=ui.BASE/'media-cache';cache.mkdir(parents=True,exist_ok=True,mode=0o700)
    destination=cache/(hashlib.sha256(str(target).encode()).hexdigest()+'.mp3')
    if destination.is_file() and destination.stat().st_size:return str(destination)
    print('Preparing audio for Kilix Amp (Ctrl-C cancels)...',flush=True)
    with tempfile.TemporaryDirectory(prefix='download-',dir=cache) as temporary:
        capture(['yt-dlp','--ignore-config','--no-playlist','--no-warnings',
            '--socket-timeout','20','--retries','1','-f','bestaudio/best',
            '-x','--audio-format','mp3','-o',str(Path(temporary)/'audio.%(ext)s'),
            '--',str(target)],timeout=1800)
        audio=Path(temporary)/'audio.mp3'
        if not audio.is_file() or not audio.stat().st_size:raise RuntimeError('No audio was downloaded.')
        os.replace(audio,destination)
    return str(destination)


def amp(ui, target=None):
    try:target=amp_audio(ui,target) if target else None
    except (OSError,RuntimeError,subprocess.TimeoutExpired) as error:
        print('Could not prepare audio: '+str(error));return False
    argv=['kilix-amp']+([str(target)] if target else [])
    import zell_audio
    opened=popup(ui,argv,'Kilix Amp',env=zell_audio.amp_environment())
    if opened and target:zell_audio.dj_started()
    return opened


def play(ui, target, listen=False):
    ui.state_write('last-media.json',{'source':str(target),'time':time.time()})
    if listen:return amp(ui,target)
    player=shutil.which('kilix-mpv') or shutil.which('mpv')
    if not player:print('Install kilix-mpv or mpv.');return False
    return popup(ui,[player,'--force-window=yes','--',str(target)],'Video player')


def voice_runtime():
    base=Path(os.environ.get('GPU_TERMINAL_HOME',Path.home()/'.local/gpu_terminal'))
    voice=base/'kilix/data/voice'
    binary=Path(os.environ.get('KILIX_ZELL_WHISPER',voice/'whisper/current/bin/kilix-whisper-stt'))
    override=os.environ.get('KILIX_ZELL_WHISPER_MODEL')
    models=[Path(override)] if override else [voice/'models'/name for name in ('whisper-medium-en','faster-whisper-small-en')]+[base/'kilix/data/desktop-apps/assets'/name for name in ('faster-whisper-medium-en','faster-whisper-small-en')]
    model=next((p for p in models if all((p/f).is_file() for f in ('model.bin','config.json','tokenizer.json','vocabulary.txt'))),None)
    if not binary.is_file() or not os.access(binary,os.X_OK) or model is None:
        raise RuntimeError('Install a Kilix Whisper runtime/model with kilix-stt, or set KILIX_ZELL_WHISPER and KILIX_ZELL_WHISPER_MODEL.')
    return binary,model


def atomic_json(path, value):
    fd,name=tempfile.mkstemp(prefix='.status-',dir=path.parent)
    try:
        with os.fdopen(fd,'w') as f:json.dump(value,f,indent=2);f.write('\n')
        os.replace(name,path)
    finally:
        if os.path.exists(name):os.unlink(name)


def start_transcript(ui, source):
    if not ui.valid_url(source):source=str(Path(source).expanduser().resolve())
    try:voice_runtime()
    except RuntimeError as error:print(error);return None
    if not shutil.which('ffmpeg'):print('ffmpeg is not installed.');return None
    if not ui.valid_url(source) and not Path(source).is_file():print('Select a local file or media URL.');return None
    if ui.valid_url(source) and not shutil.which('yt-dlp'):print('yt-dlp is not installed.');return None
    root=ui.BASE/'transcripts';root.mkdir(parents=True,exist_ok=True,mode=0o700)
    job=Path(tempfile.mkdtemp(prefix=time.strftime('%Y%m%d-%H%M%S-'),dir=root))
    atomic_json(job/'job.json',{'source':source,'status':'queued','created':time.time()})
    try:
        with (job/'worker.log').open('ab') as log:
            worker=subprocess.Popen([sys.executable,str(Path(__file__).resolve()),'--worker',str(job)],stdin=subprocess.DEVNULL,stdout=log,stderr=log,start_new_session=True)
        (job/'pid').write_text(str(worker.pid))
    except OSError as error:
        atomic_json(job/'job.json',{'source':source,'status':'failed','error':str(error)});print(error);return None
    print(f'Transcription started on CPU. Read it under DJ > Transcripts.\n{job}')
    return job


def transcribe_worker(job):
    record=json.loads((job/'job.json').read_text());record.update(status='running',pid=os.getpid())
    from zell_schedule import process_identity
    record['process_identity']=process_identity(os.getpid())
    atomic_json(job/'job.json',record)
    def interrupted(_signal,_frame):raise KeyboardInterrupt
    signal.signal(signal.SIGTERM,interrupted)
    try:
        binary,model=voice_runtime();source=record['source']
        with tempfile.TemporaryDirectory(prefix='audio-',dir=job) as tmp:
            work=Path(tmp)
            if urlparse(source).scheme in {'http','https'}:
                capture(['yt-dlp','--ignore-config','--no-playlist','--no-warnings','--socket-timeout','15',
                    '--retries','2','-f','bestaudio/best','-o',str(work/'source.%(ext)s'),'--',source],timeout=1800)
                downloads=list(work.glob('source.*'))
                if len(downloads)!=1:raise RuntimeError('The media download did not produce one audio file.')
                source=str(downloads[0])
            capture(['ffmpeg','-nostdin','-hide_banner','-loglevel','error','-i',source,'-vn','-ac','1','-ar','16000',
                '-c:a','pcm_s16le','-f','segment','-segment_time','60','-reset_timestamps','1',str(work/'part-%05d.wav')],timeout=1800)
            chunks=sorted(work.glob('part-*.wav'))
            if not chunks:raise RuntimeError('No audio was found in this media.')
            with (job/'transcript.partial.txt').open('w') as output:
                output.write('Source: '+record['source']+'\nLocal Whisper transcription (English). Times mark one-minute chunks.\n\n')
                for index,chunk in enumerate(chunks):
                    record.update(completed_chunks=index,total_chunks=len(chunks));atomic_json(job/'job.json',record)
                    text=capture([str(binary),'transcribe','--model',str(model),'--audio',str(chunk),'--threads','4'],timeout=900).strip()
                    output.write(f'[{index//60:02d}:{index%60:02d}:00] {text}\n\n');output.flush()
            os.replace(job/'transcript.partial.txt',job/'transcript.txt')
        record.update(status='complete',completed_chunks=len(chunks),finished=time.time())
    except KeyboardInterrupt:record.update(status='cancelled',finished=time.time())
    except Exception as error:record.update(status='failed',error=clean(error),finished=time.time())
    atomic_json(job/'job.json',record)
    return 0 if record['status']=='complete' else 1


def jobs(ui):
    while True:
        rows=[]
        for path in sorted((ui.BASE/'transcripts').glob('*/job.json'),reverse=True):
            try:record=json.loads(path.read_text())
            except (OSError,ValueError):continue
            label=f"[{record.get('status')}] {clean(record.get('source',''))}"
            rows.append((label,(path.parent,record)))
        chosen=ui.choose('Transcripts / Enter back',rows)
        if not chosen:return
        job,record=chosen
        if record.get('status') in {'running','queued'}:
            action=ui.choose('Transcription is running',[('Read progress / partial text','read'),('Cancel this job','cancel')])
            if action=='cancel':
                try:
                    pid=int((job/'pid').read_text());cmd=Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
                    if str(job).encode() not in cmd or b'--worker' not in cmd:raise RuntimeError('The worker identity no longer matches.')
                    os.killpg(pid,signal.SIGTERM)
                except (OSError,ValueError,RuntimeError) as error:print(error)
            elif action=='read':
                text=(job/'transcript.partial.txt').read_text() if (job/'transcript.partial.txt').exists() else json.dumps(record,indent=2)
                ui.page_text(text)
        elif (job/'transcript.txt').exists():ui.page_text((job/'transcript.txt').read_text())
        else:ui.page_text(json.dumps(record,indent=2))


def item_menu(ui, library, item):
    while True:
        action=ui.choose(item['title'],[('Watch video','watch'),('Listen in Kilix Amp','listen'),
            ('Transcribe audio','transcribe'),('Watch and transcribe','watch_transcribe'),
            ('Listen and transcribe','listen_transcribe'),('Mark as seen','seen')])
        if action is None:return
        if 'transcribe' in action:start_transcript(ui,item['url'])
        if action.startswith('watch') or action.startswith('listen'):
            if play(ui,item['url'],listen=action.startswith('listen')):library.mark_seen(item)
        if action=='seen':library.mark_seen(item)


def browse(ui, library, source=None, unseen=False):
    while True:
        feed=library.feed(source,unseen)
        rows=[]
        for item in feed:
            when=dt.datetime.fromtimestamp(item['published']).strftime('%Y-%m-%d') if item.get('published') else 'date unavailable'
            new='NEW ' if item['id'] not in library.data['seen'] else ''
            rows.append((f"{new}{when} | {item['creator']} | {item['title']}",item))
        item=ui.choose('New content' if unseen else 'Recent content',rows)
        if item is None:return
        item_menu(ui,library,item)


def follow(ui, library):
    value=input('YouTube creator URL, @handle, or playlist URL: ').strip()
    if not value:return
    source=library.follow(value,input('Display name (optional): ').strip())
    print('Followed '+source['name'])
    for error in library.refresh([source]):print(error)


def hub(ui, add=False):
    library=Library(ui)
    if add:follow(ui,library)
    while True:
        count=len(library.feed(unseen=True))
        action=ui.choose('YouTube / playlists and creators',[(f'New content ({count})','new'),
            ('Refresh followed sources','refresh'),('All recent content','all'),('Follow creator or playlist','follow'),
            ('Manage followed sources','sources'),('Transcripts and jobs','jobs')])
        if action is None:return
        if action=='follow':follow(ui,library)
        elif action=='refresh':
            for error in library.refresh():print(error)
        elif action in {'new','all'}:browse(ui,library,unseen=action=='new')
        elif action=='jobs':jobs(ui)
        elif action=='sources':
            source=ui.choose('Followed sources',[(s['kind']+' | '+s['name'],s) for s in library.data['sources']])
            if not source:continue
            operation=ui.choose(source['name'],[('Browse cached uploads','browse'),('Refresh uploads','refresh'),('Unfollow','remove')])
            if operation=='browse':browse(ui,library,source)
            elif operation=='refresh':
                for error in library.refresh([source]):print(error)
            elif operation=='remove':library.unfollow(source)


if __name__=='__main__':
    if len(sys.argv)==3 and sys.argv[1]=='--worker':raise SystemExit(transcribe_worker(Path(sys.argv[2])))
    raise SystemExit('This helper is launched by the Zell DJ.')
