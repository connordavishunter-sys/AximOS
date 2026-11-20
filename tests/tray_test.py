"""Checks the tray beside the clock. Quick settings, from the network and speaker
icons: the slider and the speaker set the mixer, the wheel over the icons changes
it, and it's kept for next time, with the network below. The bell's panel keeps
notifications until cleared."""
import re, time
from vm import VM, check, finish

# The clock, 36 wide in DejaVu Sans (four digits 8 wide and a colon 4), ends 12 from the right
# of the taskbar, 32 tall and 8 from the edges. 8 before it is the bell, 24 wide, and 2 before
# that the network and speaker together, 44 wide.
W, H = 1280, 800
BAR = 32
CLOCK = 36
BELL = (W - 8 - CLOCK - 12 - 8 - 24, H - 8 - BAR + 4)
QUICK = (BELL[0] - 2 - 44, BELL[1])
DESKTOP = 0x1E2430

v = VM(vmmouse=True)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper; rm -f /system/etc/gui.conf /system/etc/volume')
    at = v.mark()
    v.type('wm\n')
    check('wm sets the volume it had', v.wait('wm: volume 100', 10, at), v.text(at))
    time.sleep(1)

    def click(x, y):
        v.pointer(x, y, 1); v.pointer(x, y, 0)

    def rect(name, since):
        return tuple(map(int, re.findall(rf'wm: {name} at (-?\d+) (-?\d+) (\d+) (\d+)', v.text(since))[-1]))

    middle = (QUICK[0] + 22, QUICK[1] + 10)
    w, h, px = v.screenshot()
    check('icons in the tray', px(QUICK[0] + 12, middle[1]) != px(QUICK[0] - 20, middle[1]), hex(px(*middle)))

    at = v.mark()
    click(*middle)
    check('clicking them opens quick settings', v.wait('wm: quick settings', 5, at), v.text(at))
    panel, slider, mute = rect('panel 0', at), rect('slider', at), rect('mute', at)
    check('8 above the taskbar, its right edge under theirs', panel[0] + panel[2] == QUICK[0] + 44
          and panel[1] + panel[3] == H - 8 - BAR - 8, panel)
    time.sleep(0.5)
    w, h, px = v.screenshot()
    inside = (panel[0] + 100, panel[1] + panel[3] - 30)
    check('showing the network', px(*inside) != DESKTOP, hex(px(*inside)))

    at = v.mark()
    click(slider[0] + slider[2] // 2, slider[1] + 10)
    check('the slider sets the volume', v.wait('wm: volume 50\n', 5, at), v.text(at))
    at = v.mark()
    click(mute[0] + 10, mute[1] + 10)
    check('the speaker mutes it', v.wait('wm: volume 50, muted', 5, at), v.text(at))
    at = v.mark()
    v.pointer(*middle)
    v.command('mouse_move 0 0 1') # the wheel up, once
    check('the wheel over the icons turns it up', v.wait('wm: volume 55, muted', 5, at), v.text(at))

    v.key('esc'); time.sleep(0.5)
    w, h, px = v.screenshot()
    check('Escape closes it', px(*inside) == DESKTOP, hex(px(*inside)))
    v.key('meta_l-shift-q'); time.sleep(1)
    o = v.run('volume; cat /system/etc/volume')
    check('the mixer has it', 'volume 55, muted' in o, o)
    check('and it is saved', '\n55 muted\n' in o, o)
    v.run('volume 100 unmute')
    at = v.mark()
    v.type('wm\n')
    check('and set again next time', v.wait('wm: volume 55, muted', 10, at), v.text(at))
    v.key('meta_l-shift-q'); time.sleep(1)

    at = v.mark()
    v.type("wm 'sleep 1; notify Hello there'\n")
    check('the network is found', v.wait('wm: network connected', 10, at), v.text(at))
    check('a notification comes', v.wait('wm: notice Hello: there', 10, at), v.text(at))
    time.sleep(0.5)
    v.pointer(640, 400); time.sleep(0.3)
    w, h, px = v.screenshot()
    check('the bell has a dot for it', px(BELL[0] + 24 - 5, BELL[1] + 4) == 0xE5484D, hex(px(BELL[0] + 24 - 5, BELL[1] + 4)))
    at = v.mark()
    click(*middle); v.wait('wm: quick settings', 5, at)
    panel = rect('panel 0', at)
    time.sleep(0.3)
    at = v.mark()
    click(BELL[0] + 12, BELL[1] + 10)
    check('the bell opens its panel, closing the other', v.wait('wm: panel 1 at', 5, at), v.text(at))
    time.sleep(0.5)
    v.pointer(640, 400); time.sleep(0.3)
    w, h, px = v.screenshot()
    check('without the dot now', px(BELL[0] + 24 - 5, BELL[1] + 4) != 0xE5484D, hex(px(BELL[0] + 24 - 5, BELL[1] + 4)))
    bell = rect('panel 1', at)
    check('the notification is listed', px(bell[0] + 100, bell[1] + 50) != DESKTOP, hex(px(bell[0] + 100, bell[1] + 50)))
    at = v.mark()
    click(bell[0] + bell[2] - 8 - 28, bell[1] + 6 + 11)
    check('Clear clears them', v.wait('wm: notifications cleared', 5, at), v.text(at))
    v.key('esc'); time.sleep(0.5)
    w, h, px = v.screenshot()
    check('Escape closes the panel', px(bell[0] + 100, bell[1] + 50) == DESKTOP, hex(px(bell[0] + 100, bell[1] + 50)))
    v.key('meta_l-shift-q')
finally:
    v.close()
finish()
