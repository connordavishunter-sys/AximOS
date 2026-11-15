"""Checks Paint: the pencil, a box filled in, undo, the colour picker, and saving
a PNG that has what was drawn, then opening it again."""
import re, time
from PIL import Image
from vm import VM, WORK, check, disk_copy, finish, get

X, Y = 80, 84 # the first window's contents
WHITE, RED, BLUE = 0xFFFFFF, 0xE5484D, 0x2F80ED
SWATCH = 30 # each colour, 22 wide, with 8 between

img = disk_copy('paint')
v = VM(disk=img, vmmouse=True)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper; rm -f /system/etc/gui.conf /pic.png')
    at = v.mark()
    v.type('wm paint\n')
    check('Paint starts', v.wait('paint: at picture', 10, at), v.text(at))
    time.sleep(1)

    where = lambda name: [tuple(map(int, m)) for m in re.findall(rf'paint: at {name} (-?\d+) (-?\d+) (\d+) (\d+)', v.text())][-1]
    px0, py0, pw, ph = where('picture')
    sx, sy, _, _ = where('colours')
    pic = lambda x, y: (X + px0 + x, Y + py0 + y)
    swatch = lambda i: (X + sx + i * SWATCH + 11, Y + sy + 11)
    tool = lambda name: (X + where(name)[0] + 10, Y + where(name)[1] + 10)

    def click(x, y):
        v.pointer(x, y, 1); v.pointer(x, y, 0)

    def drag(a, b):
        v.pointer(*a, 1)
        for i in range(1, 5):
            v.pointer(a[0] + (b[0] - a[0]) * i // 4, a[1] + (b[1] - a[1]) * i // 4, 1)
        v.pointer(*b, 0)

    check('a white picture', pw == 640 and ph == 400, (pw, ph))
    w, h, px = v.screenshot()
    check('shown white', px(*pic(300, 200)) == WHITE, hex(px(*pic(300, 200))))

    at = v.mark()
    click(*swatch(4))
    check('a colour is chosen', v.wait('paint: colour E5484D', 5, at), v.text(at))
    drag(pic(20, 20), pic(200, 20))
    check('the pencil draws', v.wait('paint: Pencil 20 20 to 200 20', 5, at), v.text(at))
    time.sleep(0.3)
    w, h, px = v.screenshot()
    check('in red', px(*pic(110, 20)) == RED, hex(px(*pic(110, 20))))

    at = v.mark()
    click(*tool('Box'))
    drag(pic(50, 50), pic(150, 120))
    check('a box', v.wait('paint: Box 50 50 to 150 120', 5, at), v.text(at))
    click(*swatch(10))
    click(*tool('Fill'))
    click(*pic(100, 85))
    check('filled', v.wait('paint: filled 100 85', 5, at), v.text(at))
    v.pointer(*pic(400, 300)); time.sleep(0.3) # the cursor out of the way
    w, h, px = v.screenshot()
    check('inside the box', px(*pic(100, 85)) == BLUE, hex(px(*pic(100, 85))))
    check('its edge stays red', px(*pic(50, 85)) == RED, hex(px(*pic(50, 85))))
    check('and outside it white', px(*pic(300, 200)) == WHITE, hex(px(*pic(300, 200))))

    at = v.mark()
    v.key('ctrl-z')
    check('Ctrl+Z undoes it', v.wait('paint: undone', 5, at), v.text(at))
    time.sleep(0.3)
    w, h, px = v.screenshot()
    check('back to white', px(*pic(100, 85)) == WHITE, hex(px(*pic(100, 85))))

    # The picker takes the colour under it, and filling again puts it back.
    click(*tool('Picker'))
    click(*pic(110, 20))
    check('the picker takes a colour', v.wait('paint: colour E5484D', 5, at), v.text(at))
    click(*tool('Fill'))
    click(*pic(100, 85))
    check('filling with it', v.wait('paint: filled 100 85', 5, at), v.text(at))

    # Diagonal lines, and an oval, which is many short ones, end where they should.
    at = v.mark()
    click(*tool('Oval'))
    drag(pic(340, 80), pic(580, 320))
    check('an oval', v.wait('paint: Oval 340 80 to 580 320', 5, at), v.text(at))
    click(*tool('Line'))
    drag(pic(40, 360), pic(600, 40))
    check('a slanting line', v.wait('paint: Line 40 360 to 600 40', 5, at), v.text(at))

    at = v.mark()
    v.key('ctrl-s'); time.sleep(0.5)
    v.type('/pic.png\n')
    check('saving', v.wait('paint: saved /pic.png 640x400', 5, at), v.text(at))
    v.key('meta_l-shift-q')
    check('wm quits', v.wait('wm: quit', 5, at), v.text(at))
    time.sleep(0.5)
    at = v.mark()
    v.type("wm 'paint /pic.png'\n")
    check('and it opens again', v.wait('paint: opened /pic.png 640x400', 10, at), v.text(at))
    time.sleep(1)
    w, h, px = v.screenshot()
    check('as it was', px(*pic(100, 85)) == RED and px(*pic(300, 200)) == WHITE, hex(px(*pic(100, 85))))
    v.key('meta_l-shift-q')
    time.sleep(1)
finally:
    v.close()

# The saved picture has what was drawn.
get(img, '/pic.png', WORK + '/pic.png')
p = Image.open(WORK + '/pic.png').convert('RGB')
rgb = lambda c: ((c >> 16) & 255, (c >> 8) & 255, c & 255)
check('the PNG is the picture', p.size == (640, 400), p.size)
check('with the line', p.getpixel((110, 20)) == rgb(RED), p.getpixel((110, 20)))
check('and the box filled', p.getpixel((100, 85)) == rgb(RED), p.getpixel((100, 85)))
check('and white elsewhere', p.getpixel((300, 200)) == rgb(WHITE), p.getpixel((300, 200)))
finish()
