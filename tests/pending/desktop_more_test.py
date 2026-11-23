"""Checks the start menu's folders, the desktop's menus, moving its icons on their
grid and deleting one to the trash, the taskbar's menu moving it to the top and
hiding it, quick settings' night light, dark mode and airplane mode, the accent
colour, rounded corners and see-through title bars."""
import re, time
from vm import VM, check, finish

DESKTOP = 0x1E2430
RED = 0xD83B3B

v = VM(vmmouse=True)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper; printf "shadows off\\naccent red\\n" > /system/etc/gui.conf; '
          'rm -f /system/etc/desktop /system/etc/trash.db')
    v.run('cp /system/share/icons/apps/paint.png /home/Desktop/p.png; echo hi > /home/Desktop/note.txt; '
          'mkdir /home/Desktop/box')

    def click(x, y):
        v.pointer(x, y, 1); v.pointer(x, y, 0)

    def right_click(x, y):
        v.pointer(x, y)
        v.command('mouse_button 2'); v.command('mouse_button 0'); time.sleep(0.5)

    def menu(level=0, since=0):
        found = re.findall(rf'wm: menu {level} at (\d+) (\d+) (\d+) (\d+)', v.text(since))
        return tuple(map(int, found[-1])) if found else None

    def item(m, index, lines=0):
        """The middle of a menu's item, after lines separators, 24 and 9 tall."""
        return m[0] + 40, m[1] + 4 + index * 24 + lines * 9 + 12

    at = v.mark()
    v.type('wm calculator\n')
    check('wm starts', v.wait('wm: desktop: box note.txt p.png', 10, at), v.text(at))
    v.wait('calculator: ready', 10, at); time.sleep(1)

    # A window's top left corner is rounded, showing the desktop, and its title bar
    # shows a little of it too, in the accent colour.
    w, h, px = v.screenshot()
    check('the corner is rounded', px(79, 59) == DESKTOP, hex(px(79, 59)))
    # 200 of 256 red, over the desktop.
    title, red_over = px(200, 70), ((RED >> 16) * 200 + (DESKTOP >> 16) * 56) >> 8
    check('the title bar is red, a little see-through', title != RED and abs((title >> 16) - red_over) < 4,
          (hex(title), hex(red_over)))

    # The start menu has the folders in /system/bin/gui: Accessories, Demos, Games,
    # Multimedia and System, then the apps at the top, files and term.
    at = v.mark()
    v.key('meta_l')
    check('the start menu opens', v.wait('wm: menu 0 at', 5, at), v.text(at))
    m = menu(0, at)
    v.pointer(*item(m, 2)); time.sleep(0.5)
    check('Games opens beside it', v.wait('wm: menu 1 at', 5, at), v.text(at))
    games = menu(1, at)
    check('beside the menu', games[0] >= m[0] + m[2] - 4, (m, games))
    at = v.mark()
    v.pointer(games[0] + 20, games[1] + 4 + 12); time.sleep(0.2)
    click(games[0] + 20, games[1] + 4 + 12)
    check('and snake in it starts', v.wait('snake: ready', 10, at), v.text(at))
    time.sleep(0.5)
    v.key('meta_l-q'); time.sleep(0.5)

    # The desktop's own menu hides its icons and shows them again.
    at = v.mark()
    right_click(700, 500)
    m = menu(0, at)
    click(*item(m, 1))
    check('Show icons hides them', v.wait('wm: icons hidden', 5, at), v.text(at))
    time.sleep(0.5)
    w, h, px = v.screenshot()
    check('so the desktop is bare', px(50, 30) == DESKTOP, hex(px(50, 30)))
    at = v.mark()
    right_click(700, 500)
    click(*item(menu(0, at), 1))
    check('and again shows them', v.wait('wm: icons shown', 5, at), v.text(at))

    # Icons are on a grid, 88 by 72 from 8, 8, down each column: box, note.txt, p.png.
    cell = lambda c, r: (8 + c * 88 + 40, 8 + r * 72 + 30)
    at = v.mark()
    v.pointer(*cell(0, 2), 1)
    for i in range(1, 6):
        v.pointer(cell(0, 2)[0] + (cell(8, 6)[0] - cell(0, 2)[0]) * i // 5,
                  cell(0, 2)[1] + (cell(8, 6)[1] - cell(0, 2)[1]) * i // 5, 1)
    v.pointer(*cell(8, 6), 0)
    check('dragging an icon moves it on the grid', v.wait('wm: moved p.png to 8 6', 5, at), v.text(at))

    # Its menu, as in Files: Open, Open in Editor, Open with, a line, Cut, Copy,
    # Copy path, Rename, Delete. Delete puts it in the trash.
    at = v.mark()
    right_click(*cell(0, 1))
    m = menu(0, at)
    v.pointer(*item(m, 2)); time.sleep(0.5)
    check('Open with has the apps', v.wait('wm: menu 1 at', 5, at), v.text(at))
    v.pointer(*item(m, 7, 1)); time.sleep(0.3)
    click(*item(m, 7, 1))
    check('Delete puts it in the trash', v.wait('wm: trashed /home/Desktop/note.txt', 5, at), v.text(at))

    # The taskbar's menu: Position, with Top and Bottom, and Autohide taskbar.
    at = v.mark()
    right_click(700, 800 - 14)
    m = menu(0, at)
    v.pointer(*item(m, 0)); time.sleep(0.5)
    sub = menu(1, at)
    click(sub[0] + 40, sub[1] + 4 + 12)
    check('Position, Top moves the taskbar', v.wait('wm: taskbar top', 5, at), v.text(at))
    time.sleep(0.5)
    w, h, px = v.screenshot()
    check('which is drawn at the top', px(700, 14) != DESKTOP and px(700, 800 - 14) == DESKTOP,
          (hex(px(700, 14)), hex(px(700, 800 - 14))))
    at = v.mark()
    right_click(700, 14)
    click(*item(menu(0, at), 1))
    check('and Autohide hides it', v.wait('wm: autohide on', 5, at), v.text(at))
    v.pointer(700, 400)
    check('once the mouse leaves', v.wait('wm: taskbar hidden', 5, at), v.text(at))
    at = v.mark()
    v.pointer(700, 0)
    check('and shows it at the top edge', v.wait('wm: taskbar shown', 5, at), v.text(at))
    right_click(700, 14)
    click(*item(menu(0, at), 1))
    v.wait('wm: autohide off', 5, at)
    right_click(700, 14)
    v.pointer(*item(menu(0, at), 0)); time.sleep(0.5)
    sub = menu(1, at)
    click(sub[0] + 40, sub[1] + 4 + 24 + 12)
    v.wait('wm: taskbar bottom', 5, at)

    # Quick settings, from the network and the speaker by the bell.
    at = v.mark()
    click(1280 - 8 - 40 - 12 - 8 - 24 - 2 - 22, 800 - 14)
    check('quick settings open', v.wait('wm: quick settings', 5, at), v.text(at))
    tiles = [tuple(map(int, t)) for t in re.findall(r'wm: tile \d at (\d+) (\d+) (\d+) (\d+)', v.text(at))]
    w, h, px = v.screenshot()
    before = px(640, 400)
    click(tiles[1][0] + 20, tiles[1][1] + 20)
    check('Night light turns it on', v.wait('wm: night light on', 5, at), v.text(at))
    time.sleep(0.5)
    w, h, px = v.screenshot()
    after = px(640, 400)
    check('with less blue', (after & 0xFF) < (before & 0xFF) and (after >> 16) == (before >> 16), (hex(before), hex(after)))
    click(tiles[1][0] + 20, tiles[1][1] + 20)
    click(tiles[2][0] + 20, tiles[2][1] + 20)
    check('Dark mode goes light', v.wait('wm: theme light', 5, at), v.text(at))
    click(tiles[2][0] + 20, tiles[2][1] + 20)
    click(tiles[0][0] + 20, tiles[0][1] + 20)
    check('Airplane mode', v.wait('wm: airplane on', 5, at), v.text(at))
    v.key('esc')
    v.key('meta_l-shift-q'); time.sleep(1)
    o = v.run('net; cat /system/etc/desktop /system/etc/trash.db; ls /trash')
    check('the network card goes quiet', 'airplane mode' in o, o)
    check('where icons go is kept', '8 6 p.png' in o, o)
    check('the trash knows where it was', re.search(r'[\w-]{16}\t/home/Desktop/note.txt', o), o)
finally:
    v.close()
finish()
