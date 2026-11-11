"""Boots AximOS in QEMU, types through the monitor and reads the serial console."""
import os, re, shutil, socket, subprocess, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = ROOT + '/build'
WORK = tempfile.mkdtemp(prefix='aximos-test-')

NAMED = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus', '\\': 'backslash', '=': 'equal', ',': 'comma',
         ';': 'semicolon', "'": 'apostrophe', '\n': 'ret', '\t': 'tab', '[': 'bracket_left', ']': 'bracket_right',
         '`': 'grave_accent'}
SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8', '(': '9', ')': '0',
           '_': 'minus', '+': 'equal', '"': 'apostrophe', ':': 'semicolon', '?': 'slash', '~': 'grave_accent',
           '>': 'dot', '<': 'comma', '|': 'backslash', '{': 'bracket_left', '}': 'bracket_right'}
ESCAPES = re.compile(r'\x1b\[[0-9;?]*[A-Za-z]')
KERNEL_LOG = re.compile(r'(process|elf|kernel|vfs|fat32|mm|net|tcp|clock|cpu|irq|block|ahci|ata|console|fb|acpi|mouse|keyboard|serial|iso9660|procfs|exception)\S*: ')


def key_for(c):
    if c in NAMED:
        return NAMED[c]
    if c in SHIFTED:
        return 'shift-' + SHIFTED[c]
    return 'shift-' + c.lower() if c.isupper() else c


def quiet(*args):
    """Runs a host command, showing its output only if it fails."""
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f'{" ".join(args)}\n{r.stdout}{r.stderr}')


def disk_copy(name):
    """A writable copy of the built disk, for tests that read back what the OS wrote."""
    path = f'{WORK}/{name}.img'
    shutil.copy(BUILD + '/disk.img', path)
    return path


def put(img, host, guest):
    quiet('mcopy', '-o', '-s', '-i', img, host, '::' + guest)


def get(img, guest, host):
    quiet('mcopy', '-o', '-i', img, '::' + guest, host)


def mkdir(img, *guest):
    quiet('mmd', '-i', img, *('::' + g for g in guest))


LIBC_INCLUDE = ['-nostdinc', '-isystem', subprocess.check_output(['gcc', '-print-file-name=include'], text=True).strip(),
                '-I' + ROOT + '/user/libc/include', '-I' + ROOT + '/abi']
USER_CFLAGS = ['-std=gnu23', '-O2', '-w', '-fno-stack-protector', '-fno-pic', '-fno-pie', '-fcf-protection=none',
               '-march=x86-64', '-mno-red-zone', '-D__aximos__'] + LIBC_INCLUDE
LINK = ['ld', '-m', 'elf_x86_64', '-nostdlib', '-z', 'max-page-size=0x1000', '-z', 'noexecstack', '-Ttext-segment=0x400000']


def compile_program(source, out, static=False, libraries=(), export=False):
    """Builds a test program the way the Makefile builds the system's: linked against
    libc.so and any other libraries, or with libc.a if static."""
    quiet('gcc', *USER_CFLAGS, '-c', source, '-o', out + '.o')
    libc = ['-static', BUILD + '/user/libc.a'] if static else \
        ['--hash-style=sysv', '-dynamic-linker', '/system/lib/libc.so', *libraries, BUILD + '/user/libc.so']
    quiet(*LINK, '-rpath-link', WORK, *(['-E'] if export else []), '-e', '_start', BUILD + '/user/libc/crt0.o', out + '.o', *libc, '-o', out)
    return out


def compile_library(source, out, *needs):
    pic = [f for f in USER_CFLAGS if f not in ('-fno-pic', '-fno-pie')] + ['-fPIC']
    quiet('gcc', *pic, '-c', source, '-o', out + '.o')
    quiet('ld', '-m', 'elf_x86_64', '-shared', '--hash-style=sysv', '-soname', os.path.basename(out), out + '.o', *needs, '-o', out)
    return out


class VM:
    def __init__(self, disk=None, smp=4, timeout=300, extra=(), net=''):
        self.dir = tempfile.mkdtemp(dir=WORK)
        self.log, sock = self.dir + '/serial.log', self.dir + '/monitor.sock'
        drive = f'file={disk},format=raw' if disk else f'file={BUILD}/disk.img,format=raw,snapshot=on'
        self.proc = subprocess.Popen(
            ['timeout', str(timeout), 'qemu-system-x86_64', '-accel', 'kvm', '-cpu', 'host', '-smp', str(smp), '-m', '256M',
             '-M', 'q35', '-display', 'none', '-boot', 'd', '-cdrom', BUILD + '/aximos.iso',
             '-drive', drive + ',if=none,id=disk', '-device', 'ide-hd,drive=disk,bus=ide.1',
             '-netdev', 'user,id=net0' + net, '-device', 'e1000,netdev=net0',
             '-serial', 'file:' + self.log, '-monitor', f'unix:{sock},server=on,wait=off', *extra],
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
        return '\n'.join(l for l in lines if not KERNEL_LOG.match(l))

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


def program_checks(v, line, done, secs=60, prefix=''):
    """Runs a test program at the prompt and records each ok or FAIL line it prints."""
    at = v.mark()
    v.type(line + '\n')
    finished = v.wait(done, secs, at)
    time.sleep(0.3)
    out = v.text(at)
    for result, name in re.findall(r'^(ok|FAIL) (.*)$', out, re.M):
        check(prefix + name, result == 'ok')
    check(prefix + line + ' finishes', finished, out[-400:])
    return out


def finish():
    shutil.rmtree(WORK, ignore_errors=True)
    print(f'{failures} failed')
    raise SystemExit(failures > 0)
