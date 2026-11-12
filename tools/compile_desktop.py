"""Compile PNGs to soft-raster's premultiplied ARGB at their world sizes.

PNG stores straight RGBA. Both raster backends require RGB <= alpha, including
zero RGB in transparent pixels. Filter in premultiplied space so the generated
background's hidden colors cannot bleed into antialiased cutout edges.
"""
from pathlib import Path
import struct
import zlib
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'assets/v0.1.0'


def cutout(cell):
    cell = cell.convert('RGBA')
    # Generated transparent sheets contain sub-visible alpha noise outside
    # the silhouettes. Remove that noise, retaining the actual soft edges.
    cell.putalpha(cell.getchannel('A').point(lambda a: 0 if a <= 3 else a))
    box = cell.getchannel('A').point(lambda a: 255 if a > 32 else 0).getbbox()
    return cell.crop(box) if box else cell


def premultiplied_pixels(image, height=None):
    image = image.convert('RGBa')
    if height is not None:
        width = max(1, round(image.width * height / image.height))
        image = image.resize((width, height), Image.Resampling.LANCZOS)
    # Lanczos ringing may put a color just above alpha; clamp to the raster
    # contract rather than letting an edge brighten the background.
    raw = b''.join(struct.pack('<I', (a << 24) | (min(r, a) << 16) |
                              (min(g, a) << 8) | min(b, a))
                   for r, g, b, a in image.getdata())
    return image.size, raw


def load_images():
    images = [Image.open(ASSETS / 'cave.png').convert('RGBA')]
    for name in ('cast.png', 'workstations.png'):
        image = Image.open(ASSETS / name).convert('RGBA')
        # The authored cast gutter is above the mathematical midpoint.
        edges = (0, 440 if name == 'cast.png' else image.height // 2,
                 image.height)
        for y in range(2):
            for x in range(4):
                images.append(cutout(image.crop((x * image.width // 4, edges[y],
                                                 (x + 1) * image.width // 4, edges[y + 1]))))
    image = Image.open(ASSETS / 'walk.png').convert('RGBA')
    for y in range(4):
        for x in range(4):
            images.append(cutout(image.crop((x * image.width // 4, y * image.height // 4,
                                             (x + 1) * image.width // 4,
                                             (y + 1) * image.height // 4))))
    return images


def main():
    compiled = [premultiplied_pixels(im, None if i == 0 else 112 if 5 <= i <= 10 else 84)
                for i, im in enumerate(load_images())]
    with (ROOT / 'src/desktop_assets.c').open('w') as output:
        output.write('/* Generated premultiplied ARGB; do not hand-edit. */\n'
                     '#include "desktop_assets.h"\n#include <stdlib.h>\n#include <zlib.h>\n')
        for i, (_, raw) in enumerate(compiled):
            blob = zlib.compress(raw, 9)
            output.write('static const unsigned char blob%d[]={\n' % i)
            for j in range(0, len(blob), 24):
                output.write(','.join(str(v) for v in blob[j:j + 24]) + ',\n')
            output.write('};\n')
        output.write('static zell_art art[]={' + ','.join('{%d,%d,NULL}' % size
                     for size, _ in compiled) + '};\n')
        output.write('''const zell_art *zell_art_get(int id) {
    if (id < 0 || id >= ZELL_ART_COUNT) return NULL;
    zell_art *a = &art[id];
    if (a->pixels) return a;
''')
        output.write('    const unsigned char *data[]={' + ','.join('blob%d' % i for i in range(len(compiled))) + '};\n')
        output.write('    const unsigned long sizes[]={' + ','.join('sizeof blob%d' % i for i in range(len(compiled))) + '};\n')
        output.write('''    unsigned long size = (unsigned long)a->width * (unsigned long)a->height * 4;
    a->pixels = malloc(size);
    if (!a->pixels) return NULL;
    if (uncompress((unsigned char *)a->pixels, &size, data[id], sizes[id]) != Z_OK) {
        free(a->pixels);
        a->pixels = NULL;
        return NULL;
    }
    return a;
}
''')


if __name__ == '__main__':
    main()
