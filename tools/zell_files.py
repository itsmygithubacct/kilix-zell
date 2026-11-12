"""File, removable-drive and checksum-verified backup tasks."""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
from zell_paths import archive_only_root
import time
import uuid
import zipfile
from zell_os import task, ask, pick, confirm, run, path_input, positive, read_state, write_state, state_root, Cancelled

def active_path(path):
    path=Path(path).expanduser().resolve()
    reserved=archive_only_root(path)
    if reserved:
        raise ValueError(f'{reserved} is reserved for archives on this machine; choose another disk for active files.')
    return path

def target_new(prompt):
    path=path_input(prompt,exists=False)
    active_path(path)
    if path.exists() or path.is_symlink():raise ValueError('The destination already exists; choose a new name.')
    if not path.parent.is_dir():raise ValueError('The destination parent folder does not exist.')
    return path

def ordinary(path):
    path=active_path(path)
    if path in {Path('/'),Path.home().resolve()} or path in Path.home().resolve().parents:
        raise ValueError('Choose a specific file or subfolder, not an entire system or home root.')
    return path

def walk_files(root, maximum=100000):
    root=active_path(root)
    count=0
    for directory,dirs,files in os.walk(root,followlinks=False):
        dirs[:]=[d for d in dirs if not (Path(directory)/d).is_symlink() and d not in {'.git','node_modules'}]
        for name in files:
            path=Path(directory)/name
            try:
                info=path.lstat()
                if not stat.S_ISREG(info.st_mode):continue
                yield path,info
                count+=1
                if count>=maximum:return
            except OSError:continue

def digest(path):
    hasher=hashlib.sha256()
    fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW)
    with os.fdopen(fd,'rb') as stream:
        for data in iter(lambda:stream.read(2**20),b''):hasher.update(data)
    return hasher.hexdigest()

@task('porter','copy-move','Copy or move files and folders')
def copy_move():
    operation=pick('File operation',[('Copy','copy'),('Move','move')])
    source=ordinary(path_input('Source file or folder'))
    destination=target_new('New destination path')
    if source.is_dir() and source in destination.resolve().parents:raise ValueError('Destination cannot be inside the source.')
    confirm(f'{operation.capitalize()}\n  {source}\nto\n  {destination}',operation)
    # --no-clobber protects against a destination appearing after confirmation.
    if operation=='copy':run(['cp','--archive','--no-clobber','--',source,destination])
    else:run(['mv','--no-clobber','--',source,destination])

@task('porter','rename','Rename a file or folder')
def rename():
    source=ordinary(path_input('File or folder to rename'))
    name=ask('New name (without a folder path)')
    if name in {'.','..'} or '/' in name:raise ValueError('Enter a filename only.')
    dest=source.with_name(name)
    if dest.exists() or dest.is_symlink():raise ValueError('That name already exists.')
    confirm(f'Rename {source} to {dest}','rename')
    run(['mv','--no-clobber','--',source,dest])

@task('porter','mkdir','Create a folder')
def mkdir():
    path=target_new('New folder path');confirm('Create folder '+str(path),'create');path.mkdir(mode=0o700)

@task('porter','trash','Move to trash, restore or empty trash')
def trash():
    operation=pick('Trash',[(v,v) for v in ['move','open','restore','empty']])
    if operation=='open':return run(['gio','open','trash:///'])
    if operation=='move':
        path=ordinary(path_input('File or folder to trash'))
        confirm('Move to trash: '+str(path),'trash');return run(['gio','trash','--',path])
    run(['gio','trash','--list'])
    if operation=='empty':
        confirm('Permanently delete ALL items in your trash. This cannot be undone.','empty')
        return run(['gio','trash','--empty'])
    uri=ask('Exact trash:/// location from the list')
    if not uri.startswith('trash:///') or '..' in PurePosixPath(uri).parts:raise ValueError('Choose a listed trash location.')
    confirm('Restore '+uri+' to its original location without overwriting existing files.','restore')
    run(['gio','trash','--restore','--',uri])

def archive_members(source):
    """Reject links, devices, traversal and oversized archives before extraction."""
    if zipfile.is_zipfile(source):
        with zipfile.ZipFile(source) as archive:
            rows=[(i.filename,i.file_size,'dir' if i.is_dir() else 'file',i.external_attr>>16) for i in archive.infolist()]
    else:
        with tarfile.open(source,'r:*') as archive:
            rows=[(i.name,i.size,'dir' if i.isdir() else 'file' if i.isfile() else 'unsafe',0) for i in archive.getmembers()]
    seen=set();total=0
    for name,size,kind,mode in rows:
        p=PurePosixPath(name)
        if p.is_absolute() or '..' in p.parts or not p.parts or name in seen or any(ord(c)<32 for c in name):
            raise ValueError('Unsafe or duplicate archive path.')
        if kind=='unsafe' or stat.S_ISLNK(mode):raise ValueError('Archives containing links or special files are not extracted here.')
        seen.add(name);total+=size
    if len(rows)>100000 or total>20*2**30:raise ValueError('Archive exceeds 100,000 entries or 20 GiB; use a dedicated archive tool.')
    return rows,total

def extract_archive(source,destination):
    rows,_=archive_members(source)
    destination.mkdir(mode=0o700)  # Must be new; never extract into existing files.
    if zipfile.is_zipfile(source):
        with zipfile.ZipFile(source) as archive:
            for name,_,kind,_ in rows:
                target=destination/name
                if kind=='dir':target.mkdir(parents=True,exist_ok=True,mode=0o700);continue
                target.parent.mkdir(parents=True,exist_ok=True,mode=0o700)
                with archive.open(name) as src,target.open('xb') as dest:shutil.copyfileobj(src,dest)
    else:
        with tarfile.open(source,'r:*') as archive:
            for name,_,kind,_ in rows:
                target=destination/name
                if kind=='dir':target.mkdir(parents=True,exist_ok=True,mode=0o700);continue
                target.parent.mkdir(parents=True,exist_ok=True,mode=0o700)
                with archive.extractfile(name) as src,target.open('xb') as dest:shutil.copyfileobj(src,dest)

@task('porter','archive','Compress or extract an archive')
def archive_task():
    operation=pick('Archives',[('Compress as .tar.gz','compress'),('Extract ZIP or tar archive','extract')])
    source=path_input('Source file or folder' if operation=='compress' else 'Archive file')
    dest=target_new('New .tar.gz file' if operation=='compress' else 'New extraction folder')
    if operation=='compress':
        if not str(dest).endswith('.tar.gz'):raise ValueError('Output must end in .tar.gz.')
        if source.resolve() in dest.resolve().parents:raise ValueError('Archive destination cannot be inside its source.')
        confirm(f'Compress {source} into {dest}','compress')
        # Exclusive output descriptor avoids clobbering an existing archive.
        with dest.open('xb') as stream:
            with tarfile.open(fileobj=stream,mode='w:gz',dereference=False) as archive:archive.add(source,arcname=source.name)
    else:
        rows,size=archive_members(source)
        confirm(f'Extract {len(rows)} entries ({size/2**20:.1f} MiB) from {source} into new folder {dest}','extract')
        extract_archive(source,dest)

@task('porter','find','Find files by name, size or age')
def find():
    root=path_input('Folder to search',directory=True)
    mode=pick('Match files',[('Name contains','name'),('Larger than MiB','size'),('Older than days','age')])
    value=ask('Name contains').casefold() if mode=='name' else positive('Minimum '+('MiB' if mode=='size' else 'days'))
    count=0
    for path,info in walk_files(root):
        match=value in path.name.casefold() if mode=='name' else info.st_size>=value*2**20 if mode=='size' else time.time()-info.st_mtime>=value*86400
        if match:print(f'{info.st_size/2**20:10.2f} MiB  {path}');count+=1
        if count>=1000:print('Stopped at 1,000 matches.');break

@task('porter','duplicates','Find duplicate files by checksum')
def duplicates():
    root=path_input('Folder to compare',directory=True)
    sizes={}
    for path,info in walk_files(root):sizes.setdefault(info.st_size,[]).append(path)
    for size,paths in sizes.items():
        if len(paths)<2:continue
        hashes={}
        for path in paths:
            try:hashes.setdefault(digest(path),[]).append(path)
            except OSError:continue
        for same in hashes.values():
            if len(same)>1:print(f'\nDuplicate group ({size} bytes):\n'+'\n'.join(map(str,same)))

@task('porter','properties','Properties and permissions')
def properties():
    path=path_input('File or folder');info=path.lstat()
    print(f'{path}\nSize: {info.st_size} bytes\nMode: {stat.filemode(info.st_mode)}\nOwner UID: {info.st_uid}; group GID: {info.st_gid}')
    operation=pick('Properties',[('Return','return'),('Change permissions','chmod')])
    if operation=='chmod':
        if path.is_symlink():raise ValueError('Choose a regular file or folder, not a symbolic link.')
        mode=ask('Octal permissions (three digits)',r'[0-7]{3}')
        confirm(f'Change permissions of {path} to {mode}','chmod');path.chmod(int(mode,8))

@task('porter','open-with','Open with and choose default application')
def open_with():
    path=path_input('File')
    mime=run(['xdg-mime','query','filetype',path],capture=True).strip()
    print('Type:',mime,'\nDefault:',run(['xdg-mime','query','default',mime],capture=True).strip())
    op=pick('Application',[('Open with current default','open'),('Open with another application','other'),('Set default application','default')])
    if op=='open':return run(['gio','open',path])
    roots=[Path.home()/'.local/share/applications',Path('/usr/local/share/applications'),Path('/usr/share/applications')]
    app=pick('Applications',[(p.stem,p) for root in roots for p in sorted(root.glob('*.desktop'))])
    if op=='other':run(['gio','launch',app,path])
    else:confirm(f'Use {app.name} for {mime}','set');run(['xdg-mime','default',app.name,mime])

def drives():
    return json.loads(run(['lsblk','--json','--bytes','--paths','-o','NAME,TYPE,SIZE,MODEL,SERIAL,WWN,TRAN,RM,RO,MOUNTPOINTS,MAJ:MIN'],capture=True))['blockdevices']

def flatten(rows):
    for row in rows:
        yield row
        yield from flatten(row.get('children',[]))

def drive_label(d):
    return f"{d['name']} / {str(d.get('model') or '').strip()} / {int(d.get('size') or 0)/2**30:.2f} GiB / mounted: {d.get('mountpoints')}"

def choose_drive(whole=False):
    return pick('Attached drives',[(drive_label(d),d) for d in flatten(drives()) if not whole or d.get('type')=='disk'])

def device_identity(d):
    return {k:d.get(k) for k in ('name','size','serial','wwn','maj:min')}

def writable_device(identity):
    disk=next((d for d in drives() if device_identity(d)==identity),None)
    if not disk or disk.get('type')!='disk' or disk.get('ro') or not (disk.get('rm') or disk.get('tran')=='usb'):
        raise ValueError('Target is not the same writable removable/USB disk.')
    if not disk.get('serial') and not disk.get('wwn'):
        raise ValueError('Drive has no stable serial/WWN; write refused.')
    if any(any(p for p in d.get('mountpoints',[]) if p) for d in flatten([disk])):
        raise ValueError('Unmount every partition first. Mounted disks cannot be overwritten.')
    # Active swap is also a mounted use, even when lsblk reports it unusually.
    swaps=Path('/proc/swaps').read_text().splitlines()[1:]
    names={d['name'] for d in flatten([disk])}
    if any(line.split()[0] in names for line in swaps):raise ValueError('Drive contains active swap.')
    return disk

@task('laundress','attached','Attached drives, including unmounted disks')
def attached(): run(['lsblk','-o','NAME,MODEL,SIZE,FSTYPE,MOUNTPOINTS,TRAN'])

@task('laundress','mount','Mount, unmount or safely eject')
def mount():
    d=choose_drive();op=pick('Drive operation',[(v,v) for v in ['mount','unmount','power-off']])
    confirm(op+' '+drive_label(d),op)
    current=next((v for v in flatten(drives()) if device_identity(v)==device_identity(d)),None)
    if not current:raise ValueError('Drive identity changed.')
    run(['udisksctl',op,'--block-device',d['name']])

@task('laundress','health','Disk health (SMART)')
def disk_health():
    d=choose_drive(whole=True);run(['sudo','smartctl','--all',d['name']],check=False)

def disk_write(mode):
    source=path_input('Raw disk image file') if mode=='image' else None
    d=choose_drive(whole=True);identity=device_identity(d);writable_device(identity)
    if source and (not source.is_file() or source.stat().st_size>int(d['size'])):raise ValueError('Image is not a file or does not fit on the target.')
    filesystem=pick('Filesystem',[('ext4','ext4'),('FAT32','vfat')]) if mode=='format' else ''
    print('ALL partitions and data on this disk will be lost.\n'+drive_label(d))
    if source:print('Image:',source,'\nSHA-256:',digest(source))
    confirm('Erase and '+mode+' this exact device: '+d['name'],'erase '+d['name'])
    writable_device(identity)
    run(['sudo',sys.executable,str(Path(__file__).resolve()),'--device',json.dumps(identity),'--mode',mode,
         '--source',str(source or ''),'--filesystem',filesystem])

@task('laundress','image','Write a raw image to a USB disk')
def write_image():disk_write('image')

@task('laundress','format','Format a removable disk')
def format_disk():disk_write('format')

@task('laundress','usage','Which folders fill a mounted drive')
def drive_usage():
    d=choose_drive()
    paths=[p for p in d.get('mountpoints',[]) if p and p.startswith('/')]
    path=pick('Mounted filesystems',[(p,p) for p in paths]);run(['du','-x','-h','--max-depth=1','--',path])

def backup_copy(source,destination):
    source=active_path(source);destination=active_path(destination)
    if source==destination or source in destination.parents or destination in source.parents:
        raise ValueError('Backup source and destination must be separate trees.')
    # Never --delete: a backup must not remove another version or user files.
    run(['rsync','-a','--safe-links','--checksum','--',str(source)+'/',str(destination)+'/'])

def verify_backup(source,destination):
    differences=run(['rsync','-a','--safe-links','--checksum','--dry-run','--itemize-changes','--',str(source)+'/',str(destination)+'/'],capture=True,timeout=3600)
    if differences.strip():raise RuntimeError('Backup differs from source:\n'+differences[:10000])

def backup_volume(path):
    """Identify the mounted filesystem, including for a not-yet-created folder."""
    path=active_path(path)
    while not path.exists():path=path.parent
    rows=json.loads(run(['findmnt','--json','--target',path,'--output','TARGET,SOURCE,UUID,FSTYPE,FSROOT'],capture=True))['filesystems']
    if len(rows)!=1:raise ValueError('Cannot identify the backup filesystem.')
    row=rows[0]
    return {key:row.get(key) for key in ('target','uuid','fstype','fsroot')} | ({'source':row.get('source')} if not row.get('uuid') else {})

def perform_backup(source,destination,source_volume=None,destination_volume=None):
    source=active_path(source);destination=active_path(destination)
    if not source.is_dir():raise ValueError('Backup source folder is unavailable.')
    if source==destination or source in destination.parents or destination in source.parents:
        raise ValueError('Backup source and destination must be separate trees.')
    for path,expected in ((source,source_volume),(destination,destination_volume)):
        if expected is not None and backup_volume(path)!=expected:
            raise ValueError('Backup filesystem is unavailable or changed: '+str(path))
    destination.mkdir(mode=0o700,exist_ok=True)
    snapshot=destination/('zell-backup-'+time.strftime('%Y%m%d-%H%M%S',time.gmtime())+'-'+uuid.uuid4().hex[:12])
    snapshot.mkdir(mode=0o700)
    # A unique snapshot never replaces the previous run or unrelated files.
    # Failed copies remain available for inspection but are not marked complete.
    backup_copy(source,snapshot);verify_backup(source,snapshot)
    size=int(run(['du','--summarize','--bytes','--',snapshot],capture=True).split()[0])
    record={'source':str(source),'destination':str(snapshot),'completed':time.time(),'bytes':size,'verified':True}
    from zell_schedule import locked
    with locked():
        rows=read_state('backups.json',[]);rows=rows if isinstance(rows,list) else []
        write_state('backups.json',(rows+[record])[-100:])
    print('Backup completed and checksum verification passed.\n'+str(snapshot))
    return snapshot

@task('vault','backup','Back up a chosen folder to a drive')
def backup():
    source=path_input('Folder to back up',directory=True)
    destination=path_input('Backup destination folder',exists=False)
    source_volume=backup_volume(source);destination_volume=backup_volume(destination)
    confirm(f'Create a new snapshot of {source} inside {destination}; keep previous copies and compare checksums.','backup')
    perform_backup(source,destination,source_volume,destination_volume)

def backup_record():
    rows=read_state('backups.json',[])
    return pick('Recorded backups',[(f"{time.ctime(r['completed'])} / {r['source']} -> {r['destination']}",r) for r in rows])

@task('vault','status','Backup status: last run, size, destination')
def backup_status():
    rows=read_state('backups.json',[])
    for r in rows:print(f"{time.ctime(r['completed'])}: {r['bytes']/2**30:.2f} GiB / {r['destination']} / checksum verified: {r['verified']}")
    if not rows:print('No completed backups recorded.')

@task('vault','restore','Restore a file or folder')
def restore():
    record=backup_record();root=Path(record['destination']).resolve()
    source=path_input('File or folder inside the backup').resolve()
    if source!=root and root not in source.parents:raise ValueError('Choose something inside the selected backup.')
    destination=target_new('New restore destination (existing files are never replaced)')
    confirm(f'Restore {source} to {destination}','restore')
    run(['cp','--archive','--no-clobber','--',source,destination])

@task('vault','verify','Verify a backup by checksum')
def verify():
    record=backup_record();verify_backup(Path(record['source']),Path(record['destination']));print('Checksums match the source.')

def settings_files():
    root=Path(os.environ.get('GPU_TERMINAL_HOME',Path.home()/'.local/gpu_terminal'))
    # Explicit allowlist excludes the secret vault, history, sessions and credentials.
    return {'kilix-settings':root/'settings.conf','zell-model':state_root()/'model.json',
            'zell-tasks':state_root()/'tasks.json','zell-preferences':state_root()/'journal.json'}

@task('vault','settings','Export or import Zell and Kilix settings')
def settings():
    mode=pick('Settings',[('Export selected settings','export'),('Import an export','import')])
    files=settings_files()
    if mode=='export':
        dest=target_new('New settings archive (.tar.gz)')
        if not str(dest).endswith('.tar.gz'):raise ValueError('Use .tar.gz.')
        confirm('Export only these settings (review them for private information):\n'+'\n'.join(str(p) for p in files.values() if p.is_file()),'export')
        with dest.open('xb') as stream:
            with tarfile.open(fileobj=stream,mode='w:gz') as archive:
                for key,path in files.items():
                    if path.is_file() and not path.is_symlink():archive.add(path,arcname=key)
        return
    source=path_input('Settings .tar.gz archive');rows,total=archive_members(source)
    if total>2**20 or any(name not in files or kind!='file' for name,_,kind,_ in rows):raise ValueError('Not a supported settings export.')
    confirm('Replace these settings; existing files will be saved with a unique backup suffix:\n'+'\n'.join(str(files[name]) for name,_,_,_ in rows),'import')
    with tarfile.open(source,'r:gz') as archive:
        for name,_,_,_ in rows:
            dest=files[name]
            if dest.is_symlink():raise ValueError('Settings destination is a symbolic link.')
            dest.parent.mkdir(parents=True,exist_ok=True,mode=0o700)
            if dest.exists():shutil.copyfile(dest,dest.with_name(dest.name+'.before-import-'+uuid.uuid4().hex))
            with archive.extractfile(name) as src:
                data=src.read(2**20+1)
            if len(data)>2**20:raise ValueError('Settings entry too large.')
            fd=os.open(dest,os.O_CREAT|os.O_WRONLY|os.O_TRUNC|os.O_NOFOLLOW,0o600)
            with os.fdopen(fd,'wb') as stream:stream.write(data)
    print('Settings imported. Restart affected apps to load them.')

@task('vault','schedule','Schedule a backup with Timekeeper')
def schedule_backup():
    from zell_schedule import create_schedule
    source=active_path(path_input('Folder to back up',directory=True))
    destination=active_path(path_input('Backup destination',exists=False))
    create_schedule({'kind':'backup','source':str(source),'destination':str(destination),
                     'source_volume':backup_volume(source),'destination_volume':backup_volume(destination)},'Backup '+source.name)

def model_manager():
    operation=pick('Local models',[('Installed inventory and disk use','installed'),('Download a Kilix catalog model','kilix-download'),('Download an Ollama model','download'),('Remove an Ollama model','remove'),('Trash a local GGUF model','trash-gguf')])
    if operation=='installed':
        run(['kilix-llm','doctor'],check=False);run(['ollama','list'],check=False)
        roots=[Path.home()/'.cache/kilix-llm',Path.home()/'.ollama/models',Path(os.environ.get('GPU_TERMINAL_HOME',Path.home()/'.local/gpu_terminal'))/'kilix-llm']
        for root in roots:
            if root.exists():run(['du','-sh','--',root])
        return
    if operation=='kilix-download':
        run(['kilix','models','list'])
        identity=ask('Model catalog ID',r'[a-zA-Z0-9][a-zA-Z0-9._-]*')
        run(['kilix','models','show',identity])
        confirm('Download the model shown above, including its displayed size and license requirements.','download')
        return run(['kilix','models','install',identity])
    if operation=='trash-gguf':
        path=ordinary(path_input('Local .gguf model file'))
        if path.suffix.lower()!='.gguf' or not path.is_file():raise ValueError('Choose a GGUF file.')
        confirm(f'Trash model {path} ({path.stat().st_size/2**30:.2f} GiB). Stop its model server first. Empty trash later to reclaim space.','trash')
        return run(['gio','trash','--',path])
    if operation=='remove':
        listing=run(['ollama','list'],capture=True)
        names=[line.split()[0] for line in listing.splitlines()[1:] if line.split()]
        name=pick('Installed Ollama models',[(n,n) for n in names]);confirm('Remove local model '+name,'remove');return run(['ollama','rm',name])
    # Fetch only registry metadata here; display exact blob sizes before pull.
    name=ask('Ollama registry model (for example qwen3:4b)',r'[a-zA-Z0-9][a-zA-Z0-9._:/-]*')
    if '/' in name and not name.startswith('library/'):raise ValueError('Use an official library model name.')
    model,_,tag=name.partition(':');tag=tag or 'latest';model=model.removeprefix('library/')
    if not re.fullmatch(r'[a-zA-Z0-9._-]+',model) or not re.fullmatch(r'[a-zA-Z0-9._-]+',tag):raise ValueError('Invalid model tag.')
    import urllib.request
    url=f'https://registry.ollama.ai/v2/library/{model}/manifests/{tag}'
    with urllib.request.urlopen(url,timeout=20) as response:manifest=json.loads(response.read(2**20))
    size=sum(int(layer['size']) for layer in manifest['layers'])
    confirm(f'Download {name}: {size/2**30:.2f} GiB of model blobs (disk use may be higher).','download')
    run(['ollama','pull',name])

def device_main():
    import argparse
    parser=argparse.ArgumentParser();parser.add_argument('--device',required=True);parser.add_argument('--mode',choices=['image','format'],required=True)
    parser.add_argument('--source',default='');parser.add_argument('--filesystem',choices=['','ext4','vfat'],default='')
    args=parser.parse_args();identity=json.loads(args.device)
    disk=writable_device(identity);path=disk['name']
    # The image writer owns O_EXCL. For formatting, mkfs itself acquires it;
    # holding a competing exclusive descriptor would make mkfs refuse every disk.
    fd=os.open(path,os.O_RDWR|os.O_NOFOLLOW|(os.O_EXCL if args.mode=='image' else 0))
    try:
        info=os.fstat(fd)
        if not stat.S_ISBLK(info.st_mode) or f'{os.major(info.st_rdev)}:{os.minor(info.st_rdev)}'!=identity['maj:min']:
            raise ValueError('Device descriptor does not match the reviewed disk.')
        writable_device(identity)
        if args.mode=='image':
            source=Path(args.source)
            if not source.is_file() or source.stat().st_size>int(disk['size']):raise ValueError('Invalid image size.')
            with source.open('rb') as stream:
                for chunk in iter(lambda:stream.read(2**20),b''):
                    remaining=memoryview(chunk)
                    while remaining:remaining=remaining[os.write(fd,remaining):]
            os.fsync(fd)
        else:
            if args.filesystem not in {'ext4','vfat'}:raise ValueError('Unsupported filesystem.')
            # Both formatters acquire O_EXCL on this reviewed block-device inode.
            run(['mkfs.'+args.filesystem,*(['-F'] if args.filesystem=='ext4' else ['-I','-F','32']),f'/proc/self/fd/{fd}'],pass_fds=(fd,))
            os.fsync(fd)
    finally:os.close(fd)
    print('Drive operation finished. Safely eject before unplugging.')

if __name__=='__main__':device_main()
