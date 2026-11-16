"""Checks /tmp, which is kept in memory: files and folders, large files, holes,
truncating, renaming, removing, running a program from it, df, and that it
starts empty at the next boot while the disk keeps what's written to it."""
import random, re
from vm import VM, WORK, check, disk_copy, finish, get, put

# 3 MB, more than one index page of data.
img = disk_copy('tmpfs')
big = WORK + '/big.bin'
open(big, 'wb').write(random.Random(2).randbytes(3_000_000))
put(img, big, '/big.bin')
v = VM(disk=img)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    o = v.run('cat /proc/mounts; df')
    check('mounted on /tmp', 'tmpfs /tmp tmpfs' in o, o)
    check('and df shows it', 'tmpfs' in o and '/tmp\n' in o, o)

    v.run('mkdir /tmp/d /tmp/d/inner; echo hello > /tmp/d/a.txt; echo more >> /tmp/d/a.txt')
    o = v.run('cat /tmp/d/a.txt; ls /tmp/d')
    check('files and folders', 'hello\nmore\n' in o and 'a.txt' in o and 'inner' in o, o)
    o = v.run('cat /tmp/d/A.TXT')
    check('names keep their case', 'No such file' in o, o)

    # Copied in, then back out to the disk, to compare after.
    v.run('cp /big.bin /tmp/d/big; cp /tmp/d/big /tmp/big; cp /tmp/big /home/back.bin', 10)
    o = v.run('wc -c /tmp/d/big')
    check('a large file', '3000000 /tmp/d/big' in o, o)

    o = v.run('head -c 10 /tmp/d/big > /tmp/d/small; wc -c /tmp/d/small; echo x > /tmp/d/big; wc -c /tmp/d/big')
    check('truncating', '10 /tmp/d/small' in o and '2 /tmp/d/big' in o, o)

    v.run('mv /tmp/d/a.txt /tmp/d/inner/b.txt')
    o = v.run('cat /tmp/d/inner/b.txt; ls /tmp/d')
    check('renaming into a folder', 'hello' in o and 'a.txt' not in o, o)
    o = v.run('mv /tmp/d /tmp/d/inner/d')
    check("a folder can't go inside itself", 'Invalid' in o, o)
    o = v.run('rmdir /tmp/d')
    check("a folder with files can't be removed", 'not empty' in o.lower(), o)

    v.run('cp /system/bin/hello /tmp/hi')
    o = v.run('/tmp/hi')
    check('a program runs from it', 'ello' in o, o)

    o = v.run('mv /tmp/d/inner/b.txt /home/b.txt; cat /home/b.txt; ls /tmp/d/inner')
    check('moving a file to the disk', 'hello' in o and 'b.txt' not in o.split('ls')[-1], o)
    o = v.run('rm -r /tmp/d /tmp/big /tmp/hi; ls /tmp | wc -l')
    check('removing it all', re.search(r'\n\s*0\n', o), o)

    v.run('echo gone > /tmp/gone')
    v.type('shutdown\n')
    check('shutdown powers off', v.exited(30), v.text()[-300:])
finally:
    v.close()

# After a reboot /tmp is empty again, but what was moved to the disk is there.
v = VM(disk=img)
try:
    check('prompt after reboot', v.wait('/$ ', 30), v.text())
    o = v.run('ls /tmp | wc -l; cat /home/b.txt')
    check('/tmp starts empty', re.search(r'\n\s*0\n', o), o)
    check('and the disk kept the file', 'hello' in o, o)
    v.type('shutdown\n'); v.exited(30)
finally:
    v.close()
get(img, '/home/back.bin', WORK + '/back.bin')
check('and has every byte of it', open(WORK + '/back.bin', 'rb').read() == open(big, 'rb').read(), 'differs')
finish()
