"""Checks the sound card plays what's written to /dev/audio: a tone in each ear,
at two rates, with nothing after it, recorded from QEMU's side."""
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
finally:
    v.close()
finish()
