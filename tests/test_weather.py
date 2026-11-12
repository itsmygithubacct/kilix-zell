import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import zell_weather as weather


class WeatherTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.settings = self.root/'settings.conf'
        self.settings.write_text('KILIX_WEATHER_ZIP=00001\nKILIX_TEMPERATURE_UNIT=celsius\n')
        self.cache = self.root/'kilix/weather.json'; self.cache.parent.mkdir()
        self.now = 1800000000
        self.reading = dict(zip='00001', place='Example town', celsius=8, code=61,
                            observed=self.now-600, fetched=self.now-120)
        self.save()
        self.env = patch.dict(os.environ, GPU_TERMINAL_SETTINGS_FILE=str(self.settings))
        self.env.start(); self.addCleanup(self.env.stop)

    def save(self, **changes):
        self.cache.write_text(json.dumps(dict(reading={**self.reading, **changes})))

    def test_location_is_required_and_removal_clears_old_weather(self):
        self.settings.write_text('')
        with patch.dict(os.environ, KILIX_WEATHER_ZIP='00001'):
            self.assertEqual(weather.snapshot(self.now)['state'], 0)
        self.settings.write_text('KILIX_WEATHER_ZIP=bad\n')
        self.assertEqual(weather.snapshot(self.now)['state'], 0)

    def test_fresh_reading_and_temperature_units(self):
        before = self.cache.read_bytes()
        reading = weather.snapshot(self.now)
        self.assertEqual((reading['state'], reading['icon'], reading['temperature']), (1, 3, '8 C'))
        self.assertEqual(reading['condition'], 'Light rain')
        self.settings.write_text('KILIX_WEATHER_ZIP=00001\nKILIX_TEMPERATURE_UNIT=fahrenheit\n')
        self.assertEqual(weather.snapshot(self.now)['temperature'], '46 F')
        self.assertEqual(before, self.cache.read_bytes())

    def test_other_locations_stale_future_and_bad_numbers_never_claim_current(self):
        for changes in ({'zip':'00002'}, {'observed':self.now-7201}, {'observed':self.now+1801},
                        {'fetched':self.now+301}, {'celsius':float('nan')}, {'code':True}, {'code':61.5}):
            with self.subTest(changes=changes):
                self.save(**changes)
                self.assertEqual(weather.snapshot(self.now)['state'], 2)
        self.cache.write_text('{broken')
        self.assertEqual(weather.snapshot(self.now)['state'], 2)

    def test_condition_art_covers_widget_codes_and_unknown_is_explicit(self):
        cases = {0:0, 2:1, 3:2, 61:3, 73:4, 95:5, 48:6, 999:7}
        for code, expected in cases.items():
            self.assertEqual(weather.icon(code), expected)
        self.save(code=999)
        self.assertEqual(weather.snapshot(self.now)['condition'], 'Unknown conditions')


if __name__ == '__main__':
    unittest.main()
