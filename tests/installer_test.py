"""Checks the installer app installs AximOS from the desktop, asking for the drive,
the first account, and the password to install as root, and showing how far it's got."""
import subprocess, time
from vm import VM, WORK, check, finish

drive = WORK + '/drive.img'
subprocess.run(['truncate', '-s', '6G', drive], check=True)
v = VM(extra=['-drive', f'file={drive},format=raw,if=none,id=target', '-device', 'nvme,serial=target,drive=target'])
try:
    v.desktop()
    at = v.mark()
    v.start('/system/bin/gui/System/installer')
    check('the installer opens on choosing the drive', v.wait('installer: choose', 10, at), v.text(at))
    time.sleep(0.5)
    at = v.mark()
    v.keys('\n')
    check('then asks for the account', v.wait('installer: account', 5, at), v.text(at))
    time.sleep(0.3)
    at = v.mark()
    v.keys('owner\tpass\tother\n')
    check("which needs the passwords the same", v.wait("passwords aren't the same", 5, at), v.text(at))
    time.sleep(0.3)
    at = v.mark()
    v.keys('pass\tpass\n')
    check('then asks to go on', v.wait('installer: confirm', 5, at), v.text(at))
    time.sleep(0.3)
    at = v.mark()
    v.keys('secret\n')
    check('and installs', v.wait('installer: installing', 5, at) and v.wait('install: copied 50%', 120, at), v.text(at))
    check('to the end', v.wait('installer: done', 180, at), v.text(at)[-2000:])
finally:
    v.close()

out = subprocess.run(['sfdisk', '-d', drive], capture_output=True, text=True).stdout
check('over the whole drive', out.count('type=') == 2, out)
finish()
