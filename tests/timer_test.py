"""Checks libgui's timers: one repeating, one once, one stopped, while wm sends
other events in between."""
import os, re, time
from vm import VM, WORK, check, compile_program, disk_copy, finish, put

P = os.path.dirname(os.path.abspath(__file__)) + '/programs/'
img = disk_copy('timers')
put(img, compile_program(P + 'timers.c', WORK + '/timers', gui=True), '/system/bin/timers')

v = VM(disk=img)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    at = v.mark()
    v.type("wm 'sleep 1; timers'\n")
    time.sleep(1.6)
    for i in range(20): # the mouse moving over it, so events and timers mix
        v.mouse(5 if i % 2 else -5, 3)
    check('the timers finish', v.wait('timers: ', 10, at), v.text(at))
    m = re.search(r'(\d+) repeats, (\d+) once at (\d+) ms, (\d+) stopped, (\d+) others, (\d+) ms', v.text(at))
    repeats, onces, once_at, stopped, others, total = map(int, m.groups())
    check('a repeating timer comes every 100 ms', 9 <= repeats <= 10, m.group(0))
    check('a one-shot comes once, on time', onces == 1 and 240 <= once_at <= 300, m.group(0))
    check('a stopped one never comes', stopped == 0, m.group(0))
    check('the last ends it after a second', 990 <= total <= 1100, m.group(0))
    check('with wm events among them', others > 0, m.group(0))
    v.key('meta_l-shift-q')
finally:
    v.close()
finish()
