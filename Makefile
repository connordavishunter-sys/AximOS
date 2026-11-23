LIMINE_DIR := /usr/share/limine
OVMF := /usr/share/edk2/x64/OVMF.4m.fd

BUILD := build
ROOT := $(BUILD)/root# the system's files, as they go on its disk
IMAGE := $(BUILD)/image# the same without what's only for building, such as headers
STAMPS := $(BUILD)/stamps
ISO := $(BUILD)/aximos.iso
ISO_ROOT := $(BUILD)/iso_root

# Linux, configured by kernel/debug.config on top of its own x86_64 defaults.
LINUX_SRC := $(BUILD)/ports/linux-7.3.5
KERNEL_BUILD := $(BUILD)/kernel
KERNEL := $(KERNEL_BUILD)/arch/x86/boot/bzImage
INITRAMFS := $(BUILD)/initramfs.cpio.zst

# Programs are built by the host's GCC against the system's own glibc.
TARGET_CC := gcc --sysroot=$(ROOT)
TARGET_CFLAGS := -std=gnu23 -O2 -Wall -Wextra -Wno-format-truncation -D_GNU_SOURCE -Iabi

# The command-line programs, each from its folder in user/, linked against
# libaximos.so, which has what they share beyond glibc.
TOOLS := cat clear cp date df du echo env find free grep head kill ls mkdir more mv ps reboot rename rm rmdir \
	shutdown sleep sort stat tail text top touch tz wc counter hello host nc ping mount umount dmesg uname \
	hostname ln chmod chown id whoami sync lsmod insmod rmmod modprobe ttylogin sudo passwd useradd disk axfetch checkpass session
LIBS_disk := -lblkid
SETUID := sudo passwd tz reboot shutdown checkpass# which start as root, whoever runs them
LIBAXIMOS := $(ROOT)/system/lib/libaximos.so

# The desktop: libgui.so, wm, and the apps in /system/bin/gui, in the folders the
# start menu shows, or at the top for those in none. The programs that use libgui
# without a window of their own go in /system/bin.
LIBGUI := $(ROOT)/system/lib/libgui.so
GUI_CFLAGS := $(TARGET_CFLAGS) -Iuser/libgui/include -Iuser/libaximos/include -isystem ports/dist
APP_FOLDERS := Accessories Games System Demos
APPS_Accessories := calculator editor paint viewer
APPS_Games := snake
APPS_System := settings tasks
APPS_Demos := guidemo widgets
APPS := term files $(foreach f,$(APP_FOLDERS),$(APPS_$(f)))
app_dir = $(strip $(foreach f,$(APP_FOLDERS),$(if $(filter $(1),$(APPS_$(f))),$(f)/)))
APP_PATHS := $(foreach a,$(APPS),$(ROOT)/system/bin/gui/$(call app_dir,$(a))$(a))
GUI_TOOLS := run notify login
WM := $(ROOT)/system/bin/wm
WM_SOURCES := user/wm/wm.c linux/wm_drm.c linux/wm_display.c linux/evdev.c

.PHONY: all debug tools desktop linux run test clean help FORCE
.SECONDEXPANSION:

all: $(ISO)

# For now the same as all: the kernel with only what QEMU and the laptop need.
debug: all

# Each part of ports/build.sh, built once, after those it needs. Deleting its stamp
# builds it again.
$(STAMPS)/%:
	sh ports/build.sh $*
	@mkdir -p $(@D) && touch $@
$(STAMPS)/linux: $(STAMPS)/skeleton
$(STAMPS)/glibc: $(STAMPS)/linux
$(STAMPS)/dash: $(STAMPS)/glibc $(LIBAXIMOS) ports/dash.patch
$(STAMPS)/libxcrypt $(STAMPS)/util-linux: $(STAMPS)/glibc
$(STAMPS)/e2fsprogs $(STAMPS)/eudev: $(STAMPS)/util-linux
$(STAMPS)/seatd: $(STAMPS)/glibc
$(STAMPS)/dejavu: $(STAMPS)/skeleton
PORTS := dash libxcrypt util-linux e2fsprogs eudev seatd dejavu

$(ROOT)/system/bin/init: user/init/init.c $(STAMPS)/glibc
	$(TARGET_CC) $(TARGET_CFLAGS) $< -o $@
	ln -sfn system/bin/init $(ROOT)/init

$(LIBAXIMOS): $(wildcard user/libaximos/*.c user/libaximos/include/*.h) $(STAMPS)/libxcrypt
	cp user/libaximos/include/*.h $(ROOT)/system/include/
	$(TARGET_CC) $(TARGET_CFLAGS) -fPIC -shared -Wl,-soname,libaximos.so $(filter %.c,$^) -lcrypt -o $@

$(TOOLS:%=$(ROOT)/system/bin/%): $(ROOT)/system/bin/%: $$(wildcard user/$$*/*.c) $(LIBAXIMOS)
	$(TARGET_CC) $(TARGET_CFLAGS) $(filter %.c,$^) -laximos $(LIBS_$*) -o $@
	$(if $(filter $*,$(SETUID)),chmod 4755 $@)

tools: $(TOOLS:%=$(ROOT)/system/bin/%)

$(LIBGUI): $(wildcard user/libgui/*.c user/libgui/*.h user/libgui/include/*.h) $(LIBAXIMOS) $(STAMPS)/stb
	cp user/libgui/include/*.h $(ROOT)/system/include/
	$(TARGET_CC) $(GUI_CFLAGS) -fPIC -shared -Wl,-soname,libgui.so $(filter %.c,$^) -lm -o $@

$(APP_PATHS): $(ROOT)/system/bin/gui/%: $$(wildcard user/$$(notdir $$*)/*.c) $(LIBGUI)
	@mkdir -p $(@D)
	$(TARGET_CC) $(GUI_CFLAGS) $(filter %.c,$^) -lgui -laximos -lm -o $@

$(GUI_TOOLS:%=$(ROOT)/system/bin/%): $(ROOT)/system/bin/%: $$(wildcard user/$$*/*.c) $(LIBGUI)
	$(TARGET_CC) $(GUI_CFLAGS) $(filter %.c,$^) -lgui -laximos -lm -o $@

$(WM): $(WM_SOURCES) $(wildcard user/wm/*.h linux/*.h) $(LIBGUI) $(STAMPS)/seatd
	$(TARGET_CC) $(GUI_CFLAGS) $(WM_SOURCES) -lgui -laximos -lseat -lm -o $@

desktop: $(WM) $(APP_PATHS) $(GUI_TOOLS:%=$(ROOT)/system/bin/%)

# The kernel's own make decides what needs building again.
$(KERNEL_BUILD)/.config: kernel/debug.config $(STAMPS)/linux
	mkdir -p $(KERNEL_BUILD)
	$(MAKE) -s -C $(LINUX_SRC) O=$(abspath $(KERNEL_BUILD)) x86_64_defconfig
	$(LINUX_SRC)/scripts/kconfig/merge_config.sh -m -O $(KERNEL_BUILD) $(KERNEL_BUILD)/.config $< >/dev/null
	$(MAKE) -s -C $(LINUX_SRC) O=$(abspath $(KERNEL_BUILD)) olddefconfig

$(KERNEL): $(KERNEL_BUILD)/.config FORCE
	$(MAKE) -s -C $(LINUX_SRC) O=$(abspath $(KERNEL_BUILD)) -j$$(nproc) bzImage

# Until the system has a disk of its own, all of it is the initramfs.
$(INITRAMFS): $(ROOT)/system/bin/init tools desktop $(PORTS:%=$(STAMPS)/%) $(KERNEL) kernel/initramfs.list FORCE
	cp -r rootfs/. $(ROOT)/
	rsync -a --exclude posix --exclude right /usr/share/zoneinfo/ $(ROOT)/system/share/zoneinfo/
	rsync -a --delete --exclude /system/include --exclude '*.a' --exclude '*.o' $(ROOT)/ $(IMAGE)/
	cd $(KERNEL_BUILD) && $(abspath $(LINUX_SRC))/usr/gen_initramfs.sh -o $(abspath $(BUILD))/initramfs.cpio \
		-u squash -g squash $(abspath $(IMAGE)) $(abspath kernel/initramfs.list)
	zstd -q -f -T0 --rm $(BUILD)/initramfs.cpio -o $@

$(ISO): $(KERNEL) $(INITRAMFS) limine.conf
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot/limine $(ISO_ROOT)/EFI/BOOT
	cp $(KERNEL) $(ISO_ROOT)/boot/vmlinuz
	cp $(INITRAMFS) $(ISO_ROOT)/boot/initramfs.cpio.zst
	cp limine.conf $(LIMINE_DIR)/limine-uefi-cd.bin $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs -R -r -J --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
		--protective-msdos-label $(ISO_ROOT) -o $@ 2>/dev/null

# wm and apps built for Linux. wm runs in a window on the desktop, or on the screen
# itself from a text console, and the apps in it, or in windows of their own through
# SDL2 when they're started anywhere else. They use the fonts, icons and settings in
# rootfs, with the apps in the start menu's folders.
LINUX := $(BUILD)/linux
LINUX_APPS := calculator snake editor paint
LINUX_GUI := $(wildcard user/libgui/*.c) user/libaximos/util.c user/libaximos/net.c linux/gui_sdl.c linux/gui_switch.c
LINUX_WM := user/wm/wm.c $(wildcard linux/wm_*.c) linux/evdev.c
LINUX_LIBS := sdl2 libseat
LINUX_CFLAGS := -std=gnu23 -O2 -g -Wall -Wextra -Wno-format-truncation -D_GNU_SOURCE -DGUI_ROOT='"$(abspath $(LINUX))/root"' \
	-DGUI_APPS='"$(abspath $(LINUX))/apps"' -DGUI_BIN='"/usr/bin"' -DLIBWEBP='"libwebp.so.7"' -include linux/compat.h \
	-Iuser/libgui/include -Iuser/libaximos/include -idirafter abi -isystem ports/dist $(shell pkg-config --cflags $(LINUX_LIBS) 2>/dev/null)
linux_objs = $(addprefix $(LINUX)/,$(subst .c,.o,$(1) $(LINUX_GUI)))
linux: $(LINUX)/wm $(foreach p,$(LINUX_APPS),$(LINUX)/apps/$(call app_dir,$(p))$(p)) $(LINUX)/root

$(LINUX)/root: $(shell find rootfs) $(STAMPS)/dejavu
	mkdir -p $@ && cp -r rootfs/. $@/ && cp $(ROOT)/system/share/fonts/DejaVu*.ttf $@/system/share/fonts/ && touch $@

$(STAMPS)/stb: ports/build.sh
	sh ports/build.sh stb
	@mkdir -p $(@D) && touch $@

# libgui has both backends, each named for itself, and gui_switch.c picks one.
$(LINUX)/user/libgui/gui.o: BACKEND := -DBACKEND=wm -include linux/backend_names.h
$(LINUX)/linux/gui_sdl.o: BACKEND := -DBACKEND=sdl -include linux/backend_names.h

$(LINUX)/%.o: %.c $(STAMPS)/stb
	@mkdir -p $(@D)
	$(CC) $(LINUX_CFLAGS) $(BACKEND) -MMD -MP -c $< -o $@

$(LINUX)/wm: $(call linux_objs,$(LINUX_WM))
	$(CC) $^ -lm $(shell pkg-config --libs $(LINUX_LIBS) 2>/dev/null) -o $@

$(LINUX)/apps/%: $$(call linux_objs,$$(wildcard user/$$(notdir $$*)/*.c))
	@mkdir -p $(@D)
	$(CC) $^ -lm $(shell pkg-config --libs $(LINUX_LIBS) 2>/dev/null) -o $@

# KVM runs it at the CPU's own speed, where the host has it. A shell's on the serial
# port, in the terminal it's run from. The pointer's a USB tablet, which goes where the
# host's is, rather than a mouse, which only moves, so stops at the screen's edge
# while the host's goes on.
run: $(ISO)
	qemu-system-x86_64 -accel kvm -accel tcg -cpu max -smp 4 -m 1G -M q35 -bios $(OVMF) -serial mon:stdio \
		-netdev user,id=net0 -device e1000,netdev=net0 -device qemu-xhci -device usb-tablet -cdrom $(ISO)

# Each tests/*_test.py boots its own VMs, so several run at once. JOBS sets how many,
# and TESTS picks which, as in make test TESTS="tests/boot_test.py".
test: $(ISO)
	@python3 tests/run.py $(if $(JOBS),-j $(JOBS)) $(TESTS)

help:
	@echo "make           build the system into $(ISO)"
	@echo "make debug     the same, for now"
	@echo "make run       build it, then boot it in QEMU, with the shell in this terminal"
	@echo "make test      build it, then run the tests in tests/, several at once"
	@echo "               (JOBS=n sets how many, TESTS=\"tests/x_test.py ...\" picks which)"
	@echo "make linux     build wm and some apps to run on this computer, in $(LINUX)"
	@echo "make clean     delete everything that was built"

clean:
	rm -rf $(BUILD)

-include $(wildcard $(LINUX)/*/*.d $(LINUX)/*/*/*.d)
