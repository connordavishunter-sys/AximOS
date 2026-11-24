"""Checks every program and library in build/root, and in build/dev, the development
tools, links only against the system's own libraries, or the tools' own: each library
it needs is there, and has each symbol version it asks for. A port that found the
host's copy of a library when it was built fails this, as bash once did with ncurses.
Run by make after the ports are built."""
import os, re, subprocess, sys

BUILD = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'build')
TREES = [os.path.join(BUILD, tree, 'system') for tree in ('root', 'dev')]
LIB_DIRS = [os.path.join(tree, 'lib') for tree in TREES]


def readelf(*args):
    return subprocess.run(['readelf', *args], capture_output=True, text=True).stdout


def is_elf(path):
    try:
        with open(path, 'rb') as f:
            return f.read(4) == b'\x7fELF'
    except OSError:
        return False


defined = {}  # a library's file name, to the versions it defines


def versions_defined(name, extra):
    """Looking in the system's lib folders, and the program's others: its RUNPATH's, as Perl's
    libperl is in its own, and the one it's in, as glibc's gconv modules find the libraries they
    share."""
    if (name, extra) not in defined:
        path = next((os.path.join(d, name) for d in LIB_DIRS + list(extra) if os.path.exists(os.path.join(d, name))), None)
        defined[name, extra] = set(re.findall(r'Name: (\S+)', readelf('-V', '--wide', path).split('Version needs')[0])) \
            if path else None
    return defined[name, extra]


def programs(tree):
    """Each program and library in the tree's bin and lib, and lib's folders, such as Perl's and
    Python's modules and GCC's own programs, but not the kernel's modules or firmware."""
    for folder in ('bin', 'lib'):
        for base, dirs, files in os.walk(os.path.join(tree, folder)):
            if base == os.path.join(tree, 'lib'):
                dirs[:] = [d for d in dirs if d not in ('modules', 'firmware')]
            for name in sorted(files):
                path = os.path.join(base, name)
                if not name.endswith(('.o', '.a')) and not os.path.islink(path) and is_elf(path):
                    yield os.path.relpath(path, tree), path


problems = []
for tree in TREES:
    for where, path in programs(tree):
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
        dynamic = readelf('-d', path)
        runpath = [os.path.join(t, os.path.relpath(d, '/usr')) for d in ':'.join(re.findall(r'R(?:UN)?PATH\).*\[(.+)\]', dynamic)).split(':')
                   if d.startswith('/usr/') for t in TREES]
        for lib in re.findall(r'\(NEEDED\).*\[(.+)\]', dynamic):
            have = versions_defined(lib, (os.path.dirname(path), *runpath))
            if have is None:
                problems.append(f'{os.path.basename(os.path.dirname(tree))}/{where} needs {lib}, which the system hasn\'t')
            elif versions.get(lib, set()) - have:
                problems.append(f'{os.path.basename(os.path.dirname(tree))}/{where} needs {", ".join(sorted(versions[lib] - have))} from {lib}, '
                                'which its copy hasn\'t: it was linked against the host\'s')

for p in problems:
    print('check_libs:', p)
sys.exit(1 if problems else 0)
