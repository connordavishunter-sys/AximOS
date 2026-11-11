"""Checks pseudo-terminals, and term: the shell in a window, read off the screen."""
import os, re, time
from vm import VM, WORK, check, compile_program, disk_copy, finish, program_checks, put

P = os.path.dirname(os.path.abspath(__file__)) + '/programs/'
img = disk_copy('term')
put(img, compile_program(P + 'pty.c', WORK + '/pty'), '/system/bin/pty')
put(img, compile_program(P + 'winsize.c', WORK + '/winsize'), '/system/bin/winsize')

# The font's glyphs, to read characters back from the screen.
FONT = {}
source = open(os.path.dirname(P[:-1]) + '/../abi/font8x16.h').read()
for code, glyph in enumerate(re.findall(r'\{((?:\s*0x[0-9A-Fa-f]{2},?){16})\s*\}', source)):
    if 32 <= code < 127:
        FONT.setdefault(tuple(int(b, 16) for b in re.findall(r'0x(\w\w)', glyph)), chr(code))

# The first window's contents start at 80, 84, and term has a 4 pixel margin.
LEFT, TOP, COLS, ROWS = 84, 88, 80, 25


def screen_text(v, shift=0, left=LEFT, top=TOP, rows=ROWS, cols=COLS):
    """The terminal's lines, with anything unreadable as ?. Each window after the
    first opens shift pixels further right and down."""
    w, h, px = v.screenshot()
    lines = []
    for r in range(rows):
        line = ''
        for c in range(cols):
            x0, y0 = left + shift + c * 8, top + shift + r * 16
            samples = [[px(x0 + bit, y0 + row) for bit in range(8)] for row in range(16)]
            background = px(x0 + 7, y0 + 15) # no glyph reaches the bottom right
            glyph = tuple(sum(0x80 >> bit for bit in range(8) if row[bit] != background) for row in samples)
            line += FONT.get(glyph, '?')
        lines.append(line.rstrip())
    return lines


def wait_screen(v, want, secs=5, **where):
    """Waits until the terminal shows want somewhere, returning its lines."""
    deadline = time.time() + secs
    while True:
        lines = screen_text(v, **where)
        if any(want in line for line in lines) or time.time() > deadline:
            return lines
        time.sleep(0.3)


def shown(lines, want):
    return any(want in line for line in lines)


v = VM(disk=img)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    o = v.run('ls /dev'); check('/dev/ptmx exists', 'ptmx' in o, o)
    program_checks(v, 'pty', 'pty: ', 30)

    at = v.mark()
    v.type('wm term\n')
    check('wm starts', v.wait('wm: running', 10, at), v.text(at))
    v.mouse(600, 380) # out of the way, near the bottom right corner
    lines = wait_screen(v, '/$')
    check('term shows the prompt', lines[0].startswith('/$'), '\n'.join(lines))

    v.type('echo only-in-term\n')
    lines = wait_screen(v, 'only-in-term')
    check('output shows in the window', lines[1] == 'only-in-term', '\n'.join(lines))
    check("and not on the console", 'only-in-term\n' not in v.text(at), v.text(at)[-300:])

    v.type('echo a b c | wc -w; echo x > /dev/tty\n')
    lines = wait_screen(v, 'x')
    check('pipes, and /dev/tty is the window', lines[3].strip() == '3' and lines[4] == 'x', '\n'.join(lines))

    v.type('sleep 30\n'); time.sleep(1); v.key('ctrl-c'); time.sleep(0.5)
    v.type('echo status $?\n')
    lines = wait_screen(v, 'status')
    check('Ctrl+C interrupts', shown(lines, '^C') and shown(lines, 'status 130'), '\n'.join(lines))

    # Line editing: insert before the last character, then bring it back from history.
    v.type('echo abc'); v.key('left'); v.type('X\n')
    lines = wait_screen(v, 'abXc')
    check('line editing', shown(lines, 'abXc'), '\n'.join(lines))
    v.key('up'); v.key('ret')
    lines = wait_screen(v, 'abXc')
    check('history', sum('abXc' in line for line in lines) >= 4, '\n'.join(lines))

    # top fills the screen, knowing its size from the terminal.
    v.type('top\n'); time.sleep(2.5)
    lines = screen_text(v)
    check('top draws in the window', shown(lines, 'COMMAND') and shown(lines, 'CPU:'), '\n'.join(lines))
    v.key('q'); time.sleep(0.5)
    v.type('clear; echo cleared\n')
    lines = wait_screen(v, 'cleared')
    check('clear', lines[0] == 'cleared' and lines[1].startswith('/$'), '\n'.join(lines))

    # Long output scrolls, leaving the last lines at the bottom.
    v.type('seq=1; while [ $seq -le 40 ]; do echo line$seq; seq=$((seq+1)); done\n')
    lines = wait_screen(v, 'line40')
    check('scrolls', lines[23] == 'line40' and lines[22] == 'line39' and lines[24].startswith('/$'), '\n'.join(lines))

    # What scrolled off since clear is kept: Shift+Page Up goes back a screen less a
    # line, which is all of it, and the wheel goes three lines at a time.
    v.key('shift-pgup'); time.sleep(0.5)
    back = screen_text(v)
    check('Shift+Page Up scrolls back', back[0] == 'cleared' and back[2] == 'line1', '\n'.join(back))
    w, h, px = v.screenshot()
    check('with a thumb in the margin', any(px(LEFT + 80 * 8 + 1, y) == 0x666666 for y in range(TOP, TOP + 400)),
          'no thumb')
    v.mouse(-800, -500)
    v.command('mouse_move 0 0 -1'); time.sleep(0.3)
    v.mouse(800, 500); time.sleep(0.3)
    later = screen_text(v)
    check('the wheel scrolls forward', later[0] == back[3], '\n'.join(later))
    v.key('shift-pgdn'); time.sleep(0.5)
    check('Shift+Page Down to the bottom', screen_text(v) == lines, '\n'.join(screen_text(v)))
    v.key('shift-pgup'); time.sleep(0.3)
    v.type('echo back\n')
    lines = wait_screen(v, 'back')
    check('typing goes back to the bottom', lines[23] == 'back' and 'line1' not in lines, '\n'.join(lines))

    # Maximized, it fills the screen above the taskbar, from 1, 25, and tells the
    # program running in it.
    v.type('clear; winsize -w\n'); time.sleep(1)
    v.key('meta_l-up'); time.sleep(1.5)
    big = dict(left=5, top=29, rows=46, cols=158)
    lines = wait_screen(v, 'rows 46 cols 158', **big)
    check('maximizing resizes the terminal', shown(lines, 'rows 25 cols 80') and shown(lines, 'rows 46 cols 158'),
          '\n'.join(lines))
    v.key('meta_l-down'); time.sleep(1.5)
    lines = wait_screen(v, 'rows 25 cols 80')
    check('and going back', lines[2] == 'rows 25 cols 80' if len(lines) > 2 else False, '\n'.join(lines))
    v.key('ctrl-c'); time.sleep(0.5)

    # Closing the window hangs up what's running in it.
    v.type('sleep 100\n'); time.sleep(0.5)
    v.key('meta_l-q'); time.sleep(1)
    w, h, px = v.screenshot()
    check('Super+Q closes term', px(300, 300) == 0x1E2430, hex(px(300, 300)))

    # exit closes it too.
    v.key('meta_l-ret'); time.sleep(1.5)
    lines = wait_screen(v, '/$', shift=32)
    check('Super+Enter opens term', lines[0].startswith('/$'), '\n'.join(lines))
    v.type('exit\n'); time.sleep(1)
    w, h, px = v.screenshot()
    check('exit closes term', px(300, 300) == 0x1E2430, hex(px(300, 300)))

    v.key('meta_l-shift-q'); time.sleep(1)
    o = v.run('ps')
    check('nothing is left running', 'sleep' not in o and ' sh' not in o.split('ps\n', 1)[-1], o)
finally:
    v.close()
finish()
