comma := ,
CC := gcc
LD := ld
LIMINE_DIR := /usr/share/limine
OVMF := /usr/share/edk2/x64/OVMF.4m.fd

BUILD := build
KERNEL := $(BUILD)/kernel.elf
ISO := $(BUILD)/aximos.iso
ISO_ROOT := $(BUILD)/iso_root
DEBUG_ISO := $(BUILD)/aximos-debug.iso
SYSTEM := $(BUILD)/system# everything that goes on the disk, also on the ISO to install from
DISK := $(BUILD)/disk.img
DISK_SIZE_MB := 512

OBJS := $(patsubst src/%,$(BUILD)/%.o,$(basename $(wildcard src/*.c src/*.S)))

# uACPI, which runs the firmware's ACPI code, is built into the kernel from ports.
UACPI := $(BUILD)/ports/uACPI-7.0.0
UACPI_SOURCES := $(addprefix $(UACPI)/source/,default_handlers.c event.c interpreter.c io.c mutex.c namespace.c \
	notify.c opcodes.c opregion.c osi.c registers.c resources.c sleep.c stdlib.c tables.c types.c uacpi.c utilities.c)
UACPI_OBJS := $(patsubst $(UACPI)/source/%.c,$(BUILD)/uacpi/%.o,$(UACPI_SOURCES))

LIBC := $(BUILD)/user/libc.a
CRT0 := $(BUILD)/user/libc/crt0.o
LIBC_OBJS := $(filter-out $(CRT0) $(BUILD)/user/libc/ld.o,$(patsubst user/%,$(BUILD)/user/%.o,$(basename $(wildcard user/libc/*.c user/libc/*.S))))

# The same again for /system/lib/libc.so, which programs are linked against. It's
# also their dynamic linker, which is what ld.c adds.
LIBC_SO := $(BUILD)/user/libc.so
LIBC_SO_OBJS := $(patsubst user/libc/%,$(BUILD)/user/libc_pic/%.o,$(basename $(filter-out user/libc/crt0.S,$(wildcard user/libc/*.c user/libc/*.S))))

# /system/lib/libgui.so, for programs with windows, which wm, run and GUI_APPS are
# linked against. The apps go in /system/bin/gui, where wm's start menu finds them.
LIBGUI_SO := $(BUILD)/user/libgui.so
LIBGUI_SO_OBJS := $(patsubst user/libgui/%.c,$(BUILD)/user/libgui_pic/%.o,$(wildcard user/libgui/*.c))
GUI_APPS := guidemo term fbdemo widgets editor files viewer settings player calculator tasks paint snake install

# The start menu shows the folders in /system/bin/gui as they are, so the apps are
# put in these, or at the top for those in none of them.
APP_FOLDERS := Accessories Games Multimedia System Demos
APPS_Accessories := calculator editor paint viewer
APPS_Games := snake
APPS_Multimedia := player
APPS_System := settings tasks install
APPS_Demos := guidemo fbdemo widgets
app_dir = $(foreach f,$(APP_FOLDERS),$(if $(filter $(1),$(APPS_$(f))),$(f)/))

# /system/lib/libaudio.so reads and writes sound files, through a library for each
# kind, which it loads when one's first opened. Those wrap libraries from elsewhere,
# built as archives by ports/build.sh, and Vorbis and Opus share libogg.so.
LIBAUDIO_SO := $(BUILD)/user/libaudio.so
CODECS := wav mp3 flac vorbis opus aac
CODEC_SOS := $(CODECS:%=$(BUILD)/user/lib%.so)
LIBOGG_SO := $(BUILD)/user/libogg.so
CODEC_DIR := $(BUILD)/ports/codecs
CODEC_PORTS := ogg vorbis opus flac mp3 aac webp
CODEC_ARCHIVES := $(addprefix $(CODEC_DIR)/,ogg.a vorbis.a opus.a flac.a shine.a faad.a faac.a webp.a)
LIBWEBP_SO := $(BUILD)/user/libwebp.so # which libgui loads for WebP pictures
AUDIO_APPS := convert player
LIBGCC := $(shell $(CC) -print-libgcc-file-name)

# The time zones, from the host's tz database, without its copies under posix/ and
# right/ and anything that isn't a zone.
ZONEINFO := /usr/share/zoneinfo
ZONES := $(BUILD)/zoneinfo

# Programs from elsewhere, built by ports/build.sh.
DASH := $(BUILD)/ports/dash-0.5.12/src/dash
LUA := $(BUILD)/ports/lua-5.4.7/src/lua
MBEDTLS := $(BUILD)/ports/mbedtls-3.6.7
ZLIB := $(BUILD)/ports/sysroot/lib/libz.a
CURL := $(BUILD)/ports/curl-8.11.1/src/curl
NETSURF := $(BUILD)/ports/netsurf-all-3.11/netsurf/nsfb
NETSURF_RES := $(BUILD)/ports/netsurf-all-3.11/netsurf/frontends/framebuffer/res
FONTS := $(BUILD)/ports/fonts/DejaVuSans.ttf # and the rest of the DejaVu fonts NetSurf uses
MBEDTLS_LIBS := $(MBEDTLS)/library/libmbedtls.a $(MBEDTLS)/library/libmbedx509.a $(MBEDTLS)/library/libmbedcrypto.a

# Every folder in user/ but the libraries is a program. init goes in /system/boot, the
# GUI apps in /system/bin/gui and the others in /system/bin. rootfs/ is copied onto the disk as it is.
PROGRAMS := $(filter-out libc libgui libaudio codecs,$(notdir $(wildcard user/*)))
PROGRAM_ELFS := $(PROGRAMS:%=$(BUILD)/user/%.elf)
PROGRAM_OBJS := $(patsubst user/%.c,$(BUILD)/user/%.o,$(foreach p,$(PROGRAMS),$(wildcard user/$(p)/*.c)))
ROOTFS_FILES := $(shell find rootfs -type f)

BASE_CFLAGS := -std=gnu23 -O2 -g -Wall -Wextra -MMD -MP \
	-ffreestanding -fno-stack-protector -fno-stack-check -fno-pic -fno-pie \
	-fcf-protection=none -m64 -march=x86-64 -mno-red-zone

# The kernel never touches the floating point registers, so an interrupt doesn't
# have to save them. They only change hands when processes switch.
CFLAGS := $(BASE_CFLAGS) -fno-omit-frame-pointer -Iinclude -Iabi -I$(UACPI)/include -mcmodel=kernel -mno-80387 -mno-mmx -mno-sse -mno-sse2

# Programs are built small: each function has its own section, so linking leaves out
# whatever a program doesn't use. build/ keeps debug info and the disk gets it stripped.
USER_CFLAGS := $(BASE_CFLAGS) -Os -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-ffunction-sections -fdata-sections -nostdinc -isystem $(shell $(CC) -print-file-name=include) \
	-Iuser/libc/include -Iuser/libgui/include -Iabi
LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack
USER_LDFLAGS := -m elf_x86_64 -nostdlib -z max-page-size=0x1000 -z noexecstack --gc-sections -z noseparate-code \
	-Ttext-segment=0x400000 -e _start

# libc.so binds its calls to its own functions, so a program with a function of the
# same name doesn't change what libc does. Its variables can be the program's copies.
LIBC_SO_LDFLAGS := -m elf_x86_64 -nostdlib -shared --hash-style=sysv -soname libc.so -Bsymbolic-functions \
	-e _dlstart -z noexecstack
DYNAMIC_LDFLAGS := --hash-style=sysv -dynamic-linker /system/lib/libc.so

# Copies a file into the system without its debug info.
install_stripped = strip -s -o $(SYSTEM)$(2) $(1)

# KVM runs it at the CPU's own speed, where the host has it, and otherwise QEMU
# goes back to emulating every instruction, many times slower.
# Sound goes to PipeWire: make run AUDIO=pa, or another of qemu-system-x86_64 -audiodev help.
# QEMU feeds it from the thread that also draws its window, so PipeWire gets 100 ms
# of sound to keep going with while a redraw holds that up, rather than about 46.
AUDIO ?= pipewire
AUDIO_OPTIONS := $(if $(filter pipewire,$(AUDIO)),$(comma)out.latency=100000)
QEMU_MACHINE := qemu-system-x86_64 -accel kvm -accel tcg -cpu max -smp 4 -m 256M -serial stdio -bios $(OVMF) \
	-vga none -device VGA,vgamem_mb=64 -netdev user,id=net0 -device e1000,netdev=net0 \
	-audiodev $(AUDIO),id=snd0$(AUDIO_OPTIONS) -device ich9-intel-hda -device hda-output,audiodev=snd0
QEMU_BASE := $(QEMU_MACHINE) -boot d -cdrom $(ISO)

# pc has legacy IDE drives, named hda and so on. q35 has AHCI, named sda and so on.
QEMU := $(QEMU_BASE) -M pc -drive file=$(DISK),format=raw,if=ide,index=0
QEMU_Q35 := $(QEMU_BASE) -M q35 -drive file=$(DISK),format=raw,if=none,id=disk \
	-device ide-hd,drive=disk,bus=ide.1

# The ISO with a blank disk to install on, which is kept, so running it again boots
# what was installed.
INSTALL_DISK := $(BUILD)/install.img
INSTALL_DISK_MB := 2048

.PHONY: all debug linux help built run run-ide run-installer run-usb build-run test clean

# A recipe that fails leaves nothing behind that looks up to date.
.DELETE_ON_ERROR:

all: $(ISO) $(DISK)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.S
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# Its code is as it comes, so its warnings are left to it.
$(UACPI_OBJS): $(BUILD)/uacpi/%.o: $(UACPI)/source/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -w -c $< -o $@

$(OBJS) $(UACPI_SOURCES): | $(UACPI)/LICENSE
$(UACPI)/LICENSE:
	ports/build.sh uacpi

$(BUILD)/user/%.o: user/%.c
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/%.o: user/%.S
	@mkdir -p $(@D)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/libc_pic/%.o: user/libc/%.c
	@mkdir -p $(@D)
	$(CC) $(filter-out -fno-pic -fno-pie,$(USER_CFLAGS)) -fPIC -c $< -o $@

$(BUILD)/user/libc_pic/%.o: user/libc/%.S
	@mkdir -p $(@D)
	$(CC) $(filter-out -fno-pic -fno-pie,$(USER_CFLAGS)) -fPIC -c $< -o $@

$(LIBC_SO): $(LIBC_SO_OBJS)
	$(LD) $(LIBC_SO_LDFLAGS) $^ -o $@

$(BUILD)/user/libgui_pic/%.o: user/libgui/%.c
	@mkdir -p $(@D)
	$(CC) $(filter-out -fno-pic -fno-pie,$(USER_CFLAGS)) -fPIC -c $< -o $@

$(BUILD)/user/libgui_pic/font.o: ports/dist/stb_truetype.h
$(BUILD)/user/libgui_pic/font.o: USER_CFLAGS += -isystem ports/dist
ports/dist/stb_truetype.h: ports/build.sh
	ports/build.sh stb

$(LIBGUI_SO): $(LIBGUI_SO_OBJS) $(LIBC_SO)
	$(LD) -m elf_x86_64 -nostdlib -shared --hash-style=sysv -soname libgui.so -z noexecstack $^ -o $@

SHARED := $(LD) -m elf_x86_64 -nostdlib -shared --hash-style=sysv -z noexecstack

$(BUILD)/user/libaudio_pic/%.o: user/libaudio/%.c
	@mkdir -p $(@D)
	$(CC) $(filter-out -fno-pic -fno-pie,$(USER_CFLAGS)) -Iuser/libaudio/include -fPIC -c $< -o $@

$(LIBAUDIO_SO): $(patsubst user/libaudio/%.c,$(BUILD)/user/libaudio_pic/%.o,$(wildcard user/libaudio/*.c)) $(LIBC_SO)
	$(SHARED) -soname libaudio.so $^ -o $@

$(CODEC_ARCHIVES) &: $(LIBC_SO) ports/build.sh ports/aximos-cc
	for p in $(CODEC_PORTS); do ports/build.sh $$p || exit 1; done

$(LIBWEBP_SO): $(CODEC_ARCHIVES) $(LIBC_SO)
	$(SHARED) -soname libwebp.so --whole-archive $(CODEC_DIR)/webp.a --no-whole-archive $(LIBC_SO) $(LIBGCC) -o $@

$(LIBOGG_SO): $(CODEC_ARCHIVES) $(LIBC_SO)
	$(SHARED) -soname libogg.so --whole-archive $(CODEC_DIR)/ogg.a --no-whole-archive $(LIBC_SO) -o $@

# Each codec is its wrapper, the archives it wraps, and what those need.
CODEC_INCLUDES_mp3 := -I$(BUILD)/ports/minimp3-ea99364f61c14656440e8d77e9c233ccf3124633 -I$(BUILD)/ports/shine-3.1.1/src/lib
CODEC_INCLUDES_flac := -I$(BUILD)/ports/flac-1.5.0/include -DFLAC__NO_DLL
CODEC_INCLUDES_vorbis := -I$(BUILD)/ports/libvorbis-1.3.7/include -I$(BUILD)/ports/libogg-1.3.6/include
CODEC_INCLUDES_opus := -I$(BUILD)/ports/opus-1.6.1/include -I$(BUILD)/ports/libogg-1.3.6/include
CODEC_INCLUDES_aac := -I$(BUILD)/ports/faad2-2.11.2/include -I$(BUILD)/ports/faac-faac-2.2/include
CODEC_LIBS_mp3 := $(CODEC_DIR)/shine.a
CODEC_LIBS_flac := $(CODEC_DIR)/flac.a
CODEC_LIBS_vorbis := $(CODEC_DIR)/vorbis.a $(LIBOGG_SO)
CODEC_LIBS_opus := $(CODEC_DIR)/opus.a $(LIBOGG_SO)
CODEC_LIBS_aac := $(CODEC_DIR)/faad.a $(CODEC_DIR)/faac.a

$(BUILD)/user/codecs/%.o: user/codecs/%.c | $(CODEC_ARCHIVES)
	@mkdir -p $(@D)
	$(CC) $(filter-out -fno-pic -fno-pie -Os,$(USER_CFLAGS)) -O2 -Iuser/libaudio/include $(CODEC_INCLUDES_$*) \
		-fPIC -c $< -o $@

.SECONDEXPANSION:
$(CODEC_SOS): $(BUILD)/user/lib%.so: $(BUILD)/user/codecs/%.o $$(CODEC_LIBS_$$*) $(LIBAUDIO_SO) $(LIBC_SO)
	$(SHARED) -soname lib$*.so $^ $(LIBGCC) -o $@

$(KERNEL): $(OBJS) $(UACPI_OBJS) src/linker.ld
	$(LD) $(LDFLAGS) -T src/linker.ld $(OBJS) $(UACPI_OBJS) -o $@

$(LIBC): $(LIBC_OBJS)
	rm -f $@
	ar rcs $@ $^

# Programs share libc.so, except init: it's linked with all it needs, so that the
# system still starts if libc.so is missing or broken.
define PROGRAM
$(BUILD)/user/$(1).elf: $(CRT0) $(patsubst user/%.c,$(BUILD)/user/%.o,$(wildcard user/$(1)/*.c)) $(LIBC_SO)
	$$(LD) $$(USER_LDFLAGS) $$(DYNAMIC_LDFLAGS) $$+ -o $$@
endef
$(foreach p,$(filter-out init,$(PROGRAMS)),$(eval $(call PROGRAM,$(p))))
$(addprefix $(BUILD)/user/,$(addsuffix .elf,wm run notify login $(GUI_APPS))): $(LIBGUI_SO)
$(addprefix $(BUILD)/user/,$(addsuffix .elf,$(AUDIO_APPS))): $(LIBAUDIO_SO)
$(foreach a,$(AUDIO_APPS),$(BUILD)/user/$(a)/%.o): USER_CFLAGS += -Iuser/libaudio/include

$(BUILD)/user/init.elf: $(CRT0) $(patsubst user/%.c,$(BUILD)/user/%.o,$(wildcard user/init/*.c)) $(LIBC)
	$(LD) $(USER_LDFLAGS) -static $+ -o $@

# Drawing is where the window system spends its time, so it's built for speed
# rather than size.
$(BUILD)/user/libgui_pic/%.o $(BUILD)/user/wm/%.o $(BUILD)/user/term/%.o: USER_CFLAGS += -O2

# fetch speaks TLS, with mbedtls linked in.
$(BUILD)/user/fetch/fetch.o: USER_CFLAGS += -I$(MBEDTLS)/include -DMBEDTLS_CONFIG_FILE='"$(CURDIR)/ports/mbedtls_config.h"'
$(BUILD)/user/fetch/fetch.o: | $(MBEDTLS_LIBS)
$(BUILD)/user/fetch.elf: $(MBEDTLS_LIBS) $(shell $(CC) -print-libgcc-file-name)

# login hashes passwords with mbedtls's SHA-256, and ttylogin checks them the same way.
$(BUILD)/user/login/account.o: USER_CFLAGS += -I$(MBEDTLS)/include -DMBEDTLS_CONFIG_FILE='"$(CURDIR)/ports/mbedtls_config.h"'
$(BUILD)/user/login/account.o: | $(MBEDTLS_LIBS)
$(BUILD)/user/login.elf: $(MBEDTLS)/library/libmbedcrypto.a
$(BUILD)/user/ttylogin.elf: $(BUILD)/user/login/account.o $(MBEDTLS)/library/libmbedcrypto.a

$(MBEDTLS_LIBS) &: $(LIBC) $(CRT0) ports/build.sh ports/aximos-cc ports/mbedtls_config.h
	ports/build.sh mbedtls

$(DASH): $(LIBC_SO) $(CRT0) ports/build.sh ports/aximos-cc ports/dash.patch
	ports/build.sh dash

$(LUA): $(LIBC_SO) $(CRT0) ports/build.sh ports/aximos-cc ports/lua_aximos.h
	ports/build.sh lua

$(ZLIB): $(LIBC_SO) $(CRT0) ports/build.sh ports/aximos-cc
	ports/build.sh zlib

$(CURL): $(ZLIB) $(MBEDTLS_LIBS) ports/build.sh
	ports/build.sh curl

$(NETSURF): $(CURL) $(LIBGUI_SO) ports/build.sh $(wildcard ports/netsurf/*)
	ports/build.sh netsurf

$(FONTS): ports/build.sh
	ports/build.sh dejavu

$(ZONES):
	mkdir -p $@
	tar -C $(ZONEINFO) -chf - --exclude=./posix --exclude=./right . | tar -C $@ -xf -
	find $@ -type f | while read -r f; do [ "$$(head -c 4 "$$f")" = TZif ] || rm "$$f"; done

$(SYSTEM): $(PROGRAM_ELFS) $(LIBC_SO) $(LIBGUI_SO) $(LIBAUDIO_SO) $(LIBOGG_SO) $(CODEC_SOS) $(LIBWEBP_SO) $(ROOTFS_FILES) \
		$(DASH) $(LUA) $(CURL) $(NETSURF) $(FONTS) | $(ZONES)
	rm -rf $(SYSTEM)
	mkdir -p $(SYSTEM)
	cp -r rootfs/system $(SYSTEM)/
	mkdir -p $(SYSTEM)/system/boot $(SYSTEM)/system/bin/gui $(APP_FOLDERS:%=$(SYSTEM)/system/bin/gui/%) $(SYSTEM)/system/lib \
		$(SYSTEM)/tmp $(SYSTEM)/trash $(SYSTEM)/home/Desktop $(SYSTEM)/home/Documents $(SYSTEM)/home/Pictures
	cp -r $(ZONES) $(SYSTEM)/system/share/
	$(call install_stripped,$(LIBC_SO),/system/lib/libc.so)
	$(call install_stripped,$(LIBGUI_SO),/system/lib/libgui.so)
	for l in $(LIBAUDIO_SO) $(LIBOGG_SO) $(CODEC_SOS) $(LIBWEBP_SO); do $(call install_stripped,$$l,/system/lib/$$(basename $$l)); done
	$(call install_stripped,$(BUILD)/user/init.elf,/system/boot/init)
	for p in $(filter-out init $(GUI_APPS),$(PROGRAMS)); do $(call install_stripped,$(BUILD)/user/$$p.elf,/system/bin/$$p); done
	$(foreach p,$(GUI_APPS),$(call install_stripped,$(BUILD)/user/$(p).elf,/system/bin/gui/$(strip $(call app_dir,$(p)))$(p)) &&) true
	$(call install_stripped,$(DASH),/system/bin/dash)
	$(call install_stripped,$(LUA),/system/bin/lua)
	$(call install_stripped,$(CURL),/system/bin/curl)
	$(call install_stripped,$(NETSURF),/system/bin/gui/netsurf)
	cp -rL $(NETSURF_RES) $(SYSTEM)/system/share/netsurf && cp ports/netsurf/Choices $(SYSTEM)/system/share/netsurf/
	rm -rf $(addprefix $(SYSTEM)/system/share/netsurf/,de fr it ja nl zh_CN)
	cp $(BUILD)/ports/fonts/*.ttf $(SYSTEM)/system/share/fonts/

$(DISK): $(SYSTEM)
	rm -f $@
	mkfs.fat -F 32 -C $@ $$(($(DISK_SIZE_MB) * 1024))
	mcopy -s -i $@ $(SYSTEM)/* ::/

# The ISO boots by itself, from the same system as the disk, to install it.
make_iso = xorriso -as mkisofs -R -r -J --efi-boot boot/limine/limine-uefi-cd.bin \
	-efi-boot-part --efi-boot-image --protective-msdos-label $(1) -o $@

$(ISO): $(KERNEL) limine.conf $(SYSTEM)
	rm -rf $(ISO_ROOT)
	cp -r $(SYSTEM) $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot/limine $(ISO_ROOT)/EFI/BOOT
	cp $(KERNEL) $(ISO_ROOT)/boot/
	cp limine.conf $(LIMINE_DIR)/limine-uefi-cd.bin $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/
	$(call make_iso,$(ISO_ROOT))

# /system/etc/debug has init start the shell, without the desktop, as Shift does.
debug: all $(DEBUG_ISO)

$(DEBUG_ISO): $(ISO)
	rm -rf $(BUILD)/iso_debug
	cp -r $(ISO_ROOT) $(BUILD)/iso_debug
	touch $(BUILD)/iso_debug/system/etc/debug
	$(call make_iso,$(BUILD)/iso_debug)

# wm and apps built for Linux. wm runs in a window on the desktop, or on the screen
# itself from a text console, and the apps in it, or in windows of their own through
# SDL2 when they're started anywhere else. They use the built system's fonts, icons
# and settings, as on AximOS, with the apps in the start menu's folders.
LINUX := $(BUILD)/linux
LINUX_APPS := calculator snake editor paint
LINUX_GUI := $(wildcard user/libgui/*.c) linux/gui_sdl.c linux/gui_switch.c linux/compat.c
LINUX_WM := $(filter-out user/wm/display_aximos.c,$(wildcard user/wm/*.c)) $(wildcard linux/wm_*.c) linux/evdev.c
LINUX_LIBS := sdl2 libdrm
LINUX_CFLAGS := -std=gnu23 -O2 -g -Wall -Wextra -Wno-format-truncation -D_GNU_SOURCE -DGUI_ROOT='"$(abspath $(SYSTEM))"' \
	-DGUI_APPS='"$(abspath $(LINUX))/apps"' -DGUI_BIN='"/usr/bin"' -DLIBWEBP='"libwebp.so.7"' -include linux/compat.h \
	-Iuser/libgui/include -idirafter abi -isystem ports/dist $(shell pkg-config --cflags $(LINUX_LIBS) 2>/dev/null)
linux_objs = $(addprefix $(LINUX)/,$(subst .c,.o,$(1) $(LINUX_GUI)))

linux: $(LINUX)/wm $(foreach p,$(LINUX_APPS),$(LINUX)/apps/$(strip $(call app_dir,$(p)))$(p)) $(SYSTEM)

# libgui has both backends, each named for itself, and gui_switch.c picks one.
$(LINUX)/user/libgui/gui.o: BACKEND := -DBACKEND=wm -include linux/backend_names.h
$(LINUX)/linux/gui_sdl.o: BACKEND := -DBACKEND=sdl -include linux/backend_names.h

$(LINUX)/%.o: %.c ports/dist/stb_truetype.h
	@mkdir -p $(@D)
	$(CC) $(LINUX_CFLAGS) $(BACKEND) -MMD -MP -c $< -o $@

$(LINUX)/wm: $(call linux_objs,$(LINUX_WM))
	$(CC) $^ -lm $(shell pkg-config --libs $(LINUX_LIBS) 2>/dev/null) -o $@

$(LINUX)/apps/%: $$(call linux_objs,$$(wildcard user/$$(notdir $$*)/*.c))
	@mkdir -p $(@D)
	$(CC) $^ -lm $(shell pkg-config --libs $(LINUX_LIBS) 2>/dev/null) -o $@

# These run what's already built. make build-run rebuilds what changed first.
built:
	@test -f $(ISO) -a -f $(DISK) || { echo "Nothing built yet: run make first"; exit 1; }

run: built
	$(QEMU_Q35)

# The old IDE controller, which KVM makes much slower at writing.
run-ide: built
	$(QEMU)

run-installer: built
	test -f $(INSTALL_DISK) || truncate -s $(INSTALL_DISK_MB)M $(INSTALL_DISK)
	$(QEMU_BASE) -M q35 -drive file=$(INSTALL_DISK),format=raw,if=none,id=disk -device ide-hd,drive=disk,bus=ide.1

# The same, but with the ISO on a USB stick and no CD drive. The stick isn't changed.
run-usb: built
	test -f $(INSTALL_DISK) || truncate -s $(INSTALL_DISK_MB)M $(INSTALL_DISK)
	$(QEMU_MACHINE) -M q35 -drive file=$(INSTALL_DISK),format=raw,if=none,id=disk -device ide-hd,drive=disk,bus=ide.1 \
		-device qemu-xhci,id=usb -drive file=$(ISO),format=raw,if=none,id=stick,snapshot=on \
		-device usb-storage,bus=usb.0,drive=stick,bootindex=0

build-run: all
	$(QEMU_Q35)

# Each tests/*_test.py boots its own VMs from snapshots, so several run at once.
# JOBS sets how many, and TESTS picks which, as in make test TESTS="tests/usb_test.py".
test: built
	@python3 tests/run.py $(if $(JOBS),-j $(JOBS)) $(TESTS)

help:
	@echo "make                build whatever has changed (same as make all)"
	@echo "make run            run what's already built, using AHCI"
	@echo "make run-ide        run what's already built, using the slower IDE controller"
	@echo "make run-installer  boot the ISO with a blank disk to install on, kept in build/install.img"
	@echo "make run-usb        the same, with the ISO on a USB stick instead of a CD"
	@echo "make build-run      build whatever has changed, then run"
	@echo "make debug          build $(DEBUG_ISO) too, which boots to the shell, not the desktop"
	@echo "make test           run the tests in tests/ against what's already built, several at once"
	@echo "                    (JOBS=n sets how many, TESTS=\"tests/x_test.py ...\" picks which)"
	@echo "make clean          delete everything that was built"
	@echo "make help           show this list"

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d) $(UACPI_OBJS:.o=.d) $(LIBC_OBJS:.o=.d) $(LIBGUI_SO_OBJS:.o=.d) $(PROGRAM_OBJS:.o=.d) $(CRT0:.o=.d) $(wildcard $(LINUX)/*/*.d $(LINUX)/*/*/*.d)
