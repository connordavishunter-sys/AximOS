"""Checks the desktop starts at boot, behind its log-in screen, and that holding
Shift starts the shell instead, whether the kernel sees it or only the BIOS did."""
import time
from vm import VM, check, finish

v = VM(desktop=True)
try:
    check('the desktop starts by itself', v.wait('wm: running at', 30), v.text())
    check('locked, behind the log-in screen', v.wait('login: welcome', 10), v.text())
    at = v.mark()
    v.key('meta_l-shift-q'); time.sleep(1)
    check("which can't be got past by quitting", '/$ ' not in v.text(at), v.text(at))
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
