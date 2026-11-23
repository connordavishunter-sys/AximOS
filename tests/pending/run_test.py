"""Checks the run prompt: Super+R and the start menu open it, it says when there's
no such program, starts programs and opens folders, remembers what was run, and
Browse... picks a program."""
import time
from vm import VM, check, finish

BAR = 760

v = VM(vmmouse=True)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper; rm -f /system/etc/gui.conf /system/etc/run_history')
    at = v.mark()
    v.type('wm\n')
    check('wm starts', v.wait('wm: running', 10, at), v.text(at))
    time.sleep(0.5)

    def prompt():
        at = v.mark()
        v.key('meta_l-r')
        check('Super+R opens it', v.wait('run: ready', 5, at), v.text(at))
        time.sleep(0.3)

    prompt()
    at = v.mark()
    v.type('nosuch\n')
    check("it says when there's no such program", v.wait("run: can't find nosuch", 5, at), v.text(at))
    v.key('ctrl-a'); v.type('guidemo\n')
    check('and runs one there is', v.wait('run: running guidemo', 5, at) and v.wait('guidemo: window', 5, at), v.text(at))

    prompt()
    at = v.mark()
    v.key('ret')
    check('with the last command ready to run again', v.wait('run: running guidemo', 5, at) and v.wait('guidemo: window', 5, at),
          v.text(at))

    prompt()
    at = v.mark()
    v.type('/tmp\n')
    check('a folder opens in files', v.wait('files: in /tmp:', 10, at), v.text(at))

    prompt()
    at = v.mark()
    v.key('up'); v.key('ret')
    check('Up goes back through what was run', v.wait('run: running guidemo', 5, at), v.text(at))

    # The start menu: five folders and three apps, 24 pixels each, a line, then Run....
    menu_top = BAR - 8 - 11 * 24 - 9 - 8 # floating 8 above the taskbar
    at = v.mark()
    v.pointer(36, BAR + 14, 1); v.pointer(36, BAR + 14, 0)
    time.sleep(0.5)
    v.pointer(100, menu_top + 4 + 8 * 24 + 9 + 12, 1); v.pointer(100, menu_top + 4 + 8 * 24 + 9 + 12, 0)
    check('Run... in the start menu opens it', v.wait('run: ready', 5, at), v.text(at))
    time.sleep(0.3)

    # Browse... is after the box, OK and Cancel, and opens a picker in /system/bin.
    at = v.mark()
    v.key('tab'); v.key('tab'); v.key('tab'); v.key('ret'); time.sleep(0.5)
    v.type('ls\n'); time.sleep(0.5)
    v.key('ret')
    check('Browse... picks a program', v.wait('run: running /system/bin/ls', 5, at), v.text(at))

    prompt()
    at = v.mark()
    v.key('esc'); time.sleep(0.5)
    check('Escape closes it', 'run: running' not in v.text(at), v.text(at))

    v.key('meta_l-shift-q'); time.sleep(1)
    o = v.run('cat /system/etc/run_history')
    kept = [line for line in o.split('\n') if line and not line.startswith('/$')]
    check('what was run is kept, newest first', kept[:3] == ['/system/bin/ls', 'guidemo', '/tmp'], o)
finally:
    v.close()
finish()
