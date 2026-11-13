"""Checks the sound card plays what's written to /dev/audio: a tone in each ear,
at two rates, with nothing after it, two programs' at once, and the volume and
mute, recorded from QEMU's side."""
import os, time, wave
import numpy as np
from vm import VM, WORK, check, compile_program, disk_copy, finish, put

P = os.path.dirname(os.path.abspath(__file__)) + '/programs/'
img = disk_copy('sound')
put(img, compile_program(P + 'tone.c', WORK + '/tone'), '/system/bin/tone')


def heard(path):
    """The recording's left and right, as floats, from where sound starts."""
    with wave.open(path) as w:
        data = np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, 2).astype(float)
    loud = np.nonzero(np.abs(data).max(axis=1) > 1000)[0]
    return data[loud[0]:loud[-1] + 1] if len(loud) else data[:0]


def pitch(x, rate=48000):
    spectrum = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    return np.argmax(spectrum) * rate / len(x)


def pitches(x, rate=48000):
    """The two strongest, highest first."""
    spectrum = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    found = []
    for i in np.argsort(spectrum)[::-1]:
        f = i * rate / len(x)
        if all(abs(f - g) > 50 for g in found):
            found.append(f)
        if len(found) == 2:
            return sorted(found, reverse=True)


def recorded(v, name, command, secs=4):
    path = f'{v.dir}/{name}.wav'
    v.record(path)
    time.sleep(0.3)
    o = v.run(command, secs)
    v.command('stopcapture 0'); time.sleep(0.5)
    return o, heard(path)


v = VM(disk=img)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    check('the sound card is found', 'hda: codec 0, playing through 1 pin' in v.raw(),
          [l for l in v.raw().split('\n') if 'hda' in l])
    for rate in (48000, 44100):
        path = f'{v.dir}/{rate}.wav'
        v.record(path)
        time.sleep(0.3)
        start = time.time()
        o = v.run(f'tone {rate}', 4)
        took = time.time() - start
        v.command('stopcapture 0'); time.sleep(0.5)
        check(f'at {rate}, the program writes it all and finishes', f'tone: wrote {rate * 4}' in o and 'tone: done' in o, o)
        sound = heard(path)
        seconds = len(sound) / 48000
        check(f'a second of it is heard', 0.95 < seconds < 1.05, seconds)
        if len(sound) > 4800:
            middle = sound[len(sound) // 4: 3 * len(sound) // 4]
            check('1000 Hz on the left and 500 on the right', abs(pitch(middle[:, 0]) - 1000) < 10
                  and abs(pitch(middle[:, 1]) - 500) < 10, (pitch(middle[:, 0]), pitch(middle[:, 1])))
            check('loud as written', 11000 < np.abs(middle[:, 0]).max() < 13000, np.abs(middle[:, 0]).max())
            quiet = np.abs(sound[:, 1]) < 300 # 500 Hz crosses zero for 2 samples at most
            runs = np.diff(np.flatnonzero(np.diff(np.r_[0, quiet.astype(int), 0])))[::2]
            check('without gaps', runs.max(initial=0) < 8, runs.max(initial=0))

    # Two at once, one changed to the other's rate, mixed together.
    o, sound = recorded(v, 'both', 'tone 48000 1000 500 & tone 44100 700 300; wait', 4)
    middle = sound[len(sound) // 4: 3 * len(sound) // 4]
    found = (pitches(middle[:, 0]), pitches(middle[:, 1]))
    check('two programs play at once, mixed', len(sound) and all(f and abs(f[0] - a) < 10 and abs(f[1] - b) < 10
          for f, (a, b) in zip(found, ((1000, 700), (500, 300)))), found)

    o = v.run('volume 50')
    check('volume sets it', 'volume 50' in o, o)
    o, sound = recorded(v, 'half', 'tone')
    loudest = np.abs(sound).max(initial=0)
    check('which plays a quarter as loud', 2700 < loudest < 3300, loudest)
    v.run('volume mute')
    o, sound = recorded(v, 'muted', 'tone')
    check('and muting it silences it', np.abs(sound).max(initial=0) == 0, np.abs(sound).max(initial=0))
    o = v.run('volume 100 unmute')
    check('until unmuted', 'volume 100\n' in o, o)
finally:
    v.close()
finish()
