"""Checks a parent can't collect a child before it's quite gone: the child's last
thread can sleep while closing its files, as /dev/audio waits for its sound to
play, and collecting it then freed what it was still using, so logging out while
music played stopped the kernel."""
from vm import VM, WORK, check, compile_program, disk_copy, finish, put
import os

img = disk_copy('reap')
put(img, compile_program(os.path.dirname(__file__) + '/programs/reap.c', WORK + '/reap'), '/system/bin/reap')

v = VM(disk=img)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    o = v.run('reap', 60)
    check('each child is collected once it has finished', o.count('collected status 0') == 5 and 'reap: done' in o, o)
    check('and the kernel keeps going', 'exception' not in v.raw(), v.raw()[-800:])
finally:
    v.close()
finish()
