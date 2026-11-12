"""Persistent user timers, bounded notices and identity-checked job cancellation."""
from __future__ import annotations
from contextlib import contextmanager
from datetime import datetime, timedelta
import fcntl
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
import uuid
from zell_os import task, ask, pick, confirm, run, tool, positive, read_state, write_state, state_root, Cancelled

@contextmanager
def locked():
    root=state_root();root.mkdir(parents=True,exist_ok=True,mode=0o700)
    fd=os.open(root/'.os.lock',os.O_CREAT|os.O_RDWR|os.O_NOFOLLOW,0o600)
    try:
        fcntl.flock(fd,fcntl.LOCK_EX);yield
    finally:os.close(fd)

def quiet_now(now=None):
    config=read_state('quiet-hours.json',{})
    if not isinstance(config,dict) or not config.get('enabled'):return False
    hour=(now or datetime.now()).hour
    start,end=config.get('start',22),config.get('end',8)
    return start<=hour<end if start<end else hour>=start or hour<end

def notice(text, key=None):
    text=''.join(c for c in str(text) if c.isprintable())[:300]
    added=False
    with locked():
        rows=read_state('notices.json',[])
        if not isinstance(rows,list):rows=[]
        if not key or not any(r.get('key')==key for r in rows):
            rows.append({'time':time.time(),'text':text,'key':key,'seen':False})
            write_state('notices.json',rows[-200:]);added=True
    if added and not quiet_now() and shutil.which('notify-send'):
        try:
            subprocess.run(['notify-send','--app-name=Zell','--','Zell',text],
                           stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=5,check=False)
        except (OSError,subprocess.TimeoutExpired):pass

def process_identity(pid):
    try:
        path=Path('/proc')/str(int(pid))
        if path.stat().st_uid!=os.getuid():return None
        fields=(path/'stat').read_text().rsplit(')',1)[1].split()
        if fields[0]=='Z':return None
        return fields[19]
    except (OSError,ValueError,TypeError,IndexError):return None

def job_start(key,label):
    identity=uuid.uuid4().hex
    with locked():
        jobs=read_state('os-jobs.json',{})
        if not isinstance(jobs,dict):jobs={}
        jobs={k:v for k,v in list(jobs.items())[-199:]}
        jobs[identity]={'key':key,'label':label,'pid':os.getpid(),'identity':process_identity(os.getpid()),'started':time.time(),'status':'running'}
        write_state('os-jobs.json',jobs)
    return identity

def job_finish(identity,status):
    with locked():
        jobs=read_state('os-jobs.json',{})
        job=jobs.get(identity)
        if not job:return
        job.update(status=status,finished=time.time());write_state('os-jobs.json',jobs)
    if not job['key'].startswith(('herald.','notices.')):
        notice(job['label']+': '+status,'job:'+identity)

def cancel_job(job):
    if job.get('pane'):
        import zell_rooms
        pane=job['pane']
        confirm('Send Ctrl-C to '+job['label']+'. This interrupts its current task.','cancel')
        current=zell_rooms._live(pane,zell_rooms._snapshot())
        if not current or current.get('agent')!='codex':raise ValueError('Coding session identity changed.')
        zell_rooms._control('key',current['pane_id'],'ctrl-c','--expect-broker',current['broker'])
        return
    pid=job.get('pid')
    if not pid or pid in {1,os.getpid(),os.getppid()} or process_identity(pid)!=job.get('identity'):
        raise ValueError('Job process is no longer the same live process.')
    confirm('Interrupt '+job['label']+' (PID '+str(pid)+').','cancel')
    fd=os.pidfd_open(pid)
    try:
        if process_identity(pid)!=job['identity']:raise ValueError('Process changed before cancellation.')
        # The worker catches SIGINT and completes its cleanup; never kill an
        # unverified PID or an unrelated pane's process group.
        if os.getsid(pid)==pid and os.getpgid(pid)==pid:
            os.killpg(pid,signal.SIGINT)
        elif str(job.get('key','')).startswith('schedule:'):
            identity=schedule_id(job['key'].split(':',1)[1])
            run(['systemctl','--user','kill','--kill-whom=all','--signal=SIGINT','zell-task-'+identity+'.service'])
        else:signal.pidfd_send_signal(fd,signal.SIGINT)
    finally:os.close(fd)

def aggregate():
    """Collect metadata only. Terminal contents and secrets are never read."""
    for path in (state_root()/'transcripts').glob('*/job.json'):
        try:
            record=json.loads(path.read_text())
            status=record.get('status')
            if status in {'complete','failed','cancelled'}:
                notice('Transcription '+path.parent.name+': '+status,'transcript:'+path.parent.name+':'+status)
        except (OSError,ValueError):pass
    jobs=read_state('os-jobs.json',{})
    if not isinstance(jobs,dict):jobs={}
    for identity,job in jobs.items():
        if job.get('status')=='running' and process_identity(job.get('pid'))!=job.get('identity'):
            job_finish(identity,'interrupted')
    update=read_state('os-update.json',{})
    if update.get('completed_at'):
        notice('Operating-system update completed. Gatekeeper can check whether a restart is needed.','update:'+str(update['completed_at']))
    import zell_rooms
    rooms=read_state('rooms.json',{})
    for row in rooms.get('sessions',[]) if isinstance(rooms,dict) else []:
        receipt=row.get('receipt',{}) if isinstance(row,dict) else {}
        pane=receipt.get('pane')
        if receipt.get('status')=='created' and zell_rooms._identity(pane):
            notice('Coding session opened in tab '+str(pane['tab_id'])+'.','fleet:'+pane['broker']+':'+str(pane['pane_id']))
    notices=read_state('notices.json',[])
    unread=[r for r in notices if not r.get('seen')]
    return {'count':len(unread),'text':unread[-1]['text'] if unread else ''}

@task('herald','notices','Notices since I last looked')
def show_notices():
    aggregate()
    with locked():
        rows=read_state('notices.json',[])
        for row in rows:print(('* ' if not row.get('seen') else '  ')+time.strftime('%m-%d %H:%M',time.localtime(row['time']))+' / '+row['text']);row['seen']=True
        write_state('notices.json',rows)
    if not rows:print('No notices yet.')

@task('herald','jobs','Running jobs across stations; cancel a job')
def running_jobs():
    aggregate();rows=[]
    for identity,job in read_state('os-jobs.json',{}).items():
        if job.get('status')=='running' and job.get('pid')!=os.getpid():rows.append((job['label']+' / '+str(job['pid']),job))
    for path in (state_root()/'transcripts').glob('*/job.json'):
        try:
            record=json.loads(path.read_text())
            if record.get('status') in {'running','queued'} and record.get('process_identity'):
                rows.append(('Transcription '+path.parent.name,{'label':'Transcription '+path.parent.name,'pid':record.get('pid'),'identity':record['process_identity']}))
        except (OSError,ValueError):pass
    import zell_rooms
    rooms=read_state('rooms.json',{})
    receipts=rooms.get('sessions',[]) if isinstance(rooms,dict) else []
    if receipts:
        try:
            snapshot=zell_rooms._snapshot();seen=set()
            for row in receipts:
                pane=zell_rooms._live(row.get('receipt',{}).get('pane'),snapshot)
                if pane and pane.get('agent')=='codex' and pane['pane_id'] not in seen:
                    label='Coding session / tab '+str(pane['tab_id'])+' / pane '+str(pane['pane_id'])
                    rows.append((label,{'label':label,'pane':pane}));seen.add(pane['pane_id'])
        except (OSError,ValueError,RuntimeError,subprocess.SubprocessError):print('Coding session status unavailable; open Pilot for details.')
    job=pick('Running background and station jobs',rows)
    if pick(job['label'],[('Keep running','keep'),('Interrupt this job','cancel')])=='cancel':cancel_job(job)

@task('herald','attention','What needs attention')
def attention():
    from zell_system import restart_status
    print(restart_status()['text'])
    for path in ('/',str(Path.home())):
        usage=shutil.disk_usage(path);print(f'{path}: {usage.free/2**30:.1f} GiB free'+(' / LOW DISK' if usage.free/usage.total<.1 else ''))
    run(['systemctl','--failed','--no-pager'],check=False)
    run(['systemctl','--user','--failed','--no-pager'],check=False)
    run(['apt-get','--simulate','upgrade'],check=False)

@task('herald','quiet','Quiet hours')
def quiet():
    enabled=pick('Quiet hours',[('Enable',True),('Disable',False)])
    config={'enabled':enabled}
    if enabled:
        config.update(start=int(ask('Start hour (0-23)',r'(?:[01]?[0-9]|2[0-3])')),end=int(ask('End hour (0-23)',r'(?:[01]?[0-9]|2[0-3])')))
    confirm('Save quiet hours: '+str(config),'save');write_state('quiet-hours.json',config)
    print('Notices remain in the cave; desktop notifications are muted during quiet hours.')

def schedule_id(value):
    if not re.fullmatch(r'[0-9a-f]{32}',value):raise ValueError('Invalid schedule identity.')
    return value

def unit_quote(value, environment=False):
    if '\n' in value or '\r' in value or '\0' in value:raise ValueError('Invalid service argument.')
    value=value.replace('\\','\\\\').replace('"','\\"').replace('%','%%')
    if not environment:value=value.replace('$','$$')
    return '"'+value+'"'

def unit_directory():
    return Path(os.environ.get('XDG_CONFIG_HOME',Path.home()/'.config'))/'systemd/user'

def install_schedule(identity,record):
    schedule_id(identity)
    calendar=record['calendar']
    if any(c in calendar for c in '\n\r\0%'):raise ValueError('Invalid calendar expression.')
    run(['systemd-analyze','calendar','--',calendar],capture=True)
    directory=unit_directory();directory.mkdir(parents=True,exist_ok=True,mode=0o700)
    name='zell-task-'+identity
    service=directory/(name+'.service');timer=directory/(name+'.timer')
    if service.exists() or timer.exists():raise ValueError('Schedule unit already exists.')
    command=[sys.executable,str(Path(__file__).with_name('zell_os.py').resolve()),'--scheduled',identity]
    service.write_text('[Unit]\nDescription=Zell scheduled task\n[Service]\nType=oneshot\nUMask=0077\nLimitCORE=0\nEnvironment='+unit_quote('KILIX_ZELL_STATE='+str(state_root()),environment=True)+'\nExecStart='+' '.join(unit_quote(v) for v in command)+'\n')
    timer.write_text('[Unit]\nDescription=Zell task timer\n[Timer]\nOnCalendar='+calendar+'\nPersistent=true\nUnit='+name+'.service\n[Install]\nWantedBy=timers.target\n')
    service.chmod(0o600);timer.chmod(0o600)
    try:
        run(['systemctl','--user','daemon-reload'])
        run(['systemctl','--user','enable','--now',name+'.timer'])
    except Exception:
        run(['systemctl','--user','disable','--now',name+'.timer'],check=False)
        service.unlink(missing_ok=True);timer.unlink(missing_ok=True)
        run(['systemctl','--user','daemon-reload'],check=False);raise

def create_schedule(payload,label,calendar=None):
    calendar=calendar or ask('When (systemd calendar, e.g. daily or 2026-12-01 09:00:00)')
    run(['systemd-analyze','calendar','--',calendar],capture=True)
    confirm(f'Schedule {label}\nWhen: {calendar}\nAction: '+json.dumps(payload)+'\nRuns as you while your user service manager is available; missed times are caught up at login.','schedule')
    identity=uuid.uuid4().hex
    record={**payload,'label':label,'calendar':calendar,'enabled':True,'created':time.time()}
    with locked():
        rows=read_state('schedules.json',{});rows[identity]=record;write_state('schedules.json',rows)
    try:install_schedule(identity,record)
    except Exception:
        with locked():
            rows=read_state('schedules.json',{});rows.pop(identity,None);write_state('schedules.json',rows)
        raise
    print('Scheduled. View runs in Archivist > User service journal.')

@task('timekeeper','timer','Timer')
def timer():
    seconds=positive('Seconds from now',365*86400)
    text=ask('Timer message')
    create_schedule({'kind':'notice','text':text},'Timer', (datetime.now()+timedelta(seconds=seconds)).strftime('%Y-%m-%d %H:%M:%S'))

@task('timekeeper','alarm','Alarm or reminder')
def alarm():
    text=ask('Reminder message');create_schedule({'kind':'notice','text':text},'Reminder')

@task('timekeeper','scheduled','Scheduled tasks: list, add, pause, remove')
def scheduled():
    rows=read_state('schedules.json',{})
    operation=pick('Schedules',[(v,v) for v in ['list','add','pause','resume','remove']])
    if operation=='list':
        for identity,r in rows.items():print(f"{identity[:8]} / {'enabled' if r['enabled'] else 'paused'} / {r['calendar']} / {r['label']}")
        run(['systemctl','--user','list-timers','zell-task-*','--all','--no-pager'],check=False);return
    if operation=='add':
        label=ask('Task label')
        command=shlex.split(ask('Command and arguments (no shell expansion; never put secrets here)'))
        if not command:raise ValueError('Command required.')
        command[0]=tool(command[0]);return create_schedule({'kind':'command','argv':command},label)
    identity=pick('Choose a schedule',[(r['label']+' / '+r['calendar'],identity) for identity,r in rows.items()]);schedule_id(identity)
    confirm(operation+' '+rows[identity]['label'],operation)
    name='zell-task-'+identity
    run(['systemctl','--user','enable' if operation=='resume' else 'disable','--now',name+'.timer'])
    if operation=='remove':
        run(['systemctl','--user','stop',name+'.service'],check=False)
        for suffix in ('.timer','.service'):(unit_directory()/(name+suffix)).unlink(missing_ok=True)
        run(['systemctl','--user','daemon-reload'])
    with locked():
        current=read_state('schedules.json',{})
        if operation=='remove':current.pop(identity,None)
        else:current[identity]['enabled']=operation=='resume'
        write_state('schedules.json',current)

@task('timekeeper','clock','Time zone and clock synchronization')
def clock():
    run(['timedatectl','status'])
    op=pick('Clock',[('Return','return'),('Set time zone','zone'),('Enable clock synchronization','sync')])
    if op=='zone':
        zones=run(['timedatectl','list-timezones'],capture=True).splitlines()
        query=ask('Time zone contains').casefold();zone=pick('Time zones',[(z,z) for z in zones if query in z.casefold()])
        confirm('Set time zone to '+zone,'set');run(['sudo','timedatectl','set-timezone',zone])
    if op=='sync':confirm('Enable network time synchronization.','enable');run(['sudo','timedatectl','set-ntp','true'])

@task('timekeeper','upcoming','Upcoming timers and reminders')
def upcoming():run(['systemctl','--user','list-timers','zell-task-*','--all','--no-pager'])

def execute_schedule(identity):
    schedule_id(identity);record=read_state('schedules.json',{}).get(identity)
    if not record or not record.get('enabled'):return 0
    job=job_start('schedule:'+identity,record['label']);status='completed'
    try:
        if record['kind']=='notice':notice(record['text'])
        elif record['kind']=='backup':
            from zell_files import perform_backup
            perform_backup(Path(record['source']),Path(record['destination']),record.get('source_volume'),record.get('destination_volume'))
        elif record['kind']=='command':
            argv=record['argv']
            if not isinstance(argv,list) or not argv or not all(isinstance(v,str) and '\0' not in v for v in argv):raise ValueError('Invalid stored command.')
            run(argv)
        else:raise ValueError('Unknown scheduled action.')
    except (OSError,ValueError,RuntimeError,subprocess.SubprocessError):status='failed'
    except KeyboardInterrupt:status='cancelled'
    finally:job_finish(job,status)
    return 0 if status=='completed' else 1

@task('notices','clipboard','Clipboard history in the encrypted vault')
def clipboard_history():
    from zell_secrets import save_bytes, records, command
    op=pick('Encrypted clipboard history',[('Save current clipboard explicitly','save'),('Retrieve a saved clipboard item for 20 seconds','retrieve')])
    print('Clipboard history only captures when you choose Save; values are encrypted in Key Master.')
    if op=='save':
        confirm('Read the current clipboard and save it in the unlocked secret vault.','save')
        value=run(['xclip','-selection','clipboard','-o'],capture=True).encode()
        save_bytes('Clipboard / '+datetime.now().isoformat(timespec='seconds'),'clipboard','value',value)
    else:
        identity=pick('Saved clipboard entries',[(label,identity) for label,identity in records() if label.startswith('Clipboard / ')])
        run(command('copy',identity,'value','--clear-seconds','20'))
