"""Runs each line of utils_cases.txt on AximOS and on the host's GNU tools and compares."""
import os, random, re, subprocess
from vm import VM, WORK, check, disk_copy, finish, get, put

HERE = os.path.dirname(os.path.abspath(__file__))


def make_corpus(d):
    r = random.Random(5)
    words = 'the quick brown fox jumps over lazy dog The Fox dot hat'.split()
    files = {
        'a.txt': ''.join(' '.join(r.choice(words) for _ in range(r.randint(0, 9))) + '\n' for _ in range(60)),
        'b.txt': 'a.b\naxb\nA.B\n(paren)\n' + ''.join(f'line {i} the\n' for i in range(15)),
        'nonl.txt': 'one\ntwo\nthree no newline', 'empty.txt': '',
        's.txt': ''.join(r.choice(words) + str(r.randint(0, 99)) + '\n' for _ in range(50)),
        'n.txt': ''.join(f'{r.randint(-50, 500)}\n' for _ in range(50)) + '3.5\n3.25\n',
        'dup.txt': 'b\na\nb\nc\na\n', 'mixed.txt': 'Apple\nbanana\napple\nBanana\ncherry\n',
        'k.txt': ''.join(f'{r.choice(words)} {r.randint(0, 99)} {r.choice(words)}\n' for _ in range(30)),
        'c.txt': ''.join(f'{r.choice(words)}:{r.choice(words)}:{r.randint(0, 99)}\n' for _ in range(30)),
        'd/n1.txt': 'a needle here\nno\n', 'd/other.TXT': 'needle\nneedle two\n',
        'd/sub/n2.txt': 'nothing\n', 'd/sub/n3.dat': 'x needle\n', 'd/sub/deep/big.bin': r.randbytes(300000),
    }
    for name, content in files.items():
        os.makedirs(os.path.dirname(f'{d}/{name}'), exist_ok=True)
        open(f'{d}/{name}', 'wb').write(content.encode() if isinstance(content, str) else content)


def by_case(text):
    """Output split at the markers, one list of whitespace-normalised lines per case."""
    cases, current = {}, None
    for line in text.split('\n'):
        if m := re.fullmatch(r'@@@ (\d+)', line):
            current = cases.setdefault(int(m[1]), [])
        elif current is not None and (line := ' '.join(line.split())):
            current.append(line)
    return cases


cases = [l.rstrip('\n') for l in open(HERE + '/utils_cases.txt') if l.strip()]
script = ''.join(f"echo '@@@ {i}'\n{c}\n" for i, c in enumerate(cases))

host_dir, guest_dir = WORK + '/host/c', WORK + '/guest/c'
make_corpus(host_dir)
make_corpus(guest_dir)
host = subprocess.run(['sh', '-c', script], cwd=host_dir, capture_output=True, text=True, errors='replace',
                      env=dict(os.environ, LC_ALL='C')).stdout

img = disk_copy('utils')
put(img, guest_dir, '/tmp/c')
open(WORK + '/t.sh', 'w').write(script)
put(img, WORK + '/t.sh', '/tmp/t.sh')
v = VM(disk=img)
check('prompt at boot', v.wait('/$ ', 30), v.text())
v.type('cd /tmp/c; dash /tmp/t.sh > /tmp/out 2>/dev/null; shutdown\n')
check('script finishes and powers off', v.exited(240), v.text()[-300:])
v.close()
get(img, '/tmp/out', WORK + '/out')

ours, theirs = by_case(open(WORK + '/out', errors='replace').read()), by_case(host)
for i, c in enumerate(cases):
    check(c, ours.get(i) == theirs.get(i), f'ours {ours.get(i)}\n    GNU  {theirs.get(i)}')
finish()
