LIMINE_DIR := /usr/share/limine
OVMF := /usr/share/edk2/x64/OVMF.4m.fd

BUILD := build
ROOT := $(BUILD)/root# the system's files, as they go on its disk
IMAGE := $(BUILD)/image# the same without what's only for building, such as headers
STAMPS := $(BUILD)/stamps
ISO := $(BUILD)/aximos.iso
ISO_ROOT := $(BUILD)/iso_root
DISK := $(BUILD)/aximos.img
ESP_MIB := 128
SYSTEM_MIB := 2048

# Linux: make builds Arch's config, with kernel/aximos.config's changes and its drivers
# as modules, and make debug the kernel's own x86_64 defaults, with kernel/debug.config's
# drivers built in.
LINUX_SRC := $(BUILD)/ports/linux-7.3.5
ifeq ($(filter debug,$(MAKECMDGOALS)),)
KERNEL_BUILD := $(BUILD)/kernel
KERNEL_CONFIG := kernel/aximos.config
kernel_base = cp ports/dist/linux-7.3.5-arch.config $(KERNEL_BUILD)/.config
else
KERNEL_BUILD := $(BUILD)/kernel-debug
KERNEL_CONFIG := kernel/debug.config
kernel_base = $(MAKE) -s -C $(LINUX_SRC) O=$(abspath $(KERNEL_BUILD)) x86_64_defconfig
endif
KERNEL := $(KERNEL_BUILD)/arch/x86/boot/bzImage
MODULES := $(KERNEL_BUILD)/modules# installed, in lib/modules/<release>
INITRAMFS := $(BUILD)/initramfs.cpio.zst

# Programs are built by the host's GCC against the system's own glibc.
TARGET_CC := gcc --sysroot=$(ROOT)
TARGET_CFLAGS := -std=gnu23 -O2 -Wall -Wextra -Wno-format-truncation -D_GNU_SOURCE -Iabi

# The command-line programs, each from its folder in user/, linked against
# libaximos.so, which has what they share beyond glibc.
TOOLS := cat clear cp date df du echo env find free grep head kill ls mkdir more mv ps reboot rename rm rmdir \
	shutdown sleep sort stat tail text top touch tz wc counter hello host nc ping mount umount dmesg uname \
	hostname ln chmod chown id whoami sync lsmod insmod rmmod modprobe ttylogin sudo passwd useradd disk efiboot install swapon suspend inputsim netd net wifi axfetch checkpass session
LIBS_disk := -lblkid
LIBS_mount := -lblkid
LIBS_install := -lblkid
SETUID := sudo passwd tz reboot shutdown suspend checkpass# which start as root, whoever runs them
LIBAXIMOS := $(ROOT)/system/lib/libaximos.so

# The desktop: libgui.so, wm, and the apps in /system/bin/gui, in the folders the
# start menu shows, or at the top for those in none. The programs that use libgui
# without a window of their own go in /system/bin.
LIBGUI := $(ROOT)/system/lib/libgui.so
GUI_CFLAGS := $(TARGET_CFLAGS) -Iuser/libgui/include -Iuser/libaximos/include -isystem ports/dist
APP_FOLDERS := Accessories Games System Demos
APPS_Accessories := calculator editor paint viewer
APPS_Games := snake
APPS_System := settings tasks installer
APPS_Demos := guidemo widgets
APPS := term files $(foreach f,$(APP_FOLDERS),$(APPS_$(f)))
app_dir = $(strip $(foreach f,$(APP_FOLDERS),$(if $(filter $(1),$(APPS_$(f))),$(f)/)))
APP_PATHS := $(foreach a,$(APPS),$(ROOT)/system/bin/gui/$(call app_dir,$(a))$(a))
GUI_TOOLS := run notify login
WM := $(ROOT)/system/bin/wm
WM_SOURCES := user/wm/wm.c linux/wm_drm.c linux/wm_display.c linux/evdev.c

.PHONY: all debug disk tools desktop linux run run-disk test clean help FORCE
.SECONDEXPANSION:

all: $(ISO)

disk: $(DISK)

# The same, but with a kernel of only what QEMU and the laptop need, which builds quickly.
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
$(STAMPS)/openssl $(STAMPS)/libnl: $(STAMPS)/glibc
$(STAMPS)/wpa_supplicant: $(STAMPS)/openssl $(STAMPS)/libnl ports/wpa_supplicant.config
$(STAMPS)/hostapd: $(STAMPS)/openssl $(STAMPS)/libnl ports/hostapd.config
$(STAMPS)/dejavu: $(STAMPS)/skeleton
$(STAMPS)/firmware: $(STAMPS)/skeleton kernel/firmware.list
PORTS := dash libxcrypt util-linux e2fsprogs eudev seatd dejavu firmware openssl libnl wpa_supplicant hostapd

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

# Its source is a release, which doesn't change, so it's built again only when its config
# does. The kernel's own make then decides what needs building, which takes a minute
# even when nothing does, with all of Arch's modules.
$(KERNEL_BUILD)/.config: $(KERNEL_CONFIG) $(STAMPS)/linux
	mkdir -p $(KERNEL_BUILD)
	$(kernel_base)
	$(LINUX_SRC)/scripts/kconfig/merge_config.sh -m -O $(KERNEL_BUILD) $(KERNEL_BUILD)/.config $< >/dev/null
	$(MAKE) -s -C $(LINUX_SRC) O=$(abspath $(KERNEL_BUILD)) olddefconfig

$(KERNEL): $(KERNEL_BUILD)/.config
	$(MAKE) -s -C $(LINUX_SRC) O=$(abspath $(KERNEL_BUILD)) -j$$(nproc) bzImage modules
	$(MAKE) -s -C $(LINUX_SRC) O=$(abspath $(KERNEL_BUILD)) -j$$(nproc) INSTALL_MOD_PATH=$(abspath $(MODULES)) \
		INSTALL_MOD_STRIP=1 modules_install
	touch $@

# Until the system has a disk of its own, all of it is the initramfs, with this kernel's
# modules, after the CPU's microcode, which the kernel looks for in a plain cpio first.
# It has what install puts on a drive's ESP too, and the accounts as they were built.
$(INITRAMFS): $(ROOT)/system/bin/init tools desktop $(PORTS:%=$(STAMPS)/%) $(KERNEL) kernel/initramfs.list FORCE
	cp -r rootfs/. $(ROOT)/
	rsync -a --exclude posix --exclude right /usr/share/zoneinfo/ $(ROOT)/system/share/zoneinfo/
	rsync -a --delete --exclude /system/include --exclude /system/lib/modules --exclude '*.a' --exclude '*.o' \
		$(ROOT)/ $(IMAGE)/
	rsync -a --delete --exclude '/*/build' $(MODULES)/lib/modules/ $(IMAGE)/system/lib/modules/
	mkdir -p $(IMAGE)/system/boot $(IMAGE)/system/share/install
	cp $(KERNEL) $(IMAGE)/system/boot/vmlinuz
	cp $(BUILD)/microcode.cpio $(LIMINE_DIR)/BOOTX64.EFI $(IMAGE)/system/boot/
	cp -p $(IMAGE)/system/etc/passwd $(IMAGE)/system/etc/group $(IMAGE)/system/etc/shadow $(IMAGE)/system/share/install/
	cd $(IMAGE) && find . -mindepth 1 | LC_ALL=C sort | cpio -o -H newc -R 0:0 --quiet > $(abspath $(BUILD))/initramfs.cpio
	$(KERNEL_BUILD)/usr/gen_init_cpio kernel/initramfs.list >> $(BUILD)/initramfs.cpio
	zstd -q -f -T0 --rm $(BUILD)/initramfs.cpio -o $(BUILD)/initramfs.zst
	cat $(BUILD)/microcode.cpio $(BUILD)/initramfs.zst > $@

$(ISO): $(KERNEL) $(INITRAMFS) limine.conf
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot/limine $(ISO_ROOT)/EFI/BOOT
	cp $(KERNEL) $(ISO_ROOT)/boot/vmlinuz
	cp $(INITRAMFS) $(ISO_ROOT)/boot/initramfs.cpio.zst
	cp limine.conf $(LIMINE_DIR)/limine-uefi-cd.bin $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/
	xorriso -as mkisofs -R -r -J --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
		--protective-msdos-label $(ISO_ROOT) -o $@ 2>/dev/null

# The system as it's installed, on a disk image: an EFI system partition with Limine,
# the kernel and the microcode, and the system on ext4 after it, which the kernel
# mounts itself, by its PARTUUID. Its files are root's, through a tarball.
$(DISK): $(KERNEL) $(INITRAMFS) limine-disk.conf
	rm -f $@ $(BUILD)/esp.img $(BUILD)/system.img
	truncate -s $$(( $(ESP_MIB) + $(SYSTEM_MIB) + 2 ))M $@
	printf 'label: gpt\nstart=1MiB, size=$(ESP_MIB)MiB, type=uefi\nsize=$(SYSTEM_MIB)MiB, type=linux\n' | sfdisk -q $@
	sed "s/@ROOT@/$$(sfdisk --part-uuid $@ 2)/" limine-disk.conf > $(BUILD)/limine-disk.conf
	truncate -s $(ESP_MIB)M $(BUILD)/esp.img
	mformat -i $(BUILD)/esp.img -F -v AXIMOS ::
	mmd -i $(BUILD)/esp.img ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
	mcopy -i $(BUILD)/esp.img $(LIMINE_DIR)/BOOTX64.EFI ::/EFI/BOOT/
	mcopy -i $(BUILD)/esp.img $(KERNEL) ::/boot/vmlinuz
	mcopy -i $(BUILD)/esp.img $(BUILD)/microcode.cpio ::/boot/
	mcopy -i $(BUILD)/esp.img $(BUILD)/limine-disk.conf ::/boot/limine/limine.conf
	tar --owner=0 --group=0 -C $(IMAGE) -cf $(BUILD)/system.tar .
	mke2fs -q -t ext4 -L AximOS -d $(BUILD)/system.tar $(BUILD)/system.img $(SYSTEM_MIB)M
	dd if=$(BUILD)/esp.img of=$@ bs=1M seek=1 conv=notrunc status=none
	dd if=$(BUILD)/system.img of=$@ bs=1M seek=$$(( 1 + $(ESP_MIB) )) conv=notrunc,sparse status=none
	rm $(BUILD)/esp.img $(BUILD)/system.img $(BUILD)/system.tar

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
	qemu-system-x86_64 -accel kvm -accel tcg -cpu max -smp 4 -m 2G -M q35 -bios $(OVMF) -serial mon:stdio \
		-netdev user,id=net0 -device e1000,netdev=net0 -device qemu-xhci -device usb-tablet -cdrom $(ISO)

# The same, from the disk image, as an NVMe drive, which keeps what's changed on it
# until it's built again.
run-disk: $(DISK)
	qemu-system-x86_64 -accel kvm -accel tcg -cpu max -smp 4 -m 2G -M q35 -bios $(OVMF) -serial mon:stdio \
		-netdev user,id=net0 -device e1000,netdev=net0 -device qemu-xhci -device usb-tablet \
		-drive file=$(DISK),format=raw,if=none,id=disk -device nvme,serial=aximos,drive=disk

# Each tests/*_test.py boots its own VMs, so several run at once. JOBS sets how many,
# and TESTS picks which, as in make test TESTS="tests/boot_test.py".
test: $(ISO) $(DISK)
	@python3 tests/run.py $(if $(JOBS),-j $(JOBS)) $(TESTS)

help:
	@echo "make           build the system into $(ISO)"
	@echo "make debug     the same, with a kernel of only QEMU's and the laptop's drivers, built in"
	@echo "make disk      build $(DISK), the system as it's installed on a disk"
	@echo "make run       build it, then boot it in QEMU, with the shell in this terminal"
	@echo "make run-disk  the same, from the disk image"
	@echo "make test      build it, then run the tests in tests/, several at once"
	@echo "               (JOBS=n sets how many, TESTS=\"tests/x_test.py ...\" picks which)"
	@echo "make linux     build wm and some apps to run on this computer, in $(LINUX)"
	@echo "make clean     delete everything that was built"

clean:
	rm -rf $(BUILD)

-include $(wildcard $(LINUX)/*/*.d $(LINUX)/*/*/*.d)
