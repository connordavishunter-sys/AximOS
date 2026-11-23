"""Checks curl and NetSurf against a web server on the host: a page with PNG, JPEG, GIF
and WebP pictures shows with all of them and smooth text, and can be maximized; a file
that isn't a page downloads to ~/Downloads; a page's file opens from its path; and the
viewer shows GIF and WebP pictures too."""
import http.server, os, re, threading, time
from PIL import Image
from vm import VM, WORK, check, finish

web = WORK + '/web'
os.makedirs(web)
open(web + '/hello.txt', 'w').write('hello from the host\n')
open(web + '/page.html', 'w').write('<html><body style="background: white"><h1>AximOS browses</h1>'
                                    '<img src="red.png"> <img src="blue.jpg"> <img src="green.gif"> '
                                    '<img src="yellow.webp"></body></html>')
open(web + '/data.bin', 'wb').write(bytes(range(256)) * 40)
Image.new('RGB', (160, 100), (255, 0, 0)).save(web + '/red.png')
Image.new('RGB', (160, 100), (0, 0, 255)).save(web + '/blue.jpg', quality=95)
Image.new('RGB', (160, 100), (0, 255, 0)).save(web + '/green.gif')
Image.new('RGB', (160, 100), (255, 255, 0)).save(web + '/yellow.webp', lossless=True)


class Files(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kw):
        super().__init__(*args, directory=web, **kw)

    def log_message(self, *args):
        pass


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Files)
threading.Thread(target=server.serve_forever, daemon=True).start()
url = f'http://10.0.2.2:{server.server_address[1]}'


def count(px, w, h, test):
    return sum(test(px(x, y)) for x in range(0, w, 4) for y in range(0, h, 4))


def colour(r, g, b):
    return lambda p: all(abs((p >> s & 255) - c) < 50 for s, c in ((16, r), (8, g), (0, b)))


red, blue, green, yellow = colour(255, 0, 0), colour(0, 0, 255), colour(0, 255, 0), colour(255, 255, 0)


def shows(v, *colours, secs=30):
    """Waits for the screen to have plenty of each colour, giving how much of each it has."""
    for _ in range(secs):
        time.sleep(1)
        w, h, px = v.screenshot()
        counts = [count(px, w, h, c) for c in colours]
        if all(n > 300 for n in counts):
            break
    return counts, px


def greys(px):
    """The shades of grey in the top of NetSurf's page, where its heading is."""
    return len({p for x in range(100, 600) for y in range(130, 200) if (p := px(x, y)) >> 16 == (p >> 8 & 255) == (p & 255)})


v = VM()
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    for _ in range(10):  # the prompt can come up before DHCP has finished
        if '10.0.2.15' in v.run('cat /proc/net'):
            break
    o = v.run(f'curl -s {url}/hello.txt', 5)
    check('curl fetches over http', '\nhello from the host\n' in o, o)
    v.run(f'curl -s -o /tmp/green.gif {url}/green.gif; curl -s -o /tmp/yellow.webp {url}/yellow.webp; '
          '/system/bin/echo \'<h1>On disk</h1><div style="background: #00ff00; height: 200px"></div>\' > /tmp/local.html; '
          'echo none > /system/etc/wallpaper; rm -f /system/etc/gui.conf', 5)

    at = v.mark()
    v.type(f'wm "netsurf {url}/page.html; echo netsurf exited $?"\n')
    v.wait('wm: running', 10, at)
    (reds, blues, greens, yellows), px = shows(v, red, blue, green, yellow)
    check('NetSurf shows the page with its PNG', reds > 300, reds)
    check('and its JPEG', blues > 300, blues)
    check('and its GIF', greens > 300, greens)
    check('and its WebP', yellows > 300, yellows)
    check('in smooth text', greys(px) > 6, greys(px))

    # Maximizing resizes its window, which is drawn again at the new size.
    v.key('meta_l-up'); time.sleep(3)
    w, h, px = v.screenshot()
    check('it can be maximized', count(px, w, h, red) > 300 and count(px, w, h, lambda p: p == 0xFFFFFF) > 20000,
          count(px, w, h, red))
    v.key('meta_l-q')
    check('and closes cleanly', v.wait('netsurf exited 0', 10, at), v.text(at))
    v.key('meta_l-shift-q'); time.sleep(1)

    # What isn't a page goes to ~/Downloads, and a page's file opens from its path.
    at = v.mark()
    v.type(f'wm "netsurf {url}/data.bin; netsurf /tmp/local.html"\n')
    check('a file that isn\'t a page downloads', v.wait('wm: notice Downloaded: data.bin', 20, at),
          v.text(at))
    v.key('meta_l-q')
    (greens,), px = shows(v, green)
    check('and a page opens from its path', greens > 300, greens)
    v.key('meta_l-shift-q'); time.sleep(1)
    o = v.run('wc -c /home/Downloads/data.bin')
    check('with all of the download', '10240 ' in o, o)

    # The viewer shows GIF and WebP pictures as well.
    for path, test in (('/tmp/green.gif', green), ('/tmp/yellow.webp', yellow)):
        at = v.mark()
        v.type(f'wm "viewer {path}"\n')
        (n,), px = shows(v, test, secs=10)
        check(f'the viewer shows {path}', n > 300, n)
        v.key('meta_l-shift-q'); time.sleep(1)
finally:
    v.close()
finish()
