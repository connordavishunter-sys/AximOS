"""Checks disk partitions a blank drive with GPT and formats it with FAT32 and ext4,
which mount, keep what's written, and show in disk list by their labels."""
import subprocess
from vm import VM, WORK, check, finish

drive = WORK + '/blank.img'
subprocess.run(['truncate', '-s', '256M', drive], check=True)
v = VM(extra=['-drive', f'file={drive},format=raw,if=none,id=blank', '-device', 'ide-hd,drive=blank,bus=ide.1'])
try:
    v.login()
    out = v.run('disk')
    check('disk lists the blank drive', 'sda' in out and '256.0 MiB' in out and 'sr0' in out, out)
    out = v.run('disk partition sda 64 rest -y')
    check('which only root can change', 'Permission denied' in out, out)
    out = v.sudo('disk partition sda 64 rest -y')
    check('partitions it', 'partitioned sda' in out, out)
    out = v.sudo('disk format sda1 fat32 BOOT -y && disk format sda2 ext4 DATA -y', 6)
    check('formats each', 'sda1 as FAT32' in out and 'sda2 as ext4' in out, out)
    out = v.run('disk')
    check('which disk list shows', 'vfat BOOT' in out and 'ext4 DATA' in out, out)
    out = v.sudo('mount /dev/sda2 /mnt && echo kept > /mnt/note && mount /dev/sda1 /mnt/note 2>&1; disk format sda2 ext4 -y')
    check("one that's mounted can't be formatted", 'in use' in out, out)
    out = v.sudo('umount /mnt && mount -t ext4 /dev/sda2 /mnt && cat /mnt/note && df')
    check('and keeps what was written', 'kept' in out and '/dev/sda2' in out, out)
    out = v.sudo('umount /mnt && mount /dev/sda1 /mnt && echo fat > /mnt/f && cat /mnt/f && mount')
    check('FAT32 mounts too, found by itself', 'fat' in out and '/dev/sda1 /mnt vfat' in out, out)
finally:
    v.close()

finish()
