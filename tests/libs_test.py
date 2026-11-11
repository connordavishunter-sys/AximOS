"""Checks dynamic linking: programs with their own libraries, libc.so's data, dlopen and errors."""
import os
from vm import VM, WORK, check, compile_library, compile_program, disk_copy, finish, program_checks, put

P = os.path.dirname(os.path.abspath(__file__)) + '/programs/'
color = compile_library(P + 'libcolor.c', WORK + '/libcolor.so')
shape = compile_library(P + 'libshape.c', WORK + '/libshape.so', color)
plugin = compile_library(P + 'plugin.c', WORK + '/plugin.so')
missing = compile_library(P + 'libmissing.c', WORK + '/libmissing.so')

img = disk_copy('libs')
put(img, color, '/system/lib/libcolor.so')
put(img, shape, '/system/lib/libshape.so')
put(img, plugin, '/tmp/plugin.so')
put(img, compile_program(P + 'libs.c', WORK + '/libs', libraries=[shape], export=True), '/system/bin/libs')
put(img, compile_program(P + 'needs_missing.c', WORK + '/needs_missing', libraries=[missing]), '/system/bin/needs_missing')
put(img, compile_program(P + 'threads.c', WORK + '/threads'), '/system/bin/threads')
put(img, compile_program(P + 'static_dl.c', WORK + '/static_dl', static=True), '/system/bin/static_dl')

v = VM(disk=img)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    out = program_checks(v, 'libs', 'libs: ')
    check('library destructor runs at exit', 'libshape destructor ran' in out, out)
    o = v.run('libs > /tmp/out; echo st=$?; cat /tmp/out | tail -1')
    check('output through a pipe is flushed at exit', 'st=0' in o and 'libshape destructor ran' in o, o)

    program_checks(v, 'static_dl', 'static_dl: ')
    o = v.run('needs_missing; echo st=$?')
    check('missing library is reported', 'libmissing.so' in o and 'should not run' not in o and 'st=127' in o, o)
finally:
    v.close()

# Threads in a dynamically linked program.
v = VM(disk=img)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    program_checks(v, 'threads 4', 'threads: ', 120, 'dynamic: ')
    # Last, as the disk can't start a shell afterwards.
    o = v.run('mv /system/lib/libc.so /system/lib/libc.off; ls /; echo st=$?')
    check('without libc.so programs stop cleanly', 'st=127' in o, o)
finally:
    v.close()
finish()
