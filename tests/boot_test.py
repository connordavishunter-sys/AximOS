"""Checks Linux boots the ISO to AximOS's init, which mounts what the kernel shares,
starts eudev, and asks on the serial console for the first account, which can then
log in and run commands as root through sudo."""
from vm import VM, check, finish

v = VM()
try:
    check('the first boot asks for an account', v.wait('Make an account', 30), v.text())
    check('nothing went wrong on the way', 'init: ' not in v.text() and 'Kernel panic' not in v.raw(), v.text())
    v.type('-bad\n')
    check('a name is checked', v.wait('starting with a letter', 5), v.text())
    v.type('tester\n')
    v.wait('New password: ', 5)
    v.type('one\ntwo\n')
    check('as is that both passwords match', v.wait('different', 5), v.text())
    v.type('secret\nsecret\n')
    check('then it logs in', v.wait('aximos login: ', 10), v.text())
    v.type('tester\nwrong\n')
    check('not with the wrong password', v.wait('Wrong name or password', 10), v.text())
    at = v.mark()
    v.type('tester\nsecret\n')
    check('but with the right one, to a shell in its home folder', v.wait('/home/tester$ ', 10, at), v.text(at))
    check('with the welcome message', 'Welcome to AximOS' in v.text(at), v.text(at))

    out = v.run('id')
    check('as an admin', 'uid=1000(tester)' in out and '(admin)' in out, out)
    out = v.run('echo $((6 * 7)); uname -sr; hostname')
    check('on Linux, named aximos', '42' in out and 'Linux 7.3.5' in out and 'aximos' in out, out)
    out = v.run('mount')
    check('with /proc, /sys and /dev mounted', all(f' {d} ' in out for d in ('/proc', '/sys', '/dev', '/dev/pts', '/tmp')), out)
    out = v.run('ps')
    check('udevd running as a service', 'udevd' in out and '[kthreadd]' in out, out)
    out = v.run('ls /dev/input; ls -l /dev/null')
    check('which has made the device files', 'event0' in out and 'null' in out, out)
    out = v.run('echo hi > /tmp/hi; cat /tmp/hi; df; free')
    check('somewhere to write, and memory to spare', 'hi' in out and 'tmpfs' in out and 'Mem:' in out, out)
    out = v.run('(while :; do :; done) & jobs; kill %1')
    check('with job control, as the console is its terminal', 'Running' in out, out)
    out = v.run('cat /etc/shadow')
    check("which can't read the passwords", 'Permission denied' in out, out)

    at = v.mark()
    v.type('sudo id\n')
    check('sudo asks for its password', v.wait('Password for tester: ', 5, at), v.text(at))
    out = v.run('secret', 3)
    check('then runs it as root', 'uid=0(root)' in out, out)
    v.type('sudo shutdown\n')
    v.wait('Password for tester: ', 5)
    v.type('secret\n')
    check('sudo shutdown turns it off', v.wait('init: turning off', 10) and v.exited(15), v.text())
finally:
    v.close()

finish()
