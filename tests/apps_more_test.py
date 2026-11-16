"""Checks Paint's selecting, moving, words, filled boxes, redo and asking before
losing changes; the calculator's history; Snake's speeds; the player's shuffle,
repeat and what quick settings say is playing; Settings' default apps; and
rebooting waiting for a program with changes."""
import re, time
from vm import VM, check, finish

X, Y = 1, 25 # a maximized window's contents start here
WHITE = 0xFFFFFF

v = VM(vmmouse=True)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper; rm -f /system/etc/gui.conf /home/.snake-best')

    def click(x, y):
        v.pointer(x, y, 1); v.pointer(x, y, 0)

    def drag(a, b):
        v.pointer(*a, 1)
        for i in range(1, 5):
            v.pointer(a[0] + (b[0] - a[0]) * i // 4, a[1] + (b[1] - a[1]) * i // 4, 1)
        v.pointer(*b, 0)

    def run(command, ready):
        at = v.mark()
        v.key('meta_l-r'); v.wait('run: ready', 5, at)
        v.type(command + '\n')
        ok = v.wait(ready, 10, at)
        time.sleep(1)
        return ok

    at = v.mark()
    v.type('wm paint\n')
    check('Paint starts', v.wait('paint: at picture', 10, at), v.text(at))
    time.sleep(1)
    at = v.mark()
    v.key('meta_l-up'); v.wait('paint: at picture', 5, at); time.sleep(0.5)
    where = lambda name: [tuple(map(int, m)) for m in re.findall(rf'paint: at {name} (-?\d+) (-?\d+) (\d+) (\d+)', v.text())][-1]
    px0, py0, _, _ = where('picture')
    pic = lambda x, y: (X + px0 + x, Y + py0 + y)
    tool = lambda name: (X + where(name)[0] + 10, Y + where(name)[1] + 10)

    # A box, then it selected and moved.
    click(*tool('Box'))
    at = v.mark()
    drag(pic(20, 20), pic(60, 60))
    check('a box', v.wait('paint: Box 20 20 to 60 60', 5, at), v.text(at))
    w, h, px = v.screenshot()
    check('not filled at first', px(*pic(40, 40)) == WHITE, hex(px(*pic(40, 40))))
    click(*tool('Select'))
    at = v.mark()
    drag(pic(10, 10), pic(70, 70))
    check('Select drags out a rectangle', v.wait('paint: selected 10 10 61 61', 5, at), v.text(at))
    drag(pic(40, 40), pic(240, 140))
    check('which moves when dragged', v.wait('paint: moved to 210 110', 5, at), v.text(at))
    v.key('ret'); v.pointer(1200, 780); time.sleep(0.5)
    w, h, px = v.screenshot()
    check('leaving white where it was', px(*pic(20, 40)) == WHITE and px(*pic(220, 140)) != WHITE,
          (hex(px(*pic(20, 40))), hex(px(*pic(220, 140)))))
    at = v.mark()
    v.key('ctrl-z')
    check('undo puts it back', v.wait('paint: undone', 5, at), v.text(at))
    v.key('ctrl-y')
    check('and redo moves it again', v.wait('paint: redone', 5, at), v.text(at))

    # Words go where Text clicks, typed in the bar and put down with Enter.
    click(*tool('Text'))
    at = v.mark()
    click(*pic(300, 50)); time.sleep(0.3)
    v.type('Hi'); v.key('ret')
    check('Text writes words', v.wait('paint: wrote Hi at 300 50', 5, at), v.text(at))

    at = v.mark()
    v.key('ctrl-q')
    check('quitting with changes asks', v.wait('paint: asking about changes', 5, at), v.text(at))
    time.sleep(0.5)
    v.key('esc'); time.sleep(0.5)
    v.key('meta_l-q'); time.sleep(0.5); v.key('right'); v.key('ret'); time.sleep(1)
    w, h, px = v.screenshot()
    check("and Don't save closes it", px(640, 400) == 0x1E2430, hex(px(640, 400)))

    # The calculator keeps each sum, and Ctrl+C copies the answer.
    check('the calculator opens', run('calculator', 'run: running calculator'), v.text())
    at = v.mark()
    v.type('12+30\n')
    check('a sum', v.wait('calculator: 42', 5, at), v.text(at))
    v.type('6*7\n'); v.key('ctrl-c')
    check('Ctrl+C copies the answer', v.wait('calculator: copied 42', 5, at), v.text(at))
    v.key('meta_l-q'); time.sleep(0.5)

    # Snake's speeds, chosen with 1 to 3.
    check('Snake opens', run('snake', 'snake: ready'), v.text())
    at = v.mark()
    v.key('3')
    check('3 is fast', v.wait('snake: speed Fast', 5, at), v.text(at))
    v.key('spc')
    check('starting at that speed', v.wait('snake: start Fast', 5, at), v.text(at))
    v.key('esc')
    check('Escape pauses', v.wait('snake: paused', 5, at), v.text(at))
    v.key('meta_l-q'); time.sleep(0.5)

    # The player repeats the one track.
    at = v.mark()
    check('the player plays', run('player /system/share/example_music/1.wav', 'player: playing'), v.text())
    at = v.mark()
    v.key('meta_l-up'); v.wait('player: at repeat', 5, at); time.sleep(0.5)
    rx, ry, rw, rh = map(int, re.findall(r'player: at repeat (\d+) (\d+) (\d+) (\d+)', v.text())[-1])
    at = v.mark()
    click(X + rx + 20, Y + ry + 10)
    check('Repeat goes to all', v.wait('player: Repeat: all', 5, at), v.text(at))
    click(X + rx + 20, Y + ry + 10)
    check('then to one', v.wait('player: Repeat: one', 5, at), v.text(at))

    # Quick settings, from the network and speaker icons, say what's playing under
    # the volume, a line below the speaker that mutes it.
    quick = (1280 - 8 - 40 - 12 - 8 - 24 - 2 - 22, 800 - 14)
    at = v.mark()
    click(*quick)
    check('quick settings know what is playing', v.wait('wm: quick settings, Playing: 1.wav', 5, at),
          [l for l in v.text().split('\n') if 'player' in l and ' at ' not in l])
    time.sleep(0.5)
    mx, my = map(int, re.findall(r'wm: mute at (\d+) (\d+)', v.text(at))[-1])
    w, h, px = v.screenshot()
    line = [px(x, my + 28 + 8) for x in range(mx, mx + 120)]
    check('and show it', any(p != 0x141821 for p in line), sorted(set(hex(p) for p in line)))
    v.key('esc')
    v.key('meta_l-shift-q'); time.sleep(1)

    # Settings: pictures open in Paint, from the desktop, and the mouse goes faster.
    v.run('cp /system/share/icons/apps/paint.png /home/Desktop/p.png; rm -f /tmp/now-playing')
    at = v.mark()
    v.type('wm settings\n')
    check('Settings opens', v.wait('settings: at theme', 10, at), v.text(at))
    time.sleep(1)
    for name in ('Display', 'Keyboard', 'Date & time', 'Sound', 'Apps'):
        v.key('ctrl-tab'); v.wait(f'settings: tab {name}', 5, at)
    SX, SY = 80, 84
    ax, ay, aw, ah = map(int, re.findall(r'settings: at picture (\d+) (\d+) (\d+) (\d+)', v.text())[-1])
    click(SX + ax + 40, SY + ay + 10); time.sleep(0.3)
    for _ in range(6): # from viewer up past term, tasks, snake, settings and player
        v.key('up')
    v.key('ret')
    check('choosing Paint for pictures', v.wait('settings: app picture paint', 5, at), v.text(at))
    v.key('meta_l-q'); time.sleep(0.5)
    v.pointer(50, 42)
    for b in (1, 0, 1, 0):
        v.command(f'mouse_button {b}')
    check('a picture on the desktop opens in Paint', v.wait('wm: opening /home/Desktop/p.png in paint', 5, at)
          and v.wait('paint: opened /home/Desktop/p.png', 10, at), v.text(at))
    time.sleep(1)

    # Rebooting, like logging out, asks each window to close, and Paint, with
    # changes, stops it. Without logging in, the menu ends Run, Reboot, Shut down.
    at = v.mark()
    v.key('meta_l-up'); v.wait('paint: at picture', 5, at); time.sleep(0.5)
    pxy = re.findall(r'paint: at picture (-?\d+) (-?\d+)', v.text())[-1]
    click(X + int(pxy[0]) + 5, Y + int(pxy[1]) + 5)
    check('drawing on it', v.wait('paint: Pencil', 5, at), v.text(at))
    at = v.mark()
    v.key('meta_l'); time.sleep(0.5)
    v.key('up'); v.key('up'); v.key('ret')
    check('rebooting asks the windows to close', v.wait('wm: asking 1 windows to close', 5, at), v.text(at))
    check('Paint asks about its changes', v.wait('paint: asking about changes', 5, at), v.text(at))
    check('and waits, then gives up', v.wait('wm: notice Not restarted', 5, at), v.text(at))
    v.key('right'); v.key('ret')
    v.key('meta_l-shift-q'); time.sleep(1)
finally:
    v.close()
finish()
