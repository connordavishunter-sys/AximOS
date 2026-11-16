"""Checks the first boot asks how the desktop should look and for an account, that
locking and logging out need the password to get back in, and that the next boot
only asks to log in."""
import re, time
from vm import VM, WORK, check, disk_copy, finish, get

W, H = 1280, 800
TERM = (300, 300) # inside the terminal Super+Enter opens
img = disk_copy('login')


def last(v, pattern):
    return tuple(map(int, re.findall(pattern, v.text())[-1]))


def click(v, what):
    """Clicks a widget on the log-in screen, which is in the middle of the screen."""
    w, h = last(v, r'login: [a-z ]+, (\d+)x(\d+)')
    x, y, rw, rh = last(v, rf'login: at {what} (\d+) (\d+) (\d+) (\d+)')
    at = ((W - w) // 2 + x + rw // 2, (H - h) // 2 + y + rh // 2)
    v.pointer(*at, 1); v.pointer(*at, 0)


v = VM(disk=img, desktop=True, vmmouse=True)
try:
    check('the first boot welcomes', v.wait('login: welcome', 40), v.text())
    time.sleep(1)
    w, h, px = v.screenshot()
    locked_bar = px(700, H - 10)
    at = v.mark()
    v.key('down')
    check('the theme can be chosen', v.wait('login: theme Light', 5, at), v.text(at))
    click(v, 'wallpaper Dunes')
    check('and the wallpaper', v.wait('login: wallpaper /system/share/wallpapers/dunes.jpg', 5, at), v.text(at))
    click(v, 'next')
    check('then the account', v.wait('login: create', 5, at), v.text(at))
    v.key('meta_l-ret') # only Enter, for the log-in screen, while it's locked
    check('which needs a username', v.wait('login: A username is', 5, at), v.text(at))
    v.type('tester\tsecret1\tsecret2\n')
    check('whose passwords must be the same', v.wait("login: The passwords aren't the same.", 10, at), v.text(at))
    v.type('secret1\tsecret1\n')
    check('which it makes', v.wait('login: created tester', 10, at), v.text(at))
    check('and logs in to', v.wait('wm: unlocked', 5, at), v.text(at))
    time.sleep(1)
    w, h, px = v.screenshot()
    check('showing the taskbar', px(700, H - 10) != locked_bar, hex(px(700, H - 10)))
    desktop = px(*TERM)

    v.key('meta_l-ret'); time.sleep(2)
    w, h, px = v.screenshot()
    term = px(*TERM)
    at = v.mark()
    v.type('sleep 2; guidemo\n') # in the terminal, to open a window while it's locked
    v.key('meta_l-l')
    check('Super+L locks it', v.wait('login: locked', 5, at), v.text(at))
    time.sleep(1)
    w, h, px = v.screenshot()
    wallpaper = px(*TERM)
    check('hiding the windows', wallpaper != term, hex(wallpaper))
    check('of which Super+Enter while it was locked opened none', wallpaper == desktop, (hex(wallpaper), hex(desktop)))
    time.sleep(2)
    v.type('wrong\n')
    check("a window opened meanwhile doesn't take what's typed", v.wait('login: wrong', 10, at), v.text(at))
    v.type('secret1\n')
    check('and the right one unlocks it', v.wait('wm: unlocked', 10, at), v.text(at))
    time.sleep(1)
    w, h, px = v.screenshot()
    check('with the windows shown again', px(*TERM) != wallpaper, hex(px(*TERM)))

    at = v.mark()
    v.key('meta_l'); time.sleep(0.5)
    v.key('up'); v.key('up'); v.key('up'); v.key('ret') # Shut down, Reboot, then Log out
    check('Log out in the menu', v.wait('wm: logged out', 5, at), v.text(at))
    check('goes back to logging in', v.wait('login: log in', 5, at), v.text(at))
    v.type('nobody\tsecret1\n')
    check('which needs the right name', v.wait('login: wrong', 10, at), v.text(at))
    v.key('shift-tab'); v.key('ctrl-a'); v.type('tester\tsecret1\n')
    check('as well as the password', v.wait('login: logged in tester', 10, at), v.text(at))
    time.sleep(1)
    w, h, px = v.screenshot()
    check('and the programs are gone', px(*TERM) == wallpaper, hex(px(*TERM)))
finally:
    v.close()

get(img, '/system/etc/account', f'{WORK}/account')
get(img, '/system/etc/gui.conf', f'{WORK}/gui.conf')
account = open(f'{WORK}/account').read()
check('the account keeps a hash, not the password', account.startswith('tester ') and 'secret1' not in account
      and len(account.split()) == 3, account)
check('and the theme is kept', 'theme light' in open(f'{WORK}/gui.conf').read(), open(f'{WORK}/gui.conf').read())

v = VM(disk=img, desktop=True)
try:
    check('the next boot only asks to log in', v.wait('login: log in', 40), v.text())
    check('without welcoming again', 'login: welcome' not in v.text(), v.text())
finally:
    v.close()

# Without the desktop, ttylogin asks at the terminal for the same account, and
# axfetch then says who's logged in.
v = VM(disk=img)
try:
    check('the shell asks to log in', v.wait('AximOS login: ', 40), v.text())
    at = v.mark()
    v.type('tester\n'); v.wait('Password: ', 5, at); v.type('wrong\n')
    check('a wrong password is turned away', v.wait('Wrong username or password.\n\nAximOS login: ', 10, at), v.text(at))
    check("and isn't shown", 'wrong' not in v.text(at), v.text(at))
    at = v.mark()
    v.type('tester\n'); v.wait('Password: ', 5, at); v.type('secret1\n')
    check('the right one logs in', v.wait('ttylogin: logged in tester', 10, at) and v.wait('/$ ', 5, at), v.text(at))
    o = v.run('axfetch')
    check('axfetch has the username', 'tester@' in o, o)
finally:
    v.close()
finish()
