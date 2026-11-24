"""Checks install takes over a whole drive, with an ESP and the system after it, and the
development tools from their archive on the disc, which the live system leaves out, and
that it won't install where it shouldn't."""
import subprocess
from vm import VM, WORK, check, finish

drive = WORK + '/drive.img'
subprocess.run(['truncate', '-s', '6G', drive], check=True)
v = VM(extra=['-drive', f'file={drive},format=raw,if=none,id=target', '-device', 'nvme,serial=target,drive=target'])
try:
    v.login()
    out = v.sudo('aximos-install nvme0n1 free owner -y')
    check("there's no free space on a blank drive", 'no GPT' in out, out)
    out = v.sudo('aximos-install nvme0n1 again -y')
    check('nor an AximOS to install again', 'no AximOS' in out, out)
    at = v.mark()
    v.type('sudo aximos-install nvme0n1 whole owner -y\n')
    v.wait('Password for ', 5, at)
    v.type('secret\n')
    v.wait('New password', 10, at)
    v.type('pass\npass\n')
    check('installs over the whole drive', v.wait('aximos-install: done', 180, at), v.text(at))
    check('adding the development tools from the disc', 'adding the development tools' in v.text(at), v.text(at))
    look = 'mount /dev/nvme0n1p2 /mnt && ls /mnt/system/include/stdio.h /mnt/system/lib/crt1.o; umount /mnt'
    out = v.sudo(look)
    check("glibc's headers and start files among them", '/mnt/system/include/stdio.h' in out and 'crt1.o' in out
          and 'No such' not in out, out)
    out = v.run('ls /system/include 2>&1')
    check("which the live system hasn't in memory", 'No such file' in out, out)
    at = v.mark()
    v.type('sudo aximos-install nvme0n1 again --no-dev -y\n')
    v.wait('Password for ', 5, at)
    v.type('secret\n')
    check('installing again', v.wait('aximos-install: done', 180, at), v.text(at))
    out = v.sudo(look)
    check('without them, with --no-dev', 'No such file' in out, out)
finally:
    v.close()

out = subprocess.run(['sfdisk', '-d', drive], capture_output=True, text=True).stdout
check('with an ESP, then the system in the rest',
      'size=      524288, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B' in out and 'type=0FC63DAF-8483-4772-8E79-3D69D8477DE4' in out
      and out.count('type=') == 2, out)
finish()
