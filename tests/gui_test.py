"""Checks the window system: wm draws windows, sends them the mouse and keyboard,
moves, raises and closes them, and gives the console back when it quits."""
import time
from vm import VM, check, finish

DESKTOP, FOCUSED, PAPER, INK, TERMINAL = 0x1E2430, 0x3D63DD, 0xF4F1EA, 0x26282E, 0x161A22


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
finish()
