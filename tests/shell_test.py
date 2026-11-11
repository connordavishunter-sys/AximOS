"""Checks a dash session at the console: builtins, programs, job control and the terminal."""
import time

from vm import VM, check, finish

v = VM()
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())

    o = v.run('cd /system/etc'); check('cd changes the prompt', '/system/etc$ ' in o, o)
    o = v.run('pwd'); check('pwd', '\n/system/etc\n' in o, o)
    v.run('cd /')
    o = v.run('echo $SHELL $HOME $PATH'); check('environment from init', '/system/bin/dash / /system/bin' in o, o)
    o = v.run('nosuchcommand'); check('unknown command', 'not found' in o, o)
    o = v.run('echo piped | cat | cat'); check('pipes', '\npiped\n' in o, o)
    v.run('echo hi > /tmp/x')
    o = v.run('cat < /tmp/x'); check('redirects', '\nhi\n' in o, o)
    v.run('export FOO=bar')
    o = v.run('env'); check('export reaches programs', 'FOO=bar' in o, o)

    # sh is an alias for dash, under both names scripts use.
    o = v.run('/bin/sh -c "echo via-bin-sh"'); check('/bin/sh runs dash', '\nvia-bin-sh\n' in o, o)
    o = v.run('/system/bin/sh -c "echo via-system-sh"'); check('/system/bin/sh runs dash', '\nvia-system-sh\n' in o, o)
    v.run("printf '#!/bin/sh\\necho script-ran $1\\n' > /tmp/s")
    o = v.run('/tmp/s arg')
    check('#!/bin/sh script', 'script-ran arg' in o, o)

    v.type('cat\n'); time.sleep(0.8); v.key('ctrl-c'); time.sleep(0.5)
    o = v.run('echo after-ctrl-c'); check('ctrl+c stops the program, not the shell', '\nafter-ctrl-c\n' in o, o)
    v.type('sleep 100\n'); time.sleep(0.8); v.key('ctrl-c'); time.sleep(0.5)
    o = v.run('echo status=$?'); check('ctrl+c exit status', 'status=130' in o, o)

    v.run('sleep 100 &', 0.6)
    o = v.run('jobs'); check('background job', 'sleep 100' in o, o)
    v.run('kill %1', 0.6)

    # more turns echo off, so ctrl+c there shows no ^C: check how it exits instead.
    v.run('i=0; while [ $i -lt 120 ]; do i=$((i+1)); echo line $i; done > /tmp/lines', 2)
    at = v.mark(); v.type('more /tmp/lines\n')
    check('more stops at its prompt', v.wait('--More--', 5, at), v.text(at))
    v.key('ctrl-c'); time.sleep(0.5)
    o = v.run('echo status=$?'); check('ctrl+c stops more', 'status=130' in o, o)
    o = v.run('ps'); check('more is gone', 'more' not in o, o)
    o = v.run('echo echo-back'); check('terminal echo comes back', 'echo echo-back' in o, o)

    o = v.run('ps'); check('ps shows dash', '/system/bin/dash' in o, o)
    o = v.run('axfetch'); check('axfetch names the shell', 'Shell: dash' in o, o)
    v.type('ech\t'); v.type('tabbed\n'); time.sleep(0.6)
    at = v.mark(); v.key('up'); v.key('ret'); time.sleep(0.6)
    check('tab completion and history', '\ntabbed' in v.text(at), v.text(at))

    v.type('text /tmp/note\n'); time.sleep(1); v.type('words'); v.key('ctrl-s'); time.sleep(0.5); v.key('ctrl-q'); time.sleep(0.6)
    o = v.run('cat /tmp/note'); check('text editor', 'words' in o, o)
    v.run('top -n 2 -d 0.2', 1.5)
    o = v.run('echo after-top'); check('top exits', '\nafter-top\n' in o, o)

    at = v.mark(); v.type('exit\n')
    check('exit starts a new shell', v.wait('/$ ', 3, at), v.text(at))
    v.key('ctrl-d'); time.sleep(1)
    o = v.run('echo after-eof'); check('ctrl+d starts a new shell', '\nafter-eof\n' in o, o)
finally:
    v.close()
finish()
