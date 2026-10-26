#!/bin/bash
# Refresh prebuilt/ from what is built in this tree: the kernel, the loader, our user-space programs
# and the pieces of the phone's UEFI. With them somebody without the cross toolchain can still make the
# images and run the system (QEMU or the phone); see README.md.
#
# DEVICE=<name> (default: poco-x5-pro) says which devices/<name>/prebuilt the phone's UEFI pieces go
# to; devices/<name>/image.conf holds its package name and DTB. The kernel and the loader are the
# same for every device: they pick the phone at run time from its framebuffer and device tree.
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
cp $K prebuilt/mach.development.bcm2837
[ -f xnuloader/loader.efi ] && cp xnuloader/loader.efi prebuilt/loader.efi || echo "update-prebuilt.sh: no xnuloader/loader.efi here (lld-link); prebuilt/loader.efi kept"
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
