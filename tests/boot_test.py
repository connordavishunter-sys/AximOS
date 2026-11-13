"""Checks the desktop starts at boot, and that holding Shift starts the shell instead,
whether the kernel sees it or only the BIOS did."""
import time
from vm import VM, check, finish

v = VM(desktop=True)
try:
    check('the desktop starts by itself', v.wait('wm: running at', 30), v.text())
    time.sleep(1)
    w, h, px = v.screenshot()
    check('with its taskbar', px(700, h - 10) != 0, hex(px(700, h - 10)))
    at = v.mark()
    v.key('meta_l-shift-q')
    check('quitting it leaves the shell', v.wait('/$ ', 10, at), v.text(at))
finally:
    v.close()

v = VM()
try:
    check('holding Shift starts the shell', v.wait('/$ ', 30), v.text())
    check('without the desktop', 'wm: running' not in v.text(), v.text())
finally:
    v.close()

# As a VM's window sends a held key once, Shift is pressed only until the kernel
# starts, and never let go, so only the BIOS saw it.
v = VM(desktop=True)
try:
    end = time.time() + 30
    while 'kernel: started' not in v.raw() and time.time() < end:
        v.shift(True)
        time.sleep(0.05)
    check('Shift held since the BIOS starts the shell', v.wait('init: Shift is held', 30), v.text())
    check('and the prompt', v.wait('/$ ', 10), v.text())
finally:
    v.close()
finish()
