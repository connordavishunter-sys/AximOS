"""Checks netd gets the VM's card an address by DHCP, IPv6 from the router, and DNS from
DHCP and DHCPv6, so it reaches the host, which QEMU's user network shows as 10.0.2.2,
and that net sets an address by hand, kept across restarts, and goes back to DHCP."""
import socket, threading, time
from vm import VM, check, finish

server = socket.socket()
server.bind(('127.0.0.1', 0))
server.listen()
port = server.getsockname()[1]


def greet():
    while True:
        conn, _ = server.accept()
        conn.sendall(b'hello from the host\n')
        conn.close()


threading.Thread(target=greet, daemon=True).start()

v = VM()
try:
    v.login()
    end = time.time() + 15
    while 'fec0::3' not in (out := v.run('net', 0.5)) and time.time() < end:
        time.sleep(1)
    check('netd connects the card by DHCP', 'enp0s2, wired, connected, by DHCP' in out, out)
    check('with its address and gateway', 'address  10.0.2.15/24' in out and 'gateway  10.0.2.2' in out, out)
    check('and DNS, from DHCP and DHCPv6', 'dns        10.0.2.3' in out and 'dns        fec0::3' in out, out)
    out = v.run('cat /proc/net/if_inet6 /etc/resolv.conf')
    check('IPv6 from the router, and resolv.conf', 'fec0' in out and 'nameserver 10.0.2.3' in out, out)
    out = v.run(f'nc 10.0.2.2 {port}', 2)
    check('so it reaches the host', 'hello from the host' in out, out)

    out = v.run('net set enp0s2 10.0.2.20/24 10.0.2.2 10.0.2.3; net', 2)
    check('an address can be set by hand', 'set by hand' in out and 'address  10.0.2.20/24' in out, out)
    out = v.run(f'nc 10.0.2.2 {port}', 2)
    check('which works', 'hello from the host' in out, out)
    out = v.run('net set enp0s2 nonsense; net set wlan9 10.0.0.1/24', 1)
    check("though not what isn't one, or on a card there isn't",
          "that isn't an address" in out and "no card called wlan9" in out, out)
    out = v.sudo('cat /system/etc/netd.conf') + v.run('cat /system/etc/netd.conf')
    check("it's kept, for root alone", 'enp0s2 10.0.2.20/24 10.0.2.2 10.0.2.3' in out and 'Permission denied' in out, out)
    v.run('net dhcp enp0s2', 1)
    time.sleep(3)
    out = v.run('net')
    check('and DHCP comes back', 'by DHCP' in out and 'address  10.0.2.15/24' in out, out)
finally:
    v.close()
finish()
