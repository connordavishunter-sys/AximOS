"""Makes the desktop's wallpaper and icons, into rootfs/system/share. They're kept in
the repository, so this only needs running again to change them."""
import math, os, random
import numpy as np
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


def grid(w, h):
    """Each pixel's x and y, from 0 to 1."""
    return np.meshgrid(np.linspace(0, 1, w), np.linspace(0, 1, h))


def blend(a, b, t):
    """Colours a to b by t, an array of 0 to 1, as an image's array."""
    t = np.clip(t, 0, 1)[..., None]
    return np.array(a) * (1 - t) + np.array(b) * t


def image(pixels, blur=0):
    picture = Image.fromarray(np.clip(pixels, 0, 255).astype(np.uint8))
    return picture.filter(ImageFilter.GaussianBlur(blur)) if blur else picture


def ridge(x, base, height, waves, seed):
    """A line of hills across, as a height at each x from 0 to 1."""
    rng = np.random.default_rng(seed)
    y = np.full_like(x, base)
    for i in range(waves):
        y += height / (i + 1) * np.sin(x * (i + 1) * rng.uniform(2, 5) + rng.uniform(0, 6.3))
    return y


def stars(pixels, count, seed, limit=1.0):
    rng = np.random.default_rng(seed)
    h, w = pixels.shape[:2]
    for _ in range(count):
        x, y, v = rng.integers(0, w), rng.integers(0, int(h * limit)), rng.uniform(80, 255)
        pixels[y, x] = np.maximum(pixels[y, x], v)


def aurora(w=1920, h=1200):
    """Green and teal curtains over a night sky and dark hills."""
    x, y = grid(w, h)
    pixels = blend((6, 12, 28), (14, 32, 48), y)
    stars(pixels, 900, 1, 0.8)
    for colour, centre, phase, width in (((40, 220, 140), 0.32, 0.0, 0.10), ((60, 190, 210), 0.42, 2.0, 0.08),
                                         ((120, 90, 220), 0.25, 4.0, 0.06)):
        line = centre + 0.08 * np.sin(x * 5 + phase) + 0.03 * np.sin(x * 13 + phase * 2)
        below = np.clip((y - line) / width, 0, None)
        above = np.clip((line - y) / 0.012, 0, None)
        rays = 0.5 + 0.5 * np.sin(x * 70 + phase + np.sin(x * 23) * 1.5) ** 2
        glow = np.exp(-below * 2.2) * np.exp(-above ** 2) * rays
        pixels += np.array(colour) * glow[..., None] * 0.8
    for base, colour, seed in ((0.78, (10, 20, 30), 3), (0.86, (5, 10, 16), 4)):
        pixels = np.where((y > ridge(x, base, 0.04, 3, seed))[..., None], colour, pixels)
    return image(pixels, 1.2)


def sunset(w=1920, h=1200):
    """A low sun in an orange and pink sky, behind purple mountains."""
    x, y = grid(w, h)
    pixels = blend((60, 40, 110), (250, 130, 90), y / 0.62)
    pixels = blend(pixels, (255, 200, 120), np.clip((y - 0.45) / 0.2, 0, 1) * 0.7)
    d = np.hypot((x - 0.62) * w / h, y - 0.56)
    pixels = blend(pixels, (255, 240, 200), np.exp(-d * 9) * 0.9)
    pixels = np.where((d < 0.07)[..., None], (255, 236, 190), pixels)
    for base, colour, seed in ((0.6, (150, 70, 120), 5), (0.7, (100, 45, 100), 6), (0.82, (55, 25, 70), 7),
                               (0.92, (30, 15, 45), 8)):
        pixels = np.where((y > ridge(x, base, 0.05, 4, seed))[..., None], colour, pixels)
    return image(pixels, 1)


def ocean(w=1920, h=1200):
    """A calm sea under a pale sky, with the low sun's light on the water."""
    x, y = grid(w, h)
    horizon = 0.5
    rng = np.random.default_rng(9)
    sky = blend((110, 160, 215), (235, 225, 215), (y / horizon) ** 1.5)
    sky = blend(sky, (255, 240, 210), np.exp(-np.hypot((x - 0.5) * 1.6, (y - horizon) * 4) * 5) * 0.8)
    depth = np.clip((y - horizon) / (1 - horizon), 0, 1)
    sea = blend((70, 120, 160), (8, 35, 70), depth ** 0.7)

    # Ripples get bigger nearer, and catch the light in broken lines.
    ripples = np.sin(40 / (depth + 0.04) + sum(np.sin(x * f + rng.uniform(0, 6.3)) * a
                                              for f, a in ((7, 1.2), (19, 0.6), (43, 0.3))))
    broken = sum(np.sin(x * f / (depth + 0.1) + rng.uniform(0, 6.3)) for f in (3, 7, 11, 17)) > 1.9
    glitter = np.exp(-((x - 0.5) / (0.02 + depth * 0.12)) ** 2) * (broken + 0.6)
    sea = blend(sea, (255, 245, 225), (ripples > 0.85) * (glitter * 0.6 + broken * 0.1))
    pixels = np.where((y < horizon)[..., None], sky, sea)
    return image(pixels, 0.8)


def hills(w=1920, h=1200):
    """Rolling green hills, fading into the distance, under a soft sky."""
    x, y = grid(w, h)
    pixels = blend((150, 200, 240), (230, 240, 230), y / 0.6)
    for i, (base, colour) in enumerate(((0.5, (150, 190, 160)), (0.6, (110, 170, 110)), (0.72, (70, 140, 70)),
                                        (0.85, (40, 110, 50)))):
        top = ridge(x, base, 0.06, 3, 20 + i)
        shade = np.clip((y - top) * 3, 0, 0.35)
        pixels = np.where((y > top)[..., None], blend(colour, np.array(colour) * 0.6, shade), pixels)
    return image(pixels, 1.5)


def dunes(w=1920, h=1200):
    """Smooth sand dunes in warm light, each lit on one side."""
    x, y = grid(w, h)
    pixels = blend((150, 190, 230), (250, 220, 180), y / 0.45)
    for i, (base, light, dark) in enumerate(((0.45, (235, 190, 130), (200, 140, 90)),
                                             (0.58, (230, 170, 110), (180, 110, 70)),
                                             (0.74, (220, 150, 90), (160, 90, 55)))):
        top = ridge(x, base, 0.07, 2, 30 + i)
        slope = np.gradient(top, axis=1) * w
        lit = blend(dark, light, 0.5 + np.tanh(slope * 0.6) * 0.5)
        pixels = np.where((y > top)[..., None], blend(lit, np.array(dark) * 0.85, np.clip((y - top) * 2, 0, 1)), pixels)
    return image(pixels, 1.5)


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


def gear(d, s):
    c, teeth = 8 * s, 8
    for i in range(teeth):
        angle = i * 2 * math.pi / teeth
        x, y = c + 5.6 * s * math.cos(angle), c + 5.6 * s * math.sin(angle)
        d.ellipse((x - 1.8 * s, y - 1.8 * s, x + 1.8 * s, y + 1.8 * s), fill=(150, 160, 180))
    d.ellipse((c - 5.5 * s, c - 5.5 * s, c + 5.5 * s, c + 5.5 * s), fill=(150, 160, 180))
    d.ellipse((c - 2.3 * s, c - 2.3 * s, c + 2.3 * s, c + 2.3 * s), fill=(0, 0, 0, 0))


def run(d, s):
    program(d, s)
    d.polygon([(6 * s, 7 * s), (11.5 * s, 9.5 * s), (6 * s, 12 * s)], fill=(60, 170, 90))


def note(d, s, x, y, size, colour):
    """A quaver, with its head's middle at x, y."""
    d.ellipse(((x - 1.6 * size) * s, (y - 1.2 * size) * s, (x + 1.6 * size) * s, (y + 1.2 * size) * s), fill=colour)
    d.rectangle(((x + 1.6 * size - 0.9) * s, (y - 7 * size) * s, (x + 1.6 * size) * s, y * s), fill=colour)
    d.polygon([((x + 1.6 * size) * s, (y - 7 * size) * s), ((x + 4 * size) * s, (y - 5 * size) * s),
               ((x + 4 * size) * s, (y - 3.8 * size) * s), ((x + 1.6 * size) * s, (y - 5.5 * size) * s)], fill=colour)


def audio(d, s):
    page(d, s)
    note(d, s, 7, 11.5, 1, (220, 90, 60))


def player(d, s):
    d.ellipse((1 * s, 1 * s, 15 * s, 15 * s), fill=(240, 120, 50))
    note(d, s, 6.8, 10.5, 1, (255, 255, 255))


def speaker(d, s, waves=True):
    grey = (150, 160, 180)
    d.rectangle((2 * s, 6 * s, 5 * s, 10 * s), fill=grey)
    d.polygon([(5 * s, 6 * s), (9 * s, 2.5 * s), (9 * s, 13.5 * s), (5 * s, 10 * s)], fill=grey)
    for r in (3, 5.5) if waves else ():
        d.arc(((9 - r) * s, (8 - r) * s, (9 + r) * s, (8 + r) * s), -50, 50, fill=grey, width=int(1.3 * s))


def muted(d, s):
    speaker(d, s, waves=False)
    for a, b in (((10.5, 5.5), (15, 10.5)), ((15, 5.5), (10.5, 10.5))):
        d.line((a[0] * s, a[1] * s, b[0] * s, b[1] * s), fill=(230, 80, 80), width=int(1.6 * s))


def padlock(d, s):
    d.arc((4.5 * s, 1.5 * s, 11.5 * s, 10 * s), 180, 360, fill=(150, 160, 180), width=int(1.6 * s))
    d.line((4.5 * s + 0.8 * s, 5.5 * s, 4.5 * s + 0.8 * s, 8 * s), fill=(150, 160, 180), width=int(1.6 * s))
    d.line((11.5 * s - 0.8 * s, 5.5 * s, 11.5 * s - 0.8 * s, 8 * s), fill=(150, 160, 180), width=int(1.6 * s))
    d.rounded_rectangle((2.5 * s, 7 * s, 13.5 * s, 15 * s), 1.5 * s, fill=(240, 190, 60))
    d.ellipse((7 * s, 9.5 * s, 9 * s, 11.5 * s), fill=(150, 105, 20))
    d.rectangle((7.6 * s, 11 * s, 8.4 * s, 13 * s), fill=(150, 105, 20))


def restart(d, s):
    green = (70, 190, 110)
    d.arc((2 * s, 2 * s, 14 * s, 14 * s), 300, 240, fill=green, width=int(1.8 * s))
    # The arrowhead at the arc's end, at the top, pointing round to the right.
    d.polygon([(4 * s, 0.8 * s), (8.5 * s, 2.6 * s), (4.5 * s, 6 * s)], fill=green)


def power(d, s):
    red = (230, 80, 80)
    d.arc((2 * s, 2.5 * s, 14 * s, 14.5 * s), 300, 240, fill=red, width=int(1.8 * s))
    d.line((8 * s, 1 * s, 8 * s, 8 * s), fill=red, width=int(1.8 * s))


def log_out(d, s):
    grey, blue = (150, 160, 180), (90, 150, 240)
    d.line([(9 * s, 4 * s), (9 * s, 2 * s), (2 * s, 2 * s), (2 * s, 14 * s), (9 * s, 14 * s), (9 * s, 12 * s)],
           fill=grey, width=int(1.5 * s), joint='curve')
    d.line((6 * s, 8 * s, 12 * s, 8 * s), fill=blue, width=int(1.8 * s))
    d.polygon([(11 * s, 4.5 * s), (15 * s, 8 * s), (11 * s, 11.5 * s)], fill=blue)


def calculator(d, s):
    d.rounded_rectangle((2.5 * s, 1 * s, 13.5 * s, 15 * s), 1.5 * s, fill=(70, 78, 95))
    d.rectangle((4 * s, 2.5 * s, 12 * s, 5.5 * s), fill=(190, 220, 190))
    for row in range(3):
        for col in range(3):
            x, y = 4 + col * 3, 7 + row * 2.7
            colour = (240, 150, 40) if col == 2 else (220, 225, 235)
            d.rectangle((x * s, y * s, (x + 2) * s, (y + 1.8) * s), fill=colour)


def graph(d, s):
    d.rounded_rectangle((1 * s, 2 * s, 15 * s, 14 * s), 1.5 * s, fill=(22, 26, 34), outline=(90, 100, 120), width=s)
    d.line([(3 * s, 11 * s), (5.5 * s, 8 * s), (7.5 * s, 10 * s), (10 * s, 5 * s), (13 * s, 7.5 * s)],
           fill=(110, 220, 110), width=int(1.4 * s), joint='curve')


def squares(d, s):
    for i, colour in enumerate(((230, 70, 80), (70, 190, 110), (70, 120, 230), (240, 190, 60))):
        x, y = 1.5 + (i % 2) * 7, 1.5 + (i // 2) * 7
        d.rounded_rectangle((x * s, y * s, (x + 6) * s, (y + 6) * s), 1.2 * s, fill=colour)


def bars(d, s, colour=(150, 160, 180)):
    """The network, as bars rising to the right."""
    for i, h in enumerate((4, 7, 10, 13)):
        x = 1.5 + i * 3.5
        d.rounded_rectangle((x * s, (14.5 - h) * s, (x + 2.4) * s, 14.5 * s), 0.6 * s, fill=colour)


def offline(d, s):
    bars(d, s, (90, 98, 115))
    for a, b in (((9, 1.5), (15, 7.5)), ((15, 1.5), (9, 7.5))):
        d.line((a[0] * s, a[1] * s, b[0] * s, b[1] * s), fill=(230, 80, 80), width=int(1.6 * s))


def bell(d, s):
    grey = (150, 160, 180)
    d.pieslice((3 * s, 2 * s, 13 * s, 14 * s), 180, 360, fill=grey)
    d.rectangle((3 * s, 8 * s, 13 * s, 11.5 * s), fill=grey)
    d.polygon([(1.5 * s, 12.5 * s), (3 * s, 11 * s), (13 * s, 11 * s), (14.5 * s, 12.5 * s)], fill=grey)
    d.ellipse((6.5 * s, 12.5 * s, 9.5 * s, 15.5 * s), fill=grey)
    d.ellipse((7 * s, 0.5 * s, 9 * s, 2.5 * s), fill=grey)


def palette_icon(d, s):
    d.ellipse((1 * s, 2 * s, 15 * s, 14 * s), fill=(235, 215, 180))
    d.ellipse((9.5 * s, 8.5 * s, 12.5 * s, 11.5 * s), fill=(0, 0, 0, 0))
    for (x, y), colour in zip(((4.5, 6), (7.5, 4.5), (10.8, 5.5), (4.5, 10)),
                              ((230, 70, 80), (240, 190, 60), (70, 120, 230), (70, 190, 110))):
        d.ellipse(((x - 1.3) * s, (y - 1.3) * s, (x + 1.3) * s, (y + 1.3) * s), fill=colour)


def snake(d, s):
    d.rounded_rectangle((1 * s, 1 * s, 15 * s, 15 * s), 2 * s, fill=(22, 26, 34))
    d.line([(3.5 * s, 12 * s), (3.5 * s, 8 * s), (8 * s, 8 * s), (8 * s, 4 * s), (12 * s, 4 * s)], fill=(39, 174, 96),
           width=int(2.4 * s), joint='curve')
    d.rectangle((11 * s, 10.5 * s, 13 * s, 12.5 * s), fill=(229, 72, 77))


os.makedirs(SHARE + '/wallpapers', exist_ok=True)
os.makedirs(SHARE + '/icons/apps', exist_ok=True)
wallpaper().save(SHARE + '/wallpapers/default.jpg', quality=90)
for name, paint in (('aurora', aurora), ('sunset', sunset), ('ocean', ocean), ('hills', hills), ('dunes', dunes)):
    paint().save(f'{SHARE}/wallpapers/{name}.jpg', quality=90)
for name, paint in (('folder', folder), ('file', page), ('text', lambda d, s: page(d, s, lines=True)),
                    ('image', picture_icon), ('program', program), ('audio', audio),
                    ('speaker', speaker), ('muted', muted), ('lock', padlock), ('reboot', restart),
                    ('shutdown', power), ('logout', log_out), ('network', bars), ('offline', offline),
                    ('bell', bell)):
    icon(paint).save(f'{SHARE}/icons/{name}.png', optimize=True)
for name, paint in (('term', terminal), ('editor', editor), ('files', files), ('widgets', widgets),
                    ('guidemo', demo), ('fbdemo', squares), ('viewer', picture_icon), ('settings', gear), ('run', run),
                    ('player', player), ('calculator', calculator), ('tasks', graph), ('paint', palette_icon),
                    ('snake', snake)):
    icon(paint).save(f'{SHARE}/icons/apps/{name}.png', optimize=True)
