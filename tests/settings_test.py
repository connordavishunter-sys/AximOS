"""Checks the settings app: window shadows, the light theme, which every program
follows at once, the wallpaper, the taskbar's clock, the time zone and the
resolution, all kept for next time."""
import re, time
from vm import VM, check, finish

DESKTOP, DARK_BACKGROUND, LIGHT_BACKGROUND, LIGHT_TASKBAR = 0x1E2430, 0x1E2129, 0xF3F4F7, 0xEDEFF3
SETTINGS_X, SETTINGS_Y = 112, 116 # the second window's contents, after widgets'


class Pointer:
    def __init__(self, v):
        self.v, self.x, self.y = v, 640, 400

    def to(self, x, y, buttons=None):
        self.v.mouse(x - self.x, y - self.y, buttons)
        self.x, self.y = x, y

    def home(self):
        """To the top left corner, wherever it was, as a resolution change moves it."""
        self.v.mouse(-3000, -3000)
        self.x, self.y = 0, 0

    def click(self, x, y):
        self.to(x, y, 1)
        self.v.mouse(buttons=0)

    def double_click(self, x, y):
        self.to(x, y)
        for b in (1, 0, 1, 0):
            self.v.command(f'mouse_button {b}')
        time.sleep(0.3)


def count(px, x0, y0, x1, y1, colour):
    return sum(px(x, y) == colour for x in range(x0, x1) for y in range(y0, y1))


v = VM()
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper; rm -f /system/etc/gui.conf')
    at = v.mark()
    v.type("wm widgets 'sleep 1; settings'\n") # in that order, so settings is second and on top
    check('settings starts', v.wait('settings: at shadows', 10, at), v.text(at))
    time.sleep(1)
    mouse = Pointer(v)
    where = lambda name: [tuple(map(int, m)) for m in
                          re.findall(rf'settings: at {name} (\d+) (\d+) (\d+) (\d+)', v.text())][-1]
    middle = lambda name, dx=40: (SETTINGS_X + where(name)[0] + dx, SETTINGS_Y + where(name)[1] + where(name)[3] // 2)

    def tab(name):
        at = v.mark()
        v.key('ctrl-tab')
        check(f'Ctrl+Tab goes to {name}', v.wait(f'settings: tab {name}', 5, at), v.text(at))
        time.sleep(0.5)

    # Shadows: the desktop just under the settings window.
    w, h, px = v.screenshot()
    under = (300, SETTINGS_Y + 540 + 4)
    check('a shadow to start with', px(*under) != DESKTOP, hex(px(*under)))
    at = v.mark()
    mouse.click(*middle('shadows', 6))
    check('turning shadows off', v.wait('settings: shadows off', 5, at), v.text(at))
    mouse.to(1270, 700); time.sleep(0.5)
    w, h, px = v.screenshot()
    check('takes them away', px(*under) == DESKTOP, hex(px(*under)))

    # The light theme, which widgets, open beside it, takes up at once.
    check('dark to start with', px(85, 300) == DARK_BACKGROUND, hex(px(85, 300)))
    at = v.mark()
    mouse.click(*middle('theme')); v.key('down'); v.key('ret')
    check('choosing the light theme', v.wait('settings: theme light', 5, at), v.text(at))
    mouse.to(1270, 700); time.sleep(1)
    w, h, px = v.screenshot()
    check('settings turns light', px(SETTINGS_X + 5, SETTINGS_Y + 300) == LIGHT_BACKGROUND,
          hex(px(SETTINGS_X + 5, SETTINGS_Y + 300)))
    check('and so does the taskbar', px(700, 790) == LIGHT_TASKBAR, hex(px(700, 790)))
    check('and the other program', px(85, 300) == LIGHT_BACKGROUND, hex(px(85, 300)))

    # Wallpapers, with a thumbnail of each, the default first.
    check('six wallpapers', all(f'settings: at wallpaper {n} ' in v.text()
                                for n in ('Default', 'Aurora', 'Dunes', 'Hills', 'Ocean', 'Sunset')), v.text())
    x, y, tw, th = where('wallpaper Ocean')
    w, h, px = v.screenshot()
    thumb = px(SETTINGS_X + x + tw // 2, SETTINGS_Y + y + 60)
    check('with thumbnails', thumb not in (LIGHT_BACKGROUND, DARK_BACKGROUND), hex(thumb))
    at = v.mark()
    mouse.click(SETTINGS_X + x + tw // 2, SETTINGS_Y + y + 40)
    check('choosing one', v.wait('settings: wallpaper /system/share/wallpapers/ocean.jpg', 5, at), v.text(at))
    mouse.to(1270, 700); time.sleep(1.5)
    w, h, px = v.screenshot()
    check('puts it on the desktop', px(900, 100) != DESKTOP and px(900, 600) != DESKTOP, f'{px(900, 100):x} {px(900, 600):x}')

    # A 24 hour clock, 01:23, ends 12 pixels from the right, starting 52 from it,
    # and a 12 hour one, 1:23 AM, is wider. It's dark text, in the light theme.
    tab('Display'); tab('Date & time')
    mouse.to(1270, 700)
    w, h, px = v.screenshot()
    before = count(px, 1200, 778, 1226, 792, 0x1E2129)
    at = v.mark()
    mouse.click(*middle('clock')); v.key('down'); v.key('ret')
    check('choosing a 12 hour clock', v.wait('settings: clock 12', 5, at), v.text(at))
    mouse.to(1270, 700); time.sleep(0.5)
    w, h, px = v.screenshot()
    after = count(px, 1200, 778, 1226, 792, 0x1E2129)
    check('changes the clock on the taskbar', before == 0 and after > 10, (before, after))

    # The time zone, found by searching.
    at = v.mark()
    mouse.click(*middle('search', 60))
    v.type('tokyo'); time.sleep(0.5)
    mouse.double_click(SETTINGS_X + where('zones')[0] + 40, SETTINGS_Y + where('zones')[1] + 2 + 11)
    check('setting the time zone', v.wait('settings: zone Asia/Tokyo', 5, at), v.text(at))

    # The resolution: picked from the list, 1280x800 fifth, then applied. Windows
    # stay where they were, as they fit.
    tab('Appearance'); tab('Display')
    at = v.mark()
    mouse.click(*middle('resolution')); time.sleep(0.3)
    v.key('up'); v.key('up'); v.key('up'); v.key('ret'); time.sleep(0.5)
    check('picking one changes nothing yet', 'settings: 1024x768' not in v.text(at), v.text(at))
    mouse.click(*middle('apply', 10))
    check('Apply changes the resolution', v.wait('wm: running at 1024x768', 5, at), v.text(at))
    check('and settings hears of it', v.wait('settings: screen 1024x768', 5, at), v.text(at))
    time.sleep(1)
    w, h, px = v.screenshot()
    check('the screen is that size', (w, h) == (1024, 768), (w, h))
    check('with the taskbar at the bottom', px(700, 758) == LIGHT_TASKBAR, hex(px(700, 758)))
    check('and settings where it was', px(SETTINGS_X + 5, SETTINGS_Y + 300) == LIGHT_BACKGROUND,
          hex(px(SETTINGS_X + 5, SETTINGS_Y + 300)))
    check('it asks whether to keep it', v.wait('settings: keep? 15', 5, at), v.text(at))
    time.sleep(0.5)
    at = v.mark()
    mouse.home()
    mouse.click(*middle('keep', 10))
    check('Keep keeps it', v.wait('settings: kept 1024x768', 5, at), v.text(at))

    # Another, which it goes back from when nothing's said for 15 seconds.
    time.sleep(0.5)
    at = v.mark()
    mouse.click(*middle('resolution')); time.sleep(0.3)
    v.key('up'); v.key('ret'); time.sleep(0.5)
    mouse.click(*middle('apply', 10))
    check('a smaller one', v.wait('wm: running at 800x600', 5, at) and v.wait('settings: keep? 15', 5, at), v.text(at))
    check('goes back by itself', v.wait('settings: going back to 1024x768', 20, at)
          and v.wait('wm: running at 1024x768', 5, at), v.text(at))

    v.key('meta_l-shift-q'); time.sleep(1)
    o = v.run('cat /system/etc/gui.conf /system/etc/timezone /system/etc/wallpaper; date +%Z')
    check('the settings are saved', all(s in o for s in ('theme light', 'shadows off', 'clock 12', 'resolution 1024x768')),
          o)
    check('and the time zone', 'Asia/Tokyo' in o and 'JST' in o, o)
    check('and the wallpaper', '/system/share/wallpapers/ocean.jpg' in o, o)

    at = v.mark()
    v.type('wm\n')
    check('and used next time', v.wait('wm: running at 1024x768', 10, at), v.text(at))
    time.sleep(1)
    w, h, px = v.screenshot()
    check('with the light taskbar', px(700, 758) == LIGHT_TASKBAR, hex(px(700, 758)))
    v.key('meta_l-shift-q'); time.sleep(1)

    # Starting at another size than the screen's draws only what fits it.
    v.run('echo resolution 800x600 > /system/etc/gui.conf')
    at = v.mark()
    v.type('wm\n')
    check('starting at a saved size', v.wait('wm: running at 800x600', 10, at), v.text(at))
    time.sleep(1)
    check("doesn't crash", 'Segmentation fault' not in v.text(at), v.text(at))
    v.key('meta_l-shift-q')
finally:
    v.close()
finish()
