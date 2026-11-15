"""Checks term's tabs, finding in what it's kept, double-clicking a word to select
it, and its right-click menu."""
import time
from vm import VM, check, finish

# The first window's contents start at 80, 84, and term has a 4 pixel margin, with
# the tabs' bar, 26 tall, over that when there are two or more.
LEFT, TOP = 84, 88

v = VM(vmmouse=True)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper; echo text sharp > /system/etc/gui.conf')
    at = v.mark()
    v.type('wm term\n')
    check('term starts with a tab', v.wait('term: tab 1 of 1', 10, at), v.text(at))
    time.sleep(1.5)

    at = v.mark()
    v.key('ctrl-shift-t')
    check('Ctrl+Shift+T opens another', v.wait('term: tab 2 of 2', 5, at), v.text(at))
    time.sleep(1)
    v.type('echo two > /tmp/two\n')
    at = v.mark()
    v.key('ctrl-tab')
    check('Ctrl+Tab goes to the first', v.wait('term: tab 1 of 2', 5, at), v.text(at))
    v.pointer(LEFT + 4 + 170 + 40, 84 + 12, 1); v.pointer(LEFT + 4 + 170 + 40, 84 + 12, 0)
    check('clicking a tab goes to it', v.wait('term: tab 2 of 2', 5, at), v.text(at))
    v.key('ctrl-tab'); time.sleep(0.5)

    # Finding goes back from the newest line, and Enter again to the one before.
    v.type('clear; for n in 1 2 3; do echo word$n; echo needle; done\n'); time.sleep(1)
    at = v.mark()
    v.key('ctrl-shift-f'); v.type('needle'); v.key('ret')
    check('Ctrl+Shift+F finds the newest', v.wait('term: found needle on line', 5, at), v.text(at))
    first = v.text(at).split('term: found needle on line ')[1].split()[0]
    at = v.mark()
    v.key('ret')
    check('and Enter the one before', v.wait('term: found needle on line', 5, at)
          and v.text(at).split('term: found needle on line ')[1].split()[0] == str(int(first) - 2), v.text(at))
    v.key('esc')

    # Double-clicking word2, on the screen's first line after clear, selects it, to copy.
    v.type('clear; echo word2\n'); time.sleep(1)
    x, y = LEFT + 2 * 8, TOP + 26 + 8
    v.pointer(x, y)
    for b in (1, 0, 1, 0):
        v.command(f'mouse_button {b}')
    v.key('ctrl-shift-c')
    v.type('echo '); v.key('ctrl-shift-v'); v.type(' > /tmp/copied\n'); time.sleep(0.5)

    # The menu from a right-click, with New tab third.
    at = v.mark()
    v.pointer(400, 300)
    v.command('mouse_button 2'); v.command('mouse_button 0'); time.sleep(0.3)
    v.pointer(410, 300 + 4 + 2 * 22 + 11, 1); v.pointer(410, 300 + 4 + 2 * 22 + 11, 0)
    check('the right-click menu opens a tab', v.wait('term: tab 3 of 3', 5, at), v.text(at))
    at = v.mark()
    v.key('ctrl-shift-w')
    check('Ctrl+Shift+W closes it', v.wait('term: tab 2 of 2', 5, at), v.text(at))
    time.sleep(0.5)
    v.type('exit\n')
    check('exit closes a tab', v.wait('term: tab 1 of 1', 5, at), v.text(at))
    v.key('meta_l-shift-q'); time.sleep(1)
    o = v.run('cat /tmp/two /tmp/copied')
    check('each tab has its own shell', 'two' in o, o)
    check('a double click selects a word', 'word2' in o, o)
finally:
    v.close()
finish()
