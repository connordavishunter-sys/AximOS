"""Checks the media player plays a folder's sounds one after another, pauses with
Space without losing any, and goes to the next with its button, recording what
the sound card plays."""
import re, subprocess, time, wave
import numpy as np
from vm import VM, WORK, check, disk_copy, finish, mkdir, put

X, Y = 80, 84 # where the window's contents start
TRACKS = [('1 one.flac', 440, 3), ('2 two.ogg', 660, 3), ('3 three.opus', 880, 2)] # in order by name

img = disk_copy('player')
mkdir(img, '/music')
for name, pitch, seconds in TRACKS:
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i', f'sine={pitch}:d={seconds}', '-ac', '2',
                    '-ar', '48000', f'{WORK}/{name}'], check=True)
    put(img, f'{WORK}/{name}', f'/music/{name}')


def pitches(path):
    """The pitch heard in each tenth of a second, or 0 for silence."""
    with wave.open(path) as w:
        left = np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, 2)[:, 0].astype(float)
    tenth = 4800
    out = []
    for start in range(0, len(left) - tenth, tenth):
        part = left[start:start + tenth]
        out.append(round(np.argmax(np.abs(np.fft.rfft(part))) * 48000 / tenth) if np.abs(part).max() > 300 else 0)
    return out


v = VM(disk=img)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper; rm -f /system/etc/gui.conf')
    recording = f'{v.dir}/heard.wav'
    v.record(recording)
    at = v.mark()
    v.type("wm \"player '/music/1 one.flac'\"\n")
    check('it starts playing what it was given', v.wait('player: playing /music/1 one.flac', 10, at), v.text(at))
    check('with the others in the folder after it', v.wait('player: ready', 10, at), v.text(at))
    time.sleep(1.5)
    v.key('spc')
    check('Space pauses it', v.wait('player: paused', 5, at), v.text(at))
    time.sleep(1)
    v.key('spc')
    check('and again plays it', v.wait('player: playing again', 5, at), v.text(at))
    check('the first finishes, and the next plays', v.wait('player: playing /music/2 two.ogg', 10, at), v.text(at))
    check('saying so in a notification', v.wait('wm: notice Now playing: 2 two.ogg', 5, at), v.text(at))
    time.sleep(1)
    nx, ny, nw, nh = map(int, re.search(r'player: at next (\d+) (\d+) (\d+) (\d+)', v.text(at)).groups())
    v.mouse(X + nx + nw // 2 - 640, Y + ny + nh // 2 - 400, 1); v.mouse(buttons=0)
    check('Next goes to the third', v.wait('player: playing /music/3 three.opus', 5, at), v.text(at))
    check('which is the last, so it stops after it', v.wait('player: finished 3 three.opus', 10, at), v.text(at))
    time.sleep(0.5)
    v.command('stopcapture 0'); time.sleep(0.5)
    v.key('meta_l-shift-q')
finally:
    v.close()

heard = pitches(recording)
runs = [] # (pitch, tenths) in the order heard
for p in heard:
    if runs and runs[-1][0] == p:
        runs[-1][1] += 1
    else:
        runs.append([p, 1])
sounds = [r for r in runs if r[0]]
# Between tracks there's a little silence. QEMU records nothing once the sound
# card's stopped, so a pause leaves only that much. A tenth that holds the end of
# one and the start of the next can sound like neither, so those are left out.
steady = [r[0] for r in sounds if r[1] > 1]
order = [p for i, p in enumerate(steady) if i == 0 or p != steady[i - 1]]
check('440, then 660 and 880', order == [440, 660, 880], runs)
check('the pause lost nothing of the first', 28 <= sum(r[1] for r in sounds if r[0] == 440) <= 32, runs)
check('the second was cut short by Next', 6 <= sum(r[1] for r in sounds if r[0] == 660) <= 20, runs)
check('the third played through', 19 <= sum(r[1] for r in sounds if r[0] == 880) <= 21, runs)
finish()
