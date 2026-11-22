"""Checks what real machines need that QEMU's defaults hide: booting without a working
PIT, the kernel's log on screen and in /proc/log, as a laptop has no serial port to
read it from, a crash shown on screen with where the kernel was, and the power
button shutting down cleanly."""
import re, struct, subprocess, time
from vm import BUILD, VM, WORK, check, disk_copy, finish


def lid_table(path):
    """An SSDT with a lid, Device (LID0) { Name (_HID, EisaId ("PNP0C0D")) Method (_LID) { Return (One) } },
    assembled by hand. QEMU has no lid of its own."""
    hid = b'\x08_HID\x0c' + bytes([0x41, 0xD0, 0x0C, 0x0D])
    method = b'\x14\x08_LID\x00\xa4\x01'
    body = b'LID0' + hid + method
    aml = b'\x5b\x82' + bytes([len(body) + 1]) + body
    header = struct.pack('<4sIBB6s8sI4sI', b'SSDT', 36 + len(aml), 2, 0, b'AXIMOS', b'LIDTEST ', 1, b'AXIM', 1)
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
v = VM(disk=img, extra=['-acpitable', 'file=' + lid_table(WORK + '/lid.aml')])
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    check("the firmware's code runs", "acpi: running the firmware's code" in v.raw(), v.raw())
    check('and says the lid is open', "acpi: running the firmware's code, lid open" in v.raw(), v.raw())
    v.run('echo kept > /home/button.txt')
    v.command('system_powerdown')
    check('pressing it shuts down', v.exited(15), v.raw()[-600:])
    check('as the kernel says', 'acpi: power button pressed' in v.raw() and 'kernel: powering off' in v.raw(), v.raw()[-600:])
finally:
    v.close()
check('with what was written kept', subprocess.run(['mtype', '-i', img, '::/home/button.txt'],
                                                   capture_output=True, text=True).stdout == 'kept\n')
finish()
