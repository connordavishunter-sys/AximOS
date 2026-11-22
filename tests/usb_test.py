"""Checks USB sticks on an xHCI controller: one with FAT32 is read and written, one
with the ISO written to it is a disc until it's partitioned, and the ISO on a stick
boots by itself and installs onto a disk."""
import subprocess, time
from vm import BUILD, VM, WORK, check, finish, quiet


def stick(path, bus, port, boot=False):
    return ['-drive', f'file={path},format=raw,if=none,id=stick{port}',
            '-device', f'usb-storage,bus={bus}.0,port={port},drive=stick{port}' + (',bootindex=0' if boot else '')]


fat = f'{WORK}/fat.img'
quiet('mkfs.fat', '-F', '32', '-C', fat, str(300 * 1024))
open(f'{WORK}/hello.txt', 'w').write('hi from usb\n')
quiet('mcopy', '-i', fat, f'{WORK}/hello.txt', '::/')
image = f'{WORK}/image.img'
quiet('cp', '--sparse=always', BUILD + '/aximos.iso', image)

v = VM(extra=['-device', 'qemu-xhci,id=usb', *stick(fat, 'usb', 1), *stick(image, 'usb', 2)])
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    o = v.run('cat /proc/disks /proc/mounts')
    check('the sticks are disks', 'sdb 614400 512 disk\n' in o and 'sdc ' in o, o)
    check('mounted as FAT32 and as a disc', '/dev/sdb /dev/sdb fat32' in o and '/dev/sdc /dev/sdc iso9660' in o, o)
    o = v.run('cat /dev/sdb/hello.txt; echo written > /dev/sdb/new.txt; cat /dev/sdb/new.txt')
    check('which can be read and written', '\nhi from usb\nwritten\n' in o, o)
    o = v.run('disk partition sdc -y; disk format sdc1 -y; cat /proc/mounts', 30)
    check('the disc image goes once the stick is partitioned', '/dev/sdc1 /dev/sdc1 fat32' in o and '/dev/sdc /dev/sdc' not in o, o)
finally:
    v.close()
check('what was written is on the stick',
      subprocess.run(['mtype', '-i', fat, '::/new.txt'], capture_output=True, text=True).stdout == 'written\n')

# The ISO on a stick, with a blank disk to install on and no CD drive.
target = f'{WORK}/target.img'
open(target, 'wb').truncate(1 << 30)
v = VM(disk=False, cdrom=False, desktop=True,
       extra=['-device', 'qemu-xhci,id=usb', *stick(BUILD + '/aximos.iso,snapshot=on', 'usb', 1, boot=True),
              '-drive', f'file={target},format=raw,if=none,id=d1', '-device', 'ide-hd,drive=d1,bus=ide.1'])
try:
    check('the stick boots into the installer', v.wait('install: choose', 60), v.text())
    check('running from the stick', 'vfs: /dev/sdb is the root filesystem' in v.raw(), v.raw())
    time.sleep(1)
    at = v.mark()
    v.key('ret')
    check('which asks before erasing', v.wait('install: confirm', 5, at), v.text(at))
    v.key('ret')
    check('and installs', v.wait('install: done', 240, at), v.text(at))
finally:
    v.close()

v = VM(disk=target, cdrom=False, desktop=True)
try:
    check('the installed disk boots to first-boot setup', v.wait('login: welcome', 60), v.text())
finally:
    v.close()
finish()
