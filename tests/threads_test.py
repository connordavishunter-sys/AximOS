"""Runs programs/threads.c on four CPUs and on one."""
import os
from vm import VM, WORK, check, compile_program, disk_copy, finish, program_checks, put

HERE = os.path.dirname(os.path.abspath(__file__))

img = disk_copy('threads')
put(img, compile_program(HERE + '/programs/threads.c', WORK + '/threads', static=True), '/system/bin/threads')
for cpus in (4, 1):
    v = VM(disk=img, smp=cpus)
    try:
        check(f'{cpus} CPUs: prompt', v.wait('/$ ', 30), v.text())
        out = program_checks(v, f'threads {cpus}', 'threads: ', 120, f'{cpus} CPUs: ')
        check(f'{cpus} CPUs: exec after threads', 'exec after threads' in out, out)
    finally:
        v.close()
finish()
