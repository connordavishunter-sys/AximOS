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
git) # after curl: version control, with HTTPS through curl, and AximOS's more and text as its pager and editor
	fetch https://mirrors.edge.kernel.org/pub/software/scm/git git-2.56.1.tar.xz \
		f0b0bf89264b9bd2e1c640363e7529643989da76281600473c50745cd34a607f
	unpack git-2.56.1.tar.xz git-2.56.1
	cd git-2.56.1
	make -j"$jobs" prefix=/usr gitexecdir=/usr/lib/git-core CC="$target_cc" CFLAGS="-O2 -w" \
		LDFLAGS="-L$target/system/lib" CURL_LDFLAGS="-L$target/system/lib -lcurl" \
		NO_PERL=1 NO_PYTHON=1 NO_TCLTK=1 NO_GETTEXT=1 NO_EXPAT=1 NO_OPENSSL=1 INSTALL_SYMLINKS=1 \
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
	fetch https://ftp.gnu.org/gnu/gawk gawk-5.4.1.tar.xz 07f6f7342b7febe4313fc2c2542ad93d64fe20ad8717200109f105a826f5fd37
	unpack gawk-5.4.1.tar.xz gawk-5.4.1
	cd gawk-5.4.1
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
*)
	echo "build.sh: no part called $1" >&2
	exit 1
	;;
esac
