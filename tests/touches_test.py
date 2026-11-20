"""Checks wm's sounds for logging in and notifications, snapping windows to quarters
with the keys and by dragging to a corner, the layouts on the maximize button, and
Files searching the folders inside the one it shows."""
import re, time, wave
import numpy as np
from vm import VM, check, finish

W, H = 1280, 800
X, Y = 1, 25 # where a maximized window's contents start


def pitches(path):
    """The pitches heard, in the order they come, from each twentieth of a second with sound."""
    with wave.open(path) as w:
        left = np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, 2)[:, 0].astype(float)
    out = []
    for start in range(0, len(left) - 2400, 2400):
        part = left[start:start + 2400]
        if np.abs(part).max() > 300:
            p = round(np.argmax(np.abs(np.fft.rfft(part))) * 48000 / 2400)
            if not out or out[-1] != p:
                out.append(p)
    return out


def near(heard, wanted):
    return len(heard) == len(wanted) and all(abs(h - w) <= 20 for h, w in zip(heard, wanted))


v = VM(vmmouse=True)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper; rm -f /system/etc/gui.conf; '
          'mkdir -p /tmp/s/a/b; echo x > /tmp/s/a/b/deep-note.txt; echo x > /tmp/s/top-note.txt; echo x > /tmp/s/other.txt')

    def click(x, y):
        v.pointer(x, y, 1); v.pointer(x, y, 0)

    # Starting is logging in, which has its sound, and a notification has its own.
    recording = f'{v.dir}/heard.wav'
    v.record(recording)
    at = v.mark()
    v.type('wm "sleep 2; notify Hi"\n')
    v.wait('wm: notice Hi', 10, at); time.sleep(1.5)
    v.command('stopcapture 0'); time.sleep(0.5)
    heard = pitches(recording)
    check('logging in sounds C, E and G, and a notification A and E', near(heard, [523, 659, 784, 880, 1318]), heard)

    # Super+Left takes half the screen, then Up its top quarter, Down back to the
    # half, and Down again the bottom quarter.
    at = v.mark()
    v.key('meta_l-r'); v.wait('run: ready', 5, at); time.sleep(0.3)
    v.type('calculator\n'); time.sleep(1.5)
    at = v.mark()
    v.key('meta_l-left'); time.sleep(0.4)
    v.key('meta_l-up')
    check('Super+Up from the left half takes its top quarter', v.wait('wm: snapped Calculator top left', 5, at), v.text(at))
    v.key('meta_l-down')
    check('Super+Down goes back to the half', v.wait('wm: snapped Calculator left', 5, at), v.text(at))
    v.key('meta_l-down')
    check('and again takes the bottom quarter', v.wait('wm: snapped Calculator bottom left', 5, at), v.text(at))
    time.sleep(0.5)

    # Dragged to the right edge near the bottom, it takes the bottom right quarter.
    at = v.mark()
    v.pointer(300, 376 + 12, 1); v.pointer(600, 500); v.pointer(W - 1, H - 100); v.pointer(W - 1, H - 100, 0)
    check('dragging to a corner takes a quarter', v.wait('wm: snapped Calculator bottom right', 5, at), v.text(at))
    time.sleep(0.5)

    # Resting on the maximize button shows layouts, and the middle of thirds puts it there.
    at = v.mark()
    v.pointer(W - 1 - 40 + 8, 376 + 1 + 4 + 8); v.pointer(W - 1 - 40 + 9, 376 + 1 + 4 + 8)
    check('resting on maximize shows layouts', v.wait('wm: layouts for Calculator at', 5, at), v.text(at))
    lx, ly = map(int, re.search(r'wm: layouts for Calculator at (\d+) (\d+)', v.text(at)).groups())
    click(lx + 8 + 2 * 72 + 32, ly + 8 + 20)
    check('clicking a part puts the window there', v.wait('wm: layout Calculator at 426 0 427 752', 5, at), v.text(at))
    v.key('meta_l-q'); time.sleep(0.5)

    # Searching in Files finds what's in the folders inside, and opens it from there.
    at = v.mark()
    v.key('meta_l-r'); v.wait('run: ready', 5, at); time.sleep(0.3)
    v.type('files /tmp/s\n')
    v.wait('files: in /tmp/s:', 10, at); time.sleep(1)
    v.key('meta_l-up'); time.sleep(1)
    lx, ly, lw, lh = map(int, re.findall(r'files: at list (\d+) (\d+) (\d+) (\d+)', v.text(at))[-1])
    at = v.mark()
    v.key('ctrl-f'); v.type('note')
    check('searching finds names in the folders inside', v.wait('files: in /tmp/s: a/b/deep-note.txt top-note.txt\n', 5, at),
          v.text(at))
    click(X + lx + 40, Y + ly + 2 + 11); v.key('ret')
    check('and opens them where they are', v.wait('files: opening /tmp/s/a/b/deep-note.txt in editor', 5, at), v.text(at))
    v.key('meta_l-shift-q'); time.sleep(1)
finally:
    v.close()
finish()
