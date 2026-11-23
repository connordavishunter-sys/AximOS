"""Checks notifications: they stack up from the bottom right corner, a click puts
one away, and the rest go by themselves after five seconds."""
import time
from vm import VM, check, finish

W, H = 1280, 800
DESKTOP = 0x1E2430
# 300 by 52, 12 from the right and 12 above the taskbar, 32 tall, the newest lowest, 8 apart.
X = W - 300 - 12
BOTTOM = (X + 150, H - 8 - 32 - 8 - 26)        # the middle of the lowest
ABOVE = (X + 150, H - 8 - 32 - 8 - 52 - 8 - 26) # and of the one above it

v = VM(vmmouse=True)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper; rm -f /system/etc/gui.conf')
    at = v.mark()
    v.type("wm \"sleep 1; notify Hello 'from the shell'; sleep 0.5; notify Second\"\n")
    check('notify shows one', v.wait('wm: notice Hello: from the shell', 10, at), v.text(at))
    check('and another', v.wait('wm: notice Second: ', 5, at), v.text(at))
    time.sleep(0.5)
    w, h, px = v.screenshot()
    check('in the corner, stacked', px(*BOTTOM) != DESKTOP and px(*ABOVE) != DESKTOP, (hex(px(*BOTTOM)), hex(px(*ABOVE))))
    check('with nothing above them', px(ABOVE[0], ABOVE[1] - 60) == DESKTOP, hex(px(ABOVE[0], ABOVE[1] - 60)))

    v.pointer(*BOTTOM, 1); v.pointer(*BOTTOM, 0); time.sleep(0.5)
    w, h, px = v.screenshot()
    check('clicking the newest puts it away, and the other moves down', px(*BOTTOM) != DESKTOP and px(*ABOVE) == DESKTOP,
          (hex(px(*BOTTOM)), hex(px(*ABOVE))))
    v.pointer(640, 300)
    time.sleep(5)
    w, h, px = v.screenshot()
    check('the other goes by itself', px(*BOTTOM) == DESKTOP, hex(px(*BOTTOM)))

    v.key('meta_l-shift-q'); time.sleep(1)
    o = v.run('notify; echo st=$?; notify hi; echo st=$?')
    check("notify says how to use it, and that wm isn't running", 'usage: notify' in o and 'st=2' in o
          and "wm isn't running" in o and 'st=1' in o, o)
finally:
    v.close()
finish()
