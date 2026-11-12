"""Checks libgui's widgets through the widgets demo: buttons, checkboxes, sliders, text
boxes, dropdowns, lists, menus and scrolling, with the mouse and with the keyboard."""
import re, time
from vm import VM, check, finish

# The demo's window contents start at 80, 84, and the mouse in the middle.
LEFT, TOP = 80, 84
BACKGROUND, ACCENT, PANEL, POPUP, FRAME = 0x1E2129, 0x3D63DD, 0x181B21, 0x2B303B, 0x2B303B
ROW = 22 # in lists, menus and dropdowns


def wheel(v, notches):
    """Turns the wheel towards the user, which scrolls down, or away if negative."""
    for _ in range(abs(notches)):
        v.command(f'mouse_move 0 0 {-1 if notches > 0 else 1}')
        time.sleep(0.3)


class Pointer:
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


v = VM()
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper') # the plain colour, to look for
    at = v.mark()
    v.type('wm widgets\n')
    check('widgets starts', v.wait('widgets: at textbox', 10, at), v.text(at))
    mouse = Pointer(v)

    # Where each widget is now, which changes as widgets above come and go.
    def where(name):
        return [tuple(map(int, m)) for m in re.findall(rf'widgets: at {name} (\d+) (\d+) (\d+) (\d+)', v.text())][-1]

    time.sleep(0.5)
    places = {k: where(k) for k in ('count', 'reset', 'checkbox', 'slider', 'textbox', 'dropdown')}
    middle = {k: (LEFT + x + w // 2, TOP + y + h // 2) for k, (x, y, w, h) in places.items()}
    time.sleep(0.5)

    w, h, px = v.screenshot()
    empty = (LEFT + 300, TOP + places['count'][1] + 5) # right of the buttons
    check('scrolling area background', px(*empty) == PANEL, hex(px(*empty)))
    check('menu bar', px(LEFT + 300, TOP + 10) == FRAME, hex(px(LEFT + 300, TOP + 10)))

    at = v.mark()
    mouse.click(*middle['count']); mouse.click(*middle['count'])
    check('button', v.wait('widgets: count 2', 5, at), v.text(at))
    mouse.click(*middle['reset'])
    check('another button', v.wait('widgets: count 0', 5, at), v.text(at))

    # A press that's let go away from the button doesn't count.
    at = v.mark()
    mouse.to(*middle['count'], 1); mouse.to(LEFT + 300, TOP + 280); v.mouse(buttons=0)
    time.sleep(0.5)
    check('letting go elsewhere cancels', 'widgets: count' not in v.text(at), v.text(at))

    at = v.mark()
    x, y, w_, h_ = places['checkbox']
    mouse.click(LEFT + x + 5, TOP + y + 5)
    check('checkbox', v.wait('widgets: progress hidden', 5, at), v.text(at))

    # Three quarters along the slider's frame, which is all but its label.
    x, y, w_, h_ = places['slider']
    frame = w_ - len('Volume') * 8 - 8
    at = v.mark()
    mouse.click(LEFT + x + 5 + (frame - 10) * 3 // 4, TOP + y + h_ // 2)
    check('slider', v.wait('widgets: volume 75', 5, at), v.text(at))
    at = v.mark()
    mouse.to(LEFT + x + 5, TOP + y + h_ // 2, 1); mouse.to(LEFT + x - 40, TOP + y + h_ // 2); v.mouse(buttons=0)
    check('dragging a slider past its end stops at the end', v.wait('widgets: volume 0', 5, at), v.text(at))

    at = v.mark()
    x, y, w_, h_ = where('textbox')
    check('hiding the progress bar moves what was under it up', y == places['textbox'][1] - places['slider'][3] - 8, (y, places['textbox']))
    mouse.click(LEFT + x + 20, TOP + y + h_ // 2)
    v.type('Grace'); v.key('backspace'); v.key('home'); v.type('X'); v.key('end'); v.type('!')
    check('text box', v.wait('widgets: name XGrac!', 5, at), v.text(at))

    # Ctrl+A selects all of it, Ctrl+X cuts it and Ctrl+V puts it back. Shift with
    # Home selects to the start, and typing replaces what's selected.
    at = v.mark()
    v.key('ctrl-a'); v.key('ctrl-x')
    check('cut', v.wait('widgets: name \n', 5, at), v.text(at))
    v.key('ctrl-v')
    check('and paste', v.wait('widgets: name XGrac!\n', 5, at), v.text(at))
    v.key('left'); v.key('shift-home'); v.type('Z')
    check('typing over a selection', v.wait('widgets: name Z!\n', 5, at), v.text(at))

    # The keyboard alone: Tab from the text box goes past the dropdowns, list and
    # notes round to Count, then on, scrolling the page to show where it is.
    at = v.mark()
    for _ in range(5):
        v.key('tab')
    v.key('ret'); v.key('spc')
    check('Tab goes round, and Enter and Space press', v.wait('widgets: count 2', 5, at), v.text(at))
    v.key('tab'); v.key('tab'); v.key('spc')
    check('Space toggles a checkbox', v.wait('widgets: progress shown', 5, at), v.text(at))
    v.key('tab'); v.key('right'); v.key('shift-right')
    check('arrows move a slider', v.wait('widgets: volume 11', 5, at), v.text(at))
    v.key('shift-tab'); v.key('shift-tab'); v.key('ret')
    check('Shift+Tab goes back', v.wait('widgets: count 0', 5, at), v.text(at))
    w, h, px = v.screenshot()
    x, y, w_, h_ = where('reset')
    check('the focused widget has a ring', px(LEFT + x - 2, TOP + y + 5) == 0x8FA8F5, hex(px(LEFT + x - 2, TOP + y + 5)))

    # Clicking where there's no widget, like the empty end of the menu bar, takes
    # the keyboard away.
    at = v.mark()
    mouse.click(LEFT + 300, TOP + 13)
    v.key('ret'); time.sleep(0.5)
    check('clicking nowhere takes the focus', 'widgets: count' not in v.text(at), v.text(at))

    # The wheel scrolls the page, three rows a notch.
    before = where('dropdown')[1]
    mouse.to(LEFT + 300, TOP + 200)
    wheel(v, 1); time.sleep(0.5)
    check('the wheel scrolls', where('dropdown')[1] == before - 3 * ROW, (before, where('dropdown')))

    # A dropdown's list opens under it, over what's there.
    x, y, w_, h_ = where('dropdown')
    at = v.mark()
    mouse.click(LEFT + x + 50, TOP + y + 13)
    time.sleep(0.5)
    w, h, px = v.screenshot()
    first = TOP + y + h_ + 4 + ROW // 2
    check('a dropdown opens', px(LEFT + x + 200, first + ROW) == POPUP, hex(px(LEFT + x + 200, first + ROW)))
    mouse.click(LEFT + x + 50, first + 2 * ROW)
    check('choosing from it', v.wait('widgets: theme Solarized', 5, at), v.text(at))
    time.sleep(0.5)
    w, h, px = v.screenshot()
    check('closes it', px(LEFT + x + 200, first + ROW) != POPUP, hex(px(LEFT + x + 200, first + ROW)))
    v.key('down')
    check('Down picks the next', v.wait('widgets: theme Nord', 5, at), v.text(at))

    # Rows in the list are chosen with the mouse, and the keys, and it has a wheel of its own.
    x, y, w_, h_ = where('list')
    row = lambda i: TOP + y + 2 + i * ROW + ROW // 2
    at = v.mark()
    mouse.click(LEFT + x + 50, row(1))
    check('clicking a row in a list', v.wait('widgets: fruit Apricot', 5, at), v.text(at))
    v.key('end')
    check('End goes to the last', v.wait('widgets: fruit Plum', 5, at), v.text(at))
    v.key('home')
    check('Home to the first', v.wait('widgets: fruit Apple', 5, at), v.text(at))
    at = v.mark()
    wheel(v, 1); time.sleep(0.5)
    check('the wheel over a list scrolls only the list', 'widgets: at list' not in v.text(at), v.text(at))
    mouse.click(LEFT + x + 50, row(0))
    check('by three rows', v.wait('widgets: fruit Blackberry', 5, at), v.text(at))

    # Notes take several lines, and Up goes to the one above. They're further down.
    mouse.to(LEFT + 300, TOP + 60)
    wheel(v, 2); time.sleep(0.5)
    x, y, w_, h_ = where('notes')
    at = v.mark()
    mouse.click(LEFT + x + 50, TOP + y + 20)
    v.type('ab'); v.key('ret'); v.type('cd'); v.key('up'); v.type('X')
    check('text of several lines', v.wait('widgets: notes abX|cd', 5, at), v.text(at))

    # A double click selects a word, and dragging selects from where it started.
    line = lambda n: TOP + y + 5 + n * 16 + 8
    column = lambda n: LEFT + x + 5 + n * 8
    at = v.mark()
    mouse.double_click(column(1) + 4, line(0))
    v.type('Q')
    check('double-clicking selects a word', v.wait('widgets: notes Q|cd', 5, at), v.text(at))
    mouse.to(column(0) + 1, line(1), 1); mouse.to(column(2) + 1, line(1)); v.mouse(buttons=0)
    v.key('backspace')
    check('dragging selects', v.wait('widgets: notes Q|\n', 5, at), v.text(at))

    at = v.mark()
    mouse.to(LEFT + 300, TOP + 60)
    wheel(v, -3); time.sleep(0.5)
    check('and scrolling back up', where('dropdown')[1] == places['dropdown'][1],
          (where('dropdown'), v.text(at)))

    # Menus open on a press, and moving to another opens that one instead.
    at = v.mark()
    mouse.click(LEFT + 20, TOP + 13)
    time.sleep(0.5)
    w, h, px = v.screenshot()
    check('a menu opens', px(LEFT + 150, TOP + 42) == POPUP, hex(px(LEFT + 150, TOP + 42)))
    v.key('esc'); time.sleep(0.5)
    w, h, px = v.screenshot()
    check('Escape closes it', px(LEFT + 150, TOP + 42) == PANEL, hex(px(LEFT + 150, TOP + 42)))
    mouse.click(LEFT + 20, TOP + 13)
    mouse.click(LEFT + 60, TOP + 31 + ROW // 2)
    check('a menu item', v.wait('widgets: count 0', 5, at), v.text(at))
    mouse.click(LEFT + 20, TOP + 13)
    mouse.to(LEFT + 76, TOP + 13)
    mouse.click(LEFT + 100, TOP + 31 + ROW // 2)
    check("another menu's item", v.wait('widgets: progress hidden', 5, at), v.text(at))

    # A dropdown with more items than fit scrolls. Month's opens over it, where
    # there's more room, eight rows tall.
    x, y, w_, h_ = where('month')
    top = y - (8 * ROW + 8) + 4
    at = v.mark()
    mouse.click(LEFT + x + 50, TOP + y + 13)
    mouse.to(LEFT + x + 50, TOP + top + ROW // 2)
    wheel(v, 1); time.sleep(0.5)
    mouse.click(LEFT + x + 50, TOP + top + ROW // 2)
    check('a long dropdown scrolls', v.wait('widgets: month April', 5, at), v.text(at))
    v.key('ret')
    for _ in range(8):
        v.key('down')
    v.key('ret')
    check('and Down goes on past what was shown', v.wait('widgets: month December', 5, at), v.text(at))
    time.sleep(0.5)
    w, h, px = v.screenshot()
    check('closing with Enter', px(LEFT + x + 200, TOP + top + ROW // 2) != ACCENT, 'still open')

    # Shift+Tab from nowhere goes to the last widget, the notes, scrolling down to them.
    mouse.click(LEFT + 300, TOP + 13)
    v.key('shift-tab'); time.sleep(0.5)
    x, y, w_, h_ = where('notes')
    check('Tab scrolls to what it moves to', y + h_ <= 410, (x, y, w_, h_))

    # F10 opens the first menu, Right the next, and Enter chooses.
    at = v.mark()
    v.key('f10'); time.sleep(0.3)
    w, h, px = v.screenshot()
    check('F10 opens a menu', px(LEFT + 150, TOP + 42) == ACCENT, hex(px(LEFT + 150, TOP + 42)))
    v.key('right'); v.key('ret')
    check('Right goes to the next menu, and Enter chooses', v.wait('widgets: progress shown', 5, at), v.text(at))
    v.key('f10'); v.key('down'); v.key('up'); v.key('ret')
    check('Down and Up move through it', v.wait('widgets: count 0', 5, at), v.text(at))

    # Wider, the slider and text box stretch.
    at = v.mark()
    v.key('meta_l-up'); time.sleep(1)
    w, h, px = v.screenshot()
    check('widgets stretch when maximized', px(1150, 25 + places['slider'][1] + 11) != PANEL,
          hex(px(1150, 25 + places['slider'][1] + 11)))
finally:
    v.close()
finish()
