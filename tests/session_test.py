"""Checks the desktop's session: the first boot makes the account on the log-in screen,
which logs in to wm running as that account, which opens apps, locks behind its
password, gives the screen to a text console on Ctrl+Alt+F2 and back on Ctrl+Alt+F1,
and logs out to the log-in screen, which logs in again."""
import time
from vm import VM, center, check, finish

W, H = 1280, 800
v = VM()
try:
    v.desktop_login()
    out = v.text()
    check("the log-in screen ran as the greeter, and the desktop as the account",
          'uid: 999' in out and 'uid: 1000' in out, out[-1500:])

    v.login() # the serial port's own login, as the same account
    out = v.run('id; ps')
    check('the account is an admin', '(admin)' in out, out)
    check('and wm runs as it', 'tester' in out and 'wm' in out and 'session' in out, out)

    at = v.mark()
    v.key('meta_l-ret')
    check('Super+Enter opens the terminal', v.wait('term: tab 1', 10, at), v.text(at))
    time.sleep(1)
    v.keys('id > /tmp/from-term\n')
    time.sleep(1)
    out = v.run('cat /tmp/from-term')
    check('which runs as the account', 'uid=1000(tester)' in out, out)

    at = v.mark()
    v.key('meta_l-l')
    check('Super+L locks it', v.wait('login: locked', 10, at), v.text(at))
    time.sleep(0.5)
    v.keys('wrong\n')
    check("not unlocking with the wrong password", v.wait("login: wrong", 10, at), v.text(at))
    v.keys('secret\n')
    check('but with the right one', v.wait('wm: unlocked', 10, at), v.text(at))

    time.sleep(1)
    taskbar = v.screenshot()[2](700, H - 10)
    v.key('ctrl-alt-f2')
    time.sleep(1.5)
    w, h, px = v.screenshot()
    check('Ctrl+Alt+F2 shows a text console', px(700, H - 10) != taskbar, hex(px(700, H - 10)))
    v.keys('tester\n')
    time.sleep(0.5)
    v.keys('secret\n')
    time.sleep(1)
    v.keys('echo console > /tmp/console\n')
    time.sleep(1)
    out = v.run('cat /tmp/console')
    check('which logs in, on the keyboard', 'console' in out, out)
    v.key('ctrl-alt-f1')
    time.sleep(1.5)
    check('Ctrl+Alt+F1 brings the desktop back', v.screenshot()[2](700, H - 10) == taskbar, '')
    at = v.mark()
    v.key('meta_l-ret')
    check('with its keyboard', v.wait('term: tab 1', 10, at), v.text(at))

    at = v.mark()
    v.key('meta_l-shift-q')
    check('Super+Shift+Q logs out', v.wait('session: tester logged out', 15, at), v.text(at))
    check('to the log-in screen', v.wait('login: log in', 15, at), v.text(at))
    time.sleep(1)
    v.keys('tester\twrong\n')
    check('which wants the right password', v.wait('login: wrong', 10, at), v.text(at))
    v.keys('secret\n')
    check('and logs in again', v.wait('session: tester logged in', 10, at) and v.wait('wm: running', 10, at), v.text(at))
finally:
    v.close()

finish()
