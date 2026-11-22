"""Checks the desktop starts at boot, behind its log-in screen, and that holding
Shift starts the shell instead, as does a debug build, which has /system/etc/debug."""
import time
from vm import VM, WORK, check, disk_copy, finish, put

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

img = disk_copy('debug')
open(WORK + '/debug', 'w').close()
put(img, WORK + '/debug', '/system/etc/debug')
v = VM(disk=img, desktop=True)
try:
    check('a debug build starts the shell without Shift', v.wait('/$ ', 30), v.text())
    check('and says why', 'init: a debug build, so starting the shell' in v.text() and 'wm: running' not in v.text(), v.text())
finally:
    v.close()

finish()
