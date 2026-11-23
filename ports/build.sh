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

mkdir -p "$root/build/ports" "$root/ports/dist"
cd "$root/build/ports"

case $1 in
skeleton) # AximOS's /system, with the paths Linux programs expect linking into it
	mkdir -p "$target/system/bin" "$target/system/lib" "$target/system/etc" "$target/system/share" \
		"$target/system/include" "$target/dev" "$target/proc" "$target/sys" "$target/run" "$target/tmp" \
		"$target/root" "$target/home"
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
dash) # after glibc
	fetch https://git.kernel.org/pub/scm/utils/dash/dash.git/snapshot dash-0.5.12.tar.gz \
		0d632f6b945058d84809cac7805326775bd60cb4a316907d0bd4228ff7107154
	if [ ! -d dash-0.5.12 ]; then
		tar xf "$root/ports/dist/dash-0.5.12.tar.gz"
		sed -i 's|"PATH=/usr/local/sbin.*"|"PATH=/system/bin"|' dash-0.5.12/src/var.c
		sed -i 's/defpathvar + 36/defpathvar + 5/' dash-0.5.12/src/var.h
	fi
	cd dash-0.5.12
	if [ ! -e Makefile ]; then
		./autogen.sh >/dev/null
		CC=$target_cc CFLAGS="-O2" ./configure --prefix=/usr --host=x86_64-linux-gnu --without-libedit >/dev/null
	fi
	make -j"$jobs" >/dev/null
	install -m 755 src/dash "$target/system/bin/dash"
	ln -sfn dash "$target/system/bin/sh"
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
