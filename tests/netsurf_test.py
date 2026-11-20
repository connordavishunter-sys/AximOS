"""Checks curl and NetSurf against a web server on the host: a page with a PNG and a
JPEG in it shows in NetSurf's window, with both pictures, and it closes cleanly."""
import http.server, os, threading, time
from PIL import Image
from vm import VM, WORK, check, finish

web = WORK + '/web'
os.makedirs(web)
open(web + '/hello.txt', 'w').write('hello from the host\n')
open(web + '/page.html', 'w').write('<html><body style="background: white"><h1>AximOS</h1>'
                                    '<img src="red.png"> <img src="blue.jpg"></body></html>')
Image.new('RGB', (160, 100), (255, 0, 0)).save(web + '/red.png')
Image.new('RGB', (160, 100), (0, 0, 255)).save(web + '/blue.jpg', quality=95)


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


red = lambda p: p >> 16 > 200 and (p >> 8 & 255) < 60 and (p & 255) < 60
blue = lambda p: p & 255 > 200 and p >> 16 < 60 and (p >> 8 & 255) < 60

v = VM()
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    for _ in range(10):  # the prompt can come up before DHCP has finished
        if '10.0.2.15' in v.run('cat /proc/net'):
            break
    o = v.run(f'curl -s {url}/hello.txt', 5)
    check('curl fetches over http', '\nhello from the host\n' in o, o)

    v.run('echo none > /system/etc/wallpaper; rm -f /system/etc/gui.conf')
    at = v.mark()
    v.type(f'wm "netsurf {url}/page.html; echo netsurf exited $?"\n')
    v.wait('wm: running', 10, at)
    reds = blues = 0
    for _ in range(30):
        time.sleep(1)
        w, h, px = v.screenshot()
        reds, blues = count(px, w, h, red), count(px, w, h, blue)
        if reds > 500 and blues > 500:
            break
    check('NetSurf shows the page with its PNG', reds > 500, reds)
    check('and its JPEG', blues > 500, blues)
    v.key('meta_l-q')
    check('and closes cleanly', v.wait('netsurf exited 0', 10, at), v.text(at))
    v.key('meta_l-shift-q'); time.sleep(1)
finally:
    v.close()
finish()
