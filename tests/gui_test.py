"""Checks the window system: wm draws windows, sends them the mouse and keyboard,
moves, raises and closes them, and gives the console back when it quits."""
import time
from vm import VM, check, finish

DESKTOP, FOCUSED, PAPER, INK, TERMINAL = 0x1E2430, 0x3D63DD, 0xF4F1EA, 0x26282E, 0x161A22
TASKBAR, TASKBAR_BUTTON, TASKBAR_MINIMIZED = 0x141821, 0x2E3646, 0x1D222C


def count(px, x0, y0, x1, y1, color):
    return sum(px(x, y) == color for x in range(x0, x1) for y in range(y0, y1))


v = VM()
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    o = v.run('ls /dev'); check('/dev/keyboard exists', 'keyboard' in o, o)
    o = v.run('guidemo; echo st=$?'); check('guidemo without wm', "wm isn't running" in o and 'st=1' in o, o)

    at = v.mark()
    v.type("wm guidemo 'cat /dev/keyboard'\n")
    check('wm starts guidemo', v.wait('guidemo: window 1', 10, at), v.text(at))
    check('/dev/keyboard is only for one', v.wait('busy', 5, at), v.text(at))
    check('new window has focus', v.wait('guidemo: focus 1', 5, at), v.text(at))

    # The first window's contents start at 80, 84, under a title bar from y = 60.
    w, h, px = v.screenshot()
    check('desktop', px(5, 5) == DESKTOP and px(w - 5, h // 2) == DESKTOP, hex(px(5, 5)))
    check('focused title bar', px(300, 65) == FOCUSED, hex(px(300, 65)))
    check('window contents', px(300, 260) == PAPER, hex(px(300, 260)))
    check('window text', count(px, 100, 110, 400, 128, INK) > 300, count(px, 100, 110, 400, 128, INK))

    # The cursor starts in the middle of the screen.
    at = v.mark()
    v.mouse(200 - w // 2, 200 - h // 2, 1); v.mouse(buttons=0)
    check('click reaches the window', v.wait('guidemo: click 1 at 120,116', 5, at), v.text(at))
    v.type('k')
    check('keys reach the window', v.wait('guidemo: key k', 5, at), v.text(at))
    w, h, px = v.screenshot()
    check('window redraws', count(px, 100, 162, 300, 180, INK) > count(px, 100, 162, 300, 180, PAPER) // 20,
          'clicks: 1 not drawn')

    # Dragging the title bar by 300, 200 moves the window.
    v.mouse(0, -130, 1); v.mouse(300, 200); v.mouse(buttons=0)
    w, h, px = v.screenshot()
    check('drag moves the window', px(300, 65) == DESKTOP and px(600, 265) == FOCUSED,
          f'{px(300, 65):x} {px(600, 265):x}')

    # A terminal takes the focus, and Super+Q closes it.
    at = v.mark()
    v.key('meta_l-ret')
    check('Super+Enter opens a terminal, taking the focus', v.wait('guidemo: focus 0', 10, at), v.text(at))
    time.sleep(1)
    w, h, px = v.screenshot()
    check('the terminal opens on top', px(600, 265) == TERMINAL, hex(px(600, 265)))
    at = v.mark()
    v.key('meta_l-q')
    check('Super+Q closes the focused window, and focus goes back', v.wait('guidemo: focus 1', 5, at), v.text(at))
    time.sleep(0.5)
    w, h, px = v.screenshot()
    check('the title bar shows the focus again', px(600, 265) == FOCUSED, hex(px(600, 265)))

    # The close button is at the right of the title bar, now from 720, 264, and the
    # cursor is at 500, 270 after the drag.
    at = v.mark()
    v.mouse(228, 2, 1); v.mouse(buttons=0)
    check('close button closes', v.wait('guidemo: closed', 5, at), v.text(at))
    w, h, px = v.screenshot()
    check('closed window is gone', px(600, 265) == DESKTOP, hex(px(600, 265)))

    at = v.mark()
    v.key('meta_l-shift-q')
    check('Super+Shift+Q quits', v.wait('wm: quit', 5, at), v.text(at))
    time.sleep(0.5)
    o = v.run('echo back'); check('the shell has the keyboard again', '\nback\n' in o, o)
    w, h, px = v.screenshot()
    check('the console has the screen again', px(5, 5) != DESKTOP, hex(px(5, 5)))
finally:
    v.close()


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


# Resizing, maximizing, tiling, minimizing and the taskbar.
v = VM()
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    at = v.mark()
    v.type('wm guidemo\n')
    check('wm starts guidemo', v.wait('guidemo: window 1', 10, at), v.text(at))
    time.sleep(0.5)
    mouse = Pointer(v)

    w, h, px = v.screenshot()
    bar = h - 28
    check('taskbar', px(2, bar + 2) == TASKBAR and px(8, bar + 6) == TASKBAR_BUTTON, hex(px(2, bar + 2)))
    check('taskbar button for the focused window', px(150, bar + 6) == FOCUSED, hex(px(150, bar + 6)))
    check('clock', count(px, w - 60, bar + 10, w - 10, bar + 18, 0xFFFFFF) > 20, 'no clock')

    # The 360x200 contents end at 440, 284, and the frame's border just past them.
    at = v.mark()
    mouse.to(442, 286, 1); mouse.to(562, 366); v.mouse(buttons=0)
    check('dragging the corner resizes', v.wait('guidemo: size 480 280', 5, at), v.text(at))
    w, h, px = v.screenshot()
    check('the window is bigger', px(550, 350) == PAPER, hex(px(550, 350)))

    # Maximize is the middle button, left of close.
    at = v.mark()
    mouse.click(528, 72)
    check('maximize fills the desktop', v.wait('guidemo: size 1278 746', 5, at), v.text(at))
    w, h, px = v.screenshot()
    check('maximized', px(1200, 700) == PAPER and px(1200, bar + 2) == TASKBAR, hex(px(1200, 700)))
    at = v.mark()
    v.key('meta_l-down')
    check('Super+Down goes back', v.wait('guidemo: size 480 280', 5, at), v.text(at))
    at = v.mark()
    v.key('meta_l-right')
    check('Super+Right takes the right half', v.wait('guidemo: size 638 746', 5, at), v.text(at))
    w, h, px = v.screenshot()
    check('on the right half', px(1000, 400) == PAPER and px(300, 400) == DESKTOP, hex(px(1000, 400)))

    # Minimize is the third button from the right, now at the top right of the screen.
    at = v.mark()
    mouse.click(1227, 13)
    check('minimizing takes the focus away', v.wait('guidemo: focus 0', 5, at), v.text(at))
    w, h, px = v.screenshot()
    check('minimized window is hidden', px(1000, 400) == DESKTOP, hex(px(1000, 400)))
    check('and dim on the taskbar', px(150, bar + 6) == TASKBAR_MINIMIZED, hex(px(150, bar + 6)))
    at = v.mark()
    mouse.click(150, bar + 6)
    check('clicking it on the taskbar brings it back', v.wait('guidemo: focus 1', 5, at), v.text(at))
    w, h, px = v.screenshot()
    check('shown again', px(1000, 400) == PAPER, hex(px(1000, 400)))

    at = v.mark()
    mouse.click(50, bar + 14)
    check('the launcher opens a terminal', v.wait('guidemo: focus 0', 10, at), v.text(at))
    time.sleep(1)
    w, h, px = v.screenshot()
    check('which gets a taskbar button', px(300, bar + 6) == FOCUSED, hex(px(300, bar + 6)))

    v.key('meta_l-shift-q')
    o = v.run('echo back'); check('quits', '\nback\n' in o, o)
finally:
    v.close()
finish()
