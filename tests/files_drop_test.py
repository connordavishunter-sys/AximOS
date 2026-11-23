"""Checks dragging in Files: onto a folder moves a file there, or copies it with Ctrl
held, and onto a tab moves it to that tab's folder; and the grid of thumbnails."""
import re, time
from vm import VM, check, finish

X, Y = 1, 25 # maximized, the window's contents start here

v = VM()
try:
    v.desktop(setup=['echo none > ~/.config/aximos/wallpaper; rm -f ~/.config/aximos/gui.conf'])
    v.run('mkdir /tmp/fd /tmp/fd/box /tmp/other; echo a > /tmp/fd/a.txt; echo b > /tmp/fd/b.txt; '
          'cp /system/share/icons/apps/paint.png /tmp/fd/pic.png')

    def ctrl(down):
        v.qmp_command('input-send-event', events=[{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': 'ctrl'}}}])

    def drag(a, b):
        v.pointer(*a, 1); time.sleep(0.2)
        for n in range(1, 6):
            v.pointer(a[0] + (b[0] - a[0]) * n // 5, a[1] + (b[1] - a[1]) * n // 5, 1); time.sleep(0.1)
        v.pointer(*b, 0)

    where = lambda name: [tuple(map(int, m)) for m in re.findall(rf'files: at {name} (\d+) (\d+) (\d+) (\d+)', v.text())][-1]

    at = v.mark()
    v.start('files /tmp/fd')
    check('Files starts', v.wait('files: in /tmp/fd: box/ a.txt b.txt pic.png', 10, at), v.text(at))
    time.sleep(1)
    at = v.mark()
    v.key('meta_l-up')
    v.wait('files: at list', 5, at); time.sleep(0.5)
    lx, ly, lw, lh = where('list')
    row = lambda i: (X + lx + 60, Y + ly + 2 + i * 22 + 11)

    at = v.mark()
    drag(row(1), row(0))
    check('dropping a file on a folder moves it there', v.wait('files: moved /tmp/fd/a.txt to /tmp/fd/box/a.txt', 5, at)
          and v.wait('files: in /tmp/fd: box/ b.txt pic.png', 5, at), v.text(at))
    at = v.mark()
    ctrl(True); drag(row(1), row(0)); ctrl(False)
    check('with Ctrl held, copies it', v.wait('files: copied /tmp/fd/b.txt to /tmp/fd/box/b.txt', 5, at)
          and v.wait('files: in /tmp/fd: box/ b.txt pic.png', 5, at), v.text(at))
    at = v.mark()
    drag(row(1), row(2))
    time.sleep(1)
    check('dropping on a file, where it is, does nothing', 'files: dropped /tmp/fd/b.txt where it was' in v.text(at),
          v.text(at))

    # A second tab, in /tmp/other, takes pic.png from the first.
    at = v.mark()
    v.key('ctrl-t')
    check('a new tab', v.wait('files: tab 2 of 2', 5, at), v.text(at))
    ax, ay, aw, ah = where('address')
    v.pointer(X + ax + 100, Y + ay + 10, 1); v.pointer(X + ax + 100, Y + ay + 10, 0)
    v.key('ctrl-a'); v.keys('/tmp/other\n')
    check('goes elsewhere', v.wait('files: in /tmp/other:', 5, at), v.text(at))
    v.key('ctrl-shift-tab')
    check('back to the first', v.wait('files: in /tmp/fd:', 5, at), v.text(at))
    time.sleep(0.5)
    at = v.mark()
    drag(row(2), (X + 4 + 170 + 85, Y + 4 + 12))
    check('dropping on a tab moves it to its folder', v.wait('files: moved /tmp/fd/pic.png to /tmp/other/pic.png', 5, at),
          v.text(at))

    # The grid shows the entries as cells 96 wide, and takes drops the same.
    v.key('ctrl-tab'); time.sleep(0.3)
    at = v.mark()
    vx, vy, vw, vh = where('view')
    v.pointer(X + vx + 10, Y + vy + 10, 1); v.pointer(X + vx + 10, Y + vy + 10, 0)
    check('Grid shows the grid', v.wait('files: view grid', 5, at), v.text(at))
    time.sleep(0.5)
    v.pointer(1200, 700); time.sleep(0.3)
    w, h, px = v.screenshot()
    colours = {px(x, y) for x in range(X + lx + 2, X + lx + 96) for y in range(Y + ly + 2, Y + ly + 72)}
    check('with a thumbnail of the picture', len(colours) > 20, len(colours))
    v.key('ctrl-shift-tab'); time.sleep(0.5)
    cell = lambda i: (X + lx + 2 + i * 96 + 46, Y + ly + 2 + 40)
    at = v.mark()
    drag(cell(1), cell(0))
    check('dropping on a folder in the grid moves it there, by a name not taken',
          v.wait('files: moved /tmp/fd/b.txt to /tmp/fd/box/b (copy).txt', 5, at), v.text(at))
    v.key('meta_l-shift-q'); time.sleep(1)
    o = v.run('ls /tmp/fd/box; ls /tmp/other')
    check('the files are where they went', all(n in o for n in ('a.txt', 'b.txt', 'b (copy).txt', 'pic.png')), o)
finally:
    v.close()
finish()
