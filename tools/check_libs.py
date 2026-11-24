"""Checks every program and library in build/root links only against the system's own
libraries: each library it needs is there, and has each symbol version it asks for. A
port that found the host's copy of a library when it was built fails this, as bash
once did with ncurses. Run by make after the ports are built."""
import os, re, subprocess, sys

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'build', 'root', 'system')
LIBS = os.path.join(ROOT, 'lib')


def readelf(*args):
    return subprocess.run(['readelf', *args], capture_output=True, text=True).stdout


def is_elf(path):
    try:
        with open(path, 'rb') as f:
            return f.read(4) == b'\x7fELF'
    except OSError:
        return False


defined = {}  # a library's file name, to the versions it defines


def versions_defined(name):
    if name not in defined:
        path = os.path.join(LIBS, name)
        defined[name] = set(re.findall(r'Name: (\S+)', readelf('-V', '--wide', path).split('Version needs')[0])) \
            if os.path.exists(path) else None
    return defined[name]


problems = []
for folder in ('bin', 'lib', 'lib/git-core'):
    base = os.path.join(ROOT, folder)
    for name in sorted(os.listdir(base)) if os.path.isdir(base) else []:
        path = os.path.join(base, name)
        if os.path.islink(path) or not os.path.isfile(path) or not is_elf(path):
            continue
        needs = readelf('-V', '--wide', path)
        if 'Version needs' not in needs:
            versions = {}
        else:
            versions, library = {}, None
            for line in needs.split('Version needs', 1)[1].splitlines():
                m = re.search(r'File: (\S+)', line)
                if m:
                    library = m.group(1)
                m = re.search(r'Name: (\S+)', line)
                if m and library:
                    versions.setdefault(library, set()).add(m.group(1))
        for lib in re.findall(r'\(NEEDED\).*\[(.+)\]', readelf('-d', path)):
            have = versions_defined(lib)
            if have is None:
                problems.append(f'{folder}/{name} needs {lib}, which the system hasn\'t')
            elif versions.get(lib, set()) - have:
                problems.append(f'{folder}/{name} needs {", ".join(sorted(versions[lib] - have))} from {lib}, '
                                'which its copy hasn\'t: it was linked against the host\'s')

for p in problems:
    print('check_libs:', p)
sys.exit(1 if problems else 0)
