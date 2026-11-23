"""Checks bash, curl and git: bash runs a script with what dash hasn't, curl fetches from
the host over HTTP and HTTPS, refusing a certificate no authority signed, and git commits
and clones a repository from the host over HTTP."""
import http.server, os, socket, ssl, subprocess, threading
from vm import VM, WORK, check, finish

# The host serves files over HTTP, among them a git repository, and says hello over HTTPS
web = WORK + '/web'
os.makedirs(web)
open(web + '/hello.txt', 'w').write('hello from the host\n')
repo = web + '/project.git'
env = {**os.environ, 'GIT_AUTHOR_NAME': 'Host', 'GIT_AUTHOR_EMAIL': 'host@test', 'GIT_COMMITTER_NAME': 'Host',
       'GIT_COMMITTER_EMAIL': 'host@test', 'GIT_CONFIG_GLOBAL': '/dev/null'}
subprocess.run(['git', 'init', '-q', '-b', 'main', WORK + '/project'], check=True, env=env)
open(WORK + '/project/README', 'w').write('cloned from the host\n')
subprocess.run(['git', '-C', WORK + '/project', 'add', 'README'], check=True, env=env)
subprocess.run(['git', '-C', WORK + '/project', 'commit', '-qm', 'First'], check=True, env=env)
subprocess.run(['git', 'clone', '-q', '--bare', WORK + '/project', repo], check=True, env=env)
subprocess.run(['git', '-C', repo, 'update-server-info'], check=True, env=env)  # for cloning over plain HTTP


class Files(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kw):
        super().__init__(*args, directory=web, **kw)

    def log_message(self, *args):
        pass


httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Files)
threading.Thread(target=httpd.serve_forever, daemon=True).start()
port = httpd.server_address[1]

subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=aximos-test', '-days', '1',
                '-keyout', WORK + '/key.pem', '-out', WORK + '/cert.pem'], check=True, capture_output=True)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(WORK + '/cert.pem', WORK + '/key.pem')
tls = socket.socket()
tls.bind(('127.0.0.1', 0))
tls.listen()
tls_port = tls.getsockname()[1]


def serve_tls():
    while True:
        conn, _ = tls.accept()
        try:
            with context.wrap_socket(conn, server_side=True) as s:
                s.recv(4096)
                s.sendall(b'HTTP/1.0 200 OK\r\nContent-Length: 15\r\n\r\nhello over TLS\n')
        except (ssl.SSLError, OSError):
            pass


threading.Thread(target=serve_tls, daemon=True).start()

v = VM()
try:
    v.login()
    v.wait('netd: enp0s2 is 10.0.2.15', 20)
    out = v.run("bash -c 'a=(one two three); echo \"bash ${BASH_VERSINFO[0]}.${BASH_VERSINFO[1]} has ${#a[@]} [[ ok ]]\"; [[ x == x ]] && echo matched'")
    check('bash runs what dash has no words for', 'bash 5.3 has 3' in out and 'matched' in out, out)

    out = v.run(f'curl -s http://10.0.2.2:{port}/hello.txt', 2)
    check('curl fetches over HTTP', 'hello from the host' in out, out)
    out = v.run(f'curl -s https://10.0.2.2:{tls_port}/; echo "exit $?"; curl -sk https://10.0.2.2:{tls_port}/', 3)
    check("refuses HTTPS to a server no authority vouches for", 'exit 60' in out, out)
    check('unless told not to check', 'hello over TLS' in out, out)

    v.run("git config --global user.name Tester; git config --global user.email tester@aximos")
    out = v.run('mkdir mine && cd mine && git init -q && echo hi > a && git add a && git commit -qm First && '
                'git log --oneline | cat; cd', 3)
    check('git commits', 'First' in out, out)
    out = v.run(f'git clone -q http://10.0.2.2:{port}/project.git && cat project/README', 6)
    check('and clones over HTTP, through curl', 'cloned from the host' in out, out)
finally:
    v.close()
finish()
