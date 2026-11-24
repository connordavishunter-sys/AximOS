"""Checks AximOS builds itself: an installed system, with its development tools, clones this
tree and runs make, which downloads the sources as it does on the host, and the ISO it makes
boots. The tree goes in as a git bundle of what's here now, committed or not, and the ISO
comes out, on a small drive of their own. It takes an hour or more, so it isn't one of make
test's, but make test-build's."""
import os, re, shutil, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from vm import VM, BUILD, ROOT, OVMF_VARS, check, finish

# On the disk, not in /tmp, which can be in memory, as the installed system's drive fills to
# tens of gigabytes
WORK = BUILD + '/selfbuild'
shutil.rmtree(WORK, ignore_errors=True)
os.makedirs(WORK)
CARRY_MIB = 1024
drive, carry, efivars, iso = WORK + '/drive.img', WORK + '/carry.img', WORK + '/vars.fd', WORK + '/built.iso'
subprocess.run(['truncate', '-s', '128G', drive], check=True)
subprocess.run(['truncate', '-s', f'{CARRY_MIB}M', carry], check=True)
shutil.copy(OVMF_VARS, efivars)

# This tree, as one commit in a repository of its own, at the start of the carrying drive
tree = WORK + '/tree'
listed = subprocess.run(['git', '-C', ROOT, 'ls-files', '-co', '--exclude-standard', '-z'],
                        capture_output=True, check=True).stdout.decode().split('\0')
for path in filter(None, listed):
    if os.path.lexists(os.path.join(ROOT, path)):
        os.makedirs(os.path.dirname(os.path.join(tree, path)), exist_ok=True)
        shutil.copy2(os.path.join(ROOT, path), os.path.join(tree, path), follow_symlinks=False)
git = ['git', '-C', tree, '-c', 'user.name=AximOS test', '-c', 'user.email=test@aximos.invalid']
for args in (['init', '-q'], ['add', '-A'], ['commit', '-q', '-m', 'The tree under test'],
             ['bundle', 'create', '-q', WORK + '/aximos.bundle', 'HEAD', '--branches']):
    subprocess.run(git + args, check=True)
bundle = open(WORK + '/aximos.bundle', 'rb').read()
with open(carry, 'r+b') as f:
    f.write(bundle)

drives = ['-drive', f'file={drive},format=raw,if=none,id=target', '-device', 'nvme,serial=target,drive=target',
          '-drive', f'file={carry},format=raw,if=none,id=carry', '-device', 'nvme,serial=carry,drive=carry']

# Installed, with the development tools from the disc
v = VM(extra=drives, efivars=efivars)
try:
    v.login()
    at = v.mark()
    v.type('sudo aximos-install nvme0n1 whole owner -y\n')
    v.wait('Password for ', 5, at)
    v.type('secret\n')
    v.wait('New password', 10, at)
    v.type('pass\npass\n')
    check('installs', v.wait('aximos-install: done', 300, at), v.text(at)[-800:])
finally:
    v.close()

v = VM(nvme=drive, efivars=efivars, extra=drives[4:], smp=16, memory='8G', timeout=6 * 3600)
try:
    for _ in range(20):  # through Limine's menu
        if v.wait('login: ', 1):
            break
        v.key('ret')
    at = v.mark()
    v.type('owner\npass\n')
    check('which starts', v.wait('$ ', 15, at), v.text(at))
    out = v.run('for i in $(seq 30); do curl -sI https://cdn.kernel.org >/dev/null && break; sleep 1; done; '
                'curl -sI https://cdn.kernel.org | head -1', 40)
    check('and reaches the internet', 'HTTP/' in out, out)

    out = v.run(f'echo carry=$(for d in /sys/block/nvme*; do [ $(cat $d/size) = {CARRY_MIB * 2048} ] && echo ${{d##*/}}; done)', 1)
    carry_dev = '/dev/' + re.search(r'carry=(nvme\d+n1)', out).group(1)
    v.sudo(f'head -c {len(bundle)} {carry_dev} > aximos.bundle', 10, password='pass')
    out = v.run('git clone -q aximos.bundle AximOS && cd AximOS && git log --oneline | head -1', 30)
    check('clones the tree', 'The tree under test' in out, out)

    start, at = time.time(), v.mark()
    v.type('make > ../build.log 2>&1; echo "make ex""ited $?"; stat -c "the ISO, %s bytes" build/aximos.iso; tail -40 ../build.log\n')
    v.wait('make exited ', 5 * 3600, at)
    time.sleep(2)
    out = v.text(at)
    made = re.search(r'the ISO, (\d+) bytes', out)
    if not check(f'makes the ISO, in {(time.time() - start) / 60:.0f} minutes', 'make exited 0' in out and made, out[-3000:]):
        finish()
    size = int(made.group(1))

    v.sudo(f'dd if=build/aximos.iso of={carry_dev} bs=1M conv=fsync status=none', 30, password='pass')
finally:
    v.close()

shutil.copy(carry, iso)
os.truncate(iso, size)
v = VM(iso=iso)
try:
    v.login()
    out = v.run('uname -r; ls /system/bin/wm', 2)
    check('and the ISO it made boots', '7.3.5' in out and 'No such' not in out, out)
finally:
    v.close()
shutil.rmtree(WORK, ignore_errors=True)
finish()
