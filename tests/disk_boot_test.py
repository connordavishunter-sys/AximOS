"""Checks the disk image boots from an NVMe drive, its root on ext4, which init has
e2fsck check before it's made writable, and that what's written there, the first
account among it, is kept through losing power."""
import subprocess
from vm import VM, BUILD, WORK, check, finish

disk = WORK + '/disk.img'
subprocess.run(['cp', '--sparse=always', BUILD + '/aximos.img', disk], check=True)

v = VM(nvme=disk)
try:
    v.login()
    check('e2fsck checks the root first', 'AximOS: clean' in v.raw(), v.raw())
    out = v.run('mount; cat /proc/partitions')
    check('which is then mounted writable', '/dev/root / ext4 rw' in out and 'nvme0n1p2' in out, out)
    out = v.run('echo kept > note; sync; cat note')
    check('so files can be written', 'kept' in out, out)
finally:
    v.close()  # as if the power went

v = VM(nvme=disk)
try:
    check('it boots again', v.wait('login: ', 30), v.text())
    check('without asking for an account again', 'Make an account' not in v.text(), v.text())
    at = v.mark()
    v.type('tester\nsecret\n')
    check('which logs in', v.wait('$ ', 10, at), v.text(at))
    out = v.run('cat note')
    check('and has kept what was written', 'kept' in out, out)
finally:
    v.close()
finish()
