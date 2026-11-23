# AximOS

An operating system for x86-64 PCs: the Linux kernel, with AximOS's own userland on
top, from init and the command-line tools to its window system, desktop and apps. Built
from source, Linux From Scratch style, against its own copy of glibc.

![The AximOS desktop: a see-through terminal and the quick settings panel](docs/screenshot.png)

AximOS used to have a kernel of its own as well, which is kept at the `kernel-final` tag.

## Status

AximOS is moving onto Linux, a phase at a time:

1. **Booting to a shell** (done): Linux, glibc, AximOS's init and dash, in QEMU
2. **The command line**: AximOS's own tools ported to glibc, and the system tools written,
   with eudev for devices
3. **The desktop**: wm on DRM behind its log-in screen, through seatd, and the apps
4. **Real hardware**: an install image, the full kernel, firmware, and the Lenovo laptop
5. **Network and sound**: an AximOS DHCP daemon, iwd for Wi-Fi, and an ALSA mixer
6. **Packages**: AximOS's own package manager

The desktop and apps below are what's being brought over. They already run on a Linux
desktop through `make linux`.

## Features

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
| Install    | Installs AximOS on a drive, from the ISO, which boots straight into it |
| Settings   | Theme, accent, display, taskbar, keyboard and mouse, sound, date and time, and default apps |
| Snake      | Three speeds, each with its own best score                            |

## Building

AximOS builds on Linux. You need:

- GCC, binutils and make, with what Linux and glibc need to build: `bc`, `flex`, `bison`,
  `libelf`, `openssl`, `gawk` and Python 3
- [Limine](https://limine-bootloader.org), installed in `/usr/share/limine`
- `xorriso`, `rsync`, `zstd` and `curl`
- QEMU (`qemu-system-x86_64`) and OVMF, its UEFI firmware (`/usr/share/edk2/x64/OVMF.4m.fd`), to run it
- `sdl2` and `libdrm`, only for `make linux`

On Arch or CachyOS:

```bash
sudo pacman -S --needed base-devel bc flex bison libelf openssl gawk python limine xorriso rsync zstd curl qemu-full edk2-ovmf sdl2 libdrm
```

Then:

```bash
make
```

This builds `build/aximos.iso`. The first build downloads the source of Linux, glibc and
dash into `ports/dist` and checks their SHA-256 sums, then builds glibc into `build/root`,
which holds the system's files, and Linux and the system against it. That takes a few
minutes; after that only what changed is built again.

| Target        | What it does                                                    |
|---------------|-----------------------------------------------------------------|
| `make`        | Build `build/aximos.iso`                                        |
| `make run`    | Build it, then boot it in QEMU, with the shell in the terminal  |
| `make test`   | Build it, then run the tests                                    |
| `make linux`  | Build wm and the calculator, Snake, the editor and Paint to run on this computer, in `build/linux` |
| `make clean`  | Delete everything built                                         |

### wm on Linux

`make linux` builds wm and some of the apps for the computer it's run on. From a desktop,
wm runs in a window, with the apps in its start menu:

```bash
build/linux/wm
```

From a text console (Ctrl+Alt+F3, say), it takes over the screen itself through DRM,
reading the keyboard, mouse and touchpad from `/dev/input`, which needs the `input`
group. `--window` and `--drm` pick one or the other. Super+Shift+Q quits, and puts the
console back. The apps also run by themselves, in windows of their own, from
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
| Super+Shift+Q       | Leave the desktop                     |
| Print Screen        | Save the screen, and copy it          |
| Alt+Print Screen    | The same, of the focused window       |
| Shift+Print Screen  | Choose an area or window to save (also Super+Shift+S) |

## Tests

```bash
make test
```

Each `tests/*_test.py` boots its own QEMU, types into the shell on the serial port, and
checks what happens. Several run at once, the slowest first; `make test JOBS=2` runs
fewer, and `make test TESTS="tests/boot_test.py"` runs just the ones named. The tests in
`tests/pending` are for the desktop and apps, from before the move to Linux, and come
back as those do.

## Layout

```
kernel/          how Linux is configured, and what the initramfs has besides the system
ports/           downloads and builds Linux, glibc and dash, into build/root
abi/             structures wm, libgui and the apps share, such as key and mouse events
user/init/       the first program Linux runs
user/libgui/     the GUI toolkit
user/libaudio/   reading and writing sound files, through user/codecs/
user/wm/         the window manager
user/<name>/     every other folder is a program
linux/           what wm and libgui need to run on another Linux desktop
rootfs/          copied into the system as it is: settings, icons, fonts, wallpapers
tests/           the tests, and vm.py, which drives QEMU for them
```

The system's own files are in `/system`: programs in `/system/bin`, libraries in
`/system/lib`, settings in `/system/etc`. `/bin`, `/lib`, `/usr` and `/etc`, which Linux
programs expect, link into it.

## Credits

AximOS uses [Linux](https://kernel.org), [glibc](https://www.gnu.org/software/libc/),
[Limine](https://github.com/limine-bootloader/limine), [dash](http://gondor.apana.org.au/~herbert/dash/),
[stb_truetype](https://github.com/nothings/stb) and the [DejaVu](https://dejavu-fonts.github.io) fonts.
Each is under its own license.
