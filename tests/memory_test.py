"""Checks what processes cost: shared code, stacks given memory as they're used, and
running out of memory."""
import os, re
from vm import VM, WORK, check, compile_program, disk_copy, finish, program_checks, put

P = os.path.dirname(os.path.abspath(__file__)) + '/programs/'
img = disk_copy('memory')
put(img, compile_program(P + 'hog.c', WORK + '/hog'), '/system/bin/hog')
put(img, compile_program(P + 'stack.c', WORK + '/stack'), '/system/bin/stack')

v = VM(disk=img)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())

    # Each process used to have its own 104 kB of libc.so and a 64 kB stack.
    free = lambda: int(re.findall(r'MemFree:\s+(\d+)', v.run('cat /proc/meminfo'))[-1])
    before = free()
    v.run('for i in 1 2 3 4 5 6 7 8 9 10; do sleep 1000 & done', 2)
    each = (before - free()) / 10
    check('a sleeping process costs under 120 kB', each < 120, f'{each:.0f} kB per process')
    v.run('kill %1 %2 %3 %4 %5 %6 %7 %8 %9 %10; wait', 2)

    program_checks(v, 'stack', 'stack: ')
    o = v.run('stack overflow; echo st=$?', 3)
    check('stack overflow is a segfault', 'st=139' in o, o)

    o = v.run('cp /system/bin/echo /tmp/prog; /tmp/prog first; cp /system/bin/wc /tmp/prog; echo a b c | /tmp/prog -w')
    check('a rewritten program runs as its new self', 'first' in o and re.search(r' 3\n', o), o)

    # Running out of memory takes back what's cached, and nothing is lost doing it.
    o = v.run('hog; ls /system/lib; hog', 30)
    sizes = list(map(int, re.findall(r'hog: (\d+) MB', o)))
    check('cache gives memory back', len(sizes) == 2 and sizes[0] > 150 and abs(sizes[0] - sizes[1]) <= 2
          and 'libc.so' in o, o)
    o = v.run('echo after; stack | tail -1'); check('programs run after memory ran out', 'stack: done' in o, o)
finally:
    v.close()
finish()
