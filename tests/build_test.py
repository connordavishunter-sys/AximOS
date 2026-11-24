"""Checks an installed AximOS can build programs with its development tools: GCC compiles
and links C and C++, and make builds a program against AximOS's own libaximos."""
import shutil, subprocess
from vm import VM, WORK, OVMF_VARS, check, finish

drive, efivars = WORK + '/drive.img', WORK + '/vars.fd'
subprocess.run(['truncate', '-s', '8G', drive], check=True)
shutil.copy(OVMF_VARS, efivars)

HELLO_C = r'''#include <stdio.h>
int main(void) { printf("hello from C, %d\n", 6 * 7); return 0; }
'''
HELLO_CXX = r'''#include <iostream>
#include <string>
#include <vector>
int main() { std::vector<std::string> v{"hello", "from", "C++"}; for (auto &s : v) std::cout << s << ' '; std::cout << '\n'; }
'''
TOOL = r'''#include <aximos.h>
#include <stdio.h>
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: mine\nA program built on AximOS.\n");
    printf("mine, built with libaximos\n");
    return 0;
}
'''


def write(v, path, text):
    v.run(f"cat > {path} <<'EOF'\n{text}EOF", 1)


# Installed, with the development tools from the disc
v = VM(extra=['-drive', f'file={drive},format=raw,if=none,id=target', '-device', 'nvme,serial=target,drive=target'],
       efivars=efivars)
try:
    v.login()
    at = v.mark()
    v.type('sudo aximos-install nvme0n1 whole owner -y\n')
    v.wait('Password for ', 5, at)
    v.type('secret\n')
    v.wait('New password', 10, at)
    v.type('pass\npass\n')
    check('installs, with the development tools', v.wait('aximos-install: done', 300, at)
          and 'adding the development tools' in v.text(at), v.text(at)[-800:])
finally:
    v.close()

v = VM(nvme=drive, efivars=efivars)
try:
    for _ in range(20):  # through Limine's menu
        if v.wait('login: ', 1):
            break
        v.key('ret')
    at = v.mark()
    v.type('owner\npass\n')
    check('which starts', v.wait('$ ', 15, at), v.text(at))
    out = v.run('gcc --version | head -1; make --version | head -1; ld --version | head -1', 3)
    check('with GCC, make and binutils', 'gcc (GCC) 16.2.0' in out and 'GNU Make 4.4.1' in out and 'GNU ld' in out, out)

    write(v, 'hello.c', HELLO_C)
    out = v.run('gcc -O2 -Wall hello.c -o hello && ./hello', 8)
    check('GCC compiles and links C', 'hello from C, 42' in out, out)
    write(v, 'hello.cpp', HELLO_CXX)
    out = v.run('g++ -O2 -std=c++20 hello.cpp -o hellocpp && ./hellocpp', 20)
    check('and C++, with its library', 'hello from C++' in out, out)

    v.run('mkdir mine && cd mine', 0.5)
    write(v, 'mine.c', TOOL)
    v.run(r"printf 'mine: mine.c\n\tcc -O2 -Wall $< -laximos -o $@\n' > Makefile", 1)  # its tab, which typing would complete
    out = v.run('make && ./mine && ./mine --help', 10)
    check('and make builds against libaximos', 'mine, built with libaximos' in out and 'A program built on AximOS' in out, out)

    # A machine of its own, which with no disk, tries to start from the network, through its card's firmware
    out = v.run('timeout 20 qemu-system-x86_64 -accel kvm -cpu host -M q35 -m 256 -display none -nodefaults '
                '-bios /usr/share/edk2/x64/OVMF.4m.fd -serial file:vm.log -netdev user,id=n -device e1000,netdev=n; '
                "grep -ao 'PXEv4[^)]*' vm.log | head -1", 25)
    check('and QEMU runs a machine, with KVM and UEFI', 'PXEv4' in out, out)
finally:
    v.close()
finish()
