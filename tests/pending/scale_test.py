"""Checks display scaling: a 2560x1600 screen starts at 2x, each of the desktop's
pixels four on the screen, with the mouse landing where things are drawn, and
Settings changes it to 1x at once, keeping that."""
import re, time
from vm import VM, check, finish

W, H = 2560, 1600   # the screen's pixels
BAR, GAP = 32, 8    # the taskbar, in the desktop's
QUICK = (1280 - 8 - 36 - 12 - 8 - 24 - 2 - 44, 800 - 8 - BAR + 4)  # as tray_test finds it at 1280x800
SETTINGS_X, SETTINGS_Y = 112, 116  # the second window's contents, after widgets'
DESKTOP = 0x1E2430

v = VM(vmmouse=True, extra=('-vga', 'none', '-device', 'VGA,vgamem_mb=64'))
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper; echo clock 24 > /system/etc/gui.conf; '
          'echo resolution 2560x1600 >> /system/etc/gui.conf')
    at = v.mark()
    v.type("wm widgets 'sleep 1; settings'\n")
    check('2x by default on a 2560 wide screen', v.wait('wm: running at 2560x1600 at 2x', 10, at), v.text(at))
    check('which settings hears of', v.wait('settings: screen 2560x1600 at 2x', 10, at), v.text(at))
    time.sleep(1)

    def click(x, y):
        v.pointer(2 * x, 2 * y, 1, W, H); v.pointer(2 * x, 2 * y, 0, W, H)

    def rect(name, since):
        return tuple(map(int, re.findall(rf'{name} (-?\d+) (-?\d+) (\d+) (\d+)', v.text(since))[-1]))

    w, h, px = v.screenshot()
    check('the screen is 2560x1600', (w, h) == (W, H), (w, h))
    bar_y = 2 * (800 - GAP - BAR)
    blocks = all(px(x, y) == px(x + 1, y) == px(x, y + 1) == px(x + 1, y + 1)
                 for y in range(bar_y, bar_y + 2 * BAR, 2) for x in range(2 * GAP, W - 2 * GAP, 6))
    check("the taskbar's pixels are each drawn as four", blocks)
    check('and it is twice as tall, with twice the gap under it',
          px(1280, H - 2 * GAP - 2 * BAR + 2) != DESKTOP and px(1280, H - 2 * GAP + 2) == DESKTOP,
          (hex(px(1280, H - 2 * GAP - 2 * BAR + 2)), hex(px(1280, H - 2 * GAP + 2))))

    at = v.mark()
    click(QUICK[0] + 22, QUICK[1] + 10)
    check('clicking the tray where it is drawn opens quick settings', v.wait('wm: quick settings', 5, at), v.text(at))
    panel = rect('wm: panel 0 at', at)
    check('placed as at 1280x800', panel[0] + panel[2] == QUICK[0] + 44 and panel[1] + panel[3] == 800 - GAP - BAR - GAP,
          panel)
    time.sleep(0.5)
    w, h, px = v.screenshot()
    inside = (2 * (panel[0] + 100), 2 * (panel[1] + panel[3] - 30))
    check('and drawn at twice its size', px(*inside) != DESKTOP and px(2 * panel[0] - 4, inside[1]) == DESKTOP,
          (hex(px(*inside)), hex(px(2 * panel[0] - 4, inside[1]))))
    v.key('esc'); time.sleep(0.5)

    at = v.mark()
    v.key('ctrl-tab')
    check('Settings has the scale on the Display tab', v.wait('settings: at scale', 5, at), v.text(at))
    time.sleep(0.5)
    scale = rect('settings: at scale', at)
    at = v.mark()
    click(SETTINGS_X + scale[0] + 40, SETTINGS_Y + scale[1] + scale[3] // 2); time.sleep(0.3)
    v.key('up'); v.key('ret')
    check('choosing 1x', v.wait('settings: scale 1x', 5, at), v.text(at))
    check('changes it at once', v.wait('wm: running at 2560x1600\n', 5, at)
          and v.wait('settings: screen 2560x1600\n', 5, at), v.text(at))
    time.sleep(1)
    w, h, px = v.screenshot()
    check('with the taskbar its own size', px(1280, H - GAP - BAR + 2) != DESKTOP
          and px(1280, H - GAP - 2 * BAR) == DESKTOP, (hex(px(1280, H - GAP - BAR + 2)), hex(px(1280, H - GAP - 2 * BAR))))

    v.key('meta_l-shift-q'); time.sleep(1)
    o = v.run('cat /system/etc/gui.conf')
    check('and keeps it', 'scale 1' in o, o)
finally:
    v.close()
finish()
