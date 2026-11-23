"""Checks the desktop takes a laptop's touchpad gestures, power button and lid, which
inputsim pretends to be: taps click, swipes change workspace and show the overview,
the power button asks what to do, and closing the lid locks and suspends."""
import time
from vm import VM, check, finish

v = VM()
try:
    v.desktop()
    v.sudo('modprobe uinput 2>/dev/null; true')
    bar = 800 - 8 - 32

    def sim(args, wait, secs=10):
        at = v.mark()
        v.sudo(f'inputsim {args}', 0.5)
        return v.wait(wait, secs, at), v.text(at)

    v.pointer(36, bar + 14)  # on the launcher
    check('a tap clicks', *sim('tap', 'wm: menu 0 at'))
    v.key('esc')
    v.pointer(900, 300)  # on the desktop
    check('and one of two fingers right-clicks', *sim('tap 2', 'wm: menu 0 at'))
    v.key('esc')

    check('three fingers swiping left go to the next workspace', *sim('swipe left', 'wm: workspace 2'))
    check('and right, back', *sim('swipe right', 'wm: workspace 1'))
    v.start('guidemo')
    v.wait('guidemo: focus 1', 10)
    check('and up show the overview', *sim('swipe up', 'wm: overview shown'))
    v.key('esc')

    at = v.mark()
    v.qmp_command('system_powerdown')  # the power button, as ACPI has it
    check('the power button asks what to do', v.wait('wm: power menu', 10, at), v.text(at))
    v.key('esc')

    # The keyboard wakes it, as a VM's lid can't, and a laptop's does through ACPI.
    v.sudo('for w in /sys/bus/serio/devices/*/power/wakeup; do echo enabled > $w; done')
    ok, text = sim('lid close', 'wm: suspending')
    check('closing the lid locks and suspends', ok and 'wm: locked' in text, text)
    time.sleep(3)
    v.key('shift')
    time.sleep(1)
    out = v.run('cat /sys/power/suspend_stats/success', 3)
    check('which it wakes from', '\n1\n' in out, out[-200:])
finally:
    v.close()
finish()
