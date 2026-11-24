"""Checks install puts AximOS in a drive's free space beside another system, which it
leaves as it was and which Limine's menu offers, that the firmware then starts it, with
its first account and swap file, and that installing it again keeps /home."""
import re, shutil, subprocess, time
from vm import VM, WORK, OVMF_VARS, check, finish

# A drive with another system's ESP and partition at its start, and room after them
drive, efivars, other = WORK + '/drive.img', WORK + '/vars.fd', WORK + '/other.img'
subprocess.run(['truncate', '-s', '8G', drive], check=True)
subprocess.run(['sfdisk', '-q', drive], input='label: gpt\nsize=64MiB, type=uefi\nsize=512MiB, type=linux\n',
               text=True, check=True)
subprocess.run(['truncate', '-s', '64M', other], check=True)
subprocess.run(['mformat', '-i', other, '-F', '::'], check=True)
subprocess.run(['mmd', '-i', other, '::/EFI', '::/EFI/other'], check=True)
subprocess.run(['mcopy', '-i', other, '/usr/share/limine/BOOTX64.EFI', '::/EFI/other/other.efi'], check=True)
subprocess.run(['dd', f'if={other}', f'of={drive}', 'bs=1M', 'seek=1', 'conv=notrunc', 'status=none'], check=True)
shutil.copy(OVMF_VARS, efivars)
before = subprocess.run(['sfdisk', '-d', drive], capture_output=True, text=True).stdout
nvme = ['-drive', f'file={drive},format=raw,if=none,id=target', '-device', 'nvme,serial=target,drive=target']


def install(v, how, password='newpass'):
    """Runs install as root, answering sudo and, for a new system, the account's password."""
    at = v.mark()
    v.type(f'sudo aximos-install nvme0n1 {how} -y\n')
    v.wait('Password for ', 5, at)
    v.type('secret\n')
    if 'again' not in how:
        v.wait('New password', 10, at)
        v.type(f'{password}\n{password}\n')
    v.wait('aximos-install: done', 180, at) or v.wait('$ ', 1, at)
    return v.text(at)


def boot_installed():
    """Starts from the drive, through the firmware's first entry, picking AximOS in Limine's menu."""
    v = VM(nvme=drive, efivars=efivars)
    for _ in range(20):
        if 'Linux version' in v.raw() or v.wait('login: ', 1):
            break
        v.key('ret')
    return v


v = VM(extra=nvme, efivars=efivars)
try:
    v.login()
    out = v.run('aximos-install list')
    check('install lists the drive, with its free space', re.search(r'nvme0n1\t8\.0 GiB\t7\.4 GiB\tfree whole', out), out)
    v.sudo('efiboot add nvme0n1p1 /EFI/other/other.efi Other')
    out = install(v, 'free owner')
    check('installs in the free space', 'aximos-install: done' in out and 'copied 100%' in out, out)
    out = v.run('efiboot')
    check('with an entry first in the firmware\'s order', re.search(r'Order +([0-9A-F]{4})', out)
          and re.search(r'\* AximOS +nvme0n1p3 \\EFI\\BOOT\\BOOTX64.EFI', out), out)
    out = v.sudo('mount /dev/nvme0n1p3 /mnt && cat /mnt/boot/limine/limine.conf; umount /mnt')
    check('and a menu offering the other system',
          'timeout: no' in out and '/Other' in out and re.search(r'path: guid\([0-9a-f-]{36}\):/EFI/other/other.efi', out), out)
finally:
    v.close()

after = subprocess.run(['sfdisk', '-d', drive], capture_output=True, text=True).stdout
check('leaving the other system as it was', before.split('\n\n')[1].strip().split('\n')[0] == after.split('\n\n')[1].strip().split('\n')[0]
      and subprocess.run(['mdir', '-i', f'{drive}@@1M', '::/EFI/other'], capture_output=True, text=True).stdout.count('other') >= 1,
      after)

v = boot_installed()
try:
    check('the firmware starts it', v.wait('login: ', 60), v.text())
    at = v.mark()
    v.type('owner\nnewpass\n')
    check('with the account install made', v.wait('$ ', 10, at), v.text(at))
    out = v.run('cat /proc/swaps; mount; echo mine > note; sync')
    check('its swap file in use, and its root on ext4', '/swapfile' in out and '/dev/root / ext4 rw' in out, out)
finally:
    v.close()

# From the stick again, which the firmware would otherwise pass over for the drive now
shutil.copy(OVMF_VARS, WORK + '/vars-again.fd')
v = VM(extra=nvme, efivars=WORK + '/vars-again.fd')
try:
    v.login()
    out = install(v, 'again')
    check('installs again, over itself', 'aximos-install: done' in out and 'replaces' not in out, out)
finally:
    v.close()

v = boot_installed()
try:
    check('which starts', v.wait('login: ', 60), v.text())
    at = v.mark()
    v.type('owner\nnewpass\n')
    check('keeping the account', v.wait('$ ', 10, at), v.text(at))
    out = v.run('cat note')
    check('and what was in /home', 'mine' in out, out)
finally:
    v.close()
finish()
