"""The snapshot tool writes a PNG beside every PPM, with the same pixels."""
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]


def read_ppm(path):
    # Exactly one whitespace byte ends the header; pixel bytes may be whitespace.
    data = path.read_bytes()
    header = re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\s', data)
    assert header, path
    return int(header[1]), int(header[2]), data[header.end():]


def read_png(path):
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    offset, chunks = 8, []
    while offset < len(data):
        size, kind = struct.unpack('>I4s', data[offset:offset+8])
        body = data[offset+8:offset+8+size]
        crc, = struct.unpack('>I', data[offset+8+size:offset+12+size])
        assert crc == zlib.crc32(kind + body), kind
        chunks.append((kind, body))
        offset += 12 + size
    assert [kind for kind, _ in chunks] == [b'IHDR', b'IDAT', b'IEND']
    width, height, depth, colour, packing, filtering, interlace = struct.unpack('>IIBBBBB', chunks[0][1])
    assert (depth, colour, packing, filtering, interlace) == (8, 2, 0, 0, 0)
    raw = zlib.decompress(chunks[1][1])
    stride = width*3 + 1
    assert len(raw) == stride*height and all(raw[row*stride] == 0 for row in range(height))
    return width, height, b''.join(raw[row*stride+1:(row+1)*stride] for row in range(height))


class SnapTests(unittest.TestCase):
    def test_every_snapshot_has_a_matching_png(self):
        tool = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT/'build/snap'
        with tempfile.TemporaryDirectory() as directory:
            listed = subprocess.run([str(tool), directory + '/shot', '320', '240'], check=True,
                                    capture_output=True, text=True, timeout=120).stdout.split()
            ppms = sorted(Path(directory).glob('*.ppm'))
            self.assertGreater(len(ppms), 3)
            self.assertEqual(sorted(listed), sorted(str(p) for p in Path(directory).iterdir()))
            for ppm in ppms:
                width, height, pixels = read_ppm(ppm)
                self.assertEqual((width, height, len(pixels)), (320, 240, 320*240*3))
                png = read_png(ppm.with_suffix('.png'))
                self.assertEqual(png[:2], (width, height))
                # Not assertEqual: a byte diff of two images takes minutes.
                self.assertTrue(png[2] == pixels, ppm.name + ': PNG pixels differ from the PPM')


if __name__ == '__main__':
    unittest.main(argv=sys.argv[:1])
