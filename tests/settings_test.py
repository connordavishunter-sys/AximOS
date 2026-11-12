"""Checks the settings app: the taskbar's clock, window shadows, the light theme,
which every program follows at once, and the time zone, all kept for next time."""
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
    v.run('echo none > /system/etc/wallpaper')
    at = v.mark()
    v.type("wm widgets 'sleep 1; settings'\n") # in that order, so settings is second and on top
    check('settings starts', v.wait('settings: at zones', 10, at), v.text(at))
    time.sleep(1)
    mouse = Pointer(v)
    where = lambda name: [tuple(map(int, m)) for m in
                          re.findall(rf'settings: at {name} (\d+) (\d+) (\d+) (\d+)', v.text())][-1]
    middle = lambda name, dx=40: (SETTINGS_X + where(name)[0] + dx, SETTINGS_Y + where(name)[1] + where(name)[3] // 2)

    # A 24 hour clock, 01:23, ends 12 pixels from the right, starting 52 from it,
    # and a 12 hour one, 1:23 AM, is wider.
    mouse.to(1270, 700)
    w, h, px = v.screenshot()
    before = count(px, 1200, 778, 1226, 792, 0xFFFFFF)
    at = v.mark()
    mouse.click(*middle('clock')); v.key('down'); v.key('ret')
    check('choosing a 12 hour clock', v.wait('settings: clock 12', 5, at), v.text(at))
    mouse.to(1270, 700); time.sleep(0.5)
    w, h, px = v.screenshot()
    after = count(px, 1200, 778, 1226, 792, 0xFFFFFF)
    check('changes the clock on the taskbar', before == 0 and after > 10, (before, after))

    # Shadows: the desktop just under the settings window.
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

    # The time zone, found by searching.
    at = v.mark()
    mouse.click(*middle('search', 60))
    v.type('tokyo'); time.sleep(0.5)
    mouse.double_click(SETTINGS_X + where('zones')[0] + 40, SETTINGS_Y + where('zones')[1] + 2 + 11)
    check('setting the time zone', v.wait('settings: zone Asia/Tokyo', 5, at), v.text(at))

    v.key('meta_l-shift-q'); time.sleep(1)
    o = v.run('cat /system/etc/gui.conf /system/etc/timezone; date +%Z')
    check('the settings are saved', 'theme light' in o and 'shadows off' in o and 'clock 12' in o, o)
    check('and the time zone', 'Asia/Tokyo' in o and 'JST' in o, o)

    at = v.mark()
    v.type('wm\n'); v.wait('wm: running', 10, at); time.sleep(1)
    w, h, px = v.screenshot()
    check('and used next time', px(700, 790) == LIGHT_TASKBAR, hex(px(700, 790)))
    v.key('meta_l-shift-q')
finally:
    v.close()
finish()
