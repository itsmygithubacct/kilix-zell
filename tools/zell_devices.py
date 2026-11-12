"""Workshop controls with an automatic rollback for display changes."""
from __future__ import annotations
import os
from pathlib import Path
import re
import select
import sys
import tempfile
from zell_os import task, ask, pick, confirm, run, path_input, positive

def display_inventory(text):
    outputs=[];current=None
    for line in text.splitlines():
        match=re.match(r'^(\S+) connected(?: primary)?(?: (\d+x\d+)\+(\d+)\+(\d+))?',line)
        if match:
            current={'name':match[1],'mode':match[2],'pos':f'{match[3]}x{match[4]}' if match[2] else None,'modes':[]}
            outputs.append(current)
        elif line and not line[0].isspace():current=None
        elif current:
            mode=re.match(r'^\s+(\d+x\d+)\s',line)
            if mode:current['modes'].append(mode[1])
    return outputs

def restore_displays(outputs):
    command=['xrandr']
    for output in outputs:
        command+=['--output',output['name']]
        command+=['--mode',output['mode'],'--pos',output['pos']] if output['mode'] else ['--off']
    return command

@task('tinkerer','displays','Display resolution, arrangement and external monitors')
def displays():
    text=run(['xrandr','--query'],capture=True);print(text)
    outputs=display_inventory(text)
    selected=pick('Connected display',[(v['name'],v) for v in outputs])
    op=pick('Display action',[('Resolution','mode'),('Position relative to another display','position'),('Enable','on'),('Disable','off')])
    command=['xrandr','--output',selected['name']]
    if op=='mode':command+=['--mode',pick('Resolution',[(v,v) for v in selected['modes']])]
    elif op=='position':
        other=pick('Reference display',[(v['name'],v['name']) for v in outputs if v!=selected])
        where=pick('Arrangement',[(v,v) for v in ['left-of','right-of','above','below','same-as']])
        command+=['--auto','--'+where,other]
    else:
        if op=='off' and sum(bool(v['mode']) for v in outputs)<=1:raise ValueError('Keep at least one display enabled.')
        command+=['--off' if op=='off' else '--auto']
    confirm('Apply display change for 20 seconds:\n'+' '.join(command)+'\nType keep afterward, or the previous layout returns.','apply')
    rollback=restore_displays(outputs)
    kept=False
    try:
        run(command)
        print('Type keep and Enter within 20 seconds to keep this layout: ',end='',flush=True)
        kept=bool(select.select([sys.stdin],[],[],20)[0] and sys.stdin.readline().strip()=='keep')
    finally:
        if not kept:
            run(rollback)
            print('Previous display layout restored.')

@task('tinkerer','sound','Sound output and input device')
def sound():
    kind=pick('Audio devices',[('Output','sink'),('Input','source')])
    listing=run(['pactl','list','short',kind+'s'],capture=True)
    rows=[(line,line.split()[1]) for line in listing.splitlines() if len(line.split())>=2]
    identity=pick('Available '+kind+' devices',rows)
    confirm('Make '+identity+' the default '+kind+'.','set')
    run(['pactl','set-default-'+kind,identity])

@task('tinkerer','sound-test','Test sound')
def sound_test():
    default=Path('/usr/share/sounds/alsa/Front_Center.wav')
    path=default if default.is_file() else path_input('Short audio file to play')
    confirm('Play test sound through the current output: '+str(path),'play')
    run(['paplay',path])

@task('tinkerer','microphone','Microphone volume and level check')
def microphone():
    run(['pactl','get-source-volume','@DEFAULT_SOURCE@'])
    op=pick('Microphone',[('Set input volume','volume'),('Measure input for five seconds','level')])
    if op=='volume':
        value=int(ask('Percent (0-100)',r'(?:[0-9]|[1-9][0-9]|100)'))
        confirm('Set microphone input to '+str(value)+'%.','set');run(['pactl','set-source-volume','@DEFAULT_SOURCE@',str(value)+'%'])
    else:
        confirm('Listen to the microphone for five seconds and show levels. Audio is not saved.','listen')
        run(['ffmpeg','-hide_banner','-f','pulse','-i','default','-t','5','-af','volumedetect','-f','null','-'])

@task('tinkerer','printers','Printers: print, queue and cancel')
def printers():
    op=pick('Printing',[(v,v) for v in ['list','print','queue','cancel']])
    if op=='list':return run(['lpstat','-p','-d'])
    if op=='queue':return run(['lpstat','-o'])
    if op=='cancel':
        listing=run(['lpstat','-o'],capture=True)
        identity=pick('Queued print jobs',[(line,line.split()[0]) for line in listing.splitlines() if line.split()])
        confirm('Cancel print job '+identity,'cancel');return run(['cancel',identity])
    path=path_input('File to print')
    listing=run(['lpstat','-p'],capture=True)
    names=[line.split()[1] for line in listing.splitlines() if line.startswith('printer ') and len(line.split())>1]
    printer=pick('Printer',[(n,n) for n in names])
    confirm(f'Print {path} on {printer}.','print');run(['lp','-d',printer,'--',path])

@task('tinkerer','keyboard','Keyboard layout')
def keyboard():
    run(['setxkbmap','-query'])
    layout=ask('X11 layout code (for example us, gb, de)',r'[a-z]{2,3}(?:,[a-z]{2,3})*')
    confirm('Use keyboard layout '+layout+' for this desktop session.','set')
    run(['setxkbmap','-layout',layout])

@task('tinkerer','sensors','Temperatures and fans')
def sensors():run(['sensors'])

@task('tinkerer','camera','Webcam preview')
def camera():
    devices=sorted(Path('/dev').glob('video[0-9]*'))
    device=pick('Camera',[(str(p),p) for p in devices])
    confirm('Open a local preview from '+str(device)+'. Nothing is recorded. Close the preview to return.','preview')
    run(['ffplay','-hide_banner','-f','video4linux2','-i',device])

@task('tinkerer','screenshot','Screenshot or screen region capture')
def screenshot():
    from zell_files import target_new
    region=pick('Screenshot',[('Entire desktop',False),('Select a region',True)])
    path=target_new('New PNG file')
    if path.suffix.lower()!='.png':raise ValueError('Use a .png filename.')
    confirm('Capture '+('a selected region' if region else 'the desktop')+' to '+str(path),'capture')
    run(['scrot',*(['--select'] if region else ['--delay','3']),'--',path])

@task('tinkerer','brightness','Display brightness')
def brightness():
    run(['brightnessctl','info'])
    value=positive('Brightness percent (1-100)',100)
    confirm('Set backlight brightness to '+str(value)+'%.','set')
    run(['brightnessctl','set',str(value)+'%'])
