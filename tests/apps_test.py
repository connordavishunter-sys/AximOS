"""Checks the file manager and text editor together, the mouse cursor's shapes, and
window shadows."""
import re, time
from vm import VM, check, finish

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
    v.run('echo hello > /tmp/hello.txt')
    o = v.run('ls /system/bin/gui'); check('editor and files are apps', 'editor' in o and 'files' in o, o)

    at = v.mark()
    v.type("wm 'files /tmp'\n")
    check('files lists the folder', v.wait('files: in /tmp: hello.txt', 10, at), v.text(at))
    v.wait('files: list at', 5, at)
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
    mouse.to(80 + 300, 84 + 37 + 13)
    check('an I-beam over a text box', mouse.shape() == WHITE, hex(mouse.shape()))
    mouse.to(80 + 30, 84 + 37 + 13)
    check('back to an arrow over a button', mouse.shape() == BLACK, hex(mouse.shape()))
    mouse.to(643, 300)
    check('a resize arrow on the right edge', mouse.shape() == WHITE, hex(mouse.shape()))
    mouse.to(643, 527)
    check('and on the corner', mouse.shape() == WHITE, hex(mouse.shape()))

    # Double-clicking a file opens it in the editor.
    x, y, lw, lh = map(int, re.findall(r'files: list at (\d+) (\d+) (\d+) (\d+)', v.text())[-1])
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

    # Back in files: a new folder, deleted with two presses. The buttons are Open,
    # Run, Delete, Rename and New folder, then the name box.
    at = v.mark()
    mouse.click(80 + 450, 84 + 71 + 13)
    v.type('docs')
    mouse.click(80 + 297, 84 + 71 + 13)
    check('New folder', v.wait('files: made /tmp/docs', 5, at) and v.wait('files: in /tmp: docs/ copy.txt hello.txt', 5, at),
          v.text(at))
    mouse.click(80 + 141, 84 + 71 + 13)
    time.sleep(0.5)
    check('Delete asks first', 'files: deleted' not in v.text(at), v.text(at))
    mouse.click(80 + 141, 84 + 71 + 13)
    check('and deletes on the second press', v.wait('files: deleted /tmp/docs', 5, at), v.text(at))

    # hello.txt is second now, after copy.txt.
    at = v.mark()
    mouse.click(80 + x + 40, 84 + y + 2 + 22 + 11)
    mouse.click(80 + 450, 84 + 71 + 13)
    v.type('greeting.txt')
    mouse.click(80 + 211, 84 + 71 + 13)
    check('Rename', v.wait('files: renamed /tmp/hello.txt to /tmp/greeting.txt', 5, at), v.text(at))

    # What the editor copied, pasted into the name box, names a new folder.
    at = v.mark()
    mouse.click(80 + 450, 84 + 71 + 13)
    v.key('ctrl-v')
    mouse.click(80 + 297, 84 + 71 + 13)
    check('pasting from another program', v.wait('files: made /tmp/hello world', 5, at), v.text(at))

    at = v.mark()
    mouse.click(80 + 25, 84 + 37 + 13)
    check('Up goes to the folder above', v.wait('files: in /: ', 5, at), v.text(at))

    v.key('meta_l-shift-q'); time.sleep(1)
    o = v.run('cat /tmp/greeting.txt; ls /tmp')
    check('the saved file, renamed', 'hello world\n' in o and 'hello.txt' not in o and 'copy.txt' in o, o)
finally:
    v.close()
finish()
