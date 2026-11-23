"""Checks Wi-Fi through netd, on Linux's simulated radios: one an access point, through
hostapd, and the other netd's. wifi lists the networks nearby, won't keep one with the
wrong password, joins with the right one, which is saved and joined again after airplane
mode and after netd restarts, forgets it, and joins hidden and open networks."""
import time
from vm import VM, check, finish

AP = '''interface=wlx020000000100
driver=nl80211
ctrl_interface=/run/hostapd
hw_mode=g
channel=6
ssid={ssid}
{security}
'''
WPA = 'wpa=2\\nwpa_key_mgmt=WPA-PSK SAE\\nieee80211w=1\\nrsn_pairwise=CCMP\\nwpa_passphrase=correct horse\\nsae_password=correct horse'


def access_point(v, ssid, security, hidden=False):
    """hostapd on the second radio, as the network named."""
    conf = AP.format(ssid=ssid, security=security).replace('\n', '\\n') + ('ignore_broadcast_ssid=1\\n' if hidden else '')
    v.sudo(f"kill $(cat /run/hostapd.pid 2>/dev/null) 2>/dev/null; sleep 1; printf '{conf}' > /run/ap.conf; "
           "hostapd -B -P /run/hostapd.pid /run/ap.conf >/dev/null", 3)


v = VM()
try:
    v.login()
    # Named by udev for their MACs: netd leaves the second to hostapd, and takes the first
    v.sudo("echo 'wlx020000000100 ignore' > /system/etc/netd.conf; modprobe mac80211_hwsim radios=2", 2)
    check('netd finds the radio', v.wait('netd: wlx020000000000 knows 0 saved networks', 20)
          and 'found wlx020000000100' not in v.text(), v.text()[-1500:])
    access_point(v, 'AximOS Test', WPA)

    out = v.run('wifi', 10)
    check('wifi lists the network nearby', 'AximOS Test' in out and 'WPA3' in out, out)

    out = v.run("echo 'wrong horse' | wifi join 'AximOS Test'", 26)
    check("won't join with the wrong password", "couldn't join AximOS Test" in out, out)
    out = v.sudo('cat /system/etc/netd-wifi 2>&1')
    check('and so forgets it', '41786' not in out, out)

    out = v.run("echo 'correct horse' | wifi join 'AximOS Test'", 15)
    check('joins with the right one', 'joined AximOS Test' in out, out)
    out = v.run('wifi; net', 5)
    check('which it marks, and saves', '* AximOS Test' in out and 'saved' in out and 'wlx020000000000, wifi, connected' in out, out)
    out = v.sudo('cat /system/etc/netd-wifi') + v.run('cat /system/etc/netd-wifi')
    check('for root alone', '41786' in out and 'Permission denied' in out, out)

    out = v.run('wifi off; wifi', 2)
    check('airplane mode turns Wi-Fi off', 'airplane mode is on' in out, out)
    out = v.run('cat /sys/class/rfkill/*/soft')
    check('with rfkill', '1' in out and '0' not in out.split('soft')[-1], out)
    at = v.mark()
    v.run('wifi on', 1)
    check('and on, rejoining the saved network', v.wait('netd: wlx020000000000 connected', 30, at), v.text(at))

    at = v.mark()
    v.sudo('for p in /proc/[0-9]*; do [ "$(cat $p/comm)" = netd ] && kill ${p#/proc/}; done', 1)
    check('as netd does when it starts again', v.wait('netd: wlx020000000000 knows 1 saved networks', 20, at)
          and v.wait('netd: wlx020000000000 connected', 30, at), v.text(at)[-1500:])

    out = v.run("wifi forget 'AximOS Test'; wifi", 5)
    check('a network can be forgotten', 'saved' not in out and '* AximOS' not in out, out)

    access_point(v, 'Hidden Test', WPA, hidden=True)
    out = v.run('wifi', 10)
    check("a hidden network isn't listed", 'Hidden Test' not in out, out)
    out = v.run("echo 'correct horse' | wifi join 'Hidden Test' --hidden", 20)
    check('but joins by its name', 'joined Hidden Test' in out, out)

    access_point(v, 'Open Test', '')
    out = v.run("wifi join 'Open Test'", 20)
    check('and an open one joins without a password', 'joined Open Test' in out and 'Password' not in out, out)
finally:
    v.close()
finish()
