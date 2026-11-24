"""Checks term is the xterm TERM=xterm-256color says: UTF-8 with wide characters two
cells wide, 256 and true colour, answering where the cursor is, the alternate screen,
and scroll regions with lines inserted, as full-screen programs like Claude Code need."""
import re, time
from vm import VM, check, finish

# Run in term: each step marks itself done in /tmp, then waits for the test to say go on.
SCRIPT = r'''
go() { touch /tmp/step$1; while [ ! -e /tmp/go$1 ]; do sleep 0.2; done; }
printf 'h\303\251llo \342\234\223 \344\275\240\345\245\275x\n'
printf '\033[38;5;196mred\033[0m \033[48;2;0;0;255m  \033[0m\n'
printf '\033_Ga=q,i=1;AAAA\033\\\033P+q544e\033\\'  # graphics and terminfo queries it hasn't, which show nothing
printf '\344\275\240\345\245\275\033[6n'; IFS= read -rsd R pos; printf '%s\n' "${pos#*[}" > /tmp/pos
echo; tput colors > /tmp/colors; echo "$TERM" >> /tmp/colors
go 1
printf '\033[?1049h\033[Hinside'; go 2
printf '\033[?1049l'; go 3
printf '\033[2J\033[H1\n2\n3\n4\n5\033[2;4r\033[2H\033[L\033[r'; go 4
printf '\033[?1000h\033[?1006h'; touch /tmp/step5; IFS= read -rsd M -t 20 m; printf '%s' "${m#*<}" > /tmp/mouse; touch /tmp/step6
sleep 60
'''


def step(v, n):
    """Lets the script go on from the step before, waits for step n, then looks at the screen."""
    if n > 1:
        v.run(f'touch /tmp/go{n - 1}', 0.2)
    ok = wait_for(v, f'/tmp/step{n}')
    time.sleep(0.5)  # for term to draw what came with it
    header, rows = screen(v)
    return ok, header, rows


def wait_for(v, path):
    for _ in range(40):
        if 'yes' in v.run(f'[ -e {path} ] && echo yes', 0.3):
            return True
        time.sleep(0.25)
    return False


def screen(v):
    """term's screen, as it prints it on SIGUSR1: its header, and each row's text by number."""
    at = v.mark()
    v.run('kill -USR1 $(cat /tmp/termpid)', 0.2)
    v.wait('term: row 9 ', 5, at)
    time.sleep(0.3)
    text = v.text(at)
    rows = {int(n): rest.rstrip() for n, rest in re.findall(r'term: row (\d+) ?(.*)', text)}
    header = (re.findall(r'term: screen.*', text) or [''])[0]
    return header, rows


v = VM()
try:
    v.desktop(setup=['echo none > ~/.config/aximos/wallpaper'])
    v.run("cat > /tmp/t.sh <<'EOF'" + SCRIPT + "EOF", 1)
    v.run('term bash /tmp/t.sh & echo $! > /tmp/termpid', 1)
    ok, header, rows = step(v, 1)
    check('the script runs in term', ok, v.text()[-500:])
    check('UTF-8 shows as it is', rows.get(0, '').startswith('h\u00e9llo \u2713 \u4f60\u597dx'), rows)
    out = v.run('cat /tmp/pos /tmp/colors')
    check('wide characters take two cells, as it answers where the cursor is', '3;5\n' in out, out)
    check('terminfo knows it as xterm-256color, with 256 colours', '256' in out and 'xterm-256color' in out, out)
    w, h, px = v.screenshot()
    # The blue cells, the fifth and sixth of the second row, show where the cells are, for the mouse later.
    blue = [(x, y) for y in range(h) for x in range(0, w, 2) if px(x, y) == 0x0000FF]
    xs, ys = [x for x, _ in blue], [y for _, y in blue]
    check('true colour, two cells of it', blue and max(xs) - min(xs) >= 14, len(blue))
    left, top = (min(xs) - 4 * 8, min(ys) - 16) if blue else (84, 88)

    ok, header, rows = step(v, 2)
    check('the alternate screen shows on its own', ok and 'alternate' in header and rows.get(0) == 'inside', (header, rows))
    ok, header, rows = step(v, 3)
    check('and leaving it brings back what was there', 'alternate' not in header and rows.get(0, '').startswith('h\u00e9llo'),
          (header, rows))
    ok, header, rows = step(v, 4)
    check('a line inserted in a scroll region pushes its last out', [rows.get(r) for r in range(5)] == ['1', '', '2', '3', '5'],
          rows)

    v.run('touch /tmp/go4', 0.2)
    wait_for(v, '/tmp/step5')
    v.click(left + 10 * 8 + 4, top + 5 * 16 + 8)  # the 11th cell of the 6th row
    wait_for(v, '/tmp/step6')
    out = v.run('cat /tmp/mouse; echo')
    check('a program asking for the mouse gets clicks, as SGR reports', '0;11;6' in out, out)
finally:
    v.close()
finish()
