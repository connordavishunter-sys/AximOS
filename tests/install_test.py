"""Checks disk lists, partitions and formats drives, and that the ISO boots by itself
into the installer, which installs onto a blank disk that then boots without it."""
import time
from vm import VM, WORK, check, finish


def blank(name, mib):
    path = f'{WORK}/{name}.img'
    open(path, 'wb').truncate(mib << 20)
    return path


def drive(path, bus):
    return ['-drive', f'file={path},format=raw,if=none,id={bus}', '-device', f'ide-hd,drive={bus},bus=ide.{bus[-1]}']


# A second drive beside the system's, as sdb.
v = VM(extra=drive(blank('spare', 256), 'd3'))
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    o = v.run('cat /proc/disks')
    check('/proc/disks lists the drives', 'sda 1048576 512 disk\n' in o and 'sdb 524288 512 disk\n' in o, o)
    o = v.run('disk partition sdb 64 rest -y; disk format sdb1 BOOT -y; disk format sdb2 -y', 20)
    check('disk partitions and formats', 'partitioned sdb' in o and 'formatted sdb2 as FAT32' in o, o)
    o = v.run('disk')
    check('and lists what it made', 'sdb1     64.0 MiB  BOOT' in o and 'sdb2' in o and 'AXIMOS' in o, o)
    o = v.run('echo hello > /dev/sdb2/note.txt; cat /dev/sdb2/note.txt; cat /proc/mounts')
    check('which is mounted at once', 'hello\n' in o and '/dev/sdb1 /dev/sdb1 fat32' in o, o)
    o = v.run('disk format sda -y')
    check("but the system's drive is left alone", 'sda is in use' in o, o)
finally:
    v.close()

# The ISO alone, with a blank disk to install on.
target = blank('target', 1024)
v = VM(disk=False, desktop=True, extra=drive(target, 'd1'))
try:
    check('the ISO boots by itself into the installer', v.wait('install: choose', 60), v.text())
    check('running from the disc', 'vfs: /dev/sr0 is the root filesystem' in v.raw(), v.raw())
    time.sleep(1)
    at = v.mark()
    v.key('ret')
    check('which asks before erasing', v.wait('install: confirm', 5, at), v.text(at))
    v.key('ret')
    check('then copies the system', v.wait('install: copying', 30, at), v.text(at))
    check('and finishes', v.wait('install: done', 240, at), v.text(at))
finally:
    v.close()

# The installed disk, without the ISO.
v = VM(disk=target, cdrom=False, desktop=True)
try:
    check('the installed disk boots to first-boot setup', v.wait('login: welcome', 60), v.text())
    check('from its partition', 'vfs: /dev/sda1 is the root filesystem' in v.raw(), v.raw())
finally:
    v.close()
finish()
