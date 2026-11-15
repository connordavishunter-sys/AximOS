"""Checks the tray beside the clock. The sound menu from the speaker: its slider and
mute set the mixer, the wheel over the speaker changes it, and it's kept for next
time. The network's panel, and the bell's, which keeps notifications until cleared."""
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
# Left of the speaker, the bell and the network, 24 wide each. The bell's panel is
# 300 wide, 40 tall and 44 more a notification, with Clear 56 by 22 at its top
# right, 8 in and 6 down. The network's is 260 by 156.
BELL, NETWORK = (TRAY[0] - 24, TRAY[1]), (TRAY[0] - 48, TRAY[1])
NET_PANEL = (NETWORK[0] + 24 - 260, H - 28 - 156)

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
    v.key('meta_l-shift-q'); time.sleep(1)

    at = v.mark()
    v.type("wm 'sleep 1; notify Hello there'\n")
    check('the network is found', v.wait('wm: network connected', 10, at), v.text(at))
    check('a notification comes', v.wait('wm: notice Hello: there', 10, at), v.text(at))
    time.sleep(0.5)
    w, h, px = v.screenshot()
    check('the bell has a dot for it', px(BELL[0] + 24 - 5, BELL[1] + 4) == 0xE5484D, hex(px(BELL[0] + 24 - 5, BELL[1] + 4)))
    net_inside = (NET_PANEL[0] + 130, NET_PANEL[1] + 40)
    click(NETWORK[0] + 12, NETWORK[1] + 10); time.sleep(0.5)
    w, h, px = v.screenshot()
    check('the network icon opens its panel', px(*net_inside) != DESKTOP, hex(px(*net_inside)))
    click(BELL[0] + 12, BELL[1] + 10); time.sleep(0.5)
    w, h, px = v.screenshot()
    check('the bell opens its panel, closing the other', px(*net_inside) == DESKTOP, hex(px(*net_inside)))
    check('without the dot now', px(BELL[0] + 24 - 5, BELL[1] + 4) != 0xE5484D, hex(px(BELL[0] + 24 - 5, BELL[1] + 4)))
    panel_top = H - 28 - 84
    check('the notification is listed', px(TRAY[0] - 100, panel_top + 50) != DESKTOP, hex(px(TRAY[0] - 100, panel_top + 50)))
    at = v.mark()
    click(BELL[0] + 24 - 8 - 28, panel_top + 6 + 11)
    check('Clear clears them', v.wait('wm: notifications cleared', 5, at), v.text(at))
    v.key('esc'); time.sleep(0.5)
    w, h, px = v.screenshot()
    check('Escape closes the panel', px(TRAY[0] - 100, panel_top + 50) == DESKTOP, hex(px(TRAY[0] - 100, panel_top + 50)))
    v.key('meta_l-shift-q')
finally:
    v.close()
finish()
