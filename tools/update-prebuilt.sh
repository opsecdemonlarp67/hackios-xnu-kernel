#!/bin/bash
# Refresh prebuilt/ from what is built in this tree: the kernel, the loader, our user-space programs
# and the pieces of the phone's UEFI. With them somebody without the cross toolchain can still make the
# images and run the system (QEMU or the phone); see README.md.
#
# DEVICE=<name> (default: poco-x5-pro) says which devices/<name>/prebuilt the phone's UEFI pieces go
# to; devices/<name>/image.conf holds its package name and DTB. The kernel is the same for every
# device; the loader is not: its payload base (and, for a port, its platform default) is compiled
# in, so it is rebuilt here from image.conf before it is copied.
set -e
cd "$(dirname "$(readlink -f "$0")")/.."
DEVICE=${DEVICE:-poco-x5-pro}
DEV=devices/$DEVICE
CONF=$DEV/image.conf
[ -f "$CONF" ] || { echo "update-prebuilt.sh: no $CONF" >&2; exit 1; }
conf() { sed -n "s/^$1=//p" "$CONF" | head -1; }
UEFI_PKG=$(conf uefi_pkg); DTB=$(conf dtb)
# KERNEL=... to take the kernel from a tree built elsewhere (xnu/apply.sh makes one anywhere)
K=${KERNEL:-xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837}
MU=Mu-Silicium
mkdir -p prebuilt/userland "$DEV/prebuilt"
# Rebuild the loader for this device: PAYLOAD_BASE and the platform default are compiled in.
LOADER_OK=0
PB=$(conf payload_base)
PLATFORM_NAME=$(conf platform)
if [ -f xnuloader/identity.h ]; then
	if ( cd xnuloader && PAYLOAD_BASE=$PB PLATFORM=$PLATFORM_NAME ./build.sh ) >/dev/null; then
		LOADER_OK=1
	else
		echo "update-prebuilt.sh: could not rebuild the loader; keeping the prebuilt one" >&2
	fi
else
	echo "update-prebuilt.sh: xnuloader/identity.h missing; keeping the prebuilt loader" >&2
fi
if [ "$DEVICE" = poco-x5-pro ]; then
	cp $K prebuilt/mach.development.bcm2837
	if [ "$LOADER_OK" = 1 ]; then cp xnuloader/loader.efi prebuilt/loader.efi; fi
else
	# the loader's payload base is compiled in, so a port keeps its own; the kernel is the same source
	cp $K "$DEV/prebuilt/mach.development.bcm2837"
	if [ "$LOADER_OK" = 1 ]; then cp xnuloader/loader.efi "$DEV/prebuilt/loader.efi"; fi
fi
cp userland/hello prebuilt/userland/
for f in init child stage0 hello_dyn svc_test surf_test rwtouch mg_test cpu_test sound_test libhakios.dylib libhakios.sb.dylib \
	libhakios.media.dylib libhakios.app.dylib libhakios.tweaks.dylib HakiosAudio net_test; do
	cp userland/obj/$f prebuilt/userland/
done
if [ -f $MU/Build/$UEFI_PKG/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd ]; then
	cp $MU/Build/$UEFI_PKG/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd $MU/BootShim/BootShim.bin $MU/Resources/DTBs/$DTB \
		$MU/Resources/ramdisk $MU/Resources/Scripts/mkbootimg.py "$DEV/prebuilt/"
	[ "$DTB" = device-tree.dtb ] || cp "$DEV/prebuilt/$DTB" "$DEV/prebuilt/device-tree.dtb"
fi
{
	echo "Built $(date '+%Y-%m-%d %H:%M') from this tree."
	echo
	sha256sum prebuilt/mach.development.bcm2837 prebuilt/loader.efi prebuilt/userland/* "$DEV"/prebuilt/* 2>/dev/null
} > prebuilt/SHA256SUMS
ls -la prebuilt prebuilt/userland "$DEV/prebuilt"
