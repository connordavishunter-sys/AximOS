"""Checks what real machines need that QEMU's defaults hide: booting without a working
PIT, the kernel's log on screen and in /proc/log, as a laptop has no serial port to
read it from, a crash shown on screen with where the kernel was, the lid, battery and
embedded controller, and the power button shutting down cleanly."""
import re, struct, subprocess, time
from vm import BUILD, VM, WORK, check, disk_copy, finish


# A little AML, assembled by hand, for an SSDT of the laptop parts QEMU hasn't got.
def pkg(body):
    """Body with its length in front, which counts the length's own bytes."""
    n = len(body) + 1
    return bytes([n]) + body if n < 64 else bytes([0x40 | (n + 1) & 0xF, (n + 1) >> 4]) + body


def num(n):
    return bytes([n]) if n < 2 else b'\x0a' + bytes([n]) if n < 256 else b'\x0b' + struct.pack('<H', n)


def name(seg, obj):
    return b'\x08' + seg + obj


def returns(seg, obj):
    return b'\x14' + pkg(seg + b'\x00\xa4' + obj)


def package(*items):
    return b'\x12' + pkg(bytes([len(items)]) + b''.join(items))


def device(seg, *parts):
    return b'\x5b\x82' + pkg(seg + b''.join(parts))


def eisa(product):
    """EisaId ("PNPxxxx")."""
    return b'\x0c\x41\xd0' + bytes.fromhex(product)


def laptop_table(path):
    """A lid that's open, a battery charged to 4000 of 5000 and charging, a charger
    plugged in, and an embedded controller with its two ports and a GPE."""
    io = lambda port: bytes([0x47, 1, port, 0, port, 0, 0, 1])
    resources = io(0x62) + io(0x66) + b'\x79\x00'
    aml = (device(b'LID0', name(b'_HID', eisa('0C0D')), returns(b'_LID', num(1)))
           + device(b'BAT0', name(b'_HID', eisa('0C0A')), returns(b'_STA', num(0x1F)),
                    returns(b'_BIF', package(num(0), num(5000), num(5000), num(1), num(12000), num(250), num(100))),
                    returns(b'_BST', package(num(2), num(500), num(4000), num(12000))))
           + device(b'ADP0', name(b'_HID', b'\x0dACPI0003\x00'), returns(b'_PSR', num(1)))
           + device(b'EC0_', name(b'_HID', eisa('0C09')), name(b'_GPE', num(0x10)),
                    name(b'_CRS', b'\x11' + pkg(num(len(resources)) + resources))))
    header = struct.pack('<4sIBB6s8sI4sI', b'SSDT', 36 + len(aml), 2, 0, b'AXIMOS', b'LAPTOP  ', 1, b'AXIM', 1)
    table = bytearray(header + aml)
    table[9] = -sum(table) & 0xFF
    open(path, 'wb').write(table)
    return path

# Newer chipsets can switch the PIT off, so the clocks are measured by the HPET.
v = VM(extra=['-M', 'q35,pit=off'])
try:
    check('boots without the PIT', v.wait('/$ ', 40), v.raw()[-800:])
    check('timing by the HPET', "the PIT doesn't count, so timing goes by the HPET" in v.raw(), v.raw())
    start = time.time()
    o = v.run('sleep 2; echo slept', 0)
    check('and a second is still a second', v.wait('\nslept\n', 10) and 1.8 < time.time() - start < 3.5, time.time() - start)
    o = v.run('wc -c /proc/log; grep -c "kernel: started" /proc/log')
    size = re.search(r'^ *(\d+) /proc/log', o, re.M)
    check("/proc/log has the kernel's log from the start", size and int(size[1]) > 1000 and '\n1\n' in o, o)
finally:
    v.close()

# Booting from a drive the kernel can't use leaves it with no filesystem.
v = VM(disk=False, cdrom=False, desktop=True,
       extra=['-drive', f'if=none,id=n,file={BUILD}/aximos.iso,format=raw,snapshot=on',
              '-device', 'nvme,serial=crash,drive=n,bootindex=0'])
try:
    check('a kernel that dies says why', v.wait('error: no filesystems found, halting', 40), v.raw()[-800:])
    check('and where it was, by name', v.wait('trace:\n  ffffffff', 5) and v.wait(' _start+', 5), v.raw()[-400:])
    time.sleep(0.5)
    w, h, px = v.screenshot()
    lit = sum(1 for y in range(0, h // 2, 2) for x in range(0, w, 2) if px(x, y))
    check('on the screen', lit > 2000, lit)
finally:
    v.close()

# A press of the power button, which QEMU makes as the chipset's own.
img = disk_copy('button')
v = VM(disk=img, extra=['-acpitable', 'file=' + laptop_table(WORK + '/laptop.aml')])
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    check("the firmware's code runs", "acpi: running the firmware's code" in v.raw(), v.raw())
    check('and says the lid is open', "acpi: running the firmware's code, lid open" in v.raw(), v.raw())
    check('the embedded controller is found', 'ec: ports 62 and 66' in v.raw() and 'ec: events on GPE 10' in v.raw(),
          [l for l in v.raw().split('\n') if l.startswith(('ec', 'acpi'))])
    check('the battery too', 'acpi: battery at 80 percent, charging' in v.raw(), v.raw())
    o = v.run('cat /proc/battery')
    check('in /proc/battery', 'charge:\t80%\nstate:\tcharging\npower:\tac\n' in o, o)
    v.run('echo kept > /home/button.txt')
    v.command('system_powerdown')
    check('pressing it shuts down', v.exited(15), v.raw()[-600:])
    check('as the kernel says', 'acpi: power button pressed' in v.raw() and 'kernel: powering off' in v.raw(), v.raw()[-600:])
finally:
    v.close()
check('with what was written kept', subprocess.run(['mtype', '-i', img, '::/home/button.txt'],
                                                   capture_output=True, text=True).stdout == 'kept\n')
finish()
