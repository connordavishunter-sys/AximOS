"""Checks files' tabs, its address bar, its keys and its right-click menus: for an
item, and for the folder where there's none."""
import re, time
from vm import VM, check, finish

X, Y = 80, 84 # where the window's contents start
DESKTOP = 0x1E2430

v = VM(vmmouse=True)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper; rm -f /system/etc/gui.conf; mkdir /tmp/docs; echo hi > /tmp/a.txt')
    at = v.mark()
    v.type("wm 'files /tmp'\n")
    check('files starts', v.wait('files: at list', 10, at), v.text(at))
    time.sleep(1)
    lx, ly, lw, lh = map(int, re.findall(r'files: at list (\d+) (\d+) (\d+) (\d+)', v.text())[-1])
    row = lambda i: (X + lx + 40, Y + ly + 2 + i * 22 + 11)
    empty = (X + lx + 200, Y + ly + 150)

    def click(x, y, button=1):
        v.pointer(x, y, button); v.pointer(x, y, 0)

    # Menus open at the mouse, with rows 22 tall from 4 down, and a line 9 tall
    # between groups.
    def menu(at, index, lines=0):
        click(*at, 2)
        click(at[0] + 40, at[1] + 4 + index * 22 + lines * 9 + 11)

    def address(path):
        click(X + 300, Y + 41 + 13)
        v.key('ctrl-a'); v.type(path + '\n')

    # Tabs: Ctrl+T opens one on the same folder, each goes its own way, and
    # clicking or Ctrl+Tab goes between them. Tabs are 170 wide from 4 across.
    at = v.mark()
    v.key('ctrl-t')
    check('Ctrl+T opens a tab', v.wait('files: tab 2 of 2', 5, at), v.text(at))
    v.pointer(*row(0)); v.command('mouse_button 1'); v.command('mouse_button 0')
    v.command('mouse_button 1'); v.command('mouse_button 0')
    check('which goes its own way', v.wait('files: in /tmp/docs:', 5, at), v.text(at))
    at = v.mark()
    click(X + 60, Y + 15)
    check('clicking the first tab shows it', v.wait('files: tab 1 of 2', 5, at) and v.wait('files: in /tmp: docs/ a.txt', 5, at),
          v.text(at))
    at = v.mark()
    v.key('ctrl-tab')
    check('Ctrl+Tab goes to the next', v.wait('files: tab 2 of 2', 5, at) and v.wait('files: in /tmp/docs:', 5, at), v.text(at))
    at = v.mark()
    click(X + 4 + 2 * 170 + 2 + 13, Y + 15)
    check('+ opens another', v.wait('files: tab 3 of 3', 5, at), v.text(at))
    at = v.mark()
    click(X + 4 + 3 * 170 - 2 - 12, Y + 17)
    check("a tab's x closes it", v.wait('files: tab 2 of 2', 5, at), v.text(at))
    at = v.mark()
    v.key('ctrl-w')
    check('and Ctrl+W', v.wait('files: tab 1 of 1', 5, at), v.text(at))

    # A folder's menu: Open, Open in new tab, then Copy path, Rename and Delete.
    at = v.mark()
    menu(row(0), 1)
    check('Open in new tab', v.wait('files: tab 2 of 2', 5, at) and v.wait('files: in /tmp/docs:', 5, at), v.text(at))
    v.key('ctrl-w'); time.sleep(0.5)

    # The folder's menu: New folder, New file, then Open terminal here and Refresh.
    at = v.mark()
    menu(empty, 1)
    v.type('notes.txt\n')
    check('New file', v.wait('files: made /tmp/notes.txt', 5, at) and v.wait('files: in /tmp: docs/ a.txt notes.txt', 5, at),
          v.text(at))
    at = v.mark()
    menu(empty, 0)
    v.type('nothing'); v.key('esc'); time.sleep(0.5)
    check('Escape asks no more', 'files: made' not in v.text(at), v.text(at))

    # Copy path, pasted in the address bar, goes there.
    at = v.mark()
    menu(row(0), 2, 1)
    click(X + 300, Y + 41 + 13)
    v.key('ctrl-a'); v.key('ctrl-v'); v.key('ret')
    check('Copy path, then pasting it in the address bar', v.wait('files: in /tmp/docs:', 5, at), v.text(at))
    at = v.mark()
    click(X + lx + 200, Y + ly + 100)
    v.key('backspace')
    check('Backspace in the list goes up', v.wait('files: in /tmp: docs/', 5, at), v.text(at))
    at = v.mark()
    address('/system/etc')
    check('Enter in the address bar goes there', v.wait('files: in /system/etc:', 5, at), v.text(at))
    address('/tmp')

    # A terminal in the folder, which writes where it is there.
    at = v.mark()
    v.wait('files: in /tmp:', 5, at)
    menu(empty, 2, 1)
    time.sleep(2)
    v.type('pwd > where\n'); time.sleep(0.5)
    v.key('meta_l-q'); time.sleep(0.5)

    # A file's menu: Open, Open in Editor and Run, then Copy path, Rename and
    # Delete. a.txt is second.
    at = v.mark()
    menu(row(1), 5, 1)
    v.key('ret')
    check('Delete from the menu', v.wait('files: deleted /tmp/a.txt', 5, at), v.text(at))
    at = v.mark()
    menu(row(1), 1)
    check('Open in Editor', v.wait('editor: opened /tmp/notes.txt', 10, at), v.text(at))
    v.key('meta_l-q'); time.sleep(0.5)

    # Closing the last tab closes the window.
    v.key('ctrl-w'); time.sleep(1)
    w, h, px = v.screenshot()
    check('closing the last tab closes the window', px(300, 300) == DESKTOP, hex(px(300, 300)))
    v.key('meta_l-shift-q'); time.sleep(1)
    o = v.run('cat /tmp/where')
    check('Open terminal here', '\n/tmp\n' in o, o)
finally:
    v.close()
finish()
