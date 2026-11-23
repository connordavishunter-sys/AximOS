"""Boots AximOS in QEMU. What's typed goes to the shell on the serial port, and keys
and the mouse go through the monitor to the keyboard and mouse."""
import json, os, re, shutil, socket, subprocess, tempfile, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = ROOT + '/build'
OVMF = '/usr/share/edk2/x64/OVMF.4m.fd'
WORK = tempfile.mkdtemp(prefix='aximos-test-')
ESCAPES = re.compile(r'\x1b\[[0-9;?]*[A-Za-z]')
KERNEL_LOG = re.compile(r'^\[ *\d+\.\d+\] ')  # the kernel's own messages, which quiet leaves only for problems


class VM:
    def __init__(self, timeout=300, extra=(), net='', smp=4, memory='1G'):
        self.dir = tempfile.mkdtemp(dir=WORK)
        self.log = self.dir + '/serial.log'
        serial, monitor, qmp = self.dir + '/serial.sock', self.dir + '/monitor.sock', self.dir + '/qmp.sock'
        self.proc = subprocess.Popen(
            ['timeout', str(timeout), 'qemu-system-x86_64', '-accel', 'kvm', '-cpu', 'host', '-smp', str(smp), '-m', memory,
             '-M', 'q35', '-display', 'none', '-bios', OVMF, '-cdrom', BUILD + '/aximos.iso',
             '-netdev', 'user,id=net0' + net, '-device', 'e1000,netdev=net0',
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
