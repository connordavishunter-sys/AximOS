"""Makes the desktop's wallpaper and icons, into rootfs/system/share. They're kept in
the repository, so this only needs running again to change them."""
import math, os, random
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHARE = ROOT + '/rootfs/system/share'
SIZE = 16   # icons, as shown
SCALE = 4   # drawn this many times bigger, then shrunk, for smooth edges


def wallpaper(w=1920, h=1200):
    """Deep blue to violet, with soft glows and faint waves across."""
    random.seed(7)
    image = Image.new('RGB', (w, h))
    top, bottom = (18, 24, 44), (40, 26, 62)
    image.putdata([tuple(int(top[c] + (bottom[c] - top[c]) * (0.6 * y / h + 0.4 * x / w)) for c in range(3))
                   for y in range(h) for x in range(w)])

    glow = Image.new('RGB', (w, h))
    draw = ImageDraw.Draw(glow)
    for colour, (cx, cy, r) in (((70, 90, 200), (0.25, 0.3, 0.32)), ((150, 60, 170), (0.75, 0.65, 0.35)),
                                ((40, 150, 160), (0.6, 0.15, 0.2)), ((90, 70, 200), (0.15, 0.85, 0.25))):
        draw.ellipse(((cx - r) * w, (cy - r) * w + (cy * h - cy * w), (cx + r) * w, (cy + r) * w + (cy * h - cy * w)),
                     fill=colour)
    glow = glow.filter(ImageFilter.GaussianBlur(w // 10))
    image = Image.blend(image, Image.composite(glow, image, glow.convert('L').point(lambda v: min(255, v * 2))), 0.45)

    waves = Image.new('L', (w, h))
    draw = ImageDraw.Draw(waves)
    for i in range(14):
        phase, height, y0 = random.uniform(0, 6.3), random.uniform(30, 90), h * (0.35 + i * 0.03)
        points = [(x, y0 + height * math.sin(x / w * 5 + phase) + i * 6 * math.sin(x / w * 2)) for x in range(0, w + 8, 8)]
        draw.line(points, fill=50 + i * 4, width=2)
    waves = waves.filter(ImageFilter.GaussianBlur(1.2))
    image = Image.composite(Image.new('RGB', (w, h), (190, 200, 255)), image, waves.point(lambda v: v // 3))
    return image


def icon(paint):
    """An icon drawn by paint at SCALE times the size, on a clear background."""
    big = Image.new('RGBA', (SIZE * SCALE, SIZE * SCALE), (0, 0, 0, 0))
    paint(ImageDraw.Draw(big), SCALE)
    return big.resize((SIZE, SIZE), Image.LANCZOS)


def folder(d, s, colour=(240, 190, 60), dark=(200, 145, 30)):
    d.rounded_rectangle((1 * s, 3 * s, 7 * s, 6 * s), 1 * s, fill=dark)
    d.rounded_rectangle((1 * s, 4.5 * s, 15 * s, 14 * s), 1.2 * s, fill=colour)


def page(d, s, lines=False):
    d.polygon([(3 * s, 1 * s), (10 * s, 1 * s), (13 * s, 4 * s), (13 * s, 15 * s), (3 * s, 15 * s)], fill=(245, 246, 250),
              outline=(150, 156, 170), width=s)
    d.polygon([(10 * s, 1 * s), (10 * s, 4 * s), (13 * s, 4 * s)], fill=(200, 204, 215))
    for y in (6, 8.5, 11) if lines else ():
        d.line((5 * s, y * s, 11 * s, y * s), fill=(110, 120, 140), width=s)


def picture_icon(d, s):
    d.rounded_rectangle((1 * s, 2.5 * s, 15 * s, 13.5 * s), 1.5 * s, fill=(70, 130, 220))
    d.polygon([(2 * s, 12.5 * s), (6.5 * s, 6.5 * s), (10 * s, 10.5 * s), (11.5 * s, 9 * s), (14 * s, 12.5 * s)],
              fill=(60, 170, 90))
    d.ellipse((10 * s, 4 * s, 13 * s, 7 * s), fill=(255, 220, 90))


def program(d, s):
    d.rounded_rectangle((1 * s, 2 * s, 15 * s, 14 * s), 1.5 * s, fill=(225, 230, 240))
    d.rounded_rectangle((1 * s, 2 * s, 15 * s, 5 * s), 1.5 * s, fill=(61, 99, 221))
    d.rectangle((1 * s, 4 * s, 15 * s, 5 * s), fill=(61, 99, 221))


def terminal(d, s):
    d.rounded_rectangle((1 * s, 2 * s, 15 * s, 14 * s), 1.5 * s, fill=(22, 26, 34), outline=(90, 100, 120), width=s)
    d.line([(3.5 * s, 6 * s), (6 * s, 8 * s), (3.5 * s, 10 * s)], fill=(110, 220, 110), width=int(1.3 * s))
    d.line((7 * s, 11 * s, 11 * s, 11 * s), fill=(220, 225, 235), width=int(1.3 * s))


def editor(d, s):
    page(d, s, lines=True)
    d.line((7 * s, 13 * s, 14 * s, 6 * s), fill=(240, 150, 40), width=int(2.2 * s))
    d.polygon([(6 * s, 14.5 * s), (6.4 * s, 12.2 * s), (7.8 * s, 13.6 * s)], fill=(60, 60, 70))


def files(d, s):
    folder(d, s, (90, 150, 240), (60, 110, 200))
    d.rectangle((4 * s, 7.5 * s, 12 * s, 8.5 * s), fill=(230, 240, 255))


def widgets(d, s):
    d.rounded_rectangle((1 * s, 2 * s, 15 * s, 14 * s), 1.5 * s, fill=(43, 48, 59))
    for y, at in ((5, 10), (8, 5), (11, 8)):
        d.line((3 * s, y * s, 13 * s, y * s), fill=(110, 118, 135), width=s)
        d.ellipse(((at - 1.3) * s, (y - 1.3) * s, (at + 1.3) * s, (y + 1.3) * s), fill=(85, 120, 232))


def demo(d, s):
    d.ellipse((1.5 * s, 1.5 * s, 14.5 * s, 14.5 * s), fill=(255, 200, 60))
    d.ellipse((5 * s, 5 * s, 7 * s, 7.5 * s), fill=(60, 50, 40))
    d.ellipse((9 * s, 5 * s, 11 * s, 7.5 * s), fill=(60, 50, 40))
    d.arc((4.5 * s, 6 * s, 11.5 * s, 12 * s), 20, 160, fill=(60, 50, 40), width=int(1.2 * s))


def squares(d, s):
    for i, colour in enumerate(((230, 70, 80), (70, 190, 110), (70, 120, 230), (240, 190, 60))):
        x, y = 1.5 + (i % 2) * 7, 1.5 + (i // 2) * 7
        d.rounded_rectangle((x * s, y * s, (x + 6) * s, (y + 6) * s), 1.2 * s, fill=colour)


os.makedirs(SHARE + '/wallpapers', exist_ok=True)
os.makedirs(SHARE + '/icons/apps', exist_ok=True)
wallpaper().save(SHARE + '/wallpapers/default.jpg', quality=90)
for name, paint in (('folder', folder), ('file', page), ('text', lambda d, s: page(d, s, lines=True)),
                    ('image', picture_icon), ('program', program)):
    icon(paint).save(f'{SHARE}/icons/{name}.png', optimize=True)
for name, paint in (('term', terminal), ('editor', editor), ('files', files), ('widgets', widgets),
                    ('guidemo', demo), ('fbdemo', squares), ('viewer', picture_icon)):
    icon(paint).save(f'{SHARE}/icons/apps/{name}.png', optimize=True)
