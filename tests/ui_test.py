"""Checks libgui's widgets through the widgets demo: buttons, checkboxes, sliders and
text boxes, with the mouse and with the keyboard."""
import re, time
from vm import VM, check, finish

# The demo's window contents start at 80, 84, and the mouse in the middle.
LEFT, TOP = 80, 84
BACKGROUND, ACCENT = 0x1E2129, 0x3D63DD


class Pointer:
    def __init__(self, v):
        self.v, self.x, self.y = v, 640, 400

    def to(self, x, y, buttons=None):
        self.v.mouse(x - self.x, y - self.y, buttons)
        self.x, self.y = x, y

    def click(self, x, y):
        self.to(x, y, 1)
        self.v.mouse(buttons=0)


v = VM()
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    at = v.mark()
    v.type('wm widgets\n')
    check('widgets starts', v.wait('widgets: at textbox', 10, at), v.text(at))
    mouse = Pointer(v)

    # Where each widget is now, which changes as widgets above come and go.
    def where(name):
        return [tuple(map(int, m)) for m in re.findall(rf'widgets: at {name} (\d+) (\d+) (\d+) (\d+)', v.text())][-1]

    places = {k: where(k) for k in ('count', 'reset', 'checkbox', 'slider', 'textbox')}
    middle = {k: (LEFT + x + w // 2, TOP + y + h // 2) for k, (x, y, w, h) in places.items()}
    time.sleep(0.5)

    w, h, px = v.screenshot()
    check('window background', px(LEFT + 370, TOP + 290) == BACKGROUND, hex(px(LEFT + 370, TOP + 290)))

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

    # The keyboard alone: Tab from the text box goes round to Count, then on.
    at = v.mark()
    v.key('tab'); v.key('ret'); v.key('spc')
    check('Tab goes round, and Enter and Space press', v.wait('widgets: count 2', 5, at), v.text(at))
    v.key('tab'); v.key('tab'); v.key('spc')
    check('Space toggles a checkbox', v.wait('widgets: progress shown', 5, at), v.text(at))
    v.key('tab'); v.key('right'); v.key('shift-right')
    check('arrows move a slider', v.wait('widgets: volume 11', 5, at), v.text(at))
    v.key('shift-tab'); v.key('shift-tab'); v.key('ret')
    check('Shift+Tab goes back', v.wait('widgets: count 0', 5, at), v.text(at))
    w, h, px = v.screenshot()
    x, y, w_, h_ = places['reset']
    check('the focused widget has a ring', px(LEFT + x - 2, TOP + y + 5) == 0x8FA8F5, hex(px(LEFT + x - 2, TOP + y + 5)))

    # Clicking where there's no widget takes the keyboard away.
    at = v.mark()
    mouse.click(LEFT + 300, TOP + 280)
    v.key('ret'); time.sleep(0.5)
    check('clicking nowhere takes the focus', 'widgets: count' not in v.text(at), v.text(at))

    # Wider, the slider and text box stretch.
    at = v.mark()
    v.key('meta_l-up'); time.sleep(1)
    w, h, px = v.screenshot()
    check('widgets stretch when maximized', px(1200, 25 + places['slider'][1] + 11) != BACKGROUND,
          hex(px(1200, 25 + places['slider'][1] + 11)))
finally:
    v.close()
finish()
