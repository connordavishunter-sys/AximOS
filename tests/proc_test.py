"""Checks /proc and the programs built on it: ps, top, du, stat and axfetch."""
import re, time
from vm import VM, check, finish

v = VM()
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())

    o = v.run('ls /'); check('ls / has proc', 'proc' in o, o)
    o = v.run('ls /proc')
    check('ls /proc', all(x in o for x in ('cpuinfo', 'meminfo', 'uptime', 'version', 'stat', 'mounts', 'net'))
          and re.search(r'\b1/?\b', o), o)
    o = v.run('cat /proc/meminfo'); check('meminfo', 'MemTotal' in o and 'MemFree' in o, o)
    o = v.run('cat /proc/cpuinfo'); check('cpuinfo', 'cpus:\t4' in o, o)
    o = v.run('cat /proc/uptime'); check('uptime', re.search(r'\n\d+\.\d\d\n', o), o)
    o = v.run('cat /proc/version'); check('version', 'AximOS' in o, o)
    o = v.run('cat /proc/mounts'); check('mounts', ' / fat32' in o and 'iso9660' in o, o)
    o = v.run('cat /proc/net'); check('net', '10.0.2.15' in o, o)
    o = v.run('cat /proc/stat'); check('stat has every CPU', o.count('cpu_idle') == 4, o)
    o = v.run('cat /proc/self/status'); check('self is the reader', 'Name:\tcat' in o, o)
    o = v.run('cat /proc/1/status'); check('pid 1 is init', 'Name:\tinit' in o and 'Pid:\t1' in o, o)
    o = v.run('cat /proc/self/cmdline'); check('cmdline', 'cat /proc/self/cmdline' in o, o)
    o = v.run('cat /proc/99999/status'); check('missing pid', 'No such file' in o, o)
    o = v.run('cat /proc/nope'); check('missing file', 'No such file' in o, o)
    for command in ('touch /proc/x', 'rm /proc/meminfo', 'echo x > /proc/meminfo'):
        o = v.run(command); check(command + ' is refused', 'ead-only' in o, o)
    o = v.run('stat /proc/meminfo'); check('stat a /proc file', 'Type: file' in o, o)
    o = v.run('stat /proc'); check('stat /proc', 'Type: directory' in o, o)
    o = v.run('ps')
    check('ps', re.search(r'PID.*COMMAND', o) and '/system/boot/init' in o and '/system/bin/dash' in o and ' ps' in o, o)
    o = v.run('du -s /system/etc'); check('du', re.search(r'KiB.*/system/etc', o), o)
    o = v.run('grep -c needle /proc/meminfo'); check('grep reads /proc', '\n0\n' in o, o)
    o = v.run('find /proc -maxdepth 1 -name uptime'); check('find walks /proc', '/proc/uptime' in o, o)
    o = v.run('axfetch'); check('axfetch', all(x in o for x in ('Memory', 'CPU', 'Disk', '10.0.2.15')), o)

    at = v.mark(); v.type('top\n'); time.sleep(3.5); o = v.text(at)
    check('top draws', o.count('COMMAND') >= 2 and 'CPU:' in o and 'Mem:' in o, o)
    v.key('q'); time.sleep(0.5)
    o = v.run('echo after-top'); check('q quits top', '\nafter-top\n' in o, o)

    # A busy loop shows up at about 100% of a CPU.
    v.run('(while :; do :; done) &', 0.5)
    at = v.mark(); v.type('top\n'); time.sleep(3.5); o = v.text(at); v.key('q'); time.sleep(0.5)
    busy = re.findall(r'\n\s*\d+\s+R\s+\d+\s+\S+ \S+\s+(\d+)\.\d', o)
    check('busy process near 100%', max(map(int, busy), default=0) >= 90, o[-500:])
    v.run('kill %1', 0.5)

    v.run('echo one > /tmp/f')
    at = v.mark(); v.type('tail -f /tmp/f\n'); time.sleep(1)
    check('tail -f prints', 'one' in v.text(at), v.text(at))
    v.key('ctrl-c'); time.sleep(0.5)
    o = v.run('echo alive'); check('ctrl+c ends tail -f', '\nalive\n' in o, o)

    for c in 'grep find sort head tail wc ps top sleep du stat axfetch cp'.split():
        o = v.run(f'/system/bin/{c} -h', 0.5); check(c + ' -h', f'usage: {c}' in o, o)
finally:
    v.close()
finish()
