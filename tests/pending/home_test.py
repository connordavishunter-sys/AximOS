"""Checks the home folder, the desktop's icons for what's in its Desktop folder,
which open, drag to windows and take files dropped from Files, and the calendar
from the clock."""
import datetime, re, time
from vm import VM, check, finish

DESKTOP, ACCENT = 0x1E2430, 0x3D63DD
MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October',
          'November', 'December']
# Desktop icons go down from the top left in cells 88 by 72, 8 in.
cell = lambda i: (8 + 42, 8 + i * 72 + 34)
# The clock, 5 letters wide, ends 12 from the right. Its calendar is 248 by 208,
# floating 8 above the taskbar, 32 tall, and 8 from the right edge, with the next month's arrow, 24
# square, 8 in from its top right corner and 6 down.
CLOCK = (1280 - 8 - 12 - 20, 800 - 14)
PANEL = (1280 - 10 - 248, 800 - 8 - 32 - 8 - 208)
NEXT = (PANEL[0] + 248 - 8 - 12, PANEL[1] + 6 + 12)

v = VM(vmmouse=True)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    o = v.run('echo $HOME; ls /home')
    check('the home folder is /home', '\n/home\n' in o and all(d in o for d in ('Desktop', 'Documents', 'Pictures')), o)
    v.run('echo none > /system/etc/wallpaper; rm -f /system/etc/gui.conf')
    v.run('echo hello > /home/Desktop/note.txt; cp /system/share/icons/apps/paint.png /home/Desktop/pic.png')

    def click(x, y):
        v.pointer(x, y, 1); v.pointer(x, y, 0)

    def drag(a, b):
        v.pointer(*a, 1); time.sleep(0.2)
        for n in range(1, 6):
            v.pointer(a[0] + (b[0] - a[0]) * n // 5, a[1] + (b[1] - a[1]) * n // 5, 1); time.sleep(0.1)
        v.pointer(*b, 0)

    def run(command, ready):
        at = v.mark()
        v.key('meta_l-r'); v.wait('run: ready', 5, at)
        v.type(command + '\n')
        ok = v.wait(ready, 10, at)
        time.sleep(1)
        return ok

    at = v.mark()
    v.type('wm\n')
    check('the desktop shows what is in Desktop', v.wait('wm: desktop: note.txt pic.png', 10, at), v.text(at))
    time.sleep(1)
    w, h, px = v.screenshot()
    check('as icons', px(*cell(1)) != DESKTOP and px(*cell(0)) != DESKTOP, hex(px(*cell(1))))

    at = v.mark()
    click(*cell(0)); time.sleep(0.5)
    w, h, px = v.screenshot()
    check('a click selects one', px(cell(0)[0] - 36, cell(0)[1]) != DESKTOP, hex(px(cell(0)[0] - 36, cell(0)[1])))
    v.pointer(*cell(0))
    for b in (1, 0, 1, 0):
        v.command(f'mouse_button {b}')
    check('a double click opens it', v.wait('wm: opening /home/Desktop/note.txt in editor', 5, at)
          and v.wait('editor: opened /home/Desktop/note.txt', 10, at), v.text(at))
    time.sleep(1)
    v.key('meta_l-q'); time.sleep(0.5)

    check('the viewer opens, in the home folder', run('viewer', 'run: running viewer'), v.text())
    v.key('meta_l-right'); time.sleep(0.5)
    at = v.mark()
    drag(cell(1), (960, 400))
    check('an icon drags to a window', v.wait('wm: dragging /home/Desktop/pic.png', 5, at)
          and v.wait('viewer: showing /home/Desktop/pic.png', 5, at), v.text(at))
    v.key('meta_l-q'); time.sleep(0.5)

    check('Files starts in the home folder', run('files', 'files: in /home: Desktop/ Documents/ Pictures/'), v.text())
    at = v.mark()
    v.key('meta_l-left'); time.sleep(0.5)
    x, y = map(int, re.findall(r'files: at address (\d+) (\d+)', v.text())[-1])
    click(1 + x + 100, 25 + y + 10); v.key('ctrl-a'); v.type('/system/share/icons/apps\n')
    check('going elsewhere', v.wait('files: in /system/share/icons/apps: calculator.png', 5, at), v.text(at))
    time.sleep(0.5)
    lx, ly = map(int, re.findall(r'files: at list (\d+) (\d+)', v.text())[-1])
    at = v.mark()
    drag((1 + lx + 60, 25 + ly + 2 + 11), (900, 300))
    check('a file dropped on the desktop is copied there', v.wait('wm: copied /system/share/icons/apps/calculator.png to the desktop', 5, at)
          and v.wait('wm: desktop: calculator.png note.txt pic.png', 5, at), v.text(at))
    v.key('meta_l-q'); time.sleep(0.5)

    # The calendar opens on this month, and goes to the next.
    today = datetime.date.today()
    next_month = (today.replace(day=1) + datetime.timedelta(days=32))
    at = v.mark()
    click(*CLOCK)
    check('the clock opens a calendar', v.wait(f'wm: calendar {MONTHS[today.month - 1]} {today.year}', 5, at), v.text(at))
    time.sleep(0.5)
    w, h, px = v.screenshot()
    accent = sum(px(x, y) == ACCENT for x in range(PANEL[0], PANEL[0] + 248, 2) for y in range(PANEL[1] + 60, PANEL[1] + 208, 2))
    check('with today marked', accent > 20, accent)
    at = v.mark()
    click(*NEXT)
    check('and goes to the next month', v.wait(f'wm: calendar {MONTHS[next_month.month - 1]} {next_month.year}', 5, at),
          v.text(at))
    v.key('esc'); time.sleep(0.5)
    w, h, px = v.screenshot()
    check('Escape closes it', px(PANEL[0] + 120, PANEL[1] + 100) == DESKTOP, hex(px(PANEL[0] + 120, PANEL[1] + 100)))
    v.key('meta_l-shift-q'); time.sleep(1)
    o = v.run('ls /home/Desktop')
    check('the copy is in Desktop', 'calculator.png' in o, o)
finally:
    v.close()
finish()
