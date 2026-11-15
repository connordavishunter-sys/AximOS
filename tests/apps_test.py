"""Checks the file manager and text editor together, the mouse cursor's shapes, and
window shadows."""
import re, time
from PIL import Image
from vm import ROOT, VM, WORK, check, disk_copy, finish, mkdir, put

DESKTOP = 0x1E2430
BLACK, WHITE = 0x000000, 0xFFFFFF


def channels(p):
    return p >> 16, p >> 8 & 0xFF, p & 0xFF


def darker(p, than):
    return all(a < b for a, b in zip(channels(p), channels(than)))


class Pointer:
    """Moves the mouse to places on the screen, from the middle where it starts."""
    def __init__(self, v):
        self.v, self.x, self.y = v, 640, 400

    def to(self, x, y, buttons=None):
        self.v.mouse(x - self.x, y - self.y, buttons)
        self.x, self.y = x, y

    def click(self, x, y):
        self.to(x, y, 1)
        self.v.mouse(buttons=0)

    def double_click(self, x, y):
        self.to(x, y)
        for b in (1, 0, 1, 0):
            self.v.command(f'mouse_button {b}')
        time.sleep(0.3)

    def shape(self):
        """The colour under the mouse: black at the arrow's tip, white in the middle
        of the text and resize cursors."""
        w, h, px = self.v.screenshot()
        return px(self.x, self.y)


v = VM()
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper; rm -f /system/etc/gui.conf') # the plain colour and default theme, to look for
    v.run('echo hello > /tmp/hello.txt')
    o = v.run('ls /system/bin/gui'); check('editor and files are apps', 'editor' in o and 'files' in o, o)

    at = v.mark()
    v.type("wm 'files /tmp'\n")
    check('files lists the folder', v.wait('files: in /tmp: hello.txt', 10, at), v.text(at))
    v.wait('files: at list', 5, at)
    time.sleep(1)
    mouse = Pointer(v)

    # Files' contents are 560x440 from 80, 84, so the frame ends at 640 and 524.
    w, h, px = v.screenshot()
    check('a shadow under the window', darker(px(300, 528), DESKTOP) and px(300, 528) != BLACK, hex(px(300, 528)))
    check('fading out', px(300, 560) == DESKTOP and darker(px(300, 528), px(300, 536)),
          f'{px(300, 528):x} {px(300, 536):x} {px(300, 560):x}')
    check('and beside it', darker(px(645, 300), DESKTOP) and px(670, 300) == DESKTOP, hex(px(645, 300)))
    focused_shadow = px(75, 300)

    # The cursor is an arrow on the desktop and over buttons, an I-beam over text
    # boxes, and arrows on the window's edges and corners.
    mouse.to(900, 300)
    check('an arrow on the desktop', mouse.shape() == BLACK, hex(mouse.shape()))
    mouse.to(80 + 300, 84 + 41 + 13)
    check('an I-beam over a text box', mouse.shape() == WHITE, hex(mouse.shape()))
    mouse.to(80 + 25, 84 + 41 + 13)
    check('back to an arrow over a button', mouse.shape() == BLACK, hex(mouse.shape()))
    mouse.to(643, 300)
    check('a resize arrow on the right edge', mouse.shape() == WHITE, hex(mouse.shape()))
    mouse.to(643, 527)
    check('and on the corner', mouse.shape() == WHITE, hex(mouse.shape()))

    # Double-clicking a file opens it in the editor.
    x, y, lw, lh = map(int, re.findall(r'files: at list (\d+) (\d+) (\d+) (\d+)', v.text())[-1])
    at = v.mark()
    mouse.double_click(80 + x + 40, 84 + y + 2 + 11)
    check('double-clicking a file opens the editor', v.wait('editor: opened /tmp/hello.txt 6 bytes', 10, at),
          v.text(at))
    time.sleep(1)
    w, h, px = v.screenshot()
    check("the window that isn't focused has a lighter shadow", darker(focused_shadow, px(75, 300)),
          f'{focused_shadow:x} {px(75, 300):x}')

    # The editor's contents are 640x480 from 112, 116, with the text from 71 down.
    text_line = 116 + 71 + 5 + 8
    at = v.mark()
    mouse.click(112 + 300, text_line)
    check('an I-beam over the text', mouse.shape() == WHITE, hex(mouse.shape()))
    v.type(' world')
    check('typing marks the title', v.wait('editor: title *hello.txt - Editor', 5, at), v.text(at))
    v.key('ctrl-s')
    check('Ctrl+S saves', v.wait('editor: saved /tmp/hello.txt 12 bytes', 5, at), v.text(at))
    check('and unmarks it', v.wait('editor: title hello.txt - Editor', 5, at), v.text(at))

    # The line, selected with Shift+Home, goes on the clipboard for later.
    v.key('shift-home'); v.key('ctrl-c')

    # Save as picks in the file's folder, with its name selected to type over.
    at = v.mark()
    v.key('ctrl-shift-s')
    check('Save as opens the picker', v.wait('editor: picking in /tmp', 5, at), v.text(at))
    v.type('copy.txt\n')
    check('and saves under the new name', v.wait('editor: saved /tmp/copy.txt 12 bytes', 5, at)
          and v.wait('editor: title copy.txt - Editor', 5, at), v.text(at))

    # In the picker, typing a folder goes there. Its list starts 142 down, with
    # folders first, then files by name: copy.txt, hello.txt.
    at = v.mark()
    v.key('ctrl-o')
    check('Open opens the picker', v.wait('editor: picking in /tmp', 5, at), v.text(at))
    v.type('/system/etc\n')
    check('typing a folder goes there', v.wait('editor: picking in /system/etc', 5, at), v.text(at))
    back = v.mark()
    v.type('/tmp\n')
    v.wait('editor: picking in /tmp', 5, back)
    time.sleep(0.5)
    mouse.double_click(112 + 60, 116 + 142 + 2 + 22 + 11)
    check('double-clicking a file opens it', v.wait('editor: opened /tmp/hello.txt 12 bytes', 5, at), v.text(at))

    # Saving over a file that's there asks first.
    at = v.mark()
    v.key('ctrl-shift-s'); time.sleep(0.5); v.key('ret'); time.sleep(0.5)
    check('saving over a file asks first', 'editor: saved' not in v.text(at), v.text(at))
    v.key('ret')
    check('and replaces it when asked again', v.wait('editor: saved /tmp/hello.txt 12 bytes', 5, at), v.text(at))
    at = v.mark()
    v.key('ctrl-o'); time.sleep(0.5); v.key('esc')
    check('Escape cancels picking', v.wait('editor: picking cancelled', 5, at), v.text(at))

    # With changes, Open and closing ask first.
    at = v.mark()
    mouse.click(112 + 300, text_line)
    v.type('!')
    v.key('ctrl-o'); time.sleep(0.5)
    check('Open with changes asks first', 'editor: picking in' not in v.text(at), v.text(at))
    v.key('meta_l-q'); time.sleep(1)
    check("closing with changes asks first", 'editor: closed' not in v.text(at), v.text(at))
    v.key('meta_l-q')
    check('and closing again closes', v.wait('editor: closed', 5, at), v.text(at))
    time.sleep(0.5)

    # Back in files: right-clicking where there's no item has New folder first,
    # which asks for a name. Menus open at the mouse, with rows 22 tall from 4 down,
    # and a line 9 tall between groups.
    def menu(x, y, row, lines=0):
        mouse.to(x, y, 2); v.mouse(buttons=0)
        mouse.click(x + 40, y + 4 + row * 22 + lines * 9 + 11)

    empty = (80 + x + 200, 84 + y + 150)
    at = v.mark()
    menu(*empty, 0)
    v.type('docs\n')
    check('New folder', v.wait('files: made /tmp/docs', 5, at) and v.wait('files: in /tmp: docs/ copy.txt hello.txt', 5, at),
          v.text(at))

    # A folder's menu: Open, Open in new tab, a line, Cut, Copy, Copy path, Rename
    # and Delete, which asks first, with the keyboard on its Delete button.
    at = v.mark()
    menu(80 + x + 40, 84 + y + 2 + 11, 6, 1)
    time.sleep(0.5)
    check('Delete asks first', 'files: deleted' not in v.text(at), v.text(at))
    v.key('ret')
    check('and deletes when told to', v.wait('files: deleted /tmp/docs', 5, at), v.text(at))

    # F2 renames hello.txt, second after copy.txt, with its name selected to type over.
    at = v.mark()
    mouse.click(80 + x + 40, 84 + y + 2 + 22 + 11)
    v.key('f2'); time.sleep(0.3)
    v.type('greeting.txt\n')
    check('Rename', v.wait('files: renamed /tmp/hello.txt to /tmp/greeting.txt', 5, at), v.text(at))

    # What the editor copied, pasted as a new folder's name.
    at = v.mark()
    menu(*empty, 0)
    v.key('ctrl-v'); v.key('ret')
    check('pasting from another program', v.wait('files: made /tmp/hello world', 5, at), v.text(at))

    at = v.mark()
    mouse.click(80 + 25, 84 + 41 + 13)
    check('Up goes to the folder above', v.wait('files: in /: ', 5, at), v.text(at))

    v.key('meta_l-shift-q'); time.sleep(1)
    o = v.run('cat /tmp/greeting.txt; ls /tmp')
    check('the saved file, renamed', 'hello world\n' in o and 'hello.txt' not in o and 'copy.txt' in o, o)
finally:
    v.close()


# The wallpaper, icons, and the viewer with an image of each kind: four squares
# of colour, 200x150, which show in the middle of its 240 pixel square window.
QUARTERS = ((230, 40, 50), (40, 200, 70), (50, 80, 230), (250, 210, 40))
picture = Image.new('RGB', (200, 150))
for i, colour in enumerate(QUARTERS):
    picture.paste(colour, ((i % 2) * 100, (i // 2) * 75, (i % 2) * 100 + 100, (i // 2) * 75 + 75))
img = disk_copy('images')
mkdir(img, '/images')
for name, options in (('squares.png', {}), ('squares.jpg', {'quality': 95}), ('squares.bmp', {})):
    picture.save(f'{WORK}/{name}', **options)
    put(img, f'{WORK}/{name}', '/images/' + name)

# What the wallpaper should look like: scaled to cover 1280x800, cut equally.
wall = Image.open(ROOT + '/rootfs/system/share/wallpapers/default.jpg').convert('RGB')
scale = max(1280 / wall.width, 800 / wall.height)
wall = wall.resize((round(wall.width * scale), round(wall.height * scale)), Image.BILINEAR)
wall = wall.crop(((wall.width - 1280) // 2, (wall.height - 800) // 2, (wall.width - 1280) // 2 + 1280,
                  (wall.height - 800) // 2 + 800))


def near(p, want, tolerance):
    return all(abs(a - b) <= tolerance for a, b in zip(channels(p), want))


v = VM(disk=img)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    at = v.mark()
    v.type("wm 'viewer /images/squares.png'\n")
    check('the viewer shows a PNG', v.wait('viewer: showing /images/squares.png 200x150', 10, at), v.text(at))
    time.sleep(1)
    mouse = Pointer(v)
    mouse.to(1270, 790)
    w, h, px = v.screenshot()
    spots = ((1200, 40), (900, 600), (1150, 380), (600, 700))
    check('the wallpaper', all(near(px(x, y), wall.getpixel((x, y)), 12) for x, y in spots),
          [(hex(px(x, y)), wall.getpixel((x, y))) for x, y in spots])

    def squares_shown(tolerance):
        w, h, px = v.screenshot()
        got = [px(100 + (i % 2) * 100 + 50, 129 + (i // 2) * 75 + 37) for i in range(4)]
        return all(near(p, c, tolerance) for p, c in zip(got, QUARTERS)), [hex(p) for p in got]

    ok, detail = squares_shown(0)
    check('with the right colours', ok, detail)
    check("and its icon in the title bar", px(93, 72) != 0x3D63DD, hex(px(93, 72)))
    for name, tolerance in (('squares.jpg', 6), ('squares.bmp', 0)):
        at = v.mark()
        v.key('meta_l-q'); time.sleep(0.5)
        v.key('meta_l-shift-q'); time.sleep(1)
        v.type(f"wm 'viewer /images/{name}'\n")
        check(f'and a {name[-3:].upper()}', v.wait(f'viewer: showing /images/{name} 200x150', 10, at), v.text(at))
        time.sleep(1)
        ok, detail = squares_shown(tolerance)
        check(f'the {name[-3:].upper()} looks the same', ok, detail)

    # The start menu has each app's icon.
    v.key('meta_l'); time.sleep(0.5)
    w, h, px = v.screenshot()
    menu_top = 772 - 16 * 24 - 9 - 8
    icon = [px(4 + 6 + 8, menu_top + 4 + i * 24 + 12) for i in range(3)]
    check('icons in the start menu', all(p not in (0x141821, 0x3D63DD) for p in icon), [hex(p) for p in icon])
    v.key('esc')

    # Files opens a picture in the viewer.
    v.key('meta_l-q'); time.sleep(0.5)
    v.key('meta_l-shift-q'); time.sleep(1)
    at = v.mark()
    v.type("wm 'files /images'\n")
    check('files lists the pictures', v.wait('files: in /images: squares.bmp squares.jpg squares.png', 10, at),
          v.text(at))
    v.wait('files: at list', 5, at)
    time.sleep(1)
    x, y, lw, lh = map(int, re.findall(r'files: at list (\d+) (\d+) (\d+) (\d+)', v.text())[-1])
    w, h, px = v.screenshot()
    check('with picture icons', px(80 + x + 2 + 5 + 8, 84 + y + 2 + 11) not in (0x181B21, 0x3D63DD),
          hex(px(80 + x + 2 + 5 + 8, 84 + y + 2 + 11)))
    mouse = Pointer(v)
    mouse.double_click(80 + x + 60, 84 + y + 2 + 2 * 22 + 11)
    check('and opens one in the viewer', v.wait('viewer: showing /images/squares.png 200x150', 10, at), v.text(at))
finally:
    v.close()
finish()
