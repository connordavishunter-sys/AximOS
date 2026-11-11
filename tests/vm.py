"""Boots AximOS in QEMU, types through the monitor and reads the serial console."""
import os, re, socket, subprocess, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = ROOT + '/build'

NAMED = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus', '\\': 'backslash', '=': 'equal', ',': 'comma',
         ';': 'semicolon', "'": 'apostrophe', '\n': 'ret', '\t': 'tab', '[': 'bracket_left', ']': 'bracket_right'}
SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8', '(': '9', ')': '0',
           '_': 'minus', '+': 'equal', '"': 'apostrophe', ':': 'semicolon', '?': 'slash', '~': 'grave_accent',
           '>': 'dot', '<': 'comma', '|': 'backslash', '{': 'bracket_left', '}': 'bracket_right'}
ESCAPES = re.compile(r'\x1b\[[0-9;?]*[A-Za-z]')


def key_for(c):
    if c in NAMED:
        return NAMED[c]
    if c in SHIFTED:
        return 'shift-' + SHIFTED[c]
    return 'shift-' + c.lower() if c.isupper() else c


class VM:
    def __init__(self, timeout=300):
        self.dir = tempfile.mkdtemp(prefix='aximos-test-')
        self.log, sock = self.dir + '/serial.log', self.dir + '/monitor.sock'
        self.proc = subprocess.Popen(
            ['timeout', str(timeout), 'qemu-system-x86_64', '-accel', 'kvm', '-cpu', 'host', '-smp', '4', '-m', '256M',
             '-M', 'q35', '-display', 'none', '-boot', 'd', '-cdrom', BUILD + '/aximos.iso',
             '-drive', f'file={BUILD}/disk.img,format=raw,if=none,id=disk,snapshot=on', '-device', 'ide-hd,drive=disk,bus=ide.1',
             '-netdev', 'user,id=net0', '-device', 'e1000,netdev=net0',
             '-serial', 'file:' + self.log, '-monitor', f'unix:{sock},server=on,wait=off'],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        while not os.path.exists(sock):
            time.sleep(0.05)
        self.monitor = socket.socket(socket.AF_UNIX)
        self.monitor.connect(sock)
        self.monitor.settimeout(0.05)

    def raw(self):
        return open(self.log, errors='replace').read() if os.path.exists(self.log) else ''

    def mark(self):
        return len(self.raw())

    def text(self, since=0):
        """What the console showed since a mark, without escapes or kernel log lines."""
        lines = ESCAPES.sub('', self.raw()[since:]).replace('\r', '').split('\n')
        return '\n'.join(l for l in lines if not re.match(r'(process|elf):', l))

    def wait(self, needle, secs=10, since=0):
        end = time.time() + secs
        while time.time() < end:
            if needle in self.text(since):
                return True
            time.sleep(0.05)
        return False

    def key(self, name):
        self.monitor.sendall(f'sendkey {name}\n'.encode())
        try:
            while self.monitor.recv(4096):
                pass
        except socket.timeout:
            pass

    def type(self, s):
        for c in s:
            self.key(key_for(c))

    def run(self, line, secs=1.0):
        """Types a command line and returns what it printed."""
        at = self.mark()
        self.type(line + '\n')
        time.sleep(secs)
        return self.text(at)

    def close(self):
        self.proc.terminate()
        self.proc.wait()


failures = 0


def check(name, ok, detail=''):
    global failures
    failures += not ok
    print(('PASS ' if ok else 'FAIL ') + name + ('' if ok else '\n    ' + repr(detail[-300:])))
