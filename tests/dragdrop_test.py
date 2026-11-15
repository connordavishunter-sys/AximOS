"""Checks dragging files from Files onto the viewer, the editor, the player and
Paint, which open them, and the editor's colouring, finding, replacing and going
to a line."""
import io, math, re, struct, time, wave
from vm import VM, WORK, check, disk_copy, finish, get, mkdir, put

SOURCE = '''#include <stdio.h>

int main(void) {
    // say hello
    printf("hello, world\\n");
    return 0;
}
'''
KEYWORD, STRING, COMMENT, DIRECTIVE = 0xC792EA, 0xC3E88D, 0x6F7A8F, 0x89DDFF
LEFT, RIGHT = (1, 25), (641, 25) # where tiled windows' contents start

img = disk_copy('dragdrop')
mkdir(img, '/drop')
open(WORK + '/hello.c', 'w').write(SOURCE)
put(img, WORK + '/hello.c', '/drop/hello.c')
put(img, 'rootfs/system/share/icons/apps/paint.png', '/drop/pic.png')
with wave.open(WORK + '/tone.wav', 'wb') as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000)
    w.writeframes(b''.join(struct.pack('<h', int(8000 * math.sin(i * 440 * 2 * math.pi / 48000))) for i in range(24000)))
put(img, WORK + '/tone.wav', '/drop/tone.wav')

v = VM(disk=img, vmmouse=True)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('echo none > /system/etc/wallpaper; echo text sharp > /system/etc/gui.conf') # exact colours to look for
    at = v.mark()
    v.type("wm 'files /drop'\n")
    check('Files starts', v.wait('files: at list', 10, at), v.text(at))
    time.sleep(1)
    at = v.mark()
    v.key('meta_l-left')
    v.wait('files: at list', 5, at); time.sleep(0.5)
    x, y, lw, lh = map(int, re.findall(r'files: at list (\d+) (\d+) (\d+) (\d+)', v.text())[-1])
    row = lambda i: (LEFT[0] + x + 60, LEFT[1] + y + 2 + i * 22 + 11) # hello.c, pic.png, tone.wav

    def drag(i, to=(960, 400)):
        a = row(i)
        v.pointer(*a, 1); time.sleep(0.2)
        for n in range(1, 6):
            v.pointer(a[0] + (to[0] - a[0]) * n // 5, a[1] + (to[1] - a[1]) * n // 5, 1); time.sleep(0.1)
        v.pointer(*to, 0)

    def run_on_right(command, ready):
        at = v.mark()
        v.key('meta_l-r'); v.wait('run: ready', 5, at)
        v.type(command + '\n')
        ok = v.wait(ready, 10, at)
        time.sleep(1)
        v.key('meta_l-right'); time.sleep(0.5)
        return ok

    check('the viewer opens', run_on_right('viewer', 'run: running viewer'), v.text())
    at = v.mark()
    drag(1)
    check('dragging a picture from Files', v.wait('files: dragging /drop/pic.png', 5, at), v.text(at))
    check('drops it on the viewer', v.wait('wm: dropped /drop/pic.png on Viewer', 5, at), v.text(at))
    check('which shows it', v.wait('viewer: showing /drop/pic.png 16x16', 5, at), v.text(at))
    at = v.mark()
    drag(0, (300, 400))
    time.sleep(1)
    check('dropping in its own folder in Files leaves it', 'files: dropped /drop/hello.c where it was' in v.text(at)
          and 'files: moved' not in v.text(at), v.text(at))
    v.pointer(960, 400, 1); v.pointer(960, 400, 0); v.key('meta_l-q'); time.sleep(0.5)

    # The editor colours C, and finds, replaces and goes to lines.
    check('the editor opens', run_on_right('editor', 'editor: title'), v.text())
    at = v.mark()
    drag(0)
    check('dropping C on the editor opens it', v.wait('editor: opened /drop/hello.c', 5, at), v.text(at))
    check('coloured as C', v.wait('editor: colouring C', 5, at), v.text(at))
    v.pointer(1200, 700); time.sleep(0.5)
    w, h, px = v.screenshot()
    colours = {c: sum(px(x, y) == c for x in range(650, 1000) for y in range(60, 220)) for c in (KEYWORD, STRING, COMMENT, DIRECTIVE)}
    check('keywords, strings, comments and #include each have their colour', all(n > 10 for n in colours.values()),
          {hex(c): n for c, n in colours.items()})

    first, second = SOURCE.index('hello'), SOURCE.index('hello', SOURCE.index('hello') + 1)
    at = v.mark()
    v.key('ctrl-f'); time.sleep(0.3); v.type('hello\n')
    check('Ctrl+F finds', v.wait(f'editor: selected {first} {first + 5}\n', 5, at), v.text(at))
    v.key('f3')
    check('F3 finds the next', v.wait(f'editor: selected {second} {second + 5}\n', 5, at), v.text(at))
    at = v.mark()
    v.key('f3')
    check('and goes round', v.wait(f'editor: selected {first} {first + 5}\n', 5, at), v.text(at))

    at = v.mark()
    v.key('ctrl-h'); time.sleep(0.3)
    v.key('tab'); v.key('tab'); v.key('tab'); v.type('bye')
    v.key('tab'); v.key('tab'); v.key('ret') # to All
    check('Replace all', v.wait('editor: replaced 2', 5, at), v.text(at))
    replaced = SOURCE.replace('hello', 'bye')
    line5 = sum(len(l) + 1 for l in replaced.split('\n')[:4])
    at = v.mark()
    v.key('ctrl-g'); time.sleep(0.3); v.type('5\n')
    check('Ctrl+G goes to a line', v.wait(f'editor: selected {line5} {line5}\n', 5, at), v.text(at))
    at = v.mark()
    v.key('ctrl-s')
    check('and it saves', v.wait('editor: saved /drop/hello.c', 5, at), v.text(at))
    v.key('meta_l-q'); time.sleep(0.5)

    check('the player opens', run_on_right('player', 'player: ready'), v.text())
    at = v.mark()
    drag(2)
    check('dropping a sound on the player plays it', v.wait('player: dropped /drop/tone.wav', 5, at), v.text(at))
    v.pointer(960, 400, 1); v.pointer(960, 400, 0); v.key('meta_l-q'); time.sleep(0.5)

    check('Paint opens', run_on_right('paint', 'paint: at picture'), v.text())
    at = v.mark()
    drag(1)
    check('dropping a picture on Paint opens it', v.wait('paint: opened /drop/pic.png 16x16', 5, at), v.text(at))
    v.key('meta_l-shift-q')
    time.sleep(1)
finally:
    v.close()

get(img, '/drop/hello.c', WORK + '/saved.c')
saved = open(WORK + '/saved.c').read()
check('what was saved has the replacements', saved == SOURCE.replace('hello', 'bye'), saved)
finish()
