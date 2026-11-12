"""Checks libgui's PNG, JPEG and BMP decoders on the host, against Pillow, on images
of every kind they read: compiled natively, they decode each file to raw pixels."""
import io, os, struct, subprocess, zlib
from PIL import Image, ImageDraw
from vm import ROOT, WORK, check, finish

LIBGUI = ROOT + '/user/libgui'
HARNESS = WORK + '/decode.c'
PROGRAM = WORK + '/decode'

open(HARNESS, 'w').write(r'''
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    struct gui_image image;
    if (!gui_image_load(argv[1], &image)) {
        printf("error %s\n", gui_image_error());
        return 1;
    }
    printf("%d %d\n", image.width, image.height);
    fwrite(image.pixels, 4, (size_t)image.width * image.height, stdout);
    return 0;
}
''')
subprocess.run(['gcc', '-std=gnu2x', '-O2', '-Wall', '-I', LIBGUI + '/include', '-I', ROOT + '/abi', '-o', PROGRAM,
                HARNESS] + [f'{LIBGUI}/{n}.c' for n in ('image', 'png', 'jpeg', 'bmp')], check=True)


def decode(path):
    """What libgui makes of a file: an RGBA image, or the error it gave."""
    out = subprocess.run([PROGRAM, path], capture_output=True).stdout
    header, _, pixels = out.partition(b'\n')
    if header.startswith(b'error'):
        return header.decode()
    w, h = map(int, header.split())
    # 0xAARRGGBB words, little endian, are B G R A bytes.
    return Image.frombytes('RGBA', (w, h), pixels, 'raw', 'BGRA')


def picture(w=67, h=45, mode='RGB'):
    """Gradients, shapes and edges, to catch mistakes anywhere in an image."""
    image = Image.new('RGB', (w, h))
    image.putdata([(x * 255 // w, y * 255 // h, (x + y) * 255 // (w + h)) for y in range(h) for x in range(w)])
    draw = ImageDraw.Draw(image)
    draw.ellipse((w // 5, h // 5, w * 3 // 4, h * 4 // 5), fill=(240, 200, 30))
    draw.rectangle((w // 2, 2, w - 3, h // 3), fill=(20, 40, 220))
    draw.line((0, h - 1, w - 1, 0), fill=(255, 255, 255), width=2)
    if mode == 'RGBA':
        image.putalpha(Image.linear_gradient('L').resize((w, h)))
    return image.convert(mode) if mode != 'RGBA' else image


def same(name, path, want, tolerance=0):
    """Checks the decode against want, exactly or on average within tolerance."""
    got = decode(path)
    if isinstance(got, str) or got.size != want.size:
        check(name, False, got if isinstance(got, str) else f'size {got.size} not {want.size}')
        return
    a, b = got.tobytes(), want.convert('RGBA').tobytes()
    diffs = [abs(x - y) for x, y in zip(a, b)]
    mean = sum(diffs) / len(diffs)
    check(name, mean <= tolerance if tolerance else a == b, f'mean difference {mean:.2f}, most {max(diffs)}')


def saved(name, image, **options):
    path = f'{WORK}/{name}'
    image.save(path, **options)
    return path


def converted(name, source, *args):
    """A file made by ImageMagick, from a PNG of source."""
    path = f'{WORK}/{name}'
    subprocess.run(['magick', saved(name + '.src.png', source), *args, path], check=True)
    return path


# PNG: each colour type, the small depths, 16 bits, transparency and interlacing.
base = picture()
same('PNG RGB', saved('rgb.png', base), base)
same('PNG RGBA', saved('rgba.png', picture(mode='RGBA')), picture(mode='RGBA'))
same('PNG grey', saved('grey.png', base.convert('L')), base.convert('L'))
same('PNG grey and alpha', saved('la.png', picture(mode='RGBA').convert('LA')), picture(mode='RGBA').convert('LA'))
same('PNG 1 bit', saved('one.png', base.convert('1')), base.convert('1'))
palette = base.quantize(16)
same('PNG 4 bit palette', saved('p4.png', palette, bits=4), palette)
same('PNG 8 bit palette', saved('p8.png', base.quantize(200)), base.quantize(200))
clear = base.quantize(16)
clear.info['transparency'] = 3
same('PNG palette with transparency', saved('ptrns.png', clear, transparency=3), clear.convert('RGBA'))
same('PNG 2 bit grey', converted('g2.png', base.convert('L'), '-depth', '2', '-define', 'png:color-type=0'),
     Image.open(f'{WORK}/g2.png').convert('RGBA'))
same('PNG 16 bit RGB', converted('rgb16.png', base, '-depth', '16'), base)
same('PNG interlaced', converted('interlaced.png', base, '-interlace', 'PNG'), base)
same('PNG interlaced RGBA', converted('interlaced_rgba.png', picture(mode='RGBA'), '-interlace', 'PNG'),
     picture(mode='RGBA'))
same('PNG of one pixel', saved('tiny.png', Image.new('RGB', (1, 1), (9, 99, 199))), Image.new('RGB', (1, 1), (9, 99, 199)))
big = picture(800, 600)
same('PNG, bigger', saved('big.png', big, compress_level=9), big)
same('PNG with no compression', saved('stored.png', base, compress_level=0), base)

# JPEG: the usual subsamplings, grey, progressive, restart markers and odd sizes,
# within a little of what libjpeg makes, as the arithmetic differs.
for sub, label in ((0, '4:4:4'), (1, '4:2:2'), (2, '4:2:0')):
    path = saved(f'sub{sub}.jpg', base, quality=92, subsampling=sub)
    same(f'JPEG {label}', path, Image.open(path), 2.5)
path = saved('grey.jpg', base.convert('L'), quality=90)
same('JPEG grey', path, Image.open(path), 1.5)
path = saved('progressive.jpg', base, quality=90, progressive=True)
same('JPEG progressive', path, Image.open(path), 2.5)
path = saved('progressive_big.jpg', big, quality=80, progressive=True, subsampling=2)
same('JPEG progressive, bigger', path, Image.open(path), 2.5)
path = saved('progressive_grey.jpg', big.convert('L'), quality=85, progressive=True)
same('JPEG progressive grey', path, Image.open(path), 1.5)
path = f'{WORK}/restart.jpg'
subprocess.run(['cjpeg', '-restart', '2B', '-outfile', path, saved('restart.ppm', big)], check=True)
same('JPEG with restart markers', path, Image.open(path), 2.5)
path = f'{WORK}/restart_progressive.jpg'
subprocess.run(['cjpeg', '-progressive', '-restart', '1', '-outfile', path, saved('restart.ppm', big)], check=True)
same('JPEG progressive with restart markers', path, Image.open(path), 2.5)
odd = picture(13, 7)
path = saved('odd.jpg', odd, quality=95, subsampling=2)
same('JPEG of an odd size', path, Image.open(path), 3)

# BMP: 24 and 32 bits, palettes of 1, 4 and 8 bits, 16 bits and top down.
same('BMP 24 bit', saved('rgb.bmp', base), base)
# Without a mask for it, the fourth byte isn't alpha, as Pillow reads it too.
path = saved('rgbx.bmp', picture(mode='RGBA'))
same('BMP 32 bit', path, Image.open(path))
same('BMP 8 bit palette', saved('p8.bmp', base.quantize(200)), base.quantize(200))
same('BMP 1 bit', saved('one.bmp', base.convert('1')), base.convert('1'))
path = converted('p4.bmp', base, '-colors', '16', '-type', 'Palette', '-compress', 'None')
same('BMP 4 bit or 8 bit palette', path, Image.open(path))


def bmp(name, w, h, bpp, rows, masks=None, top_down=False):
    """A BMP written by hand, for the kinds Pillow doesn't write."""
    stride = (w * bpp + 31) // 32 * 4
    data = b''.join(r.ljust(stride, b'\0') for r in (rows if top_down else rows[::-1]))
    compression = 0 if not masks else 3 if len(masks) == 3 else 6
    header = struct.pack('<IiiHHIIiiII', 40, w, -h if top_down else h, 1, bpp, compression, len(data), 2835, 2835, 0, 0)
    extra = struct.pack(f'<{len(masks)}I', *masks) if masks else b''
    offset = 14 + len(header) + len(extra)
    path = f'{WORK}/{name}'
    open(path, 'wb').write(b'BM' + struct.pack('<IHHI', offset + len(data), 0, 0, offset) + header + extra + data)
    return path


pixels = list(base.get_flattened_data())
w, h = base.size
rows565 = [b''.join(struct.pack('<H', (r >> 3) << 11 | (g >> 2) << 5 | b >> 3) for r, g, b in pixels[y * w:(y + 1) * w])
           for y in range(h)]
want565 = Image.new('RGB', (w, h))
want565.putdata([((r >> 3) * 255 // 31, (g >> 2) * 255 // 63, (b >> 3) * 255 // 31) for r, g, b in pixels])
same('BMP 16 bit 5:6:5', bmp('565.bmp', w, h, 16, rows565, (0xF800, 0x07E0, 0x001F)), want565)
rows24 = [b''.join(bytes((b, g, r)) for r, g, b in pixels[y * w:(y + 1) * w]) for y in range(h)]
same('BMP top down', bmp('topdown.bmp', w, h, 24, rows24, top_down=True), base)
rgba = picture(mode='RGBA')
rows32 = [b''.join(bytes((b, g, r, a)) for r, g, b, a in list(rgba.get_flattened_data())[y * w:(y + 1) * w])
          for y in range(h)]
same('BMP 32 bit with an alpha mask', bmp('alpha.bmp', w, h, 32, rows32, (0xFF0000, 0xFF00, 0xFF, 0xFF000000)), rgba)

# Broken files fail rather than crash.
for name, data in (('truncated.png', open(f'{WORK}/rgb.png', 'rb').read()[:300]),
                   ('truncated.jpg', open(f'{WORK}/sub2.jpg', 'rb').read()[:400]),
                   ('truncated.bmp', open(f'{WORK}/rgb.bmp', 'rb').read()[:500]),
                   ('garbage.png', b'\x89PNG\r\n\x1a\n' + bytes(range(256)) * 4),
                   ('not_an_image', b'hello')):
    open(f'{WORK}/{name}', 'wb').write(data)
    got = decode(f'{WORK}/{name}')
    check(f'{name} fails cleanly', isinstance(got, str) or got.size[0] > 0, got)

finish()
