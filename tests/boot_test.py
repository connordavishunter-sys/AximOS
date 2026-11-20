"""Checks the desktop starts at boot, behind its log-in screen, and that holding
Shift starts the shell instead."""
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

finish()
