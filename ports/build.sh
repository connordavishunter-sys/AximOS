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

# Compiles sources, for a shared library, into build/ports/codecs/name.a:
# archive name flags... -- sources...
archive() {
	name=$1; shift
	flags=
	while [ "$1" != -- ]; do flags="$flags $1"; shift; done
	shift
	mkdir -p "$root/build/ports/codecs/$name"
	objects=
	for source; do
		object=$root/build/ports/codecs/$name/$(echo "$source" | tr / _).o
		"$cc" -c -fPIC -O2 -std=gnu17 -w $flags "$source" -o "$object" || exit 1
		objects="$objects $object"
	done
	rm -f "$root/build/ports/codecs/$name.a"
	ar rcs "$root/build/ports/codecs/$name.a" $objects
}

# Sources listed in a makefile fragment under a variable's name.
listed() {
	awk -v name="$2" '$1 == name { on = 1 } on { for (i = 1; i <= NF; i++) if ($i ~ /\.c$/) print $i; on = $NF == "\\" }' "$1"
}

mkdir -p "$root/build/ports" "$root/ports/dist"
cd "$root/build/ports"

case $1 in
stb) # only stb_truetype, which libgui includes
	fetch https://raw.githubusercontent.com/nothings/stb/9600522de53973336fa033a289858a4354cbb04c stb_truetype.h \
		428cfe93520f2aed5e413a220b0db5bba5b17e2ebe5a64e4a8efa9eeeb67ec5f
	;;
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
ogg)
	fetch https://downloads.xiph.org/releases/ogg libogg-1.3.6.tar.xz \
		5c8253428e181840cd20d41f3ca16557a9cc04bad4a3d04cce84808677fa1061
	[ -d libogg-1.3.6 ] || tar xf "$root/ports/dist/libogg-1.3.6.tar.xz"
	cd libogg-1.3.6
	sed 's/@INCLUDE_[A-Z_]*@/1/; s/@SIZE16@/int16_t/; s/@USIZE16@/uint16_t/; s/@SIZE32@/int32_t/;
		s/@USIZE32@/uint32_t/; s/@SIZE64@/int64_t/; s/@USIZE64@/uint64_t/' \
		include/ogg/config_types.h.in > include/ogg/config_types.h
	archive ogg -Iinclude -- src/bitwise.c src/framing.c
	;;
vorbis)
	fetch https://downloads.xiph.org/releases/vorbis libvorbis-1.3.7.tar.xz \
		b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b
	[ -d libvorbis-1.3.7 ] || tar xf "$root/ports/dist/libvorbis-1.3.7.tar.xz"
	cd libvorbis-1.3.7/lib
	archive vorbis -I../include -I. -I../../libogg-1.3.6/include -- analysis.c bitrate.c block.c codebook.c \
		envelope.c floor0.c floor1.c info.c lookup.c lpc.c lsp.c mapping0.c mdct.c psy.c registry.c res0.c \
		sharedbook.c smallft.c synthesis.c vorbisenc.c vorbisfile.c window.c
	;;
opus)
	fetch https://downloads.xiph.org/releases/opus opus-1.6.1.tar.gz \
		6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
	[ -d opus-1.6.1 ] || tar xf "$root/ports/dist/opus-1.6.1.tar.gz"
	cd opus-1.6.1
	archive opus -DOPUS_BUILD -DVAR_ARRAYS -DHAVE_LRINTF -DHAVE_LRINT -Iinclude -Icelt -Isilk -Isilk/float -- \
		$(listed celt_sources.mk CELT_SOURCES) $(listed silk_sources.mk SILK_SOURCES) \
		$(listed silk_sources.mk SILK_SOURCES_FLOAT) $(listed opus_sources.mk OPUS_SOURCES) \
		$(listed opus_sources.mk OPUS_SOURCES_FLOAT)
	;;
flac)
	fetch https://downloads.xiph.org/releases/flac flac-1.5.0.tar.xz \
		f2c1c76592a82ffff8413ba3c4a1299b6c7ab06c734dee03fd88630485c2b920
	[ -d flac-1.5.0 ] || tar xf "$root/ports/dist/flac-1.5.0.tar.xz"
	cd flac-1.5.0/src/libFLAC
	archive flac -DFLAC__NO_ASM -DFLAC__HAS_OGG=0 -DHAVE_LROUND -DHAVE_STDINT_H -DHAVE_INTTYPES_H -DENABLE_64_BIT_WORDS=1 \
		-DWORDS_BIGENDIAN=0 -DCPU_IS_LITTLE_ENDIAN=1 -DFLAC__NO_DLL '-DPACKAGE_VERSION="1.5.0"' \
		-Iinclude -I../../include -- bitmath.c bitreader.c bitwriter.c cpu.c crc.c fixed.c float.c format.c \
		lpc.c md5.c memory.c metadata_iterators.c metadata_object.c stream_decoder.c stream_encoder.c \
		stream_encoder_framing.c window.c
	;;
mp3)
	fetch https://github.com/lieff/minimp3/archive ea99364f61c14656440e8d77e9c233ccf3124633.tar.gz \
		5628166eb82a9bb581317918a334c317a2c0a30278bb14a20381307976768f34
	fetch https://github.com/toots/shine/releases/download/3.1.1 shine-3.1.1.tar.gz \
		58e61e70128cf73f88635db495bfc17f0dde3ce9c9ac070d505a0cd75b93d384
	[ -d minimp3-ea99364f61c14656440e8d77e9c233ccf3124633 ] \
		|| tar xf "$root/ports/dist/ea99364f61c14656440e8d77e9c233ccf3124633.tar.gz"
	[ -d shine-3.1.1 ] || tar xf "$root/ports/dist/shine-3.1.1.tar.gz"
	cd shine-3.1.1/src/lib
	archive shine -I. -- bitstream.c huffman.c l3bitstream.c l3loop.c l3mdct.c l3subband.c layer3.c \
		reservoir.c tables.c
	;;
aac)
	fetch https://github.com/knik0/faad2/archive/refs/tags 2.11.2.tar.gz \
		3fcbd305e4abd34768c62050e18ca0986f7d9c5eca343fb98275418013065c0e
	fetch https://github.com/knik0/faac/archive/refs/tags faac-2.2.tar.gz \
		a93963573907c83e26e8cfabbf80d3a9c360f06ea4ecf1ea6cb74a202494d8d9
	[ -d faad2-2.11.2 ] || tar xf "$root/ports/dist/2.11.2.tar.gz"
	[ -d faac-faac-2.2 ] || tar xf "$root/ports/dist/faac-2.2.tar.gz"
	(cd faad2-2.11.2/libfaad && archive faad -DHAVE_INTTYPES_H=1 -DHAVE_STDINT_H=1 -DHAVE_STRING_H=1 \
		-DHAVE_MEMCPY=1 -DHAVE_SYS_TYPES_H=1 -DHAVE_LRINTF=1 -DSTDC_HEADERS=1 '-DPACKAGE_VERSION="2.11.2"' \
		-I. -I../include -- *.c)
	cd faac-faac-2.2/libfaac
	archive faac -DMAX_CHANNELS=8 -DFAAC_SBR_DECIMATION=1 -DFAAC_STATS=0 -DWORDS_BIGENDIAN=0 \
		'-DPACKAGE_VERSION="2.2"' -I. -I../include -- bitstream.c blockswitch.c channels.c cpu_compute.c faac.c \
		filtbank.c fft.c frame.c huff2.c huffdata.c quantize.c ratecontrol.c sbr.c sbr_bitstream.c \
		sbr_tables.c sbr_analysis.c resample.c stereo.c tns.c util.c
	;;
esac
