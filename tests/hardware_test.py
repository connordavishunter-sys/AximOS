"""Checks what real machines need that QEMU's defaults hide: booting without a working
PIT, the kernel's log on screen and in /proc/log, as a laptop has no serial port to
read it from, and a crash shown on screen with where the kernel was."""
import re, time
from vm import BUILD, VM, check, finish

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
finish()
