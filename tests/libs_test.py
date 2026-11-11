"""Checks dynamic linking: programs with their own libraries, libc.so's data, dlopen and errors,
and that processes share libc.so's pages."""
import os, re
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
put(img, compile_program(P + 'hog.c', WORK + '/hog'), '/system/bin/hog')

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

    # Without sharing, each process has its own 104 kB of libc.so's code and data.
    free = lambda: int(re.findall(r'MemFree:\s+(\d+)', v.run('cat /proc/meminfo'))[-1])
    before = free()
    v.run('for i in 1 2 3 4 5 6 7 8 9 10; do sleep 1000 & done', 2)
    each = (before - free()) / 10
    check('processes share libc.so', each < 200, f'{each:.0f} kB per process')
    v.run('kill %1 %2 %3 %4 %5 %6 %7 %8 %9 %10; wait', 2)

    o = v.run('cp /system/bin/echo /tmp/prog; /tmp/prog first; cp /system/bin/wc /tmp/prog; echo a b c | /tmp/prog -w')
    check('a rewritten program runs as its new self', 'first' in o and re.search(r' 3\n', o), o)

    # Running out of memory takes back what's cached, and nothing is lost doing it.
    o = v.run('hog; ls /system/lib; hog', 30)
    sizes = list(map(int, re.findall(r'hog: (\d+) MB', o)))
    check('cache gives memory back', len(sizes) == 2 and sizes[0] > 150 and abs(sizes[0] - sizes[1]) <= 2
          and 'libc.so' in o, o)
    o = v.run('echo after; ls /system/lib | wc -l'); check('programs run after memory ran out', 'after' in o, o)
    # Last, as the disk can't start a shell afterwards.
    o = v.run('mv /system/lib/libc.so /system/lib/libc.off; ls /; echo st=$?')
    check('without libc.so programs stop cleanly', 'st=127' in o, o)
finally:
    v.close()
finish()
