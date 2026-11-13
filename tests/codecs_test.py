"""Checks the codec libraries through convert: files ffmpeg made are read as ffmpeg
reads them, from the start and from part way in, and files written here are read
by ffmpeg as what they were made from."""
import os, subprocess, wave
import numpy as np
from vm import VM, WORK, check, disk_copy, finish, get, put

RATE, SECONDS = 44100, 3
KINDS = ['wav', 'mp3', 'flac', 'ogg', 'opus', 'aac']
LOSSLESS = {'wav', 'flac'}
ENCODE = {'mp3': ['-c:a', 'libmp3lame', '-b:a', '192k'], 'ogg': ['-c:a', 'libvorbis', '-q:a', '5'],
          'opus': ['-c:a', 'libopus', '-b:a', '128k'], 'aac': ['-c:a', 'aac', '-b:a', '192k'], 'flac': [], 'wav': []}


def ffmpeg(*args):
    subprocess.run(['ffmpeg', '-v', 'error', '-y', *args], check=True)


def samples(path):
    with wave.open(path) as w:
        return w.getframerate(), np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, w.getnchannels()).astype(float)


def likeness(a, b, search=3000):
    """How alike two recordings are, from -1 to 1, and how far b is behind a."""
    n = min(len(a), len(b)) - 2 * search
    if n < RATE // 2:
        return 0.0, 0
    x = a[search:search + n, 0]
    lags = range(-search, search + 1)
    lag = max(lags, key=lambda l: np.dot(x, b[search + l:search + l + n, 0]) / (np.linalg.norm(b[search + l:search + l + n, 0]) + 1))
    y = b[search + lag:search + lag + n]
    x = a[search:search + n]
    return min(np.corrcoef(x[:, c], y[:, c])[0, 1] for c in range(2)), lag


# A chord that swells and fades, a different one in each ear.
t = np.arange(RATE * SECONDS) / RATE
swell = np.sin(np.pi * t / SECONDS) ** 0.5
left = swell * (0.30 * np.sin(2 * np.pi * 440 * t) + 0.12 * np.sin(2 * np.pi * 1320 * t))
right = swell * (0.30 * np.sin(2 * np.pi * 660 * t) + 0.10 * np.sin(2 * np.pi * 2000 * t + 1))
source = WORK + '/source.wav'
with wave.open(source, 'wb') as w:
    w.setnchannels(2), w.setsampwidth(2), w.setframerate(RATE)
    w.writeframes((np.stack([left, right], 1) * 32767).astype('<i2').tobytes())

img = disk_copy('codecs')
put(img, source, '/tmp/source.wav')
for kind in KINDS:
    ffmpeg('-i', source, *ENCODE[kind], f'{WORK}/theirs.{kind}')
    put(img, f'{WORK}/theirs.{kind}', f'/tmp/theirs.{kind}')

v = VM(disk=img)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    for kind in KINDS:
        at = v.mark()
        v.type(f'convert -i /tmp/theirs.{kind} && convert /tmp/theirs.{kind} /tmp/read.{kind}.wav && '
               f'convert -s 1.5 /tmp/theirs.{kind} /tmp/seek.{kind}.wav && convert /tmp/source.wav /tmp/ours.{kind}; '
               f'echo done-$?\n')
        check(f'{kind}: converts', v.wait('done-0', 120, at), v.text(at))
        o = v.text(at)
        check(f'{kind}: what it is', 'channels' in o and ' 0 samples' not in o, o)
    v.run('sync', 3)
finally:
    v.close()

for kind in KINDS:
    get(img, f'/tmp/read.{kind}.wav', f'{WORK}/read.{kind}.wav')
    get(img, f'/tmp/seek.{kind}.wav', f'{WORK}/seek.{kind}.wav')
    get(img, f'/tmp/ours.{kind}', f'{WORK}/ours.{kind}')
    ffmpeg('-i', f'{WORK}/theirs.{kind}', f'{WORK}/reference.{kind}.wav')
    rate, reference = samples(f'{WORK}/reference.{kind}.wav')
    _, read = samples(f'{WORK}/read.{kind}.wav')
    _, seek = samples(f'{WORK}/seek.{kind}.wav')
    # ffmpeg keeps the frame its AAC encoder primes with, which faad2 leaves out.
    primed = 1024 if kind == 'aac' else 0
    alike, lag = likeness(reference, read)
    check(f'{kind}: read as ffmpeg reads it', alike > 0.999 and lag == -primed
          and abs(len(read) + primed - len(reference)) < 2048, (alike, lag, len(read), len(reference)))
    if kind in LOSSLESS:
        check(f'{kind}: exactly', len(read) == len(reference) and np.array_equal(read, reference), len(read))
    alike, lag = likeness(read[int(1.5 * rate):], seek)
    check(f'{kind}: and from 1.5 seconds in', alike > 0.999 and lag == 0
          and abs(len(seek) + int(1.5 * rate) - len(read)) <= 1, (alike, lag, len(seek), len(read)))

    # What it wrote, read back by ffmpeg at the rate it came from.
    ffmpeg('-i', f'{WORK}/ours.{kind}', '-ar', str(RATE), f'{WORK}/back.{kind}.wav')
    _, back = samples(f'{WORK}/back.{kind}.wav')
    _, original = samples(source)
    alike, lag = likeness(original, back)
    check(f'{kind}: what it wrote, ffmpeg reads as what it was', alike > (0.9999 if kind in LOSSLESS else 0.98),
          (alike, lag))
finish()
