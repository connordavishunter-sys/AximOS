"""Checks the editor's tabs, line numbers and wrapping, keeping the indent and
indenting with Tab, the files opened last, and asking about changes before closing."""
import time
from vm import VM, check, finish

v = VM(vmmouse=True)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper; rm -f /system/etc/gui.conf /home/.editor')
    v.run("printf 'int main() {' > /home/a.c")

    def click(x, y):
        v.pointer(x, y, 1); v.pointer(x, y, 0)

    # The shell is wm's while it runs, so what's saved is checked by its size.
    def saved(size):
        at = v.mark()
        v.key('ctrl-s')
        return v.wait(f'editor: saved /home/a.c {size} bytes', 5, at)

    at = v.mark()
    v.type("wm 'editor /home/a.c'\n")
    check('the editor opens the file', v.wait('editor: opened /home/a.c 12 bytes', 10, at), v.text(at))
    time.sleep(1)
    v.key('meta_l-up'); time.sleep(0.5)
    w, h, px = v.screenshot()
    click(640, 400)
    v.key('ctrl-end'); v.key('end'); v.key('ret'); v.type('x;')
    check('Enter after { goes in further', saved(19), v.text())
    v.key('ret'); v.type('y;')
    check('and keeps the indent', saved(26), v.text())
    v.key('ctrl-a'); v.key('tab')
    check('Tab indents what is selected', saved(38), v.text())
    v.key('shift-tab'); v.key('shift-tab')
    check('Shift+Tab takes it back', saved(18), v.text())
    v.key('ctrl-z')
    check('and undo puts back a step', saved(26), v.text())

    # The line numbers' gutter is on by default, and View turns them off and wraps.
    at = v.mark()
    v.key('f10'); v.key('right'); v.key('right'); v.key('ret')
    check('View hides the line numbers', v.wait('editor: line numbers off', 5, at), v.text(at))
    v.key('f10'); v.key('right'); v.key('right'); v.key('down'); v.key('ret')
    check('and wraps', v.wait('editor: wrapping on', 5, at), v.text(at))

    # A second tab, with changes, asks before closing.
    at = v.mark()
    v.key('ctrl-n')
    check('Ctrl+N opens a tab', v.wait('editor: tab 2 of 2', 5, at), v.text(at))
    click(640, 400); v.type('words')
    at = v.mark()
    v.key('ctrl-w')
    check('closing it with changes asks', v.wait('editor: asking about Untitled', 5, at), v.text(at))
    time.sleep(0.5)
    v.key('esc')
    check('Escape cancels', v.wait('editor: close cancelled', 5, at), v.text(at))
    at = v.mark()
    v.key('ctrl-w'); time.sleep(0.5); v.key('right'); v.key('ret')
    check("and Don't save closes it", v.wait('editor: tab 1 of 1', 5, at), v.text(at))

    # Quitting with changes asks, and Save saves before closing.
    click(640, 400); v.key('ctrl-end'); v.type(' z')
    at = v.mark()
    v.key('ctrl-q')
    check('Ctrl+Q asks about changes', v.wait('editor: asking about a.c', 5, at), v.text(at))
    time.sleep(0.5)
    v.key('ret')
    check('and Save saves them, then quits', v.wait('editor: saved /home/a.c', 5, at) and v.wait('editor: closed', 5, at),
          v.text(at))
    v.key('meta_l-shift-q'); time.sleep(1)
    o = v.run('cat /home/a.c; echo "|"; cat /home/.editor')
    check('the file is as it was saved', 'int main() {\n    x;\n    y; z' in o, o)
    check('and the view is kept, with the file opened', 'numbers 0' in o and 'wrap 1' in o and 'recent /home/a.c' in o, o)
finally:
    v.close()
finish()
