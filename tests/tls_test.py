"""Checks the ports networking needs: OpenSSL makes a TLS connection to a server on
the host, finding the system's certificate authorities, and wpa_supplicant and libnl
run, as netd will run them."""
import socket, ssl, subprocess, threading
from vm import VM, WORK, check, finish

# A server on the host with a certificate of its own, which says hello over TLS
subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=aximos-test', '-days', '1',
                '-keyout', WORK + '/key.pem', '-out', WORK + '/cert.pem'], check=True, capture_output=True)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(WORK + '/cert.pem', WORK + '/key.pem')
server = socket.socket()
server.bind(('127.0.0.1', 0))
server.listen()
port = server.getsockname()[1]


def serve():
    while True:
        conn, _ = server.accept()
        try:
            with context.wrap_socket(conn, server_side=True) as tls:
                tls.sendall(b'hello over TLS\n')
        except (ssl.SSLError, OSError):
            pass


threading.Thread(target=serve, daemon=True).start()

v = VM()
try:
    v.login()
    v.wait('netd: enp0s2 is 10.0.2.15', 20)
    out = v.run('openssl version')
    check('OpenSSL runs', 'OpenSSL 4.1.0' in out, out)
    out = v.run(f'openssl s_client -connect 10.0.2.2:{port} -servername aximos-test -ign_eof </dev/null 2>&1', 4)
    check('and makes a TLS connection to the host', 'hello over TLS' in out and 'CN=aximos-test' in out, out[-600:])
    check("checking the server's certificate, which no authority signed",
          'self-signed certificate' in out, out[-600:])
    out = v.run('openssl x509 -in /etc/ssl/cert.pem -noout -subject')
    check('with the system\'s authorities where OpenSSL looks', 'subject=' in out, out)
    out = v.run('wpa_supplicant -v; wpa_cli -v')
    check('wpa_supplicant and wpa_cli run', 'wpa_supplicant v2.12' in out and 'wpa_cli v2.12' in out, out)
    out = v.sudo('modprobe mac80211_hwsim 2>/dev/null; wpa_supplicant -i nothing -D nl80211 -C /run/wpa 2>&1 | head -3', 3)
    check('and talks to the kernel through libnl', 'nothing' in out and 'error while loading' not in out, out)
finally:
    v.close()
finish()
