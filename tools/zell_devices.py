"""Workshop controls. Lock, display, and theme changes call their existing owners."""
from __future__ import annotations
import copy
import json
import os
from pathlib import Path
import re
import tempfile
from zell_os import task, ask, pick, confirm, run, path_input, positive, tool

DISPLAY_FIELDS = ('name','identity','enabled','primary','mode','rate','x','y','rotation','scale','filter')
ROTATIONS = ('normal','left','right','inverted')
FILTERS = ('','nearest','bilinear')
LOCK_POLICIES = ('session','on','off')
NEXT_LOGIN = 'Changes apply at your next login.'
BLANKING = (
    ('dpms-enabled','bool','/xfce4-power-manager/dpms-enabled'),
    ('dpms-on-ac-sleep','uint','/xfce4-power-manager/dpms-on-ac-sleep'),
    ('dpms-on-ac-off','uint','/xfce4-power-manager/dpms-on-ac-off'),
    ('dpms-on-battery-sleep','uint','/xfce4-power-manager/dpms-on-battery-sleep'),
    ('dpms-on-battery-off','uint','/xfce4-power-manager/dpms-on-battery-off'),
)
THEME_PROPS = {
    'theme':'/Net/ThemeName',
    'icons':'/Net/IconThemeName',
    'cursor':'/Gtk/CursorThemeName',
}
NIGHT_MARKER = '# kilix-zell night light 1\n'
AWAKE = ['systemd-inhibit','--what','idle:sleep','--who','Zell Tinkerer',
         '--why','Stay awake for one hour','--mode','block','sleep','3600']

def capture(argv):
    return run(argv, capture=True, detail=True)

def write_private(path, text):
    path = Path(path)
    parent = path.parent
    if parent.is_symlink() or path.is_symlink():
        raise ValueError('Refusing to follow a symlink. Nothing was changed.')
    created = not parent.exists()
    parent.mkdir(parents=True, exist_ok=True)
    if created:
        os.chmod(parent, 0o700)
    fd, temporary = tempfile.mkstemp(prefix='.'+path.name+'-', dir=parent)
    temporary = Path(temporary)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, 'w') as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)
    return path

def layout_document(layout):
    if not isinstance(layout, dict) or layout.get('version') != 2 or not isinstance(layout.get('outputs'), list) or not layout['outputs']:
        raise ValueError('Unsupported layout format. Nothing was changed.')
    cleaned = []
    for output in layout['outputs']:
        if not isinstance(output, dict) or set(output) != set(DISPLAY_FIELDS):
            raise ValueError('A layout record must contain only display settings. Nothing was changed.')
        cleaned.append({key: output[key] for key in DISPLAY_FIELDS})
    return {'version':2, 'outputs':cleaned}

def parse_inventory(data):
    if not isinstance(data, dict) or not isinstance(data.get('outputs'), list) or not data['outputs']:
        raise ValueError('pleb displays list did not return a display inventory. Nothing was changed.')
    maximum = data.get('maximum')
    if (not isinstance(maximum, list) or len(maximum) != 2
            or any(type(value) is not int or value <= 0 for value in maximum)):
        raise ValueError('Display inventory has no framebuffer limit. Nothing was changed.')
    return data

def validate_layout(layout, inventory):
    layout = layout_document(layout)
    inventory = parse_inventory(inventory)
    outputs = layout['outputs']
    actual = {output['name']: output for output in inventory['outputs']}
    if sorted((output['name'], output['identity']) for output in outputs) != sorted(
            (output['name'], output['identity']) for output in inventory['outputs']):
        raise ValueError('Connected monitors differ from this layout. Nothing was changed.')
    if len({output['name'] for output in outputs}) != len(outputs):
        raise ValueError('Duplicate output. Nothing was changed.')
    maximum = inventory['maximum']
    active = []
    for output in outputs:
        if type(output['enabled']) is not bool or type(output['primary']) is not bool:
            raise ValueError('Output flags must be booleans. Nothing was changed.')
        source = actual.get(output['name'])
        if source is None or source.get('safe') is not True:
            raise ValueError('This output cannot be safely previewed. Nothing was changed.')
        scale = output['scale']
        if isinstance(scale, bool) or not isinstance(scale, (int, float)) or not 0.5 <= scale <= 4:
            raise ValueError('Scale must be a number from 0.5 to 4. Nothing was changed.')
        if output['filter'] not in FILTERS or (scale != 1 and not output['filter']):
            raise ValueError('Scaled outputs require a nearest or bilinear filter. Nothing was changed.')
        if not output['enabled']:
            if output['primary']:
                raise ValueError('A disabled output cannot be primary. Nothing was changed.')
            continue
        if output['rotation'] not in ROTATIONS:
            raise ValueError('Invalid rotation. Nothing was changed.')
        if not isinstance(output['mode'], str) or not re.fullmatch(r'[0-9]+x[0-9]+', output['mode']):
            raise ValueError('Only standard width-by-height modes are supported. Nothing was changed.')
        if not isinstance(output['rate'], str) or output['rate'] not in source.get('modes', {}).get(output['mode'], []):
            raise ValueError('Unsupported mode or refresh for '+output['name']+'. Nothing was changed.')
        if any(type(output[key]) is not int or output[key] < 0 for key in ('x','y')):
            raise ValueError('Positions must be nonnegative integers. Nothing was changed.')
        width, height = map(int, output['mode'].split('x'))
        if output['rotation'] in ('left','right'):
            width, height = height, width
        if output['x'] + width > maximum[0] or output['y'] + height > maximum[1]:
            raise ValueError('Layout exceeds the server framebuffer limit. Nothing was changed.')
        active.append(output)
    if not active or sum(output['primary'] for output in active) != 1:
        raise ValueError('Select at least one enabled output and exactly one primary. Nothing was changed.')
    return layout

def output_size(output):
    width, height = map(int, output['mode'].split('x'))
    return (height, width) if output['rotation'] in ('left','right') else (width, height)

def place_output(output, where, other):
    width, height = output_size(output)
    other_width, other_height = output_size(other)
    if where == 'left-of':
        output['x'], output['y'] = other['x'] - width, other['y']
    elif where == 'right-of':
        output['x'], output['y'] = other['x'] + other_width, other['y']
    elif where == 'above':
        output['x'], output['y'] = other['x'], other['y'] - height
    elif where == 'below':
        output['x'], output['y'] = other['x'], other['y'] + other_height
    elif where == 'same-as':
        output['x'], output['y'] = other['x'], other['y']
    else:
        raise ValueError('Choose a listed arrangement. Nothing was changed.')
    if output['x'] < 0 or output['y'] < 0:
        raise ValueError('That arrangement does not fit. Nothing was changed.')

def edit_layout(layout, inventory):
    layout = layout_document(layout)
    outputs = layout['outputs']
    selected = pick('Connected display', [(output['name'], output) for output in outputs])
    operation = pick('Display action', [
        ('Resolution and refresh','mode'), ('Scale','scale'), ('Rotation','rotation'),
        ('Position','position'), ('Primary','primary'), ('Enable','on'), ('Disable','off')])
    source = next(output for output in inventory['outputs'] if output['name'] == selected['name'])
    if operation == 'mode':
        mode = pick('Resolution', [(mode, mode) for mode in source.get('modes', {}) if re.fullmatch(r'[0-9]+x[0-9]+', mode)])
        selected['mode'] = mode
        selected['rate'] = pick('Refresh', [(rate, rate) for rate in source['modes'][mode]])
        selected['enabled'] = True
    elif operation == 'scale':
        scale = float(pick('Scale', [(str(value), str(value)) for value in (1, 1.25, 1.5, 2)]))
        selected['scale'] = scale
        selected['filter'] = '' if scale == 1 else 'bilinear'
        selected['enabled'] = True
    elif operation == 'rotation':
        selected['rotation'] = pick('Rotation', [(value, value) for value in ROTATIONS])
    elif operation == 'position':
        other = pick('Reference display', [(output['name'], output) for output in outputs if output is not selected and output['enabled']])
        place_output(selected, pick('Arrangement', [(value, value) for value in ('left-of','right-of','above','below','same-as')]), other)
    elif operation == 'primary':
        if not selected['enabled']:
            raise ValueError('Enable the output before making it primary. Nothing was changed.')
        for output in outputs:
            output['primary'] = output is selected
    elif operation == 'off':
        if sum(output['enabled'] for output in outputs) <= 1 and selected['enabled']:
            raise ValueError('Keep at least one display enabled. Nothing was changed.')
        selected['enabled'] = False
        selected['primary'] = False
        enabled = [output for output in outputs if output['enabled']]
        if enabled and not any(output['primary'] for output in enabled):
            enabled[0]['primary'] = True
    else:
        selected['enabled'] = True
        if not any(output['primary'] for output in outputs if output['enabled']):
            selected['primary'] = True
    return validate_layout(layout, inventory)

class DisplayPreview:
    """One display preview. Only a matching confirmation is saved.

    Timeout, cancellation, a dead UI, a hotplug, and a failed read-back restore
    the layout that was current when the preview opened.
    """
    def __init__(self, saved, timeout=15):
        if type(timeout) is not int or timeout != 15:
            raise ValueError('Display preview waits 15 seconds.')
        self.saved = copy.deepcopy(saved)
        self.live = copy.deepcopy(saved)
        self.proposed = None
        self.open = False

    def start(self, proposed):
        if self.open:
            raise RuntimeError('A display preview is already open.')
        self.proposed = copy.deepcopy(proposed)
        self.live = copy.deepcopy(proposed)
        self.open = True

    def confirm(self):
        self._require()
        matched = self.live == self.proposed
        self.open = False
        if not matched:
            self.live = copy.deepcopy(self.saved)
            self.proposed = None
            return 'reverted'
        self.saved = copy.deepcopy(self.proposed)
        return 'confirmed'

    def cancel(self):
        return self._revert()

    def timeout(self):
        return self._revert()

    def ui_dead(self):
        self._require()
        self.live = None
        return self._revert()

    def hotplug(self):
        self._require()
        self.live = {'version':2, 'outputs':[]}
        return self._revert()

    def _revert(self):
        self._require()
        self.live = copy.deepcopy(self.saved)
        self.proposed = None
        self.open = False
        return 'reverted'

    def _require(self):
        if not self.open:
            raise RuntimeError('No display preview is open.')

def commit_preview(layout, inventory, directory=None):
    """Ask pleb displays to preview one private layout. The file is removed either way."""
    layout = validate_layout(layout, inventory)
    tool('pleb')
    owned = directory is None
    directory = Path(tempfile.mkdtemp(prefix='zell-display-')) if owned else Path(directory)
    if owned:
        os.chmod(directory, 0o700)
    path = directory/'layout.json'
    try:
        write_private(path, json.dumps(layout, sort_keys=True)+'\n')
        code = run(['pleb','displays','preview', str(path), '--timeout','15'], check=False)
    finally:
        path.unlink(missing_ok=True)
        if owned:
            directory.rmdir()
    if code == 0:
        print('Layout confirmed and saved.')
    elif code == 130:
        print('Cancelled. The previous layout was restored.')
    else:
        print('Previous layout restored. Nothing was saved.')
    return code

@task('tinkerer','displays','Display resolution, arrangement and external monitors')
def displays():
    inventory = parse_inventory(json.loads(capture(['pleb','displays','list'])))
    layout = layout_document(json.loads(capture(['pleb','displays','snapshot'])))
    print(json.dumps(layout, indent=2, sort_keys=True))
    layout = edit_layout(layout, inventory)
    confirm('Preview this layout for 15 seconds through pleb displays. '
            'Only an explicit confirmation saves it. Timeout, cancellation, a failed read-back, '
            'or a monitor change restores the previous layout.', 'preview')
    return commit_preview(layout, inventory)

def validate_screen_lock(data):
    if (not isinstance(data, dict) or set(data) != {'version','policy','idle_seconds'}
            or type(data['version']) is not int or data['version'] != 1
            or data['policy'] not in LOCK_POLICIES
            or type(data['idle_seconds']) is not int or not 0 <= data['idle_seconds'] <= 86400):
        raise ValueError('Screen-lock settings were not a policy and an idle timeout. Nothing was changed.')
    return data

def apply_screen_lock(policy, idle, previous, runner):
    if policy not in LOCK_POLICIES or type(idle) is not int or not 0 <= idle <= 86400:
        raise ValueError('Choose session, on, or off and an idle timeout from 0 to 86400 seconds. Nothing was changed.')
    previous = validate_screen_lock(previous)
    if policy == previous['policy'] and idle == previous['idle_seconds']:
        print('Unchanged. '+NEXT_LOGIN+' Lid settings were not changed.')
        return previous
    runner(['pleb','screen-lock','--set', policy, '--idle-seconds', str(idle)])
    try:
        shown = validate_screen_lock(json.loads(runner(['pleb','screen-lock','--show'])))
        matched = (shown['policy'], shown['idle_seconds']) == (policy, idle)
    except (OSError, RuntimeError, ValueError, TypeError):
        matched = False
    if not matched:
        try:
            runner(['pleb','screen-lock','--set', previous['policy'], '--idle-seconds', str(previous['idle_seconds'])])
        except (OSError, RuntimeError, ValueError):
            raise RuntimeError('Screen-lock read-back did not match and the previous policy could not be restored. '+NEXT_LOGIN) from None
        raise RuntimeError('Screen-lock read-back did not match the requested policy. The previous policy was requested again. '+NEXT_LOGIN)
    print('Saved. '+NEXT_LOGIN+' Lid settings were not changed.')
    return shown

def screen_lock():
    current = validate_screen_lock(json.loads(capture(['pleb','screen-lock','--show'])))
    print(json.dumps(current, sort_keys=True))
    print(NEXT_LOGIN+' This is one combined idle policy. Lid settings stay as they are.')
    policy = pick('Automatic locking', [('Follow session defaults','session'), ('Enabled','on'), ('Disabled','off')])
    idle = int(ask('Idle seconds from 0 to 86400 (0 disables the idle timer)', r'(?:0|[1-9][0-9]{0,4})'))
    if idle > 86400:
        raise ValueError('Idle timeout must be from 0 to 86400 seconds. Nothing was changed.')
    confirm('Save automatic locking as '+policy+' after '+str(idle)+' seconds. '+NEXT_LOGIN, 'set')
    return apply_screen_lock(policy, idle, current, capture)

def lock_screen():
    confirm('Lock this desktop now. The supervised locker is used when this is the Pleb session.', 'lock')
    try:
        run(['pleb','lock'], detail=True)
    except RuntimeError as error:
        raise RuntimeError('The screen was not locked. '+str(error)+' An unanswered lock request is not success.') from None
    print('The lock command finished.')

def blanking_argv(prop, kind, value):
    if 'lid-action' in prop or prop.endswith('lock-screen-suspend-hibernate') or kind not in {'bool','uint'}:
        raise ValueError('That power setting is owned by the screen-lock policy. Nothing was changed.')
    if prop not in {item[2] for item in BLANKING}:
        raise ValueError('Unsupported display power setting. Nothing was changed.')
    if kind == 'bool' and value not in {'true','false'}:
        raise ValueError('Choose true or false. Nothing was changed.')
    if kind == 'uint' and (not re.fullmatch(r'[0-9]+', value) or not 0 <= int(value) <= 240):
        raise ValueError('Choose a delay from 0 to 240 minutes. Nothing was changed.')
    return ['xfconf-query','-c','xfce4-power-manager','-p', prop, '--create','--type', kind, '--set', value]

def apply_blanking(prop, kind, value, runner):
    argv = blanking_argv(prop, kind, value)
    try:
        runner(['xfconf-query','-c','xfce4-power-manager','-l'])
    except FileNotFoundError:
        raise
    except (OSError, RuntimeError) as error:
        raise RuntimeError('This session has no xfce4-power-manager settings. Nothing was changed. '+str(error)) from None
    try:
        previous = runner(['xfconf-query','-c','xfce4-power-manager','-p', prop]).strip()
    except (OSError, RuntimeError):
        previous = None
    runner(argv)
    try:
        shown = runner(['xfconf-query','-c','xfce4-power-manager','-p', prop]).strip()
        matched = shown == value
    except (OSError, RuntimeError, ValueError, TypeError):
        matched = False
    if not matched:
        try:
            if previous is None:
                runner(['xfconf-query','-c','xfce4-power-manager','-p', prop, '-r'])
            else:
                runner(blanking_argv(prop, kind, previous))
        except (OSError, RuntimeError, ValueError):
            raise RuntimeError('Display power read-back did not match and the previous value could not be restored.') from None
        raise RuntimeError('Display power read-back did not match. The previous value was requested again.')
    print('Saved '+prop+' = '+shown+'.')
    return shown

def stay_awake():
    confirm('Keep this session awake for one hour. Saved blanking and lock settings stay as they are. Cancelling releases the hold.', 'awake')
    run(AWAKE)
    print('The one-hour hold has ended. Saved settings were not changed.')

def blanking():
    operation = pick('Display power', [('Show display power settings','show'), ('Change a display power setting','set'),
                                       ('Stay awake for one hour','awake')])
    if operation == 'awake':
        return stay_awake()
    try:
        print(capture(['xfconf-query','-c','xfce4-power-manager','-l']))
    except FileNotFoundError:
        raise
    except (OSError, RuntimeError) as error:
        raise RuntimeError('This session has no xfce4-power-manager settings. Nothing was changed. '+str(error)) from None
    if operation == 'show':
        for _name, _kind, prop in BLANKING:
            try:
                print(prop+' = '+capture(['xfconf-query','-c','xfce4-power-manager','-p', prop]).strip())
            except (OSError, RuntimeError):
                print(prop+' is unset.')
        print('Idle locking is the automatic screen-lock policy. '+NEXT_LOGIN)
        return
    choice = pick('Display power setting', [(prop, (kind, prop)) for _name, kind, prop in BLANKING])
    kind, prop = choice
    if kind == 'bool':
        value = pick(prop, [('Enabled','true'), ('Disabled','false')])
    else:
        value = ask('Minutes from 0 to 240', r'(?:0|[1-9][0-9]{0,2})')
        if int(value) > 240:
            raise ValueError('Choose a delay from 0 to 240 minutes. Nothing was changed.')
    confirm('Save '+prop+' as '+value+'. Lid settings are not changed.', 'set')
    return apply_blanking(prop, kind, value, capture)

def night_light_text(temp):
    if type(temp) is not int or not 1000 <= temp <= 6500:
        raise ValueError('Night light temperature must be from 1000 to 6500 K. Nothing was changed.')
    return (NIGHT_MARKER+'[redshift]\n'+f'temp-day={temp}\n'+f'temp-night={temp}\n'
            +'location-provider=manual\nadjustment-method=randr\n\n[manual]\nlat=0.0\nlon=0.0\n')

def night_light_desktop(config, enabled):
    path = str(config)
    if not re.fullmatch(r'/[A-Za-z0-9_./-]+', path):
        raise ValueError('Night-light configuration path is not a plain absolute path. Nothing was changed.')
    return ('[Desktop Entry]\nType=Application\nName=Kilix Zell night light\n'
            +'Exec=redshift -c '+path+'\nHidden='+('false' if enabled else 'true')+'\n')

def restore_file(path, previous):
    path = Path(path)
    if path.is_symlink():
        raise ValueError('Night-light path changed during restore.')
    if previous is None:
        path.unlink(missing_ok=True)
    else:
        write_private(path, previous)

def apply_night_light(temp, enabled, config_home, runner):
    tool('redshift')
    config_home = Path(config_home)
    config = config_home/'redshift'/'redshift.conf'
    desktop = config_home/'autostart'/'kilix-zell-redshift.desktop'
    foreign = [name for name in ('redshift.desktop','redshift-gtk.desktop')
               if (config_home/'autostart'/name).exists()]
    if foreign:
        raise RuntimeError('Another night-light startup already exists ('+foreign[0]+'). It was left unchanged.')
    if config.exists() and (config.is_symlink() or not config.read_text().startswith(NIGHT_MARKER)):
        raise RuntimeError('An existing redshift configuration was left unchanged.')
    if desktop.is_symlink():
        raise RuntimeError('The night-light startup entry is a symlink. It was left unchanged.')
    previous_config = config.read_text() if config.exists() else None
    previous_desktop = desktop.read_text() if desktop.exists() else None
    text = night_light_text(6500 if not enabled else temp)
    entry = night_light_desktop(config, enabled)
    try:
        write_private(config, text)
        write_private(desktop, entry)
        if enabled:
            runner(['redshift','-O', str(temp)])
        else:
            runner(['redshift','-x'])
        if config.read_text() != text or desktop.read_text() != entry:
            raise RuntimeError('Night-light read-back did not match.')
    except Exception:
        restore_file(desktop, previous_desktop)
        restore_file(config, previous_config)
        raise
    print('Saved. Night light is applied now and again at the next login.')
    return text

def night_light():
    operation = pick('Night light', [('Turn on','on'), ('Turn off','off')])
    home = Path(os.environ.get('XDG_CONFIG_HOME', Path.home()/'.config'))
    temp = 4500
    if operation == 'on':
        temp = int(ask('Color temperature from 1000 to 6500 K', r'[1-9][0-9]{3,4}'))
    confirm(('Use ' if operation == 'on' else 'Turn off ')+'the managed night-light configuration.', 'set')
    return apply_night_light(temp, operation == 'on', home, lambda argv: run(argv, detail=True))

def installed_choices(kind, roots):
    found = []
    for root in roots:
        root = Path(root)
        if not root.is_dir() or root.is_symlink():
            continue
        for path in sorted(root.iterdir()):
            if not path.is_dir() or path.is_symlink():
                continue
            if kind == 'theme' and ((path/'gtk-3.0').is_dir() or (path/'gtk-2.0').is_dir()):
                found.append(path.name)
            elif kind == 'icons' and (path/'index.theme').is_file():
                found.append(path.name)
            elif kind == 'cursor' and (path/'cursors').is_dir():
                found.append(path.name)
    return found

def theme_argv(prop, value):
    if prop not in THEME_PROPS.values() or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9 ._+-]{0,80}', value):
        raise ValueError('Choose an installed theme name. Nothing was changed.')
    return ['xfconf-query','-c','xsettings','-p', prop, '--create','--type','string','--set', value]

def apply_theme(prop, value, runner):
    argv = theme_argv(prop, value)
    try:
        runner(['xfconf-query','-c','xsettings','-l'])
    except FileNotFoundError:
        raise
    except (OSError, RuntimeError) as error:
        raise RuntimeError('This session has no xsettings owner. Nothing was changed. '+str(error)) from None
    try:
        previous = runner(['xfconf-query','-c','xsettings','-p', prop]).strip()
    except (OSError, RuntimeError):
        previous = None
    runner(argv)
    try:
        shown = runner(['xfconf-query','-c','xsettings','-p', prop]).strip()
        matched = shown == value
    except (OSError, RuntimeError, ValueError, TypeError):
        matched = False
    if not matched:
        try:
            if previous is None:
                runner(['xfconf-query','-c','xsettings','-p', prop, '-r'])
            else:
                runner(theme_argv(prop, previous))
        except (OSError, RuntimeError, ValueError):
            raise RuntimeError('Theme read-back did not match and the previous value could not be restored.') from None
        raise RuntimeError('Theme read-back did not match. The previous value was requested again.')
    print('Saved '+prop+' = '+shown+'.')
    return shown

def format_points(text):
    try:
        value = float(text)
    except (TypeError, ValueError):
        raise ValueError('Font size must be from 4 to 110 points. Nothing was changed.') from None
    if not 4 <= value <= 110:
        raise ValueError('Font size must be from 4 to 110 points. Nothing was changed.')
    if value == int(value):
        return str(int(value))
    return f'{value:.2f}'.rstrip('0').rstrip('.')

def parse_font(text):
    match = re.fullmatch(r'font_size ([0-9]+(?:\.[0-9]+)?)', text.strip())
    if not match:
        raise ValueError('Kilix font settings were not readable. Nothing was changed.')
    return format_points(match[1])

def apply_font(value, previous, runner):
    value = format_points(value)
    previous = format_points(previous)
    runner(['kilix','screen-size','set', value])
    try:
        shown = parse_font(runner(['kilix','screen-size','show']))
        matched = shown == value
    except (OSError, RuntimeError, ValueError, TypeError):
        matched = False
    if not matched:
        try:
            runner(['kilix','screen-size','set', previous])
        except (OSError, RuntimeError, ValueError):
            raise RuntimeError('Kilix font read-back did not match and the previous size could not be restored.') from None
        raise RuntimeError('Kilix font read-back did not match. The previous size was requested again.')
    print('Saved. Kilix font size is '+shown+'.')
    return shown

def theme_roots(kind):
    data_home = Path(os.environ.get('XDG_DATA_HOME', Path.home()/'.local/share'))
    if kind == 'theme':
        return [Path('/usr/share/themes'), Path.home()/'.themes', data_home/'themes']
    return [Path('/usr/share/icons'), Path.home()/'.icons', data_home/'icons']

def themes():
    operation = pick('Appearance', [('GTK theme','theme'), ('Icons','icons'), ('Cursor','cursor'), ('Kilix font size','font')])
    if operation == 'font':
        current = parse_font(capture(['kilix','screen-size','show']))
        print('Kilix font size is '+current+'.')
        value = format_points(ask('Font size from 4 to 110 points', r'[0-9]+(?:\.[0-9]{1,2})?'))
        confirm('Set the shared Kilix font size to '+value+'.', 'set')
        return apply_font(value, current, capture)
    names = installed_choices(operation, theme_roots(operation))
    choice = pick('Installed '+operation+' names', [(name, name) for name in names])
    prop = THEME_PROPS[operation]
    confirm('Set '+prop+' to '+choice+' through the session theme owner.', 'set')
    return apply_theme(prop, choice, capture)

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

task('tinkerer','screen-lock','Automatic screen lock')(screen_lock)
task('tinkerer','lock','Lock the screen now')(lock_screen)
task('tinkerer','blanking','Display blanking and power saving')(blanking)
task('tinkerer','night-light','Night light')(night_light)
task('tinkerer','themes','Themes, icons, cursor and Kilix font')(themes)
