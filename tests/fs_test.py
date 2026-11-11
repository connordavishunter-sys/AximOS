"""Checks FAT32: everyday file operations, long names, large files, the journal and fsck."""
import hashlib, os, random, subprocess
from vm import VM, WORK, check, disk_copy, finish, get, put


def fsck(img):
    r = subprocess.run(['fsck.fat', '-n', img], capture_output=True, text=True)
    return check('fsck finds the disk clean', r.returncode == 0, r.stdout)


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


img = disk_copy('fs')
fsck(img)
big = WORK + '/big.bin'
open(big, 'wb').write(random.Random(1).randbytes(700_000))
put(img, big, '/big.bin')

v = VM(disk=img)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    v.run('mkdir /d1 /d1/inner')
    v.run('echo hello > "/d1/A Long File Name With Spaces.txt"')
    o = v.run('ls /d1'); check('long name listed', 'A Long File Name With Spaces.txt' in o, o)
    o = v.run('cat "/d1/a long file name with spaces.TXT"'); check('names are case-insensitive', '\nhello\n' in o, o)
    v.run('mv "/d1/A Long File Name With Spaces.txt" /d1/inner/moved.txt')
    o = v.run('cat /d1/inner/moved.txt'); check('mv into a directory', '\nhello\n' in o, o)
    v.run('echo more >> /d1/inner/moved.txt')
    o = v.run('cat /d1/inner/moved.txt'); check('append', 'hello\nmore\n' in o, o)
    v.run('cp /big.bin /d1/copy.bin', 3)
    o = v.run('wc -c /d1/copy.bin'); check('large copy has every byte', '700000' in o, o)
    # Enough long names to need more than one directory cluster.
    v.run('i=10; while [ $i -lt 50 ]; do touch /d1/file-number-$i; i=$((i+1)); done', 3)
    o = v.run('ls /d1 | wc -l'); check('40 files in one directory', ' 42\n' in o, o)
    v.run('rm /d1/file-number-1*')
    o = v.run('ls /d1 | wc -l'); check('rm with a pattern', ' 32\n' in o, o)
    v.run('mkdir /gone; rmdir /gone')
    o = v.run('ls /'); check('rmdir', 'gone' not in o, o)
    o = v.run('rmdir /d1'); check('rmdir refuses a full directory', 'not empty' in o.lower(), o)

    o = v.run('ls /'); check('journal hidden from ls', '.metadata' not in o, o)
    o = v.run('ls -a /'); check('ls -a shows the journal', '.metadata' in o, o)
    for command in ('rm /.metadata/journal', 'touch /.metadata/x', 'echo x > /.metadata/journal', 'mv /.metadata /m'):
        o = v.run(command); check(command + ' is refused', 'ermitted' in o, o)
    o = v.run('df /'); check('df', 'fat32' in o, o)

    v.type('shutdown\n')
    check('shutdown powers off', v.exited(30), v.text()[-300:])
finally:
    v.close()

fsck(img)
get(img, '/d1/copy.bin', WORK + '/copy.bin')
check('large copy matches byte for byte', sha(WORK + '/copy.bin') == sha(big))
get(img, '/d1/inner/moved.txt', WORK + '/moved.txt')
check('host reads the long-name file', open(WORK + '/moved.txt').read() == 'hello\nmore\n')

# What was written is there after a reboot.
v = VM(disk=img)
try:
    check('prompt after reboot', v.wait('/$ ', 30), v.text())
    o = v.run('cat /d1/inner/moved.txt'); check('file survives reboot', 'hello\nmore\n' in o, o)
    o = v.run('ls /d1 | wc -l'); check('directory survives reboot', ' 32\n' in o, o)
    v.run('rm -r /d1', 2)
    o = v.run('ls /'); check('rm -r', 'd1' not in o, o)
    v.type('shutdown\n'); v.exited(30)
finally:
    v.close()
fsck(img)
finish()
