"""Checks the window system: wm draws windows, sends them the mouse and keyboard,
moves, raises and closes them, and logs out to the log-in screen when it quits."""
import time
from vm import VM, check, finish

DESKTOP, FOCUSED, PAPER, INK, TERMINAL = 0x1E2430, 0x3D63DD, 0xF4F1EA, 0x26282E, 0x161A22
TASKBAR, TASKBAR_BUTTON, TASKBAR_MINIMIZED = 0x141821, 0x2E3646, 0x1D222C
# Title bars and the taskbar show a little of what's under them.
near = lambda a, b: all(abs((a >> s & 255) - (b >> s & 255)) < 48 for s in (0, 8, 16))


def count(px, x0, y0, x1, y1, color):
    return sum(px(x, y) == color for x in range(x0, x1) for y in range(y0, y1))


v = VM()
try:
    v.desktop('text sharp', 'none') # the plain colour, default theme and bitmap font, to look for
    o = v.run('GUI_SOCKET=/tmp/none guidemo; echo st=$?'); check('guidemo without wm', "wm isn't running" in o and 'st=1' in o, o)

    at = v.mark()
    v.start('guidemo')
    check('wm starts guidemo', v.wait('guidemo: window 1', 10, at), v.text(at))
    check('new window has focus', v.wait('guidemo: focus 1', 5, at), v.text(at))

    # The first window's contents start at 80, 84, under a title bar from y = 60,
    # once it's finished growing in.
    time.sleep(0.5)
    w, h, px = v.screenshot()
    check('desktop', px(5, 5) == DESKTOP and px(w - 5, h // 2) == DESKTOP, hex(px(5, 5)))
    check('focused title bar', near(px(300, 65), FOCUSED), hex(px(300, 65)))
    check('window contents', px(300, 240) == PAPER, hex(px(300, 240)))
    check('window text', count(px, 100, 110, 400, 128, INK) > 300, count(px, 100, 110, 400, 128, INK))

    # The cursor starts in the middle of the screen.
    at = v.mark()
    v.mouse(200 - w // 2, 200 - h // 2, 1); v.mouse(buttons=0)
    check('click reaches the window', v.wait('guidemo: click 1 at 120,116', 5, at), v.text(at))
    v.keys('k')
    check('keys reach the window', v.wait('guidemo: key k', 5, at), v.text(at))
    w, h, px = v.screenshot()
    check('window redraws', count(px, 100, 162, 300, 180, INK) > count(px, 100, 162, 300, 180, PAPER) // 20,
          'clicks: 1 not drawn')

    # Dragging the title bar by 300, 200 moves the window.
    v.mouse(0, -130, 1); v.mouse(300, 200); v.mouse(buttons=0)
    w, h, px = v.screenshot()
    check('drag moves the window', px(300, 65) == DESKTOP and near(px(600, 265), FOCUSED),
          f'{px(300, 65):x} {px(600, 265):x}')

    # A terminal takes the focus, and Super+Q closes it.
    at = v.mark()
    v.key('meta_l-ret')
    check('Super+Enter opens a terminal, taking the focus', v.wait('guidemo: focus 0', 10, at), v.text(at))
    time.sleep(1)
    w, h, px = v.screenshot()
    check('the terminal opens on top, a little see-through', near(px(600, 265), TERMINAL), hex(px(600, 265)))
    at = v.mark()
    v.key('meta_l-q')
    check('Super+Q closes the focused window, and focus goes back', v.wait('guidemo: focus 1', 5, at), v.text(at))
    time.sleep(0.5)
    w, h, px = v.screenshot()
    check('the title bar shows the focus again', near(px(600, 265), FOCUSED), hex(px(600, 265)))

    # The close button is at the right of the title bar, now from 720, 264, and the
    # cursor is at 500, 270 after the drag.
    at = v.mark()
    v.mouse(228, 2, 1); v.mouse(buttons=0)
    check('close button closes', v.wait('guidemo: closed', 5, at), v.text(at))
    w, h, px = v.screenshot()
    check('closed window is gone', px(600, 265) == DESKTOP, hex(px(600, 265)))

    at = v.mark()
    v.key('meta_l-shift-q')
    check('Super+Shift+Q quits, logging out', v.wait('wm: quit', 5, at) and v.wait('session: tester logged out', 5, at),
          v.text(at))
    check('to the log-in screen', v.wait('login: log in', 10, at), v.text(at))
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
    v.desktop('text sharp', 'none') # the plain colour, default theme and bitmap font, to look for
    at = v.mark()
    v.start('guidemo')
    check('wm starts guidemo', v.wait('guidemo: window 1', 10, at), v.text(at))
    time.sleep(0.5)
    mouse = Pointer(v)

    w, h, px = v.screenshot()
    bar = h - 8 - 32 # floating 8 above the bottom
    check('taskbar', near(px(10, bar + 2), TASKBAR) and px(16, bar + 6) == TASKBAR_BUTTON, hex(px(10, bar + 2)))
    check('taskbar button for the focused window', px(248, bar + 6) == FOCUSED, hex(px(248, bar + 6)))
    check('clock', count(px, w - 68, bar + 10, w - 18, bar + 18, 0xFFFFFF) > 20, 'no clock')

    # The 360x200 contents end at 440, 284, and the frame's border just past them.
    at = v.mark()
    mouse.to(442, 286, 1); mouse.to(562, 366); v.mouse(buttons=0)
    check('dragging the corner resizes', v.wait('guidemo: size 480 280', 5, at), v.text(at))
    w, h, px = v.screenshot()
    check('the window is bigger', px(550, 350) == PAPER, hex(px(550, 350)))

    # The left edge of the frame is at 79, and dragging it leaves the right edge where
    # it was. Then the top edge, at 59 above the title bar.
    at = v.mark()
    mouse.to(76, 200, 1); mouse.to(36, 200); v.mouse(buttons=0)
    check('dragging the left edge resizes', v.wait('guidemo: size 520 280', 5, at), v.text(at))
    time.sleep(0.5)
    w, h, px = v.screenshot()
    check('and moves the window left', px(50, 200) == PAPER and px(558, 200) == PAPER and px(562, 200) != PAPER,
          f'{px(50, 200):x} {px(558, 200):x} {px(562, 200):x}')
    at = v.mark()
    mouse.to(300, 56, 1); mouse.to(300, 26); v.mouse(buttons=0)
    check('dragging the top edge resizes', v.wait('guidemo: size 520 310', 5, at), v.text(at))
    time.sleep(0.5)
    w, h, px = v.screenshot()
    check('and moves the window up', near(px(400, 32), FOCUSED) and px(300, 360) == PAPER,
          f'{px(400, 32):x} {px(300, 360):x}')

    # Maximize is the middle button, left of close.
    at = v.mark()
    mouse.click(528, 42)
    check('maximize fills the desktop', v.wait('guidemo: size 1278 726', 5, at), v.text(at))
    w, h, px = v.screenshot()
    check('maximized', px(1200, 700) == PAPER and near(px(1200, bar + 2), TASKBAR), hex(px(1200, 700)))
    at = v.mark()
    v.key('meta_l-down')
    check('Super+Down goes back', v.wait('guidemo: size 520 310', 5, at), v.text(at))
    at = v.mark()
    v.key('meta_l-right')
    check('Super+Right takes the right half', v.wait('guidemo: size 638 726', 5, at), v.text(at))
    time.sleep(0.5) # once it's finished moving there
    w, h, px = v.screenshot()
    check('on the right half', px(1000, 400) == PAPER and px(300, 400) == DESKTOP, (hex(px(1000, 400)), hex(px(300, 400))))

    # Minimize is the third button from the right, now at the top right of the screen.
    at = v.mark()
    mouse.click(1227, 13)
    check('minimizing takes the focus away', v.wait('guidemo: focus 0', 5, at), v.text(at))
    w, h, px = v.screenshot()
    check('minimized window is hidden', px(1000, 400) == DESKTOP, hex(px(1000, 400)))
    check('and dim on the taskbar', px(240, bar + 6) == TASKBAR_MINIMIZED, hex(px(240, bar + 6)))
    at = v.mark()
    mouse.click(240, bar + 6)
    check('clicking it on the taskbar brings it back', v.wait('guidemo: focus 1', 5, at), v.text(at))
    w, h, px = v.screenshot()
    check('shown again', px(1000, 400) == PAPER, hex(px(1000, 400)))

    # The menu lists /system/bin/gui above the launcher, 24 pixels an item: its
    # folders, Accessories, Demos, Games and System, then the apps outside them, files
    # and term. Then a line, Run..., Lock, Log out, Reboot and Shut down. It floats 8
    # above the taskbar.
    menu_top = bar - 11 * 24 - 9 - 8 - 8
    mouse.click(36, bar + 14)
    time.sleep(0.5)
    w, h, px = v.screenshot()
    check('the launcher opens the menu', px(150, menu_top + 10) == FOCUSED, hex(px(150, menu_top + 10)))
    at = v.mark()
    mouse.click(100, menu_top + 4 + 5 * 24 + 12)
    check('clicking term in the menu opens a terminal', v.wait('guidemo: focus 0', 10, at), v.text(at))
    time.sleep(1)
    w, h, px = v.screenshot()
    check('which gets a taskbar button', px(400, bar + 6) == FOCUSED, hex(px(400, bar + 6)))
    check('and the menu closes', px(150, menu_top + 10) != FOCUSED, hex(px(150, menu_top + 10)))

    v.key('meta_l')
    time.sleep(0.5)
    w, h, px = v.screenshot()
    check('Super on its own opens the menu', px(150, menu_top + 10) == FOCUSED, hex(px(150, menu_top + 10)))
    v.key('esc')
    time.sleep(0.5)
    w, h, px = v.screenshot()
    check('Escape closes it', px(150, menu_top + 10) != FOCUSED, hex(px(150, menu_top + 10)))

    at = v.mark()
    v.key('alt-tab')
    check('Alt+Tab switches to the window under', v.wait('guidemo: focus 1', 5, at), v.text(at))
    at = v.mark()
    v.key('alt-tab')
    check('and again switches back', v.wait('guidemo: focus 0', 5, at), v.text(at))
    at = v.mark()
    v.key('alt-shift-tab')
    check('Alt+Shift+Tab goes the other way', v.wait('guidemo: focus 1', 5, at), v.text(at))
    w, h, px = v.screenshot()
    check('the switched to window comes to the top', px(1000, 400) == PAPER, hex(px(1000, 400)))

    # Up from the first item goes round to the last, Shut down.
    v.key('meta_l'); time.sleep(0.3); v.key('up'); v.key('ret')
    check('Shut down in the menu turns the computer off', v.exited(15), v.text()[-300:])
finally:
    v.close()
finish()
