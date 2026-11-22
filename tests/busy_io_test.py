"""Checks the system keeps up while the disk is busy: two programs writing and
reading back big files mustn't hold up looking at files, /proc, /tmp or starting
programs, for which every process waits its turn at the filesystem."""
import os
from vm import VM, WORK, check, compile_program, disk_copy, finish, program_checks, put

img = disk_copy('busy_io')
put(img, compile_program(os.path.dirname(__file__) + '/programs/busy_io.c', WORK + '/busy_io'), '/system/bin/busy_io')

v = VM(disk=img)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    program_checks(v, 'busy_io', 'busy_io: done', 120)
finally:
    v.close()
finish()
