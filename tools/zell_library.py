"""Reference-only local libraries. Indexing never moves or deletes source files."""
import os
from pathlib import Path

KINDS={
    'music':{'.mp3','.flac','.ogg','.opus','.m4a','.aac','.wav','.wma','.aiff'},
    'documents':{'.epub','.pdf','.txt','.md','.djvu','.mobi','.azw3','.docx','.odt','.rtf','.html'},
}
LIMIT=10000

def guide(ui,kind):
    ui.page_text('FIND AND ADD YOUR '+('MUSIC' if kind=='music' else 'BOOKS AND DOCUMENTS')+'\n\n'
        'Start in ~/Music, ~/Books, ~/Documents or ~/Downloads. Files copied from a USB drive may be under /media or /run/media. '
        'Use Add a folder to scan a specific location, including its subfolders; use Add a file for a single item. '
        'You can type a full path or use ~ for your home folder. Spaces in names are fine; do not add shell quotes.\n\n'
        'Adding creates library references: your originals stay where they are. Refresh finds new files in saved folders. '
        'If a disk is disconnected or a file moved, reconnect it or add its new location. Remove from library only forgets the reference.\n\n'
        'Browse and search filters by filename and path. Folder scans skip hidden folders, symlinks and common build/cache directories, '
        'and stop at 10,000 entries/files. Add a narrower folder if a scan reaches that limit.\n\n'
        +('Select a track to open Kilix Amp. YouTube creators and playlists live in the separate Playlists and creators menu.'
          if kind=='music' else 'Select an item to read or open it. For scanned PDFs/images, use the Librarian OCR actions first. '
          'Read aloud and audiobook export currently accept TXT or Markdown; convert other formats to text first.'))

class Library:
    def __init__(self,ui,kind):
        self.ui=ui;self.kind=kind;self.name='library-'+kind+'.json'
        data=ui.state_read(self.name,{})
        self.data={'schema':'kilix.zell.local-library/v1','files':[],'folders':[]}
        if isinstance(data,dict):
            for key in ('files','folders'):
                self.data[key]=list(dict.fromkeys(p for p in data.get(key,[]) if isinstance(p,str) and Path(p).is_absolute())) if isinstance(data.get(key),list) else []

    def save(self):self.ui.state_write(self.name,self.data)

    def scan(self,root):
        found=[];visited=0;errors=[]
        def failure(error):errors.append(str(error))
        for directory,dirs,files in os.walk(root,followlinks=False,onerror=failure):
            dirs[:]=sorted(d for d in dirs if not d.startswith('.') and d not in {'node_modules','build','target','__pycache__'} and not (Path(directory)/d).is_symlink())
            visited+=len(dirs)+len(files)
            for name in sorted(files):
                p=Path(directory)/name
                if not name.startswith('.') and p.suffix.lower() in KINDS[self.kind] and not p.is_symlink() and p.is_file():found.append(str(p))
                if len(found)>=LIMIT:break
            if visited>=LIMIT or len(found)>=LIMIT:
                errors.append('Scan limit reached; add a smaller folder to include more files.');break
        return found,errors

    def add(self,value,folder=False):
        raw=Path(value).expanduser()
        if raw.is_symlink():raise ValueError('Choose the original file or folder, not a symbolic link.')
        path=raw.resolve()
        if folder:
            if not path.is_dir():raise ValueError('That folder is unavailable.')
            found,errors=self.scan(path)
            self.data['folders']=list(dict.fromkeys(self.data['folders']+[str(path)]))
        else:
            if not path.is_file() or path.suffix.lower() not in KINDS[self.kind]:raise ValueError('Choose a supported '+self.kind+' file.')
            found=[str(path)];errors=[]
        before=len(self.data['files'])
        self.data['files']=list(dict.fromkeys(self.data['files']+found));self.save()
        return len(self.data['files'])-before,errors

    def refresh(self):
        added=0;errors=[]
        for folder in self.data['folders'][:]:
            try:
                n,issues=self.add(folder,folder=True);added+=n;errors+=issues
            except (OSError,ValueError) as error:errors.append(str(error)+' '+folder)
        return added,errors

    def remove_file(self,path):
        self.data['files']=[p for p in self.data['files'] if p!=path];self.save()


def hub(ui,kind):
    library=Library(ui,kind)
    while True:
        action=ui.choose(('Music' if kind=='music' else 'Books and documents')+' on disk',[
            ('How to find files and add them','help'),
            (f'Browse and search ({len(library.data["files"])})','browse'),
            ('Add a file','file'),('Add a folder and its subfolders','folder'),
            ('Refresh saved folders','refresh'),('Manage saved folders','folders')])
        if action is None:return
        if action=='help':guide(ui,kind)
        elif action in {'file','folder'}:
            if action=='folder':
                common=[Path.home()/n for n in ('Music','Books','Documents','Downloads')]
                selected=ui.choose('Where to look',[(str(p),str(p)) for p in common if p.is_dir()]+[('Enter another folder path','custom')])
                if selected is None:continue
                value=input('Folder path: ').strip() if selected=='custom' else selected
            else:
                selected=ui.file_pick(Path.home(),KINDS[kind]);value=str(selected) if selected else ''
            if not value:continue
            try:
                count,errors=library.add(value,folder=action=='folder')
                ui.page_text(f'Added {count} files. Originals remain in place.\n'+'\n'.join(errors))
            except (OSError,ValueError) as error:ui.page_text(str(error))
        elif action=='refresh':
            count,errors=library.refresh();ui.page_text(f'Added {count} new files.\n'+'\n'.join(errors))
        elif action=='folders':
            folder=ui.choose('Saved folders',[(p,p) for p in library.data['folders']])
            if folder and ui.choose(folder,[('Stop scanning this folder (keep indexed files)','remove')])=='remove':
                library.data['folders'].remove(folder);library.save()
        elif action=='browse':
            value=ui.choose('Library: filter by filename or path',[(Path(p).name+' | '+p,p) for p in sorted(library.data['files'],key=str.casefold)])
            if not value:continue
            operation=ui.choose(Path(value).name,[('Open in Kilix Amp' if kind=='music' else 'Read / open document','open'),('Show location','where'),('Remove from library (keep file)','remove')])
            if operation=='remove':library.remove_file(value)
            elif operation=='where':ui.page_text(value+'\n\n'+('Available on disk.' if Path(value).is_file() else 'Unavailable. Reconnect the disk or add the new path.')+'\n\nRefresh can add this file again if it is inside a saved folder.')
            elif operation=='open':
                if not Path(value).is_file():ui.page_text('This file is unavailable. Reconnect its disk or add the new location.');continue
                if kind=='music':ui.zell_media.play(ui.media_ui(),value,listen=True)
                else:ui.open_file(Path(value))
