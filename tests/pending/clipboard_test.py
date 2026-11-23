"""Checks zooming in the viewer and Paint, pictures on the clipboard from the viewer
and from Print Screen pasted into Paint, and undo and redo in text."""
import re, time
from PIL import Image
from vm import VM, check, finish

X, Y = 80, 84 # the first window's contents
BACKGROUND = 0x1E2129

v = VM(vmmouse=True)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper; rm -f /system/etc/gui.conf')
    v.run('cp /system/share/icons/apps/paint.png /home/Pictures/pic.png')

    def click(x, y):
        v.pointer(x, y, 1); v.pointer(x, y, 0)

    def run(command, ready):
        at = v.mark()
        v.key('meta_l-r'); v.wait('run: ready', 5, at)
        v.type(command + '\n')
        ok = v.wait(ready, 10, at)
        time.sleep(1)
        return ok

    def shown():
        w, h, px = v.screenshot()
        return sum(px(x, y) != BACKGROUND for x in range(X, X + 240) for y in range(Y, Y + 240))

    at = v.mark()
    v.type("wm 'viewer /home/Pictures/pic.png'\n")
    check('the viewer shows a picture', v.wait('viewer: showing /home/Pictures/pic.png 16x16', 10, at), v.text(at))
    time.sleep(1)
    v.pointer(700, 600); time.sleep(0.3)
    small = shown()
    at = v.mark()
    v.key('equal')
    check('= zooms in', v.wait('viewer: zoom 200%', 5, at), v.text(at))
    v.pointer(X + 120, Y + 120)
    v.command('mouse_move 0 0 1') # the wheel up, once
    check('and the wheel', v.wait('viewer: zoom 400%', 5, at), v.text(at))
    v.key('equal')
    check('again', v.wait('viewer: zoom 800%', 5, at), v.text(at))
    v.pointer(700, 600); time.sleep(0.3)
    big = shown()
    check('showing it bigger', 40 * small < big, (small, big))

    # At 800% in the 240 square window, each pixel is 8 square from 56 in, and what's
    # see-through shows the checks, 0x3A3F4B and 0x2E323C, through it.
    def over(p, under):
        a = p[3] + (p[3] >> 7)
        return tuple((p[i] * a + under[i] * (256 - a)) >> 8 for i in range(3))
    source = Image.open('rootfs/system/share/icons/apps/paint.png').convert('RGBA')
    w, h, px = v.screenshot()
    wrong = []
    for j in range(16):
        for i in range(16):
            got = px(X + 56 + 8 * i + 4, Y + 56 + 8 * j + 4)
            got = (got >> 16, (got >> 8) & 255, got & 255)
            if got not in (over(source.getpixel((i, j)), (0x3A, 0x3F, 0x4B)), over(source.getpixel((i, j)), (0x2E, 0x32, 0x3C))):
                wrong.append((i, j, got, source.getpixel((i, j))))
    check("each pixel's colour is the picture's, over the checks", not wrong, wrong[:5])
    v.key('minus')
    check('- zooms out', v.wait('viewer: zoom 400%', 5, at), v.text(at))
    v.key('0')
    check('0 fits it to the window', v.wait('viewer: zoom to fit', 5, at), v.text(at))
    v.key('ctrl-c')
    check('Ctrl+C copies the picture', v.wait('viewer: copied 16x16', 5, at), v.text(at))
    v.key('meta_l-q'); time.sleep(0.5)

    check('Paint opens', run('paint', 'paint: at picture'), v.text())
    at = v.mark()
    v.key('meta_l-up') # maximized, its contents start at 1, 25
    v.wait('paint: at picture', 5, at); time.sleep(0.5)
    X, Y = 1, 25
    where = lambda name: [tuple(map(int, m)) for m in re.findall(rf'paint: at {name} (-?\d+) (-?\d+) (\d+) (\d+)', v.text())][-1]
    px0, py0, _, _ = where('picture')
    pic = lambda x, y: (X + px0 + x, Y + py0 + y)
    at = v.mark()
    v.key('ctrl-v')
    check('and Ctrl+V pastes it there', v.wait('paint: pasted 16x16', 5, at), v.text(at))
    v.pointer(700, 700); time.sleep(0.5)
    w, h, px = v.screenshot()
    corner = [px(*pic(x, y)) for x, y in ((5, 5), (8, 8), (10, 6))]
    check('at the top left', any(c != 0xFFFFFF for c in corner), [hex(c) for c in corner])

    # At 200%, each pixel of the picture is 2 by 2, and the mouse draws at half the
    # distance. It zooms on the middle, so it's scrolled, and that's scrolled back.
    at = v.mark()
    v.key('ctrl-equal')
    check('Ctrl+= zooms in', v.wait('paint: zoom 200%', 5, at), v.text(at))
    time.sleep(0.5)
    sx, sy = map(int, re.findall(r'paint: scrolled (\d+) (\d+)', v.text())[-1])
    on = lambda x, y: (X + px0 + 2 * x + 1 - sx, Y + py0 + 2 * y + 1 - sy)
    check('keeping the middle in the middle', sx > 0 and sy > 0, (sx, sy))
    at = v.mark()
    v.pointer(*on(sx // 2 + 50, sy // 2 + 50), 1); v.pointer(*on(sx // 2 + 75, sy // 2 + 50), 1)
    v.pointer(*on(sx // 2 + 100, sy // 2 + 50), 0)
    check('drawing where the mouse is on the picture',
          v.wait(f'paint: Pencil {sx // 2 + 50} {sy // 2 + 50} to {sx // 2 + 100} {sy // 2 + 50}', 5, at), v.text(at))
    v.key('ctrl-minus')
    check('Ctrl+- zooms out', v.wait('paint: zoom 100%', 5, at), v.text(at))

    at = v.mark()
    v.key('print')
    check('Print Screen copies the screen too', v.wait('wm: notice Screenshot saved and copied', 10, at), v.text(at))
    v.key('ctrl-v')
    check('which pastes in Paint, making the picture bigger', v.wait('paint: pasted 1280x800', 5, at), v.text(at))
    v.key('ctrl-z')
    check('and undoes', v.wait('paint: undone', 5, at), v.text(at))
    v.key('meta_l-q'); time.sleep(0.5); v.key('right'); v.key('ret'); time.sleep(0.5) # not saved: Don't save

    # Undo takes typing back a word at a time, and redo puts it back.
    check('the editor opens', run('editor', 'editor: title'), v.text())
    v.key('meta_l-up'); time.sleep(0.5)
    click(640, 400)
    v.type('hello world')
    v.key('ctrl-z')
    at = v.mark()
    v.key('ctrl-s'); time.sleep(0.5); v.type('/home/Documents/undo.txt\n')
    check('Ctrl+Z takes back the last word', v.wait('editor: saved /home/Documents/undo.txt 6 bytes', 5, at), v.text(at))
    click(640, 400)
    v.key('ctrl-y')
    at = v.mark()
    v.key('ctrl-s')
    check('Ctrl+Y puts it back', v.wait('editor: saved /home/Documents/undo.txt 11 bytes', 5, at), v.text(at))
    click(640, 400)
    for _ in range(3):
        v.key('ctrl-z')
    at = v.mark()
    v.key('ctrl-s')
    check('and Ctrl+Z goes back to the start', v.wait('editor: saved /home/Documents/undo.txt 0 bytes', 5, at), v.text(at))
    v.key('meta_l-shift-q')
    time.sleep(1)
finally:
    v.close()
finish()
