"""Read the weather widget's chosen location and private cache, without networking."""
import json
import math
import os
from pathlib import Path
import re
import time

CONDITIONS = {
    0: 'Clear', 1: 'Mostly clear', 2: 'Partly cloudy', 3: 'Overcast',
    45: 'Fog', 48: 'Freezing fog', 51: 'Light drizzle', 53: 'Drizzle',
    55: 'Heavy drizzle', 56: 'Freezing drizzle', 57: 'Freezing drizzle',
    61: 'Light rain', 63: 'Rain', 65: 'Heavy rain', 66: 'Freezing rain',
    67: 'Freezing rain', 71: 'Light snow', 73: 'Snow', 75: 'Heavy snow',
    77: 'Snow grains', 80: 'Rain showers', 81: 'Rain showers', 82: 'Heavy showers',
    85: 'Snow showers', 86: 'Heavy snow', 95: 'Thunderstorm', 96: 'Thunder / hail',
    97: 'Thunderstorm', 99: 'Thunder / hail',
}
ATTRIBUTION = 'Weather: Open-Meteo (CC BY 4.0). Location: GeoNames.'


def settings_path():
    override = os.environ.get('GPU_TERMINAL_SETTINGS_FILE')
    if override:
        return Path(override).expanduser().absolute()
    return Path(os.environ.get('GPU_TERMINAL_HOME') or Path.home()/'.local/gpu_terminal')/'settings.conf'


def settings(path):
    # Match widget precedence: an existing settings file takes precedence over
    # inherited environment values, including when the location is removed.
    keys = {'KILIX_WEATHER_ZIP', 'KILIX_TEMPERATURE_UNIT'}
    if not path.exists():
        return {key: os.environ.get(key, '') for key in keys}
    values = {}
    with path.open(encoding='utf-8', errors='replace') as stream:
        for line in stream.read(65536).splitlines():
            key, separator, value = line.strip().partition('=')
            if separator and key.strip() in keys:
                values[key.strip()] = value.strip()
    return values


def number(value, low, high):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not low <= value <= high:
        raise ValueError('Invalid weather reading')
    return value


def icon(code):
    if code in (0, 1): return 0
    if code == 2: return 1
    if code == 3: return 2
    if code in (45, 48): return 6
    if code in (71, 73, 75, 77, 85, 86): return 4
    if code in (95, 96, 97, 99): return 5
    if code in (51, 53, 55, 56, 57, 61, 63, 65, 66, 67, 80, 81, 82): return 3
    return 7


def clean(value, length):
    return ''.join(c for c in str(value) if c.isprintable())[:length]


def snapshot(now=None):
    now = time.time() if now is None else now
    off = dict(state=0, icon=7, temperature='', place='', condition='')
    try:
        path = settings_path(); values = settings(path)
        selected = values.get('KILIX_WEATHER_ZIP', '')
        if not re.fullmatch(r'[0-9]{5}', selected):
            return off
    except OSError:
        return off
    unavailable = dict(state=2, icon=7, temperature='', place='Weather', condition='Refresh widget')
    try:
        with (path.parent/'kilix/weather.json').open() as stream:
            raw = json.loads(stream.read(8193))
        reading = raw['reading']
        if not isinstance(reading, dict) or reading.get('zip') != selected or raw.get('zip', selected) != selected:
            return unavailable
        code = number(reading['code'], 0, 999)
        if int(code) != code: raise ValueError('Invalid weather code')
        celsius = number(reading['celsius'], -100, 70)
        observed = number(reading['observed'], 1, 1e11)
        fetched = number(reading['fetched'], 1, 1e11)
        if now-observed > 7200 or observed > now+1800 or fetched > now+300 or raw.get('error'):
            return {**unavailable, 'condition': 'Reading stale'}
        unit = values.get('KILIX_TEMPERATURE_UNIT', 'fahrenheit')
        temperature = f'{celsius:.0f} C' if unit == 'celsius' else f'{celsius*9/5+32:.0f} F'
        return dict(state=1, icon=icon(int(code)), temperature=temperature,
                    place=clean(reading.get('place', 'Local weather'), 80),
                    condition=CONDITIONS.get(int(code), 'Unknown conditions'))
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return unavailable


def report(ui):
    reading = snapshot()
    if not reading['state']:
        return ui.page_text('Set your location in the weather widget to show local weather on the wall map.')
    if reading['state'] != 1:
        return ui.page_text('Current weather is unavailable. Refresh the weather widget.\n\n'+ATTRIBUTION)
    ui.page_text(f"{reading['place']}\n{reading['temperature']} / {reading['condition']}\n\n"
                 'Current conditions are weather-model estimates. Refresh and location settings follow the weather widget.\n\n'+ATTRIBUTION)
