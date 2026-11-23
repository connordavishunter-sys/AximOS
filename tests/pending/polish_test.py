"""Checks the shell's word editing, history search and kept history, the overview of
windows, previews on taskbar buttons, choosing an area to save as a screenshot, and
notifications' buttons, stacking and do not disturb."""
import re, time
from vm import VM, check, finish

W, H = 1280, 800
NOTICE = (W - 300 - 8, H - 8 - 32 - 60) # where the lowest notification starts
BUTTON0 = (250, H - 8 - 16)             # the middle of the first window's taskbar button

v = VM(vmmouse=True)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())

    # Ctrl+W takes away the word before the cursor, and Ctrl+Left moves back a word.
    at = v.mark()
    v.type('echo alpha beta'); v.key('ctrl-w'); v.type('gamma\n')
    check('Ctrl+W deletes a word', v.wait('alpha gamma\n', 5, at), v.text(at))
    at = v.mark()
    v.type('echo one three'); v.key('ctrl-left'); v.type('two '); v.key('ret')
    check('Ctrl+Left moves back a word', v.wait('one two three\n', 5, at), v.text(at))
    at = v.mark()
    v.type('echo zebra'); v.key('alt-b'); v.key('alt-d'); v.type('yak\n')
    check('Alt+B and Alt+D move and delete by words', v.wait('\nyak\n', 5, at), v.text(at))

    # Ctrl+R finds an earlier line by what's in it, and Enter runs it.
    at = v.mark()
    v.key('ctrl-r'); v.type('alp')
    check('Ctrl+R shows the match', v.wait("(search 'alp'): echo alpha gamma", 5, at), v.text(at))
    v.key('ret'); time.sleep(1)
    check('and Enter runs it', v.text(at).count('alpha gamma\n') >= 2, v.text(at))
    o = v.run('cat /home/.history')
    check('history is kept in the home folder', 'echo one two three' in o, o)

    v.run('echo none > /system/etc/wallpaper; echo clock 24 > /system/etc/gui.conf; '
          'echo \'notify -a Act "touch /tmp/acted" Hello there\' > /tmp/n.sh')

    def click(x, y):
        v.pointer(x, y, 1); v.pointer(x, y, 0)

    at = v.mark()
    v.type('wm\n')
    v.wait('wm: running', 10, at); time.sleep(1)

    # A notification's button runs its command.
    at = v.mark()
    v.key('meta_l-r'); v.wait('run: ready', 5, at); time.sleep(0.3)
    v.type('sh /tmp/n.sh\n')
    v.wait('wm: notice Hello: there', 5, at); time.sleep(0.5)
    click(NOTICE[0] + 300 - 12 - 20, NOTICE[1] + 26)
    check('a notification button runs its command', v.wait('wm: notice action Act: touch /tmp/acted', 5, at),
          v.text(at))

    # Two windows show side by side in the overview, and clicking one goes to it.
    for app in ('snake', 'calculator'):
        at = v.mark()
        v.key('meta_l-r'); v.wait('run: ready', 5, at); time.sleep(0.3)
        v.type(app + '\n'); time.sleep(1.5)
    at = v.mark()
    v.key('meta_l-tab')
    check('Super+Tab shows the overview', v.wait('wm: overview shown', 5, at), v.text(at))
    v.wait('wm: overview Calculator at', 5, at)
    thumbs = {t: tuple(map(int, r)) for t, *r in re.findall(r'wm: overview (\w+) at (\d+) (\d+) (\d+) (\d+)', v.text(at))}
    check('with both windows', set(thumbs) == {'Snake', 'Calculator'}, thumbs)
    s = thumbs['Snake']
    check('side by side', s[0] + s[2] <= thumbs['Calculator'][0], thumbs)
    time.sleep(0.5)
    _, _, px = v.screenshot()
    p = px(W // 2, 30)
    check('over the darkened desktop', (p >> 16) + (p >> 8 & 0xFF) + (p & 0xFF) < 3 * 70, hex(p))
    at = v.mark()
    click(s[0] + s[2] // 2, s[1] + s[3] // 2)
    check('clicking one goes to it', v.wait('wm: overview picked Snake', 5, at), v.text(at))
    at = v.mark()
    v.key('meta_l-tab'); v.wait('wm: overview shown', 5, at)
    v.key('esc')
    check('Escape leaves it', v.wait('wm: overview closed', 5, at), v.text(at))

    # Resting on a taskbar button shows its window.
    at = v.mark()
    v.pointer(*BUTTON0); v.pointer(BUTTON0[0] + 2, BUTTON0[1])
    check('resting on a button previews its window', v.wait('wm: preview Snake', 5, at), v.text(at))
    v.pointer(W // 2, 300); time.sleep(0.5)

    # Shift+Print Screen saves the area dragged across, and a click saves a window.
    at = v.mark()
    v.key('shift-print')
    check('Shift+Print Screen chooses an area', v.wait('wm: choosing an area', 5, at), v.text(at))
    v.pointer(100, 100, 1); v.pointer(200, 150); v.pointer(300, 250, 0)
    check('saving what was dragged across', v.wait('.png 201x151', 5, at), v.text(at))
    area = re.findall(r'wm: screenshot (\S+) 201x151', v.text(at))[-1]
    time.sleep(0.5)
    at = v.mark()
    v.key('shift-print'); v.wait('wm: choosing an area', 5, at)
    v.key('esc')
    check('Escape stops choosing', v.wait('wm: screenshot stopped', 5, at), v.text(at))

    # The second screenshot's notice takes the first's place, counting them, and its
    # Show button opens Files at it.
    at = v.mark()
    v.key('print')
    check('another screenshot stacks with the first', v.wait('(2)\n', 5, at)
          and re.search(r'wm: notice Screenshot saved and copied: \S+ \(2\)', v.text(at)), v.text(at))
    name = re.findall(r'wm: screenshot /home/Pictures/(\S+\.png)', v.text(at))[-1]
    click(NOTICE[0] + 300 - 12 - 24, NOTICE[1] + 26)
    check('Show opens Files at it', v.wait('files: in /home/Pictures: ', 10, at) and name in v.text(at), v.text(at))
    time.sleep(1)
    v.key('meta_l-q'); time.sleep(0.5)

    # With do not disturb on, a notification only goes to the tray.
    at = v.mark()
    click(W - 8 - 36 - 12 - 8 - 24 - 2 - 22, H - 8 - 16) # the network and speaker, by a 24 hour clock
    v.wait('wm: tile 3 at', 5, at)
    t = tuple(map(int, re.findall(r'wm: tile 3 at (\d+) (\d+) (\d+) (\d+)', v.text(at))[-1]))
    click(t[0] + 20, t[1] + 20)
    check('the Quiet tile turns on do not disturb', v.wait('wm: quiet on', 5, at), v.text(at))
    v.key('esc'); time.sleep(0.3)
    at = v.mark()
    v.key('meta_l-r'); v.wait('run: ready', 5, at); time.sleep(0.3)
    v.type('notify Shh\n')
    check('then notifications only go to the tray', v.wait('wm: notice Shh:  (quiet)', 5, at), v.text(at))
    v.key('meta_l-shift-q'); time.sleep(1)
    o = v.run('cat /system/etc/gui.conf; ls /tmp')
    check('it is kept in the settings', 'quiet on' in o, o)
    check('and the button ran', 'acted' in o, o)
    o = v.run(f'wc -c {area}')
    check('the area saved is what was on the screen, not blank', int(re.search(r'(\d+) /home', o).group(1)) > 2000, o)
finally:
    v.close()
finish()
