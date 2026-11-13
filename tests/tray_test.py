"""Checks the sound menu from the speaker beside the clock: its slider and mute set
the mixer, the wheel over the speaker changes it, and it's kept for next time."""
import time
from vm import VM, check, finish

# The clock, 5 letters 8 wide, ends 12 from the right, with the speaker 24 wide 8
# before it. The menu, 220 by 82, sits on the taskbar with its right edge under
# the speaker's, and has a slider 16 in from each side, 30 down and 20 tall, and
# Mute 56 down.
W, H = 1280, 800
TRAY = (W - 40 - 12 - 8 - 24, H - 28 + 4, 24, 20)
PANEL = (TRAY[0] + 24 - 220, H - 28 - 82, 220, 82)
SLIDER = (PANEL[0] + 16, PANEL[1] + 30, 220 - 32, 20)
MUTE = (PANEL[0] + 12 + 30, PANEL[1] + 56 + 10)
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

    w, h, px = v.screenshot()
    middle = (TRAY[0] + 12, TRAY[1] + 10)
    check('a speaker in the tray', px(*middle) != px(TRAY[0] - 20, middle[1]), hex(px(*middle)))
    inside = (PANEL[0] + 100, PANEL[1] + 20)
    check('with no menu yet', px(*inside) == DESKTOP, hex(px(*inside)))

    click(*middle)
    time.sleep(0.5)
    w, h, px = v.screenshot()
    check('clicking it opens the sound menu', px(*inside) != DESKTOP, hex(px(*inside)))

    at = v.mark()
    click(SLIDER[0] + SLIDER[2] // 2, SLIDER[1] + 10)
    check('the slider sets the volume', v.wait('wm: volume 50\n', 5, at), v.text(at))
    at = v.mark()
    click(*MUTE)
    check('Mute mutes it', v.wait('wm: volume 50, muted', 5, at), v.text(at))
    at = v.mark()
    v.pointer(*middle)
    v.command('mouse_move 0 0 1') # the wheel up, once
    check('the wheel over the speaker turns it up', v.wait('wm: volume 55, muted', 5, at), v.text(at))

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
    v.key('meta_l-shift-q')
finally:
    v.close()
finish()
