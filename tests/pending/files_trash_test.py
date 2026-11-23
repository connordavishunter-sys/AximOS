"""Checks choosing several files in Files with Ctrl, moving them to the trash,
which shows the names they had, restoring one, emptying it once that's agreed to,
deleting for good with Shift+Delete, Open with, running a program, and opening
Files on a file's properties, as the desktop's menu does."""
import re, time
from vm import VM, check, finish

X, Y = 1, 25 # maximized, the window's contents start here

v = VM(vmmouse=True)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper; rm -f /system/etc/gui.conf /system/etc/trash.db')
    v.run('mkdir /tmp/t; echo a > /tmp/t/a.txt; echo b > /tmp/t/b.txt; echo c > /tmp/t/c.txt; '
          'cp /system/bin/hello /tmp/t/prog')

    def click(x, y):
        v.pointer(x, y, 1); v.pointer(x, y, 0)

    def ctrl(down):
        v.qmp_command('input-send-event', events=[{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': 'ctrl'}}}])
        time.sleep(0.1)

    def menu(x, y, item, lines=0):
        v.pointer(x, y)
        v.command('mouse_button 2'); v.command('mouse_button 0'); time.sleep(0.3)
        click(x + 20, y + 4 + item * 22 + lines * 9 + 11)

    where = lambda name: [tuple(map(int, m)) for m in re.findall(rf'files: at {name} (\d+) (\d+) (\d+) (\d+)', v.text())][-1]

    at = v.mark()
    v.type("wm 'files /tmp/t'\n")
    check('Files lists them', v.wait('files: in /tmp/t: a.txt b.txt c.txt prog\n', 10, at), v.text(at))
    time.sleep(1)
    v.key('meta_l-up'); time.sleep(1)
    lx, ly, lw, lh = where('list')
    row = lambda i: (X + lx + 60, Y + ly + 2 + i * 22 + 11)

    # a.txt, then c.txt with Ctrl, then Delete moves both to the trash.
    at = v.mark()
    click(*row(0))
    ctrl(True); click(*row(2)); ctrl(False)
    v.key('delete')
    check('Delete trashes both chosen', v.wait('files: trashed /tmp/t/a.txt', 5, at)
          and v.wait('files: trashed /tmp/t/c.txt', 5, at), v.text(at))
    check('leaving the rest', v.wait('files: in /tmp/t: b.txt prog\n', 5, at), v.text(at))

    # The trash is the last of the places, and shows the names they had.
    at = v.mark()
    click(X + 30, Y + ly + 2 + 5 * 22 + 11)
    check('the trash shows their names', v.wait('files: in /trash: a.txt c.txt\n', 5, at), v.text(at))
    at = v.mark()
    menu(*row(0), 0)
    check('Restore puts one back', v.wait('files: restored /tmp/t/a.txt', 5, at), v.text(at))
    time.sleep(0.5)
    ex, ey, ew, eh = where('empty')
    at = v.mark()
    click(X + ex + 20, Y + ey + 10)
    check('Empty trash asks first', v.wait('files: asking to empty the trash', 5, at), v.text(at))
    time.sleep(0.3)
    v.key('ret')
    check('then empties it', v.wait('files: emptied the trash', 5, at) and v.wait('files: in /trash:\n', 5, at),
          v.text(at))

    # Shift+Delete deletes for good, once agreed to. Back to the folder first,
    # typed in the address box.
    at = v.mark()
    ax, ay, aw, ah = where('address')
    click(X + ax + 40, Y + ay + 10)
    v.key('ctrl-a'); v.type('/tmp/t\n')
    check('back in the folder', v.wait('files: in /tmp/t: a.txt b.txt prog\n', 5, at), v.text(at))
    at = v.mark()
    click(*row(1))
    v.key('shift-delete')
    check('Shift+Delete asks', v.wait('files: asking to delete', 5, at), v.text(at))
    time.sleep(0.3)
    v.key('ret')
    check('then deletes it for good', v.wait('files: deleted /tmp/t/b.txt', 5, at), v.text(at))

    # Open with..., third in a file's menu, asks which app, the apps in order:
    # calculator, then editor.
    at = v.mark()
    menu(*row(0), 2)
    time.sleep(0.5)
    v.key('down')
    ox, oy, ow, oh = where('ok')
    click(X + ox + 10, Y + oy + 10)
    check('Open with opens it in the app chosen', v.wait('files: opening /tmp/t/a.txt in editor', 5, at), v.text(at))
    v.wait('editor: ', 10, at); time.sleep(1)
    v.key('meta_l-q'); time.sleep(1)

    # A program, an ELF file, runs when it's opened.
    at = v.mark()
    v.pointer(*row(1))
    for b in (1, 0, 1, 0):
        v.command(f'mouse_button {b}')
    check('a program runs', v.wait('files: running /tmp/t/prog', 5, at), v.text(at))
    v.key('meta_l-q'); time.sleep(0.5)

    at = v.mark()
    v.key('meta_l-r'); v.wait('run: ready', 5, at)
    v.type('files --properties /tmp/t/a.txt\n')
    check('files --properties shows a file', v.wait('files: properties of /tmp/t/a.txt', 10, at), v.text(at))
    v.key('meta_l-shift-q'); time.sleep(1)
    o = v.run('ls /tmp/t /trash; cat /system/etc/trash.db')
    check('what was restored is there, and the trash is empty', 'a.txt' in o and 'c.txt' not in o and 'b.txt' not in o, o)
finally:
    v.close()
finish()
