"""Checks searching the start menu by typing, clicks in the gap below the floating
taskbar reaching it, the clipboard's history in the tray, putting something copied
before back, and Recent in Files, of the files opened lately."""
import re, time
from vm import VM, check, finish

W, H, BAR = 1280, 800, 32
X, Y = 1, 25 # where a maximized window's contents start
CLIPS = (W - 8 - 36 - 12 - 8 - 24 - 2 - 44 - 2 - 24 + 12, H - 8 - BAR // 2) # left of the network and speaker, by a 24 hour clock

v = VM(vmmouse=True)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper; echo clock 24 > /system/etc/gui.conf; rm -f /system/etc/recent; '
          'mkdir /tmp/r; echo hello > /tmp/r/note.txt')

    def click(x, y):
        v.pointer(x, y, 1); v.pointer(x, y, 0)

    at = v.mark()
    v.type('wm\n')
    v.wait('wm: running', 10, at); time.sleep(1)

    # The start menu finds apps in its folders by what's typed, and Enter starts the first.
    at = v.mark()
    v.key('meta_l'); v.wait('wm: menu 0 at', 5, at)
    v.type('snak')
    check('typing searches the apps', v.wait('wm: search snak: 1 apps', 5, at), v.text(at))
    v.key('backspace'); v.key('backspace'); v.key('backspace')
    check('Backspace changes it', v.wait('wm: search s: ', 5, at), v.text(at))
    v.type('nak\n')
    check('and Enter starts the one found', v.wait('snake: ready', 10, at), v.text(at))
    time.sleep(0.5)
    v.key('meta_l-q'); time.sleep(0.5)

    # Below the taskbar, at the very bottom of the screen, is still the taskbar.
    at = v.mark()
    click(30, H - 1)
    check('a click under Apps opens the menu', v.wait('wm: menu 0 at', 5, at), v.text(at))
    v.key('esc'); time.sleep(0.3)
    at = v.mark()
    click(W - 30, H - 1)
    check('and under the clock, the calendar', v.wait('wm: calendar', 5, at), v.text(at))
    v.key('esc'); time.sleep(0.3)

    # Two things copied in the run box are both in the clipboard's history, newest
    # first, and choosing the older puts it back on the clipboard.
    for text in ('first thing', 'second thing'):
        at = v.mark()
        v.key('meta_l-r'); v.wait('run: ready', 5, at); time.sleep(0.3)
        v.type(text); v.key('ctrl-a'); v.key('ctrl-c'); v.key('esc'); time.sleep(0.5)
    at = v.mark()
    click(*CLIPS)
    check('the clipboard icon shows its history', v.wait('wm: clipboard history 2', 5, at), v.text(at))
    p = tuple(map(int, re.findall(r'wm: panel 2 at (\d+) (\d+) (\d+) (\d+)', v.text(at))[-1]))
    at = v.mark()
    click(p[0] + 40, p[1] + 34 + 24 + 12)
    check('choosing the older one', v.wait('wm: clipboard from history 1', 5, at), v.text(at))
    at = v.mark()
    v.key('meta_l-r'); v.wait('run: ready', 5, at); time.sleep(0.3)
    v.key('ctrl-v'); v.key('ret')
    check('puts it back to paste', v.wait("run: can't find first", 5, at), v.text(at))
    v.key('esc'); time.sleep(0.5)

    # A file opened in Files is in Recent, its last place.
    at = v.mark()
    v.key('meta_l-r'); v.wait('run: ready', 5, at)
    v.type('files /tmp/r\n')
    v.wait('files: in /tmp/r: note.txt', 10, at); time.sleep(1)
    v.key('meta_l-up'); time.sleep(1)
    lx, ly, lw, lh = map(int, re.findall(r'files: at list (\d+) (\d+) (\d+) (\d+)', v.text(at))[-1])
    at = v.mark()
    click(X + lx + 40, Y + ly + 2 + 11); v.key('ret')
    check('opening a file', v.wait('files: opening /tmp/r/note.txt in editor', 5, at), v.text(at))
    v.wait('editor: ', 10, at); time.sleep(1)
    v.key('meta_l-q'); time.sleep(1)
    at = v.mark()
    click(X + 30, Y + ly + 2 + 6 * 22 + 11)
    check('Recent lists it', v.wait('files: in recent: note.txt', 5, at), v.text(at))
    v.key('meta_l-shift-q'); time.sleep(1)
    o = v.run('cat /system/etc/recent')
    check('kept in /system/etc/recent', '/tmp/r/note.txt' in o, o)
finally:
    v.close()
finish()
