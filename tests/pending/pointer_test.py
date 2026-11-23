"""Checks the absolute pointer a virtual machine gives: the cursor goes exactly
where the host's is, even after pushing at the screen's edges, and windows can be
clicked and dragged with it. Without one, the PS/2 mouse moves it as before."""
import re, time
from vm import VM, check, finish

DESKTOP, FOCUSED = 0x1E2430, 0x3D63DD


def wheel(v):
    """What mousetest says scrolling up once gives."""
    v.type('mousetest\n'); time.sleep(0.5)
    at = v.mark()
    v.command('mouse_move 0 0 1'); time.sleep(0.5)
    v.key('ctrl-c'); time.sleep(0.3)
    return v.text(at)

v = VM()
try:
    check('prompt at boot without the VM mouse', v.wait('/$ ', 30), v.text())
    check('only the PS/2 mouse', 'mouse: found' in v.raw() and 'placed by' not in v.raw(),
          [l for l in v.raw().split('\n') if 'mouse' in l])
    o = wheel(v); check('scrolling up is a negative wheel', 'wheel -1 ' in o, o)
finally:
    v.close()

v = VM(vmmouse=True)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    check('the VM places the mouse', 'placed by the virtual machine' in v.raw(),
          [l for l in v.raw().split('\n') if 'mouse' in l])
    o = wheel(v); check('and the same with the VM mouse', 'wheel -1 ' in o, o)
    v.type('mousetest\n'); time.sleep(0.5)
    at = v.mark()
    v.pointer(1279, 799); v.key('ctrl-c'); time.sleep(0.3)
    seen = re.search(r'at x (\d+) y (\d+)', v.text(at))
    check('mousetest sees where it is', seen and min(map(int, seen.groups())) > 65400, v.text(at))
    v.run('echo none > /system/etc/wallpaper; rm -f /system/etc/gui.conf') # the plain colour and default theme
    at = v.mark()
    v.type('wm guidemo\n')
    check('wm starts guidemo', v.wait('guidemo: focus 1', 10, at), v.text(at))

    # The window's contents start at 80, 84, under a title bar from y = 60.
    at = v.mark()
    v.pointer(200, 200, 1); v.pointer(200, 200, 0)
    check('a click lands where the pointer is', v.wait('guidemo: click 1 at 120,116', 5, at), v.text(at))

    # A relative mouse would lose its place against the edges, and this can't.
    at = v.mark()
    for x, y in ((0, 0), (1279, 799), (0, 799), (1279, 0)):
        v.pointer(x, y)
    v.pointer(203, 207, 1); v.pointer(203, 207, 0)
    check('and still does after the edges', v.wait('guidemo: click 2 at 123,123', 5, at), v.text(at))

    v.pointer(200, 70, 1); v.pointer(350, 170); v.pointer(500, 270); v.pointer(500, 270, 0)
    w, h, px = v.screenshot()
    check('dragging the title bar moves the window', px(300, 65) == DESKTOP and abs((px(600, 265) & 255) - (FOCUSED & 255)) < 48,
          f'{px(300, 65):x} {px(600, 265):x}')
finally:
    v.close()
finish()
