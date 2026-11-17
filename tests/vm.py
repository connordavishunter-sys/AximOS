"""Boots AximOS in QEMU, types through the monitor and reads the serial console."""
import json, os, re, shutil, socket, subprocess, tempfile, time

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


def base_disk():
    """The built disk as make left it, without what running it by hand adds to
    /system/etc, such as an account, which would ask for a password at boot."""
    path = WORK + '/base.img'
    if not os.path.exists(path):
        quiet('cp', '--sparse=always', BUILD + '/disk.img', path)
        shipped = set(os.listdir(ROOT + '/rootfs/system/etc'))
        found = subprocess.run(['mdir', '-b', '-i', path, '::/system/etc'], capture_output=True, text=True).stdout.split()
        added = [f for f in found if os.path.basename(f.rstrip('/')) not in shipped and not f.endswith('/')]
        if added:
            quiet('mdel', '-i', path, *added)
    return path


def disk_copy(name):
    """A writable copy of the built disk, for tests that read back what the OS wrote.
    Only what's been written is copied, as most of the disk is empty."""
    path = f'{WORK}/{name}.img'
    quiet('cp', '--sparse=always', base_disk(), path)
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


def compile_program(source, out, static=False, libraries=(), export=False, gui=False):
    """Builds a test program the way the Makefile builds the system's: linked against
    libc.so and any other libraries, or with libc.a if static, and libgui.so for gui."""
    if gui:
        libraries = [*libraries, BUILD + '/user/libgui.so']
    quiet('gcc', *USER_CFLAGS, *(['-I' + ROOT + '/user/libgui/include'] if gui else []), '-c', source, '-o', out + '.o')
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
    def __init__(self, disk=None, smp=4, timeout=300, extra=(), net='', vmmouse=False, desktop=False):
        """With vmmouse, the mouse is placed where the host says, through QMP, as a
        desktop's QEMU window does. Without it there's only the relative PS/2 mouse.
        Shift is held through boot for the shell, unless it's to start the desktop."""
        self.dir = tempfile.mkdtemp(dir=WORK)
        self.log, sock, qmp = self.dir + '/serial.log', self.dir + '/monitor.sock', self.dir + '/qmp.sock'
        drive = f'file={disk},format=raw' if disk else f'file={base_disk()},format=raw,snapshot=on'
        self.proc = subprocess.Popen(
            ['timeout', str(timeout), 'qemu-system-x86_64', '-accel', 'kvm', '-cpu', 'host', '-smp', str(smp), '-m', '256M',
             '-M', 'q35,vmport=' + ('on' if vmmouse else 'off'), '-display', 'none', '-boot', 'd', '-cdrom', BUILD + '/aximos.iso',
             '-drive', drive + ',if=none,id=disk', '-device', 'ide-hd,drive=disk,bus=ide.1',
             '-netdev', 'user,id=net0' + net, '-device', 'e1000,netdev=net0',
             '-audiodev', 'none,id=snd0', '-device', 'ich9-intel-hda', '-device', 'hda-output,audiodev=snd0',
             '-serial', 'file:' + self.log, '-monitor', f'unix:{sock},server=on,wait=off',
             '-qmp', f'unix:{qmp},server=on,wait=off', *extra],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        while not os.path.exists(sock):
            time.sleep(0.05)
        self.monitor = socket.socket(socket.AF_UNIX)
        self.monitor.connect(sock)
        self.monitor.settimeout(0.05)
        while not os.path.exists(qmp):
            time.sleep(0.05)
        q = socket.socket(socket.AF_UNIX)
        q.connect(qmp)
        self.qmp = q.makefile('rw')
        self.qmp.readline()
        self.qmp_command('qmp_capabilities')
        if not desktop:
            self.hold_shift()

    def shift(self, down):
        self.qmp_command('input-send-event', events=[{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': 'shift'}}}])

    def hold_shift(self):
        """Presses Shift again and again, as a keyboard repeats a held key, until init
        has seen it."""
        end = time.time() + 60
        while 'init: ' not in self.raw() and time.time() < end:
            self.shift(True)
            time.sleep(0.05)
        self.shift(False)

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

    def mouse(self, dx=0, dy=0, buttons=None):
        """Moves the mouse by dx, dy, then sets which buttons are held (1 left, 2 right, 4 middle).
        QEMU only queues a little PS/2 movement, so a long move goes in steps."""
        while dx or dy:
            step_x, step_y = max(-100, min(100, dx)), max(-100, min(100, dy))
            self.command(f'mouse_move {step_x} {step_y}')
            dx, dy = dx - step_x, dy - step_y
            time.sleep(0.02)
        if buttons is not None:
            self.command(f'mouse_button {buttons}')
        time.sleep(0.2)

    def qmp_command(self, name, **arguments):
        self.qmp.write(json.dumps({'execute': name, 'arguments': arguments}) + '\n')
        self.qmp.flush()
        while 'event' in (reply := json.loads(self.qmp.readline())):
            pass
        return reply

    def pointer(self, x, y, buttons=None, width=1280, height=800):
        """Puts the absolute pointer on pixel x, y, scaled as QEMU's window does it, then sets the buttons."""
        axis = lambda name, at, size: {'type': 'abs', 'data': {'axis': name, 'value': at * 0x7FFF // size}}
        self.qmp_command('input-send-event', events=[axis('x', x, width), axis('y', y, height)])
        time.sleep(0.05)
        if buttons is not None:
            self.command(f'mouse_button {buttons}')
        time.sleep(0.2)

    def record(self, path, rate=48000):
        """Starts writing what the sound card plays to a WAV file, until close."""
        self.command(f'wavcapture {path} snd0 {rate} 16 2')

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
