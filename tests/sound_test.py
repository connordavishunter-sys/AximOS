"""Checks audiod plays what programs give it on the sound card: a tone in each ear,
at two rates, without gaps, two programs' at once, mixed, the volume and mute, and
only for root and whoever's at the screen, recorded from QEMU's side."""
import math, re, struct, subprocess, time, wave
import numpy as np
from vm import VM, WORK, check, finish

TONES = {'a48.wav': (48000, 1000, 500), 'a44.wav': (44100, 1000, 500), 'b44.wav': (44100, 700, 300)}


def tone(path, rate, left, right, amplitude=12000):
    """A second of left Hz in the left ear and right Hz in the right."""
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b''.join(struct.pack('<hh', round(amplitude * math.sin(2 * math.pi * left * i / rate)),
                                           round(amplitude * math.sin(2 * math.pi * right * i / rate)))
                               for i in range(rate)))


def heard(path):
    """The recording's left and right, as floats, from where sound starts to where it ends."""
    with wave.open(path) as w:
        data = np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, 2).astype(float)
    loud = np.nonzero(np.abs(data).max(axis=1) > 1000)[0]
    return data[loud[0]:loud[-1] + 1] if len(loud) else data[:0]


def longest_gap(sound):
    """The longest stretch of silence on the right, at 500 Hz, which crosses zero in
    2 samples at most, and where it starts."""
    quiet = np.abs(sound[:, 1]) < 300
    edges = np.flatnonzero(np.diff(np.r_[0, quiet.astype(int), 0]))
    runs = [(edges[i + 1] - edges[i], edges[i]) for i in range(0, len(edges), 2)]
    return max(runs, default=(0, 0))


def pitches(x, count=1, rate=48000):
    """The strongest, highest first."""
    spectrum = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    found = []
    for i in np.argsort(spectrum)[::-1]:
        f = i * rate / len(x)
        if all(abs(f - g) > 50 for g in found):
            found.append(f)
        if len(found) == count:
            return sorted(found, reverse=True)


drive = WORK + '/sounds.img'
subprocess.run(['mkfs.fat', '-C', drive, str(8 * 1024)], check=True, capture_output=True)
for name, (rate, left, right) in TONES.items():
    tone(f'{WORK}/{name}', rate, left, right)
    subprocess.run(['mcopy', '-i', drive, f'{WORK}/{name}', '::/'], check=True)

v = VM(sound=True, extra=['-drive', f'file={drive},format=raw,if=none,id=files', '-device', 'ide-hd,drive=files,bus=ide.1'])
try:
    v.login()
    v.type('sudo sh\n')  # a shell as root, which is heard whoever's at the screen
    v.wait('Password for ', 5)
    v.run('secret')
    v.run('mount /dev/sda /mnt')
    check('audiod finds the sound card', 'audiod: playing' in v.raw(),
          [l for l in v.raw().split('\n') if 'audiod' in l])
    v.run('play /mnt/a48.wav', 3)  # as QEMU's card adds a period of silence the first time it starts

    def recorded(name, command, secs=4):
        """What a command says, and what's heard while it runs."""
        path = f'{v.dir}/{name}.wav'
        v.record(path)
        time.sleep(0.3)
        out = v.run(command, secs)
        v.stop_recording()
        return out, heard(path)

    for name, rate in (('a48.wav', 48000), ('a44.wav', 44100)):
        out, sound = recorded(name, f'play /mnt/{name}; echo played $?')
        check(f'at {rate}, play plays it to its end', 'played 0' in out, out)
        seconds = len(sound) / 48000
        check('a second of it is heard', 0.95 < seconds < 1.05, seconds)
        if len(sound) > 4800:
            middle = sound[len(sound) // 4: 3 * len(sound) // 4]
            found = (pitches(middle[:, 0])[0], pitches(middle[:, 1])[0])
            check('1000 Hz on the left and 500 on the right', abs(found[0] - 1000) < 10 and abs(found[1] - 500) < 10, found)
            check('loud as written', 11000 < np.abs(middle[:, 0]).max() < 13000, np.abs(middle[:, 0]).max())
            gap = longest_gap(sound)
            check('without gaps', gap[0] < 8, f'{gap[0]} frames, {gap[1]} in, of {len(sound)}')

    out, sound = recorded('both', 'play /mnt/a48.wav & play /mnt/b44.wav; wait')
    middle = sound[len(sound) // 4: 3 * len(sound) // 4]
    found = (pitches(middle[:, 0], 2), pitches(middle[:, 1], 2)) if len(middle) else (None, None)
    check('two programs play at once, mixed', all(f and abs(f[0] - a) < 10 and abs(f[1] - b) < 10
          for f, (a, b) in zip(found, ((1000, 700), (500, 300)))), found)

    check("the card's own control sets the volume", "audiod: the volume's" in v.raw(),
          [l for l in v.raw().split('\n') if 'audiod' in l])
    # Its square, in loudness: 50 is -12 dB, on the card, and 1 is -80, which is past the
    # -74 of QEMU's card, so audiod takes the rest away. QEMU's card plays its steps evenly,
    # rather than in the decibels it gives them, so what's heard isn't what they'd be.
    at = v.mark()
    out = v.run('volume 50')
    check('volume sets it', 'volume 50' in out, out)
    check('on the card, at -12 dB', v.wait('volume 50, the card at -12.00 dB and -0.04 dB more here', 2, at), v.text(at))
    at = v.mark()
    v.run('volume 1')
    check("and past what the card goes down to, here", v.wait('volume 1, the card at -74.00 dB and -6.00 dB more here', 2, at),
          v.text(at))
    v.run('volume mute')
    out, sound = recorded('muted', 'play /mnt/a48.wav')
    check('and muting it silences it', len(sound) == 0, np.abs(sound).max(initial=0))
    out = v.run('volume 100 unmute')
    check('until unmuted', 'volume 100\n' in out, out)

    at = v.mark()
    out = v.run('volume low-latency on')
    check('low latency is set', 'low latency' in out and v.wait('120 frames a period', 3, at), v.raw()[at:][-300:])
    out, sound = recorded('low', 'play /mnt/a44.wav')
    # It can crackle while the computer's busy, as when the tests run together, so a
    # period can go missing, but no more.
    gap = longest_gap(sound)
    check('and plays all of it, losing a period at most', 0.95 < len(sound) / 48000 < 1.05 and gap[0] <= 121,
          (len(sound), gap))
    v.run('volume low-latency off')

    # What's set is kept when audiod's started again.
    v.run('volume 30')
    at = v.mark()
    v.run('for p in /proc/[0-9]*; do [ "$(cat $p/comm 2>/dev/null)" = audiod ] && kill ${p#/proc/}; done')
    v.wait("audiod: the volume's", 10, at)
    out = v.run('volume')
    check("it's kept when audiod's started again", 'volume 30\n' in out, out)
    v.run('volume 100')

    # A USB card, which plays once it's plugged in, at its own volume, until unplugged.
    out = v.run('volume output')
    check('the computer has its own output', '* Built-in' in out, out)
    at = v.mark()
    v.plug_usb_sound()
    check('a USB card plugged in plays', v.wait('audiod: playing through', 15, at)
          and 'playing through Built-in' not in v.raw()[at:], v.raw()[at:][-500:])
    out = v.run('volume output')
    check('and is listed, playing', out.count('\n* ') + out.startswith('* ') == 1 and 'Built-in' in out
          and '* Built-in' not in out, out)
    path = f'{v.dir}/usb.wav'
    v.record(path, 'snd1')
    time.sleep(0.3)
    v.run('play /mnt/a48.wav', 4)
    v.stop_recording()
    sound = heard(path)
    middle = sound[len(sound) // 4: 3 * len(sound) // 4]
    check('what plays is heard on it', 0.95 < len(sound) / 48000 < 1.05 and abs(pitches(middle[:, 0])[0] - 1000) < 10,
          len(sound))
    v.run('volume 50')
    v.run('volume output Built-in')
    out = v.run('volume')
    check('each output keeps its own volume', 'volume 100\n' in out, out)
    usb = [l.split(' (')[0][2:] for l in v.run('volume output').split('\n') if 'Built-in' not in l and '(' in l][0]
    v.run(f"volume output '{usb}'")
    out = v.run('volume')
    check('as the USB card does', 'volume 50\n' in out, out)
    at = v.mark()
    v.plug_usb_sound(False)
    check('unplugged, the computer plays again', v.wait('audiod: playing through Built-in', 15, at), v.raw()[at:][-500:])
    out, sound = recorded('back', 'play /mnt/a48.wav')
    check('and is heard', 0.95 < len(sound) / 48000 < 1.05, len(sound))
    at = v.mark()
    v.plug_usb_sound()
    check('plugged in again, it plays again', v.wait('audiod: playing through', 15, at)
          and 'through Built-in' not in v.raw()[at:], v.raw()[at:][-300:])
    v.run('volume output Built-in')

    # The serial console's login isn't the one at the screen, which is the desktop's.
    v.run('exit')
    out, sound = recorded('unheard', 'play /mnt/a48.wav; echo played $?')
    check("someone not at the screen isn't heard", 'played 0' in out and len(sound) == 0, (out, len(sound)))
    out = v.run('volume 20')
    check("and can't change the volume", 'only whoever' in out, out)
finally:
    v.close()

# On the desktop, its account's the one at the screen: wm's chime on logging in is
# heard, and so is what that account plays, as the player does.
v = VM(sound=True, extra=['-drive', f'file={drive},format=raw,if=none,id=files', '-device', 'ide-hd,drive=files,bus=ide.1'])
try:
    path = f'{v.dir}/chime.wav'
    v.record(path)
    v.desktop()
    time.sleep(2)
    v.stop_recording()
    sound = heard(path)
    check("wm's chime is heard on logging in", len(sound) > 4800 and abs(pitches(sound[:2400, 0])[0] - 523) < 15,
          (len(sound), pitches(sound[:2400, 0]) if len(sound) > 2400 else None))
    at = v.mark()
    out = v.run('volume 40')
    check("the tray follows the volume set elsewhere", v.wait("wm: the volume's 40", 3, at), v.text(at))
    v.run('volume 100')

    # The tray lists what can be played through, once there's a choice, and plays
    # through one clicked.
    icons = [int(n) for n in re.findall(r"wm: quick settings' icons at (\d+) (\d+) (\d+) (\d+)", v.text())[-1]]
    at = v.mark()
    v.plug_usb_sound()
    v.wait('audiod: playing through', 15, at)
    time.sleep(1)
    at = v.mark()
    v.click(icons[0] + icons[2] // 2, icons[1] + icons[3] // 2)
    v.wait('wm: quick settings', 5, at)
    time.sleep(0.5)
    rows = {name: (int(x) + int(w) // 2, int(y) + int(h) // 2)
            for x, y, w, h, name in re.findall(r'wm: output \d+ at (\d+) (\d+) (\d+) (\d+), ([^,\n]+)', v.text(at))}
    check('the tray lists the outputs', 'Built-in' in rows and len(rows) == 2, v.text(at)[-600:])
    if 'Built-in' in rows:
        at = v.mark()
        v.click(*rows['Built-in'])
        check('and plays through one clicked', v.wait('wm: playing through Built-in', 5, at)
              and v.wait('audiod: playing through Built-in', 5, at), v.raw()[at:][-400:])
    v.key('esc')
    v.sudo('mount -o uid=1000,gid=100 /dev/sda /mnt')
    path = f'{v.dir}/player.wav'
    v.record(path)
    v.start('player /mnt/a48.wav')
    v.wait('player: playing', 10)
    time.sleep(2.5)
    v.stop_recording()
    sound = heard(path)
    tenths = [round(pitches(sound[i:i + 4800, 0])[0]) for i in range(0, len(sound) - 4800, 4800)]
    check('and the player plays a file, then the next in its folder',
          tenths[:10] == [1000] * 10 and tenths[-9:] == [700] * 9, (len(sound), tenths))
finally:
    v.close()
finish()
