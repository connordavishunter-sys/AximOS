"""Checks networking against servers on the host, which QEMU's user network shows as 10.0.2.2."""
import http.server, os, random, socket, socketserver, threading, time
from vm import VM, WORK, check, finish

web = WORK + '/web'
os.makedirs(web)
open(web + '/hello.txt', 'w').write('hello from the host\n')
open(web + '/big.bin', 'wb').write(random.Random(2).randbytes(400_000))


class Files(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kw):
        super().__init__(*args, directory=web, **kw)

    def log_message(self, *args):
        pass


class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        while data := self.request.recv(4096):
            self.request.sendall(data.upper())


def serve(server):
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server.server_address[1]


http_port = serve(http.server.ThreadingHTTPServer(('127.0.0.1', 0), Files))
echo_port = serve(socketserver.ThreadingTCPServer(('127.0.0.1', 0), Echo))
with socket.socket() as s:
    s.bind(('127.0.0.1', 0))
    forward_port = s.getsockname()[1]

v = VM(net=f',hostfwd=tcp:127.0.0.1:{forward_port}-:7000')
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    for _ in range(10):  # the prompt can come up before DHCP has finished
        if '10.0.2.15' in (o := v.run('cat /proc/net')):
            break
    check('address from DHCP', '10.0.2.15' in o, o)
    o = v.run('ping -c 2 10.0.2.2', 3); check('ping the gateway', 'bytes from 10.0.2.2' in o, o)

    o = v.run(f'fetch http://10.0.2.2:{http_port}/hello.txt', 2); check('fetch over http', '\nhello from the host\n' in o, o)
    o = v.run(f'fetch -o /tmp/big http://10.0.2.2:{http_port}/big.bin; wc -c /tmp/big', 5)
    check('fetch a large file', '400000 /tmp/big' in o, o)
    o = v.run(f'fetch http://10.0.2.2:{http_port}/missing; echo st=$?', 2); check('fetch reports a 404', 'st=0' not in o, o)

    o = v.run(f'echo shout | nc 10.0.2.2 {echo_port}', 2); check('nc to a host server', '\nSHOUT\n' in o, o)
    o = v.run('nc 10.0.2.2 1; echo st=$?', 3); check('refused connection fails', 'st=0' not in o, o)

    # The other way round: the host connects to a listener in AximOS.
    v.type('nc -l 7000 > /tmp/got\n')
    time.sleep(1)
    with socket.create_connection(('127.0.0.1', forward_port), 5) as s:
        s.sendall(b'from the host\n')
    time.sleep(1)
    o = v.run('cat /tmp/got'); check('nc -l receives from the host', 'from the host' in o, o)
finally:
    v.close()
finish()
