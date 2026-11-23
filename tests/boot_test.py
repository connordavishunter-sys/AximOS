"""Checks Linux boots the ISO to AximOS's init, which mounts what the kernel shares
and runs the shell on the serial console, and turns the computer off when asked."""
from vm import VM, check, finish

v = VM()
try:
    check('the shell starts', v.wait('/# ', 30), v.text())
    check('nothing went wrong on the way', 'init: ' not in v.text() and 'Kernel panic' not in v.raw(), v.text())
    out = v.run('echo $((6 * 7)) $$')
    check('which runs commands, as the first program init started', '42 ' in out, out)
    out = v.run('read version < /proc/version; echo $version')
    check('on Linux', 'Linux version 7.3.5' in out, out)
    out = v.run('[ -c /dev/null ] && [ -d /sys/class ] && [ -d /dev/pts ] && echo mounted')
    check('with /proc, /sys and /dev mounted', 'mounted' in out, out)
    out = v.run('echo hi > /tmp/hi; read word < /tmp/hi; echo got $word')
    check('and somewhere to write', 'got hi' in out, out)
    out = v.run('(while :; do :; done) & jobs; kill %1')
    check('with job control, as the console is its terminal', 'Running' in out, out)
    v.type('kill -USR2 1\n')
    check('SIGUSR2 to init turns it off', v.wait('init: turning off', 10) and v.exited(15), v.text())
finally:
    v.close()

finish()
