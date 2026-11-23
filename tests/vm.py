"""Boots AximOS in QEMU. What's typed goes to the shell on the serial port, and keys
and the pointer go through the monitor and QMP to the keyboard and a USB tablet."""
import json, os, re, shlex, shutil, socket, subprocess, tempfile, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = ROOT + '/build'
OVMF = '/usr/share/edk2/x64/OVMF.4m.fd'
WORK = tempfile.mkdtemp(prefix='aximos-test-')
ESCAPES = re.compile(r'\x1b\[[0-9;?]*[A-Za-z]')
NAMED = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus', '\\': 'backslash', '=': 'equal', ',': 'comma',
         ';': 'semicolon', "'": 'apostrophe', '\n': 'ret', '\t': 'tab', '[': 'bracket_left', ']': 'bracket_right',
         '`': 'grave_accent'}
SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8', '(': '9', ')': '0',
           '_': 'minus', '+': 'equal', '"': 'apostrophe', ':': 'semicolon', '?': 'slash', '~': 'grave_accent',
           '>': 'dot', '<': 'comma', '|': 'backslash', '{': 'bracket_left', '}': 'bracket_right'}
KERNEL_LOG = re.compile(r'^\[ *\d+\.\d+\] ')  # the kernel's own messages, which quiet leaves only for problems


def key_for(c):
    if c in NAMED:
        return NAMED[c]
    if c in SHIFTED:
        return 'shift-' + SHIFTED[c]
    return 'shift-' + c.lower() if c.isupper() else c


class VM:
    def __init__(self, timeout=300, extra=(), net='', smp=4, memory='2G', nvme=None):
        """Boots the ISO, or with nvme, the disk image there as an NVMe drive."""
        boot = ['-drive', f'file={nvme},format=raw,if=none,id=boot', '-device', 'nvme,serial=aximos,drive=boot'] \
            if nvme else ['-cdrom', BUILD + '/aximos.iso']
        self.dir = tempfile.mkdtemp(dir=WORK)
        self.log = self.dir + '/serial.log'
        serial, monitor, qmp = self.dir + '/serial.sock', self.dir + '/monitor.sock', self.dir + '/qmp.sock'
        self.proc = subprocess.Popen(
            ['timeout', str(timeout), 'qemu-system-x86_64', '-accel', 'kvm', '-cpu', 'host', '-smp', str(smp), '-m', memory,
             '-M', 'q35', '-display', 'none', '-bios', OVMF, *boot,
             '-netdev', 'user,id=net0' + net, '-device', 'e1000,netdev=net0', '-device', 'qemu-xhci',
             '-device', 'usb-tablet',
             '-chardev', f'socket,id=serial,path={serial},server=on,wait=off,logfile={self.log}', '-serial', 'chardev:serial',
             '-monitor', f'unix:{monitor},server=on,wait=off', '-qmp', f'unix:{qmp},server=on,wait=off', *extra],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.serial = self._connect(serial)
        threading.Thread(target=self._drain, daemon=True).start()
        self.monitor = self._connect(monitor)
        self.monitor.settimeout(0.05)
        self.qmp = self._connect(qmp).makefile('rw')
        self.qmp.readline()
        self.qmp_command('qmp_capabilities')

    @staticmethod
    def _connect(path):
        while not os.path.exists(path):
            time.sleep(0.05)
        s = socket.socket(socket.AF_UNIX)
        s.connect(path)
        return s

    def _drain(self):
        """Reads what the serial port sends, which QEMU also logs, so it never stops
        for want of a reader."""
        try:
            while self.serial.recv(65536):
                pass
        except OSError:
            pass

    def raw(self):
        return open(self.log, errors='replace').read() if os.path.exists(self.log) else ''

    def mark(self):
        return len(self.raw())

    def text(self, since=0):
        """What the serial port showed since a mark, without escapes or kernel messages."""
        lines = ESCAPES.sub('', self.raw()[since:]).replace('\r', '').split('\n')
        return '\n'.join(l for l in lines if not KERNEL_LOG.match(l))

    def wait(self, needle, secs=10, since=0):
        end = time.time() + secs
        while time.time() < end:
            if needle in self.text(since):
                return True
            time.sleep(0.05)
        return False

    def type(self, s):
        self.serial.sendall(s.encode())

    def run(self, line, secs=1.0):
        """Types a command line into the shell and returns what it printed."""
        at = self.mark()
        self.type(line + '\n')
        time.sleep(secs)
        return self.text(at)

    def login(self, name='tester', password='secret'):
        """Logs in at the serial port, first making the account, as the first boot
        asks, unless the desktop's made it, when the name asked for goes to logging in."""
        assert self.wait('Make an account', 30), self.text()
        if 'session: ' not in self.text():
            self.type(f'{name}\n{password}\n{password}\n')
            assert self.wait('login: ', 10), self.text()
        at = self.mark()
        self.type(f'{name}\n{password}\n')
        assert self.wait('$ ', 10, at), self.text(at)

    def sudo(self, line, secs=2.0, password='secret'):
        """Runs a command line as root, giving the password sudo asks for."""
        at = self.mark()
        self.type(f'sudo sh -c {shlex.quote(line)}\n')
        assert self.wait('Password for ', 5, at), self.text(at)
        return self.run(password, secs)

    def command(self, line):
        """Runs a QEMU monitor command, without waiting for it to finish."""
        self.monitor.sendall(line.encode() + b'\n')
        try:
            while self.monitor.recv(4096):
                pass
        except socket.timeout:
            pass

    def key(self, name):
        self.command(f'sendkey {name}')

    def keys(self, s):
        """Types on the keyboard, as the desktop takes it, rather than at the serial port."""
        for c in s:
            self.key(key_for(c))

    def pointer(self, x, y, buttons=None, width=1280, height=800):
        """Puts the tablet's pointer on pixel x, y of the screen, then sets the buttons
        held: 1 left, 2 right, 4 middle."""
        axis = lambda name, at, size: {'type': 'abs', 'data': {'axis': name, 'value': at * 0x7FFF // (size - 1)}}
        self.qmp_command('input-send-event', events=[axis('x', x, width), axis('y', y, height)])
        time.sleep(0.05)
        if buttons is not None:
            self.command(f'mouse_button {buttons}')
        time.sleep(0.2)

    def mouse(self, dx=0, dy=0, buttons=None):
        """Moves the pointer by dx, dy from where it was, which starts in the middle of the
        screen, as wm's does, then sets the buttons."""
        self.at = getattr(self, 'at', (640, 400))
        self.at = (max(0, min(1279, self.at[0] + dx)), max(0, min(799, self.at[1] + dy)))
        self.pointer(*self.at, buttons)

    def desktop(self, conf='', wallpaper='', setup=(), name='tester', password='secret'):
        """Logs in on the desktop as the account the serial port makes, with its settings
        as given: gui.conf's lines, and a wallpaper, or none for the plain colour, and
        any other commands to run first. Programs started at the serial port then show
        on that desktop."""
        self.login(name, password)
        self.run(f"mkdir ~/.config ~/.config/aximos; printf '{conf}\\n' > ~/.config/aximos/gui.conf")
        if wallpaper:
            self.run(f'echo {wallpaper} > ~/.config/aximos/wallpaper')
        for line in setup:
            self.run(line)
        end = time.time() + 30
        while 'login: log in' not in self.text() and 'login: at next' not in self.text() and time.time() < end:
            time.sleep(0.1)
        at = self.mark()
        if 'login: log in' not in self.text(): # the first boot's pages, from before the account
            self.click(*center(self.text(), 'next')) # which finds the account, and asks to log in
            assert self.wait('login: log in', 10, at), self.text(at)
        time.sleep(0.5)
        self.keys(f'{name}\t{password}\n')
        assert self.wait(f'session: {name} logged in', 15, at), self.text(at)
        assert self.wait('wm: running at', 15, at), self.text(at)
        self.run('for s in /tmp/aximos-wm-*.sock; do [ -O $s ] && export GUI_SOCKET=$s; done') # its own wm's
        self.at = (640, 400)

    def relogin(self, name='tester', password='secret'):
        """Logs out of the desktop, unless it's been quit already, and in again, as
        starting wm again did."""
        text = self.text()
        if text.rfind('session: {name} logged out'.format(name=name)) < text.rfind(f'session: {name} logged in'):
            self.key('meta_l-shift-q')
        assert self.wait(f'session: {name} logged out', 15), self.text()
        at = self.mark()
        assert self.wait('login: log in', 15, at), self.text(at)
        time.sleep(0.5)
        self.keys(f'{name}\t{password}\n')
        assert self.wait('wm: running at', 15, at), self.text(at)
        self.run('for s in /tmp/aximos-wm-*.sock; do [ -O $s ] && export GUI_SOCKET=$s; done') # its own wm's
        self.at = (640, 400)

    def start(self, command):
        """Starts a program on the desktop, from the serial port."""
        self.type(f'{command} &\n')

    def click(self, x, y):
        self.pointer(x, y, 1)
        self.pointer(x, y, 0)

    def screenshot(self):
        """Returns the screen as (width, height, pixel(x, y) giving 0xRRGGBB)."""
        path = self.dir + '/screen.ppm'
        if os.path.exists(path):
            os.remove(path)
        self.command(f'screendump {path}')
        for _ in range(100):
            if os.path.exists(path) and os.path.getsize(path) > 15:
                time.sleep(0.1)
                break
            time.sleep(0.05)
        data = open(path, 'rb').read()
        magic, width, height, depth, pixels = data.split(maxsplit=4)
        width, height = int(width), int(height)
        assert magic == b'P6' and depth == b'255'
        return width, height, lambda x, y: int.from_bytes(pixels[(y * width + x) * 3:(y * width + x) * 3 + 3], 'big')

    def desktop_login(self, name='tester', password='secret'):
        """Goes through the desktop's first boot, and makes the account, which logs in,
        waiting for its desktop."""
        assert self.wait('login: at next', 30), self.text()
        at = self.mark()
        self.click(*center(self.text(), 'next'))
        assert self.wait('login: at next', 10, at), self.text(at)
        at = self.mark()
        self.click(*center(self.text(), 'next'))
        assert self.wait('login: create', 10, at), self.text(at)
        time.sleep(0.5)
        self.keys(f'{name}\t{password}\t{password}\n')
        assert self.wait(f'session: {name} logged in', 15, at), self.text(at)
        assert self.wait('wm: running at', 15, at), self.text(at)

    def qmp_command(self, name, **arguments):
        self.qmp.write(json.dumps({'execute': name, 'arguments': arguments}) + '\n')
        self.qmp.flush()
        while 'event' in (reply := json.loads(self.qmp.readline())):
            pass
        return reply

    def exited(self, secs):
        try:
            self.proc.wait(secs)
            return True
        except subprocess.TimeoutExpired:
            return False

    def close(self):
        self.proc.terminate()
        self.proc.wait()


def center(text, what, width=1280, height=800):
    """Where on the screen a widget of the log-in screen is, from the last lines it gave
    for its window, which is in the middle of the screen, and the widget in it."""
    w, h = map(int, re.findall(r'login: [a-z ]+, (\d+)x(\d+)', text)[-1])
    x, y, rw, rh = map(int, re.findall(rf'login: at {re.escape(what)} (\d+) (\d+) (\d+) (\d+)', text)[-1])
    return (width - w) // 2 + x + rw // 2, (height - h) // 2 + y + rh // 2


failures = 0


def check(name, ok, detail=''):
    global failures
    failures += not ok
    print(('PASS ' if ok else 'FAIL ') + name + ('' if ok else '\n    ' + repr(str(detail)[-400:])))
    return ok


def finish():
    shutil.rmtree(WORK, ignore_errors=True)
    print(f'{failures} failed')
    raise SystemExit(failures > 0)
