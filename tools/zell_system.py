"""Power, connectivity, package and account operations for the cave staff."""
from __future__ import annotations
import fcntl
import getpass
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import time
import uuid
from zell_os import task, ask, pick, confirm, run, tool, path_input, positive, Cancelled

def restart_status(root=Path('/'), capture=None, kernel=None):
    """Compare running kernel/module identities with installed files, read-only."""
    capture = capture or (lambda argv: run(argv, capture=True))
    kernel = kernel or platform.release()
    reasons = []
    if (root/'var/run/reboot-required').exists():
        reasons.append('The operating system requested a restart.')
    versions = [p.name.removeprefix('vmlinuz-') for p in (root/'boot').glob('vmlinuz-*')]
    def version_key(value):
        return tuple((0, int(v)) if v.isdigit() else (1, v) for v in re.split(r'(\d+)',value))
    if versions and version_key(max(versions,key=version_key)) > version_key(kernel):
        reasons.append('A newer installed kernel is waiting for restart: '+max(versions,key=version_key))
    loaded = root/'sys/module/nvidia/version'
    if loaded.is_file():
        try:
            current = loaded.read_text().strip()
            installed = capture(['modinfo','-F','version','nvidia']).strip()
            if installed and current != installed:
                reasons.append('NVIDIA module changed: running '+current+', installed '+installed+'.')
        except (OSError, RuntimeError, subprocess.SubprocessError):
            reasons.append('NVIDIA module is loaded but its installed version could not be checked.')
    return {'needed':bool(reasons), 'reasons':reasons,
            'text':'\n'.join(reasons) or 'No restart requirement detected; running kernel '+kernel+'.'}

@task('gatekeeper','lock','Lock the screen')
def lock():
    run(['xdg-screensaver','lock'])

def power(operation):
    run(['systemd-inhibit','--list','--no-pager'], check=False)
    confirm(f'{operation.capitalize()} this machine. Save open work first.', operation)
    run(['systemctl', operation])

@task('gatekeeper','suspend','Suspend')
def suspend(): power('suspend')

@task('gatekeeper','restart','Restart')
def restart(): power('reboot')

@task('gatekeeper','shutdown','Shut down')
def shutdown(): power('poweroff')

@task('gatekeeper','logout','Log out of this session')
def logout():
    session = os.environ.get('XDG_SESSION_ID')
    if not session or not re.fullmatch(r'[a-zA-Z0-9_-]+',session):
        raise ValueError('Current login session could not be identified.')
    print(run(['loginctl','show-session',session,'-p','Name','-p','Type','-p','State'],capture=True))
    confirm('Log out of session '+session+' and close its applications.', 'logout')
    run(['loginctl','terminate-session',session])

@task('gatekeeper','battery','Battery and charger status')
def battery(): run(['upower','--dump'])

@task('gatekeeper','restart-needed','Restart needed?')
def restart_needed(): print(restart_status()['text'])

@task('gatekeeper','inhibitors','What blocks sleep or shutdown')
def inhibitors(): run(['systemd-inhibit','--list','--no-pager'])

@task('gatekeeper','power-profile','Choose a power profile')
def profile():
    run(['powerprofilesctl','list'])
    choice = pick('Power profile',[(v,v) for v in ['power-saver','balanced','performance']])
    confirm('Set power profile to '+choice)
    run(['powerprofilesctl','set',choice])

@task('signalman','wifi','Wi-Fi networks: list, connect, forget')
def wifi():
    operation = pick('Wi-Fi',[('List nearby networks','list'),('Connect a personal/open network','connect'),
                            ('Connect a saved network','saved'),('Forget a saved network','forget')])
    if operation == 'list': return run(['nmcli','device','wifi','list','--rescan','yes'])
    if operation in {'saved','forget'}:
        run(['nmcli','-f','NAME,UUID,TYPE','connection','show'])
        identity = ask('Connection UUID',r'[0-9a-fA-F-]{36}')
        if operation == 'forget':
            confirm('Remove NetworkManager profile '+identity+'. Vault entries are kept.', 'forget')
            return run(['nmcli','connection','delete','uuid',identity])
        security=run(['nmcli','-g','802-11-wireless-security.key-mgmt','connection','show','uuid',identity],capture=True).strip()
        if not security or security=='--':
            confirm('Connect open profile '+identity,'connect')
            return run(['nmcli','connection','up','uuid',identity])
        if security not in {'wpa-psk','sae'}:raise ValueError('This saved profile requires an enterprise authentication method. Configure it with NetworkManager first.')
        from zell_secrets import records,command
        record=pick('Key Master Wi-Fi passwords',[(label,rid) for label,rid in records() if label.startswith('Wi-Fi / ')])
        confirm('Connect profile '+identity+' using the selected vault password.','connect')
        run(['nmcli','connection','modify','uuid',identity,'wifi-sec.psk-flags','2'])
        return run(command('run',record,'password','--',sys.executable,str(Path(__file__).resolve()),'--wifi-profile',identity))
    run(['nmcli','device','wifi','list','--rescan','yes'])
    ssid = ask('Exact network name (SSID)')
    if len(ssid.encode())>32: raise ValueError('SSID exceeds 32 bytes.')
    security = pick('Security',[('WPA2 personal','wpa-psk'),('WPA3 personal','sae'),('Open network','open')])
    password = getpass.getpass('Wi-Fi password (hidden): ') if security != 'open' else ''
    if security != 'open' and (not password or '\n' in password or '\r' in password):
        raise ValueError('Invalid password.')
    confirm('Create a connection for '+ssid+'. Its password will be stored only in Key Master.', 'connect')
    if password:
        from zell_secrets import save_bytes
        save_bytes('Wi-Fi / '+ssid,'wifi','password',password.encode())
    identity = str(uuid.uuid4())
    argv = ['nmcli','connection','add','type','wifi','ifname','*','con-name','Zell / '+ssid,
            'connection.uuid',identity,'ssid',ssid,'connection.autoconnect','no']
    if password: argv += ['wifi-sec.key-mgmt',security,'wifi-sec.psk-flags','2']
    run(argv)
    fd = os.memfd_create('zell-wifi', os.MFD_CLOEXEC|os.MFD_ALLOW_SEALING)
    try:
        os.write(fd, ('802-11-wireless-security.psk:'+password+'\n').encode() if password else b'')
        del password
        os.lseek(fd,0,os.SEEK_SET)
        fcntl.fcntl(fd,fcntl.F_ADD_SEALS,fcntl.F_SEAL_WRITE|fcntl.F_SEAL_GROW|fcntl.F_SEAL_SHRINK|fcntl.F_SEAL_SEAL)
        run(['nmcli','connection','up','uuid',identity,'passwd-file',f'/proc/self/fd/{fd}'],pass_fds=(fd,))
    finally: os.close(fd)

@task('signalman','status','Connections, addresses, gateway and DNS')
def network_status(): run(['nmcli','device','show'])

@task('signalman','test','Test gateway, DNS and internet')
def connection_test():
    routes = json.loads(run(['ip','-json','route','show','default'],capture=True))
    gateway = next((r.get('gateway') for r in routes if r.get('gateway')),None)
    if gateway: run(['ping','-c','3','-W','2',gateway],check=False)
    else: print('No default gateway.')
    host = ask('Host to test (DNS lookup and HTTPS connection)',r'[a-zA-Z0-9][a-zA-Z0-9.-]*')
    run(['getent','ahosts',host],check=False)
    run(['curl','--head','--max-time','10','--proto','=https','--','https://'+host],check=False)

@task('signalman','vpn','VPN connections: up or down')
def vpn():
    run(['nmcli','-f','NAME,UUID,TYPE,DEVICE','connection','show'])
    identity = ask('VPN connection UUID',r'[0-9a-fA-F-]{36}')
    operation = pick('VPN',[(v,v) for v in ['up','down']])
    confirm(operation+' VPN '+identity,operation)
    run(['nmcli','--ask','connection',operation,'uuid',identity])

@task('signalman','bluetooth','Bluetooth devices: pair, connect, forget')
def bluetooth():
    run(['bluetoothctl','devices'])
    operation = pick('Bluetooth',[(v,v) for v in ['scan','pair','connect','disconnect','remove']])
    if operation == 'scan': return run(['bluetoothctl','--timeout','15','scan','on'])
    address = ask('Device address',r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}')
    confirm(operation+' Bluetooth device '+address,operation)
    # Interactive agent permits numeric comparison and PIN confirmation.
    run(['bluetoothctl','--agent','KeyboardDisplay',operation,address])

@task('signalman','airplane','Airplane mode (Wi-Fi and Bluetooth)')
def airplane():
    enabled = pick('Airplane mode',[('On: disable Wi-Fi and Bluetooth',True),('Off: enable radios',False)])
    confirm('Disable wireless radios' if enabled else 'Enable wireless radios')
    run(['nmcli','radio','all','off' if enabled else 'on'])
    run(['bluetoothctl','power','off' if enabled else 'on'],check=False)

@task('signalman','ports','Listening ports and owners')
def ports(): run(['ss','-lntup'])

@task('signalman','ssh','Saved SSH hosts: open a session')
def ssh():
    hosts=[]
    path=Path.home()/'.ssh/config'
    if path.exists():
        for line in path.read_text().splitlines():
            words=shlex.split(line,comments=True)
            if words and words[0].lower()=='host':
                hosts.extend(h for h in words[1:] if re.fullmatch(r'[a-zA-Z0-9][a-zA-Z0-9._-]*',h))
    host=pick('Saved SSH hosts (from ~/.ssh/config)',[(h,h) for h in sorted(set(hosts))])
    run(['kilix','new-tab','--title','SSH / '+host,'--','ssh','--',host])

def packages():
    values=ask('Debian package names').split()
    if not values or not all(re.fullmatch(r'[a-z0-9][a-z0-9+.-]*(?::[a-z0-9]+)?',v) for v in values):
        raise ValueError('Invalid package name.')
    return values

@task('merchant','search','Search Debian packages')
def package_search(): run(['apt-cache','search','--',ask('Description contains')])

def apt_change(operation, names):
    run(['apt-get','--simulate',operation,*names])
    confirm('Apply the package changes previewed above.',operation)
    run(['sudo','apt-get',operation,*names])

@task('merchant','install','Install Debian packages')
def install(): apt_change('install',packages())

@task('merchant','remove','Remove Debian packages')
def remove(): apt_change('remove',packages())

@task('merchant','autoremove','Remove unused dependencies')
def autoremove(): apt_change('autoremove',[])

@task('merchant','pending','Pending updates and planned changes')
def pending():
    print('Using the local package index. Administrator > Update OS refreshes it.')
    run(['apt-get','--simulate','upgrade'])

@task('merchant','holds','Hold or release packages')
def holds():
    run(['apt-mark','showhold'])
    operation=pick('Package hold',[(v,v) for v in ['hold','unhold']])
    names=packages();confirm(operation+' '+', '.join(names),operation)
    run(['sudo','apt-mark',operation,*names])

@task('merchant','kilix','Kilix apps: installed, available, install, update')
def kilix_apps():
    operation=pick('Kilix software',[(v,v) for v in ['installed','available','install','update']])
    if operation=='installed':
        root=Path(os.environ.get('GPU_TERMINAL_HOME',Path.home()/'.local/gpu_terminal'))/'kilix/data/desktop-apps'
        for path in sorted(root.iterdir()) if root.is_dir() else []:
            if path.is_dir():print(path.name)
        return
    if operation=='available': return run(['kilix','install'])
    name=ask('Kilix app or component name',r'[a-zA-Z0-9][a-zA-Z0-9._-]*')
    confirm(operation+' Kilix component '+name,operation)
    run(['kilix',operation,name])

@task('merchant','models','Local models: inventory, download, remove')
def models():
    from zell_files import model_manager
    model_manager()

@task('merchant','history','Recent package installation and upgrade history')
def package_history():
    path=Path('/var/log/apt/history.log')
    if not path.exists(): print('No package history yet.'); return
    print(path.read_text(errors='replace')[-24000:])

@task('merchant','installed','Installed Debian packages')
def installed(): run(['dpkg-query','-W'])

@task('sentinel','password','Change my login password')
def password(): run(['passwd'])

@task('sentinel','logins','Logged-in users and recent logins')
def logins():
    run(['loginctl','list-sessions','--no-pager'])
    run(['last','-n','30'],check=False)

@task('sentinel','security-updates','Pending security updates')
def security_updates():
    # The configured unattended-upgrades origins define the security policy.
    run(['sudo','unattended-upgrade','--dry-run','--debug'])

@task('sentinel','automatic','Automatic security updates: status, on or off')
def automatic():
    run(['systemctl','status','apt-daily-upgrade.timer','unattended-upgrades.service','--no-pager'],check=False)
    for name in ('10periodic','20auto-upgrades'):
        path=Path('/etc/apt/apt.conf.d')/name
        if path.exists(): print(path.read_text())
    operation=pick('Automatic security updates',[('Status only','status'),('Enable','enable'),('Disable','disable')])
    if operation=='status':return
    confirm(operation.capitalize()+' automatic security upgrades. This changes unattended-upgrades configuration.',operation)
    run(['sudo',sys.executable,str(Path(__file__).resolve()),'--automatic','1' if operation=='enable' else '0'])
    run(['sudo','systemctl',operation,'--now','apt-daily-upgrade.timer'])

@task('sentinel','firewall','Open ports and firewall rules')
def firewall():
    ports()
    run(['sudo','nft','list','ruleset'])

@task('sentinel','ssh-keys','SSH keys: list, create, show public key')
def ssh_keys():
    operation=pick('SSH keys',[(v,v) for v in ['list','create','show-public']])
    keys=sorted((Path.home()/'.ssh').glob('*.pub'))
    if operation=='list':
        for key in keys:run(['ssh-keygen','-lf',key])
    elif operation=='show-public':
        key=pick('Public keys',[(p.name,p) for p in keys]); print(key.read_text()[:16000])
    else:
        name=ask('New key filename (inside ~/.ssh)',r'[a-zA-Z0-9][a-zA-Z0-9._-]*')
        target=Path.home()/'.ssh'/name
        if target.exists() or target.with_suffix(target.suffix+'.pub').exists():raise ValueError('A key already exists there.')
        confirm('Create an Ed25519 key at '+str(target)+'. Enter its passphrase privately when asked.','create')
        target.parent.mkdir(mode=0o700,exist_ok=True)
        run(['ssh-keygen','-t','ed25519','-f',target])

@task('sentinel','readable','Home files readable by everyone')
def world_readable():
    from zell_files import walk_files
    count=0
    for path,info in walk_files(Path.home()):
        if info.st_mode & 4:
            print(path);count+=1
        if count>=1000:print('Stopped at 1,000 matches.');break

@task('admin','services','System and user services')
def services():
    scope=pick('Service scope',[('User','user'),('System','system')])
    flag=['--user'] if scope=='user' else []
    listing=run(['systemctl',*flag,'list-units','--all','--type=service','--no-legend','--plain','--no-pager'],capture=True)
    units=[(line,line.split()[0]) for line in listing.splitlines() if line.split() and line.split()[0].endswith('.service')]
    unit=pick('Services',units)
    op=pick(unit,[(v,v) for v in ['status','start','restart','stop','enable','disable']])
    if op!='status': confirm(op+' '+scope+' service '+unit,op)
    prefix=['sudo'] if scope=='system' and op!='status' else []
    run([*prefix,'systemctl',*flag,op,unit,'--no-pager'])

@task('admin','locale','Language, region, keyboard and font size')
def locale_settings():
    op=pick('Preferences',[('Language / region','locale'),('Keyboard','keyboard'),('Kilix font size and settings','font')])
    if op=='font':return run(['kilix','settings'])
    if op=='keyboard':
        from zell_devices import keyboard
        return keyboard()
    run(['locale','-a'])
    value=ask('Installed locale name',r'[a-zA-Z0-9_.@-]+')
    confirm('Set the system locale to '+value+'. New sessions use this value.','set')
    run(['sudo','localectl','set-locale','LANG='+value])

@task('housekeeper','stop','Stop one of my processes')
def stop_process():
    run(['ps','-u',str(os.getuid()),'-o','pid,etime,comm'])
    pid=positive('PID to stop')
    path=Path('/proc')/str(pid)
    if pid in {1,os.getpid(),os.getppid()} or path.stat().st_uid != os.getuid():raise ValueError('Choose another process owned by you.')
    identity=path.joinpath('stat').read_text().rsplit(')',1)[1].split()[19]
    name=path.joinpath('comm').read_text().strip()
    confirm(f'Send TERM to PID {pid} ({name}). Unsaved work may be lost.','stop')
    if path.joinpath('stat').read_text().rsplit(')',1)[1].split()[19]!=identity:raise ValueError('Process identity changed.')
    # pidfd keeps the signal attached to this process even if the PID is recycled.
    import signal
    fd=os.pidfd_open(pid)
    try:signal.pidfd_send_signal(fd,signal.SIGTERM)
    finally:os.close(fd)

@task('housekeeper','startup','Startup applications')
def startup():
    directory=Path.home()/'.config/autostart'
    operation=pick('Startup applications',[('List','list'),('Add an application','add'),('Enable / disable','toggle')])
    paths=sorted({p.name:p for d in [Path('/etc/xdg/autostart'),directory] for p in d.glob('*.desktop')}.values())
    if operation=='list':
        for path in paths:print(path)
        return
    directory.mkdir(parents=True,exist_ok=True)
    if operation=='add':
        source=path_input('Application .desktop file')
        if source.suffix!='.desktop':raise ValueError('Choose a .desktop application.')
        dest=directory/source.name
        if dest.exists():raise ValueError('A startup entry with that name already exists.')
        confirm('Start '+str(source)+' at login.','add');shutil.copyfile(source,dest)
    else:
        source=pick('Startup entry',[(p.name,p) for p in paths])
        import configparser
        config=configparser.ConfigParser(interpolation=None,strict=False);config.optionxform=str
        config.read(source);entry=config['Desktop Entry']
        enabled=entry.get('Hidden','false').lower()=='true'
        confirm(('Enable ' if enabled else 'Disable ')+source.name)
        entry['Hidden']='false' if enabled else 'true'
        with (directory/source.name).open('w') as stream:config.write(stream,space_around_delimiters=False)

@task('wizard','tour','First-run tour of Zell')
def tour():
    print('Welcome to Zell. Walk with WASD; hold Shift to run. Enter opens a nearby station.\n'
          'Tab finds stations and actions; type a task name to search across rooms.\n\n'
          'Wizard: questions. Archivist: logs. Administrator: settings, services and OS updates.\n'
          'Housekeeper: health and cleanup. DJ: media. Librarian: documents.\n'
          'Gatekeeper: power. Signalman: network. Merchant: software. Sentinel: security.\n'
          'Timekeeper: reminders. Herald: notices and running jobs.\n'
          'Command Center downstairs: coding screens, Git Wizard, Pilot and Key Master.\n'
          'Laundry: drives, Porter for files and Laundress for removable media.\n'
          'Vault: backups. Workshop: displays, audio, printers and other devices.\n\n'
          'Changes show their targets and ask for confirmation. Enter on an empty prompt cancels.\n'
          'Secrets and sudo passwords stay in the private task terminal. Minimize a running task\n'
          'to keep walking; click its bottom tab to return. The map follows your weather widget.')


def private_main():
    import argparse, resource, tempfile
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    parser=argparse.ArgumentParser();parser.add_argument('--wifi-profile');parser.add_argument('--automatic',choices=['0','1'])
    args=parser.parse_args()
    if args.wifi_profile:
        if not re.fullmatch(r'[0-9a-fA-F-]{36}',args.wifi_profile):raise ValueError('Invalid profile UUID.')
        source=int(os.environ.pop('KILIX_SECRET_FD'))
        with os.fdopen(source,'rb') as stream:value=bytearray(stream.read(65537))
        if not value or len(value)>65536 or b'\n' in value or b'\r' in value:raise ValueError('Invalid stored password.')
        fd=os.memfd_create('zell-wifi',os.MFD_CLOEXEC|os.MFD_ALLOW_SEALING)
        try:
            os.write(fd,b'802-11-wireless-security.psk:'+value+b'\n');value[:]=b'\0'*len(value);os.lseek(fd,0,os.SEEK_SET)
            fcntl.fcntl(fd,fcntl.F_ADD_SEALS,fcntl.F_SEAL_WRITE|fcntl.F_SEAL_GROW|fcntl.F_SEAL_SHRINK|fcntl.F_SEAL_SEAL)
            run(['nmcli','connection','up','uuid',args.wifi_profile,'passwd-file',f'/proc/self/fd/{fd}'],pass_fds=(fd,))
        finally:os.close(fd)
    elif args.automatic is not None:
        if os.geteuid()!=0:raise PermissionError('Administrator privileges required.')
        directory=Path('/etc/apt/apt.conf.d')
        fd,name=tempfile.mkstemp(prefix='.zell-auto-',dir=directory)
        try:
            with os.fdopen(fd,'w') as stream:
                stream.write('APT::Periodic::Update-Package-Lists "'+args.automatic+'";\nAPT::Periodic::Unattended-Upgrade "'+args.automatic+'";\n')
                stream.flush();os.fsync(stream.fileno())
            os.chmod(name,0o644);os.replace(name,directory/'99zz-zell-auto-upgrades')
        finally:Path(name).unlink(missing_ok=True)
    else:raise ValueError('A private operation is required.')

if __name__=='__main__':private_main()
