"""Checks Snake: it starts with Space, dies at the wall, turns, pauses and goes on."""
import time
from vm import VM, check, finish

v = VM(vmmouse=True)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper; rm -f /system/etc/gui.conf')
    at = v.mark()
    v.type('wm snake\n')
    check('Snake starts', v.wait('snake: ready', 10, at), v.text(at))
    time.sleep(0.5)

    # It starts in the middle, going right, 12 squares from the wall.
    at = v.mark()
    v.key('spc')
    check('Space starts a game', v.wait('snake: start', 5, at), v.text(at))
    check('which ends at the wall', v.wait('snake: game over', 5, at), v.text(at))

    # Up, then paused, it goes nowhere until it's played again.
    at = v.mark()
    v.key('spc')
    check('Space starts again', v.wait('snake: start', 5, at), v.text(at))
    v.key('up'); v.key('p')
    check('P pauses', v.wait('snake: paused', 5, at), v.text(at))
    time.sleep(2.5)
    check('and nothing happens', 'game over' not in v.text(at), v.text(at))
    v.key('p')
    check('P again plays on', v.wait('snake: playing', 5, at), v.text(at))
    check('until the top wall', v.wait('snake: game over', 5, at), v.text(at))
    v.key('meta_l-shift-q')
    time.sleep(0.5)
finally:
    v.close()
finish()
