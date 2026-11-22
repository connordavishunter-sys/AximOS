"""Checks USB keyboards and mice, without a PS/2 controller, as on laptops whose own
keyboard is on USB: typing reaches the shell, and the mouse moves, clicks and scrolls."""
import re, time
from vm import VM, check, finish

v = VM(extra=['-M', 'q35,i8042=off', '-device', 'qemu-xhci,id=usb',
              '-device', 'usb-kbd,bus=usb.0', '-device', 'usb-mouse,bus=usb.0'])
try:
    check('prompt at boot, with Shift held on the USB keyboard', v.wait('/$ ', 40), v.raw()[-800:])
    check('the keyboard and mouse are found', 'usb: found a keyboard' in v.raw() and 'usb: found a mouse' in v.raw(),
          [l for l in v.raw().split('\n') if 'usb' in l])
    o = v.run('echo "Typed: A-Z, a~z & (1+2)=3!"')
    check('typing, shifted keys and all', '\nTyped: A-Z, a~z & (1+2)=3!\n' in o, o)
    at = v.mark()
    v.type('echo abx'); v.key('backspace'); v.key('left'); v.type('Z'); v.key('end'); v.type('c\n'); time.sleep(1)
    check('backspace and the arrow keys', '\naZbc\n' in v.text(at), v.text(at))

    v.type('mousetest\n'); time.sleep(0.5)
    at = v.mark()
    v.mouse(30, 20)
    v.mouse(buttons=1); v.mouse(buttons=0)
    v.command('mouse_move 0 0 1'); time.sleep(0.5)
    v.key('ctrl-c'); time.sleep(0.3)
    o = v.text(at)
    moved = re.findall(r'^x (-?\d+) y (-?\d+) wheel (-?\d+) buttons (\S+)', o, re.M)
    check('the mouse moves right and down', any(int(x) > 0 and int(y) > 0 for x, y, _, _ in moved), o)
    check('clicks', any(b == 'L--' for _, _, _, b in moved), o)
    check('and scrolling up is a negative wheel', any(w == '-1' for _, _, w, _ in moved), o)
finally:
    v.close()
finish()
