# AximOS

An operating system for x86-64 PCs: the Linux kernel, with AximOS's own userland on
top, from init and the command-line tools to its window system, desktop and apps. Built
from source, Linux From Scratch style, against its own copy of glibc.

![The AximOS desktop: a see-through terminal and the quick settings panel](docs/screenshot.png)

AximOS used to have a kernel of its own as well, which is kept at the `kernel-final` tag.

## Status

AximOS is moving onto Linux, a phase at a time:

1. **Booting to a shell** (done): Linux, glibc, AximOS's init and dash, in QEMU
2. **The command line** (done): AximOS's own tools ported to glibc, the system tools
   written, accounts, and eudev for devices
3. **The desktop** (done): wm on DRM behind its log-in screen, through seatd, and the
   apps but Player and NetSurf, which come with sound and networking
4. **Real hardware**: an install image, the full kernel, firmware, and the Lenovo laptop
5. **Network and sound**: an AximOS DHCP daemon, iwd for Wi-Fi, and an ALSA mixer
6. **Packages**: AximOS's own package manager

Player and NetSurf, below, are still to come back, as are some of the tests.

## Features

### System

- Linux 7.3, with glibc 2.44 built for AximOS, booted by [Limine](https://limine-bootloader.org).
  The kernel is built from Arch's config, with its drivers as modules that AximOS's own
  `modprobe` loads, and has the Lenovo 82Y3's firmware from linux-firmware, and its CPU's
  microcode, loaded before the kernel starts. NVIDIA's GPUs are never driven
- From the ISO, the system runs in memory. Installed, it's on ext4, beside an EFI system
  partition for Limine, the kernel and the microcode. The kernel mounts it itself, as
  the disks it boots from have their drivers built in, and init has e2fsck check it
  before it's made writable
- netd, AximOS's own network service: it brings up each card as it appears, gets addresses
  by DHCP and DHCPv6, or uses those set by hand, takes IPv6 from the router, and sets DNS,
  preferring wired cards to Wi-Fi
- AximOS's own init, which mounts what Linux shares, starts the services in
  `/system/etc/services`, one file each, restarting those that ask, and keeps a text login
  on the screen's consoles from the second, Ctrl+Alt+F2, and on the serial port
- The desktop on the first console: wm on the screen through DRM, with the keyboard, mouse,
  touchpad and tablets from evdev, handed over by seatd. The log-in screen runs as its own
  account, and the session, which runs as root, checks passwords and starts wm as whoever
  logs in. Each account's settings, recent files, trash and desktop are its own, in
  `~/.config/aximos`
- eudev for device files and their permissions, which loads drivers for new devices through
  AximOS's own `modprobe`
- Accounts in `/etc/passwd` and `/etc/shadow`, hashed with yescrypt. The first boot makes
  the first account, on the log-in screen, or at a text console, which is in the `admin`
  group and can run commands as root with `sudo`, and set the time zone

### Command line

`dash`, with AximOS's line editor: the arrow keys, Home and End, Tab completion, Ctrl+W
and Ctrl+arrows edit the line, and Ctrl+R searches the history, which is kept in `~/.history`.

AximOS's own tools: `ls cat cp mv rm mkdir rmdir touch rename ln chmod chown stat find grep
sort head tail wc more du df free ps top kill date tz sleep env echo clear text axfetch`,
system tools `mount umount dmesg uname hostname sync lsmod insmod rmmod modprobe`, accounts
with `id whoami passwd useradd sudo`, `disk` (lists drives and their free space, partitions
them with GPT, adds partitions in free space beside what's there, and formats them as ext4 or
FAT32), `efiboot` (the firmware's boot entries and their order), `install` (installs AximOS from the
live system), `swapon`, `suspend`, `inputsim` (pretend laptop input, for the tests), `net`
(the network, as netd has it, and addresses set by hand), and `host nc ping`. Run any of them with `--help`. ext4 is
made and checked by [e2fsprogs](https://e2fsprogs.sourceforge.net)' `mkfs.ext4` and `e2fsck`.

### Desktop

- Smooth text in DejaVu Sans, and DejaVu Sans Mono in the terminal, the editor and text
  boxes, or the 8x16 bitmap font for those who'd rather have it sharp
- **wm**, a compositing window manager: rounded, anti-aliased windows with shadows,
  resizing from every edge, snapping to halves and quarters, layouts on the maximize
  button, minimizing, four workspaces, Alt+Tab, and an overview of every window side by side
- Resting the mouse on a taskbar button shows a live copy of its window
- Windows grow and fade in as they open, shrink away as they close, and fly to and
  from their taskbar buttons when minimized and restored, and move smoothly into place
  when maximized, restored or tiled. Menus and the taskbar's panels
  fade and slide in and out, and locking, logging in and logging out fade the screen
  across. Settings can turn all of it off.
- A floating, see-through taskbar at the top or bottom, which can hide itself (and still
  takes clicks at the very edge of the screen), with a start menu sorted into folders
  that searches the apps as you type, opened by throwing the mouse into the corner, and a
  show desktop corner at the other end
- Blurred glass for the taskbar, title bars, menus, panels, notifications and the
  window switcher, and for any window that asks for it (the terminal does). Settings
  can turn off the blur, or the transparency altogether, and set how round corners are, down
  to square
- A tray with quick settings (airplane mode, night light, dark mode, do not disturb,
  volume), the network, notifications, a calendar, and the clipboard's history, to put
  back something copied earlier
- On a laptop: the battery in the tray, with warnings at 10% and 5% and a suspend at 2%;
  brightness and volume keys, with a popup, and a brightness slider in quick settings;
  closing the lid locks and suspends (s2idle), and the power button asks whether to
  suspend, restart or shut down; and on the touchpad, tap to click (two fingers
  right-click), two-finger scrolling, and three-finger swipes between workspaces and up
  to the overview
- Notifications that can carry a button, and stack up when one app sends several
- Soft sounds of its own for notifications, logging in and out, the volume, snapping
  windows and screenshots, made as they play, which Settings can turn off
- Screenshots of the screen, a window, or an area dragged across, saved to Pictures
  and copied
- Desktop icons on a grid, drag and drop between apps, and a clipboard for text and pictures
- A log-in screen, a lock screen and first-boot setup. Booting to text asks for the
  same account.
- Light and dark themes, eight accent colours, wallpapers, resolution, time zones and
  12 or 24 hour time, all set in Settings
- Display scaling at 1x or 2x, chosen under Settings > Display and applied at once. At
  2x everything on the desktop is twice the size, each pixel drawn as four, while apps
  keep working in the desktop's own pixels. It's 2x by default on screens 2560 wide or
  more, and 1x on smaller ones, unless `/system/etc/gui.conf` says `scale 1` or `scale 2`
- **libgui**, an immediate-mode toolkit for apps: buttons, text boxes with undo,
  lists, tabs, menus, dialogs, a file picker, a code editor with syntax colouring,
  PNG/JPEG/BMP images and TrueType text

### Apps

| App        | What it does                                                          |
|------------|-----------------------------------------------------------------------|
| NetSurf    | Web browser, with HTTPS, CSS, smooth text, PNG, JPEG, GIF and WebP pictures, some JavaScript, downloads to ~/Downloads, and the system clipboard |
| Files      | File manager: places, recent files, search through folders, tabs, multi-select, thumbnails, Open with, and a trash you can restore from |
| Terminal   | Tabs, scrollback, selection, find, and a see-through background       |
| Editor     | Tabs, find and replace, go to line, colouring for C and shell scripts |
| Paint      | Pencil, shapes, fill, text, selections, zoom, undo and redo           |
| Viewer     | PNG, JPEG, BMP, GIF and WebP pictures, with zoom and turning          |
| Player     | WAV, MP3, FLAC, Vorbis, Opus and AAC, with shuffle and repeat         |
| Calculator | With a history                                                        |
| Tasks      | What's running, sortable, with a graph of CPU and memory use          |
| Installer  | Installs AximOS from the USB stick: in free space, over a whole drive, or again over itself, keeping /home |
| Settings   | Theme, accent, display, taskbar, keyboard and mouse, sound, date and time, and default apps |
| Snake      | Three speeds, each with its own best score                            |

## Installing

Boot the USB stick, then open System → Installer, or run `sudo install` at a terminal (see
`install --help`). It asks for the drive and how to install on it:

- **In its free space**, beside what's there, in the largest stretch no partition has
- **Over the whole drive**, erasing it
- **Again**, over an AximOS on the drive, keeping its accounts and `/home`, or erasing those too

AximOS gets an EFI system partition of its own, with Limine, the kernel and the microcode,
and the system on ext4, with a swap file of 16 GiB, or a quarter of the partition if that's
less. Its Limine menu offers the systems the firmware has entries for too, such as CachyOS,
which it starts as they are, and waits for a choice. The firmware gets an entry for AximOS,
first in its order, through `efiboot`. Nothing on other partitions is changed.

### Making room beside CachyOS

AximOS goes in free space on a drive, and never resizes what's there. Where a system fills
its drive, as CachyOS's btrfs does on the laptop's `nvme0n1p2`, make room from that system
first. For 128 GB, back up what matters, then in CachyOS:

```bash
sudo btrfs filesystem resize -130G /
```

That shrinks the filesystem a little more than the partition, which is then cut by 128 GiB,
from its end, and Linux told of it:

```bash
echo ", -128GiB" | sudo sfdisk -N 2 --no-reread /dev/nvme0n1
```

```bash
sudo partx -u /dev/nvme0n1
```

```bash
sudo btrfs filesystem resize max /
```

The last grows btrfs back to fill what's left of the partition. `lsblk` then shows
`nvme0n1p2` 128 GiB smaller, and AximOS's `disk` shows the space after it as free.

## Building

AximOS builds on Linux. You need:

- GCC, binutils and make, with what Linux and glibc need to build: `bc`, `flex`, `bison`,
  `libelf`, `openssl`, `gawk` and Python 3
- [Limine](https://limine-bootloader.org), installed in `/usr/share/limine`
- `xorriso`, `rsync`, `zstd`, `cpio`, `curl`, and mtools and e2fsprogs for the disk image
- QEMU (`qemu-system-x86_64`) and OVMF, its UEFI firmware (`/usr/share/edk2/x64/OVMF.4m.fd`), to run it
- `sdl2` and `libseat`, only for `make linux`

On Arch or CachyOS:

```bash
sudo pacman -S --needed base-devel bc flex bison libelf openssl gawk python limine xorriso rsync zstd cpio mtools e2fsprogs curl qemu-full edk2-ovmf sdl2 seatd
```

Then:

```bash
make
```

This builds `build/aximos.iso`. The first build downloads the source of Linux and Arch's
config for it, glibc, dash, libxcrypt, util-linux, e2fsprogs, eudev, seatd, the DejaVu
fonts, stb_truetype, the laptop's firmware and its CPU's microcode into `ports/dist` and
checks their SHA-256 sums, then builds glibc into `build/root`, which holds the system's
files, and Linux and the system against it. Arch's config has nearly every driver, so the
first kernel build takes a while; `make debug` builds one with only what QEMU and the
laptop need in a few minutes. After that only what changed is built again.

| Target        | What it does                                                    |
|---------------|-----------------------------------------------------------------|
| `make`        | Build `build/aximos.iso`                                        |
| `make debug`  | The same, with a kernel of only QEMU's and the laptop's drivers |
| `make disk`   | Build `build/aximos.img`, the system as it's installed on a disk |
| `make run`    | Build the ISO, then boot it in QEMU, with the shell in the terminal |
| `make run-disk` | The same, from the disk image, as an NVMe drive, which keeps what changes |
| `make test`   | Build both, then run the tests                                  |
| `make linux`  | Build wm and the calculator, Snake, the editor and Paint to run on this computer, in `build/linux` |
| `make clean`  | Delete everything built                                         |

### wm on Linux

`make linux` builds wm and some of the apps for the computer it's run on. From a desktop,
wm runs in a window, with the apps in its start menu:

```bash
build/linux/wm
```

From a text console (Ctrl+Alt+F3, say), it takes over the screen itself through DRM,
as on AximOS, with the screen and input from logind, through libseat. `--window` and
`--drm` pick one or the other. Super+Shift+Q quits, and puts the console back. The apps also run by themselves, in windows of their own, from
`build/linux/apps`.

### Keyboard shortcuts

| Keys                | Does                                  |
|---------------------|---------------------------------------|
| Super               | Start menu                            |
| Super+R             | Run a program, folder or file         |
| Super+Enter         | Terminal                              |
| Alt+Tab             | Switch windows                        |
| Super+Tab           | See every window, and pick one        |
| Super+Q             | Close the window                      |
| Super+arrows        | Maximize, or tile to a side, then Up or Down for a quarter |
| Super+1 to 4        | Change workspace                      |
| Super+L             | Lock it                               |
| Three fingers left or right | The workspace either side     |
| Three fingers up    | The overview                          |
| Super+Shift+Q       | Log out                               |
| Ctrl+Alt+F2 to F6   | A text console, and Ctrl+Alt+F1 back  |
| Print Screen        | Save the screen, and copy it          |
| Alt+Print Screen    | The same, of the focused window       |
| Shift+Print Screen  | Choose an area or window to save (also Super+Shift+S) |

## Tests

```bash
make test
```

Each `tests/*_test.py` boots its own QEMU, makes the first account and logs in on the
serial port, types into the shell, and checks what happens. Those for the desktop log in
on it too, and type and click there, with a USB tablet for the pointer. `utils_test.py` runs the
same commands with the host's GNU tools and compares. Several run at once, the slowest first; `make test JOBS=2` runs
fewer, and `make test TESTS="tests/boot_test.py"` runs just the ones named. The tests in
`tests/pending` are from before the move to Linux, and come back as they're ported.

## Layout

```
kernel/          how Linux is configured, and what the initramfs has besides the system
ports/           downloads and builds Linux, glibc, dash, libxcrypt, util-linux's libraries,
                 e2fsprogs, eudev, seatd and the fonts, into build/root
abi/             structures wm, libgui and the apps share, such as key and mouse events
user/init/       the first program Linux runs
user/session/    runs the log-in screen, then the desktop as whoever logs in
user/libaximos/  what AximOS's programs share beyond glibc: the line editor, accounts, /proc
user/libgui/     the GUI toolkit
user/libaudio/   reading and writing sound files, through user/codecs/
user/wm/         the window manager
user/<name>/     every other folder is a program
linux/           wm on DRM and evdev, and what wm and libgui need on another Linux desktop
rootfs/          copied into the system as it is: settings, icons, fonts, wallpapers
tests/           the tests, and vm.py, which drives QEMU for them
```

The system's own files are in `/system`: programs in `/system/bin`, libraries in
`/system/lib`, settings in `/system/etc`. `/bin`, `/lib`, `/usr` and `/etc`, which Linux
programs expect, link into it.

## Credits

AximOS uses [Linux](https://kernel.org), [glibc](https://www.gnu.org/software/libc/),
[Limine](https://github.com/limine-bootloader/limine), [dash](http://gondor.apana.org.au/~herbert/dash/),
[libxcrypt](https://github.com/besser82/libxcrypt), [seatd](https://git.sr.ht/~kennylevinsen/seatd), [util-linux](https://github.com/util-linux/util-linux)'s
libblkid and libuuid, [e2fsprogs](https://e2fsprogs.sourceforge.net), [eudev](https://github.com/eudev-project/eudev),
[stb_truetype](https://github.com/nothings/stb), the [DejaVu](https://dejavu-fonts.github.io) fonts,
[linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git),
[Intel's microcode](https://github.com/intel/Intel-Linux-Processor-Microcode-Data-Files) and
[Arch Linux](https://gitlab.archlinux.org/archlinux/packaging/packages/linux)'s kernel config.
Each is under its own license.
