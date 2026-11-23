"""Checks Files' places down the side, hidden files, sorting, searching, the
properties of a file, moving a folder with what's in it to the trash, and the viewer going
through a folder's pictures and turning them."""
import re, time
from vm import VM, check, finish

X, Y = 1, 25 # maximized, the window's contents start here

v = VM(vmmouse=True)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper; rm -f /system/etc/gui.conf')
    v.run('mkdir /tmp/m /tmp/m/full; echo a > /tmp/m/full/in.txt; echo small > /tmp/m/b.txt; '
          'echo a much bigger file than that > /tmp/m/a.txt; echo hide > /tmp/m/.secret')

    def click(x, y):
        v.pointer(x, y, 1); v.pointer(x, y, 0)

    def menu(x, y, item):
        v.pointer(x, y)
        v.command('mouse_button 2'); v.command('mouse_button 0'); time.sleep(0.3)
        click(x + 20, y + 4 + item + 11)

    where = lambda name: [tuple(map(int, m)) for m in re.findall(rf'files: at {name} (\d+) (\d+) (\d+) (\d+)', v.text())][-1]

    at = v.mark()
    v.type("wm 'files /tmp/m'\n")
    check('dot files are hidden', v.wait('files: in /tmp/m: full/ a.txt b.txt\n', 10, at), v.text(at))
    time.sleep(1)
    v.key('meta_l-up'); time.sleep(1)
    lx, ly, lw, lh = where('list')
    at = v.mark()
    click(X + lx + 60, Y + ly + 2 + 11)
    v.key('ctrl-h')
    check('Ctrl+H shows them', v.wait('files: in /tmp/m: full/ .secret a.txt b.txt', 5, at), v.text(at))
    v.key('ctrl-h')
    v.wait('files: hidden files hidden', 5, at)

    # Sorting, from under the places.
    sx, sy, sw, sh = where('sort')
    at = v.mark()
    click(X + sx + 20, Y + sy + 10); time.sleep(0.3)
    v.key('down'); v.key('ret')
    check('sorting by size', v.wait('files: sorted by size', 5, at) and v.wait('files: in /tmp/m: full/ a.txt b.txt', 5, at),
          v.text(at))

    at = v.mark()
    v.key('ctrl-f'); v.type('b.t')
    check('searching lists what matches', v.wait('files: in /tmp/m: b.txt\n', 5, at), v.text(at))
    v.key('ctrl-a'); v.key('backspace')
    v.wait('files: in /tmp/m: full/ a.txt b.txt', 5, at)

    # A file's menu ends with a line and Properties, after Open, Open in Editor,
    # Run, a line, Cut, Copy, Copy path, Rename and Delete.
    at = v.mark()
    menu(X + lx + 60, Y + ly + 2 + 22 + 11, 8 * 22 + 2 * 9)
    check('Properties says how big it is', v.wait('files: properties of /tmp/m/a.txt: 29 bytes', 5, at), v.text(at))
    time.sleep(0.5)
    v.key('ret')

    at = v.mark()
    click(X + lx + 60, Y + ly + 2 + 11); v.key('delete')
    check('Delete moves a folder with what is in it to the trash', v.wait('files: trashed /tmp/m/full', 5, at), v.text(at))

    # The places: Home first.
    at = v.mark()
    click(X + 30, Y + ly + 2 + 11)
    check('Home in the places', v.wait('files: in /home: Desktop/ Documents/ Pictures/', 5, at), v.text(at))
    v.key('meta_l-q'); time.sleep(0.5)

    # The viewer goes through the pictures in a folder, and turns them.
    v.key('meta_l-shift-q'); time.sleep(1)
    v.run('mkdir /tmp/p; cp /system/share/icons/apps/paint.png /tmp/p/a.png; '
          'cp /system/share/icons/apps/snake.png /tmp/p/b.png; cp /system/share/wallpapers/default.jpg /tmp/p/c.jpg')
    at = v.mark()
    v.type("wm 'viewer /tmp/p/a.png'\n")
    check('the viewer shows one', v.wait('viewer: showing /tmp/p/a.png', 10, at), v.text(at))
    time.sleep(1)
    at = v.mark()
    v.key('right')
    check('Right shows the next', v.wait('viewer: showing /tmp/p/b.png', 5, at), v.text(at))
    v.key('end')
    check('End the last', v.wait('viewer: showing /tmp/p/c.jpg', 5, at), v.text(at))
    v.key('left')
    check('Left the one before', v.wait('viewer: showing /tmp/p/b.png', 5, at), v.text(at))
    w = re.search(r'viewer: showing /tmp/p/c.jpg (\d+)x(\d+)', v.text(at)).groups()
    v.key('end'); v.key('r')
    check('R turns it', v.wait(f'viewer: turned right {w[1]}x{w[0]}', 5, at), v.text(at))
    v.key('meta_l-shift-q'); time.sleep(1)
finally:
    v.close()
finish()
