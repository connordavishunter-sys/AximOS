#!/bin/sh
# Builds one part of the system into build/root, which holds what goes on its disk:
# build.sh glibc. Source comes from ports/dist, downloaded the first time, and
# unpacks in build/ports. Programs are compiled by the host's GCC against build/root,
# so they use the system's own glibc rather than the host's.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
target=$root/build/root
jobs=$(nproc)
target_cc="gcc --sysroot=$target"
linux=linux-7.3.5
glibc=glibc-2.44

fetch() { # url file sha256
	[ -e "$root/ports/dist/$2" ] || curl -fsSL -o "$root/ports/dist/$2" "$1/$2"
	echo "$3  $root/ports/dist/$2" | sha256sum -c --status
}

unpack() { # file directory
	[ -d "$2" ] || tar xf "$root/ports/dist/$1"
}

# Configures, builds and installs an autotools package for the system, in its folder.
autotools() { # configure options...
	if [ ! -e Makefile ]; then
		CC=$target_cc CFLAGS="-O2" ./configure --host=x86_64-linux-gnu --prefix=/usr --libdir=/usr/lib --sbindir=/usr/bin \
			--sysconfdir=/etc --disable-static --disable-nls "$@" >/dev/null
	fi
	make -j"$jobs" >/dev/null
	make install DESTDIR="$target" >/dev/null
}

# Packages find each other's libraries in the system, not on the host.
export PKG_CONFIG_SYSROOT_DIR="$target" PKG_CONFIG_LIBDIR="$target/usr/lib/pkgconfig"

mkdir -p "$root/build/ports" "$root/ports/dist"
cd "$root/build/ports"

case $1 in
skeleton) # AximOS's /system, with the paths Linux programs expect linking into it
	mkdir -p "$target/system/bin" "$target/system/lib" "$target/system/etc" "$target/system/share" \
		"$target/system/include" "$target/dev" "$target/proc" "$target/sys" "$target/run" "$target/tmp" \
		"$target/root" "$target/home" "$target/mnt"
	for link in bin:system/bin sbin:system/bin lib:system/lib lib64:system/lib usr:system etc:system/etc; do
		ln -sfn "${link#*:}" "$target/${link%%:*}"
	done
	ln -sfn bin "$target/system/sbin"
	ln -sfn lib "$target/system/lib64"
	touch "$target/system/etc/ld.so.conf"
	;;
linux) # the source, for the kernel and the headers glibc is built against
	fetch https://cdn.kernel.org/pub/linux/kernel/v7.x $linux.tar.xz \
		8f293f83d54f0b7b9be56863ebc5e4ea79d73e530b4ae55c572be97ef39f0990
	unpack $linux.tar.xz $linux
	make -C $linux O="$root/build/ports/linux-headers" headers_install INSTALL_HDR_PATH="$target/system" >/dev/null
	;;
glibc) # after linux
	fetch https://ftp.gnu.org/gnu/glibc $glibc.tar.xz 37f600f2bef3c5e8300147059568b2a2e40a7ad6ccc65ce942556d49429cc667
	unpack $glibc.tar.xz $glibc
	mkdir -p glibc-build
	cd glibc-build
	if [ ! -e config.status ]; then
		printf 'slibdir=/usr/lib\nrtlddir=/usr/lib\n' > configparms # in lib, not lib64, which links to it
		../$glibc/configure --prefix=/usr --libdir=/usr/lib --libexecdir=/usr/lib --with-headers="$target/system/include" \
			--enable-kernel=5.10 --enable-stack-protector=strong --disable-werror --disable-nscd >/dev/null
	fi
	make -j"$jobs" >/dev/null
	make install DESTDIR="$target" >/dev/null
	cp -L /usr/lib/libgcc_s.so.1 "$target/system/lib" # GCC's own, which unwinding needs
	;;
dash) # after glibc and libaximos, whose line editor it reads commands with
	fetch https://git.kernel.org/pub/scm/utils/dash/dash.git/snapshot dash-0.5.12.tar.gz \
		0d632f6b945058d84809cac7805326775bd60cb4a316907d0bd4228ff7107154
	if [ ! -d dash-0.5.12 ]; then
		tar xf "$root/ports/dist/dash-0.5.12.tar.gz"
		sed -i 's|"PATH=/usr/local/sbin.*"|"PATH=/system/bin"|' dash-0.5.12/src/var.c
		sed -i 's/defpathvar + 36/defpathvar + 5/' dash-0.5.12/src/var.h
		patch -d dash-0.5.12 -p1 < "$root/ports/dash.patch"
	fi
	cd dash-0.5.12
	if [ ! -e Makefile ]; then
		./autogen.sh >/dev/null
		CC=$target_cc CFLAGS="-O2" LIBS=-laximos ./configure --prefix=/usr --host=x86_64-linux-gnu --without-libedit >/dev/null
	fi
	make -j"$jobs" >/dev/null
	install -m 755 src/dash "$target/system/bin/dash"
	ln -sfn dash "$target/system/bin/sh"
	;;
libxcrypt) # after glibc: hashing passwords, which glibc no longer does
	fetch https://github.com/besser82/libxcrypt/releases/download/v4.5.2 libxcrypt-4.5.2.tar.xz \
		71513a31c01a428bccd5367a32fd95f115d6dac50fb5b60c779d5c7942aec071
	unpack libxcrypt-4.5.2.tar.xz libxcrypt-4.5.2
	cd libxcrypt-4.5.2
	autotools --enable-hashes=strong,glibc --enable-obsolete-api=no --disable-failure-tokens --disable-werror
	;;
util-linux) # after glibc: only its libraries, finding filesystems by what's on them and making UUIDs
	fetch https://mirrors.edge.kernel.org/pub/linux/utils/util-linux/v2.42 util-linux-2.42.4.tar.xz \
		fbd62a100ab7bb8746ba0661255c3c48185b1e9021507c624da01fbc696330ec
	unpack util-linux-2.42.4.tar.xz util-linux-2.42.4
	cd util-linux-2.42.4
	autotools --disable-all-programs --enable-libblkid --enable-libuuid --without-python --without-systemd \
		--without-udev --without-ncursesw --without-ncurses --disable-bash-completion
	;;
e2fsprogs) # after util-linux: making and checking ext4
	fetch https://mirrors.edge.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v1.47.4 e2fsprogs-1.47.4.tar.xz \
		fd5bf388cbdbe006a3d3b318d983b2948382440acc85a87f1e7d108653e8db0b
	unpack e2fsprogs-1.47.4.tar.xz e2fsprogs-1.47.4
	cd e2fsprogs-1.47.4
	autotools --enable-elf-shlibs --disable-libblkid --disable-libuuid --disable-uuidd --disable-fsck --disable-fuse2fs \
		--without-libarchive
	;;
eudev) # after e2fsprogs: device files, their permissions, and what's plugged in later
	fetch https://github.com/eudev-project/eudev/releases/download/v3.3.0 eudev-3.3.0.tar.gz \
		8fba6e4f49ea4b1938828a3c2e7cc48874816c68621386e527076b6477f5d6ef
	unpack eudev-3.3.0.tar.gz eudev-3.3.0
	cd eudev-3.3.0
	autotools --disable-kmod --disable-manpages --disable-selinux --enable-blkid
	;;
stb) # only stb_truetype, which libgui includes
	fetch https://raw.githubusercontent.com/nothings/stb/9600522de53973336fa033a289858a4354cbb04c stb_truetype.h \
		428cfe93520f2aed5e413a220b0db5bba5b17e2ebe5a64e4a8efa9eeeb67ec5f
	;;
*)
	echo "build.sh: no part called $1" >&2
	exit 1
	;;
esac
