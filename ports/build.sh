#!/bin/sh
# Builds one part of the system into build/root, which holds what goes on its disk:
# build.sh glibc. Source comes from ports/dist, downloaded the first time, and
# unpacks in build/ports. Programs are compiled by the host's GCC against build/root,
# so they use the system's own glibc rather than the host's.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
target=$root/build/root
dev=$root/build/dev # the development tools, which go in /system on installed systems only
jobs=$(nproc)
target_cc="gcc --sysroot=$target"
linux=linux-7.3.5
glibc=glibc-2.44
arch_linux=d921c20ed125e79b4d0d7f100aeba9cdbf17043e # Arch's linux package, at 7.3.5.arch1-1
firmware=20261019
microcode=20261110
regdb=2026-09-03 # wireless-regdb's

fetch() { # url file sha256 [name to keep it as]
	name=${4:-$2}
	[ -e "$root/ports/dist/$name" ] || curl -fsSL -o "$root/ports/dist/$name" "$1/$2"
	echo "$3  $root/ports/dist/$name" | sha256sum -c --status
}

unpack() { # file directory
	[ -d "$2" ] || tar xf "$root/ports/dist/$1"
}

# Configures, builds and installs an autotools package for the system, in its folder,
# linking against the system's libraries first, as the compiler's own paths can reach
# the host's.
autotools() { # configure options...
	if [ ! -e Makefile ]; then
		CC=$target_cc CFLAGS="-O2" LDFLAGS="-L$target/system/lib" ./configure --host=x86_64-linux-gnu --prefix=/usr --libdir=/usr/lib --sbindir=/usr/bin \
			--sysconfdir=/etc --disable-static --disable-nls "$@" >/dev/null
	fi
	make -j"$jobs" >/dev/null
	make install DESTDIR="$target" >/dev/null
}

# The same for a development tool, into the development tree, finding libraries in it too, with
# CFLAGS of the port's own, if it has them.
devtools() { # configure options...
	if [ ! -e Makefile ]; then
		CC="$target_cc" CXX="g++ --sysroot=$target" CFLAGS="${CFLAGS:--O2}" CXXFLAGS="-O2" \
			CPPFLAGS="-I$dev/system/include" LDFLAGS="-L$target/system/lib -L$dev/system/lib" \
			./configure --host=x86_64-linux-gnu --prefix=/usr --libdir=/usr/lib --sbindir=/usr/bin --sysconfdir=/etc \
			--disable-static --disable-nls "$@" >/dev/null
	fi
	make -j"$jobs" >/dev/null
	make install DESTDIR="$dev" >/dev/null
}

# The same for a meson package. Its libraries are found in both trees, without a sysroot, as
# pkgconf works out each .pc file's prefix from where it is.
# Meson and ninja are the development tree's own, on the host too.
meson=$dev/system/bin/meson ninja=$dev/system/bin/ninja
mesondev() { # meson options...
	[ -e build/build.ninja ] || NINJA=$ninja env -u PKG_CONFIG_SYSROOT_DIR PKG_CONFIG_RELOCATE_PATHS=1 \
		PKG_CONFIG_LIBDIR="$target/system/lib/pkgconfig:$dev/system/lib/pkgconfig" \
		CC="$target_cc" CXX="g++ --sysroot=$target" LDFLAGS="-L$target/system/lib -L$dev/system/lib" \
		"$meson" setup build --prefix=/usr --libdir=lib --buildtype=release -Ddefault_library=shared "$@" >/dev/null
	"$ninja" -C build >/dev/null
	NINJA=$ninja "$meson" install -C build --strip --destdir "$dev" >/dev/null
}

# Packages find each other's libraries in the system, not on the host.
export PKG_CONFIG_SYSROOT_DIR="$target" PKG_CONFIG_LIBDIR="$target/usr/lib/pkgconfig"

mkdir -p "$root/build/ports" "$root/ports/dist"
mkdir -p "$dev/system/lib" # and its /usr and lib64, as the system's, leading into it
[ -e "$dev/usr" ] || ln -s system "$dev/usr"
[ -e "$dev/system/lib64" ] || ln -s lib "$dev/system/lib64"
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
linux) # the source, for the kernel and the headers glibc is built against, and Arch's config for it
	fetch https://cdn.kernel.org/pub/linux/kernel/v7.x $linux.tar.xz \
		8f293f83d54f0b7b9be56863ebc5e4ea79d73e530b4ae55c572be97ef39f0990
	fetch https://gitlab.archlinux.org/archlinux/packaging/packages/linux/-/raw/$arch_linux config.x86_64 \
		75366b057ddfafeb2c02a902aebdcbeb36ddcd56de74c82b1eadf4fac308555a $linux-arch.config
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
			--enable-kernel=5.10 --enable-stack-protector=strong --disable-werror --disable-nscd \
			--without-gd >/dev/null # not memusagestat, which would draw with the host's libgd
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
util-linux) # after glibc and ncurses: its libraries, finding filesystems by what's on them and making UUIDs, and fdisk, sfdisk and cfdisk
	fetch https://mirrors.edge.kernel.org/pub/linux/utils/util-linux/v2.42 util-linux-2.42.4.tar.xz \
		fbd62a100ab7bb8746ba0661255c3c48185b1e9021507c624da01fbc696330ec
	unpack util-linux-2.42.4.tar.xz util-linux-2.42.4
	cd util-linux-2.42.4
	autotools --disable-all-programs --enable-libblkid --enable-libuuid --enable-libfdisk --enable-libsmartcols --enable-fdisks \
		--without-python --without-systemd --without-udev --without-readline --disable-bash-completion
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
seatd) # after glibc: hands the screen and input to whichever session's console is showing
	fetch https://git.sr.ht/~kennylevinsen/seatd/archive 0.9.3.tar.gz \
		302564d54d8e28191fadfd734f2675ecb0c9e0615a58011b89ef15dfa4dbaa96 seatd-0.9.3.tar.gz
	unpack seatd-0.9.3.tar.gz seatd-0.9.3
	cd seatd-0.9.3
	flags="-std=c11 -O2 -w -D_XOPEN_SOURCE=700 -DSEATD_VERSION=\"0.9.3\" -DSEATD_DEFAULTPATH=\"/run/seatd.sock\" -DSEATD_INSTALLPATH=\"/system/bin/seatd\" -I. -Iinclude"
	$target_cc $flags -o "$target/system/bin/seatd" common/log.c common/linked_list.c common/terminal.c \
		common/connection.c common/evdev.c common/hidraw.c common/drm.c common/wscons.c seatd/*.c
	$target_cc $flags -DLIBSEAT=1 -DSEATD_ENABLED=1 -fPIC -shared -Wl,-soname,libseat.so.1 \
		-Wl,--version-script,libseat/libseat.syms -o "$target/system/lib/libseat.so.1" libseat/libseat.c \
		libseat/backend/noop.c libseat/backend/seatd.c common/connection.c common/linked_list.c common/log.c
	ln -sf libseat.so.1 "$target/system/lib/libseat.so"
	cp include/libseat.h "$target/system/include/"
	;;
firmware) # what kernel/firmware.list names, and the Intel CPUs' microcode, loaded before the kernel starts
	mkdir -p "$root/ports/dist/firmware" "$target/system/lib/firmware"
	grep -v '^#' "$root/kernel/firmware.list" | while read -r sum path link; do
		[ -n "$sum" ] || continue
		file=$root/ports/dist/firmware/$path
		[ -e "$file" ] || curl -fsSL --create-dirs -o "$file" \
			"https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/$path?h=$firmware"
		echo "$sum  $file" | sha256sum -c --status
		install -D -m 644 "$file" "$target/system/lib/firmware/$path"
		if [ -n "$link" ]; then
			mkdir -p "$(dirname "$target/system/lib/firmware/$link")"
			ln -sfn "$(realpath -m --relative-to="$(dirname "/$link")" "/$path")" "$target/system/lib/firmware/$link"
		fi
	done
	# Wi-Fi's regulatory database, which the kernel checks is signed, so it can follow the
	# country routers say they're in, rather than keeping to what's allowed everywhere
	for f in regulatory.db regulatory.db.p7s; do
		fetch https://git.kernel.org/pub/scm/linux/kernel/git/wens/wireless-regdb.git/plain "$f?h=master-$regdb" \
			"$(grep " $f\$" "$root/kernel/regdb.sums" | cut -d' ' -f1)" "$regdb-$f"
		install -m 644 "$root/ports/dist/$regdb-$f" "$target/system/lib/firmware/$f"
	done
	# Only the 82Y3's i9-13900H's (family 6, model 0xba, stepping 2), in the early cpio the kernel looks for it in
	fetch https://raw.githubusercontent.com/intel/Intel-Linux-Processor-Microcode-Data-Files/microcode-$microcode/intel-ucode \
		06-ba-02 184f72215da596c1bd05e5fbb032fb7a7bf897a32fad5274fe01da115c07a047 intel-ucode-$microcode-06-ba-02
	rm -rf microcode && mkdir -p microcode/kernel/x86/microcode
	cp "$root/ports/dist/intel-ucode-$microcode-06-ba-02" microcode/kernel/x86/microcode/GenuineIntel.bin
	(cd microcode && find kernel | cpio -o -H newc -R 0:0 --quiet) > "$root/build/microcode.cpio"
	;;
dejavu) # the desktop's text, and NetSurf's
	fetch https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37 dejavu-fonts-ttf-2.37.tar.bz2 \
		fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7
	mkdir -p "$target/system/share/fonts"
	tar xjf "$root/ports/dist/dejavu-fonts-ttf-2.37.tar.bz2" -C "$target/system/share/fonts" --strip-components=2 \
		$(for f in Sans Sans-Bold Sans-Oblique Sans-BoldOblique Serif Serif-Bold SansMono SansMono-Bold; do
			echo dejavu-fonts-ttf-2.37/ttf/DejaVu$f.ttf; done)
	;;
stb) # only stb_truetype, which libgui includes
	fetch https://raw.githubusercontent.com/nothings/stb/9600522de53973336fa033a289858a4354cbb04c stb_truetype.h \
		428cfe93520f2aed5e413a220b0db5bba5b17e2ebe5a64e4a8efa9eeeb67ec5f
	;;
openssl) # after glibc: TLS, for HTTPS, and the cryptography wpa_supplicant's WPA3 needs
	fetch https://github.com/openssl/openssl/releases/download/openssl-4.1.0 openssl-4.1.0.tar.gz \
		50c17baf54cb008f6ce42d50deb4b28050a1aad711636cabd4b241c7e90cc2fd
	unpack openssl-4.1.0.tar.gz openssl-4.1.0
	cd openssl-4.1.0
	[ -e Makefile ] || CC=$target_cc ./Configure linux-x86_64 --prefix=/usr --libdir=lib --openssldir=/etc/ssl \
		shared no-docs no-tests no-static-engine -O2 >/dev/null
	make -j"$jobs" build_sw >/dev/null
	make install_sw install_ssldirs DESTDIR="$target" >/dev/null
	ln -sfn ../ca-certificates.crt "$target/system/etc/ssl/cert.pem" # the system's CAs, as OpenSSL looks for them
	;;
libnl) # after glibc: how wpa_supplicant talks to Linux's Wi-Fi, through netlink
	fetch https://github.com/thom311/libnl/releases/download/libnl3_12_0 libnl-3.12.0.tar.gz \
		fc51ca7196f1a3f5fdf6ffd3864b50f4f9c02333be28be4eeca057e103c0dd18
	unpack libnl-3.12.0.tar.gz libnl-3.12.0
	cd libnl-3.12.0
	autotools --disable-cli --disable-debug
	;;
wpa_supplicant) # after openssl and libnl: joins Wi-Fi networks, for netd
	fetch https://w1.fi/releases wpa_supplicant-2.12.tar.gz \
		08e23937e16d0155e55cab2b51f51fbe10d80a1aa91c4e15442645059b737ef6
	unpack wpa_supplicant-2.12.tar.gz wpa_supplicant-2.12
	cd wpa_supplicant-2.12/wpa_supplicant
	cp "$root/ports/wpa_supplicant.config" .config
	make -j"$jobs" CC="$target_cc" PKG_CONFIG=pkg-config wpa_supplicant wpa_cli >/dev/null
	install -m 755 wpa_supplicant wpa_cli "$target/system/bin/"
	;;
hostapd) # after openssl and libnl: an access point, which the Wi-Fi test joins, on a simulated radio
	fetch https://w1.fi/releases hostapd-2.12.tar.gz f43502561c28ba47ab77e18e1a973d07361c68cc8b14178e619bd5796b70eabd
	unpack hostapd-2.12.tar.gz hostapd-2.12
	cd hostapd-2.12/hostapd
	cp "$root/ports/hostapd.config" .config
	make -j"$jobs" CC="$target_cc" PKG_CONFIG=pkg-config hostapd >/dev/null
	install -m 755 hostapd "$target/system/bin/"
	;;
zlib-ng) # after glibc: zlib's compression, which git and curl need, as zlib-ng's compatible build
	fetch https://github.com/zlib-ng/zlib-ng/archive/refs/tags 2.3.3.tar.gz \
		f9c65aa9c852eb8255b636fd9f07ce1c406f061ec19a2e7d508b318ca0c907d1 zlib-ng-2.3.3.tar.gz
	unpack zlib-ng-2.3.3.tar.gz zlib-ng-2.3.3
	cd zlib-ng-2.3.3
	[ -e Makefile ] || CC=$target_cc CFLAGS=-O2 ./configure --prefix=/usr --libdir=/usr/lib --zlib-compat >/dev/null
	make -j"$jobs" >/dev/null
	make install DESTDIR="$target" >/dev/null
	rm -f "$target/usr/lib/libz.a"
	;;
curl) # after openssl and zlib-ng: HTTPS downloads, for git, and Claude Code's installer
	fetch https://curl.se/download curl-8.23.0.tar.xz 8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5
	unpack curl-8.23.0.tar.xz curl-8.23.0
	cd curl-8.23.0
	autotools --with-openssl --with-zlib --with-ca-bundle=/etc/ssl/cert.pem --enable-ipv6 --enable-threaded-resolver \
		--disable-ldap --disable-ldaps --disable-manual --without-libpsl --without-brotli --without-zstd \
		--without-libidn2 --without-nghttp2 --without-libssh2
	;;
ncurses) # after glibc: terminfo, the terminals' sequences, for the few terminals AximOS meets, and the library that reads it
	fetch https://invisible-island.net/archives/ncurses ncurses-6.6.tar.gz \
		355b4cbbed880b0381a04c46617b7656e362585d52e9cf84a67e2009b749ff11
	unpack ncurses-6.6.tar.gz ncurses-6.6
	cd ncurses-6.6
	autotools --with-shared --without-normal --without-debug --without-cxx-binding --without-ada --without-manpages \
		--without-tests --enable-widec --enable-pc-files --with-pkg-config-libdir=/usr/lib/pkgconfig \
		--with-default-terminfo-dir=/usr/share/terminfo --disable-root-environ --disable-db-install
	# Compiled by the host's tic, which writes the same format, as the system's can't run here
	mkdir -p "$target/system/share/terminfo"
	tic -x -e linux,xterm,xterm-256color,xterm-color,vt100,vt220,screen,screen-256color,tmux,tmux-256color,dumb,ansi \
		-o "$target/system/share/terminfo" misc/terminfo.src
	for lib in ncurses tinfo; do # for programs linked against those names
		ln -sf libncursesw.so.6 "$target/system/lib/lib$lib.so.6"
		ln -sf libncursesw.so "$target/system/lib/lib$lib.so"
	done
	;;
bash) # after glibc and ncurses: the shell most scripts expect, with its patches to 5.3.20
	fetch https://ftp.gnu.org/gnu/bash bash-5.3.tar.gz "$(grep ' bash-5.3.tar.gz$' "$root/ports/bash.sums" | cut -d' ' -f1)"
	for patch in $(seq -f %03g 1 20); do
		fetch https://ftp.gnu.org/gnu/bash/bash-5.3-patches bash53-$patch \
			"$(grep " bash53-$patch\$" "$root/ports/bash.sums" | cut -d' ' -f1)"
	done
	if [ ! -d bash-5.3 ]; then
		unpack bash-5.3.tar.gz bash-5.3
		for patch in $(seq -f %03g 1 20); do
			patch -s -d bash-5.3 -p0 < "$root/ports/dist/bash53-$patch"
		done
	fi
	cd bash-5.3
	bash_cv_termcap_lib=libncurses autotools --without-bash-malloc --disable-rpath
	;;
git) # after curl: version control, with HTTPS through curl, and AximOS's more and text as its pager and editor,
	# without its parts in Rust, which AximOS has no compiler for
	fetch https://mirrors.edge.kernel.org/pub/software/scm/git git-2.56.1.tar.xz \
		f0b0bf89264b9bd2e1c640363e7529643989da76281600473c50745cd34a607f
	unpack git-2.56.1.tar.xz git-2.56.1
	cd git-2.56.1
	make -j"$jobs" prefix=/usr gitexecdir=/usr/lib/git-core CC="$target_cc" CFLAGS="-O2 -w" \
		LDFLAGS="-L$target/system/lib" CURL_LDFLAGS="-L$target/system/lib -lcurl" \
		NO_PERL=1 NO_PYTHON=1 NO_TCLTK=1 NO_GETTEXT=1 NO_EXPAT=1 NO_OPENSSL=1 NO_RUST=1 INSTALL_SYMLINKS=1 \
		DEFAULT_PAGER=more DEFAULT_EDITOR=text DESTDIR="$target" strip install >/dev/null
	rm -f "$target"/system/lib/git-core/git-daemon "$target"/system/lib/git-core/git-http-backend \
		"$target"/system/lib/git-core/git-imap-send # serving repositories, and mailing patches, which it isn't for
	;;
coreutils) # after glibc: ls, cp, sort, sha256sum and the rest, as every script expects them
	fetch https://ftp.gnu.org/gnu/coreutils coreutils-9.12.tar.xz a480198559733e9b3da999e90543ac6f888a2caa544d8d664c5a1f17e528e210
	unpack coreutils-9.12.tar.xz coreutils-9.12
	cd coreutils-9.12
	autotools --disable-acl --disable-xattr --disable-libcap --without-selinux --without-gmp --without-openssl \
		--enable-no-install-program=kill,hostname,uptime # AximOS's own
	;;
sed) # after glibc
	fetch https://ftp.gnu.org/gnu/sed sed-4.10.tar.xz b8e72182b2ec96a3574e2998c47b7aaa64cc20ce000d8e9ac313cc07cecf28c7
	unpack sed-4.10.tar.xz sed-4.10
	cd sed-4.10
	autotools --disable-acl --without-selinux
	;;
grep) # after glibc, without Perl's regular expressions
	fetch https://ftp.gnu.org/gnu/grep grep-3.12.tar.xz 2649b27c0e90e632eadcd757be06c6e9a4f48d941de51e7c0f83ff76408a07b9
	unpack grep-3.12.tar.xz grep-3.12
	cd grep-3.12
	autotools --disable-perl-regexp
	;;
gawk) # after glibc: awk
	fetch https://ftp.gnu.org/gnu/gawk gawk-5.4.2.tar.xz b95e4909533b5f988c6b5e5539b3b3ddf76e7d7df02ca2e5b5b7d6f62db78e6a
	unpack gawk-5.4.2.tar.xz gawk-5.4.2
	cd gawk-5.4.2
	autotools --without-readline --without-mpfr --disable-pma
	;;
findutils) # after glibc: find and xargs
	fetch https://ftp.gnu.org/gnu/findutils findutils-4.11.0.tar.xz bfd19cb06cc71f3352d567e90284d8cdac02ac89774bbeadf0b533b0c11432fd
	unpack findutils-4.11.0.tar.xz findutils-4.11.0
	cd findutils-4.11.0
	autotools --without-selinux --localstatedir=/var/lib/locate
	;;
diffutils) # after glibc: diff and cmp
	fetch https://ftp.gnu.org/gnu/diffutils diffutils-3.12.tar.xz 7c8b7f9fc8609141fdea9cece85249d308624391ff61dedaf528fcb337727dfd
	unpack diffutils-3.12.tar.xz diffutils-3.12
	cd diffutils-3.12
	autotools
	;;
openssh) # after openssl and zlib-ng: ssh, ssh-keygen and ssh-agent, for git and logging in, and sshd, off unless turned on
	fetch https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable openssh-10.6p2.tar.gz \
		9ae44872090cf2f06b3cf9ff147e7f16f309671340763ed2f99248398ae29d5b
	unpack openssh-10.6p2.tar.gz openssh-10.6p2
	cd openssh-10.6p2
	# Its own SHA-2, not the host's libmd, which configure would otherwise find
	[ -e Makefile ] || ac_cv_search_SHA256Update=no CC=$target_cc CFLAGS="-O2 -w" LDFLAGS="-L$target/system/lib" ./configure --host=x86_64-linux-gnu \
		--prefix=/usr --sbindir=/usr/bin --libexecdir=/usr/lib/ssh --sysconfdir=/etc/ssh --with-privsep-path=/var/empty \
		--with-privsep-user=sshd --with-default-path=/system/bin --with-superuser-path=/system/bin --without-pam \
		--without-selinux --disable-strip >/dev/null
	make -j"$jobs" >/dev/null
	make install-nokeys DESTDIR="$target" >/dev/null # its host keys are made where it runs, the first time
	install -d -m 755 "$target/var/empty"
	;;
zstd) # after glibc: zstd's compression, which the development tools' archive is in
	fetch https://github.com/facebook/zstd/releases/download/v1.5.7 zstd-1.5.7.tar.gz \
		eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
	unpack zstd-1.5.7.tar.gz zstd-1.5.7
	cd zstd-1.5.7
	# Its own flags, which it adds to, and the system's libraries first in the compiler's command
	make -j"$jobs" CC="$target_cc -L$target/system/lib" HAVE_ZLIB=0 HAVE_LZMA=0 HAVE_LZ4=0 PREFIX=/usr >/dev/null
	make install CC="$target_cc -L$target/system/lib" HAVE_ZLIB=0 HAVE_LZMA=0 HAVE_LZ4=0 PREFIX=/usr DESTDIR="$target" \
		>/dev/null
	rm -f "$target/system/lib/libzstd.a"
	;;
tar) # after glibc: GNU tar, which unpacks the development tools, with zstd
	fetch https://ftp.gnu.org/gnu/tar tar-1.35.tar.xz 4d62ff37342ec7aed748535323930c7cf94acf71c3591882b26a7ea50f3edc16
	unpack tar-1.35.tar.xz tar-1.35
	cd tar-1.35
	autotools --without-selinux --disable-acl --without-posix-acls --without-xattrs
	;;
binutils) # after glibc: the assembler, the linker and the rest, for building on AximOS
	fetch https://ftp.gnu.org/gnu/binutils binutils-2.47.tar.xz 154ab23b60070e8f27013c22977f1129425d67d1e8acd6e13010e617811e4cff
	unpack binutils-2.47.tar.xz binutils-2.47
	cd binutils-2.47
	# Not flex's libfl, which configure would find on the host, and its scanners don't need
	LEXLIB= devtools --disable-werror --disable-gprofng --disable-gdb --disable-gdbserver --disable-sim --disable-libdecnumber \
		--disable-readline --enable-plugins --enable-default-hash-style=gnu --with-system-zlib
	;;
make) # after glibc: GNU make
	fetch https://ftp.gnu.org/gnu/make make-4.4.1.tar.gz dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3
	unpack make-4.4.1.tar.gz make-4.4.1
	cd make-4.4.1
	devtools --without-guile
	;;
m4) # after glibc: the macro processor, which autoconf and flex need
	fetch https://ftp.gnu.org/gnu/m4 m4-1.4.21.tar.xz f25c6ab51548a73a75558742fb031e0625d6485fe5f9155949d6486a2408ab66
	unpack m4-1.4.21.tar.xz m4-1.4.21
	cd m4-1.4.21
	devtools
	;;
gzip) # after glibc
	fetch https://ftp.gnu.org/gnu/gzip gzip-1.15.tar.xz 9aa0cc780dec156b8282844833b342ab7cb08c25d2cd9a1869cdd0df31deff48
	unpack gzip-1.15.tar.xz gzip-1.15
	cd gzip-1.15
	devtools
	;;
patch) # after glibc
	fetch https://ftp.gnu.org/gnu/patch patch-2.8.1.tar.xz 3240c7959c5347c8ed347e3c4201c0337bf4240d5cf09d68889e58eeaba30e17
	unpack patch-2.8.1.tar.xz patch-2.8.1
	cd patch-2.8.1
	devtools --disable-xattr
	;;
xz) # after glibc: xz and liblzma, for .xz files, which most source comes as
	fetch https://github.com/tukaani-project/xz/releases/download/v5.8.4 xz-5.8.4.tar.xz \
		4ce24038fd4221e0d13bc1a2de7a4db56e90b92b3bf75321f6c14be73f65de4b
	unpack xz-5.8.4.tar.xz xz-5.8.4
	cd xz-5.8.4
	devtools --disable-doc
	;;
bzip2) # after glibc: bzip2 and libbz2, built by its own makefile
	fetch https://sourceware.org/pub/bzip2 bzip2-1.0.8.tar.gz ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
	unpack bzip2-1.0.8.tar.gz bzip2-1.0.8
	cd bzip2-1.0.8
	make -j"$jobs" -f Makefile-libbz2_so CC="$target_cc" CFLAGS="-O2 -fPIC -D_FILE_OFFSET_BITS=64" >/dev/null
	make -j"$jobs" bzip2 bzip2recover CC="$target_cc" CFLAGS="-O2 -D_FILE_OFFSET_BITS=64" >/dev/null
	install -D -m 755 bzip2 "$dev/system/bin/bzip2"
	install -m 755 bzip2recover "$dev/system/bin/"
	ln -sf bzip2 "$dev/system/bin/bunzip2"
	ln -sf bzip2 "$dev/system/bin/bzcat"
	install -D -m 755 libbz2.so.1.0.8 "$dev/system/lib/libbz2.so.1.0.8"
	ln -sf libbz2.so.1.0.8 "$dev/system/lib/libbz2.so.1.0"
	ln -sf libbz2.so.1.0.8 "$dev/system/lib/libbz2.so.1"
	ln -sf libbz2.so.1.0.8 "$dev/system/lib/libbz2.so"
	install -D -m 644 bzlib.h "$dev/system/include/bzlib.h"
	;;
gcc) # after binutils: GCC, for C and C++, with GMP, MPFR and MPC built with it. Built here, it uses the
	# system's headers and libraries, and on AximOS, /usr's, which are the same, in /system.
	fetch https://ftp.gnu.org/gnu/gcc/gcc-16.2.0 gcc-16.2.0.tar.xz e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e
	fetch https://ftp.gnu.org/gnu/gmp gmp-6.3.0.tar.xz a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898
	fetch https://ftp.gnu.org/gnu/mpfr mpfr-4.2.2.tar.xz b67ba0383ef7e8a8563734e2e889ef5ec3c3b898a01d00fa0a6869ad81c6ce01
	fetch https://ftp.gnu.org/gnu/mpc mpc-1.3.1.tar.gz ab642492f5cf882b74aa0cb730cd410a81edcdbec895183ce930e706c1c759b8
	if [ ! -d gcc-16.2.0 ]; then
		unpack gcc-16.2.0.tar.xz gcc-16.2.0
		for lib in gmp-6.3.0.tar.xz mpfr-4.2.2.tar.xz mpc-1.3.1.tar.gz; do
			tar xf "$root/ports/dist/$lib" -C gcc-16.2.0
			ln -sfn "${lib%%.tar*}" "gcc-16.2.0/${lib%%-*}"
		done
		sed -i '/m64=/s/lib64/lib/' gcc-16.2.0/gcc/config/i386/t-linux64 # its libraries in lib, as AximOS has them
	fi
	mkdir -p gcc-build
	cd gcc-build
	[ -e Makefile ] || CC="$target_cc" CXX="g++ --sysroot=$target" ../gcc-16.2.0/configure --build=x86_64-linux-gnu \
		--host=x86_64-linux-gnu --target=x86_64-linux-gnu --prefix=/usr --libdir=/usr/lib \
		--libexecdir=/usr/lib --with-build-sysroot="$target" --enable-languages=c,c++ --disable-multilib \
		--disable-bootstrap --disable-nls --enable-shared --enable-threads=posix --enable-default-pie \
		--enable-default-ssp --enable-__cxa_atexit --enable-linker-build-id --with-system-zlib --without-isl \
		--disable-libsanitizer --disable-libgomp --disable-libquadmath --disable-libvtv --disable-werror >/dev/null
	make -j"$jobs" >/dev/null
	make install-strip DESTDIR="$dev" >/dev/null
	ln -sf gcc "$dev/system/bin/cc"
	rm -f "$dev"/system/lib/*.la "$dev"/system/lib/gcc/*/*/*.la
	;;
perl) # after glibc: Perl, which the kernel's build, OpenSSL's and autoconf need. Its Configure looks for
	# libraries only in the system's folders, not the host's.
	fetch https://www.cpan.org/src/5.0 perl-5.44.0.tar.xz 505cf43912e9480495c344c70260452e32aa2a73c546a026b3f100053b23ce91
	unpack perl-5.44.0.tar.xz perl-5.44.0
	cd perl-5.44.0
	[ -e config.sh ] || sh Configure -des -Dprefix=/usr -Dvendorprefix=/usr -Dcc="$target_cc" -Doptimize=-O2 \
		-Dlibpth="$target/system/lib $dev/system/lib" -Dglibpth= -Dloclibpth= -Dlocincpth= \
		-Dldflags="-L$target/system/lib -L$dev/system/lib" -Dlddlflags="-shared -O2 -L$target/system/lib" \
		-Duseshrplib -Dusethreads -Dman1dir=none -Dman3dir=none >/dev/null
	make -j"$jobs" >/dev/null
	make install DESTDIR="$dev" >/dev/null
	;;
libffi) # after glibc: calling C from Python, for its ctypes
	fetch https://github.com/libffi/libffi/releases/download/v3.8.1 libffi-3.8.1.tar.gz \
		81e879c094cc5ec3186c48510c6385ac6b3e866192e8bc71a35245703757ee40
	unpack libffi-3.8.1.tar.gz libffi-3.8.1
	cd libffi-3.8.1
	devtools --with-gcc-arch=x86-64 --disable-docs # any x86-64, not this one's
	;;
python) # after libffi, xz, bzip2 and openssl: Python, which the build's checks and the tests are in. Its build
	# runs the host's Python of the same version, as for another machine, whose answers configure is given.
	fetch https://www.python.org/ftp/python/3.14.10 Python-3.14.10.tar.xz \
		de414a57119abef52784d8993e341d53aed635fc2b05c2607d9f98a2c5b72032
	unpack Python-3.14.10.tar.xz Python-3.14.10
	cd Python-3.14.10
	# OpenSSL 4 has no methods of one TLS version only, which Python's ssl looks for unless told
	no_methods="-DOPENSSL_NO_SSL3 -DOPENSSL_NO_SSL3_METHOD -DOPENSSL_NO_TLS1_METHOD -DOPENSSL_NO_TLS1_1_METHOD -DOPENSSL_NO_TLS1_2_METHOD"
	[ -e Makefile ] || ac_cv_file__dev_ptmx=yes ac_cv_file__dev_ptc=no ac_cv_buggy_getaddrinfo=no CC="$target_cc" CXX="g++ --sysroot=$target" \
		CFLAGS=-O2 CPPFLAGS="-I$dev/system/include $no_methods" LDFLAGS="-L$target/system/lib -L$dev/system/lib" \
		./configure --build=x86_64-pc-linux-gnu --host=x86_64-linux-gnu --prefix=/usr --enable-shared \
		--with-build-python=python3.14 --without-ensurepip --disable-test-modules >/dev/null
	make -j"$jobs" >/dev/null
	make install DESTDIR="$dev" >/dev/null
	ln -sf python3 "$dev/system/bin/python"
	;;
bc) # after glibc: the calculator the kernel's build works out its timer constants with
	fetch https://ftp.gnu.org/gnu/bc bc-1.08.2.tar.gz ae470fec429775653e042015edc928d07c8c3b2fc59765172a330d3d87785f86
	unpack bc-1.08.2.tar.gz bc-1.08.2
	cd bc-1.08.2
	MAKEFLAGS=MAKEINFO=true devtools # set for make, as its configure ignores MAKEINFO
	;;
flex) # after m4: the scanner generator, which the kernel's build needs
	fetch https://github.com/westes/flex/releases/download/v2.7.7 flex-2.7.7.tar.gz \
		4fff8a32ce095788537fdc4dbecd04ec069110c84429d182259f108db1003db2
	unpack flex-2.7.7.tar.gz flex-2.7.7
	cd flex-2.7.7
	M4=/usr/bin/m4 devtools
	ln -sf flex "$dev/system/bin/lex"
	;;
bison) # after m4: the parser generator, which the kernel's and glibc's builds need
	fetch https://ftp.gnu.org/gnu/bison bison-3.8.2.tar.xz 9bba0214ccf7f1079c5d59210045227bcf619519840ebfa80cd3849cff5a5bf2
	unpack bison-3.8.2.tar.xz bison-3.8.2
	cd bison-3.8.2
	M4=/usr/bin/m4 devtools
	;;
gperf) # after glibc: perfect hash tables, which eudev's build makes
	fetch https://ftp.gnu.org/gnu/gperf gperf-3.3.tar.gz fd87e0aba7e43ae054837afd6cd4db03a3f2693deb3619085e6ed9d8d9604ad8
	unpack gperf-3.3.tar.gz gperf-3.3
	cd gperf-3.3
	devtools
	;;
elfutils) # after xz and bzip2: libelf, which the kernel's objtool reads its objects with, and the eu- tools
	fetch https://sourceware.org/elfutils/ftp/0.197 elfutils-0.197.tar.bz2 \
		1fdf525f22dcf796d99a99ba3ea00a1f7b1ae5421bc62e8443a708920b99b904
	unpack elfutils-0.197.tar.bz2 elfutils-0.197
	cd elfutils-0.197
	devtools --disable-debuginfod --disable-libdebuginfod --without-valgrind
	;;
pkgconf) # after glibc: pkg-config, which finds libraries' flags for packages' builds
	fetch https://distfiles.ariadne.space/pkgconf pkgconf-3.0.11.tar.xz \
		d0cd841bc8e55b9da888106c00dc256d05b7d1d8e5a46f4be4eb4c5125bd2dc4
	unpack pkgconf-3.0.11.tar.xz pkgconf-3.0.11
	cd pkgconf-3.0.11
	devtools --with-system-libdir=/usr/lib --with-system-includedir=/usr/include \
		--with-pkg-config-dir=/usr/lib/pkgconfig:/usr/share/pkgconfig
	ln -sf pkgconf "$dev/system/bin/pkg-config"
	;;
autoconf) # after perl and m4: configure scripts, which dash's build makes
	fetch https://ftp.gnu.org/gnu/autoconf autoconf-2.73.tar.xz 9fd672b1c8425fac2fa67fa0477b990987268b90ff36d5f016dae57be0d6b52e
	unpack autoconf-2.73.tar.xz autoconf-2.73
	cd autoconf-2.73
	M4=/usr/bin/m4 PERL=/usr/bin/perl devtools
	;;
automake) # after autoconf: makefiles from Makefile.am, which dash's build makes
	fetch https://ftp.gnu.org/gnu/automake automake-1.20.tar.xz f6f34dad4cbe395715762b0710f7c511e39bd5cf777e6b95818ccbecf7714248
	unpack automake-1.20.tar.xz automake-1.20
	cd automake-1.20
	PERL=/usr/bin/perl devtools
	;;
rsync) # after openssl and zstd: copying trees, which the build makes the system's image with
	fetch https://github.com/RsyncProject/rsync/releases/download/v3.5.2 rsync-3.5.2.tar.gz \
		a10d591825251e070f74c98df3daad195def4ed81b4a2de16169026aea33f6c8
	unpack rsync-3.5.2.tar.gz rsync-3.5.2
	cd rsync-3.5.2
	devtools --disable-xxhash --disable-lz4 --disable-idn --disable-md2man --disable-acl-support --disable-xattr-support \
		--with-included-popt
	;;
cpio) # after glibc: cpio archives, which the initramfs is
	fetch https://ftp.gnu.org/gnu/cpio cpio-2.15.tar.bz2 937610b97c329a1ec9268553fb780037bcfff0dcffe9725ebc4fd9c1aa9075db
	unpack cpio-2.15.tar.bz2 cpio-2.15
	cd cpio-2.15
	CFLAGS='-O2 -std=gnu17' devtools --with-rmt=/dev/null # its declarations, from before C23's
	;;
xorriso) # after glibc: makes the ISO, with libburn's, libisofs' and libisoburn's code in it
	fetch https://ftp.gnu.org/gnu/xorriso xorriso-1.5.8.pl02.tar.gz \
		b1455ecafbf0692ddafe1d71002a96f2ce2d77f4deae602678261ce033f97bc8
	unpack xorriso-1.5.8.pl02.tar.gz xorriso-1.5.8
	cd xorriso-1.5.8
	devtools --disable-libreadline --disable-libedit --disable-libacl --disable-xattr --disable-launch-frontend
	;;
mtools) # after glibc: writes FAT images without mounting them, for Limine's CD image and the disk's ESP
	fetch https://ftp.gnu.org/gnu/mtools mtools-4.0.50.tar.bz2 f3a58e6cfc2634b03c240442548e3b00103692357afc28e1e7104744dd6478b5
	unpack mtools-4.0.50.tar.bz2 mtools-4.0.50
	cd mtools-4.0.50
	devtools --without-x
	;;
nasm) # after glibc: the assembler Limine's x86 code is in
	fetch https://www.nasm.us/pub/nasm/releasebuilds/3.02 nasm-3.02.tar.xz \
		87336eba53b4acfe917424ab5d500d2b0054d9f5148d35c2273ccf2cfb712f0d
	unpack nasm-3.02.tar.xz nasm-3.02
	cd nasm-3.02
	devtools
	;;
limine) # after nasm and mtools: the bootloader, its UEFI program and CD image, built by this machine's
	# GCC and binutils, for the ISO and installed systems
	fetch https://github.com/Limine-Bootloader/Limine/releases/download/v12.11.1 limine-12.11.1.tar.xz \
		31518344aa35d0c86910e8a03016a7d742ab7d0e42098c7a7fa322945034d28a
	unpack limine-12.11.1.tar.xz limine-12.11.1
	cd limine-12.11.1
	[ -e GNUmakefile ] || TOOLCHAIN_FOR_TARGET= CC="$target_cc" LDFLAGS="-L$target/system/lib" ./configure \
		--prefix=/usr --enable-uefi-x86-64 --enable-uefi-cd >/dev/null
	make -j"$jobs" >/dev/null
	make install DESTDIR="$dev" >/dev/null
	;;
ninja) # after python: the build tool meson's builds run
	fetch https://github.com/ninja-build/ninja/archive/v1.13.2 ninja-1.13.2.tar.gz \
		974d6b2f4eeefa25625d34da3cb36bdcebe7fbce40f4c16ac0835fd1c0cbae17
	unpack ninja-1.13.2.tar.gz ninja-1.13.2
	cd ninja-1.13.2
	[ -e ninja ] || CXX="g++ --sysroot=$target" CXXFLAGS=-O2 LDFLAGS="-L$target/system/lib -L$dev/system/lib" \
		python3 configure.py --bootstrap >/dev/null
	install -D -m 755 ninja "$dev/system/bin/ninja"
	;;
meson) # after python and ninja: the build system many libraries use, as one file Python runs
	fetch https://github.com/mesonbuild/meson/releases/download/1.12.1 meson-1.12.1.tar.gz \
		ab0a6ca09f8ef70c564c8241fb5a23957886a0b53fb58412b5e07eaf07dba743
	unpack meson-1.12.1.tar.gz meson-1.12.1
	cd meson-1.12.1
	python3 packaging/create_zipapp.py --outfile "$dev/system/bin/meson" --interpreter /usr/bin/python3 . >/dev/null
	;;
kmod) # after zstd: only depmod, which lists the kernel's modules for modprobe, as modules_install runs it.
	# AximOS has its own modprobe, insmod, rmmod and lsmod.
	fetch https://mirrors.kernel.org/pub/linux/utils/kernel/kmod kmod-34.4.tar.xz \
		4c7942dda4793d08f975270322db582c9701438afd4d76ed9e6e7cf4094cb967
	unpack kmod-34.4.tar.xz kmod-34.4
	cd kmod-34.4
	if [ ! -e Makefile ]; then
		CC="$target_cc" CFLAGS=-O2 LDFLAGS="-L$target/system/lib" ./configure --host=x86_64-linux-gnu --prefix=/usr \
			--libdir=/usr/lib --sysconfdir=/etc --with-zstd --without-xz --without-zlib --without-openssl \
			--disable-manpages --disable-static >/dev/null
	fi
	make -j"$jobs" >/dev/null
	install -D -m 755 tools/kmod "$dev/system/bin/kmod"
	ln -sf kmod "$dev/system/bin/depmod"
	;;
tzdata) # the time zones, compiled from IANA's database by its own zic, built for this machine
	fetch https://data.iana.org/time-zones/releases tzcode2026f.tar.gz \
		5a18da01393679ef2559214dc687563f7f3d05691f6f58b7fbc0166f3e796cd6
	fetch https://data.iana.org/time-zones/releases tzdata2026f.tar.gz \
		01b4a91cec5167ed48c11e8ab78f2048cead78ce94b0cafcad0ae8c5a3044d3d
	rm -rf tzdb && mkdir tzdb && cd tzdb
	tar xf "$root/ports/dist/tzcode2026f.tar.gz" && tar xf "$root/ports/dist/tzdata2026f.tar.gz"
	make -s cc=gcc CC=gcc zic tzdata.zi >/dev/null
	zones=$target/system/share/zoneinfo
	rm -rf "$zones"
	./zic -d "$zones" africa antarctica asia australasia europe northamerica southamerica etcetera backward factory
	cp iso3166.tab zone.tab zone1970.tab zonenow.tab leapseconds leap-seconds.list tzdata.zi SECURITY "$zones/"
	;;
dosfstools) # after glibc: mkfs.fat and fsck.fat
	fetch https://github.com/dosfstools/dosfstools/releases/download/v4.2 dosfstools-4.2.tar.gz \
		64926eebf90092dca21b14259a5301b7b98e7b1943e8a201c7d726084809b527
	unpack dosfstools-4.2.tar.gz dosfstools-4.2
	cd dosfstools-4.2
	autotools --enable-compat-symlinks
	;;
pcre2) # after glibc: the regular expressions glib uses
	fetch https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.49 pcre2-10.49.tar.bz2 \
		53c156e1ba416a20da8e65395daa132da0d80e76910424caca3fcdae7831d384
	unpack pcre2-10.49.tar.bz2 pcre2-10.49
	cd pcre2-10.49
	devtools --enable-jit
	;;
glib) # after pcre2, libffi and zlib-ng: GLib, which QEMU is built on
	fetch https://download.gnome.org/sources/glib/2.90 glib-2.90.1.tar.xz \
		93c941aa17d5eb1d53fe838365f29a8b4e539c222a256d974ec8f30fc413e396
	unpack glib-2.90.1.tar.xz glib-2.90.1
	cd glib-2.90.1
	mesondev -Dselinux=disabled -Dlibmount=disabled -Dlibelf=disabled -Dintrospection=disabled -Dnls=disabled \
		-Dsysprof=disabled -Dman-pages=disabled -Ddocumentation=false -Dtests=false
	;;
pixman) # after glibc: the pixel drawing QEMU's displays use
	fetch https://www.x.org/releases/individual/lib pixman-0.46.4.tar.xz \
		a098c33924754ad43f981b740f6d576c70f9ed1006e12221b1845431ebce1239
	unpack pixman-0.46.4.tar.xz pixman-0.46.4
	cd pixman-0.46.4
	mesondev -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled
	;;
libslirp) # after glib: the network QEMU gives its machines, through the host's own connections
	fetch https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v4.9.7 libslirp-v4.9.7.tar.gz \
		d2567fa06a6357c7755d61c67293b2c0bf069b3f2173bde6b76cf768529a1938
	unpack libslirp-v4.9.7.tar.gz libslirp-v4.9.7
	cd libslirp-v4.9.7
	mesondev
	;;
qemu) # after glib, pixman, libslirp, meson and ninja: QEMU, for x86-64 machines with KVM, without a window,
	# as make test runs them. Its own build of edk2's UEFI firmware goes where Arch keeps OVMF's, which is
	# its variables, then its code.
	fetch https://download.qemu.org qemu-11.1.3.tar.xz 5a8289c7940dc77d3873c81a6c29ba3afe1446fc9d36e77fa1658cdbecb84b22
	unpack qemu-11.1.3.tar.xz qemu-11.1.3
	cd qemu-11.1.3
	[ -e build/build.ninja ] || env -u PKG_CONFIG_SYSROOT_DIR PKG_CONFIG_RELOCATE_PATHS=1 \
		PKG_CONFIG_LIBDIR="$target/system/lib/pkgconfig:$dev/system/lib/pkgconfig" \
		./configure --prefix=/usr --libexecdir=/usr/lib/qemu --cc="$target_cc" --cxx="g++ --sysroot=$target" \
		--host-cc=gcc --ninja="$ninja" --extra-ldflags="-L$target/system/lib -L$dev/system/lib" --target-list=x86_64-softmmu \
		--enable-kvm --enable-slirp --disable-sdl --disable-gtk --disable-vnc --disable-opengl --disable-docs \
		--disable-werror --disable-download --disable-fdt >/dev/null
	"$ninja" -C build >/dev/null
	build/pyvenv/bin/meson install -C build --strip --destdir "$dev" >/dev/null # its own meson, which set it up
	cd "$dev/system/share/qemu" # without the firmware of other machines than x86's
	rm -rf edk2-arm-* edk2-aarch64-* edk2-riscv-* edk2-loongarch64-* openbios-* skiboot.lid slof.bin vof*.bin \
		hppa-firmware* u-boot* opensbi-* s390-* qemu_vga.ndrv npcm* pnv-* canyonlands.dtb petalogix-* bamboo.dtb dtb \
		ast27x0_bootrom.bin palcode-clipper QEMU,cgthree.bin QEMU,tcx.bin qemu-nsis.bmp
	edk2=$dev/system/share/edk2/x64
	mkdir -p "$edk2"
	ln -sf ../../qemu/edk2-x86_64-code.fd "$edk2/OVMF_CODE.4m.fd"
	ln -sf ../../qemu/edk2-i386-vars.fd "$edk2/OVMF_VARS.4m.fd"
	cat edk2-i386-vars.fd edk2-x86_64-code.fd > "$edk2/OVMF.4m.fd"
	;;
*)
	echo "build.sh: no part called $1" >&2
	exit 1
	;;
esac
