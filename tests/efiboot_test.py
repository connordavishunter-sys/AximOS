"""Checks disk adds partitions in a drive's free space while the system on it runs,
leaving the others as they were, and that efiboot adds a boot entry the firmware
then starts from, which keeps it through restarts, and removes it."""
import re, shutil, subprocess
from vm import VM, BUILD, WORK, OVMF_VARS, check, finish

# The disk image, with 256 MiB more at its end, as a drive with room beside a system
disk, efivars = WORK + '/disk.img', WORK + '/vars.fd'
subprocess.run(['cp', '--sparse=always', BUILD + '/aximos.img', disk], check=True)
subprocess.run(['truncate', '-s', '+256M', disk], check=True)
subprocess.run(['sfdisk', '-q', '--relocate', 'gpt-bak-std', disk], check=True)
shutil.copy(OVMF_VARS, efivars)

v = VM(nvme=disk, efivars=efivars)
try:
    v.login()
    out = v.run('disk')
    check('disk list shows the free space', re.search(r'free +25\d\.\d MiB', out), out)
    out = v.sudo('disk add nvme0n1 32 esp && disk add nvme0n1 rest linux')
    check('disk add fills it while the system runs from the drive',
          'added nvme0n1p3,  32.0 MiB' in out and 'added nvme0n1p4' in out, out)
    out = v.sudo('disk format nvme0n1p4 ext4 NEW -y && mount /dev/nvme0n1p4 /mnt && echo new > /mnt/f && cat /mnt/f', 6)
    check('which Linux sees, so it can be formatted and mounted', 'new' in out, out)
    out = v.run('echo still > note && cat note')
    check('and the system is as it was', 'still' in out, out)

    out = v.run('efiboot')
    check('efiboot lists the entries', 'Order' in out and 'UEFI' in out, out)
    out = v.sudo('efiboot add nvme0n1p1 /EFI/BOOT/BOOTX64.EFI AximOS')
    entry = re.search(r'added ([0-9A-F]{4})', out)
    check('adds one', entry, out)
    entry = entry.group(1) if entry else '0000'
    out = v.run('efiboot')
    check('which is first, with its partition and loader',
          re.search(rf'Order +{entry} ', out) and re.search(rf'{entry} \* AximOS +nvme0n1p1 \\EFI\\BOOT\\BOOTX64.EFI', out), out)
    out = v.sudo(f'efiboot order {entry} && efiboot next {entry} && efiboot')
    check('can be ordered and set to start next', f'Next time    {entry}' in out, out)
    out = v.run('efiboot next 7777')
    check('though not with entries there are none of', 'no entry 7777' in out, out)
finally:
    v.run('sync', 1)
    v.close()

v = VM(nvme=disk, efivars=efivars)
try:
    check('it boots again', v.wait('login: ', 30), v.text())
    at = v.mark()
    v.type('tester\nsecret\n')
    check('which logs in', v.wait('$ ', 10, at), v.text(at))
    out = v.run('efiboot')
    check('from the entry that was added', f'Started from {entry}' in out and 'Next time' not in out, out)
    out = v.run('cat /proc/partitions')
    check('with the partitions that were', 'nvme0n1p3' in out and 'nvme0n1p4' in out, out)
    out = v.sudo(f'efiboot remove {entry} && efiboot')
    check('and efiboot removes it', 'AximOS' not in out and entry not in out.split('Order')[1].split('\n')[0], out)
finally:
    v.close()

out = subprocess.run(['sfdisk', '-V', disk], capture_output=True, text=True).stdout
check('the partition table is sound', 'No errors detected' in out, out)
finish()
