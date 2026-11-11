"""Checks time zones: local time from the zone files and POSIX rules, against the
host's C library reading the same zones, and date and tz."""
import os, re, subprocess, time
from datetime import datetime, timedelta, timezone
from zoneinfo import ZoneInfo
from vm import VM, WORK, check, compile_program, disk_copy, finish, put

P = os.path.dirname(os.path.abspath(__file__)) + '/programs/'
img = disk_copy('time')
put(img, compile_program(P + 'tzcheck.c', WORK + '/tzcheck'), '/system/bin/tzcheck')

# Zones with half and quarter hours, the south, no summer time, summer time that
# was abolished, and Dublin, whose summer time is its standard time.
ZONES = ['Europe/London', 'America/New_York', 'Australia/Sydney', 'Asia/Kolkata', 'Australia/Lord_Howe',
         'America/St_Johns', 'Pacific/Chatham', 'Asia/Tehran', 'America/Sao_Paulo', 'Europe/Dublin',
         'Pacific/Kiritimati', 'Africa/Casablanca', 'UTC']
RULES = ['EST5EDT,M3.2.0,M11.1.0', '<+0530>-5:30', 'NZST-12NZDT,M9.5.0,M4.1.0/3', 'CET-1CEST,M3.5.0,M10.5.0/3',
         'AEST-10AEDT,M10.1.0,M4.1.0/3']
FIXED = [-1000000000, 0, 1000000000, 1700000000, 2000000000, 2500000000, 4000000000]


def changes(zone, year):
    """When the clocks changed in a year, and a second either side."""
    tz, out = ZoneInfo(zone), []
    t = datetime(year, 1, 1, tzinfo=timezone.utc)
    while t.year == year:
        if t.astimezone(tz).utcoffset() != (t + timedelta(hours=1)).astimezone(tz).utcoffset():
            low = int(t.timestamp())
            high = low + 3600
            while high - low > 1:
                middle = (low + high) // 2
                if datetime.fromtimestamp(middle, tz).utcoffset() == t.astimezone(tz).utcoffset():
                    low = middle
                else:
                    high = middle
            out += [high - 1, high, high + 1]
        t += timedelta(hours=1)
    return out


def host(tz, stamps):
    return [subprocess.run(['date', '-d', f'@{s}', '+%Y-%m-%d %H:%M:%S %Z %z'], env={'TZ': tz},
                           capture_output=True, text=True).stdout.strip() for s in stamps]


v = VM(disk=img)
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    # glibc works out POSIX rules' dates from 1970 on, so it's wrong about 1938.
    cases = [(z, FIXED + changes(z, 2026) + changes(z, 2040)) for z in ZONES] + \
            [(r, FIXED[1:] + [1772953199, 1772953200, 1793512799, 1793512800]) for r in RULES]
    for tz, stamps in cases:
        at = v.mark()
        v.type(f"TZ='{tz}' tzcheck {' '.join(map(str, stamps))}\n")
        v.wait('tzcheck: done', 15, at)
        got = {int(m[0]): (m[1], m[2]) for m in re.findall(r'^(-?\d+) (.+ [+-]\d{4}) (back|same|LOST)$', v.text(at), re.M)}
        expected = dict(zip(stamps, host(tz, stamps)))
        wrong = [f'{s}: got {got.get(s, ("missing",))[0]}, want {expected[s]}' for s in stamps
                 if got.get(s, ('',))[0] != expected[s]]
        check(f'{tz}: local time', not wrong, '\n'.join(wrong[:8]))
        lost = [str(s) for s in stamps if got.get(s, ('', ''))[1] not in ('back', 'same')]
        check(f'{tz}: mktime turns it back', not lost, ' '.join(lost[:8]))

    o = v.run('date -u +%Z'); check('date -u', '\nUTC\n' in o, o)
    o = v.run('date +%Z'); check('UTC without a zone', '\nUTC\n' in o, o)
    o = v.run('tz'); check('tz shows UTC', 'UTC, where' in o, o)
    o = v.run('tz -l Europe | wc -l; tz -l | grep -c /')
    counts = list(map(int, re.findall(r'^\s*(\d+)$', o, re.M)))
    check('tz -l', len(counts) == 2 and counts[0] > 50 and counts[1] > 400, o)
    o = v.run('tz Nowhere/Atlantis; echo st=$?'); check('tz refuses a zone that isn\'t', 'no zone' in o and 'st=1' in o, o)

    o = v.run('tz Asia/Kolkata; date +%Z%z; TZ=Asia/Tokyo date +%Z')
    check('tz sets the zone, and TZ goes over it', 'IST+0530' in o and '\nJST\n' in o, o)
    o = v.run('tz'); check('tz shows it', 'Asia/Kolkata, where' in o and 'IST (+0530)' in o, o)
    want = host('America/New_York', [int(time.time())])[0].split()[2]
    o = v.run('tz America/New_York >/dev/null; date +%Z'); check('and changes it', f'\n{want}\n' in o, o)
    o = v.run('touch /tmp/now; stat /tmp/now'); check('stat in local time', f' {want}' in o, o)
finally:
    v.close()
finish()
