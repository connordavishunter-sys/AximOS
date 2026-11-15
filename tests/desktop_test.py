"""Checks the calculator, screenshots, the task manager, workspaces and snapping
windows by dragging them to the edges of the screen."""
import re, time
from PIL import Image
from vm import VM, WORK, check, disk_copy, finish, get

DESKTOP, ACCENT = 0x1E2430, 0x3D63DD
TASKBAR_Y = 800 - 28
# The calculator is the first window, its contents at 80, 84: a display, then
# buttons 54 wide and 40 tall, 8 apart, under it.
CALC_X, CALC_Y = 80, 84
button = lambda col, row: (CALC_X + 10 + col * 62 + 27, CALC_Y + 74 + row * 48 + 20)

img = disk_copy('desktop')
v = VM(disk=img, vmmouse=True)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper; rm -f /system/etc/gui.conf /Pictures/*')
    at = v.mark()
    v.type('wm calculator\n')
    check('wm starts', v.wait('wm: running', 10, at), v.text(at))
    time.sleep(1.5)

    def click(x, y):
        v.pointer(x, y, 1); v.pointer(x, y, 0)

    # Typed, and clicked.
    at = v.mark()
    v.type('12+30=')
    check('typing a sum', v.wait('calculator: 42\n', 5, at), v.text(at))
    v.type('7/0\n')
    check('dividing by nothing', v.wait('calculator: Error', 5, at), v.text(at))
    v.key('esc')
    check('Escape clears it', v.wait('calculator: 0\n', 5, at), v.text(at))
    at = v.mark()
    for b in ((2, 1), (3, 1), (2, 3), (3, 4)): # 9 * 3 =
        click(*button(*b))
    check('clicking one', v.wait('calculator: 27\n', 5, at), v.text(at))

    # Screenshots: the screen, and the window, in Pictures.
    v.pointer(700, 500); time.sleep(0.3)
    at = v.mark()
    v.key('print')
    check('Print Screen saves the screen', v.wait('wm: screenshot /Pictures/screenshot-', 10, at)
          and v.wait(' 1280x800', 2, at), v.text(at))
    check('and says so', v.wait('wm: notice Screenshot saved', 5, at), v.text(at))
    shot = re.findall(r'wm: screenshot (\S+) ', v.text(at))[0]
    w, h, px = v.screenshot()
    screen = [px(x, y) for x, y in ((10, 10), (CALC_X + 20, CALC_Y + 30), (*button(0, 0),), (700, 790))]
    at = v.mark()
    v.key('alt-print')
    check('Alt+Print Screen saves the window, with its frame', v.wait('wm: screenshot /Pictures/screenshot-', 10, at)
          and v.wait(' 262x342', 2, at), v.text(at))
    time.sleep(0.5)
    at = v.mark()
    click(1280 - 12 - 150, TASKBAR_Y - 12 - 26)
    check('clicking the notice opens it', v.wait('viewer: showing /Pictures/screenshot-', 10, at)
          and v.wait('262x342', 2, at), v.text(at))
    time.sleep(1)
    v.key('meta_l-q'); time.sleep(0.5)

    # The task manager, from Ctrl+Shift+Escape, ending the calculator.
    at = v.mark()
    v.key('ctrl-shift-esc')
    check('Ctrl+Shift+Escape opens the task manager', v.wait('tasks: at list', 10, at), v.text(at))
    time.sleep(1)
    TASKS_X, TASKS_Y = 80 + 2 * 32, 84 + 2 * 32 # the third window
    where = lambda name: [tuple(map(int, m)) for m in re.findall(rf'tasks: at {name} (\d+) (\d+) (\d+) (\d+)', v.text())][-1]
    x, y, bw, bh = where('sort')
    click(TASKS_X + x + 40, TASKS_Y + y + bh // 2); v.key('down'); v.key('down'); v.key('down'); v.key('ret')
    check('sorting by PID', v.wait('tasks: sort by PID', 5, at), v.text(at))
    x, y, lw, lh = where('list')
    click(TASKS_X + x + 40, TASKS_Y + y + 12)
    for _ in range(60):
        if re.search(r'tasks: selected \d+ \S*calculator', v.text(at)):
            break
        v.key('down'); time.sleep(0.1)
    check('finding the calculator', re.search(r'tasks: selected \d+ \S*calculator', v.text(at)), v.text(at)[-500:])
    x, y, bw, bh = where('end')
    click(TASKS_X + x + 10, TASKS_Y + y + 5)
    check('and ending it', v.wait('tasks: Ended ', 5, at) and 'calculator.' in v.text(at), v.text(at))
    time.sleep(1)
    w, h, px = v.screenshot()
    check('which takes its window away', px(CALC_X + 20, CALC_Y + 30) == DESKTOP, hex(px(CALC_X + 20, CALC_Y + 30)))

    # Workspaces: the task manager sent to the second, which is gone to and back.
    inside = (TASKS_X + 300, TASKS_Y + 300)
    at = v.mark()
    v.key('meta_l-shift-2')
    check('Super+Shift+2 sends the window', v.wait('wm: Task Manager to workspace 2', 5, at), v.text(at))
    time.sleep(0.5)
    w, h, px = v.screenshot()
    check('away from this one', px(*inside) == DESKTOP, hex(px(*inside)))
    v.key('meta_l-2')
    check('Super+2 goes to it', v.wait('wm: workspace 2', 5, at), v.text(at))
    time.sleep(0.5)
    w, h, px = v.screenshot()
    check('where it is', px(*inside) != DESKTOP, hex(px(*inside)))
    check('with its box on the taskbar lit', px(76 + 20 + 2, TASKBAR_Y + 7) == ACCENT, hex(px(76 + 20 + 2, TASKBAR_Y + 7)))
    click(76 + 8, TASKBAR_Y + 14)
    check('clicking the first box goes back', v.wait('wm: workspace 1', 5, at), v.text(at))

    # Snapping: a terminal dragged by its title to the left edge, then the top.
    at = v.mark()
    v.key('meta_l-ret'); time.sleep(2)
    TERM_X, TERM_Y = 80 + 3 * 32, 84 + 3 * 32
    v.pointer(TERM_X + 200, TERM_Y - 12, 1)
    for x in (TERM_X + 100, 300, 0):
        v.pointer(x, 400)
    time.sleep(0.5)
    w, h, px = v.screenshot()
    check('dragging to the left edge shows where it goes', px(1, 300) == ACCENT and px(639, 300) == ACCENT
          and px(645, 300) != ACCENT, (hex(px(1, 300)), hex(px(639, 300)), hex(px(645, 300))))
    v.pointer(0, 400, 0)
    check('and letting go tiles it', v.wait('wm: snapped Terminal left', 5, at), v.text(at))
    time.sleep(1)
    w, h, px = v.screenshot()
    check('on the left half', px(600, 700) != DESKTOP and px(700, 700) == DESKTOP and px(1, 300) != ACCENT,
          (hex(px(600, 700)), hex(px(700, 700))))
    v.pointer(300, 12, 1)
    for x in (600, 1000, 1279):
        v.pointer(x, 400)
    time.sleep(0.5)
    w, h, px = v.screenshot()
    check('dragging to the right edge, the last pixel the host gives, shows where it goes',
          px(1278, 300) == ACCENT and px(641, 300) == ACCENT, (hex(px(1278, 300)), hex(px(641, 300))))
    v.pointer(1279, 400, 0)
    check('and letting go tiles it there', v.wait('wm: snapped Terminal right', 5, at), v.text(at))
    time.sleep(1)
    v.pointer(900, 12, 1)
    for x, y in ((800, 200), (640, 100)):
        v.pointer(x, y)
    time.sleep(0.5)
    w, h, px = v.screenshot()
    check('dragging it off goes back to its size', px(100, 700) == DESKTOP, hex(px(100, 700)))
    v.pointer(640, 0); time.sleep(0.3)
    v.pointer(640, 0, 0)
    check('and to the top maximizes it', v.wait('wm: snapped Terminal maximized', 5, at), v.text(at))
    time.sleep(1)
    w, h, px = v.screenshot()
    check('over the whole desktop', px(5, 700) != DESKTOP and px(1275, 700) != DESKTOP, (hex(px(5, 700)), hex(px(1275, 700))))

    v.key('meta_l-shift-q'); time.sleep(1)
    o = v.run('ls /Pictures')
    check('both screenshots are in Pictures', o.count('screenshot-') == 2, o)
finally:
    v.close()

get(img, shot, f'{WORK}/shot.png')
saved = Image.open(f'{WORK}/shot.png').convert('RGB')
pixels = [int.from_bytes(bytes(saved.getpixel(p)), 'big') for p in ((10, 10), (CALC_X + 20, CALC_Y + 30), button(0, 0), (700, 790))]
check('the PNG has what was on the screen', saved.size == (1280, 800) and pixels == screen,
      (saved.size, [hex(p) for p in pixels], [hex(p) for p in screen]))
finish()
