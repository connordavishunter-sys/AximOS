#!/bin/sh
# Builds one ported program against AximOS's libc: build.sh dash. Source comes
# from ports/dist, downloaded the first time, and unpacks in build/ports.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
cc=$root/ports/aximos-cc

fetch() { # url file sha256
	[ -e "$root/ports/dist/$2" ] || curl -fsSL -o "$root/ports/dist/$2" "$1/$2"
	echo "$3  $root/ports/dist/$2" | sha256sum -c --status
}

mkdir -p "$root/build/ports" "$root/ports/dist"
cd "$root/build/ports"

case $1 in
mbedtls)
	fetch https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7 mbedtls-3.6.7.tar.bz2 \
		a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
	[ -d mbedtls-3.6.7 ] || tar xf "$root/ports/dist/mbedtls-3.6.7.tar.bz2"
	make -C mbedtls-3.6.7/library clean # a changed configuration affects everything
	make -j"$(nproc)" -C mbedtls-3.6.7/library CC="$cc" \
		CFLAGS="-Os -std=gnu11 -DMBEDTLS_CONFIG_FILE='\"$root/ports/mbedtls_config.h\"'" \
		WARNING_CFLAGS=-w libmbedcrypto.a libmbedx509.a libmbedtls.a
	;;
dash)
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
		./autogen.sh
		CC=$cc CFLAGS="-Os -std=gnu11" ./configure --host=x86_64-aximos --build=x86_64-linux --without-libedit
	fi
	rm -f src/dash
	make -j"$(nproc)" CC_FOR_BUILD=gcc
	;;
esac
