"""Read desktop notices and system messages, and sample the source of fatigue."""
from contextlib import closing
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import time

from zell_os import read_state, write_state
from zell_schedule import locked
from zell_vitals import cpu_memory


def clean(value, limit=220):
    return ' '.join(''.join(ch if ch.isprintable() else ' ' for ch in str(value)).split())[:limit]


def program_json(args):
    try:
        result = subprocess.run(args, capture_output=True, text=True, errors='replace', timeout=2)
        return result.stdout if result.returncode == 0 else ''
    except (OSError, subprocess.TimeoutExpired):
        return ''


def notification_rows():
    db = Path(os.environ.get('XDG_CACHE_HOME') or Path.home()/'.cache')/'xfce4/notifyd/log.sqlite'
    if db.is_file():
        try:
            with closing(sqlite3.connect(db.resolve().as_uri()+'?mode=ro', uri=True, timeout=.15)) as connection:
                for stamp, app, title, body in connection.execute(
                    'SELECT timestamp, app_name, summary, body FROM notifications ORDER BY timestamp DESC LIMIT 40'):
                    yield int(stamp)//1000000, clean(app or 'Desktop', 24), clean(' '.join(filter(None,(title,body))))
            return
        except (OSError, sqlite3.Error, ValueError, TypeError):
            pass
    if shutil.which('dunstctl'):
        try:
            data = json.loads(program_json(['dunstctl','history']))
            for group in data.get('data', []):
                for row in group[:40]:
                    value = lambda key: row.get(key, {}).get('data', '')
                    # Dunst timestamps use monotonic microseconds.
                    stamp = time.time()-time.monotonic()+int(value('timestamp'))/1e6
                    yield int(stamp), clean(value('appname') or 'Desktop',24), clean(f"{value('summary')} {value('body')}")
        except (ValueError, TypeError, AttributeError):
            pass


def history():
    rows = list(notification_rows())
    if shutil.which('journalctl'):
        lines = program_json(['journalctl','-q','-b','-n','36','--no-pager','-o','json'])
        for line in lines.splitlines():
            try:
                row = json.loads(line)
                rows.append((int(row['__REALTIME_TIMESTAMP'])//1000000,
                             clean(row.get('SYSLOG_IDENTIFIER') or 'System',24), clean(row['MESSAGE'])))
            except (ValueError, KeyError, TypeError):
                continue
    for filename, source in (('notices.json','Zell'), ('speech-history.json','Kilix')):
        records = read_state(filename, [])
        for row in records[-80:] if isinstance(records,list) else []:
            if isinstance(row,dict) and isinstance(row.get('time'),(int,float)) and isinstance(row.get('text'),str):
                rows.append((int(row['time']), source, clean(row['text'])))
    return sorted(set(rows),reverse=True)[:100]


class FatigueSpeech:
    def __init__(self, proc_root=Path('/proc')):
        self.proc_root = proc_root
        self.cpu = None
        self.processes = {}
        self.last_spoke = 0

    def sample(self, headroom):
        try:
            cpu, memory = cpu_memory()
            usage = {}
            for entry in os.scandir(self.proc_root):
                if not entry.name.isdigit():
                    continue
                try:
                    raw = (Path(entry.path)/'stat').read_text(errors='replace')
                    name, _, rest = raw.partition('(')[2].rpartition(')')
                    fields = rest.split()
                    usage[(entry.name,fields[19])] = (clean(name,24), int(fields[11])+int(fields[12]), int(fields[21]))
                except (OSError, IndexError, ValueError):
                    continue
            old, previous = self.cpu, self.processes
            self.cpu, self.processes = cpu, usage
            now = time.monotonic()
            if old is None or cpu[0]<=old[0] or not 0<=headroom<=75 or now-self.last_spoke<120:
                return None
            idle = 100*(cpu[1]-old[1])/(cpu[0]-old[0])
            ram = memory<=idle
            if min(memory,idle)>75:
                return None
            totals = {}
            for pid,(name,ticks,rss) in usage.items():
                weight = rss if ram else ticks-previous.get(pid,('',ticks,0))[1]
                if name and weight>0:
                    totals[name] = totals.get(name,0)+weight
            names = sorted(totals,key=totals.get,reverse=True)[:2]
            if not names:
                return None
            verb = 'use' if len(names)>1 else 'uses'
            text = ('Memory is tight. ' if ram else "I'm feeling tired. ")+' and '.join(names)+f' {verb} the most '+('RAM.' if ram else 'CPU.')
            stamp = int(time.time())
            with locked():
                rows = read_state('speech-history.json',[])
                write_state('speech-history.json',((rows if isinstance(rows,list) else [])+[{'time':stamp,'text':text}])[-80:])
            self.last_spoke = now
            return stamp,text
        except (OSError, ValueError, IndexError, KeyError, ZeroDivisionError):
            return None
